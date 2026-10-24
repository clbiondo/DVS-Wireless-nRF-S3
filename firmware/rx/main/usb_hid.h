/* usb_hid.h - modo HID do RX: manda a velocidade dos 2 decks por USB (USB-OTG,
 * TinyUSB), para o Mixxx aplicar direto em [ChannelN],scratch2.
 *
 * Dispositivo: VID 0x303A (Espressif), PID 0x4004, interface HID 0,
 * usage page 0xFF00 / usage 0x01 (vendor-defined) - casa com
 * "DVS Wireless.hid.xml" do Mixxx.
 *
 * Report de entrada (ID 1 + 6 bytes, little-endian):
 *   [0] seq   uint8   incrementa a cada report (o Mixxx descarta reports
 *                     identicos consecutivos; o seq garante que nenhum e igual)
 *   [1] flags uint8   reservado (0)
 *   [2] vel1  int16   velocidade do deck 1 em graus/s x 10
 *   [4] vel2  int16   velocidade do deck 2 em graus/s x 10
 */
#ifndef USB_HID_H
#define USB_HID_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define DVS_HID_REPORT_ID   1
#define DVS_HID_REPORT_LEN  6   /* sem o byte de report ID */

/* Instala o TinyUSB com os descritores do DVS. Assume o PHY USB (sai do
 * USB-Serial-JTAG). Chamar uma vez, so no modo HID. */
esp_err_t usb_hid_start(void);

/* true quando o host configurou o dispositivo e o endpoint esta livre. */
bool usb_hid_ready(void);

/* Monta e envia um report com as velocidades em graus/s (saturadas no range
 * do int16 apos x10). Incrementa o seq. Retorna false se o endpoint nao estava
 * pronto (report descartado, o seq nao avanca). */
bool usb_hid_send_velocities(float dps_deck1, float dps_deck2, uint8_t flags);

#endif /* USB_HID_H */
