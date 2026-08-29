#ifndef DOCK_PAIRING_H
#define DOCK_PAIRING_H

/* Modulo unificado de pareamento + troca de canal, via dock:
 *
 * Quando a chave de um deck fica pressionada por 3 segundos continuos:
 *   1. O RX sorteia um canal aleatorio dentro da faixa dedicada daquele
 *      deck (RNG de hardware do ESP32-S3), resolvendo interferencia no
 *      ambiente sem precisar reflashar nada.
 *   2. O RX grava esse canal na propria NVS (sobrevive a reboot).
 *   3. O RX manda o canal sorteado + o endereco fixo daquele deck pro
 *      TX que estiver docado, via UART do dock (comando PAIR).
 *   4. O TX grava canal+endereco na propria NVS - por ser um comando
 *      que so' funciona com o TX fisicamente docado, esse mesmo gesto
 *      tambem serve pra "batizar" qualquer TX fisico como aquele deck
 *      (troca de placa, substituicao por defeito, etc).
 *   5. O RX reinicia (esp_restart) para religar seu proprio radio com o
 *      canal novo, ja lido da NVS no boot seguinte.
 *
 * Faixas de canal por deck (acima do teto ISM 2483,5MHz = canal 83,
 * fora do range mais congestionado de WiFi/BLE; faixa REAL suportada
 * pela biblioteca ESB da Nordic e' 0-100, NAO 0-125 como o registrador
 * de hardware RF_CH tecnicamente aceitaria - corrigido 29/jul apos
 * confirmar na pratica que canal 125 causava falha de esb_init() no TX,
 * loop de 9 piscadas de erro):
 *   - Deck1: canais 84-90 (7 opcoes)
 *   - Deck2: canais 93-100 (8 opcoes)
 *   - 91-92 ficam de guarda entre os dois
 *
 * Endereco de cada deck continua FIXO (nao sorteado, so' o canal):
 *   - Deck1: C3:5A:71:98
 *   - Deck2: 8E:2D:64:B1
 *
 * NVS: usa o namespace "dvs_dock", chaves "ch_deck1"/"ch_deck2" (uint8_t).
 * O main.c deve ler essas chaves no boot (com fallback pro canal atual
 * hardcoded, caso nunca tenha sido sorteado ainda) - ver README no
 * final de dock_pairing.c para o trecho exato a integrar.
 */

#include "nrf24.h"

void dock_pairing_init(void);

/* Conecta os ponteiros dos radios nRF24 aos contextos de pareamento,
 * para a varredura de energia (CCA) poder escolher um canal
 * genuinamente livre em vez de so' sortear as cegas. Chamar uma vez,
 * logo apos dock_pairing_init() - os enderecos de deck1/deck2 sao
 * globais estaticas, validas desde o inicio do programa (nao precisa
 * esperar nrf24_init() ja ter rodado). */
void dock_pairing_attach_radios(nrf24_dev_t *radio_deck1, nrf24_dev_t *radio_deck2);

/* Envia um comando de texto (ex: "CLEAROFFSET\n", "ENTERDFU\n") para
 * o TX docado, de forma thread-safe - a UART e' de propriedade
 * exclusiva de pairing_task, entao esta funcao apenas enfileira o
 * pedido e espera a resposta, ao inves de acessar a UART diretamente
 * (evita corrida de recurso entre tasks diferentes). Bloqueia ate
 * timeout_ms ou ate a resposta chegar. Retorna true se recebeu
 * resposta dentro do prazo (resp_out preenchido), false em timeout. */
bool dock_pairing_send_command(const char *cmd_line, char *resp_out,
                                size_t resp_out_len, uint32_t timeout_ms);

#endif /* DOCK_PAIRING_H */
