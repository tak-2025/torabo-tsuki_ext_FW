# Torabo ZMK features. Each subdirectory is a self-contained feature whose
# sources compile only when its CONFIG is enabled (the guards live inside each
# subdir's CMakeLists). Distinct subdirectories => distinct zephyr_library
# names, so the two features never collide.
add_subdirectory(macros)
add_subdirectory(combos)
add_subdirectory(trackball)
add_subdirectory(trackpad)
add_subdirectory(encoder)
add_subdirectory(caps)
add_subdirectory(led)
add_subdirectory(status_led_ext)
add_subdirectory(live_feed)
add_subdirectory(timing)
