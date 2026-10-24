/* teste_hid_main.c - firmware de TESTE do modo HID (TESTE_HID=1 no CMake).
 *
 * Sobe direto em modo HID, sem radio, I2S nem seletor, e manda velocidades
 * falsas para validar que o Mac/Mixxx reconhecem o dispositivo e respondem:
 *   deck1: senoide entre 160 e 240 graus/s (0,8x a 1,2x), periodo de 4 s
 *   deck2: fixo em 200 graus/s (1,0x), para comparar
 *
 * O firmware normal (main.c) nao e compilado neste modo - ver main/CMakeLists.txt.
 */
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "usb_hid.h"

static const char *TAG = "teste_hid";

#define TEST_PERIOD_MS     10        /* 1 tick com CONFIG_FREERTOS_HZ=100 */
#define TEST_CENTER_DPS    200.0f    /* 33 1/3 rpm */
#define TEST_SWING_DPS     40.0f     /* +-40 -> 160..240 */
#define TEST_WAVE_S        4.0f      /* periodo da senoide do deck1 */

static void hid_test_task(void *arg)
{
    (void)arg;
    const float two_pi = 6.28318530718f;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t sent = 0, skipped = 0;
    int64_t last_log_us = 0;

    while (1) {
        float t = (float)esp_timer_get_time() / 1e6f;
        float dps1 = TEST_CENTER_DPS + TEST_SWING_DPS * sinf(two_pi * t / TEST_WAVE_S);
        float dps2 = TEST_CENTER_DPS;

        if (usb_hid_send_velocities(dps1, dps2, 0)) {
            sent++;
        } else {
            skipped++;   /* host ainda nao configurou o dispositivo, ou EP ocupado */
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= 1000000) {
            last_log_us = now_us;
            ESP_LOGI(TAG, "mounted=%d enviados=%lu pulados=%lu deck1=%.1f deck2=%.1f dps",
                     usb_hid_ready(), (unsigned long)sent, (unsigned long)skipped, dps1, dps2);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TEST_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DVS Wireless RX - FIRMWARE DE TESTE HID (sem radio/I2S)");
    ESP_ERROR_CHECK(usb_hid_start());
    xTaskCreatePinnedToCore(hid_test_task, "hid_test", 4096, NULL, 5, NULL, 0);
}
