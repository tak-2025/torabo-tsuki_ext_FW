if(CONFIG_ZMK_TRACKBALL_CONFIG OR CONFIG_ZTC_ENCODER)
    zephyr_library()
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
    # Shared header-only helpers (e.g. the tunnel apply+save wrapper macro).
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common/include)

    # Runtime store + its processors + GATT (only with the trackball config module).
    zephyr_library_sources_ifdef(CONFIG_ZMK_TRACKBALL_CONFIG src/config_state.c)
    zephyr_library_sources_ifdef(CONFIG_ZTC_POINTER src/ztc_pointer.c)
    zephyr_library_sources_ifdef(CONFIG_ZTC_TEMP_LAYER src/ztc_temp_layer.c)

    # Optional: blink the status LED with the active BLE profile number.
    zephyr_library_sources_ifdef(CONFIG_ZTC_PROFILE_LED_INDICATOR src/ztc_profile_led.c)

    # BLE settings window.
    if(CONFIG_ZMK_TRACKBALL_CONFIG_BLE)
        zephyr_library_sources(src/gatt_service.c)
    endif()

    # Studio RPC (USB/BLE) settings window, same wire blob as the GATT one.
    zephyr_library_sources_ifdef(CONFIG_ZMK_TRACKBALL_CONFIG_TUNNEL src/tunnel_bridge.c)

    # Encoder: self-contained (no store / GATT), used by the torabo-trackpad snippet.
    zephyr_library_sources_ifdef(CONFIG_ZTC_ENCODER src/ztc_encoder.c)
endif()
