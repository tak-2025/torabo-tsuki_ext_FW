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
};

/*
 * Per-feature `caps` bits. Meaning is feature-specific — it answers "which
 * variant of this feature", not "does it exist" (presence is the entry itself).
 */
/* TORABO_FEAT_LED: which halves actually have an LED, and which half is central.
 * Mirrors LED_CAP_* in zmk_led_config/config.h. */
#define TORABO_CAPS_LED_LEFT 0x0001
#define TORABO_CAPS_LED_RIGHT 0x0002
#define TORABO_CAPS_LED_CENTRAL_IS_LEFT 0x0004

/* TORABO_FEAT_TRACKPAD: how many pads the wire carries. */
#define TORABO_CAPS_TP_DEVICE_MASK 0x000f

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

/*
 * wire:
 *   header (8B): magic u16 | desc_ver u8 | fw_major u8 | fw_minor u8 |
 *                fw_patch u8 | feature_count u8 | _rsv u8
 *   per feature (4B): id u8 | wire_ver u8 | caps u16
 *
 * 8 + 9*4 = 44 B at most — a single read.
 */
#define TORABO_CAPS_HDR 8
#define TORABO_CAPS_FEAT 4
#define TORABO_CAPS_MAX_FEATURES 9
#define TORABO_CAPS_WIRE_CAP (TORABO_CAPS_HDR + TORABO_CAPS_MAX_FEATURES * TORABO_CAPS_FEAT)

int torabo_caps_encode(uint8_t *buf, uint16_t cap, uint16_t *out_len);
