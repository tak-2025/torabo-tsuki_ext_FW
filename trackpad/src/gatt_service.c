/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * BLE GATT window for the trackpad settings. docs/DESIGN-trackpad.md §4.
 * One encrypted characteristic carrying the packed versioned wire (§3).
 *   READ  -> tp_encode_wire(live snapshot)
 *   WRITE -> tp_apply_wire(): validates magic+version+length, clamps every field
 *            into a shadow, then publishes via a single atomic swap; only then
 *            persists. Invalid writes are rejected and change nothing.
 * The write path NEVER touches the live store directly, so the lockless input
 * reader can never observe a half-applied or out-of-range config.
 *
 * UUIDs (allocated after trackball e1f4a900 / macros e1f4aa00 / combos e1f4ab00;
 * must match zmk-studio src-tauri/.../transport/trackpad.rs):
 *   service e1f4ac00-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 *   config  e1f4ac01-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk_trackpad_config/config.h>

LOG_MODULE_DECLARE(tp_config, CONFIG_ZMK_TRACKPAD_CONFIG_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_CONFIG_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#define TP_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4ac00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define TP_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4ac01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 tp_svc_uuid = BT_UUID_INIT_128(TP_BT_UUID_SVC);
static struct bt_uuid_128 tp_cfg_uuid = BT_UUID_INIT_128(TP_BT_UUID_CFG);

static ssize_t tp_read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
    /* Static (not on the BLE-callback stack): the v2 wire can be ~1KB, and GATT
     * callbacks are serialised on the BT RX thread, so a shared buffer is safe.
     * A Read Long re-enters this for each offset; we re-encode each time (cheap,
     * and always reflects the current live snapshot). */
    static uint8_t wire[TP_WIRE_CAP];
    uint16_t wlen = 0;
    if (tp_encode_wire(wire, sizeof(wire), &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

/* Wire reassembly for BOTH write transports (DESIGN-trackpad-v2.md §4.5).
 *
 * A full v2 wire can exceed one ATT MTU, so it must be split. Two very different
 * clients reach this callback and we must serve both from one static buffer:
 *
 *  (A) ATT Write Long (proper long write). Zephyr delivers Prepare Write chunks
 *      (flag PREPARE, queued, not committed) then replays every chunk on Execute
 *      with the flag clear and a RISING offset (0, then accumulated-length, ...).
 *      A single small Write Request also lands here once with offset==0.
 *
 *  (B) Plain chunked writes. The desktop app (bluest 0.6.x on Windows/WinRT) does
 *      NOT emit prepare/execute — WinRT will not promote an oversized payload into
 *      an ATT Write Long. Instead the app splits the wire itself and sends a
 *      sequence of ordinary Write Requests, so EVERY chunk arrives with offset==0.
 *      There is no offset to tell us "this continues the previous write", so we
 *      frame by parsing the staged header for the expected total length and
 *      accumulating consecutive offset==0 chunks until we reach it.
 *
 * All validation stays centralised in tp_apply_wire (magic/version/length/clamp,
 * atomic publish). The assembler below only does framing; a completed blob is
 * always re-validated by tp_apply_wire before it is applied. Cap overflow / offset
 * discontinuity / a garbled restart => drop the partial buffer and reject.
 */
#define TP_WIRE_MAGIC 0x7470u
#define TP_ASM_TIMEOUT_MS 2000 /* max gap between plain chunks before we give up */

static uint8_t tp_asm_buf[TP_WIRE_CAP];
static uint16_t tp_asm_len;      /* bytes currently staged */
static int64_t tp_asm_last_ms;   /* k_uptime_get() of the last staged chunk */

/* Parse a staged wire header (>= TP_WIRE_HDR bytes) into the total expected wire
 * length, or 0 if it is not a plausible start-of-transfer (bad magic / unknown
 * version / device or layer count out of range). Mirrors the length arithmetic in
 * config_state.c, kept in sync through the shared TP_WIRE_* macros in config.h. */
static uint16_t tp_expected_len(const uint8_t *hdr) {
    if ((uint16_t)(hdr[0] | (hdr[1] << 8)) != TP_WIRE_MAGIC) {
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

static ssize_t tp_write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                            uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    const uint8_t *data = buf;

    if ((uint32_t)offset + len > sizeof(tp_asm_buf)) {
        tp_asm_len = 0; /* drop any partial: this transfer can't fit */
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        /* Queue phase of an ATT Write Long: validate bounds, commit nothing. */
        return len;
    }

    int64_t now = k_uptime_get();

    /* ---- Transport (A): ATT Write Long continuation (offset > 0) ------------
     * A real long write replays chunks with a rising offset that must equal the
     * running length. tp_apply_wire only accepts an exact-length blob, so trying
     * it after every append applies exactly when the final chunk lands. */
    if (offset > 0) {
        if (offset != tp_asm_len) {
            LOG_WRN("tp write-long discontinuity (offset=%u expected=%u)", offset, tp_asm_len);
            tp_asm_len = 0;
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
        }
        memcpy(&tp_asm_buf[offset], data, len);
        tp_asm_len = (uint16_t)(offset + len);
        tp_asm_last_ms = now;
        if (tp_apply_wire(tp_asm_buf, tp_asm_len) == 0) {
            (void)tp_save();
            tp_asm_len = 0;
        }
        return len;
    }

    /* ---- offset == 0: single write OR a plain chunked transport (B) --------- */

    /* Case 1 — FAST PATH: this write ALONE is a complete, valid wire. Covers
     * every config that fits in one MTU and the whole v1 wire. */
    if (tp_apply_wire(data, len) == 0) {
        (void)tp_save();
        tp_asm_len = 0;
        return len;
    }

    /* Case 2 — continuation of a plain chunked transfer already in progress: the
     * previous chunk was recent, the staged header parses to an expected total,
     * and this chunk still fits. Append at the running length. */
    if (tp_asm_len > 0 && (now - tp_asm_last_ms) <= TP_ASM_TIMEOUT_MS) {
        uint16_t expected = tp_expected_len(tp_asm_buf);
        if (expected != 0 && (uint32_t)tp_asm_len + len <= expected) {
            memcpy(&tp_asm_buf[tp_asm_len], data, len);
            tp_asm_len = (uint16_t)(tp_asm_len + len);
            tp_asm_last_ms = now;
            if (tp_asm_len < expected) {
                return len; /* still assembling; await more chunks */
            }
            /* Reached the expected length: re-validate the whole blob. */
            if (tp_apply_wire(tp_asm_buf, tp_asm_len) == 0) {
                (void)tp_save();
                tp_asm_len = 0;
                return len;
            }
            LOG_WRN("tp chunked wire complete but rejected; dropping");
            tp_asm_len = 0;
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        LOG_WRN("tp chunked restart (staged=%u expected=%u chunk=%u)", tp_asm_len, expected, len);
        /* stale header / overflow: fall through and try to start fresh below */
    }

    /* Case 3 — start a NEW transfer. This first chunk must look like a plausible
     * header (magic + known version) that needs more bytes to complete; otherwise
     * reject without staging. (An exactly-complete header would have hit Case 1.) */
    if (len < TP_WIRE_HDR) {
        tp_asm_len = 0;
        LOG_WRN("tp write: %u bytes, too short for a header; rejected", len);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    uint16_t expected = tp_expected_len(data);
    if (expected == 0 || len >= expected) {
        tp_asm_len = 0;
        LOG_WRN("tp write: not a valid header start nor a continuation; rejected");
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    memcpy(tp_asm_buf, data, len); /* offset == 0 */
    tp_asm_len = len;
    tp_asm_last_ms = now;
    return len;
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(tp_svc,
    BT_GATT_PRIMARY_SERVICE(&tp_svc_uuid),
    BT_GATT_CHARACTERISTIC(&tp_cfg_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT |
                               BT_GATT_PERM_PREPARE_WRITE,
                           tp_read_cfg, tp_write_cfg, NULL),
);
/* clang-format on */

#endif /* CONFIG_ZMK_TRACKPAD_CONFIG_BLE */
