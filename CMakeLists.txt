# Torabo ZMK features. Each subdirectory under features/ is a self-contained
# feature whose sources compile only when its CONFIG is enabled (the guards live
# inside each subdir's CMakeLists). Distinct subdirectories => distinct
# zephyr_library names, so the two features never collide.
add_subdirectory(features/macros)
add_subdirectory(features/combos)
add_subdirectory(features/trackball)
add_subdirectory(features/trackpad)
add_subdirectory(features/encoder)
add_subdirectory(features/caps)
add_subdirectory(features/led)
add_subdirectory(features/live_feed)
add_subdirectory(features/timing)
