if(CONFIG_ZMK_ENCODER_CONFIG)
    zephyr_library()
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
    # Shared header-only helpers (e.g. the tunnel apply+save wrapper macro).
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common/include)

    # Runtime store (double-buffered, validated, NVS-backed).
    zephyr_library_sources(src/config_state.c)

    # Rotation: a sensor behavior that resolves cw/ccw from the store.
    # Button: an input processor that resolves the click from the store.
    # Both compile without a referencing DT node (guarded by DT_HAS_COMPAT_STATUS_OKAY).
    zephyr_library_sources(src/enc_behavior.c)
    zephyr_library_sources(src/enc_button.c)

    # BLE settings window.
    if(CONFIG_ZMK_ENCODER_CONFIG_BLE)
        zephyr_library_sources(src/gatt_service.c)
    endif()

    # Studio RPC (USB/BLE) settings window, same wire blob as the GATT one.
    zephyr_library_sources_ifdef(CONFIG_ZMK_ENCODER_CONFIG_TUNNEL src/tunnel_bridge.c)
endif()
