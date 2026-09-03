if(CONFIG_ZMK_LED_CONFIG)
    zephyr_library()
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
    # Shared header-only helpers (e.g. the tunnel apply+save wrapper macro).
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common/include)

    # The renderer owns the GPIOs and runs on BOTH halves.
    zephyr_library_sources(src/led_render.c)

    # The rule table + its BLE window live on the central, which is the only side
    # that decides anything. Compiled on both (the store is harmless on the
    # peripheral); the brain guards itself with CONFIG_ZMK_SPLIT_ROLE_CENTRAL.
    zephyr_library_sources(src/config_state.c)
    zephyr_library_sources(src/led_central.c)

    # The peripheral's receiver. Guarded by DT_HAS_COMPAT_STATUS_OKAY, so it costs
    # nothing on a build without the node.
    zephyr_library_sources(src/led_ext_beh.c)

    if(CONFIG_ZMK_LED_CONFIG_BLE)
        zephyr_library_sources(src/gatt_service.c)
    endif()

    # Studio RPC (USB/BLE) settings window, same wire blob as the GATT one.
    zephyr_library_sources_ifdef(CONFIG_ZMK_LED_CONFIG_TUNNEL src/tunnel_bridge.c)
endif()
