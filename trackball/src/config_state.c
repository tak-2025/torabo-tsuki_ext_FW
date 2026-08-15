/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Double-buffered, validated RAM store for the trackball settings (v2).
 * docs/DESIGN_v2.md §11.D (publish) / §11.E (validate). Wire is parsed via
 * explicit byte offsets (no packed-struct unaligned access on Cortex-M).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk_trackball_config/config.h>

LOG_MODULE_REGISTER(ztc_config, CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL);

/* ---- wire layout (LE, fixed header THEN variable layers[]) ---------------- */
/* hdr(8): magic[2] version layer_count temp_target _rsv timeout[2]
 * layer(12): x{role dir speed rsv} y{role dir speed rsv} temp_enable rsv[3]  */
#define ZTC_WIRE_MAGIC 0x7A74u
#define ZTC_WIRE_VERSION 2u
#define ZTC_WIRE_HDR 8u
#define ZTC_WIRE_LAYER 12u

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

uint16_t ztc_wire_len(void) { return ZTC_WIRE_HDR + (uint16_t)ZTC_MAX_LAYERS * ZTC_WIRE_LAYER; }

/* ---- defaults == current static overlay behavior (fail-open baseline) ----- */

#define AX(r, d, s) {.role = (r), .direction = (d), .speed_div = (s)}
#define MOVE_INV AX(ZTC_ROLE_MOVE, 1, 1)
#define MOVE_PT AX(ZTC_ROLE_MOVE, 0, 1)

static void fill_defaults(struct ztc_snapshot *s) {
    /* every layer defaults to plain move passthrough (safe) */
    for (int i = 0; i < ZTC_MAX_LAYERS; i++) {
        s->layers[i].x = (struct ztc_axis_cfg)MOVE_PT;
        s->layers[i].y = (struct ztc_axis_cfg)MOVE_PT;
        s->layers[i].temp_enable = false;
    }
    /* 0,1 = move + invert XY + temp-layer (mouse_mode) */
    for (int i = 0; i <= 1 && i < ZTC_MAX_LAYERS; i++) {
        s->layers[i].x = (struct ztc_axis_cfg)MOVE_INV;
        s->layers[i].y = (struct ztc_axis_cfg)MOVE_INV;
        s->layers[i].temp_enable = true;
    }
    /* 2 = horizontal scroll: X scroll(inv,1/8), Y off */
    if (ZTC_MAX_LAYERS > 2) {
        s->layers[2].x = (struct ztc_axis_cfg)AX(ZTC_ROLE_SCROLL, 1, 8);
        s->layers[2].y = (struct ztc_axis_cfg)AX(ZTC_ROLE_OFF, 0, 1);
        s->layers[2].temp_enable = false;
    }
    /* 3 = vertical scroll: Y scroll(normal,1/8), X off */
    if (ZTC_MAX_LAYERS > 3) {
        s->layers[3].x = (struct ztc_axis_cfg)AX(ZTC_ROLE_OFF, 0, 1);
        s->layers[3].y = (struct ztc_axis_cfg)AX(ZTC_ROLE_SCROLL, 0, 8);
        s->layers[3].temp_enable = false;
    }
    s->temp_target = (ZTC_MAX_LAYERS > 1) ? 1 : 0;
    s->temp_timeout_ms = 500;
}

/* ---- double buffer ------------------------------------------------------- */

static struct ztc_snapshot snap[2];
static atomic_t live_idx = ATOMIC_INIT(0);
static atomic_t initialized = ATOMIC_INIT(0);

static void ensure_init(void) {
    if (atomic_cas(&initialized, 0, 1)) {
        fill_defaults(&snap[0]);
        fill_defaults(&snap[1]);
    }
}

const struct ztc_snapshot *ztc_live(void) {
    ensure_init();
    return &snap[atomic_get(&live_idx)];
}

/* publish a fully-built snapshot via single atomic index swap */
static void publish(const struct ztc_snapshot *built) {
    ensure_init();
    int next = 1 - (int)atomic_get(&live_idx);
    snap[next] = *built;       /* completes before the index swap below */
    atomic_set(&live_idx, next);
}

/* ---- clamping helpers ---------------------------------------------------- */

static uint8_t clamp_role(uint8_t r) {
    return (r <= ZTC_ROLE_OFF) ? r : (uint8_t)ZTC_ROLE_MOVE; /* unknown => MOVE */
}
static uint8_t clamp_speed(uint8_t s) {
    if (s < ZTC_SPEED_MIN) {
        return ZTC_SPEED_MIN;
    }
    return (s > ZTC_SPEED_MAX) ? (uint8_t)ZTC_SPEED_MAX : s;
}
static uint16_t clamp_timeout(uint16_t t) {
    if (t < ZTC_TIMEOUT_MIN) {
        return ZTC_TIMEOUT_MIN;
    }
    return (t > ZTC_TIMEOUT_MAX) ? (uint16_t)ZTC_TIMEOUT_MAX : t;
}

static void decode_axis(const uint8_t *p, struct ztc_axis_cfg *a) {
    a->role = clamp_role(p[0]);
    a->direction = p[1] ? 1 : 0;
    a->speed_div = clamp_speed(p[2]);
}

/* ---- wire -> shadow (validate+clamp), then publish ----------------------- */

int ztc_apply_wire(const uint8_t *buf, uint16_t len) {
    if (!buf || len != ztc_wire_len()) {
        LOG_WRN("ztc wire bad len %u (want %u)", len, ztc_wire_len());
        return -EINVAL;
    }
    if (rd16(&buf[0]) != ZTC_WIRE_MAGIC || buf[2] != ZTC_WIRE_VERSION) {
        LOG_WRN("ztc wire bad magic/version");
        return -EINVAL;
    }
    uint8_t layer_count = buf[3];
    if (layer_count > ZTC_MAX_LAYERS) {
        LOG_WRN("ztc layer_count %u > max %u", layer_count, (unsigned)ZTC_MAX_LAYERS);
        return -EINVAL;
    }

    struct ztc_snapshot sh;
    fill_defaults(&sh); /* unspecified layers keep safe defaults */

    for (uint8_t i = 0; i < layer_count; i++) {
        const uint8_t *lp = &buf[ZTC_WIRE_HDR + (uint32_t)i * ZTC_WIRE_LAYER];
        decode_axis(&lp[0], &sh.layers[i].x);
        decode_axis(&lp[4], &sh.layers[i].y);
        sh.layers[i].temp_enable = lp[8] ? true : false;
    }

    uint8_t target = buf[4];
    sh.temp_target = (target < ZTC_MAX_LAYERS) ? target : ((ZTC_MAX_LAYERS > 1) ? 1 : 0);
    sh.temp_timeout_ms = clamp_timeout(rd16(&buf[6]));

    publish(&sh);
    LOG_INF("ztc config applied (live)");
    return 0;
}

int ztc_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    uint16_t need = ztc_wire_len();
    if (!buf || cap < need) {
        return -ENOMEM;
    }
    const struct ztc_snapshot *s = ztc_live();
    memset(buf, 0, need);
    wr16(&buf[0], ZTC_WIRE_MAGIC);
    buf[2] = ZTC_WIRE_VERSION;
    buf[3] = ZTC_MAX_LAYERS;
    buf[4] = s->temp_target;
    wr16(&buf[6], s->temp_timeout_ms);
    for (uint8_t i = 0; i < ZTC_MAX_LAYERS; i++) {
        uint8_t *lp = &buf[ZTC_WIRE_HDR + (uint32_t)i * ZTC_WIRE_LAYER];
        lp[0] = s->layers[i].x.role;
        lp[1] = s->layers[i].x.direction;
        lp[2] = s->layers[i].x.speed_div;
        lp[4] = s->layers[i].y.role;
        lp[5] = s->layers[i].y.direction;
        lp[6] = s->layers[i].y.speed_div;
        lp[8] = s->layers[i].temp_enable ? 1 : 0;
    }
    if (out_len) {
        *out_len = need;
    }
    return 0;
}

/* ---- NVS persistence (validate on load too) ------------------------------ */

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>

#define ZTC_KEY "ztc"
#define ZTC_VAL "wire"

int ztc_save(void) {
    uint8_t buf[ZTC_WIRE_HDR + ZTC_MAX_LAYERS * ZTC_WIRE_LAYER];
    uint16_t len = 0;
    int rc = ztc_encode_wire(buf, sizeof(buf), &len);
    if (rc) {
        return rc;
    }
    rc = settings_save_one(ZTC_KEY "/" ZTC_VAL, buf, len);
    LOG_INF("ztc save: %d", rc);
    return rc;
}

static int ztc_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, ZTC_VAL, &next) || next) {
        return -ENOENT;
    }
    uint8_t buf[ZTC_WIRE_HDR + ZTC_MAX_LAYERS * ZTC_WIRE_LAYER];
    if (len != sizeof(buf)) {
        LOG_WRN("ztc nvs bad size %u; keeping defaults", (unsigned)len);
        return 0; /* keep defaults, do not fail boot */
    }
    ssize_t r = read_cb(cb_arg, buf, sizeof(buf));
    if (r < 0) {
        return (int)r;
    }
    /* re-validate on load (NVS can bit-rot); reject => defaults */
    if (ztc_apply_wire(buf, (uint16_t)r) == 0) {
        LOG_INF("ztc loaded from NVS");
    } else {
        LOG_WRN("ztc NVS blob rejected; keeping defaults");
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ztc, ZTC_KEY, NULL, ztc_settings_set, NULL, NULL);

#else
int ztc_save(void) { return 0; }
#endif

/* compile-time wire layout guarantees */
BUILD_ASSERT(ZTC_WIRE_HDR == 8, "wire header must be 8 bytes");
BUILD_ASSERT(ZTC_WIRE_LAYER == 12, "wire layer must be 12 bytes");
