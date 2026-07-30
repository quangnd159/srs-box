idf_component_register(
    SRCS "devctl.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
             esp_lvgl_port
             esp_driver_usb_serial_jtag
             esp_timer
             littlefs
)
