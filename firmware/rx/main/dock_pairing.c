/* dock_pairing.c - le as chaves fisicas de troca de canal (GP40/GP41) e,
 * quando pressionadas por 3s continuos, sorteia um canal novo dentro da
 * faixa daquele deck, grava na propria NVS, envia o comando de
 * pareamento pela UART do dock correspondente, e reinicia o RX pra
 * aplicar o canal novo no proprio radio.
 *
 * Formato do comando confirmado e implementado no TX em 29/jul:
 *   "PAIR:<canal decimal>:<addr hex 4 bytes>\n"
 *   Resposta do TX: "PAIRACK:OK\r\n" ou "PAIRACK:FAIL\r\n", enviada
 *   ANTES do TX reiniciar (confirmado - ver dvs-tx-pareamento-nvs-
 *   implementado-29jul2026.md no cofre).
 *
 * CORRECAO 01/ago: dock2 deixou de ter UART/conector proprios. Agora
 * so existe UMA estacao fisica de pareamento+gravacao (fiada no dock1)
 * - os pinos GP38/GP39, antes usados pela UART do dock2, foram
 * liberados para uso do modulo dock_swd (gravacao via SWD). Pra
 * parear OU atualizar o TX2, ele precisa ser fisicamente encaixado no
 * dock1. As duas chaves fisicas (GP40 e GP41) continuam existindo e
 * decidindo qual IDENTIDADE aplicar (deck1 ou deck2 - endereco e faixa
 * de canal), nao mais qual UART usar - as duas agora mandam o comando
 * PAIR pela mesma UART fisica (DOCK1_UART_NUM).
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "dock_pairing.h"
#include "freertos/queue.h"
#include "dock_led.h"

static const char *TAG = "dock_pairing";

/* ── Pinos das chaves fisicas de troca de canal ──
   As duas continuam existindo - decidem qual IDENTIDADE (deck1/deck2)
   aplicar ao TX que estiver fisicamente no dock1 no momento, nao mais
   qual UART usar (so existe uma UART agora). */
#define SWITCH_DECK1_PIN   40
#define SWITCH_DECK2_PIN   41

/* ── UART unica do dock1 (unico conector fisico de pareamento/gravacao) ──
   GP21=RX, GP47=TX (ja validado em producao desde 29/jul).
   GP38/GP39 (antiga UART do dock2) foram liberados em 01/ago para uso
   do modulo dock_swd (SWDIO/SWCLK), fiados fisicamente no dock1. */
#define DOCK1_UART_NUM     UART_NUM_1
#define DOCK1_UART_RX_PIN  21
#define DOCK1_UART_TX_PIN  47
/* CORRECAO 07/ago: dock2 volta a ter UART propria (GP38/GP39) - a
 * consolidacao de 01/ago (que juntou tudo em DOCK1_UART_NUM) foi para
 * liberar esses pinos para SWD. Como SWD nao e' mais o caminho
 * principal (decisao de 03/ago) e a fiacao fisica do dock2 ja foi
 * refeita para UART de verdade, voltamos ao modelo original: 2
 * pratos, 2 docks fisicos independentes. */
#define DOCK2_UART_NUM     UART_NUM_2
#define DOCK2_UART_RX_PIN  38
#define DOCK2_UART_TX_PIN  39

#define UART_BAUD_RATE     115200
#define UART_BUF_SIZE      256

#define HOLD_TIME_MS       3000   /* tempo de pressao continua exigido */
#define POLL_INTERVAL_MS   20     /* intervalo de leitura das chaves */
#define ACK_TIMEOUT_MS     2000   /* tempo maximo esperando PAIRACK */

/* ── Faixas de canal por deck (guarda 91-92 entre as duas) ──
   CORRECAO 29/jul: reduzido de 84-102/106-125 para 84-90/93-100 - a
   biblioteca ESB da Nordic (NCS/Zephyr) so' suporta oficialmente ate
   canal 100 (confirmado na pratica: canal 125 causava esb_init()
   falhar no TX). Ver nota completa em dock_pairing.h. */
#define DECK1_CH_MIN       2
#define DECK1_CH_MAX       45
#define DECK2_CH_MIN       55
#define DECK2_CH_MAX       100

#define NVS_NAMESPACE      "dvs_dock"
#define NVS_KEY_CH_DECK1   "ch_deck1"
#define NVS_KEY_CH_DECK2   "ch_deck2"

/* CORRECAO 02/ago: enderecos deixaram de ser fixos por identidade -
   agora sao sorteados junto com o canal a cada pareamento, pra evitar
   colisao quando varias placas fisicas com o mesmo firmware (mesmo
   endereco de fabrica) coexistem na bancada. Persistidos como blob de
   4 bytes na NVS, mesma namespace dos canais. */
#define NVS_KEY_ADDR_DECK1 "addr_deck1"
#define NVS_KEY_ADDR_DECK2 "addr_deck2"

typedef struct {
    const char   *label;
    gpio_num_t    switch_pin;
    uart_port_t   uart_num;
    uint8_t       ch_min;
    uint8_t       ch_max;
    const char   *nvs_key;
    const char   *addr_nvs_key;
    uint8_t       addr[4];   /* endereco ATUAL do deck - sorteado a cada
                                 pareamento (nao e' mais fixo de fabrica
                                 depois do primeiro pareamento) */
    /* estado interno do debounce/hold */
    bool          was_pressed;
    int64_t       press_start_us;
    bool          fired_this_press;
    nrf24_dev_t   *radio;   /* NULL ate dock_pairing_attach_radios() ser
                                chamado - varredura CCA so' roda se isso
                                estiver setado, senao cai no sorteio
                                puro de sempre (comportamento antigo
                                preservado como fallback seguro) */
} deck_pairing_ctx_t;

/* Short-press callback: cycles timecode format (Serato->Traktor->MixVibes->Pioneer).
 * Implementada em main.c onde deck1/deck2 sao acessiveis. */
extern void dvs_cycle_tc_format(void);

static deck_pairing_ctx_t s_deck1 = {
    .label = "deck1",
    .switch_pin = SWITCH_DECK1_PIN,
    .uart_num = DOCK1_UART_NUM,
    .ch_min = DECK1_CH_MIN,
    .ch_max = DECK1_CH_MAX,
    .nvs_key = NVS_KEY_CH_DECK1,
    .addr_nvs_key = NVS_KEY_ADDR_DECK1,
    .addr = { 0xC3, 0x5A, 0x71, 0x98 }, /* endereco de fabrica - so' vale ate o 1o pareamento */
};

static deck_pairing_ctx_t s_deck2 = {
    .label = "deck2",
    .switch_pin = SWITCH_DECK2_PIN,
    .uart_num = DOCK2_UART_NUM,  /* CORRECAO 07/ago: UART propria de volta */
    .ch_min = DECK2_CH_MIN,
    .ch_max = DECK2_CH_MAX,
    .nvs_key = NVS_KEY_CH_DECK2,
    .addr_nvs_key = NVS_KEY_ADDR_DECK2,
    .addr = { 0x8E, 0x2D, 0x64, 0xB1 }, /* endereco de fabrica - so' vale ate o 1o pareamento */
};

static void dock_uart_setup(uart_port_t uart_num, int rx_pin, int tx_pin) {
    uart_config_t cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(uart_num, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void switch_setup(gpio_num_t pin) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static uint8_t roll_random_channel(uint8_t ch_min, uint8_t ch_max) {
    uint32_t span = (uint32_t)(ch_max - ch_min + 1);
    return (uint8_t)(ch_min + (esp_random() % span));
}

/* CCA - escolhe um canal livre dentro da faixa do deck, preferindo
 * evitar energia de RF ja presente (Wi-Fi, BLE, outros nRF24 por
 * perto). Seguro porque so' e' chamado durante pareamento (TX docado,
 * ESB suprimido naquele deck - ver nota no header). Comeca de um
 * ponto aleatorio dentro da faixa (evita viés de sempre testar
 * ch_min primeiro) e varre ate achar livre ou esgotar a faixa; se
 * TODOS os canais testados vierem ocupados (ambiente muito
 * congestionado) ou o radio ainda nao estiver conectado
 * (ctx->radio == NULL), cai no sorteio puro de sempre como
 * fallback seguro - nunca impede o pareamento de acontecer. */
static uint8_t pick_channel_cca(deck_pairing_ctx_t *ctx) {
    if (ctx->radio == NULL) {
        return roll_random_channel(ctx->ch_min, ctx->ch_max);
    }

    uint32_t span = (uint32_t)(ctx->ch_max - ctx->ch_min + 1);
    uint32_t start_offset = esp_random() % span;

    for (uint32_t i = 0; i < span; i++) {
        uint8_t candidate = (uint8_t)(ctx->ch_min + ((start_offset + i) % span));
        if (!nrf24_channel_busy(ctx->radio, candidate)) {
            ESP_LOGI(TAG, "[%s] CCA: canal %u livre (testados %lu de %lu)",
                     ctx->label, candidate, (unsigned long)(i + 1), (unsigned long)span);
            return candidate;
        }
    }

    ESP_LOGW(TAG, "[%s] CCA: todos os %lu canais da faixa pareciam ocupados - "
             "sorteando as cegas mesmo assim", ctx->label, (unsigned long)span);
    return roll_random_channel(ctx->ch_min, ctx->ch_max);
}

void dock_pairing_attach_radios(nrf24_dev_t *radio_deck1, nrf24_dev_t *radio_deck2) {
    s_deck1.radio = radio_deck1;
    s_deck2.radio = radio_deck2;
    ESP_LOGI(TAG, "radios conectados aos contextos de pareamento - CCA ativa");
}

// CORRECAO 02/ago: sorteia um endereco novo de 4 bytes, pra evitar
// colisao entre placas fisicas diferentes que compartilham o mesmo
// firmware/endereco de fabrica.
static void roll_random_address(uint8_t addr_out[4]) {
    uint32_t r = esp_random();
    addr_out[0] = (uint8_t)(r >> 24);
    addr_out[1] = (uint8_t)(r >> 16);
    addr_out[2] = (uint8_t)(r >> 8);
    addr_out[3] = (uint8_t)(r);
}

static bool save_channel_to_own_nvs(const char *key, uint8_t channel) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open falhou: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u8(handle, key, channel);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8/commit falhou pra '%s': %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool save_addr_to_own_nvs(const char *key, const uint8_t addr[4]) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open falhou (addr): %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, key, addr, 4);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob/commit falhou pra '%s': %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

// Carrega o endereco previamente salvo (se houver) por cima do valor
// de fabrica ja presente em ctx->addr. Chamado no boot, pra que um
// endereco pareado anteriormente sobreviva a um reset do RX (o TX ja
// gravou aquele endereco na propria NVS dele - o RX precisa lembrar
// o mesmo endereco pra continuar escutando certo).
static void load_addr_from_own_nvs(deck_pairing_ctx_t *ctx) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return; // NVS ainda nao existe (nunca pareado) - mantem o de fabrica
    }
    uint8_t addr[4];
    size_t len = sizeof(addr);
    if (nvs_get_blob(handle, ctx->addr_nvs_key, addr, &len) == ESP_OK && len == sizeof(addr)) {
        memcpy(ctx->addr, addr, sizeof(addr));
        ESP_LOGI(TAG, "[%s] endereco carregado da NVS: %02X%02X%02X%02X",
                 ctx->label, ctx->addr[0], ctx->addr[1], ctx->addr[2], ctx->addr[3]);
    }
    nvs_close(handle);
}

#define PAIR_MAX_ATTEMPTS  3

static bool send_pair_command_once(deck_pairing_ctx_t *ctx, uint8_t channel, int attempt) {
    char cmd[48];
    int len = snprintf(cmd, sizeof(cmd), "PAIR:%u:%02X%02X%02X%02X\n",
                        channel, ctx->addr[0], ctx->addr[1], ctx->addr[2], ctx->addr[3]);

    ESP_LOGI(TAG, "[%s] enviando comando de pareamento (tentativa %d/%d): %.*s",
             ctx->label, attempt, PAIR_MAX_ATTEMPTS, len - 1, cmd);

    // Descarta qualquer byte que tenha ficado no buffer de entrada de uma
    // tentativa ANTERIOR (ex: resposta do TX que chegou atrasada, depois
    // do timeout ja ter desistido). Sem isso, a proxima tentativa podia
    // ler esse lixo velho por engano.
    uart_flush_input(ctx->uart_num);

    uart_write_bytes(ctx->uart_num, cmd, len);

    uint8_t rx_buf[64] = {0};
    int total = 0;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)ACK_TIMEOUT_MS * 1000;

    while (esp_timer_get_time() < deadline_us && total < (int)sizeof(rx_buf) - 1) {
        int n = uart_read_bytes(ctx->uart_num, rx_buf + total, sizeof(rx_buf) - 1 - total,
                                 pdMS_TO_TICKS(100));
        if (n > 0) {
            total += n;
            if (memchr(rx_buf, '\n', total) != NULL) {
                break;
            }
        }
    }

    if (total == 0) {
        ESP_LOGW(TAG, "[%s] SEM RESPOSTA do TX dentro de %dms (tentativa %d/%d)",
                 ctx->label, ACK_TIMEOUT_MS, attempt, PAIR_MAX_ATTEMPTS);
        return false;
    }

    rx_buf[total] = '\0';
    if (strstr((char *)rx_buf, "PAIRACK:OK") != NULL) {
        ESP_LOGI(TAG, "[%s] pareamento CONFIRMADO pelo TX (canal %u)", ctx->label, channel);
        return true;
    } else if (strstr((char *)rx_buf, "PAIRACK:FAIL") != NULL) {
        ESP_LOGE(TAG, "[%s] TX respondeu FALHA ao gravar NVS", ctx->label);
        return false;
    }

    ESP_LOGW(TAG, "[%s] resposta inesperada do TX: '%s'", ctx->label, (char *)rx_buf);
    return false;
}

// Envolve send_pair_command_once com retry automatico. Objetivo:
// pressionar a chave deve ser resiliente a falhas transitorias (contato
// momentaneo ruim, TX ainda terminando o debounce de deteccao de dock,
// etc.) - tenta ate PAIR_MAX_ATTEMPTS vezes antes de desistir de vez.
static bool send_pair_command(deck_pairing_ctx_t *ctx, uint8_t channel) {
    for (int attempt = 1; attempt <= PAIR_MAX_ATTEMPTS; attempt++) {
        if (send_pair_command_once(ctx, channel, attempt)) {
            return true;
        }
        if (attempt < PAIR_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(300)); // pequena pausa antes de tentar de novo
        }
    }
    ESP_LOGE(TAG, "[%s] pareamento falhou apos %d tentativas", ctx->label, PAIR_MAX_ATTEMPTS);
    return false;
}

static void do_channel_hop_and_pair(deck_pairing_ctx_t *ctx) {
    dock_led_set_state(DOCK_LED_PAIRING);
    uint8_t new_channel = pick_channel_cca(ctx);
    ESP_LOGI(TAG, "[%s] canal sorteado: %u (faixa %u-%u)",
             ctx->label, new_channel, ctx->ch_min, ctx->ch_max);

    // CORRECAO 02/ago: sorteia endereco novo tambem, pra evitar colisao
    // com outras placas fisicas que compartilhem o mesmo firmware
    // (mesmo endereco de fabrica). Guarda o antigo pra reverter se o
    // pareamento falhar - so' confirma (grava na NVS) em caso de sucesso.
    uint8_t old_addr[4];
    memcpy(old_addr, ctx->addr, sizeof(old_addr));
    roll_random_address(ctx->addr);
    ESP_LOGI(TAG, "[%s] endereco sorteado: %02X%02X%02X%02X",
             ctx->label, ctx->addr[0], ctx->addr[1], ctx->addr[2], ctx->addr[3]);

    if (!save_channel_to_own_nvs(ctx->nvs_key, new_channel)) {
        ESP_LOGE(TAG, "[%s] abortando - falha ao gravar canal na NVS do proprio RX", ctx->label);
        memcpy(ctx->addr, old_addr, sizeof(old_addr)); // reverte
        dock_led_set_state(DOCK_LED_PAIR_FAIL);
        return;
    }

    bool ok = send_pair_command(ctx, new_channel);
    if (!ok) {
        ESP_LOGE(TAG, "[%s] TX nao confirmou - RX NAO vai reiniciar (canal do TX pode estar "
                       "dessincronizado do RX agora - repetir o pareamento antes de usar)", ctx->label);
        memcpy(ctx->addr, old_addr, sizeof(old_addr)); // reverte - TX nao aplicou o novo
        dock_led_set_state(DOCK_LED_PAIR_FAIL);
        return;
    }

    if (!save_addr_to_own_nvs(ctx->addr_nvs_key, ctx->addr)) {
        // TX ja confirmou e aplicou o endereco novo - se a gravacao local
        // falhar aqui, o RX ficaria com o endereco antigo na proxima vez
        // que reiniciar, dessincronizado do TX. Log de alerta, mas ainda
        // assim reinicia com o endereco novo em RAM (vale pra sessao atual).
        ESP_LOGW(TAG, "[%s] TX confirmou mas falha ao salvar endereco na NVS local - "
                       "risco de dessincronizar apos reboot", ctx->label);
    }

    dock_led_set_state(DOCK_LED_PAIR_OK);
    ESP_LOGI(TAG, "[%s] pareamento completo - reiniciando RX em 500ms para aplicar canal novo...",
             ctx->label);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void pairing_poll_deck(deck_pairing_ctx_t *ctx) {
    bool pressed = (gpio_get_level(ctx->switch_pin) == 0);

    if (pressed && !ctx->was_pressed) {
        ctx->press_start_us = esp_timer_get_time();
        ctx->fired_this_press = false;
    } else if (pressed && ctx->was_pressed && !ctx->fired_this_press) {
        int64_t held_ms = (esp_timer_get_time() - ctx->press_start_us) / 1000;
        if (held_ms >= HOLD_TIME_MS) {
            ctx->fired_this_press = true;
            do_channel_hop_and_pair(ctx);
        }
    } else if (!pressed) {
        /* Short press: soltou antes dos 3s sem ter disparado o hop/pair.
         * Cicla o formato de timecode (Serato->Traktor->MixVibes->Pioneer). */
        if (ctx->was_pressed && !ctx->fired_this_press) {
            dvs_cycle_tc_format();
        }
        ctx->fired_this_press = false;
    }

    ctx->was_pressed = pressed;
}

/* --- Infraestrutura de fila para SENDDOCK (corrige corrida de UART) --- */
#define SENDDOCK_CMD_MAXLEN  64
#define SENDDOCK_RESP_MAXLEN 128

typedef struct {
    char cmd[SENDDOCK_CMD_MAXLEN];
    QueueHandle_t response_queue; /* fila de 1 item, criada pelo chamador */
} senddock_request_t;

typedef struct {
    bool ok;
    char text[SENDDOCK_RESP_MAXLEN];
} senddock_response_t;

static QueueHandle_t s_senddock_request_queue = NULL;

bool dock_pairing_send_command(const char *cmd_line, char *resp_out,
                                size_t resp_out_len, uint32_t timeout_ms)
{
    if (s_senddock_request_queue == NULL) {
        return false; /* dock_pairing_init() ainda nao rodou */
    }

    senddock_request_t req = {0};
    snprintf(req.cmd, sizeof(req.cmd), "%s", cmd_line);
    req.response_queue = xQueueCreate(1, sizeof(senddock_response_t));
    if (req.response_queue == NULL) {
        return false;
    }

    if (xQueueSend(s_senddock_request_queue, &req, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vQueueDelete(req.response_queue);
        return false; /* fila de pedidos cheia - pairing_task travada? */
    }

    senddock_response_t resp;
    bool got = (xQueueReceive(req.response_queue, &resp, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
    vQueueDelete(req.response_queue);

    if (got && resp_out != NULL) {
        snprintf(resp_out, resp_out_len, "%s", resp.text);
    }
    return got && resp.ok;
}

/* Processa UM pedido pendente da fila (se houver), usando a MESMA UART
 * que pairing_poll_deck ja usa - chamado de dentro de pairing_task,
 * nunca de outra task, entao nao ha corrida possivel. */
static void process_senddock_queue(void)
{
    senddock_request_t req;
    if (xQueueReceive(s_senddock_request_queue, &req, 0) != pdTRUE) {
        return; /* nada pendente */
    }

    uart_flush_input(DOCK1_UART_NUM);
    uart_write_bytes(DOCK1_UART_NUM, req.cmd, strlen(req.cmd));

    uint8_t rx_buf[SENDDOCK_RESP_MAXLEN] = {0};
    int total = 0;
    int64_t deadline_us = esp_timer_get_time() + 3000 * 1000; /* 3s */
    while (esp_timer_get_time() < deadline_us && total < (int)sizeof(rx_buf) - 1) {
        int n = uart_read_bytes(DOCK1_UART_NUM, rx_buf + total,
                                 sizeof(rx_buf) - 1 - total, pdMS_TO_TICKS(100));
        if (n > 0) {
            total += n;
            if (memchr(rx_buf, '\n', total) != NULL) break;
        }
    }

    senddock_response_t resp = {0};
    resp.ok = (total > 0);
    snprintf(resp.text, sizeof(resp.text), "%s",
             total > 0 ? (char *)rx_buf : "SEM_RESPOSTA");

    xQueueSend(req.response_queue, &resp, 0);
}

static void pairing_task(void *arg) {
    ESP_LOGI(TAG, "task de pareamento+troca de canal iniciada (hold=%dms, poll=%dms)",
             HOLD_TIME_MS, POLL_INTERVAL_MS);
    while (1) {
        pairing_poll_deck(&s_deck1);
        pairing_poll_deck(&s_deck2);
        process_senddock_queue();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void dock_pairing_init(void) {
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // CORRECAO 02/ago: restaura o endereco pareado anteriormente (se
    // houver) por cima do valor de fabrica - senao, apos um reboot do
    // RX, ele voltaria a esperar o endereco de fabrica, dessincronizado
    // do endereco que o TX de fato tem gravado desde o ultimo pareamento.
    load_addr_from_own_nvs(&s_deck1);
    load_addr_from_own_nvs(&s_deck2);

    switch_setup(s_deck1.switch_pin);
    switch_setup(s_deck2.switch_pin);

    /* HISTORICO: entre 01/ago e 07/ago, dock2 nao tinha UART propria
       (consolidado numa unica UART para liberar GP38/39 para SWD).
       CORRECAO 07/ago: revertido - SWD nao e' mais o caminho
       principal, e a fiacao fisica do dock2 foi refeita para UART de
       verdade. Cada deck tem sua propria UART/conector fisico
       independente de novo (modelo original: 2 pratos, 2 docks). */
    dock_uart_setup(DOCK1_UART_NUM, DOCK1_UART_RX_PIN, DOCK1_UART_TX_PIN);
    dock_uart_setup(DOCK2_UART_NUM, DOCK2_UART_RX_PIN, DOCK2_UART_TX_PIN);
    s_senddock_request_queue = xQueueCreate(4, sizeof(senddock_request_t));
    xTaskCreatePinnedToCore(pairing_task, "dock_pairing", 4096, NULL, 3, NULL, 0);
}
