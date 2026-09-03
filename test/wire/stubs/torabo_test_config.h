/*
 * Force-included (-include) into every translation unit of the host wire tests.
 *
 * It stands in for Zephyr's generated autoconf.h: the Kconfig symbols the wire
 * codecs read directly. Every value here is a DELIBERATE test fixture — the caps
 * descriptor's pinned byte vector, in particular, is stated for exactly this set.
 *
 * Deliberately NOT defined (so the corresponding `#if IS_ENABLED(...)` blocks
 * compile out, as they do in a host build):
 *   CONFIG_SETTINGS                 - NVS persistence (needs a Zephyr backend)
 *   CONFIG_TORABO_CAPS_BLE et al.   - the GATT service definitions
 *   CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS
 */
#pragma once

/* ---- caps: firmware version reported in the descriptor header ------------- */
#define CONFIG_TORABO_FW_VERSION_MAJOR 0
#define CONFIG_TORABO_FW_VERSION_MINOR 1
#define CONFIG_TORABO_FW_VERSION_PATCH 0

/* ---- caps: the "everything on" build the 10-feature vector describes ------ */
#define CONFIG_ZMK_TRACKBALL_CONFIG 1
#define CONFIG_ZMK_DYNAMIC_KEYMAP 1
#define CONFIG_ZMK_DYNAMIC_COMBOS 1
#define CONFIG_ZMK_TRACKPAD_CONFIG 1
#define CONFIG_ZMK_ENCODER_CONFIG 1
#define CONFIG_ZMK_LED_CONFIG 1
#define CONFIG_TORABO_RESERVED_LAYERS 4
#define CONFIG_ZMK_LIVE_FEED 1
#define CONFIG_ZMK_STUDIO_TORABO_TUNNEL 1
#define CONFIG_ZMK_TIMING_CONFIG 1

/* ---- led: which halves carry an LED, and which half is central ------------
 * LEFT+RIGHT present, central is RIGHT (CENTRAL_IS_LEFT left undefined) =>
 * caps byte 0x03 in both the led wire and the caps descriptor. */
#define CONFIG_ZMK_LED_CONFIG_LEFT_PRESENT 1
#define CONFIG_ZMK_LED_CONFIG_RIGHT_PRESENT 1

/* ---- timing: the split debounce sync caps bit, and the DT debounce windows */
#define CONFIG_ZMK_SPLIT_BLE_DEBOUNCE_SYNC 1
#define CONFIG_ZMK_TIMING_CONFIG_DEBOUNCE_PRESS_MS 5
#define CONFIG_ZMK_TIMING_CONFIG_DEBOUNCE_RELEASE_MS 5

/* ---- trackpad: FW-authoritative per-device identity bytes -----------------
 * DEV0 = left pad on the standard FFC  : side=1 conn=1 kind=1 -> 0x15
 * DEV1 = right pad on the extender FPC : side=2 conn=2 kind=1 -> 0x1A
 * DEV2/DEV3 unpopulated (Kconfig default 0). */
#define CONFIG_ZMK_TRACKPAD_CONFIG_DEV0_META 0x15
#define CONFIG_ZMK_TRACKPAD_CONFIG_DEV1_META 0x1A
#define CONFIG_ZMK_TRACKPAD_CONFIG_DEV2_META 0
#define CONFIG_ZMK_TRACKPAD_CONFIG_DEV3_META 0
