if(CONFIG_ZMK_DYNAMIC_COMBOS)
    zephyr_library()
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

    # Validated, wire-native staged store + NVS persistence (always).
    zephyr_library_sources(src/combo_state.c)

    # BLE settings window.
    if(CONFIG_ZMK_DYNAMIC_COMBOS_BLE)
        zephyr_library_sources(src/gatt_service.c)
    endif()

    # Studio RPC (USB/BLE) settings window, same wire blobs as the GATT one.
    zephyr_library_sources_ifdef(CONFIG_ZMK_DYNAMIC_COMBOS_TUNNEL src/tunnel_bridge.c)

    # The combo matching engine (literal copy of ZMK combo.c, RAM-backed defs)
    # is a position_state_changed listener that MUST run BEFORE zmk's keymap
    # listener to intercept combo keys. Listener order == link order of the
    # `.event_subscription` section (zmk-events.ld uses KEEP, no SORT). A
    # separate zephyr_library() links AFTER zmk's `app`, so our listener would
    # land after keymap (verified: dyn_combo subscription > keymap by address).
    #
    # Fix: compile the engine straight into the `app` target from here. This
    # CMakeLists runs during find_package(Zephyr) — i.e. BEFORE app/CMakeLists
    # adds src/combo.c / src/keymap.c — so our object (and its subscription)
    # lands ahead of keymap's. Self-guards on the "zmk,dynamic-combos" DT node.
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/combo_engine.c)
    target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
