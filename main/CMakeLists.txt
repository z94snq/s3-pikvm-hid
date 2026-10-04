idf_component_register(
    SRCS "main.c" "core.c" "usb_hid.c" "outputs_esp.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer
)
