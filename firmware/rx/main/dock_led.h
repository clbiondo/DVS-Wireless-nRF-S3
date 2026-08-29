#ifndef DOCK_LED_H
#define DOCK_LED_H

#include <stdint.h>

/**
 * dock_led — controle do LED RGB onboard (WS2812/NeoPixel single-LED)
 * do ESP32-S3, via periferico RMT (sem dependencia externa - so' usa
 * o componente esp_driver_rmt, ja incluido no ESP-IDF base).
 *
 * Pino: GPIO48 (confirmado via serigrafia/tabela de pinagem da placa
 * DOIT ESP32-S3 N8R2 - rotulado "RGB" no silkscreen).
 *
 * Uso pretendido: feedback visual de processos (modo bridge/SWD ativo,
 * pareamento em andamento/sucesso/falha, status geral do sistema).
 */

#define DOCK_LED_PIN  48

// Estados de alto nivel - cada um mapeia pra uma cor/padrao especifico.
// Chamar dock_led_set_state() ao entrar em cada fase do processo.
typedef enum {
    DOCK_LED_OFF,          // apagado - estado de repouso, nada acontecendo
    DOCK_LED_IDLE,         // sistema rodando normal (azul fraco)
    DOCK_LED_BRIDGE_SWD,   // gravacao/SWD ativa (ciano)
    DOCK_LED_PAIRING,      // pareamento em andamento (roxo)
    DOCK_LED_PAIR_OK,      // pareamento confirmado (verde)
    DOCK_LED_PAIR_FAIL,    // pareamento falhou (vermelho)
} dock_led_state_t;

// Inicializa o driver RMT + configura o pino. Chamar uma vez no boot.
void dock_led_init(void);

// Define a cor via um dos estados pre-definidos acima.
void dock_led_set_state(dock_led_state_t state);

// Define uma cor RGB arbitraria (0-255 cada canal), para casos que
// nao se encaixam nos estados pre-definidos.
void dock_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

#endif // DOCK_LED_H
