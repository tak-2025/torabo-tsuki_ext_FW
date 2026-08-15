if(CONFIG_TORABO_STATUS_LED_EXT)
    zephyr_library()
    zephyr_library_sources(src/status_led_ext.c)
endif()
