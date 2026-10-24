# TESTE_HID: firmware de teste do modo HID (so TinyUSB + report fake, sem radio/I2S).
#   Compilar o teste:  idf.py -B build_teste_hid -DTESTE_HID=1 build
#   Firmware normal:   idf.py build            (pasta build/, TESTE_HID desligado)
# Pastas de build separadas para o valor em cache de um nao vazar para o outro.
if(TESTE_HID)
    set(srcs "teste_hid_main.c" "usb_hid.c")
else()
    set(srcs "main.c" "nrf24.c" "tone_synth.c" "dock_pairing.c" "dock_bridge.c" "dock_led.c" "cv02_synth.c")
endif()

idf_component_register(SRCS ${srcs}
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_timer nvs_flash esp_driver_usb_serial_jtag esp_driver_rmt esp_tinyusb)
