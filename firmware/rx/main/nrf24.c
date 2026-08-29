/* nrf24.c - Driver minimo do nRF24L01, registrador cru (sem biblioteca RF24),
 * generalizado por instancia para permitir 2 radios independentes (deck1 e
 * deck2), cada um com seu proprio barramento SPI e pinos.
 *
 * ── ORDEM DE BYTES DO ENDERECO: VALIDADA NA PRATICA em 25/jul ──
 * A ordem de bytes do endereco no ar (ADDR_BYTES: [0]=prefix, [1..4]=
 * base_address) funcionou de primeira, sem precisar de ajuste - recepcao
 * de pacotes reais do TX (nRF52840/ESB) confirmada em multiplos testes
 * desde entao. Nao e mais premissa a validar.
 */
#include <string.h>
#include "nrf24.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h" // esp_rom_delay_us, usado na varredura RPD

static const char *TAG = "nrf24";

/* ── Registradores nRF24L01 ── */
#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_RF_CH       0x05
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_RX_ADDR_P0  0x0A
#define REG_RX_PW_P0    0x11
#define REG_FIFO_STATUS 0x17
#define REG_DYNPD       0x1C
#define REG_FEATURE     0x1D
#define REG_RPD         0x09  /* Received Power Detector - CCA/"listen before talk" */

#define CMD_R_REGISTER    0x00
#define CMD_W_REGISTER    0x20
#define CMD_R_RX_PAYLOAD  0x61
#define CMD_FLUSH_RX      0xE2
#define CMD_R_RX_PL_WID   0x60
#define CMD_NOP           0xFF

static inline void csn_low(nrf24_dev_t *dev)  { gpio_set_level(dev->pin_csn, 0); }
static inline void csn_high(nrf24_dev_t *dev) { gpio_set_level(dev->pin_csn, 1); }
static inline void ce_low(nrf24_dev_t *dev)   { gpio_set_level(dev->pin_ce, 0); }
static inline void ce_high(nrf24_dev_t *dev)  { gpio_set_level(dev->pin_ce, 1); }

static uint8_t spi_xfer_byte(nrf24_dev_t *dev, uint8_t tx) {
    uint8_t rx = 0;
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &tx,
        .rx_buffer = &rx,
    };
    esp_err_t err = spi_device_transmit(dev->spi, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] spi_xfer falhou: %d", dev->label, err);
    }
    return rx;
}

static uint8_t reg_read(nrf24_dev_t *dev, uint8_t reg) {
    csn_low(dev);
    spi_xfer_byte(dev, CMD_R_REGISTER | (reg & 0x1F));
    uint8_t val = spi_xfer_byte(dev, CMD_NOP);
    csn_high(dev);
    return val;
}

static void reg_write(nrf24_dev_t *dev, uint8_t reg, uint8_t val) {
    csn_low(dev);
    spi_xfer_byte(dev, CMD_W_REGISTER | (reg & 0x1F));
    spi_xfer_byte(dev, val);
    csn_high(dev);
}

static void reg_write_multi(nrf24_dev_t *dev, uint8_t reg, const uint8_t *buf, size_t len) {
    csn_low(dev);
    spi_xfer_byte(dev, CMD_W_REGISTER | (reg & 0x1F));
    for (size_t i = 0; i < len; i++) {
        spi_xfer_byte(dev, buf[i]);
    }
    csn_high(dev);
}

static void flush_rx(nrf24_dev_t *dev) {
    csn_low(dev);
    spi_xfer_byte(dev, CMD_FLUSH_RX);
    csn_high(dev);
}

esp_err_t nrf24_init(nrf24_dev_t *dev) {
    gpio_set_direction(dev->pin_csn, GPIO_MODE_OUTPUT);
    gpio_set_direction(dev->pin_ce, GPIO_MODE_OUTPUT);
    gpio_set_direction(dev->pin_irq, GPIO_MODE_INPUT);
    gpio_set_pull_mode(dev->pin_irq, GPIO_PULLUP_ONLY); /* IRQ do nRF24 e ativo-baixo */
    csn_high(dev);
    ce_low(dev); /* CORRECAO: fica em standby (CE baixo) - nrf24_enter_rx() e quem
                    entra em modo RX de verdade, DEPOIS que a interrupcao de GPIO
                    ja estiver armada no chamador (ver nota abaixo). */

    spi_bus_config_t buscfg = {
        .miso_io_num = dev->pin_miso,
        .mosi_io_num = dev->pin_mosi,
        .sclk_io_num = dev->pin_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t err = spi_bus_initialize(dev->spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "[%s] spi_bus_initialize falhou: %d", dev->label, err);
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000, /* 4MHz - nRF24L01 aceita ate 10MHz */
        .mode = 0,
        .spics_io_num = -1, /* CSN controlado manualmente */
        .queue_size = 1,
    };
    err = spi_bus_add_device(dev->spi_host, &devcfg, &dev->spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] spi_bus_add_device falhou: %d", dev->label, err);
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(5)); /* estabilizacao apos power-on */

    reg_write(dev, REG_EN_AA, 0x01);
    reg_write(dev, REG_EN_RXADDR, 0x01);
    reg_write(dev, REG_SETUP_AW, 0x03);       /* endereco de 5 bytes */
    reg_write(dev, REG_RF_CH, dev->rf_channel);
    reg_write(dev, REG_RF_SETUP, 0x0E);       /* 2Mbps, 0dBm */
    reg_write_multi(dev, REG_RX_ADDR_P0, dev->addr_bytes, 5);
    reg_write(dev, REG_FEATURE, 0x04);        /* EN_DPL */
    reg_write(dev, REG_DYNPD, 0x01);          /* DPL_P0 */
    reg_write(dev, REG_CONFIG, 0x0F);         /* EN_CRC, CRCO=1 (16bit), PWR_UP, PRIM_RX */
    vTaskDelay(pdMS_TO_TICKS(2)); /* Tpd2stby */

    flush_rx(dev);
    reg_write(dev, REG_STATUS, 0x70); /* limpa RX_DR/TX_DS/MAX_RT - garante flag zerado antes de armar IRQ */

    ESP_LOGI(TAG, "[%s] init OK (standby): canal=%d CONFIG=0x%02x FEATURE=0x%02x",
              dev->label, dev->rf_channel, reg_read(dev, REG_CONFIG), reg_read(dev, REG_FEATURE));
    return ESP_OK;
}

/* CORRECAO: chamar SO DEPOIS que a interrupcao de GPIO ja estiver armada no
   chamador (gpio_isr_handler_add). Antes, ce_high() acontecia dentro de
   nrf24_init() ANTES da interrupcao existir - se um pacote chegasse nesse
   intervalo, o IRQ (ativo-baixo, so' sobe de novo quando o flag e limpo)
   ja caia pra 0 antes da interrupcao de borda de descida estar armada,
   e como e trigger de BORDA (nao nivel), a interrupcao nunca mais
   disparava - impasse circular. Confirmado na pratica: multimetro
   mostrou IRQ do deck1 preso em 0V mesmo com CONFIG lido corretamente. */
void nrf24_enter_rx(nrf24_dev_t *dev) {
    ce_high(dev);
}

/* CORRECAO 02/ago: varredura de energia (CCA - Clear Channel Assessment)
 * via registrador RPD do proprio nRF24L01+, pesquisado e documentado no
 * datasheet (secao 6.4). Objetivo: antes de escolher um canal novo no
 * pareamento, evitar canais com energia de RF ja presente (Wi-Fi,
 * Bluetooth, outros nRF24 por perto, etc), reduzindo risco de
 * interferencia/colisao.
 *
 * ATENCAO - integracao pendente (nao chamada em lugar nenhum ainda):
 * este radio pode estar ATIVO (recebendo audio em tempo real) quando o
 * pareamento e disparado. Chamar esta funcao no meio da operacao normal
 * troca o canal (REG_RF_CH) e o modo (CE) temporariamente - precisa
 * coordenar com a task de radio que ja roda em paralelo (pausar o
 * recebimento antes de escanear, ou usar isso so' num momento em que o
 * deck esta comprovadamente ocioso) antes de integrar ao pareamento de
 * verdade. Ver nota na sessao de 02/ago no cofre.
 *
 * Retorna true se detectou energia de RF (>= -64dBm) no canal, indicando
 * canal ocupado - false = canal livre (ou near-livre, dentro da
 * granularidade grosseira do RPD).
 */
bool nrf24_channel_busy(nrf24_dev_t *dev, uint8_t channel) {
    ce_low(dev); // garante que nao esta em RX antes de reconfigurar o canal
    reg_write(dev, REG_RF_CH, channel);
    ce_high(dev); // entra em modo RX no canal candidato

    // Datasheet exige >=40us de sinal presente antes do RPD subir: 200us
    // da boa margem de seguranca sem tornar a varredura lenta (com 7-8
    // canais por deck, o total fica bem abaixo de 2ms).
    esp_rom_delay_us(200);

    uint8_t rpd = reg_read(dev, REG_RPD) & 0x01;
    ce_low(dev); // volta pro standby antes de testar o proximo canal

    return rpd != 0;
}

bool nrf24_data_ready(nrf24_dev_t *dev) {
    uint8_t status = reg_read(dev, REG_STATUS);
    if (status & 0x40) { /* RX_DR */
        reg_write(dev, REG_STATUS, 0x40); /* write-1-to-clear */
        return true;
    }
    return false;
}

bool nrf24_read_packet(nrf24_dev_t *dev, dvs_packet_t *out) {
    csn_low(dev);
    spi_xfer_byte(dev, CMD_R_RX_PL_WID);
    uint8_t width = spi_xfer_byte(dev, CMD_NOP);
    csn_high(dev);

    if (width > 32) {
        flush_rx(dev); /* payload corrompido - nunca deixar ler tamanho invalido */
        return false;
    }

    uint8_t buf[32];
    csn_low(dev);
    spi_xfer_byte(dev, CMD_R_RX_PAYLOAD);
    for (uint8_t i = 0; i < width; i++) {
        buf[i] = spi_xfer_byte(dev, CMD_NOP);
    }
    csn_high(dev);

    if (width != sizeof(dvs_packet_t)) {
        ESP_LOGW(TAG, "[%s] payload tamanho inesperado: %d (esperado %d)",
                  dev->label, width, (int)sizeof(dvs_packet_t));
        return false;
    }

    memcpy(out, buf, sizeof(dvs_packet_t));
    return true;
}

/* CORRECAO 26/jul: drena o FIFO RX INTEIRO (ate RX_EMPTY), aplicando so' o
   pacote MAIS RECENTE - corrige um bug real de acumulo de atraso,
   VALIDADO NA PRATICA em 26/jul: o log periodico da propria radio_task
   (ESP_LOGI bloqueante no UART a 115200, ~4ms por linha) fabricava a
   condicao de 2+ pacotes no FIFO duas vezes por segundo - o patch
   estava evitando um engasgo real, so' pequeno demais pra perceber no
   ouvido isoladamente.
   O IRQ e trigger de BORDA de descida (GPIO_INTR_NEGEDGE). O pino fica
   em nivel baixo enquanto ha dado nao lido no FIFO (3 pacotes de
   profundidade). Se 2+ pacotes chegam antes da task ser servida, o
   codigo antigo (data_ready + read_packet, um pacote por evento de IRQ)
   limpava RX_DR e lia so' UM - o(s) pacote(s) restante(s) ficava(m)
   parado(s) no FIFO SEM disparar nova interrupcao. Resultado: fila
   permanente de 1-2 pacotes, atraso sistematico de 2.4-4.8ms.
   CORRECAO ADICIONAL (fecha janela residual apontada pelo Fable5): entre
   o RX_EMPTY do loop de drain e o reg_write(STATUS, 0x40) que limpa o
   flag, um pacote novo pode chegar - ficaria no FIFO com o flag ja
   limpo, sem nova borda de IRQ ate o PROXIMO pacote (autocorrige em
   2.4ms, nao e mais fila permanente, mas ainda vale fechar). Estruturado
   como do/while reconferindo o FIFO apos o clear, com teto defensivo de
   iteracoes (FIFO tem so' 3 posicoes - 6 leituras cobre com folga,
   protege contra um chip mentindo no FIFO_STATUS, cenario ja visto
   nesses modulos baratos). Analise e diagnostico do Fable5, verificados
   por nos contra a arquitetura real do nosso codigo. */
#define FIFO_DRAIN_MAX_ROUNDS 6

bool nrf24_read_latest_packet(nrf24_dev_t *dev, dvs_packet_t *out, uint32_t *packets_drained) {
    uint8_t status = reg_read(dev, REG_STATUS);
    if (!(status & 0x40)) { /* RX_DR nao setado - nada pendente */
        if (packets_drained) *packets_drained = 0;
        return false;
    }

    bool got_valid = false;
    uint8_t buf[32];
    int rounds = 0;
    uint32_t drained = 0;

    do {
        while (1) {
            uint8_t fifo_status = reg_read(dev, REG_FIFO_STATUS);
            if (fifo_status & 0x01) { /* RX_EMPTY */
                break;
            }

            csn_low(dev);
            spi_xfer_byte(dev, CMD_R_RX_PL_WID);
            uint8_t width = spi_xfer_byte(dev, CMD_NOP);
            csn_high(dev);

            if (width > 32) {
                flush_rx(dev); /* payload corrompido - descarta tudo e sai */
                break;
            }

            csn_low(dev);
            spi_xfer_byte(dev, CMD_R_RX_PAYLOAD);
            for (uint8_t i = 0; i < width; i++) {
                buf[i] = spi_xfer_byte(dev, CMD_NOP);
            }
            csn_high(dev);

            if (width == sizeof(dvs_packet_t)) {
                memcpy(out, buf, sizeof(dvs_packet_t));
                got_valid = true;
                drained++;
            } else {
                ESP_LOGW(TAG, "[%s] payload tamanho inesperado no drain: %d (esperado %d)",
                          dev->label, width, (int)sizeof(dvs_packet_t));
            }
        }

        /* Limpa RX_DR SO' DEPOIS de esvaziar o FIFO - ordem recomendada
           pelo datasheet. Depois reconfere: se algo chegou bem no
           intervalo entre o RX_EMPTY acima e este clear, o proximo
           "while" externo pega antes de sair da funcao. */
        reg_write(dev, REG_STATUS, 0x40);
        rounds++;
    } while (!(reg_read(dev, REG_FIFO_STATUS) & 0x01) && rounds < FIFO_DRAIN_MAX_ROUNDS);

    if (packets_drained) *packets_drained = drained;
    return got_valid;
}
