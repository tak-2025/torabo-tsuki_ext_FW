/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Dynamic macro store: lock-free double-buffered RAM mirror + NVS persistence.
 * All validation/clamping happens here; the GATT write path and the behavior
 * reader only ever see a fully-built, in-range snapshot. Explicit byte offsets
 * on the wire (no packed-struct unaligned access on Cortex-M).
 *
 * Fail-safe: the static store is zero-initialized => every slot len==0 (empty,
 * = does nothing). A bad write or bad NVS blob changes nothing.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk_dynamic_keymap/dmac.h>

LOG_MODULE_REGISTER(dmac_config, CONFIG_ZMK_DYNAMIC_KEYMAP_LOG_LEVEL);

/* ---- little-endian helpers ----------------------------------------------- */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}
static inline uint8_t clamp_action(uint8_t a) { return (a <= DM_ACT_RELEASE) ? a : DM_ACT_TAP; }

/* ---- double buffer (zero-init = all empty = fail-safe) ------------------- */
static struct dm_store {
    struct dm_slot slots[DM_SLOTS];
} store[2];
static atomic_t live_idx = ATOMIC_INIT(0);

static int dm_save_slot(uint8_t slot); /* fwd */

const struct dm_slot *dm_live_slot(uint8_t idx) {
    if (idx >= DM_SLOTS) {
        return NULL;
    }
    return &store[atomic_get(&live_idx)].slots[idx];
}

uint16_t dm_read_wire_len(void) { return DM_READ_WIRE_LEN; }

/* Update one slot's STEPS: refresh the inactive buffer from live (this is what
 * carries the slot's NAME across untouched — apply_slot never touches
 * name_len/name), edit the steps, swap. The lock-free reader always observes a
 * complete buffer. */
static int apply_slot(uint8_t slot, const struct dm_step *steps, uint8_t len) {
    if (slot >= DM_SLOTS || len > DM_STEPS) {
        return -EINVAL;
    }
    int cur = atomic_get(&live_idx);
    int next = 1 - cur;
    store[next] = store[cur];
    struct dm_slot *d = &store[next].slots[slot];
    d->len = len;
    for (uint8_t i = 0; i < len; i++) {
        d->steps[i].action = clamp_action(steps[i].action);
        d->steps[i].keycode = steps[i].keycode;
    }
    for (uint8_t i = len; i < DM_STEPS; i++) {
        d->steps[i].action = 0;
        d->steps[i].keycode = 0;
    }
    atomic_set(&live_idx, next);
    return 0;
}

/* Update one slot's NAME (PLAN phase 8): same refresh-edit-swap shape as
 * apply_slot, but only ever touches name_len/name -- the slot's steps (and
 * every other slot) come across untouched from the live buffer. */
static int apply_slot_name(uint8_t slot, uint8_t name_len, const uint8_t *name) {
    if (slot >= DM_SLOTS || name_len > DM_NAME_MAX) {
        return -EINVAL;
    }
    int cur = atomic_get(&live_idx);
    int next = 1 - cur;
    store[next] = store[cur];
    struct dm_slot *d = &store[next].slots[slot];
    d->name_len = name_len;
    memset(d->name, 0, DM_NAME_MAX);
    memcpy(d->name, name, name_len);
    atomic_set(&live_idx, next);
    return 0;
}

static int dm_save_name(uint8_t slot); /* fwd */

/* ---- one-slot WRITE wire -> validate -> publish -> persist ----------------
 * v1 (DM_VERSION_V1): STEPS for one slot. Accepted unchanged, forever -- this
 * is what lets an app that has never heard of names (old build, old backup)
 * keep editing steps without erasing names it cannot see.
 * v2 (DM_VERSION_V2): NAME for one slot, a fixed 20 B op (DM_NAME_WRITE_LEN).
 * `kind` must be DM_WRITE_KIND_NAME; DM_WRITE_KIND_STEPS is defined by the
 * wire spec but REJECTED here on purpose -- the app never emits it, and the
 * v1 op above is the only steps path. */
int dm_apply_write_wire(const uint8_t *buf, uint16_t len) {
    if (!buf || len < 1) {
        return -EINVAL;
    }
    const uint8_t ver = buf[0];

    if (ver == DM_VERSION_V1) {
        if (len < DM_WRITE_HDR) {
            return -EINVAL;
        }
        uint8_t slot = buf[1];
        uint8_t used = buf[2];
        if (slot >= DM_SLOTS || used > DM_STEPS) {
            LOG_WRN("dmac write bad slot/len %u/%u", slot, used);
            return -EINVAL;
        }
        if (len != DM_WRITE_HDR + (uint16_t)used * DM_WIRE_STEP) {
            LOG_WRN("dmac write bad len %u", len);
            return -EINVAL;
        }

        struct dm_step steps[DM_STEPS];
        for (uint8_t i = 0; i < used; i++) {
            const uint8_t *p = &buf[DM_WRITE_HDR + (uint32_t)i * DM_WIRE_STEP];
            steps[i].action = p[0];
            steps[i].keycode = rd32(&p[1]);
        }

        int rc = apply_slot(slot, steps, used);
        if (rc) {
            return rc;
        }
        dm_save_slot(slot);
        LOG_INF("dmac slot %u applied (live), %u step(s)", slot, used);
        return 0;
    }

    if (ver == DM_VERSION_V2) {
        if (len != DM_NAME_WRITE_LEN) {
            LOG_WRN("dmac name write bad len %u", len);
            return -EINVAL;
        }
        uint8_t slot = buf[1];
        uint8_t kind = buf[2];
        uint8_t name_len = buf[3];
        if (kind != DM_WRITE_KIND_NAME) {
            /* DM_WRITE_KIND_STEPS (0) and anything else: reject rather than
             * implement a branch the app never exercises (PLAN フェーズ8). */
            LOG_WRN("dmac v2 write bad kind %u", kind);
            return -EINVAL;
        }
        if (slot >= DM_SLOTS || name_len > DM_NAME_MAX) {
            LOG_WRN("dmac name write bad slot/len %u/%u", slot, name_len);
            return -EINVAL;
        }

        int rc = apply_slot_name(slot, name_len, &buf[4]);
        if (rc) {
            return rc;
        }
        dm_save_name(slot);
        LOG_INF("dmac slot %u name applied (live), %u byte(s)", slot, name_len);
        return 0;
    }

    LOG_WRN("dmac write bad version %u", ver);
    return -EINVAL;
}

/* ---- full READ wire (all slots) --------------------------------------------
 * v2 (PLAN phase 8): the v1 slot region, byte-for-byte, then the per-slot name
 * block appended at DM_READ_NAMES_BASE (== DM_READ_WIRE_LEN_V1). A v1-only
 * decoder that stops after the slot region (its own slot_count * 81 B) still
 * reads correct steps -- nothing before the name block moved. */
int dm_encode_read_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    if (!buf || cap < DM_READ_WIRE_LEN) {
        return -ENOMEM;
    }
    const struct dm_store *s = &store[atomic_get(&live_idx)];
    memset(buf, 0, DM_READ_WIRE_LEN);
    wr16(&buf[0], DM_MAGIC);
    buf[2] = DM_VERSION;
    buf[3] = DM_SLOTS;
    for (uint8_t k = 0; k < DM_SLOTS; k++) {
        uint8_t *sp = &buf[DM_READ_HDR + (uint32_t)k * DM_READ_SLOT];
        const struct dm_slot *sl = &s->slots[k];
        sp[0] = sl->len;
        for (uint8_t i = 0; i < DM_STEPS; i++) {
            uint8_t *st = &sp[1 + (uint32_t)i * DM_WIRE_STEP];
            st[0] = sl->steps[i].action;
            wr32(&st[1], sl->steps[i].keycode);
        }
    }
    for (uint8_t k = 0; k < DM_SLOTS; k++) {
        uint8_t *np = &buf[DM_READ_NAMES_BASE + (uint32_t)k * DM_READ_NAME];
        const struct dm_slot *sl = &s->slots[k];
        /* name_len is validated <= DM_NAME_MAX on every path that sets it
         * (apply_slot_name, dm_settings_set), so no clamp is needed here --
         * but the wire spec is explicit that READ must never emit > DM_NAME_MAX,
         * so guard it anyway rather than trust that invariant silently. */
        uint8_t nlen = sl->name_len <= DM_NAME_MAX ? sl->name_len : DM_NAME_MAX;
        np[0] = nlen;
        memcpy(&np[1], sl->name, DM_NAME_MAX);
    }
    if (out_len) {
        *out_len = DM_READ_WIRE_LEN;
    }
    return 0;
}

/* ---- NVS persistence (per slot; validate on load) ------------------------ */
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>

#define DM_KEY "dmk"

static int dm_save_slot(uint8_t slot) {
    if (slot >= DM_SLOTS) {
        return -EINVAL;
    }
    const struct dm_slot *sl = &store[atomic_get(&live_idx)].slots[slot];
    uint8_t blob[1 + DM_STEPS * DM_WIRE_STEP];
    blob[0] = sl->len;
    for (uint8_t i = 0; i < sl->len; i++) {
        uint8_t *st = &blob[1 + (uint32_t)i * DM_WIRE_STEP];
        st[0] = sl->steps[i].action;
        wr32(&st[1], sl->steps[i].keycode);
    }
    uint16_t blen = 1 + (uint16_t)sl->len * DM_WIRE_STEP;
    char key[20];
    snprintf(key, sizeof(key), DM_KEY "/s%u", slot);
    int rc = settings_save_one(key, blob, blen);
    LOG_INF("dmac save slot %u: %d", slot, rc);
    return rc;
}

/* New key `dmk/nN` (PLAN phase 8) -- a slot's NAME, kept fully separate from
 * `dmk/sN` (steps). Never touches, never renames, never shares a prefix with
 * the existing steps key: an old NVS with only `dmk/sN` entries is untouched,
 * and a `dmk/nN` write can never be mistaken for a `dmk/sN` one. */
static int dm_save_name(uint8_t slot) {
    if (slot >= DM_SLOTS) {
        return -EINVAL;
    }
    const struct dm_slot *sl = &store[atomic_get(&live_idx)].slots[slot];
    uint8_t blob[1 + DM_NAME_MAX];
    blob[0] = sl->name_len;
    memcpy(&blob[1], sl->name, DM_NAME_MAX);
    char key[20];
    snprintf(key, sizeof(key), DM_KEY "/n%u", slot);
    int rc = settings_save_one(key, blob, sizeof(blob));
    LOG_INF("dmac save name %u: %d", slot, rc);
    return rc;
}

static int dm_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (name[0] == 's') {
        unsigned long slot = strtoul(&name[1], NULL, 10);
        if (slot >= DM_SLOTS) {
            return 0; /* stale key from a larger build; ignore */
        }
        uint8_t blob[1 + DM_STEPS * DM_WIRE_STEP];
        if (len == 0 || len > sizeof(blob)) {
            LOG_WRN("dmac nvs slot %lu bad size %u; skip", slot, (unsigned)len);
            return 0;
        }
        ssize_t r = read_cb(cb_arg, blob, sizeof(blob));
        if (r <= 0) {
            return 0;
        }
        uint8_t used = blob[0];
        if (used > DM_STEPS || (size_t)(1 + (size_t)used * DM_WIRE_STEP) > (size_t)r) {
            LOG_WRN("dmac nvs slot %lu malformed; skip", slot);
            return 0;
        }
        struct dm_step steps[DM_STEPS];
        for (uint8_t i = 0; i < used; i++) {
            const uint8_t *st = &blob[1 + (uint32_t)i * DM_WIRE_STEP];
            steps[i].action = st[0];
            steps[i].keycode = rd32(&st[1]);
        }
        apply_slot((uint8_t)slot, steps, used);
        return 0;
    }

    if (name[0] == 'n') {
        unsigned long slot = strtoul(&name[1], NULL, 10);
        if (slot >= DM_SLOTS) {
            return 0; /* stale key from a larger build; ignore */
        }
        uint8_t blob[1 + DM_NAME_MAX];
        if (len != sizeof(blob)) {
            LOG_WRN("dmac nvs name %lu bad size %u; skip", slot, (unsigned)len);
            return 0;
        }
        ssize_t r = read_cb(cb_arg, blob, sizeof(blob));
        if (r <= 0) {
            return 0;
        }
        uint8_t name_len = blob[0];
        if (name_len > DM_NAME_MAX) {
            LOG_WRN("dmac nvs name %lu malformed; skip", slot);
            return 0;
        }
        apply_slot_name((uint8_t)slot, name_len, &blob[1]);
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(dmac, DM_KEY, NULL, dm_settings_set, NULL, NULL);

#else
static int dm_save_slot(uint8_t slot) {
    ARG_UNUSED(slot);
    return 0;
}
static int dm_save_name(uint8_t slot) {
    ARG_UNUSED(slot);
    return 0;
}
#endif /* CONFIG_SETTINGS */

/* compile-time wire layout guarantees (must match the app codec) */
BUILD_ASSERT(DM_WIRE_STEP == 5, "wire step must be 5 bytes");
BUILD_ASSERT(DM_READ_WIRE_LEN_V1 == 4 + DM_SLOTS * (1 + DM_STEPS * 5), "read wire v1 (slot region) size");
BUILD_ASSERT(DM_READ_WIRE_LEN == DM_READ_WIRE_LEN_V1 + DM_SLOTS * (1 + DM_NAME_MAX),
             "read wire v2 (slot region + name block) size");
BUILD_ASSERT(DM_READ_WIRE_LEN == 1964, "PLAN フェーズ8: dm v2 READ wire is 1964 B");
BUILD_ASSERT(DM_READ_WIRE_LEN <= 2048, "dm v2 READ wire fits the 2048 B tunnel blob budget");
BUILD_ASSERT(DM_NAME_WRITE_LEN == 20, "PLAN フェーズ8: dm v2 name WRITE op is a fixed 20 B");
