#include "dock_bridge.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "dock_pairing.h"

static const char *TAG = "dock_bridge";

// TODO: confirmar que estes UART_NUM batem com os mesmos usados em
// dock_pairing.c para cada deck (deck1 = GP21/GP47, deck2 = GP38/GP39)
#define UART_NUM_DECK1      UART_NUM_1
#define UART_NUM_DECK2      UART_NUM_2

// TODO: confirmar baud real usado no teste de ponte validado em 21/jul
#define DOCK_BRIDGE_BAUD    115200

#define DOCK_BRIDGE_IDLE_TIMEOUT_MS   8000
#define DOCK_BRIDGE_CMD_BUF_LEN       32
#define DOCK_BRIDGE_XFER_BUF_LEN      256

static void enter_bridge_mode(uart_port_t dock_uart)
{
    ESP_LOGI(TAG, "Entrando em modo bridge (UART %d)", dock_uart);

    // Silencia o log ANTES do trafego binario comecar - se o console
    // usa o mesmo canal USB-Serial-JTAG, uma linha de log no meio do
    // DFU corrompe o stream. Validar em bancada se isso e' suficiente
    // ou se e' preciso desinstalar o driver de console por completo.
    esp_log_level_set("*", ESP_LOG_NONE);

    // Garante que a UART do dock esta no baud do bootloader (pode já
    // estar, se dock_pairing.c usar o mesmo valor - redundante mas
    // inofensivo)
    uart_set_baudrate(dock_uart, DOCK_BRIDGE_BAUD);

    // Dispara o TX a entrar em bootloader, igual ao comando de dock
    // ja existente no protocolo de pareamento
    const char *dfu_cmd = "DVSDFU\n";
    uart_write_bytes(dock_uart, dfu_cmd, strlen(dfu_cmd));

    // Da tempo do TX processar o reset e subir no bootloader antes de
    // comecar a repassar bytes
    vTaskDelay(pdMS_TO_TICKS(300));

    uint8_t usb_buf[DOCK_BRIDGE_XFER_BUF_LEN];
    uint8_t uart_buf[DOCK_BRIDGE_XFER_BUF_LEN];
    TickType_t last_activity = xTaskGetTickCount();

    while (1) {
        bool had_activity = false;

        // Computador (USB) -> UART do dock
        int n = usb_serial_jtag_read_bytes(usb_buf, sizeof(usb_buf), pdMS_TO_TICKS(5));
        if (n > 0) {
            uart_write_bytes(dock_uart, (const char *)usb_buf, n);
            had_activity = true;
        }

        // UART do dock -> Computador (USB)
        n = uart_read_bytes(dock_uart, uart_buf, sizeof(uart_buf), pdMS_TO_TICKS(5));
        if (n > 0) {
            usb_serial_jtag_write_bytes((const char *)uart_buf, n, pdMS_TO_TICKS(50));
            // FIX (espressif/esp-idf#12605): usb_serial_jtag_write_bytes() nao
            // garante que os bytes realmente saiam pela USB sem um flush
            // explicito do FIFO de transmissao - sem isso, dados podem ficar
            // "presos" internamente e nunca chegar ao computador.
            usb_serial_jtag_ll_txfifo_flush();
            had_activity = true;
        }

        if (had_activity) {
            last_activity = xTaskGetTickCount();
        } else if ((xTaskGetTickCount() - last_activity) > pdMS_TO_TICKS(DOCK_BRIDGE_IDLE_TIMEOUT_MS)) {
            // Sem string magica de saida no meio do stream, de proposito:
            // um payload binario de firmware pode conter por acaso a
            // sequencia "BRIDGE:OFF\n", o que reiniciaria o RX no meio
            // de uma gravacao real. Timeout de inatividade e' mais
            // seguro - a ferramenta no PC simplesmente fecha a porta
            // quando termina o DFU, e o trafego para.
            esp_restart();
        }
    }
}

static void dock_bridge_task(void *arg)
{
    char cmd_buf[DOCK_BRIDGE_CMD_BUF_LEN];
    int cmd_len = 0;

    while (1) {
        uint8_t c;
        int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (cmd_len == 0) {
                continue; // linha vazia, ignora
            }
            cmd_buf[cmd_len] = '\0';

            if (strcmp(cmd_buf, "BRIDGE:1") == 0) {
                enter_bridge_mode(UART_NUM_DECK1);
                // enter_bridge_mode so retorna via esp_restart() - o
                // codigo abaixo nunca deveria rodar, mantido so por
                // seguranca defensiva
            } else if (strcmp(cmd_buf, "BRIDGE:2") == 0) {
                enter_bridge_mode(UART_NUM_DECK2);
            } else if (strncmp(cmd_buf, "SENDDOCK:", 9) == 0) {
                /* CORRIGIDO 03/ago: nao acessa mais UART_NUM_1
                   diretamente (causava corrida de recurso com
                   pairing_task, que tambem usa essa UART). Agora so'
                   enfileira o pedido - pairing_task e' quem de fato
                   fala com a UART, com exclusividade. */
                const char *payload = cmd_buf + 9;
                char line[DOCK_BRIDGE_CMD_BUF_LEN + 2];
                snprintf(line, sizeof(line), "%s\n", payload);

                char resp[128] = {0};
                bool ok = dock_pairing_send_command(line, resp, sizeof(resp), 3000);
                if (ok) {
                    ESP_LOGI(TAG, "SENDDOCK resposta: %s", resp);
                } else {
                    ESP_LOGW(TAG, "SENDDOCK: sem resposta do TX (docado?)");
                }
            }
            cmd_len = 0;
        } else if (cmd_len < DOCK_BRIDGE_CMD_BUF_LEN - 1) {
            cmd_buf[cmd_len++] = (char)c;
        } else {
            cmd_len = 0; // linha longa demais (lixo), descarta
        }
    }
}

void dock_bridge_init(void)
{
    // Assume que UART_NUM_DECK1/DECK2 ja foram instalados (uart_driver_install)
    // dentro de dock_pairing_init(), com os pinos fisicos do dock. Este
    // modulo so reusa esses drivers ja existentes - nao reinstala nada.

    // O driver usb_serial_jtag NAO vem instalado soh por causa do console
    // secundario (CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG usa um
    // caminho de escrita direta, mais leve, sem alocar o contexto que
    // usb_serial_jtag_read_bytes/write_bytes precisam). Precisamos
    // instalar explicitamente aqui, ou toda leitura crasha com
    // ponteiro nulo (EXCVADDR proximo de 0x0).
    usb_serial_jtag_driver_config_t usj_cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&usj_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao instalar driver usb_serial_jtag: %s", esp_err_to_name(err));
        // Nao cria a task se o driver falhou - evita crashar de novo
        // na primeira leitura
        return;
    }

    // CORRECAO 31/jul: prioridade configMAX_PRIORITIES-1 (maxima) causava
    // starvation da task idle do core, disparando o Task Watchdog do
    // proprio sistema (TWDT) e reiniciando o RX no meio do bridge - esse
    // era o crash real por tras do "DFU nao retorna dados". Prioridade 10
    // ainda fica acima das tasks padrao (tipicamente 1-5) sem sufocar o
    // scheduler - ajustar empiricamente se ainda houver latencia excessiva,
    // mas SEM voltar a usar configMAX_PRIORITIES-1.
    xTaskCreatePinnedToCore(dock_bridge_task, "dock_bridge", 4096, NULL,
                             10, NULL, 0);
    ESP_LOGI(TAG, "dock_bridge pronto - aguardando BRIDGE:1 / BRIDGE:2");
}
