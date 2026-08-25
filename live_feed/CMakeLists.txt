if(CONFIG_ZMK_LIVE_FEED)
    zephyr_library()
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

    # Central-only feature (guarded by the Kconfig depends): the brain that watches
    # key/layer events and coalesces them, plus its NOTIFY GATT window.
    zephyr_library_sources(src/live_feed_central.c)
    zephyr_library_sources(src/gatt_service.c)

    # Studio RPC (USB/BLE) window: same 16-byte records, one stream.
    zephyr_library_sources_ifdef(CONFIG_ZMK_LIVE_FEED_TUNNEL src/tunnel_bridge.c)
endif()
