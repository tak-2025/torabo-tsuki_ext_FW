if(CONFIG_TORABO_CAPS)
    zephyr_library()
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

    # caps.c reports each feature's wire version from that feature's OWN header,
    # so it needs every feature's include dir. Listed unconditionally (an include
    # path costs nothing when the #include behind it is compiled out); the
    # #include itself is guarded by the same CONFIG as the row that uses it.
    zephyr_library_include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../trackball/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../trackpad/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../encoder/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../led/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../timing/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../combos/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../live_feed/include
    )

    # <zmk/torabo_timing.h> (pulled in by zmk_timing_config/config.h) lives with
    # the rest of the zmk app headers, which are PRIVATE to the `app` target.
    # Ask for the directory explicitly rather than relying on some other
    # CMakeLists having made it global — same reason features/timing does.
    zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

    zephyr_library_sources(src/caps.c)

    # Studio RPC (USB/BLE) window, same wire blob as the GATT one.
    zephyr_library_sources_ifdef(CONFIG_TORABO_CAPS_TUNNEL src/tunnel_bridge.c)
endif()
