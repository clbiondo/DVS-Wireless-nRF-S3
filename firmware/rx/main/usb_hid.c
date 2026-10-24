/* usb_hid.c - descritores USB/HID, callbacks do TinyUSB e envio do report de
 * velocidade. Ver formato do report em usb_hid.h.
 */
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

#include "usb_hid.h"

static const char *TAG = "usb_hid";

#define DVS_USB_VID  0x303A   /* Espressif */
#define DVS_USB_PID  0x4004   /* PID do TinyUSB/ESP-IDF para "so HID" - casa com o .hid.xml */

/* ── Report descriptor: vendor-defined, 1 report de entrada de 6 bytes ── */
static const uint8_t s_report_desc[] = {
    HID_USAGE_PAGE_N(0xFF00, 2),
    HID_USAGE(0x01),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
        HID_REPORT_ID(DVS_HID_REPORT_ID)
        HID_USAGE(0x01),
        HID_LOGICAL_MIN(0x00),
        HID_LOGICAL_MAX_N(0xFF, 2),
        HID_REPORT_SIZE(8),
        HID_REPORT_COUNT(DVS_HID_REPORT_LEN),
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_COLLECTION_END,
};

/* ── Device descriptor explicito: VID/PID fixos, nao dependem do menuconfig ── */
static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,          /* classe definida na interface */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = DVS_USB_VID,
    .idProduct          = DVS_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

/* ── Configuration descriptor: 1 interface HID (interface 0), EP IN 0x81 ── */
#define DVS_HID_ITF       0
#define DVS_HID_EP_IN     0x81
#define DVS_HID_EP_SIZE   16      /* >= 1 (ID) + 6 bytes */
#define DVS_HID_POLL_MS   1       /* bInterval: host pede report a cada 1 ms */
#define DVS_CONFIG_LEN    (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_config_desc[] = {
    /* config numero, n de interfaces, string, tamanho total, atributos, mA */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, DVS_CONFIG_LEN, 0, 100),
    /* interface, string, protocolo (nenhum), tam. report desc, EP IN, tam. EP, intervalo */
    TUD_HID_DESCRIPTOR(DVS_HID_ITF, 4, HID_ITF_PROTOCOL_NONE, sizeof(s_report_desc),
                       DVS_HID_EP_IN, DVS_HID_EP_SIZE, DVS_HID_POLL_MS),
};

/* ── Strings: [0] idioma (en-US), depois os indices usados acima ── */
static const char *s_strings[] = {
    (const char[]){ 0x09, 0x04 },   /* 0: LANGID 0x0409 */
    "DVS Wireless",                 /* 1: fabricante */
    "DVS Wireless RX",              /* 2: produto */
    "DVSRX0001",                    /* 3: numero de serie */
    "DVS Wireless HID",             /* 4: interface HID */
};

/* ── Callbacks obrigatorios do TinyUSB HID ── */

/* Host pediu o report descriptor (so ha uma interface HID). */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_report_desc;
}

/* GET_REPORT pelo canal de controle: nao usado (dados vao pelo EP IN). */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

/* SET_REPORT / report de saida: nao usado. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

/* ── Eventos de conexao (so log) ── */
static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "USB conectado ao host");
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "USB desconectado");
        break;
    default:
        break;
    }
}

esp_err_t usb_hid_start(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    cfg.descriptor.device            = &s_device_desc;
    cfg.descriptor.full_speed_config = s_config_desc;
    cfg.descriptor.string            = s_strings;
    cfg.descriptor.string_count      = sizeof(s_strings) / sizeof(s_strings[0]);

    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install falhou: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "TinyUSB HID instalado (VID %04X PID %04X, interface %d)",
             DVS_USB_VID, DVS_USB_PID, DVS_HID_ITF);
    return ESP_OK;
}

bool usb_hid_ready(void)
{
    return tud_mounted() && tud_hid_ready();
}

/* graus/s -> int16 em decimos de grau/s, com saturacao */
static int16_t dps_to_i16(float dps)
{
    float v = dps * 10.0f;
    if (!isfinite(v)) {
        return 0;
    }
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (int16_t)lroundf(v);
}

bool usb_hid_send_velocities(float dps_deck1, float dps_deck2, uint8_t flags)
{
    static uint8_t seq = 0;

    if (!usb_hid_ready()) {
        return false;
    }
    int16_t v1 = dps_to_i16(dps_deck1);
    int16_t v2 = dps_to_i16(dps_deck2);
    uint8_t report[DVS_HID_REPORT_LEN] = {
        (uint8_t)(seq + 1),
        flags,
        (uint8_t)(v1 & 0xFF), (uint8_t)((uint16_t)v1 >> 8),
        (uint8_t)(v2 & 0xFF), (uint8_t)((uint16_t)v2 >> 8),
    };
    if (!tud_hid_report(DVS_HID_REPORT_ID, report, sizeof(report))) {
        return false;
    }
    seq++;
    return true;
}
