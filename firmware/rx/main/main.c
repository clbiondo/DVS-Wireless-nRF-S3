/* main.c - DVS Wireless RX: 2 decks (nRF24L01 + PCM5102) num unico ESP32-S3.
 *
 * Arquitetura: cada deck tem seu proprio radio (SPI independente) e seu
 * proprio I2S (I2S_NUM_0 para deck1, I2S_NUM_1 para deck2). Radios ficam
 * no nucleo 0 (recepcao, IRQ por hardware); sintese/I2S ficam no nucleo 1
 * (tempo real de audio, prioridade alta).
 *
 * Pinagem baseada na foto real da placa (DevKitC-1 clone "HW678",
 * ESP32-S3-N8R2). GPIO35/36/37 EVITADOS DE PROPOSITO (dedicados a PSRAM
 * octal nessa variante R2, mesmo aparecendo no header fisico). Deck2
 * migrado pra fora da faixa GP38-42 apos confirmar que aqueles pinos
 * (JTAG classico do ESP32-S3: MTCK=39/MTDO=40/MTDI=41/MTMS=42) causavam
 * comunicacao SPI incorreta com o nRF24L01 nesta placa - regra permanente
 * de pinagem daqui pra frente, nao usar essa faixa pra SPI/perifericos.
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "nrf24.h"
#include "tone_synth.h"
#include "cv02_synth.h"
#include "timecode_formats.h"
#include "dock_pairing.h"
#include "dock_led.h"
#include "dock_bridge.h"

static const char *TAG = "dvs_rx";

/* ── Pinagem deck1 (lado esquerdo da placa) ── */
#define D1_SCK   12
#define D1_MOSI  11
#define D1_MISO  13
#define D1_CSN   10
#define D1_CE    9
#define D1_IRQ   14
#define D1_I2S_BCK  4
#define D1_I2S_LRCK 6
#define D1_I2S_DOUT 5

/* ── Pinagem deck2 (lado direito da placa) ──
   CORRECAO 26/jul: pinos originais (GP38-42) coincidem exatamente com os
   4 pinos classicos de JTAG do ESP32-S3 (MTCK=39, MTDO=40, MTDI=41,
   MTMS=42) - suspeita de causa da comunicacao instavel observada
   (CONFIG sempre errado, independente de modulo/fiacao/alimentacao ja
   testados e descartados). Teste: mover TUDO pra uma faixa de pinos
   longe de qualquer strapping/JTAG/PSRAM, pra isolar se resolve. */
#define D2_SCK   7
#define D2_MOSI  8
#define D2_MISO  15
#define D2_CSN   16
#define D2_CE    17
#define D2_IRQ   18
#define D2_I2S_BCK  42
#define D2_I2S_LRCK 2
#define D2_I2S_DOUT 1

#define SAMPLE_RATE   44444
#define BLOCK_SAMPLES 64

/* CORRECAO 29/jul: namespace/chaves de NVS onde o dock_pairing.c grava o
   canal sorteado - main.c le aqui no boot, ANTES de deck_setup(), para
   que o proprio RX suba no canal certo apos um esp_restart() disparado
   pelo pareamento. Fallback (NVS vazia = nunca pareado ainda) e' o novo
   padrao de fabrica (125/84), consistente com o firmware TX atualizado
   em 29/jul (ver dvs-tx-pareamento-nvs-implementado-29jul2026.md). */
#define DOCK_NVS_NAMESPACE   "dvs_dock"
#define DOCK_NVS_KEY_DECK1   "ch_deck1"
#define DOCK_NVS_KEY_DECK2   "ch_deck2"
#define DOCK_NVS_KEY_ADDR_DECK1 "addr_deck1"
#define DOCK_NVS_KEY_ADDR_DECK2 "addr_deck2"
#define DOCK_NVS_KEY_TCFMT   "tc_fmt"   /* formato de timecode (0-3) */
/* CORRECAO 29/jul (tarde): 125/84 -> 87/96. Canal 125 estava ACIMA DO
   LIMITE REAL da biblioteca ESB da Nordic (max validado = 100, nao 125
   como o registrador de hardware RF_CH tecnicamente aceitaria) -
   causava esb_set_rf_channel() falhar no TX (loop de 9 piscadas de
   erro). Faixa "limpa" revisada para 84-100 (nao mais 84-125) - ver
   dock_pairing.c para os novos ranges de sorteio por deck. */
#define FACTORY_CHANNEL_DECK1 87
#define FACTORY_CHANNEL_DECK2 96

typedef struct {
    nrf24_dev_t radio;
    tone_state_t tone;               /* sintese de tom puro, sem NoiseMap (estilo Phase DJ) */
    cv02_state_t cv02;               /* sintese CV02 real (LUT+NoiseMap+dual-level) */
    volatile float raw_velocity;      /* velocidade bruta do radio (pre-calibracao) */
    volatile uint32_t raw_last_pkt_ms; /* timestamp do ultimo pacote valido */
    i2s_chan_handle_t i2s_tx;
    QueueHandle_t irq_evt_queue;
} deck_ctx_t;

static deck_ctx_t deck1;
static deck_ctx_t deck2;

static float deck1_vel_correction = 0.99538f;
static float deck2_vel_correction = 1.00044f;
static portMUX_TYPE velocity_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* ── ISR de GPIO: so envia o ponteiro do deck pra fila, nada de trabalho
   pesado dentro da ISR (boa pratica - handler real roda na task) ── */
static void IRAM_ATTR gpio_irq_handler(void *arg) {
    deck_ctx_t *ctx = (deck_ctx_t *)arg;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(ctx->irq_evt_queue, &ctx, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/*/* ── Task de radio: uma por deck, nucleo 0. Espera IRQ, le pacote, atualiza o estado do deck. ── */
static void radio_task(void *arg) {
    deck_ctx_t *ctx = (deck_ctx_t *)arg;

    esp_err_t err = nrf24_init(&ctx->radio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] falha ao iniciar radio, task suspensa", ctx->radio.label);
        vTaskSuspend(NULL);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << ctx->radio.pin_irq),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE, /* IRQ do nRF24 e ativo-baixo */
    };
    gpio_config(&io_conf);
    gpio_isr_handler_add(ctx->radio.pin_irq, gpio_irq_handler, ctx);

    nrf24_enter_rx(&ctx->radio);

    ESP_LOGI(TAG, "[%s] task de radio pronta, aguardando pacotes...", ctx->radio.label);

    deck_ctx_t *evt;
    uint32_t pkt_count = 0;
    uint32_t fifo_extra = 0;
    while (1) {
        if (xQueueReceive(ctx->irq_evt_queue, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {
            dvs_packet_t pkt;
            uint32_t drained = 0;
            if (nrf24_read_latest_packet(&ctx->radio, &pkt, &drained)) {
                if (drained > 1) {
                    fifo_extra++;
                }

                if (isfinite(pkt.velocity) && fabsf(pkt.velocity) < 4000.0f) {
                    uint32_t now_ms_radio = (uint32_t)(esp_timer_get_time() / 1000);
                    taskENTER_CRITICAL(&velocity_spinlock);
                    ctx->raw_velocity = pkt.velocity;
                    ctx->raw_last_pkt_ms = now_ms_radio;
                    ctx->cv02.current_velocity = pkt.velocity;
                    ctx->cv02.last_pkt_ms = now_ms_radio;
                    taskEXIT_CRITICAL(&velocity_spinlock);
                }

                pkt_count++;
                if (pkt_count % 2000 == 0) {
                    ESP_LOGI(TAG, "[%s] pkt#%lu vel=%.2f fifo_extra=%lu",
                             ctx->radio.label, (unsigned long)pkt_count, pkt.velocity,
                             (unsigned long)fifo_extra);
                }
            }
        }
    }
}

/* ── Task de audio: uma por deck, nucleo 1. ── */
static void audio_task(void *arg) {
    deck_ctx_t *ctx = (deck_ctx_t *)arg;
    uint32_t buf[BLOCK_SAMPLES];

    ESP_LOGI(TAG, "[%s] task de audio iniciada (sintese: CV02 (LUT+NoiseMap))", ctx->radio.label);

    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        float vel;
        uint32_t last_ms;
        taskENTER_CRITICAL(&velocity_spinlock);
        vel = ctx->raw_velocity;
        last_ms = ctx->raw_last_pkt_ms;
        taskEXIT_CRITICAL(&velocity_spinlock);
        if ((now_ms - last_ms) > 200) {
            vel = 0.0f;
        }
        if (ctx == &deck1) {
            vel *= deck1_vel_correction;
        } else if (ctx == &deck2) {
            vel *= deck2_vel_correction;
        }
        ctx->cv02.current_velocity = vel;
        ctx->cv02.last_pkt_ms = last_ms;
        cv02_fill(&ctx->cv02, buf, BLOCK_SAMPLES, now_ms);

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(ctx->i2s_tx, buf, sizeof(buf), &bytes_written, portMAX_DELAY);
        if (err != ESP_OK || bytes_written != sizeof(buf)) {
            ESP_LOGW(TAG, "[%s] i2s_channel_write incompleto: err=%d bytes=%d",
                     ctx->radio.label, err, (int)bytes_written);
        }
    }
}

static void i2s_setup(deck_ctx_t *ctx, i2s_port_t port, int bck, int lrck, int dout) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);

    chan_cfg.dma_frame_num       = 64;
    chan_cfg.dma_desc_num        = 4;
    chan_cfg.auto_clear_after_cb = true;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &ctx->i2s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bck,
            .ws   = lrck,
            .dout = dout,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(ctx->i2s_tx, &std_cfg));

    uint32_t prime_buf[BLOCK_SAMPLES];
    size_t loaded;
    for (int i = 0; i < 4; i++) {
        memset(prime_buf, 0, sizeof(prime_buf));
        ESP_ERROR_CHECK(i2s_channel_preload_data(ctx->i2s_tx, prime_buf,
                                                 sizeof(prime_buf), &loaded));
        if (loaded < sizeof(prime_buf)) {
            break;
        }
    }

    ESP_LOGI(TAG, "I2S port=%d configurado: dma_desc_num=%lu dma_frame_num=%lu (%.2fms de pipeline)",
             (int)port, (unsigned long)chan_cfg.dma_desc_num, (unsigned long)chan_cfg.dma_frame_num,
             (chan_cfg.dma_desc_num * chan_cfg.dma_frame_num * 1000.0f) / SAMPLE_RATE);

    ESP_ERROR_CHECK(i2s_channel_enable(ctx->i2s_tx));
}

static void deck_setup(deck_ctx_t *ctx, const char *label, uint8_t channel,
                       const uint8_t addr[5], int sck, int mosi, int miso,
                       int csn, int ce, int irq, spi_host_device_t spi_host) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->radio.spi_host  = spi_host;
    ctx->radio.pin_sck   = sck;
    ctx->radio.pin_mosi  = mosi;
    ctx->radio.pin_miso  = miso;
    ctx->radio.pin_csn   = csn;
    ctx->radio.pin_ce    = ce;
    ctx->radio.pin_irq   = irq;
    ctx->radio.rf_channel = channel;
    memcpy(ctx->radio.addr_bytes, addr, 5);
    ctx->radio.label = label;

    tone_state_init(&ctx->tone);
    cv02_state_init(&ctx->cv02);
    ctx->irq_evt_queue = xQueueCreate(4, sizeof(deck_ctx_t *));
}

/* CORRECAO 29/jul: le o canal sorteado (se houver) da NVS do namespace
   "dvs_dock" - mesmo namespace/chaves que dock_pairing.c usa pra
   gravar. Fallback pro padrao de fabrica (125/84) se a NVS estiver
   vazia (nunca pareado) ou a leitura falhar por qualquer motivo. NAO
   abre em modo READWRITE aqui - so' leitura, dock_pairing.c e' quem
   escreve. */
static void load_channels_from_nvs(uint8_t *ch_deck1, uint8_t *ch_deck2) {
    *ch_deck1 = FACTORY_CHANNEL_DECK1;
    *ch_deck2 = FACTORY_CHANNEL_DECK2;

    nvs_handle_t h;
    esp_err_t err = nvs_open(DOCK_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS '%s' ainda nao existe (nunca pareado) - usando canais de fabrica "
                       "deck1=%u deck2=%u", DOCK_NVS_NAMESPACE, *ch_deck1, *ch_deck2);
        return;
    }

    uint8_t v;
    if (nvs_get_u8(h, DOCK_NVS_KEY_DECK1, &v) == ESP_OK) {
        *ch_deck1 = v;
    }
    if (nvs_get_u8(h, DOCK_NVS_KEY_DECK2, &v) == ESP_OK) {
        *ch_deck2 = v;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "Canais carregados da NVS: deck1=%u deck2=%u", *ch_deck1, *ch_deck2);
}

/* CORRECAO 02/ago: endereco deixou de ser fixo por identidade - agora e'
   sorteado e salvo pelo dock_pairing.c a cada pareamento, junto com o
   canal, pra evitar colisao entre placas fisicas diferentes com o
   mesmo firmware/endereco de fabrica. Le o blob de 4 bytes da NVS
   (mesma namespace/chaves que dock_pairing.c usa), com fallback pro
   endereco de fabrica se nunca pareado. */
static void load_addresses_from_nvs(uint8_t out_deck1[4], uint8_t out_deck2[4]) {
    static const uint8_t factory_deck1[4] = { 0xC3, 0x5A, 0x71, 0x98 };
    static const uint8_t factory_deck2[4] = { 0x8E, 0x2D, 0x64, 0xB1 };
    memcpy(out_deck1, factory_deck1, 4);
    memcpy(out_deck2, factory_deck2, 4);

    nvs_handle_t h;
    esp_err_t err = nvs_open(DOCK_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS '%s' ainda nao existe (nunca pareado) - usando enderecos de fabrica",
                 DOCK_NVS_NAMESPACE);
        return;
    }
    size_t len = 4;
    if (nvs_get_blob(h, DOCK_NVS_KEY_ADDR_DECK1, out_deck1, &len) != ESP_OK || len != 4) {
        memcpy(out_deck1, factory_deck1, 4);
    }
    len = 4;
    if (nvs_get_blob(h, DOCK_NVS_KEY_ADDR_DECK2, out_deck2, &len) != ESP_OK || len != 4) {
        memcpy(out_deck2, factory_deck2, 4);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Enderecos carregados da NVS: deck1=%02X%02X%02X%02X deck2=%02X%02X%02X%02X",
             out_deck1[0], out_deck1[1], out_deck1[2], out_deck1[3],
             out_deck2[0], out_deck2[1], out_deck2[2], out_deck2[3]);
}
    
/* ── Timecode format: save/load em NVS ── */
static uint8_t load_tc_format_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(DOCK_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;  /* default: Serato CV02 */
    }
    uint8_t fmt = 0;
    nvs_get_u8(h, DOCK_NVS_KEY_TCFMT, &fmt);  /* se falhar, fica 0 */
    nvs_close(h);
    if (fmt >= TC_FORMAT_COUNT) fmt = 0;
    return fmt;
}

static void save_tc_format_to_nvs(uint8_t fmt) {
    nvs_handle_t h;
    if (nvs_open(DOCK_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS: nao foi possivel abrir para salvar tc_format");
        return;
    }
    nvs_set_u8(h, DOCK_NVS_KEY_TCFMT, fmt);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Task de console: troca formato de timecode via serial ── */
/* ── Timecode format cycling via short button press ──
 * Chamada de dock_pairing.c (pairing_poll_deck) quando o botao do dock
 * e' solto antes de 3s (toque curto). Cicla: Serato->Traktor->MixVibes->Pioneer.
 * Hold 3s continua sendo pareamento de canal (comportamento original).
 * LED RGB feedback: cada formato tem uma cor propria (800ms de flash). */
static const uint8_t tc_led_colors[TC_FORMAT_COUNT][3] = {
    { 0, 80, 0 },    /* Serato CV02  - verde   */
    { 0, 0, 80 },    /* Traktor MK1  - azul    */
    { 80, 0, 80 },   /* MixVibes V2  - roxo    */
    { 80, 60, 0 },   /* Pioneer RB   - amarelo */
};

void dvs_cycle_tc_format(void) {
    uint8_t fmt = (deck1.cv02.tc_format + 1) % TC_FORMAT_COUNT;
    deck1.cv02.tc_format = fmt;
    deck2.cv02.tc_format = fmt;
    save_tc_format_to_nvs(fmt);

    /* LED: flash na cor do formato por 800ms, depois volta ao idle */
    dock_led_set_rgb(tc_led_colors[fmt][0], tc_led_colors[fmt][1], tc_led_colors[fmt][2]);
    vTaskDelay(pdMS_TO_TICKS(800));
    dock_led_set_state(DOCK_LED_IDLE);

    ESP_LOGI(TAG, "TC format: [%d] %s (%.0fHz, %s, %s)",
             fmt, TC_FORMATS[fmt].name, TC_FORMATS[fmt].res_hz,
             TC_FORMATS[fmt].phase_270 ? "270deg" : "90deg",
             TC_FORMATS[fmt].swap_lr ? "swap" : "normal");
}

void app_main(void) {
    ESP_LOGI(TAG, "DVS Wireless RX (ESP32-S3 + 2x nRF24L01 + 2x PCM5102)");
    if (!cv02_table_selfcheck()) {
        ESP_LOGE(TAG, "FALHA no self-check da tabela CV02!");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "Self-check CV02: OK");


    gpio_install_isr_service(0);

    /* CORRECAO 29/jul: dock_pairing_init() e' quem chama nvs_flash_init()
       primeiro (idempotente, mas so' deve rodar uma vez de fato) - por
       isso e' chamado ANTES de qualquer leitura de NVS aqui, senao
       load_channels_from_nvs() falharia por flash ainda nao montado.
       Tambem configura as chaves fisicas (GP40/GP41) e as 2 UARTs do
       dock, e sobe a task que fica de olho no hold de 3s. */
    dock_led_init();
    dock_pairing_init();
    dock_pairing_attach_radios(&deck1.radio, &deck2.radio);
#ifdef DVS_NVMC_TEST_ENABLED
    extern void nvmc_test_run(void);
    nvmc_test_run();
    extern void nvmc_flash_test_run(void);
#endif
    dock_bridge_init();
    /* swd_test_run() removida 07/ago - conflitava com GP38/39
       agora usados pela UART propria do dock2 (ver dock_pairing.c) */

    uint8_t ch_deck1, ch_deck2;
    load_channels_from_nvs(&ch_deck1, &ch_deck2);

    /* Enderecos: [0]=prefix, [1..4]=base_address (ver nota de premissa em nrf24.c).
       CORRECAO 02/ago: base_address agora vem da NVS (sorteado no
       pareamento), nao mais fixo - prefix continua sendo o 1o byte do
       base_address, por convencao ja usada aqui desde o inicio. */
    uint8_t base_deck1[4], base_deck2[4];
    load_addresses_from_nvs(base_deck1, base_deck2);
    uint8_t addr_deck1[5] = { base_deck1[0], base_deck1[0], base_deck1[1], base_deck1[2], base_deck1[3] };
    uint8_t addr_deck2[5] = { base_deck2[0], base_deck2[0], base_deck2[1], base_deck2[2], base_deck2[3] };

    deck_setup(&deck1, "deck1", ch_deck1, addr_deck1,
               D1_SCK, D1_MOSI, D1_MISO, D1_CSN, D1_CE, D1_IRQ, SPI2_HOST);
    deck_setup(&deck2, "deck2", ch_deck2, addr_deck2,
               D2_SCK, D2_MOSI, D2_MISO, D2_CSN, D2_CE, D2_IRQ, SPI3_HOST);



    i2s_setup(&deck1, I2S_NUM_0, D1_I2S_BCK, D1_I2S_LRCK, D1_I2S_DOUT);
    i2s_setup(&deck2, I2S_NUM_1, D2_I2S_BCK, D2_I2S_LRCK, D2_I2S_DOUT);

    /* Carregar formato de timecode salvo na NVS */
    uint8_t saved_tc = load_tc_format_from_nvs();
    deck1.cv02.tc_format = saved_tc;
    deck2.cv02.tc_format = saved_tc;
    ESP_LOGI(TAG, "Timecode: [%d] %s", saved_tc, TC_FORMATS[saved_tc].name);

    xTaskCreatePinnedToCore(radio_task, "radio_deck1", 4096, &deck1, 10, NULL, 0);
    xTaskCreatePinnedToCore(radio_task, "radio_deck2", 4096, &deck2, 10, NULL, 0);

    xTaskCreatePinnedToCore(audio_task, "audio_deck1", 4096, &deck1, 12, NULL, 1);
    xTaskCreatePinnedToCore(audio_task, "audio_deck2", 4096, &deck2, 12, NULL, 1);

    ESP_LOGI(TAG, "Todas as tasks criadas. Sistema rodando.");
}
