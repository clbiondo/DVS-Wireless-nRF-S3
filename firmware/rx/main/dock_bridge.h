#ifndef DOCK_BRIDGE_H
#define DOCK_BRIDGE_H

/**
 * dock_bridge — repasse transparente USB-Serial-JTAG <-> UART do dock,
 * para permitir gravar firmware nos TX (bootloader Adafruit nRF52,
 * protocolo DFU serial legado da Nordic) através do RX, sem precisar
 * de conexão USB direta com o TX.
 *
 * Reaproveita o que já foi validado manualmente em 21/jul (placa-ponte
 * nice!nano fazendo USB<->UART) — aqui o RX assume esse mesmo papel.
 *
 * COMANDOS (linha terminada em '\n', recebida pelo mesmo canal USB
 * usado pro console):
 *   BRIDGE:1   -> entra em modo bridge para o dock do deck1
 *   BRIDGE:2   -> entra em modo bridge para o dock do deck2
 *
 * Sai do bridge sozinho após DOCK_BRIDGE_IDLE_TIMEOUT_MS sem tráfego
 * em nenhuma direção, e dá esp_restart() pra garantir volta limpa ao
 * modo normal (rádio/áudio). Não existe comando explícito de saída —
 * decisão deliberada, ver comentário em dock_bridge.c sobre o risco de
 * colisão de string dentro de um payload binário.
 *
 * PONTOS A VALIDAR EM BANCADA ANTES DE CONFIAR NISSO (checklist):
 *   [ ] UART_NUM_DECK1 / UART_NUM_DECK2 abaixo batem com os mesmos
 *       números já usados em dock_pairing.c? (ajustar os defines se não)
 *   [ ] DOCK_BRIDGE_BAUD é o mesmo baud usado no teste de ponte de
 *       21/jul? (conferir no firmware da placa-ponte daquela sessão)
 *   [ ] O console (printf/ESP_LOG) usa o mesmo canal USB-Serial-JTAG?
 *       Se sim, os logs podem se intercalar com bytes binários do DFU
 *       e quebrar o protocolo — testar com log silenciado (ver
 *       enter_bridge_mode) antes de confiar em uma gravação real
 *   [ ] Rodar um teste completo de DFU através do bridge numa placa
 *       de teste (não numa TX de produção) antes de usar em campo
 */

void dock_bridge_init(void);

#endif // DOCK_BRIDGE_H
