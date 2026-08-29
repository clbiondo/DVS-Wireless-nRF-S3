idf_component_register(SRCS "main.c" "nrf24.c" "tone_synth.c" "dock_pairing.c" "dock_bridge.c" "dock_led.c" "cv02_synth.c"
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_timer nvs_flash esp_driver_usb_serial_jtag esp_driver_rmt)
