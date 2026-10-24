# Variantes de build (pastas de build separadas para o valor em cache de uma
# nao vazar para a outra):
#   Firmware normal (CV02):      idf.py build
#   Modo HID (radio + HID real): idf.py -B build_teste_hid -DTESTE_HID=1 build
#                                -> main.c com DVS_MODO_HID: sem I2S/audio/dock_bridge
#   Teste HID sem TX (fake):     idf.py -B build_teste_hid_fake -DTESTE_HID_FAKE=1 build
#                                -> teste_hid_main.c: so TinyUSB + velocidade senoidal
if(TESTE_HID_FAKE)
    set(srcs "teste_hid_main.c" "usb_hid.c")
else()
    set(srcs "main.c" "nrf24.c" "tone_synth.c" "dock_pairing.c" "dock_bridge.c" "dock_led.c" "cv02_synth.c")
    if(TESTE_HID)
        list(APPEND srcs "usb_hid.c")
    endif()
endif()

idf_component_register(SRCS ${srcs}
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_timer nvs_flash esp_driver_usb_serial_jtag esp_driver_rmt esp_tinyusb)

if(TESTE_HID AND NOT TESTE_HID_FAKE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DVS_MODO_HID=1)
endif()
