#include "dock_led.h"

#include "freertos/FreeRTOS.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "dock_led";

// WS2812 espera 800kHz de bit rate. Resolucao de 10MHz do RMT da'
// granularidade de 0.1us por tick - suficiente pra respeitar a
// tolerancia de timing do protocolo (T0H~0.35us, T0L~0.8us,
// T1H~0.7us, T1L~0.6us, valores tipicos - ligeira variacao entre
// datasheets de clones e' tolerada pelo chip na pratica).
#define WS2812_RESOLUTION_HZ 10000000

static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;

// Simbolos de bit "0" e "1" do protocolo WS2812 (em ticks de 0.1us)
static const rmt_symbol_word_t WS2812_BIT0 = {
    .duration0 = 3,  .level0 = 1,   // ~0.3us alto
    .duration1 = 9,  .level1 = 0,   // ~0.9us baixo
};
static const rmt_symbol_word_t WS2812_BIT1 = {
    .duration0 = 9,  .level0 = 1,   // ~0.9us alto
    .duration1 = 3,  .level1 = 0,   // ~0.3us baixo
};

void dock_led_init(void) {
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = DOCK_LED_PIN,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .trans_queue_depth = 4,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &s_led_chan));

    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = WS2812_BIT0,
        .bit1 = WS2812_BIT1,
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_encoder_config, &s_led_encoder));

    ESP_ERROR_CHECK(rmt_enable(s_led_chan));

    ESP_LOGI(TAG, "LED RGB inicializado (GPIO%d, via RMT)", DOCK_LED_PIN);

    // Comeca apagado
    dock_led_set_rgb(0, 0, 0);
}

void dock_led_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (s_led_chan == NULL || s_led_encoder == NULL) {
        ESP_LOGW(TAG, "dock_led_set_rgb chamado antes de dock_led_init()");
        return;
    }

    // WS2812 espera a ordem GRB (nao RGB) no fio
    uint8_t grb[3] = { g, r, b };

    rmt_transmit_config_t tx_config = {
        .loop_count = 0, // sem repeticao - um pulso so'
    };

    esp_err_t err = rmt_transmit(s_led_chan, s_led_encoder, grb, sizeof(grb), &tx_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_transmit falhou: %s", esp_err_to_name(err));
        return;
    }

    // Espera a transmissao terminar antes de retornar - garante que a
    // proxima chamada (ou o reset de +50us de silencio que o WS2812
    // exige entre atualizacoes) nao atropele esta
    rmt_tx_wait_all_done(s_led_chan, pdMS_TO_TICKS(100));
}

void dock_led_set_state(dock_led_state_t state) {
    switch (state) {
        case DOCK_LED_OFF:
            dock_led_set_rgb(0, 0, 0);
            break;
        case DOCK_LED_IDLE:
            dock_led_set_rgb(0, 0, 20);      // azul fraco
            break;
        case DOCK_LED_BRIDGE_SWD:
            dock_led_set_rgb(0, 60, 60);     // ciano
            break;
        case DOCK_LED_PAIRING:
            dock_led_set_rgb(60, 0, 60);     // roxo
            break;
        case DOCK_LED_PAIR_OK:
            dock_led_set_rgb(0, 80, 0);      // verde
            break;
        case DOCK_LED_PAIR_FAIL:
            dock_led_set_rgb(80, 0, 0);      // vermelho
            break;
        default:
            ESP_LOGW(TAG, "dock_led_set_state: estado desconhecido (%d)", state);
            break;
    }
}
