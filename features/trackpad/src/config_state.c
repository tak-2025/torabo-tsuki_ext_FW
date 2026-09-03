/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Double-buffered, validated RAM store for the trackpad settings (v2).
 * docs/DESIGN-trackpad-v2.md §3 (wire) / §4 (FW). Wire is parsed via explicit
 * byte offsets (no packed-struct unaligned access on Cortex-M).
 *
 * Wire layout (LE, fixed header THEN device blocks — matches tpConfigV2.ts):
 *   hdr(6): magic[2]=0x7470 version device_count layer_count flags
 *     flags bit0 = gesture section present, bit1 = coast block present (v3)
 *   per device: device_id meta (2B, v1/v2)
 *               device_id meta coast_enable coast_friction coast_threshold
 *                              (5B, v3)
 *               then layer_count layers:
 *     axis x { role dir step pos(4) neg(4) }   (11B)
 *     axis y { same }                          (11B)
 *     gesture { tap(4) tap2(4) hold(4) dtap(4) } (16B, only if flags bit0)
 *   binding(4) = behavior mods param(2 LE)
 *   total(v2) = 6 + device_count*(2 + layer_count*(22 [+16 if gestures]))
 *   total(v3) = 6 + device_count*(5 + layer_count*(22 [+16 if gestures]))
 *
 * WRITE accepts version 1 (old fixed-role wire, upgraded to ENCODER + preset
 * pos/neg), version 2 and version 3; READ always emits version 3 with both the
 * gesture section and the coast block. A v1/v2 write leaves coasting DISABLED,
 * so an app that has not learned v3 yet cannot silently turn it on.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk_trackpad_config/config.h>

LOG_MODULE_REGISTER(tp_config, CONFIG_ZMK_TRACKPAD_CONFIG_LOG_LEVEL);

#define TP_WIRE_VERSION_V1 1u
#define TP_WIRE_VERSION_V2 2u
#define TP_WIRE_VERSION_V3 3u

/* HID usage ids / mods for the v1->v2 preset upgrade (must match tpConfigV2.ts
 * presetForV1Role). Raw usage ids; the page is applied when the binding is
 * synthesised at runtime (binding.h). */
#define TP_KC_MINUS 0x2du
#define TP_KC_EQUAL 0x2eu
#define TP_C_VOL_UP 0xe9u
#define TP_C_VOL_DN 0xeau
#define TP_C_BRI_UP 0x6fu
#define TP_C_BRI_DN 0x70u
#define TP_AC_FORWARD 0x225u
#define TP_AC_BACK 0x224u
#define TP_MOD_LCTL 0x01u

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

uint16_t tp_wire_len_for(uint8_t device_count, uint8_t layer_count) {
    /* v3 length WITH the gesture section and the coast block (what we encode). */
    return (uint16_t)(TP_WIRE_HDR +
                      (uint32_t)device_count *
                          (TP_WIRE_DEV_HDR_V3 + (uint32_t)layer_count * TP_WIRE_LAYER_V2));
}

/* Total wire length a blob starting with this header claims, or 0 if the header
 * is not a plausible start of one (bad magic / unknown version / device or
 * layer count out of range). This is the SOLE place that turns a header's
 * shape into a byte length: tp_apply_wire()'s exact-length check below calls it
 * directly on the same buffer it is validating, and the GATT write assembler
 * (gatt_service.c) calls it for chunk framing on a possibly-incomplete header.
 * Both therefore always agree on where a wire ends. */
uint16_t tp_expected_len(const uint8_t *hdr) {
    if (!hdr || (uint16_t)(hdr[0] | (hdr[1] << 8)) != TP_WIRE_MAGIC) {
        return 0;
    }
    uint8_t version = hdr[2];
    uint8_t device_count = hdr[3];
    uint8_t layer_count = hdr[4];
    uint8_t flags = hdr[5];
    if (device_count > TP_MAX_DEVICES || layer_count > TP_MAX_LAYERS) {
        return 0;
    }
    uint32_t stride;
    uint32_t dev_hdr = TP_WIRE_DEV_HDR;
    if (version == 3u) {
        /* v3 = v2 layers with a 5B device header (coast block). */
        stride = TP_WIRE_AXIS * 2u + ((flags & TP_FLAG_GESTURES) ? TP_WIRE_GEST : 0u);
        dev_hdr = TP_WIRE_DEV_HDR_V3;
    } else if (version == 2u) {
        stride = TP_WIRE_AXIS * 2u + ((flags & TP_FLAG_GESTURES) ? TP_WIRE_GEST : 0u);
    } else if (version == 1u) {
        stride = TP_WIRE_LAYER_V1;
    } else {
        return 0; /* unknown version: not stageable */
    }
    return (uint16_t)(TP_WIRE_HDR +
                      (uint32_t)device_count * (dev_hdr + (uint32_t)layer_count * stride));
}

/* ---- defaults == current fixed torabo-trackpad overlay behavior ----------- */

/* Compound-literal init zero-fills the pos/neg bindings (=> TP_BEH_NONE). */
#define AX(r, d, s) {.role = (r), .direction = (d), .step = (s)}
#define MOVE_INV AX(TP_ROLE_MOVE, 1, 1)
#define MOVE_PT AX(TP_ROLE_MOVE, 0, 1)

/* Device identity is FW-authoritative: it comes from Kconfig (emitted per
 * hardware pattern by the firmware builder), never from the wire. So a config
 * written by the app can't corrupt it — deserialize calls this too. */
static uint8_t meta_for(uint8_t device_id) {
    switch (device_id) {
    case 0:
        return (uint8_t)CONFIG_ZMK_TRACKPAD_CONFIG_DEV0_META;
    case 1:
        return (uint8_t)CONFIG_ZMK_TRACKPAD_CONFIG_DEV1_META;
    case 2:
        return (uint8_t)CONFIG_ZMK_TRACKPAD_CONFIG_DEV2_META;
    case 3:
        return (uint8_t)CONFIG_ZMK_TRACKPAD_CONFIG_DEV3_META;
    default:
        return 0; /* unknown => app falls back to a generic label */
    }
}

static void fill_device_defaults(struct tp_device_cfg *dev, uint8_t device_id) {
    dev->device_id = device_id;
    dev->meta = meta_for(device_id);
    /* Coasting is OPT-IN: the shipped default is the exact pre-v3 behavior, so a
     * firmware update never changes how anyone's pad feels until they ask. */
    dev->coast.enable = 0;
    dev->coast.friction = (uint8_t)TP_COAST_FRICTION_DEFAULT;
    dev->coast.threshold = (uint8_t)TP_COAST_THRESHOLD_DEFAULT;
    /* every layer defaults to plain move passthrough (safe); gestures stay NONE */
    for (int i = 0; i < TP_MAX_LAYERS; i++) {
        dev->layers[i].x = (struct tp_axis_cfg)MOVE_PT;
        dev->layers[i].y = (struct tp_axis_cfg)MOVE_PT;
        dev->layers[i].gestures = (struct tp_gestures){0};
    }
    /* 0,1 = move + invert XY (mouse_mode / tp_move) */
    for (int i = 0; i <= 1 && i < TP_MAX_LAYERS; i++) {
        dev->layers[i].x = (struct tp_axis_cfg)MOVE_INV;
        dev->layers[i].y = (struct tp_axis_cfg)MOVE_INV;
    }
    /* 2 = horizontal scroll: X scroll(inv,1/8), Y off */
    if (TP_MAX_LAYERS > 2) {
        dev->layers[2].x = (struct tp_axis_cfg)AX(TP_ROLE_SCROLL, 1, 8);
        dev->layers[2].y = (struct tp_axis_cfg)AX(TP_ROLE_OFF, 0, 1);
    }
    /* 3 = vertical scroll: Y scroll(normal,1/8), X off */
    if (TP_MAX_LAYERS > 3) {
        dev->layers[3].x = (struct tp_axis_cfg)AX(TP_ROLE_OFF, 0, 1);
        dev->layers[3].y = (struct tp_axis_cfg)AX(TP_ROLE_SCROLL, 0, 8);
    }
}

static void fill_defaults(struct tp_snapshot *s) {
    memset(s, 0, sizeof(*s));
    s->has_gestures = true; /* we always support/expose the gesture section */
    /* Expose two devices (wire order): left pad (id 0) + right extender pad (id 1). */
    s->device_count = TP_DEFAULT_DEVICE_COUNT;
    fill_device_defaults(&s->devices[0], TP_DEVICE_LEFT_PAD);
    fill_device_defaults(&s->devices[1], TP_DEVICE_RIGHT_EXT_PAD);
    for (int d = TP_DEFAULT_DEVICE_COUNT; d < TP_MAX_DEVICES; d++) {
        fill_device_defaults(&s->devices[d], (uint8_t)d);
    }
}

/* ---- double buffer ------------------------------------------------------- */

static struct tp_snapshot snap[2];
static atomic_t live_idx = ATOMIC_INIT(0);
static atomic_t initialized = ATOMIC_INIT(0);

/* Shadow snapshot for the wire->store apply path. STATIC, NOT stack: with the
 * v2 layout (11B axes + 16B gestures x TP_MAX_LAYERS x 4 devices) this struct
 * is multiple KB — far bigger than the BT RX thread stack (~2.2KB) that runs
 * the GATT write callback. Putting it on that stack hard-faults the whole
 * keyboard on the first WRITE (found on-device 2026-07-11). Serialization is
 * guaranteed by the callers: GATT writes are serialized on the BT RX thread,
 * and the settings loader runs once at boot before BLE is connectable. */
static struct tp_snapshot apply_shadow;

static void ensure_init(void) {
    if (atomic_cas(&initialized, 0, 1)) {
        fill_defaults(&snap[0]);
        fill_defaults(&snap[1]);
    }
}

const struct tp_snapshot *tp_live(void) {
    ensure_init();
    return &snap[atomic_get(&live_idx)];
}

/* publish a fully-built snapshot via single atomic index swap */
static void publish(const struct tp_snapshot *built) {
    ensure_init();
    int next = 1 - (int)atomic_get(&live_idx);
    snap[next] = *built;       /* completes before the index swap below */
    atomic_set(&live_idx, next);
}

/* ---- clamping helpers ---------------------------------------------------- */

static uint8_t clamp_role(uint8_t r) {
    return (r <= TP_ROLE_MAX) ? r : (uint8_t)TP_ROLE_MOVE; /* unknown => MOVE */
}
static uint8_t clamp_step(uint8_t s) {
    if (s < TP_STEP_MIN) {
        return (uint8_t)TP_STEP_MIN;
    }
    return (s > TP_STEP_MAX) ? (uint8_t)TP_STEP_MAX : s;
}
static uint8_t clamp_friction(uint8_t f) {
    if (f < TP_COAST_FRICTION_MIN) {
        return (uint8_t)TP_COAST_FRICTION_DEFAULT; /* 0 = "unset" => default, never 0 decay */
    }
    return (f > TP_COAST_FRICTION_MAX) ? (uint8_t)TP_COAST_FRICTION_MAX : f;
}
static uint8_t clamp_threshold(uint8_t t) {
    /* Max is the u8 range, so only the "unset" bottom needs a decision. */
    return (t < TP_COAST_THRESHOLD_MIN) ? (uint8_t)TP_COAST_THRESHOLD_DEFAULT : t;
}

/* v3 device-header tail: coast_enable coast_friction coast_threshold. */
static void decode_coast(const uint8_t *p, struct tp_coast_cfg *c) {
    c->enable = p[0] ? 1 : 0;
    c->friction = clamp_friction(p[1]);
    c->threshold = clamp_threshold(p[2]);
}

static void decode_bind(const uint8_t *p, struct tp_binding *b) {
    uint8_t beh = p[0];
    b->behavior = (beh <= TP_BEH_MAX) ? beh : (uint8_t)TP_BEH_NONE; /* unknown => NONE */
    b->mods = p[1];
    b->param = rd16(&p[2]);
}

static void decode_axis_v2(const uint8_t *p, struct tp_axis_cfg *a) {
    a->role = clamp_role(p[0]);
    a->direction = p[1] ? 1 : 0;
    a->step = clamp_step(p[2]);
    decode_bind(&p[3], &a->pos);
    decode_bind(&p[7], &a->neg);
}

/* v1 -> v2 preset (mirror tpConfigV2.ts presetForV1Role). */
static void preset_for_v1_role(uint8_t role, struct tp_binding *pos, struct tp_binding *neg) {
#define BIND(b, m, prm) (struct tp_binding){.behavior = (b), .mods = (m), .param = (prm)}
    switch (role) {
    case 3: /* Volume */
        *pos = BIND(TP_BEH_CP, 0, TP_C_VOL_UP);
        *neg = BIND(TP_BEH_CP, 0, TP_C_VOL_DN);
        break;
    case 4: /* Brightness */
        *pos = BIND(TP_BEH_CP, 0, TP_C_BRI_UP);
        *neg = BIND(TP_BEH_CP, 0, TP_C_BRI_DN);
        break;
    case 5: /* Zoom => Ctrl+= / Ctrl+- */
        *pos = BIND(TP_BEH_KP, TP_MOD_LCTL, TP_KC_EQUAL);
        *neg = BIND(TP_BEH_KP, TP_MOD_LCTL, TP_KC_MINUS);
        break;
    case 6: /* Browser fwd/back */
        *pos = BIND(TP_BEH_CP, 0, TP_AC_FORWARD);
        *neg = BIND(TP_BEH_CP, 0, TP_AC_BACK);
        break;
    default:
        *pos = (struct tp_binding){0};
        *neg = (struct tp_binding){0};
        break;
    }
#undef BIND
}

static void decode_axis_v1(const uint8_t *p, struct tp_axis_cfg *a) {
    uint8_t raw = p[0];
    a->direction = p[1] ? 1 : 0;
    a->step = clamp_step(p[2]);
    if (raw >= 3) {
        /* discrete v1 role => ENCODER + preset pair (default NONE for unknown) */
        a->role = TP_ROLE_ENCODER;
        preset_for_v1_role(raw, &a->pos, &a->neg);
    } else {
        a->role = clamp_role(raw); /* 0..2 continuous */
        a->pos = (struct tp_binding){0};
        a->neg = (struct tp_binding){0};
    }
}

/* ---- wire -> shadow (validate+clamp), then publish ----------------------- */

/* v2 and v3 differ only in the device header (v3 carries the coast block), so one
 * decoder serves both: `has_coast` says whether to read it, and the defaults it
 * would otherwise keep are "disabled". */
static int apply_v2_v3(const uint8_t *buf, uint16_t len, uint8_t device_count, uint8_t layer_count,
                       uint8_t flags, bool has_coast) {
    bool has_gest = (flags & TP_FLAG_GESTURES) != 0;
    const uint8_t dev_hdr = has_coast ? (uint8_t)TP_WIRE_DEV_HDR_V3 : (uint8_t)TP_WIRE_DEV_HDR;
    uint16_t want = tp_expected_len(buf); /* single source of truth (see above) */
    if (len != want) {
        LOG_WRN("tp wire v%d bad len %u (want %u)", has_coast ? 3 : 2, len, want);
        return -EINVAL;
    }

    struct tp_snapshot *sh = &apply_shadow; /* static: too big for the RX stack */
    fill_defaults(sh); /* unspecified devices/layers keep safe defaults */
    sh->device_count = device_count;

    uint32_t o = TP_WIRE_HDR;
    for (uint8_t d = 0; d < device_count; d++) {
        uint8_t dev_id = buf[o];
        /* rebaseline this device's layers to defaults for its (possibly new) id,
         * so layers beyond layer_count stay safe. Do it BEFORE decoding coast so
         * the decoded values are not overwritten by the defaults. */
        fill_device_defaults(&sh->devices[d], dev_id);
        if (has_coast) {
            decode_coast(&buf[o + 2], &sh->devices[d].coast);
        }
        o += dev_hdr;
        for (uint8_t i = 0; i < layer_count; i++) {
            struct tp_layer_cfg *l = &sh->devices[d].layers[i];
            decode_axis_v2(&buf[o], &l->x);
            decode_axis_v2(&buf[o + TP_WIRE_AXIS], &l->y);
            o += TP_WIRE_AXIS * 2u;
            if (has_gest) {
                decode_bind(&buf[o], &l->gestures.tap);
                decode_bind(&buf[o + TP_WIRE_BIND], &l->gestures.tap2);
                decode_bind(&buf[o + TP_WIRE_BIND * 2u], &l->gestures.hold);
                decode_bind(&buf[o + TP_WIRE_BIND * 3u], &l->gestures.dtap);
                o += TP_WIRE_GEST;
            }
        }
    }

    publish(sh);
    LOG_INF("tp config applied v%d (live): %u dev, %u layer, gestures=%d", has_coast ? 3 : 2,
            device_count, layer_count, (int)has_gest);
    return 0;
}

static int apply_v1(const uint8_t *buf, uint16_t len, uint8_t device_count, uint8_t layer_count) {
    uint16_t want = tp_expected_len(buf); /* single source of truth (see above) */
    if (len != want) {
        LOG_WRN("tp wire v1 bad len %u (want %u)", len, want);
        return -EINVAL;
    }

    struct tp_snapshot *sh = &apply_shadow; /* static: too big for the RX stack */
    fill_defaults(sh);
    sh->device_count = device_count;

    uint32_t o = TP_WIRE_HDR;
    for (uint8_t d = 0; d < device_count; d++) {
        uint8_t dev_id = buf[o];
        o += TP_WIRE_DEV_HDR;
        fill_device_defaults(&sh->devices[d], dev_id);
        for (uint8_t i = 0; i < layer_count; i++) {
            struct tp_layer_cfg *l = &sh->devices[d].layers[i];
            decode_axis_v1(&buf[o], &l->x);
            decode_axis_v1(&buf[o + TP_WIRE_AXIS_V1], &l->y);
            o += TP_WIRE_LAYER_V1;
            /* v1 carried no gestures; leave them NONE (fail-open passthrough). */
        }
    }

    publish(sh);
    LOG_INF("tp config applied v1->v2 upgrade (live): %u dev, %u layer", device_count, layer_count);
    return 0;
}

int tp_apply_wire(const uint8_t *buf, uint16_t len) {
    if (!buf || len < TP_WIRE_HDR) {
        LOG_WRN("tp wire too short %u", len);
        return -EINVAL;
    }
    if (rd16(&buf[0]) != TP_WIRE_MAGIC) {
        LOG_WRN("tp wire bad magic");
        return -EINVAL;
    }
    uint8_t version = buf[2];
    uint8_t device_count = buf[3];
    uint8_t layer_count = buf[4];
    uint8_t flags = buf[5];
    if (device_count > TP_MAX_DEVICES || layer_count > TP_MAX_LAYERS) {
        LOG_WRN("tp wire dc %u/lc %u out of range", device_count, layer_count);
        return -EINVAL;
    }

    /* docs/BACKLOG.md B-1 guard (refactor phase 5). A WRITE is short — it may be
     * v1, or carry fewer layers — but the READ it turns into is always full v3 at
     * TP_MAX_LAYERS. Accepting a device_count whose READ no longer fits the
     * tunnel's blob budget would leave READ permanently in ERROR (the tunnel
     * refuses rather than truncates), and the config is persisted, so a power
     * cycle would not clear it. Refuse the write instead: nothing is applied, the
     * previous config stays live, and the settings window keeps working.
     *
     * Compiled out entirely when this build has no trackpad tunnel window — with
     * no tunnel there is no blob budget to overrun, and the GATT path streams
     * with Read Blob, so any length is readable. The Kconfig default is 2048. */
#if defined(CONFIG_ZMK_TRACKPAD_CONFIG_TUNNEL) &&                                                  \
    defined(CONFIG_ZMK_STUDIO_TORABO_TUNNEL_BLOB_MAX_SIZE)
    if (!tp_read_fits(device_count, (uint16_t)CONFIG_ZMK_STUDIO_TORABO_TUNNEL_BLOB_MAX_SIZE)) {
        LOG_WRN("tp wire dc %u would READ back as %u B > tunnel blob cap %u; rejected",
                device_count, tp_wire_len_for(device_count, TP_MAX_LAYERS),
                (unsigned)CONFIG_ZMK_STUDIO_TORABO_TUNNEL_BLOB_MAX_SIZE);
        return -EINVAL;
    }
#endif

    if (version == TP_WIRE_VERSION_V3) {
        return apply_v2_v3(buf, len, device_count, layer_count, flags, true);
    }
    if (version == TP_WIRE_VERSION_V2) {
        return apply_v2_v3(buf, len, device_count, layer_count, flags, false);
    }
    if (version == TP_WIRE_VERSION_V1) {
        return apply_v1(buf, len, device_count, layer_count);
    }
    LOG_WRN("tp wire bad version %u", version);
    return -EINVAL;
}

static void encode_bind(uint8_t *p, const struct tp_binding *b) {
    p[0] = b->behavior;
    p[1] = b->mods;
    wr16(&p[2], b->param);
}

static void encode_axis(uint8_t *p, const struct tp_axis_cfg *a) {
    p[0] = a->role;
    p[1] = a->direction;
    p[2] = a->step;
    encode_bind(&p[3], &a->pos);
    encode_bind(&p[7], &a->neg);
}

int tp_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    const struct tp_snapshot *s = tp_live();
    uint8_t device_count = (s->device_count <= TP_MAX_DEVICES) ? s->device_count : TP_MAX_DEVICES;
    uint16_t need = tp_wire_len_for(device_count, TP_MAX_LAYERS); /* v3: gestures + coast */
    if (!buf || cap < need) {
        return -ENOMEM;
    }
    memset(buf, 0, need);
    wr16(&buf[0], TP_WIRE_MAGIC);
    buf[2] = TP_WIRE_VERSION_V3;
    buf[3] = device_count;
    buf[4] = TP_MAX_LAYERS;
    buf[5] = TP_FLAG_GESTURES | TP_FLAG_COAST;

    uint32_t o = TP_WIRE_HDR;
    for (uint8_t d = 0; d < device_count; d++) {
        buf[o] = s->devices[d].device_id;
        buf[o + 1] = s->devices[d].meta; /* formerly _rsv; identity for the app */
        buf[o + 2] = s->devices[d].coast.enable;
        buf[o + 3] = s->devices[d].coast.friction;
        buf[o + 4] = s->devices[d].coast.threshold;
        o += TP_WIRE_DEV_HDR_V3;
        for (uint8_t i = 0; i < TP_MAX_LAYERS; i++) {
            const struct tp_layer_cfg *l = &s->devices[d].layers[i];
            encode_axis(&buf[o], &l->x);
            encode_axis(&buf[o + TP_WIRE_AXIS], &l->y);
            o += TP_WIRE_AXIS * 2u;
            encode_bind(&buf[o], &l->gestures.tap);
            encode_bind(&buf[o + TP_WIRE_BIND], &l->gestures.tap2);
            encode_bind(&buf[o + TP_WIRE_BIND * 2u], &l->gestures.hold);
            encode_bind(&buf[o + TP_WIRE_BIND * 3u], &l->gestures.dtap);
            o += TP_WIRE_GEST;
        }
    }
    if (out_len) {
        *out_len = need;
    }
    return 0;
}

/* ---- NVS persistence (validate on load too) ------------------------------ */

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>

#define TP_KEY "tp"
#define TP_VAL "wire"

int tp_save(void) {
    /* Static, NOT stack: TP_WIRE_CAP is multiple KB in v2 and this runs on the
     * BT RX thread right after a GATT write (see apply_shadow comment). */
    static uint8_t buf[TP_WIRE_CAP];
    uint16_t len = 0;
    int rc = tp_encode_wire(buf, sizeof(buf), &len);
    if (rc) {
        return rc;
    }
    rc = settings_save_one(TP_KEY "/" TP_VAL, buf, len);
    LOG_INF("tp save: %d", rc);
    return rc;
}

static int tp_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, TP_VAL, &next) || next) {
        return -ENOENT;
    }
    /* Static, NOT stack: multiple KB; runs once on the settings thread at boot. */
    static uint8_t buf[TP_WIRE_CAP];
    if (len > sizeof(buf)) {
        /* Larger than any wire we can hold (shape grew beyond this build). Ignore
         * and keep the new defaults rather than truncate. */
        LOG_WRN("tp nvs size %u > cap %u; keeping defaults", (unsigned)len, (unsigned)sizeof(buf));
        return 0;
    }
    ssize_t r = read_cb(cb_arg, buf, sizeof(buf));
    if (r < 0) {
        return (int)r;
    }
    /* re-validate on load (NVS can bit-rot / layer count can change; accepts v1
     * or v2); reject => keep defaults. */
    if (tp_apply_wire(buf, (uint16_t)r) == 0) {
        LOG_INF("tp loaded from NVS");
    } else {
        LOG_WRN("tp NVS blob rejected; keeping defaults");
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tp, TP_KEY, NULL, tp_settings_set, NULL, NULL);

#else
int tp_save(void) { return 0; }
#endif

/* compile-time wire layout guarantees (DESIGN-trackpad-v2.md §3) */
BUILD_ASSERT(TP_WIRE_HDR == 6, "wire header must be 6 bytes");
BUILD_ASSERT(TP_WIRE_DEV_HDR == 2, "device header must be 2 bytes");
BUILD_ASSERT(TP_WIRE_DEV_HDR_V3 == 5, "v3 device header must be 5 bytes (2 + coast 3)");
BUILD_ASSERT(TP_WIRE_BIND == 4, "binding must be 4 bytes");
BUILD_ASSERT(TP_WIRE_AXIS == 11, "wire axis must be 11 bytes");
BUILD_ASSERT(TP_WIRE_GEST == 16, "wire gesture must be 16 bytes");
BUILD_ASSERT(TP_WIRE_LAYER_V2 == 38, "wire v2 layer must be 38 bytes");
BUILD_ASSERT(TP_WIRE_LAYER_V1 == 6, "wire v1 layer must be 6 bytes");
