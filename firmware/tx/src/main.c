#include <zephyr/kernel.h>
#include <stdio.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/watchdog.h>
#include <math.h>
#include <esb.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/poweroff.h>

/* ── LED ── */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* ── Detecção de dock / fonte de despertar do System OFF ── */
static const struct gpio_dt_spec dock_detect = GPIO_DT_SPEC_GET(DT_ALIAS(dock_detect), gpios);

/* ── Listener de comando do dock: entra em bootloader sob demanda ── */
#define DOCK_DFU_CMD     "DVSDFU\n"
#define DOCK_DFU_CMD_LEN (sizeof(DOCK_DFU_CMD) - 1)

/* CORRECAO 29/jul: comando de pareamento/troca de canal, recebido do RX
   (ESP32-S3) via UART quando este TX esta docado. Formato exato:
   "PAIR:<canal decimal>:<endereco hex, 8 digitos>\n"
   Ex: "PAIR:91:C35A7198\n". Resposta: "PAIRACK:OK\r\n" ou
   "PAIRACK:FAIL\r\n", enviada ANTES do reset (acordado com o lado RX
   em dock_pairing.c - RX espera o ACK por ate 2000ms antes de desistir
   e NAO reiniciar o proprio radio, evitando dessincronia). */
#define ENTER_DFU_CMD        "ENTERDFU\n"
#define ENTER_DFU_CMD_LEN    (sizeof(ENTER_DFU_CMD) - 1)
static int enter_dfu_match_pos = 0;
static volatile bool enter_dfu_requested = false;

#define CLEAR_OFFSET_CMD     "CLEAROFFSET\n"
#define CLEAR_OFFSET_CMD_LEN (sizeof(CLEAR_OFFSET_CMD) - 1)
static int clear_offset_match_pos = 0;
static volatile bool clear_offset_requested = false;

#define PAIR_CMD_PREFIX     "PAIR:"
#define PAIR_CMD_PREFIX_LEN (sizeof(PAIR_CMD_PREFIX) - 1)
#define PAIR_LINE_MAX        32

static size_t pair_prefix_match_pos = 0;
static bool   pair_capturing        = false;
static char   pair_line_buf[PAIR_LINE_MAX];
static size_t pair_line_len         = 0;
static volatile bool pair_line_ready = false; /* ISR seta, worker thread consome e limpa */

static const struct device *dock_uart;
static size_t dock_dfu_match_pos = 0;

static void reset_to_bootloader(void) {
    NRF_POWER->GPREGRET = 0x57;
    NVIC_SystemReset();
}

static void dock_uart_isr(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);
    uint8_t c;

    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (uart_irq_rx_ready(dev)) {
            while (uart_fifo_read(dev, &c, 1) == 1) {
                /* Maquina de estados 1: DVSDFU (ja existia, intocada) */
                if (c == DOCK_DFU_CMD[dock_dfu_match_pos]) {
                    dock_dfu_match_pos++;
                    if (dock_dfu_match_pos == DOCK_DFU_CMD_LEN) {
                        if (gpio_pin_get_dt(&dock_detect) > 0) {
                            reset_to_bootloader();
                        }
                        dock_dfu_match_pos = 0;
                    }
                } else {
                    dock_dfu_match_pos = (c == DOCK_DFU_CMD[0]) ? 1 : 0;
                }

                /* Maquina de estados 2: PAIR:<canal>:<addr hex>\n -
                   independente da de cima, roda em paralelo no mesmo
                   fluxo de bytes. So acumula/parseia aqui (contexto de
                   ISR) - NVS e resposta ficam pro worker thread, fora
                   de ISR (nvs_write pode levar ate ~85ms, e
                   dock_uart_send_line e bloqueante). */
                if (!pair_capturing) {
                    if (c == PAIR_CMD_PREFIX[pair_prefix_match_pos]) {
                        pair_prefix_match_pos++;
                        if (pair_prefix_match_pos == PAIR_CMD_PREFIX_LEN) {
                            pair_capturing = true;
                            pair_line_len  = 0;
                        }
                    } else {
                        pair_prefix_match_pos = (c == PAIR_CMD_PREFIX[0]) ? 1 : 0;
                    }
                } else {
                    if (c == '\n' || pair_line_len >= PAIR_LINE_MAX - 1) {
                        pair_line_buf[pair_line_len] = '\0';
                        pair_capturing = false;
                        pair_prefix_match_pos = 0;
                        if (!pair_line_ready) { /* nao sobrescreve um comando ainda nao processado */
                            pair_line_ready = true;
                        }
                    } else {
                        pair_line_buf[pair_line_len++] = (char)c;
                    }
                }
                /* Maquina de estados 3: CLEAROFFSET\n - comando fixo,
                   sem parametros, mesmo padrao da DVSDFU (maquina 1).
                   So' seta uma flag aqui - o apagamento real da NVS
                   acontece no worker thread (fora de ISR), mesmo
                   motivo do PAIR (nvs_delete pode levar tempo). */
                if (c == CLEAR_OFFSET_CMD[clear_offset_match_pos]) {
                    clear_offset_match_pos++;
                    if (clear_offset_match_pos == CLEAR_OFFSET_CMD_LEN) {
                        if (!clear_offset_requested) {
                            clear_offset_requested = true;
                        }
                        clear_offset_match_pos = 0;
                    }
                } else {
                    clear_offset_match_pos = (c == CLEAR_OFFSET_CMD[0]) ? 1 : 0;
                }
                /* Maquina de estados 4: ENTERDFU\n - comando fixo, sem
                   parametros, mesmo padrao das outras (1, 3). So' seta
                   uma flag aqui - o reset real acontece no worker
                   thread, para dar tempo do ACK ser enviado antes do
                   TX sumir da UART. */
                if (c == ENTER_DFU_CMD[enter_dfu_match_pos]) {
                    enter_dfu_match_pos++;
                    if (enter_dfu_match_pos == ENTER_DFU_CMD_LEN) {
                        if (!enter_dfu_requested) {
                            enter_dfu_requested = true;
                        }
                        enter_dfu_match_pos = 0;
                    }
                } else {
                    enter_dfu_match_pos = (c == ENTER_DFU_CMD[0]) ? 1 : 0;
                }
            }
        }
    }
}

static int dock_uart_setup(void) {
    dock_uart = DEVICE_DT_GET(DT_NODELABEL(uart0));
    if (!device_is_ready(dock_uart)) {
        dock_uart = NULL;
        return -ENODEV;
    }
    uart_irq_callback_set(dock_uart, dock_uart_isr);
    uart_irq_rx_enable(dock_uart);
    return 0;
}

/* CORRECAO 24/jul: envio de linha de diagnostico pelo UART do dock
   (poll_out, bloqueante - so e chamado com o TX docked, quando o audio
   ja esta suprimido de proposito, entao o custo nao afeta nada). */
static void dock_uart_send_line(const char *s) {
    if (dock_uart == NULL) return;
    while (*s) {
        uart_poll_out(dock_uart, (unsigned char)*s++);
    }
}

/* CORRECAO 29/jul (tarde): 1h -> 30min - sono por inatividade mais
   agressivo, economiza mais bateria quando o TX fica parado (ex:
   esquecido fora do prato) sem precisar esperar tanto tempo. */
#define IDLE_POWEROFF_MS (30UL * 60UL * 1000UL)

/* CORRECAO 23/jul: atraso antes de retomar transmissao apos V+ sumir -
   evita reagir a bounce mecanico/eletrico no contato do dock. Enquanto
   contando, o TX continua suprimido (mesmo comportamento de "ainda
   docked"). */
/* CORRECAO 29/jul (tarde): 2000 -> 5000 - debounce de saida do dock
   aumentado pra 5s. Alem de reforcar a protecao contra bounce mecanico,
   essa janela agora tambem serve de "tempo de seguranca" antes do TX
   aplicar um pareamento pendente (ver pending_reset_after_pairing mais
   abaixo) - da tempo do usuario terminar de manusear o TX/dock antes do
   reset acontecer. */
#define UNDOCK_DEBOUNCE_MS 5000

/* CORRECAO 24/jul: debounce SIMETRICO tambem na ENTRADA do estado
   docked. A logica anterior aceitava UMA unica leitura alta do pino
   dock_detect como dock valido - um unico glitch (EMI/ESD/acoplamento
   num ambiente de som alto) bastava para: (1) enviar velocity=0 ao RX
   (musica para) e (2) suprimir ESB por UNDOCK_DEBOUNCE_MS = 2000ms.
   Ou seja: 1 amostra espuria = 2s de mudez, por construcao.
   NOTA DE HONESTIDADE: isso NAO e a causa das pausas de ~2s do set
   antigo (aquele firmware nem tinha dock_detect - cronologia nao
   fecha, apontado pelo Sonnet na revisao 24/jul). E vacina contra o
   futuro, nao cura do passado. A 416Hz, 12 amostras = ~29ms de atraso
   para reconhecer o dock - imperceptivel no encaixe real. */
#define DOCK_CONFIRM_SAMPLES 12
static uint32_t dock_confirm_count = 0;

/* ── Sensor / LSM6DS3 ── */
static const struct device *lsm;
#define RAD_TO_DEG 57.29577951308232f

static void pisca(int n, int ms) {
    for (int i = 0; i < n; i++) {
        gpio_pin_set_dt(&led, 1); k_msleep(ms);
        gpio_pin_set_dt(&led, 0); k_msleep(ms);
    }
}

static float lsm_read_gyro_z_dps(void) {
    struct sensor_value val;
    int ret = sensor_sample_fetch(lsm);
    if (ret != 0) {
        return 0.0f;
    }
    sensor_channel_get(lsm, SENSOR_CHAN_GYRO_Z, &val);
    return (float)sensor_value_to_double(&val) * RAD_TO_DEG;
}

#define GRAVITY_MS2        9.80665f
#define SHOCK_THRESHOLD_G  0.5f
#define DEADZONE_SHOCK_DPS 5.0f
#define SHOCK_HOLD_MS 100
#define GYRO_COHERENCE_DPS 3.0f

static bool lsm_read_gyro_and_shock(float *gyroZ_dps_out, bool *accel_shock_out) {
    struct sensor_value val, ax, ay, az;
    int ret = sensor_sample_fetch(lsm);
    if (ret != 0) {
        *gyroZ_dps_out = 0.0f;
        *accel_shock_out = false;
        return false;
    }

    sensor_channel_get(lsm, SENSOR_CHAN_GYRO_Z, &val);
    *gyroZ_dps_out = (float)sensor_value_to_double(&val) * RAD_TO_DEG;

    sensor_channel_get(lsm, SENSOR_CHAN_ACCEL_X, &ax);
    sensor_channel_get(lsm, SENSOR_CHAN_ACCEL_Y, &ay);
    sensor_channel_get(lsm, SENSOR_CHAN_ACCEL_Z, &az);

    float ax_ms2 = (float)sensor_value_to_double(&ax);
    float ay_ms2 = (float)sensor_value_to_double(&ay);
    float az_ms2 = (float)sensor_value_to_double(&az);
    /* FIX #1 11/ago: sqrtf eliminada — comparacao quadratica equivalente,
       economiza ~100 ciclos/amostra no Cortex-M4F (sem sqrtf em hardware). */
    float g_const = GRAVITY_MS2;
    float acc_mag_sq = (ax_ms2 * ax_ms2 + ay_ms2 * ay_ms2 + az_ms2 * az_ms2) / (g_const * g_const);
    float upper = (1.0f + SHOCK_THRESHOLD_G) * (1.0f + SHOCK_THRESHOLD_G);
    float lower = (1.0f - SHOCK_THRESHOLD_G) * (1.0f - SHOCK_THRESHOLD_G);

    *accel_shock_out = (acc_mag_sq > upper || acc_mag_sq < lower);
    return true;
}

static int clocks_start(void) {
    int err, res;
    struct onoff_manager *clk_mgr;
    struct onoff_client clk_cli;
    clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    if (!clk_mgr) return -ENXIO;
    sys_notify_init_spinwait(&clk_cli.notify);
    err = onoff_request(clk_mgr, &clk_cli);
    if (err < 0) return err;
    uint32_t deadline_ms = k_uptime_get_32() + 100;
    do {
        err = sys_notify_fetch_result(&clk_cli.notify, &res);
        if (!err && res) return res;
        if (k_uptime_get_32() > deadline_ms) return -ETIMEDOUT;
    } while (err);
    return 0;
}

/* ── ESB ──
   CORRECAO 29/jul: canal e endereco deixam de ser #define fixos e viram
   variaveis runtime, carregadas da NVS no boot (ver nvs_load_channel_addr
   mais abaixo) - permite o comando PAIR reconfigurar sem recompilar.
   FACTORY_* sao os valores de fabrica, usados so' na PRIMEIRA vez (NVS
   vazia) ou se a leitura falhar. Substituem os antigos 95/{0xC3,...}
   como baseline, seguindo a decisao de separacao maxima de canal
   (84-125, fora do teto ISM de 2483,5MHz = canal 83) - ver
   dvs-esp32-canais-125-84-correcao-29jul2026.md no cofre. */
/* CORRECAO 29/jul (tarde): 125 estava ACIMA DO LIMITE REAL da biblioteca
   ESB da Nordic (NCS/Zephyr) - embora o registrador de hardware RF_CH
   aceite ate 125, a biblioteca ESB valida e rejeita canais acima de
   100 (confirmado: engenheiro da Nordic no forum oficial - "you should
   not use a channel number higher than 100", bug de guarda frouxa
   demais no SDK). Isso causava esb_set_rf_channel() falhar,
   dvs_esb_init() retornar erro, e o TX entrar no loop de 9 piscadas de
   erro fatal. Faixa "limpa" (acima do teto ISM 2483,5MHz = canal 83)
   revisada para 84-100 (17 canais totais, nao mais 84-125). */
#define FACTORY_ESB_CHANNEL 96
static const uint8_t FACTORY_TX_ADDR[4] = { 0x8E, 0x2D, 0x64, 0xB1 };

static uint8_t esb_channel_runtime = FACTORY_ESB_CHANNEL;
static uint8_t tx_addr_runtime[4];
static uint8_t tx_prefix_runtime;

static struct esb_payload tx_payload = ESB_CREATE_PAYLOAD(0, 0,0,0,0, 0,0,0,0);

struct dvs_packet { float angle; float velocity; } __packed;

void dvs_esb_event_handler(struct esb_evt const *e) { ARG_UNUSED(e); }

static int dvs_esb_init(void) {
    struct esb_config config = ESB_DEFAULT_CONFIG;
    config.protocol           = ESB_PROTOCOL_ESB_DPL;
    config.mode               = ESB_MODE_PTX;
    config.event_handler      = dvs_esb_event_handler;
    config.bitrate            = ESB_BITRATE_2MBPS;
    config.crc                = ESB_CRC_16BIT;
    config.tx_output_power    = ESB_TX_POWER_8DBM; /* CORRECAO 24/jul: 0dBM -> +8dBm, testando se atenuacao de RF (bateria montada perto da antena) causa perda de pacotes ao parar o disco */
    config.retransmit_count   = 0;
    config.retransmit_delay   = 0;
    config.payload_length     = 8;
    config.selective_auto_ack = true;

    int err = esb_init(&config);
    if (err) return err;
    err = esb_set_base_address_0(tx_addr_runtime);
    if (err) return err;
    err = esb_set_prefixes(&tx_prefix_runtime, 1);
    if (err) return err;
    return esb_set_rf_channel(esb_channel_runtime);
}

/* ── NVS ── */
static struct nvs_fs nvs;
#define NVS_PARTITION        storage_partition
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(NVS_PARTITION)
#define NVS_ID_GYRO_OFFSET   1
/* CORRECAO 24/jul: contador persistente de resets por watchdog (DOG).
   Permite inocentar ou condenar o watchdog formalmente. */
#define NVS_ID_DOG_COUNT     2
/* CORRECAO 29/jul: canal e endereco agora persistidos, gravados pelo
   comando PAIR vindo do RX (ver process_pair_command mais abaixo). */
#define NVS_ID_ESB_CHANNEL   3
#define NVS_ID_TX_ADDR       4
#define CALIB_SANITY_TOLERANCE_DPS 3.0f
#define NVS_WRITE_MIN_DELTA_DPS 0.05f

static bool nvs_ready = false;

/* CORRECAO 29/jul: le canal+endereco da NVS; se nao existir (primeiro
   boot, NVS vazia) ou a leitura falhar, usa o padrao de fabrica. Deve
   ser chamada DEPOIS de nvs_setup() e ANTES de dvs_esb_init(). */
static void nvs_load_channel_addr(void) {
    memcpy(tx_addr_runtime, FACTORY_TX_ADDR, sizeof(tx_addr_runtime));
    esb_channel_runtime = FACTORY_ESB_CHANNEL;

    if (!nvs_ready) {
        tx_prefix_runtime = tx_addr_runtime[0];
        return;
    }

    uint8_t saved_channel;
    if (nvs_read(&nvs, NVS_ID_ESB_CHANNEL, &saved_channel, sizeof(saved_channel))
            == (ssize_t)sizeof(saved_channel)) {
        esb_channel_runtime = saved_channel;
    }

    uint8_t saved_addr[4];
    if (nvs_read(&nvs, NVS_ID_TX_ADDR, saved_addr, sizeof(saved_addr))
            == (ssize_t)sizeof(saved_addr)) {
        memcpy(tx_addr_runtime, saved_addr, sizeof(tx_addr_runtime));
    }

    /* Prefixo sempre = primeiro byte do endereco (mesma convencao ja
       usada nos enderecos hardcoded originais: 0xC3/0x8E repetido). */
    tx_prefix_runtime = tx_addr_runtime[0];
}

static int nvs_setup(void) {
    nvs.flash_device = NVS_PARTITION_DEVICE;
    if (!device_is_ready(nvs.flash_device)) {
        return -ENODEV;
    }
    nvs.offset = NVS_PARTITION_OFFSET;
    struct flash_pages_info info;
    int rc = flash_get_page_info_by_offs(nvs.flash_device, nvs.offset, &info);
    if (rc) return rc;
    nvs.sector_size = info.size;
    nvs.sector_count = 7U;
    rc = nvs_mount(&nvs);
    nvs_ready = (rc == 0);
    return rc;
}

static bool nvs_load_offset(float *out) {
    ssize_t rc = nvs_read(&nvs, NVS_ID_GYRO_OFFSET, out, sizeof(*out));
    return (rc == sizeof(*out));
}

static float last_saved_offset = 0.0f;
static bool  last_saved_valid  = false;

static bool nvs_save_offset_if_needed(float new_offset) {
    if (!nvs_ready) {
        return false;
    }
    if (last_saved_valid && fabsf(new_offset - last_saved_offset) < NVS_WRITE_MIN_DELTA_DPS) {
        return false;
    }
    ssize_t wr = nvs_write(&nvs, NVS_ID_GYRO_OFFSET, &new_offset, sizeof(new_offset));
    if (wr != (ssize_t)sizeof(new_offset)) {
        return false;
    }
    last_saved_offset = new_offset;
    last_saved_valid  = true;
    return true;
}

#define CALIB_SAMPLES_QUICK 250

static float gyroZ_offset = 0.0f;
static float angle        = 0.0f;
static uint32_t idle_accum_ms = 0;
static int32_t  shock_hold_remaining_ms = 0;

/* CORRECAO 24/jul: contador de resets DOG carregado/atualizado no boot */
static uint32_t dog_reset_count = 0;

/* CORRECAO 23/jul: estado do dock, para a logica de "Docked ativo" */
static bool     tx_docked_state    = false;
static bool     dock_present_prev  = false;
static uint32_t undock_debounce_ms = 0;
/* CORRECAO 29/jul (tarde): reset apos pareamento NAO acontece mais
   imediatamente (ainda docked) - fica pendente ate o TX sair
   FISICAMENTE do dock (depois do debounce de 5s), dando tempo do
   usuario terminar de manusear TX/dock antes do reset cortar tudo. */
static bool     pending_reset_after_pairing = false;

static void enter_system_off(void) {
    nvs_save_offset_if_needed(gyroZ_offset);
    gpio_pin_set_dt(&led, 0);

    /* CORRECAO 07/ago (v2 - sem depender de CONFIG_PM_DEVICE): sem
       isso, o radio ESB e o sensor LSM6DS3 continuam consumindo
       corrente normal mesmo com o nRF52840 em System OFF - medido
       1.8mA de consumo real "dormindo", contra ~0.3-1.5uA teorico do
       System OFF sozinho.
       Radio: esb_disable() para o radio embutido.
       Sensor: sensor_attr_set com frequencia 0 pede power-down via
       ODR=0 - jeito padrao/portavel do Zephyr, funciona com qualquer
       driver que implemente o attr_set basico (bem mais comum que
       suporte completo a CONFIG_PM_DEVICE). */
    esb_disable();
    if (device_is_ready(lsm)) {
        struct sensor_value freq_off = { .val1 = 0, .val2 = 0 };
        sensor_attr_set(lsm, SENSOR_CHAN_GYRO_XYZ,
                         SENSOR_ATTR_SAMPLING_FREQUENCY, &freq_off);
        sensor_attr_set(lsm, SENSOR_CHAN_ACCEL_XYZ,
                         SENSOR_ATTR_SAMPLING_FREQUENCY, &freq_off);
    }

    gpio_pin_configure_dt(&dock_detect, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_interrupt_configure_dt(&dock_detect, GPIO_INT_LEVEL_ACTIVE);
    sys_poweroff();
}

/* CORRECAO 29/jul: processa o comando PAIR capturado pela ISR
   (pair_line_buf/pair_line_ready). Chamada de dentro do worker thread,
   SO enquanto o TX estiver docked (mesma exigencia de seguranca ja
   usada no DVSDFU: comando que reconfigura o radio so' e aceito com
   presenca fisica confirmada no dock, protegendo contra ruido/EMI
   isolado disparando isso por acidente enquanto o TX esta no prato).
   Formato esperado (depois do "PAIR:" ja consumido pela ISR):
   "<canal decimal>:<endereco hex, 8 digitos>", ex "91:C35A7198". */
static void process_pair_command(void) {
    if (!tx_docked_state) {
        /* comando so' e aceito docked - ignora silenciosamente (nao
           manda PAIRACK nenhum, ja que nem deveria estar chegando
           nesse estado na pratica: o RX so' fala com o TX pela UART
           do dock, que so' tem sinal real com o TX fisicamente
           encaixado). */
        return;
    }

    unsigned int channel = 0;
    unsigned int addr_bytes[4] = {0};
    /* "%u:%2x%2x%2x%2x" - canal decimal, depois 4 pares de hex */
    int parsed = sscanf(pair_line_buf, "%u:%2x%2x%2x%2x",
                         &channel, &addr_bytes[0], &addr_bytes[1],
                         &addr_bytes[2], &addr_bytes[3]);

    bool ok = false;
    if (parsed == 5 && channel <= 100) {
        /* CORRECAO 29/jul (tarde): limite corrigido de <=125 para <=100 -
           a biblioteca ESB da Nordic (NCS/Zephyr) so' suporta oficialmente
           ate canal 100, mesmo o registrador de hardware RF_CH aceitando
           ate 125. Canal >100 causava esb_set_rf_channel() falhar no
           proximo boot (loop de 9 piscadas de erro fatal) - descoberto na
           pratica gravando canal 125 hoje. */
        uint8_t new_channel = (uint8_t)channel;
        uint8_t new_addr[4] = {
            (uint8_t)addr_bytes[0], (uint8_t)addr_bytes[1],
            (uint8_t)addr_bytes[2], (uint8_t)addr_bytes[3]
        };

        if (nvs_ready) {
            ssize_t w1 = nvs_write(&nvs, NVS_ID_ESB_CHANNEL, &new_channel, sizeof(new_channel));
            ssize_t w2 = nvs_write(&nvs, NVS_ID_TX_ADDR, new_addr, sizeof(new_addr));
            ok = (w1 == (ssize_t)sizeof(new_channel)) && (w2 == (ssize_t)sizeof(new_addr));
        }
    }

    dock_uart_send_line(ok ? "PAIRACK:OK\r\n" : "PAIRACK:FAIL\r\n");

    if (ok) {
        /* CORRECAO 29/jul (tarde): NAO reinicia mais aqui - so marca a
           intencao. O reset de verdade so acontece quando o TX sair
           fisicamente do dock (undock_debounce_ms chegar a zero), la
           embaixo no loop principal - ver bloco "else if (tx_docked_state)"
           que trata a transicao docked->undocked. Isso evita reiniciar o
           TX enquanto ele ainda esta sendo manuseado dentro do dock. */
        pending_reset_after_pairing = true;
    }
}

#define DEADZONE_DPS 0.15f
#define ZUPT_THRESHOLD_DPS   0.25f
#define ZUPT_STABLE_SAMPLES  250
#define ZUPT_ALPHA           0.02f

static uint32_t zupt_stable_count = 0;
/* FIX #2 11/ago: double -> float — FPU do nRF52840 e simples precisao apenas */
static float    zupt_sum_raw      = 0.0f;

static const struct adc_dt_spec batt_adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

#define BATT_MV_GOOD     3600
#define BATT_MV_LOW      3400
#define BATT_MV_CRITICAL 3200

static volatile uint32_t led_blink_period_ms = 0;

static int battery_read_mv(int *out_mv) {
    uint16_t raw;
    struct adc_sequence sequence = {
        .buffer = &raw,
        .buffer_size = sizeof(raw),
    };
    int err = adc_sequence_init_dt(&batt_adc, &sequence);
    if (err) return err;
    err = adc_read_dt(&batt_adc, &sequence);
    if (err) return err;
    int32_t val = raw;
    err = adc_raw_to_millivolts_dt(&batt_adc, &val);
    if (err) return err;
    *out_mv = val * 5;
    return 0;
}

K_SEM_DEFINE(sample_sem, 0, 1);

static void drdy_trigger_handler(const struct device *dev, const struct sensor_trigger *trig) {
    ARG_UNUSED(dev);
    ARG_UNUSED(trig);
    k_sem_give(&sample_sem);
}

static int drdy_setup(void) {
    struct sensor_trigger trig = {
        .type = SENSOR_TRIG_DATA_READY,
        .chan = SENSOR_CHAN_GYRO_XYZ,
    };
    return sensor_trigger_set(lsm, &trig, drdy_trigger_handler);
}

static uint32_t dbg_count = 0;
static uint32_t dbg_spi_us_total = 0;
static uint32_t dbg_esb_us_total = 0;
static uint32_t dbg_last_report_ms = 0;
static uint32_t dbg_loop_last_ms = 0;
static uint32_t dbg_loop_dt_max_ms = 0;

static const struct device *wdt_dev;
static int wdt_channel_id = -1;

static void dvs_worker(void *a, void *b, void *c) {
    dbg_loop_last_ms = k_uptime_get_32();
    /* FIX #3 11/ago: cycle counter para dt da integracao do angulo */
    uint32_t last_cyc = k_cycle_get_32();
    dbg_last_report_ms = dbg_loop_last_ms;

    while (1) {
        k_sem_take(&sample_sem, K_FOREVER);

        uint32_t now_ms = k_uptime_get_32();
        uint32_t loop_dt_ms = now_ms - dbg_loop_last_ms;
        dbg_loop_last_ms = now_ms;
        if (loop_dt_ms > dbg_loop_dt_max_ms) dbg_loop_dt_max_ms = loop_dt_ms;

        /* ── Docked ativo: suprime ESB enquanto o TX estiver no dock ──
           CORRECAO 23/jul: na borda de subida (acabou de encaixar), manda
           um ultimo pacote com velocity=0 explicito ANTES de calar.
           CORRECAO 24/jul: a entrada no estado docked agora exige
           DOCK_CONFIRM_SAMPLES leituras altas CONSECUTIVAS do pino
           (~29ms a 416Hz) - simetrico ao debounce de saida que ja
           existia. Uma leitura espuria isolada nao dispara mais nada. */
        bool dock_raw = gpio_pin_get_dt(&dock_detect) > 0;
        bool dock_present;
        if (dock_raw) {
            if (dock_confirm_count < DOCK_CONFIRM_SAMPLES) {
                dock_confirm_count++;
            }
            dock_present = (dock_confirm_count >= DOCK_CONFIRM_SAMPLES);
        } else {
            dock_confirm_count = 0;
            dock_present = false;
        }

        if (dock_present && !dock_present_prev) {
            struct dvs_packet zero_pkt = { .angle = angle, .velocity = 0.0f };
            memcpy(tx_payload.data, &zero_pkt, sizeof(zero_pkt));
            tx_payload.length = sizeof(zero_pkt);
            tx_payload.noack  = true;
            esb_write_payload(&tx_payload);

            /* CORRECAO 24/jul: momento SEGURO para persistir o offset em
               flash - o audio acabou de ser suprimido de proposito, entao
               o stall do NVMC (ate ~85ms se houver GC/erase de setor) nao
               tem efeito audivel nenhum. Este e o novo ponto de save
               periodico, substituindo o save que ficava dentro do bloco
               ZUPT do caminho de tempo real (removido - ver comentario
               no bloco ZUPT abaixo). */
            nvs_save_offset_if_needed(gyroZ_offset);

            /* CORRECAO 24/jul: relatorio de diagnostico pro dock - o
               contador DOG persistido diz se houve reset por watchdog
               desde o ultimo boot/consulta. */
            char diag[64];
            snprintk(diag, sizeof(diag), "DVSDIAG DOG=%u OFF=%d.%03d\r\n",
                     dog_reset_count, (int)gyroZ_offset,
                     (int)(fabsf(gyroZ_offset - (int)gyroZ_offset) * 1000));
            dock_uart_send_line(diag);
        }
        dock_present_prev = dock_present;

        if (dock_present) {
            tx_docked_state    = true;
            undock_debounce_ms = UNDOCK_DEBOUNCE_MS;
        } else if (tx_docked_state) {
            if (undock_debounce_ms > loop_dt_ms) {
                undock_debounce_ms -= loop_dt_ms;
            } else {
                undock_debounce_ms = 0;
                tx_docked_state = false;

                /* CORRECAO 29/jul (tarde): se um pareamento foi confirmado
                   (PAIRACK:OK ja enviado) enquanto o TX estava docked, o
                   reset de verdade acontece SO AGORA, no exato instante em
                   que o TX acabou de sair do dock (debounce de 5s ja
                   decorrido) - nao antes, enquanto ainda podia estar sendo
                   manuseado. Depois do reset, o proximo boot ja le o
                   canal/endereco novo da NVS antes de dvs_esb_init(). */
                if (pending_reset_after_pairing) {
                    pending_reset_after_pairing = false;
                    NVIC_SystemReset();
                }
            }
        }

        if (tx_docked_state) {
            /* CORRECAO 29/jul: verifica se a ISR acumulou um comando
               PAIR completo, e processa aqui (fora de ISR - seguro pra
               chamar nvs_write/dock_uart_send_line/reset). */
            if (clear_offset_requested) {
                clear_offset_requested = false;
                if (tx_docked_state) {
                    nvs_delete(&nvs, NVS_ID_GYRO_OFFSET);
                    last_saved_valid = false; /* forca proxima gravacao real */
                    dock_uart_send_line("CLEAROFFSET:OK\r\n");
                } else {
                    dock_uart_send_line("CLEAROFFSET:FAIL_NOT_DOCKED\r\n");
                }
            }
            if (enter_dfu_requested) {
                enter_dfu_requested = false;
                if (tx_docked_state) {
                    dock_uart_send_line("ENTERDFU:OK\r\n");
                    /* Da tempo do ACK acima realmente sair pela UART
                       antes do reset - sem isso, o RX pode nunca ver
                       a confirmacao (TX some da UART assim que reseta
                       para o bootloader). */
                    k_msleep(50);
                    NRF_POWER->GPREGRET = 0x57; /* entra em modo UF2 */
                    NVIC_SystemReset();
                } else {
                    dock_uart_send_line("ENTERDFU:FAIL_NOT_DOCKED\r\n");
                }
            }
            if (pair_line_ready) {
                process_pair_command();
                pair_line_ready = false;
            }

            /* CORRECAO 23/jul: continua lendo o sensor (so para manter o
               DRDY limpo e a interrupcao viva) mesmo docked - sem isso, o
               flag de dado pronto do LSM6DS3 nunca e limpo (so se limpa
               na leitura), a interrupcao para de disparar, a thread fica
               presa no k_sem_take() para sempre, o watchdog nunca e
               alimentado (so roda depois do k_sem_take), e o TX entra
               num loop de reset a cada ~4s (a janela do watchdog).
               Confirmado na pratica. O valor lido aqui e descartado
               de proposito - nao processa angulo, nao transmite ESB. */
            float dummy_gyro;
            bool dummy_shock;
            lsm_read_gyro_and_shock(&dummy_gyro, &dummy_shock);

            if (wdt_channel_id >= 0) {
                wdt_feed(wdt_dev, wdt_channel_id);
            }
            gpio_pin_set_dt(&led, 1);
            continue;
        }

        uint32_t t0 = k_cycle_get_32();
        float gyroZ_raw;
        bool accel_shock;
        lsm_read_gyro_and_shock(&gyroZ_raw, &accel_shock);
        float gyroZ = gyroZ_raw - gyroZ_offset;
        uint32_t t1 = k_cycle_get_32();

        bool shock_detected = accel_shock && (fabsf(gyroZ) < GYRO_COHERENCE_DPS);

        if (shock_detected) {
            shock_hold_remaining_ms = SHOCK_HOLD_MS;
        } else if (shock_hold_remaining_ms > 0) {
            if (fabsf(gyroZ) >= GYRO_COHERENCE_DPS) {
                shock_hold_remaining_ms = 0;
            } else {
                shock_hold_remaining_ms -= (int32_t)loop_dt_ms;
            }
        }

        float active_deadzone = (shock_detected || shock_hold_remaining_ms > 0)
                                     ? DEADZONE_SHOCK_DPS : DEADZONE_DPS;
        if (fabsf(gyroZ) < active_deadzone) gyroZ = 0.0f;

        if (gyroZ == 0.0f) {
            idle_accum_ms += loop_dt_ms;
            if (idle_accum_ms >= IDLE_POWEROFF_MS) {
                enter_system_off();
            }
        } else {
            idle_accum_ms = 0;
        }

        if (fabsf(gyroZ) < ZUPT_THRESHOLD_DPS) {
            zupt_sum_raw += gyroZ_raw;
            zupt_stable_count++;
            if (zupt_stable_count >= ZUPT_STABLE_SAMPLES) {
                float window_avg_raw = zupt_sum_raw / (float)zupt_stable_count;
                gyroZ_offset += (window_avg_raw - gyroZ_offset) * ZUPT_ALPHA;
                /* CORRECAO 24/jul: REMOVIDO o nvs_save_offset_if_needed()
                   que ficava aqui. Motivo (especifico do nRF52, nao
                   teorico): o NVMC executa write/erase com o CPU
                   ESTAGNADO - ~41us por palavra escrita e, quando o NVS
                   precisa rotacionar setor (garbage collection), um page
                   erase de ~85ms com o CPU completamente parado. 85ms
                   dentro da thread de tempo real = nenhuma amostra DRDY
                   processada, nenhum pacote ESB, velocidade congelada no
                   RX - um soluco audivel em momento imprevisivel.
                   O offset continua sendo atualizado em RAM normalmente
                   (a linha acima); a PERSISTENCIA agora acontece so em
                   momentos seguros: entrada no dock (audio ja suprimido)
                   e enter_system_off(). Se o TX desligar por bateria sem
                   passar por nenhum dos dois, perde-se no maximo a deriva
                   de offset de uma sessao - recuperada pela calibracao
                   rapida do proximo boot. */
                zupt_stable_count = 0;
                zupt_sum_raw = 0.0f;
            }
        } else {
            zupt_stable_count = 0;
            zupt_sum_raw = 0.0f;
        }

        /* FIX #3 11/ago: dt via cycle counter (resolucao ns em vez de ms) */
        {
            uint32_t now_cyc = k_cycle_get_32();
            uint32_t cyc_dt = now_cyc - last_cyc;
            last_cyc = now_cyc;
            float dt_sec = (float)k_cyc_to_ns_floor64(cyc_dt) / 1e9f;
            angle += gyroZ * dt_sec;
        }
        if (angle >  180.0f) angle -= 360.0f;
        if (angle < -180.0f) angle += 360.0f;

        struct dvs_packet pkt = { .angle = angle, .velocity = gyroZ };
        memcpy(tx_payload.data, &pkt, sizeof(pkt));
        tx_payload.length = sizeof(pkt);
        tx_payload.noack = true;

        uint32_t t2 = k_cycle_get_32();
        int ret = esb_write_payload(&tx_payload);
        uint32_t t3 = k_cycle_get_32();

        if (ret != 0) {
            gpio_pin_set_dt(&led, 1);
        } else {
            uint32_t period = led_blink_period_ms;
            if (period == 0) {
                gpio_pin_set_dt(&led, 1);
            } else {
                bool on = ((now_ms / period) % 2u) == 0u;
                gpio_pin_set_dt(&led, on ? 1 : 0);
            }
        }

        if (wdt_channel_id >= 0) {
            wdt_feed(wdt_dev, wdt_channel_id);
        }

        dbg_spi_us_total += (uint32_t)(k_cyc_to_ns_floor64(t1 - t0) / 1000);
        dbg_esb_us_total += (uint32_t)(k_cyc_to_ns_floor64(t3 - t2) / 1000);
        dbg_count++;

        if (now_ms - dbg_last_report_ms >= 2000) {
            uint32_t elapsed_ms = now_ms - dbg_last_report_ms;
            uint32_t hz_x10 = (dbg_count * 10000) / elapsed_ms;
            uint32_t spi_avg_us = dbg_count ? dbg_spi_us_total / dbg_count : 0;
            uint32_t esb_avg_us = dbg_count ? dbg_esb_us_total / dbg_count : 0;

            printk("[DBG] loop=%u.%uHz gyro_avg=%uus esb_avg=%uus loop_dt_max=%ums last_ret=%d offset=%d.%03d dog=%u\n",
                   hz_x10 / 10, hz_x10 % 10, spi_avg_us, esb_avg_us, dbg_loop_dt_max_ms, ret,
                   (int)gyroZ_offset, (int)(fabsf(gyroZ_offset - (int)gyroZ_offset) * 1000),
                   dog_reset_count);

            dbg_count = 0;
            dbg_spi_us_total = 0;
            dbg_esb_us_total = 0;
            dbg_loop_dt_max_ms = 0;
            dbg_last_report_ms = now_ms;
        }
    }
}
K_THREAD_DEFINE(worker_tid, 4096, dvs_worker, NULL, NULL, NULL, 2, 0, 0);

int main(void) {
    /* CORRECAO 24/jul: le e limpa o RESETREAS ANTES de qualquer outra
       coisa (mesmo diagnostico que o RX ja tinha e o TX nao). O valor
       e usado mais abaixo, depois do NVS montar, para incrementar o
       contador persistente de resets DOG. */
    uint32_t reset_reas = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = 0xFFFFFFFFUL;

    lsm = DEVICE_DT_GET_ONE(st_lsm6ds3);

    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    k_msleep(10);

    gpio_pin_configure_dt(&dock_detect, GPIO_INPUT | GPIO_PULL_DOWN);

    dock_uart_setup();

    pisca(1, 500);

    if (!device_is_ready(lsm)) {
        while(1) { pisca(10, 50); k_msleep(1000); }
    }
    pisca(2, 150);

    bool have_saved_offset = false;
    float saved_offset = 0.0f;
    if (nvs_setup() == 0) {
        have_saved_offset = nvs_load_offset(&saved_offset);
        if (have_saved_offset) {
            last_saved_offset = saved_offset;
            last_saved_valid  = true;
            pisca(4, 100);
        }

        /* CORRECAO 24/jul: carrega o contador DOG persistido e, se ESTE
           boot foi causado por watchdog (bit 1 do RESETREAS), incrementa
           e regrava. Consulta: via printk (campo dog= no [DBG]) ou via
           linha DVSDIAG no UART do dock ao encaixar. */
        uint32_t saved_dog = 0;
        if (nvs_read(&nvs, NVS_ID_DOG_COUNT, &saved_dog, sizeof(saved_dog))
                == (ssize_t)sizeof(saved_dog)) {
            dog_reset_count = saved_dog;
        }
        if (reset_reas & (1UL << 1)) {
            dog_reset_count++;
            nvs_write(&nvs, NVS_ID_DOG_COUNT, &dog_reset_count,
                      sizeof(dog_reset_count));
        }
    }

    /* CORRECAO 29/jul: carrega canal+endereco da NVS (ou padrao de
       fabrica, se nunca foi pareado) - precisa vir DEPOIS do nvs_setup()
       (usa nvs_ready) e ANTES do dvs_esb_init() logo abaixo. */
    nvs_load_channel_addr();

    double soma = 0;
    for (int i = 0; i < CALIB_SAMPLES_QUICK; i++) {
        soma += (double)lsm_read_gyro_z_dps();
        k_msleep(2);
    }
    float quick_offset = (float)(soma / CALIB_SAMPLES_QUICK);

    if (!have_saved_offset) {
        gyroZ_offset = quick_offset;
        nvs_save_offset_if_needed(gyroZ_offset);
        pisca(3, 150);
    } else if (fabsf(quick_offset - saved_offset) < CALIB_SANITY_TOLERANCE_DPS) {
        gyroZ_offset = quick_offset;
        nvs_save_offset_if_needed(gyroZ_offset);
        pisca(3, 150);
    } else {
        gyroZ_offset = saved_offset;
        pisca(6, 150);
    }

    clocks_start();

    int err = dvs_esb_init();
    if (err) {
        while(1) { pisca(9, 80); k_msleep(1000); }
    }
    pisca(5, 80);

    if (drdy_setup() != 0) {
        while(1) { pisca(7, 60); k_msleep(1000); }
    }

    wdt_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt));
    if (wdt_dev != NULL && device_is_ready(wdt_dev)) {
        struct wdt_timeout_cfg wdt_cfg = {
            .window.min = 0,
            .window.max = 4000,
            .callback = NULL,
            .flags = WDT_FLAG_RESET_SOC,
        };
        wdt_channel_id = wdt_install_timeout(wdt_dev, &wdt_cfg);
        if (wdt_channel_id >= 0) {
            wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
        }
    }

    if (adc_is_ready_dt(&batt_adc)) {
        adc_channel_setup_dt(&batt_adc);
    }

    while (1) {
        int mv;
        if (battery_read_mv(&mv) == 0) {
            if (mv >= BATT_MV_GOOD) {
                led_blink_period_ms = 0;
            } else if (mv >= BATT_MV_LOW) {
                led_blink_period_ms = 1000;
            } else if (mv >= BATT_MV_CRITICAL) {
                led_blink_period_ms = 400;
            } else {
                led_blink_period_ms = 150;
            }
        }
        k_msleep(5000);
    }
    return 0;
}
