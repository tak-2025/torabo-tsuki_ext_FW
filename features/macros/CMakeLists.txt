if(CONFIG_ZMK_DYNAMIC_KEYMAP)
    zephyr_library()
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

    # Validated, double-buffered RAM store + NVS persistence (always).
    zephyr_library_sources(src/config_state.c)

    # The &dmac behavior. Self-guards on the devicetree node existing, so it
    # compiles to nothing when no node is instantiated.
    zephyr_library_sources(src/dmac_behavior.c)

    # BLE settings window.
    if(CONFIG_ZMK_DYNAMIC_KEYMAP_BLE)
        zephyr_library_sources(src/gatt_service.c)
    endif()

    # Studio RPC (USB/BLE) settings window, same wire blobs as the GATT one.
    zephyr_library_sources_ifdef(CONFIG_ZMK_DYNAMIC_KEYMAP_TUNNEL src/tunnel_bridge.c)
endif()
