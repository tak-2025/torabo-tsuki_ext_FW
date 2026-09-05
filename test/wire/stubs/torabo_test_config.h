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
#define CONFIG_TORABO_FW_VERSION_PATCH 1

/* CONFIG_TORABO_CAPS itself: gates the TORABO_FEAT_MODULES row (always added
 * whenever the caps descriptor is built at all, PLAN phase 9). This is the
 * menuconfig bool, not CONFIG_TORABO_CAPS_BLE (the GATT service, deliberately
 * left undefined above — host tests never touch Bluetooth). */
#define CONFIG_TORABO_CAPS 1

/* ---- caps: PLAN phase 9 module-layout declaration bits (re-redesigned
 * 2026-09-03: one 4-bit slot value per physical connector, replacing the two
 * earlier per-feature schemes) --------------------------------------------
 * The baseline fixture for this file is "conf doesn't set these" — every real
 * Kconfig int defaults to 0 when unset, so this is not a special case, it's
 * the literal default. 0 = TORABO_CAPS_SLOT_UNDECLARED on all four slots,
 * which is why the 52B golden vector in test_caps.c has an all-zero MODULES
 * row: this is what every conf that never adopts the new Kconfig lines
 * produces. torabo_test_config_decl.h overrides these to a real hardware
 * fixture for the "declared" build (see test_caps_decl.c). */
#define CONFIG_TORABO_CENTRAL_SIDE 0
#define CONFIG_TORABO_SLOT_LEFT_STD 0
#define CONFIG_TORABO_SLOT_LEFT_EXT 0
#define CONFIG_TORABO_SLOT_RIGHT_STD 0
#define CONFIG_TORABO_SLOT_RIGHT_EXT 0

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

/* ---- the torabo tunnel's blob budget --------------------------------------
 * The trackpad's WRITE guard (docs/BACKLOG.md B-1) refuses a device_count whose
 * READ wire would outgrow this. 2048 is the Kconfig default, and what the field
 * firmware actually runs (fw-test leaves it unset), so the guard is fixtured on
 * the number that decides real behavior. Registering the trackpad on the tunnel
 * is what makes the budget apply at all, hence the _TUNNEL symbol too. */
#define CONFIG_ZMK_TRACKPAD_CONFIG_TUNNEL 1
#define CONFIG_ZMK_STUDIO_TORABO_TUNNEL_BLOB_MAX_SIZE 2048

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
