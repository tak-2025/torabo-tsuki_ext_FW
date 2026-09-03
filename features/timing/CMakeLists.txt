if(CONFIG_ZMK_TIMING_CONFIG OR CONFIG_ZMK_TIMING_SPLIT_PERIPHERAL)
    zephyr_library()
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
    # Shared header-only helpers (e.g. the tunnel apply+save wrapper macro).
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common/include)

    # <zmk/torabo_timing.h> (the fork's override points) lives with the rest of
    # the zmk app headers, which are PRIVATE to the `app` target. Ask for the
    # directory explicitly rather than relying on some other CMakeLists having
    # made it global.
    zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

    # The central half: the wire, the store, and the strong definitions of the
    # fork's __weak override points. Without it the feature is just the fork's
    # stock behavior.
    if(CONFIG_ZMK_TIMING_CONFIG)
        zephyr_library_sources(src/config_state.c)

        # BLE settings window.
        if(CONFIG_ZMK_TIMING_CONFIG_BLE)
            zephyr_library_sources(src/gatt_service.c)
        endif()

        # Studio RPC (USB/BLE) settings window, same wire blob as the GATT one.
        zephyr_library_sources_ifdef(CONFIG_ZMK_TIMING_CONFIG_TUNNEL src/tunnel_bridge.c)
    endif()

    # The peripheral half: two bytes pushed over the split link, and this side's
    # definition of zmk_torabo_debounce_effective. Mutually exclusive with the
    # central sources above (both define that seam) — the Kconfig enforces it.
    zephyr_library_sources_ifdef(CONFIG_ZMK_TIMING_SPLIT_PERIPHERAL src/split_peripheral.c)
endif()
