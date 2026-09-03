/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Dynamic combo store: wire-native staged config + NVS persistence.
 *
 * The store keeps the canonical config as raw, validated wire rows (one 26-byte
 * row per slot, zero-initialized => every slot disabled = fail-safe). The GATT
 * threads (BT RX) only ever touch this staging area; the combo engine pulls a
 * resolved snapshot at idle, in its own (serialized) listener context — so the
 * only cross-thread sharing is staging<->fetch, guarded by one spinlock.
 *
 * All validation/clamping happens here and on fetch; a bad write or bad NVS
 * blob changes nothing. Explicit byte offsets on the wire (no packed-struct
 * unaligned access on Cortex-M).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <zmk_dynamic_keymap/dcombo.h>

LOG_MODULE_REGISTER(dcombo_config, CONFIG_ZMK_DYNAMIC_COMBOS_LOG_LEVEL);

/* ---- little-endian helpers ----------------------------------------------- */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- staged wire rows (zero-init = all disabled = fail-safe) -------------- */
static uint8_t pending[CB_SLOTS][CB_WIRE_SLOT];
static struct k_spinlock lock;
static uint32_t pending_seq = 1; /* engines start at 0, so initial NVS-loaded state is fetched once */

static int cb_save_slot(uint8_t slot); /* fwd */

uint16_t cb_read_wire_len(void) { return CB_READ_WIRE_LEN; }

/* Validate a single 26-byte wire row (structural only; the engine guards key
 * position ranges against the keymap). Returns true if it is safe to store. */
static bool valid_row(const uint8_t *row) {
    uint8_t enabled = row[CB_W_ENABLED];
    uint8_t pos_count = row[CB_W_POS_COUNT];
    uint8_t flags = row[CB_W_FLAGS];
    uint8_t tgt = row[CB_W_TGT_TYPE];
    if (enabled > 1) {
        return false;
    }
    if (pos_count > CB_MAX_POS) {
        return false;
    }
    if (flags & ~CB_FLAG_SLOW_RELEASE) {
        return false;
    }
    if (tgt > CB_TGT_MAX) {
        return false;
    }
    return true;
}

/* ---- one-slot WRITE wire -> validate -> stage -> persist ------------------ */
int cb_apply_write_wire(const uint8_t *buf, uint16_t len) {
    if (!buf || len != CB_WRITE_MAX) {
        return -EINVAL;
    }
    if (buf[0] != CB_VERSION) {
        LOG_WRN("combo write bad version %u", buf[0]);
        return -EINVAL;
    }
    uint8_t slot = buf[1];
    if (slot >= CB_SLOTS) {
        LOG_WRN("combo write bad slot %u", slot);
        return -EINVAL;
    }
    const uint8_t *row = &buf[CB_WRITE_HDR];
    if (!valid_row(row)) {
        LOG_WRN("combo write slot %u malformed", slot);
        return -EINVAL;
    }

    k_spinlock_key_t k = k_spin_lock(&lock);
    memcpy(pending[slot], row, CB_WIRE_SLOT);
    pending_seq++;
    k_spin_unlock(&lock, k);

    cb_save_slot(slot);
    LOG_INF("combo slot %u staged (live on next idle), enabled=%u", slot, row[CB_W_ENABLED]);
    return 0;
}

/* ---- full READ wire (all slots) ------------------------------------------ */
int cb_encode_read_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    if (!buf || cap < CB_READ_WIRE_LEN) {
        return -ENOMEM;
    }
    memset(buf, 0, CB_READ_WIRE_LEN);
    wr16(&buf[0], CB_MAGIC);
    buf[2] = CB_VERSION;
    buf[3] = CB_SLOTS;
    k_spinlock_key_t k = k_spin_lock(&lock);
    for (uint8_t s = 0; s < CB_SLOTS; s++) {
        memcpy(&buf[CB_READ_HDR + (uint32_t)s * CB_WIRE_SLOT], pending[s], CB_WIRE_SLOT);
    }
    k_spin_unlock(&lock, k);
    if (out_len) {
        *out_len = CB_READ_WIRE_LEN;
    }
    return 0;
}

/* ---- enum target -> concrete behavior binding (fail-open) ----------------- */
/* Device names of the standard behaviors, resolved at compile time when the
 * node exists in the build. An absent behavior => that combo stays disabled. */
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_key_press)
#define CB_DEV_KP DEVICE_DT_NAME(DT_INST(0, zmk_behavior_key_press))
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_momentary_layer)
#define CB_DEV_MO DEVICE_DT_NAME(DT_INST(0, zmk_behavior_momentary_layer))
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_to_layer)
#define CB_DEV_TO DEVICE_DT_NAME(DT_INST(0, zmk_behavior_to_layer))
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_toggle_layer)
#define CB_DEV_TOG DEVICE_DT_NAME(DT_INST(0, zmk_behavior_toggle_layer))
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_dynamic_macro)
#define CB_DEV_DMAC DEVICE_DT_NAME(DT_INST(0, zmk_behavior_dynamic_macro))
#endif

static const char *target_dev(uint8_t tgt) {
    switch (tgt) {
#ifdef CB_DEV_KP
    case CB_TGT_KP:
        return CB_DEV_KP;
#endif
#ifdef CB_DEV_MO
    case CB_TGT_MO:
        return CB_DEV_MO;
#endif
#ifdef CB_DEV_TO
    case CB_TGT_TO:
        return CB_DEV_TO;
#endif
#ifdef CB_DEV_TOG
    case CB_TGT_TOG:
        return CB_DEV_TOG;
#endif
#ifdef CB_DEV_DMAC
    case CB_TGT_DMAC:
        return CB_DEV_DMAC;
#endif
    default:
        return NULL;
    }
}

/* Parse one validated wire row into an engine-ready combo. Sets enabled=false
 * (no key captured, ever) if the row is disabled or its behavior is absent. */
static void row_to_combo(const uint8_t *row, struct cb_combo *c) {
    memset(c, 0, sizeof(*c));

    uint8_t pos_count = row[CB_W_POS_COUNT];
    if (pos_count > CB_MAX_POS) {
        pos_count = CB_MAX_POS;
    }
    for (uint8_t i = 0; i < pos_count; i++) {
        c->key_positions[i] = row[CB_W_POSITIONS + i];
    }
    c->key_position_len = pos_count;
    c->layer_mask = rd32(&row[CB_W_LAYER_MASK]);
    c->timeout_ms = rd16(&row[CB_W_TIMEOUT]);
    c->require_prior_idle_ms = rd16(&row[CB_W_PRIOR_IDLE]);
    c->slow_release = (row[CB_W_FLAGS] & CB_FLAG_SLOW_RELEASE) != 0;

    const char *dev = target_dev(row[CB_W_TGT_TYPE]);
    bool ok = row[CB_W_ENABLED] && pos_count > 0 && dev != NULL;
    if (!ok) {
        c->enabled = false;
        return;
    }
    c->behavior.behavior_dev = dev;
    c->behavior.param1 = rd32(&row[CB_W_TGT_P1]);
    c->behavior.param2 = rd32(&row[CB_W_TGT_P2]);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    c->behavior.local_id = zmk_behavior_get_local_id(dev);
#endif
    c->enabled = true;
}

bool cb_fetch_pending(struct cb_combo out[CB_SLOTS], uint32_t *seq) {
    uint8_t snapshot[CB_SLOTS][CB_WIRE_SLOT];
    k_spinlock_key_t k = k_spin_lock(&lock);
    if (pending_seq == *seq) {
        k_spin_unlock(&lock, k);
        return false;
    }
    memcpy(snapshot, pending, sizeof(snapshot));
    *seq = pending_seq;
    k_spin_unlock(&lock, k);

    for (uint8_t s = 0; s < CB_SLOTS; s++) {
        row_to_combo(snapshot[s], &out[s]);
    }
    return true;
}

/* ---- NVS persistence (per slot; validate on load) ------------------------ */
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>

#define CB_KEY "cmb"

static int cb_save_slot(uint8_t slot) {
    if (slot >= CB_SLOTS) {
        return -EINVAL;
    }
    uint8_t blob[CB_WIRE_SLOT];
    k_spinlock_key_t k = k_spin_lock(&lock);
    memcpy(blob, pending[slot], CB_WIRE_SLOT);
    k_spin_unlock(&lock, k);
    char key[20];
    snprintf(key, sizeof(key), CB_KEY "/s%u", slot);
    int rc = settings_save_one(key, blob, sizeof(blob));
    LOG_INF("combo save slot %u: %d", slot, rc);
    return rc;
}

static int cb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (name[0] != 's') {
        return -ENOENT;
    }
    unsigned long slot = strtoul(&name[1], NULL, 10);
    if (slot >= CB_SLOTS) {
        return 0; /* stale key from a larger build; ignore */
    }
    if (len != CB_WIRE_SLOT) {
        LOG_WRN("combo nvs slot %lu bad size %u; skip", slot, (unsigned)len);
        return 0;
    }
    uint8_t blob[CB_WIRE_SLOT];
    ssize_t r = read_cb(cb_arg, blob, sizeof(blob));
    if (r != CB_WIRE_SLOT || !valid_row(blob)) {
        LOG_WRN("combo nvs slot %lu malformed; skip", slot);
        return 0;
    }
    k_spinlock_key_t k = k_spin_lock(&lock);
    memcpy(pending[slot], blob, CB_WIRE_SLOT);
    pending_seq++;
    k_spin_unlock(&lock, k);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dcombo, CB_KEY, NULL, cb_settings_set, NULL, NULL);

#else
static int cb_save_slot(uint8_t slot) {
    ARG_UNUSED(slot);
    return 0;
}
#endif /* CONFIG_SETTINGS */

/* compile-time wire layout guarantees (must match the app codec) */
BUILD_ASSERT(CB_WIRE_SLOT == 26, "combo wire slot must be 26 bytes");
BUILD_ASSERT(CB_W_TGT_P2 + 4 == CB_WIRE_SLOT, "combo wire field offsets");
BUILD_ASSERT(CB_READ_WIRE_LEN == 4 + CB_SLOTS * 26, "combo read wire size");
BUILD_ASSERT(CB_WRITE_MAX == 2 + 26, "combo write wire size");
