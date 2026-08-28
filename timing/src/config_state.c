/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Double-buffered, validated RAM store for the timing settings, plus the strong
 * implementations of the three zmk-fork override points. docs/DESIGN-timing.md.
 *
 * Wire is parsed via explicit byte offsets (no packed-struct unaligned access on
 * Cortex-M) and is fixed-length, so validation is: right version, right shape,
 * right length — then clamp everything.
 *
 * WHO CALLS WHAT, AND ON WHICH THREAD
 *   zmk_torabo_ht_report_dt   behavior_hold_tap_init, once per node, at boot
 *   zmk_torabo_ht_override    the keymap/behavior thread, once per key press
 *   zmk_torabo_debounce_effective  the kscan thread, every scan
 *   tmg_apply_wire            the BT RX thread (GATT) / RPC thread / settings
 *                             loader at boot
 * The readers never take a lock: they read one atomic index and then a buffer
 * that is not being written. The writers are serialised among themselves (a
 * GATT write and the boot-time settings load cannot overlap).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include <zmk/debounce.h>
#include <zmk_timing_config/config.h>

LOG_MODULE_REGISTER(tmg_config, CONFIG_ZMK_TIMING_CONFIG_LOG_LEVEL);

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* ---- fallback defaults ---------------------------------------------------
 *
 * These mirror zmk's mod_tap.dtsi / layer_tap.dtsi so a READ that lands before
 * the behaviors have initialised still answers with something truthful. The real
 * devicetree values replace them as soon as zmk_torabo_ht_report_dt arrives,
 * which is the number the app's "standard" preset must agree with.
 */
#define TMG_DEFAULT_TAPPING_TERM_MS 200u
#define TMG_FLAVOR_HOLD_PREFERRED 0u
#define TMG_FLAVOR_TAP_PREFERRED 2u

static void fill_defaults(struct tmg_snapshot *s) {
    memset(s, 0, sizeof(*s));
    s->ht[TMG_NODE_MT] = (struct zmk_torabo_ht_params){
        .tapping_term_ms = TMG_DEFAULT_TAPPING_TERM_MS,
        .quick_tap_ms = -1,
        .require_prior_idle_ms = -1,
        .flavor = TMG_FLAVOR_HOLD_PREFERRED,
    };
    s->ht[TMG_NODE_LT] = (struct zmk_torabo_ht_params){
        .tapping_term_ms = TMG_DEFAULT_TAPPING_TERM_MS,
        .quick_tap_ms = -1,
        .require_prior_idle_ms = -1,
        .flavor = TMG_FLAVOR_TAP_PREFERRED,
    };
    /* The kscan debounce devicetree values are private to the driver, so the
     * build states them here instead (see the Kconfig help). */
    s->debounce_press_ms = (uint8_t)CONFIG_ZMK_TIMING_CONFIG_DEBOUNCE_PRESS_MS;
    s->debounce_release_ms = (uint8_t)CONFIG_ZMK_TIMING_CONFIG_DEBOUNCE_RELEASE_MS;
}

/* ---- double buffer ------------------------------------------------------- */

static struct tmg_snapshot snap[2];
static atomic_t live_idx = ATOMIC_INIT(0);
static atomic_t initialized = ATOMIC_INIT(0);

/* Set once a valid wire has been applied (from NVS at boot, or a write). Until
 * then the override hooks decline and the firmware runs purely on devicetree —
 * installing this module changes nothing until the user actually asks for it. */
static atomic_t wire_present = ATOMIC_INIT(0);

/* The debounce config handed to the kscan driver, in its own double buffer: the
 * kscan thread dereferences the pointer we return long after we returned it, so
 * the bytes behind it must never be edited in place. */
static struct zmk_debounce_config dbn[2];
static atomic_t dbn_idx = ATOMIC_INIT(0);

static void ensure_init(void) {
    if (atomic_cas(&initialized, 0, 1)) {
        fill_defaults(&snap[0]);
        fill_defaults(&snap[1]);
    }
}

const struct tmg_snapshot *tmg_live(void) {
    ensure_init();
    return &snap[atomic_get(&live_idx)];
}

/* Publish a fully-built snapshot with a single atomic index swap. */
static void publish(const struct tmg_snapshot *built) {
    ensure_init();
    int next = 1 - (int)atomic_get(&live_idx);
    snap[next] = *built; /* completes before the index swap below */
    atomic_set(&live_idx, next);

    int dnext = 1 - (int)atomic_get(&dbn_idx);
    dbn[dnext].debounce_press_ms = built->debounce_press_ms;
    dbn[dnext].debounce_release_ms = built->debounce_release_ms;
    atomic_set(&dbn_idx, dnext);
}

/* ---- wire -> shadow (validate + clamp), then publish --------------------- */

uint16_t tmg_expected_len(const uint8_t *hdr) {
    if (!hdr || hdr[0] != TMG_WIRE_VERSION || hdr[1] != TMG_HT_NODES ||
        hdr[2] != TMG_HT_POS_SLOTS) {
        return 0;
    }
    return (uint16_t)TMG_WIRE_LEN;
}

static void decode_ht(const uint8_t *b, struct zmk_torabo_ht_params *p) {
    memset(p, 0, sizeof(*p));

    p->tapping_term_ms = (uint16_t)CLAMP(rd16(&b[TMG_HT_OFF_TAPPING_TERM]), TMG_TAPPING_TERM_MIN,
                                         TMG_TAPPING_TERM_MAX);

    uint16_t qt = rd16(&b[TMG_HT_OFF_QUICK_TAP]);
    p->quick_tap_ms = (qt == TMG_U16_DISABLED) ? -1 : (int32_t)qt;

    uint16_t rpi = rd16(&b[TMG_HT_OFF_PRIOR_IDLE]);
    p->require_prior_idle_ms = (rpi == TMG_U16_DISABLED) ? -1 : (int32_t)rpi;

    /* An unknown flavor would silently change how every key on the node decides,
     * so fall back to the safest of the four rather than trusting the byte. */
    uint8_t flavor = b[TMG_HT_OFF_FLAVOR];
    p->flavor = (flavor <= TMG_FLAVOR_MAX) ? flavor : (uint8_t)TMG_FLAVOR_HOLD_PREFERRED;

    p->flags = b[TMG_HT_OFF_FLAGS] & (uint8_t)TMG_FLAGS_MASK;

    uint8_t n = b[TMG_HT_OFF_POS_COUNT];
    if (n > TMG_HT_POS_SLOTS) {
        n = (uint8_t)TMG_HT_POS_SLOTS;
    }
    p->pos_count = n;
    memcpy(p->positions, &b[TMG_HT_OFF_POSITIONS], n);
}

int tmg_apply_wire(const uint8_t *buf, uint16_t len) {
    if (!buf || len != TMG_WIRE_LEN) {
        LOG_WRN("tmg wire bad len %u (want %u)", len, (unsigned)TMG_WIRE_LEN);
        return -EINVAL;
    }
    /* Fail closed: the header must describe exactly the shape this build knows.
     * A peer that disagrees about node count or positional slots is speaking a
     * different wire, and guessing which one would be worse than refusing. */
    if (tmg_expected_len(buf) != TMG_WIRE_LEN) {
        LOG_WRN("tmg wire bad header (ver=%u nodes=%u slots=%u)", buf[0], buf[1], buf[2]);
        return -EINVAL;
    }

    /* Small enough (~110 B) for the BT RX thread stack, unlike the trackpad one. */
    struct tmg_snapshot sh;
    fill_defaults(&sh);

    sh.debounce_press_ms = (uint8_t)CLAMP(buf[3], TMG_DEBOUNCE_MIN, TMG_DEBOUNCE_MAX);
    sh.debounce_release_ms = (uint8_t)CLAMP(buf[4], TMG_DEBOUNCE_MIN, TMG_DEBOUNCE_MAX);

    for (uint8_t i = 0; i < TMG_HT_NODES; i++) {
        decode_ht(&buf[TMG_WIRE_HDR + (uint32_t)i * TMG_HT_BLOCK], &sh.ht[i]);
    }

    publish(&sh);
    atomic_set(&wire_present, 1);
    LOG_INF("tmg config applied: mt=%ums lt=%ums debounce=%u/%u", sh.ht[TMG_NODE_MT].tapping_term_ms,
            sh.ht[TMG_NODE_LT].tapping_term_ms, sh.debounce_press_ms, sh.debounce_release_ms);

    /* The other half scans its own matrix, so the debounce windows have to be
     * carried over the split link as well. After wire_present, so the push finds
     * something to send. No-op on a build without the split sync. */
    zmk_torabo_debounce_split_push();
    return 0;
}

/* ---- live -> wire (READ) ------------------------------------------------- */

/* Encode a timeout that may be "disabled". Anything at or above the sentinel is
 * pulled just below it so a huge value can never read back as "off". */
static uint16_t enc_timeout(int32_t v) {
    if (v < 0) {
        return TMG_U16_DISABLED;
    }
    return (v >= (int32_t)TMG_U16_DISABLED) ? (uint16_t)(TMG_U16_DISABLED - 1u) : (uint16_t)v;
}

static void encode_ht(uint8_t *b, const struct zmk_torabo_ht_params *p) {
    wr16(&b[TMG_HT_OFF_TAPPING_TERM], p->tapping_term_ms);
    wr16(&b[TMG_HT_OFF_QUICK_TAP], enc_timeout(p->quick_tap_ms));
    wr16(&b[TMG_HT_OFF_PRIOR_IDLE], enc_timeout(p->require_prior_idle_ms));
    b[TMG_HT_OFF_FLAVOR] = p->flavor;
    b[TMG_HT_OFF_FLAGS] = p->flags;
    uint8_t n = MIN(p->pos_count, (uint8_t)TMG_HT_POS_SLOTS);
    b[TMG_HT_OFF_POS_COUNT] = n;
    memcpy(&b[TMG_HT_OFF_POSITIONS], p->positions, n);
}

int tmg_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    if (!buf || cap < TMG_WIRE_LEN) {
        return -ENOMEM;
    }
    const struct tmg_snapshot *s = tmg_live();

    memset(buf, 0, TMG_WIRE_LEN);
    buf[0] = TMG_WIRE_VERSION;
    buf[1] = TMG_HT_NODES;
    buf[2] = TMG_HT_POS_SLOTS;
    buf[3] = s->debounce_press_ms;
    buf[4] = s->debounce_release_ms;

    for (uint8_t i = 0; i < TMG_HT_NODES; i++) {
        encode_ht(&buf[TMG_WIRE_HDR + (uint32_t)i * TMG_HT_BLOCK], &s->ht[i]);
    }

    if (out_len) {
        *out_len = (uint16_t)TMG_WIRE_LEN;
    }
    return 0;
}

/* ---- zmk fork override points (strong defs; __weak in the fork) ---------- */

static int node_index(const char *name) {
    if (!name) {
        return -1;
    }
    if (strcmp(name, TMG_NODE_NAME_MT) == 0) {
        return (int)TMG_NODE_MT;
    }
    if (strcmp(name, TMG_NODE_NAME_LT) == 0) {
        return (int)TMG_NODE_LT;
    }
    return -1; /* any other hold-tap node keeps its devicetree config */
}

bool zmk_torabo_ht_override(const struct device *dev, struct zmk_torabo_ht_params *out) {
    if (!dev || !out || !atomic_get(&wire_present)) {
        return false;
    }
    int idx = node_index(dev->name);
    if (idx < 0) {
        return false;
    }
    /* By value: the caller latches this for the whole press. */
    *out = tmg_live()->ht[idx];
    return true;
}

void zmk_torabo_ht_report_dt(const char *dev_name, const struct zmk_torabo_ht_params *dt) {
    int idx = node_index(dev_name);
    if (idx < 0 || !dt) {
        return;
    }
    /* A stored wire already describes what the firmware is doing; the devicetree
     * values are only interesting as the starting point when there is none. */
    if (atomic_get(&wire_present)) {
        return;
    }
    struct tmg_snapshot sh = *tmg_live();
    sh.ht[idx] = *dt;
    publish(&sh);
    LOG_DBG("tmg dt defaults for %s: term=%u flavor=%u", dev_name, dt->tapping_term_ms,
            dt->flavor);
}

const struct zmk_debounce_config *
zmk_torabo_debounce_effective(const struct zmk_debounce_config *dt) {
    if (!atomic_get(&wire_present)) {
        return dt; /* nothing written yet: the driver's own devicetree config */
    }
    return &dbn[atomic_get(&dbn_idx)];
}

bool zmk_torabo_debounce_split_values(uint8_t *press_ms, uint8_t *release_ms) {
    if (!atomic_get(&wire_present)) {
        /* Saying "no value" rather than the devicetree fallback is deliberate:
         * the peripheral's own devicetree is the better answer for its matrix,
         * and CONFIG_ZMK_TIMING_CONFIG_DEBOUNCE_*_MS only describes this half. */
        return false;
    }
    const struct tmg_snapshot *s = tmg_live();
    if (press_ms) {
        *press_ms = s->debounce_press_ms;
    }
    if (release_ms) {
        *release_ms = s->debounce_release_ms;
    }
    return true;
}

/* ---- NVS persistence (validate on load too) ------------------------------ */

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>

#define TMG_KEY "tmg"
#define TMG_VAL "wire"

int tmg_save(void) {
    uint8_t buf[TMG_WIRE_CAP];
    uint16_t len = 0;
    int rc = tmg_encode_wire(buf, sizeof(buf), &len);
    if (rc) {
        return rc;
    }
    rc = settings_save_one(TMG_KEY "/" TMG_VAL, buf, len);
    LOG_INF("tmg save: %d", rc);
    return rc;
}

static int tmg_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, TMG_VAL, &next) || next) {
        return -ENOENT;
    }
    uint8_t buf[TMG_WIRE_CAP];
    if (len > sizeof(buf)) {
        /* Written by a firmware whose wire outgrew this one: keep defaults
         * rather than truncate it into something that would parse wrong. */
        LOG_WRN("tmg nvs size %u > cap %u; keeping defaults", (unsigned)len,
                (unsigned)sizeof(buf));
        return 0;
    }
    ssize_t r = read_cb(cb_arg, buf, sizeof(buf));
    if (r < 0) {
        return (int)r;
    }
    if (tmg_apply_wire(buf, (uint16_t)r) == 0) {
        LOG_INF("tmg loaded from NVS");
    } else {
        LOG_WRN("tmg NVS blob rejected; keeping defaults");
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tmg, TMG_KEY, NULL, tmg_settings_set, NULL, NULL);

#else
int tmg_save(void) { return 0; }
#endif

/* compile-time wire layout guarantees (DESIGN-timing.md §"Wire v1") */
BUILD_ASSERT(TMG_WIRE_HDR == 8, "wire header must be 8 bytes");
BUILD_ASSERT(TMG_HT_BLOCK == 44, "hold-tap block must be 44 bytes");
BUILD_ASSERT(TMG_WIRE_LEN == 96, "wire must be 96 bytes");
BUILD_ASSERT(TMG_HT_POS_SLOTS == ZMK_TORABO_HT_MAX_POSITIONS,
             "wire positional slots must match the fork's hold-tap storage");
BUILD_ASSERT(TMG_HT_OFF_POSITIONS + TMG_HT_POS_SLOTS + 2u == TMG_HT_BLOCK,
             "positions + trailing reserved must fill the ht block");
