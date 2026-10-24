# Firmware (CV02 + HID sempre juntos): idf.py build
#   CV02 pelo I2S (audio) + HID pelo USB nativo (velocidade), no mesmo binario.
# Teste de bancada sem TX (pasta de build separada para nao misturar o cache):
#   idf.py -B build_teste_hid_fake -DTESTE_HID_FAKE=1 build
#   -> teste_hid_main.c: so TinyUSB + velocidade senoidal
if(TESTE_HID_FAKE)
    set(srcs "teste_hid_main.c" "usb_hid.c")
else()
    set(srcs "main.c" "nrf24.c" "tone_synth.c" "dock_pairing.c" "dock_bridge.c" "dock_led.c" "cv02_synth.c" "usb_hid.c")
endif()

idf_component_register(SRCS ${srcs}
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_timer nvs_flash esp_driver_usb_serial_jtag esp_driver_rmt esp_tinyusb)
