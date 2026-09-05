/*
 * Torabo capability descriptor — "what can this firmware actually do?"
 *
 * WHY THIS EXISTS
 * Torabo-Studio must not assume what the keyboard on the other end supports. A
 * build is assembled from snippets, so two keyboards running the same app can
 * have completely different feature sets — one has an encoder, one doesn't; one
 * has LEDs on both halves, one on neither. And as firmware moves on, a wire
 * format can gain fields the app must know about before it writes.
 *
 * So the firmware NAMES ITSELF, once, on connect:
 *   - its own version,
 *   - which feature modules were compiled in,
 *   - the wire version of each, plus per-feature capability bits.
 *
 * The app then shows only the tabs that exist, and can say "this firmware is too
 * old for X" instead of writing a blob the firmware will reject. Discovering
 * features by probing each GATT service and seeing which ones fail would work,
 * but it costs a round trip per feature and still tells you nothing about
 * versions.
 *
 * A firmware without this service is simply "pre-capabilities": the app falls
 * back to showing everything and letting individual reads fail, which is what it
 * did before. So adding this never breaks an older keyboard.
 */

#pragma once

#include <zephyr/types.h>

#define TORABO_CAPS_MAGIC 0x4354 /* "TC" little-endian */
#define TORABO_CAPS_DESC_VERSION 1

/* Stable feature ids. NEVER renumber: the app matches on these. Append only. */
enum torabo_feature_id {
    TORABO_FEAT_TRACKBALL = 1,
    TORABO_FEAT_MACROS = 2,
    TORABO_FEAT_COMBOS = 3,
    TORABO_FEAT_TRACKPAD = 4,
    TORABO_FEAT_ENCODER = 5,
    TORABO_FEAT_LED = 6,
    TORABO_FEAT_RESERVED_LAYERS = 7,
    TORABO_FEAT_LIVE_FEED = 8,
    TORABO_FEAT_RPC_TUNNEL = 9,
    TORABO_FEAT_TIMING = 10,
    TORABO_FEAT_MODULES = 11,
};

/*
 * Per-feature `caps` bits. Meaning is feature-specific — it answers "which
 * variant of this feature", not "does it exist" (presence is the entry itself).
 */

/*
 * Shared "which physical half" numbering (PLAN phase 9), used by the header
 * `_rsv` byte (which half is CENTRAL — genuinely a 1-of-3 choice, a board has
 * exactly one central half or none declared). Deliberately restated rather
 * than shared via a common enum with zmk_trackpad_config/config.h's
 * TP_META_SIDE_ enum (same 0/1/2 meaning) so caps.h keeps no compile
 * dependency on the trackpad feature. 0 is the value an unset (default)
 * Kconfig int reports, so "unknown" is what every existing conf and every
 * pre-phase-9 firmware reports — the whole reason this addition is
 * byte-identical until a builder opts in.
 */
enum torabo_caps_side { TORABO_CAPS_SIDE_UNKNOWN = 0, TORABO_CAPS_SIDE_LEFT = 1, TORABO_CAPS_SIDE_RIGHT = 2 };

/* TORABO_FEAT_LED: which halves actually have an LED, and which half is central.
 * Mirrors LED_CAP_* in zmk_led_config/config.h. */
#define TORABO_CAPS_LED_LEFT 0x0001
#define TORABO_CAPS_LED_RIGHT 0x0002
#define TORABO_CAPS_LED_CENTRAL_IS_LEFT 0x0004

/* TORABO_FEAT_TRACKPAD: how many pads the wire carries. */
#define TORABO_CAPS_TP_DEVICE_MASK 0x000f
/* COAST = this firmware has the per-device inertial-scroll ("momentum") engine
 * and its wire carries the three coast bytes per device. Implied by wire v3, but
 * stated separately so the app can gate the UI on a capability rather than on a
 * version comparison. Bit 4 keeps clear of the device-count mask above. */
#define TORABO_CAPS_TP_COAST 0x0010

/* TORABO_FEAT_TRACKBALL: COAST = inertial scroll for the ball (wire v3 trailer).
 * Independent numbering per feature id, so 0x0001 here is unrelated to the
 * identically-numbered bit under TORABO_FEAT_TIMING below. This word carries
 * nothing about WHERE the ball sits — that question moved to the
 * TORABO_FEAT_MODULES row below (PLAN phase 9, re-redesigned 2026-09-03; two
 * earlier per-feature placement schemes lived here and are both gone — see
 * that row's comment for why). */
#define TORABO_CAPS_ZTC_COAST 0x0001

/* TORABO_FEAT_RESERVED_LAYERS: how many reserved layers were injected. */
#define TORABO_CAPS_LAYERS_MASK 0x00ff

/* TORABO_FEAT_LIVE_FEED: optional extras on the live-feed service.
 * DIAG = the diagnostic characteristic e1f4af02 is present (Torabo-Float §13). */
#define TORABO_CAPS_LIVE_FEED_DIAG 0x0001

/* TORABO_FEAT_RPC_TUNNEL: which tunnel ops this firmware answers. Every settings
 * feature listed above is also reachable through the tunnel, over whichever
 * transport Studio RPC selected — USB serial included — using the exact same wire
 * blob as its GATT service, addressed by the low byte of that service's UUID.
 * The app needs this to know it can talk to a USB-connected keyboard at all. */
#define TORABO_CAPS_TUNNEL_NOTIFY 0x0001 /* SUBSCRIBE/UNSUBSCRIBE + pushes work */

/* TORABO_FEAT_TIMING: SPLIT_DEBOUNCE = this central carries the debounce windows
 * across the split link, so they reach BOTH halves' key scanning rather than only
 * the central's. The wire is unchanged (still v1) — the same two bytes simply go
 * further — so this is a caps bit, not a version bump.
 *
 * It reports how the CENTRAL was built, which is all it can see; the peripheral
 * needs the matching torabo-timing-split snippet for the bytes to land. Both
 * halves come out of one build.yaml, so in practice they agree. */
#define TORABO_CAPS_TIMING_SPLIT_DEBOUNCE 0x0001

/*
 * TORABO_FEAT_MODULES: what is plugged into each of the four physical
 * connector slots (PLAN phase 9, re-redesigned 2026-09-03 — the second
 * redesign of this phase. The first shipped design (commit 6659672) reported
 * one 1-of-3 side/conn enum per feature; the second (35a8331) split that into
 * a per-feature bitmask spread across the ztc and enc caps words. Both were
 * scrapped: "one row that says what's in each of the 4 slots" is simpler than
 * either, and is exactly the 2x2 grid Torabo-Studio's module layout already
 * draws, so this row is now the ONLY place placement lives. The ztc/enc caps
 * words above carry no placement bits any more.
 *
 * Four bits per slot, four slots packed into one u16:
 *   bit0-3   = left standard connector
 *   bit4-7   = left extension connector
 *   bit8-11  = right standard connector
 *   bit12-15 = right extension connector
 *
 * Each 4-bit slot value is `enum torabo_caps_slot` below. These numbers are
 * INDEPENDENT of zmk_trackpad_config/config.h's `enum tp_meta_kind`
 * (TP_META_KIND_PAD=1/_BALL=2/_ENCODER=3): the two enumerations are different
 * things that happen to describe overlapping hardware, so an app that needs to
 * relate them keeps its own mapping table. Do not assume a shared numbering.
 * caps.h therefore keeps no compile dependency on the trackpad feature (same
 * reasoning as torabo_caps_side above). 0=undeclared is what an unset
 * (default 0) Kconfig int reports, so every pre-this-field firmware and every
 * conf that never adopts the new Kconfig lines reports all four slots
 * undeclared — the whole reason this row is byte-identical (a new, empty row)
 * until a builder opts in. 15=none is NOT the same as 0=undeclared: none is a
 * builder's explicit "this slot is empty" (e.g. no extension board at all),
 * distinct from an old firmware that never had an opinion. Come from
 * CONFIG_TORABO_SLOT_LEFT_STD / _LEFT_EXT / _RIGHT_STD / _RIGHT_EXT (int,
 * range 0-15, features/caps/Kconfig), masked and shifted into place.
 */
enum torabo_caps_slot {
    TORABO_CAPS_SLOT_UNDECLARED = 0, /* nothing declared (pre-this-field fw) */
    TORABO_CAPS_SLOT_BALL = 1,       /* trackball */
    TORABO_CAPS_SLOT_PAD = 2,        /* mini trackpad */
    TORABO_CAPS_SLOT_SWITCH4 = 3,    /* 4-way switch module (reserved; the
                                      * builder does not offer it yet) */
    TORABO_CAPS_SLOT_DIAL = 4,       /* high-resolution dial */
    TORABO_CAPS_SLOT_ENCODER = 9,    /* rotary encoder (a self-made module, so
                                      * it sits away from the others) */
    TORABO_CAPS_SLOT_NONE = 15,      /* explicitly empty */
};
#define TORABO_CAPS_MOD_SLOT_BITS 4
#define TORABO_CAPS_MOD_SLOT_MASK 0xF
#define TORABO_CAPS_MOD_LEFT_STD_SHIFT 0
#define TORABO_CAPS_MOD_LEFT_EXT_SHIFT 4
#define TORABO_CAPS_MOD_RIGHT_STD_SHIFT 8
#define TORABO_CAPS_MOD_RIGHT_EXT_SHIFT 12

/* Header `_rsv` byte, bit0-1: which physical half is the CENTRAL (torabo_caps_
 * side numbering) — independent of any feature, unlike the previous only way to
 * learn this (TORABO_CAPS_LED_CENTRAL_IS_LEFT, which doesn't exist in an
 * LED-less build). This is exactly the app-decoder contract's rule (c): new
 * header-level information goes in `_rsv`, never between the header and the
 * table (torabo-studio/src/caps/toraboCaps.ts:36-64). Comes from
 * CONFIG_TORABO_CENTRAL_SIDE; an unset conf (default 0) reports UNKNOWN, so
 * `_rsv` stays 0x00 exactly as it always has (PLAN phase 9). */
#define TORABO_CAPS_HDR_CENTRAL_SHIFT 0
#define TORABO_CAPS_HDR_CENTRAL_MASK 0x03

/* Header `_rsv` byte, bit2: this firmware understands the WINDOWED READ control
 * frame on every settings characteristic (torabo_common/window_read.h,
 * 2026-09-05). Header-level, not per-feature, because it is a property of the
 * GATT layer that every settings service shares — the same contract rule (c)
 * that put the central side in bit0-1.
 *
 * WHY AN APP NEEDS IT: Android's BluetoothGatt#readCharacteristic() runs the ATT
 * Read + Read Blob sequence itself and stops at 512 B, with no public API to go
 * further, so the macros READ wire (1964 B) and a fully populated trackpad wire
 * (~1.5 KB) are simply unreadable from Torabo-Key-App / Torabo-Studio-Android
 * against an older firmware. With this bit set the app may instead WRITE
 * [0xFF]['W'][offset u16 LE] and READ back [offset u16][total u16][data], at
 * most 512 B, repeating until it has `total` bytes. Clear (every firmware before
 * today) means the app must keep doing a plain whole-blob read and live with the
 * truncation. desc_ver stays 1 and the descriptor stays 52 B: an app that does
 * not know this bit ignores it, exactly as the contract requires. */
#define TORABO_CAPS_HDR_WINDOW_READ 0x04

/*
 * wire:
 *   header (8B): magic u16 | desc_ver u8 | fw_major u8 | fw_minor u8 |
 *                fw_patch u8 | feature_count u8 | _rsv u8
 *   per feature (4B): id u8 | wire_ver u8 | caps u16
 *
 * 8 + 32*4 = 136 B at most (32 slots, 11 used by the features above today —
 * TORABO_FEAT_MODULES, PLAN phase 9, is always present whenever CONFIG_TORABO_CAPS
 * is on, on top of whichever of the other 10 features the build compiles in).
 * The wire itself is still count-driven (feature_count), so the current
 * 11-feature build still encodes exactly 52 B — up from 48 B before phase 9
 * added this row (PLAN-ext-fw-refactor.md phase 6, B-4, raised the cap 10->16;
 * phase 9 raises it again 16->32 for headroom, purely a bigger static buffer —
 * desc_ver stays 1, and the count-driven wire is unaffected either way). 52 B
 * still fits one ATT read; a future build with a bigger feature_count grows
 * past that and needs ATT Read Long (Blob Read), same as any other blob that
 * outgrows one MTU.
 */
#define TORABO_CAPS_HDR 8
#define TORABO_CAPS_FEAT 4
#define TORABO_CAPS_MAX_FEATURES 32
#define TORABO_CAPS_WIRE_CAP (TORABO_CAPS_HDR + TORABO_CAPS_MAX_FEATURES * TORABO_CAPS_FEAT)

int torabo_caps_encode(uint8_t *buf, uint16_t cap, uint16_t *out_len);
