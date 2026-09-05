/*
 * PLAN-ext-fw-refactor.md phase 9 fixture: the "declared" build.
 *
 * Force-included AFTER stubs/torabo_test_config.h (see run-tests.sh's second
 * -include on the dedicated `wire_tests_decl` binary), so it starts from that
 * baseline (feature set, fw version, LED presence, etc. all unchanged) and
 * overrides only the five PLAN phase 9 Kconfig ints from their 0 (undeclared)
 * default to the REAL right-central hardware pattern (right = central + ball
 * on its standard connector, left = encoder on its standard connector, both
 * halves carry an extension pad) — this is the exact conf fw-test builds
 * today and firmware-builder's genConf() produces for it, and it is the
 * fixture torabo-studio's own caps decoder test shares (MODULES caps word
 * 0x2129, wire bytes 0x29 0x21 little-endian):
 *   left std  = TORABO_CAPS_SLOT_ENCODER (9) -> bits0-3
 *   left ext  = TORABO_CAPS_SLOT_PAD     (2) -> bits4-7
 *   right std = TORABO_CAPS_SLOT_BALL    (1) -> bits8-11
 *   right ext = TORABO_CAPS_SLOT_PAD     (2) -> bits12-15
 *   0x9 | (0x2<<4) | (0x1<<8) | (0x2<<12) = 0x2129
 *
 * This is a separate binary (main_decl.c + test_caps_decl.c), not the regular
 * wire_tests_l* run: test_caps.c's golden vector is pinned to the baseline
 * (all-zero / "unset conf") values and must NOT see these overrides, which is
 * exactly the point — proving the two are independent.
 *
 * A second, additional "double" configuration (ball on both standard
 * connectors, encoder on both extension connectors) is fixtured separately in
 * torabo_test_config_decl2.h / test_caps_decl2.c, purely to exercise a
 * multi-bit-per-nibble-adjacent-slot case this single-instance-per-kind
 * fixture cannot (every slot value here is used at most... well BALL and PAD
 * both appear twice, but never adjacent-bit aliasing across slot boundaries —
 * decl2 stresses that separately).
 */
#pragma once

#undef CONFIG_TORABO_CENTRAL_SIDE
#undef CONFIG_TORABO_SLOT_LEFT_STD
#undef CONFIG_TORABO_SLOT_LEFT_EXT
#undef CONFIG_TORABO_SLOT_RIGHT_STD
#undef CONFIG_TORABO_SLOT_RIGHT_EXT

#define CONFIG_TORABO_CENTRAL_SIDE 2     /* right */
#define CONFIG_TORABO_SLOT_LEFT_STD 9    /* encoder */
#define CONFIG_TORABO_SLOT_LEFT_EXT 2    /* pad */
#define CONFIG_TORABO_SLOT_RIGHT_STD 1   /* ball */
#define CONFIG_TORABO_SLOT_RIGHT_EXT 2   /* pad */
