if(CONFIG_ZMK_TRACKPAD_CONFIG)
    zephyr_library()
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
    # Shared header-only helpers (e.g. the tunnel apply+save wrapper macro).
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common/include)

    # Runtime store (double-buffered, validated, NVS-backed).
    zephyr_library_sources(src/config_state.c)

    # Device-aware input processors that apply the store to the pad input path.
    # Compile even without a referencing DT node (no instances): tp_pointer drives
    # the continuous/ENCODER axis path, tp_keys remaps the tap/gesture clicks.
    zephyr_library_sources(src/tp_pointer.c)
    zephyr_library_sources(src/tp_keys.c)

    # BLE settings window.
    if(CONFIG_ZMK_TRACKPAD_CONFIG_BLE)
        zephyr_library_sources(src/gatt_service.c)
    endif()

    # Studio RPC (USB/BLE) settings window, same wire blob as the GATT one.
    zephyr_library_sources_ifdef(CONFIG_ZMK_TRACKPAD_CONFIG_TUNNEL src/tunnel_bridge.c)
endif()
