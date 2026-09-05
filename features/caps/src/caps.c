/*
 * Build the capability descriptor, and serve it over its own GATT service.
 *
 * The table is assembled purely from Kconfig, which is what the snippets set — so
 * it always describes the firmware that was actually built, with no chance of the
 * two drifting apart.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk_torabo_caps/caps.h>

LOG_MODULE_REGISTER(torabo_caps, CONFIG_TORABO_CAPS_LOG_LEVEL);

struct feat_entry {
    uint8_t id;
    uint8_t wire_ver;
    uint16_t caps;
};

static uint16_t led_caps_bits(void) {
    uint16_t c = 0;
#if IS_ENABLED(CONFIG_ZMK_LED_CONFIG_LEFT_PRESENT)
    c |= TORABO_CAPS_LED_LEFT;
#endif
#if IS_ENABLED(CONFIG_ZMK_LED_CONFIG_RIGHT_PRESENT)
    c |= TORABO_CAPS_LED_RIGHT;
#endif
#if IS_ENABLED(CONFIG_ZMK_LED_CONFIG_CENTRAL_IS_LEFT)
    c |= TORABO_CAPS_LED_CENTRAL_IS_LEFT;
#endif
    return c;
}

static uint16_t timing_caps_bits(void) {
    uint16_t c = 0;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_DEBOUNCE_SYNC)
    c |= TORABO_CAPS_TIMING_SPLIT_DEBOUNCE;
#endif
    return c;
}

/* PLAN phase 9 (re-redesigned 2026-09-03): one 4-bit slot value per physical
 * connector, packed left-std/left-ext/right-std/right-ext into a single u16 —
 * see TORABO_FEAT_MODULES's comment in caps.h for why placement now lives
 * ONLY here and not spread across the ztc/enc caps words (both earlier
 * per-feature schemes are gone). CONFIG_TORABO_SLOT_LEFT_STD / _LEFT_EXT /
 * _RIGHT_STD / _RIGHT_EXT are plain int Kconfigs (range 0-4, features/caps/
 * Kconfig) read unconditionally whenever CONFIG_TORABO_CAPS is on, same as
 * CONFIG_TORABO_CENTRAL_SIDE below. Each is masked to 4 bits so an
 * out-of-range conf (Kconfig `range` doesn't stop a hand-edited .config)
 * can't bleed into a neighboring slot. Default 0 (unset conf) on all four
 * leaves the whole word 0 = every slot undeclared, so this row is a new,
 * empty row on every pre-this-field build. */
static uint16_t modules_caps_bits(void) {
    uint16_t c = 0;
    c |= ((uint16_t)CONFIG_TORABO_SLOT_LEFT_STD & TORABO_CAPS_MOD_SLOT_MASK)
         << TORABO_CAPS_MOD_LEFT_STD_SHIFT;
    c |= ((uint16_t)CONFIG_TORABO_SLOT_LEFT_EXT & TORABO_CAPS_MOD_SLOT_MASK)
         << TORABO_CAPS_MOD_LEFT_EXT_SHIFT;
    c |= ((uint16_t)CONFIG_TORABO_SLOT_RIGHT_STD & TORABO_CAPS_MOD_SLOT_MASK)
         << TORABO_CAPS_MOD_RIGHT_STD_SHIFT;
    c |= ((uint16_t)CONFIG_TORABO_SLOT_RIGHT_EXT & TORABO_CAPS_MOD_SLOT_MASK)
         << TORABO_CAPS_MOD_RIGHT_EXT_SHIFT;
    return c;
}

/* Append one entry, dropping it (and logging) instead of overrunning `out` once
 * the table is full. `out` is always sized TORABO_CAPS_MAX_FEATURES by the only
 * caller (torabo_caps_encode's stack array); every #if block below adds at most
 * one entry, and the table uses 10 of its 16 slots today (raised from 10/10
 * full in PLAN phase 6, B-4), so this guard is currently slack — it only
 * matters the day a 17th feature is added without also raising
 * TORABO_CAPS_MAX_FEATURES again (PLAN phase 2 A-5 added the guard itself). */
static void add_feat(struct feat_entry *out, uint8_t *n, struct feat_entry entry) {
    if (*n >= TORABO_CAPS_MAX_FEATURES) {
        LOG_ERR("caps table full (%u); dropping feature id %u", TORABO_CAPS_MAX_FEATURES,
                entry.id);
        return;
    }
    out[(*n)++] = entry;
}

/* One entry per feature that is actually compiled into THIS build. The wire
 * version is bumped by whoever changes that feature's wire — the app compares it
 * against what it knows how to speak. */
static uint8_t build_features(struct feat_entry *out) {
    uint8_t n = 0;

#if IS_ENABLED(CONFIG_ZMK_TRACKBALL_CONFIG)
    /* wire v3 = the v2 layer array plus the 4B inertial-scroll trailer. (The
     * blob has carried version byte 2 since the v2 rework; the 1 reported here
     * before was stale, and is corrected along with the bump.) */
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_TRACKBALL, 3, TORABO_CAPS_ZTC_COAST});
#endif
#if IS_ENABLED(CONFIG_ZMK_DYNAMIC_KEYMAP)
    /* wire v2 (PLAN phase 8) = the v1 slot region with a per-slot NAME block
     * appended to the READ wire, plus a name-only WRITE op. Steps WRITE stays
     * v1 forever, so this bump only matters to a client deciding whether to
     * show/send names -- it does not change how steps are written. */
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_MACROS, 2, 0});
#endif
#if IS_ENABLED(CONFIG_ZMK_DYNAMIC_COMBOS)
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_COMBOS, 1, 0});
#endif
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_CONFIG)
    /* wire v3 = the v2 gesture/encoder-role wire with the per-device
     * inertial-scroll block added to each device header. */
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_TRACKPAD, 3, TORABO_CAPS_TP_COAST});
#endif
#if IS_ENABLED(CONFIG_ZMK_ENCODER_CONFIG)
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_ENCODER, 1, 0});
#endif
#if IS_ENABLED(CONFIG_ZMK_LED_CONFIG)
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_LED, 1, led_caps_bits()});
#endif
#if defined(CONFIG_TORABO_RESERVED_LAYERS) && (CONFIG_TORABO_RESERVED_LAYERS > 0)
    add_feat(out, &n,
             (struct feat_entry){TORABO_FEAT_RESERVED_LAYERS, 1,
                                 (uint16_t)(CONFIG_TORABO_RESERVED_LAYERS &
                                            TORABO_CAPS_LAYERS_MASK)});
#endif
#if IS_ENABLED(CONFIG_ZMK_LIVE_FEED)
    /* wire v1 = the packed live_feed_evt the app parses today (unchanged). The diag
     * char (e1f4af02) ships with the live feed and is advertised by a caps bit, not
     * a wire bump. */
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_LIVE_FEED, 1, TORABO_CAPS_LIVE_FEED_DIAG});
#endif
#if IS_ENABLED(CONFIG_ZMK_STUDIO_TORABO_TUNNEL)
    /* wire v1 = the (feature_id, op, blob) tunnel request. The per-feature blobs
     * are unchanged, so their own wire_ver above still governs what the app sends;
     * this entry only says "the tunnel exists", i.e. USB can reach all of them. */
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_RPC_TUNNEL, 1, TORABO_CAPS_TUNNEL_NOTIFY});
#endif
#if IS_ENABLED(CONFIG_ZMK_TIMING_CONFIG)
    /* wire v1 = the fixed 96 B hold-tap + debounce blob (DESIGN-timing.md). The
     * split propagation of the debounce windows moves the same two bytes further,
     * so it is a caps bit rather than a wire bump. */
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_TIMING, 1, timing_caps_bits()});
#endif
#if IS_ENABLED(CONFIG_TORABO_CAPS)
    /* wire v1 = a single u16 of four 4-bit slot values (see caps.h). Always
     * present whenever the caps descriptor itself is built, regardless of
     * which other features are compiled in — appended last (feature id 11)
     * so every byte of the 10 rows above is untouched by its addition (PLAN
     * phase 9, re-redesigned 2026-09-03). */
    add_feat(out, &n, (struct feat_entry){TORABO_FEAT_MODULES, 1, modules_caps_bits()});
#endif

    return n;
}

int torabo_caps_encode(uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    struct feat_entry feats[TORABO_CAPS_MAX_FEATURES];
    const uint8_t n = build_features(feats);
    const uint16_t need = (uint16_t)(TORABO_CAPS_HDR + n * TORABO_CAPS_FEAT);

    if (!buf || cap < need) {
        return -ENOMEM;
    }
    memset(buf, 0, need);

    sys_put_le16(TORABO_CAPS_MAGIC, &buf[0]);
    buf[2] = TORABO_CAPS_DESC_VERSION;
    buf[3] = CONFIG_TORABO_FW_VERSION_MAJOR;
    buf[4] = CONFIG_TORABO_FW_VERSION_MINOR;
    buf[5] = CONFIG_TORABO_FW_VERSION_PATCH;
    buf[6] = n;
    /* PLAN phase 9: bit0-1 = which half is the CENTRAL (torabo_caps_side
     * numbering), independent of any feature. CONFIG_TORABO_CENTRAL_SIDE is an
     * int Kconfig (0/1/2) declared unconditionally alongside TORABO_FW_VERSION_*
     * above (features/caps/Kconfig) — always defined whenever CONFIG_TORABO_CAPS
     * is on, just like those. Default 0 (unset conf) => UNKNOWN, so this byte
     * stays 0x00 exactly as before this field existed. */
    buf[7] = ((uint8_t)CONFIG_TORABO_CENTRAL_SIDE << TORABO_CAPS_HDR_CENTRAL_SHIFT) &
             TORABO_CAPS_HDR_CENTRAL_MASK;

    uint32_t o = TORABO_CAPS_HDR;
    for (uint8_t i = 0; i < n; i++) {
        buf[o] = feats[i].id;
        buf[o + 1] = feats[i].wire_ver;
        sys_put_le16(feats[i].caps, &buf[o + 2]);
        o += TORABO_CAPS_FEAT;
    }

    if (out_len) {
        *out_len = need;
    }
    return 0;
}

/* ---- GATT ----------------------------------------------------------------
 * Read-only: this describes the build, so there is nothing for the app to write.
 * UUID e1f4a000, ahead of the per-feature services (trackball e1f4a900, macros
 * e1f4aa00, combos e1f4ab00, trackpad e1f4ac00, encoder e1f4ad00, led e1f4ae00,
 * live_feed e1f4af00, timing e1f4b000). */

#if IS_ENABLED(CONFIG_TORABO_CAPS_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#define CAPS_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4a000, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define CAPS_BT_UUID_VAL BT_UUID_128_ENCODE(0xe1f4a001, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 caps_svc_uuid = BT_UUID_INIT_128(CAPS_BT_UUID_SVC);
static struct bt_uuid_128 caps_val_uuid = BT_UUID_INIT_128(CAPS_BT_UUID_VAL);

static ssize_t caps_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                         uint16_t len, uint16_t offset) {
    static uint8_t wire[TORABO_CAPS_WIRE_CAP];
    uint16_t wlen = 0;
    if (torabo_caps_encode(wire, sizeof(wire), &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(torabo_caps_svc,
    BT_GATT_PRIMARY_SERVICE(&caps_svc_uuid),
    BT_GATT_CHARACTERISTIC(&caps_val_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT,
                           caps_read, NULL, NULL),
);
/* clang-format on */

#endif /* CONFIG_TORABO_CAPS_BLE */
