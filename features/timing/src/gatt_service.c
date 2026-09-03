/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * BLE GATT window for the timing settings. docs/DESIGN-timing.md.
 * One encrypted characteristic carrying the fixed 96-byte wire:
 *   READ  -> tmg_encode_wire(live snapshot)
 *   WRITE -> tmg_apply_wire(): validates shape+length, clamps every field into a
 *            shadow, publishes with one atomic swap, and only then persists.
 *            An invalid write is rejected and changes nothing.
 *
 * UUIDs (allocated after led e1f4ae00 / live_feed e1f4af00):
 *   service e1f4b000-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 *   config  e1f4b001-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk_timing_config/config.h>

LOG_MODULE_DECLARE(tmg_config, CONFIG_ZMK_TIMING_CONFIG_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_TIMING_CONFIG_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#define TMG_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4b000, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define TMG_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4b001, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 tmg_svc_uuid = BT_UUID_INIT_128(TMG_BT_UUID_SVC);
static struct bt_uuid_128 tmg_cfg_uuid = BT_UUID_INIT_128(TMG_BT_UUID_CFG);

static ssize_t tmg_read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    /* Re-encoded on every (re-)entry so a Read Long always reflects the current
     * live snapshot. GATT callbacks are serialised on the BT RX thread, so one
     * shared static buffer is safe and keeps 96 B off that thread's stack. */
    static uint8_t wire[TMG_WIRE_CAP];
    uint16_t wlen = 0;
    if (tmg_encode_wire(wire, sizeof(wire), &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

/* Wire reassembly (same problem, and same solution, as the trackpad service —
 * see trackpad/src/gatt_service.c for the long version).
 *
 * 96 bytes fits one ATT write only once the MTU has been negotiated up; on the
 * default 23-byte MTU, and on Windows/WinRT clients that refuse to promote an
 * oversized payload into an ATT Write Long at all, the blob arrives in pieces:
 *
 *  (A) ATT Write Long — Prepare chunks (committed on Execute, replayed with a
 *      RISING offset), which we append by offset.
 *  (B) Plain chunked writes — EVERY chunk arrives at offset 0, so there is no
 *      offset to frame with. We parse the staged header for the expected total
 *      and accumulate consecutive offset-0 chunks until we reach it.
 *
 * The assembler only frames. All validation stays in tmg_apply_wire, which sees
 * the completed blob before anything is applied. Overflow, an offset
 * discontinuity, or a garbled restart drops the partial buffer and rejects.
 */
#define TMG_ASM_TIMEOUT_MS 2000 /* max gap between plain chunks before we give up */

static uint8_t tmg_asm_buf[TMG_WIRE_CAP];
static uint16_t tmg_asm_len;    /* bytes currently staged */
static int64_t tmg_asm_last_ms; /* k_uptime_get() of the last staged chunk */

static ssize_t tmg_write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    const uint8_t *data = buf;

    if ((uint32_t)offset + len > sizeof(tmg_asm_buf)) {
        tmg_asm_len = 0; /* drop any partial: this transfer can't fit */
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        /* Queue phase of an ATT Write Long: validate bounds, commit nothing. */
        return len;
    }

    int64_t now = k_uptime_get();

    /* ---- Transport (A): ATT Write Long continuation (offset > 0) ---------- */
    if (offset > 0) {
        if (offset != tmg_asm_len) {
            LOG_WRN("tmg write-long discontinuity (offset=%u expected=%u)", offset, tmg_asm_len);
            tmg_asm_len = 0;
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
        }
        memcpy(&tmg_asm_buf[offset], data, len);
        tmg_asm_len = (uint16_t)(offset + len);
        tmg_asm_last_ms = now;
        if (tmg_apply_wire(tmg_asm_buf, tmg_asm_len) == 0) {
            (void)tmg_save();
            tmg_asm_len = 0;
        }
        return len;
    }

    /* ---- offset == 0: a single complete write, OR transport (B) ----------- */

    /* Case 1 — FAST PATH: this write alone is a complete, valid wire. */
    if (tmg_apply_wire(data, len) == 0) {
        (void)tmg_save();
        tmg_asm_len = 0;
        return len;
    }

    /* Case 2 — continuation of a plain chunked transfer already in progress. */
    if (tmg_asm_len > 0 && (now - tmg_asm_last_ms) <= TMG_ASM_TIMEOUT_MS) {
        uint16_t expected = tmg_expected_len(tmg_asm_buf);
        if (expected != 0 && (uint32_t)tmg_asm_len + len <= expected) {
            memcpy(&tmg_asm_buf[tmg_asm_len], data, len);
            tmg_asm_len = (uint16_t)(tmg_asm_len + len);
            tmg_asm_last_ms = now;
            if (tmg_asm_len < expected) {
                return len; /* still assembling; await more chunks */
            }
            if (tmg_apply_wire(tmg_asm_buf, tmg_asm_len) == 0) {
                (void)tmg_save();
                tmg_asm_len = 0;
                return len;
            }
            LOG_WRN("tmg chunked wire complete but rejected; dropping");
            tmg_asm_len = 0;
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        LOG_WRN("tmg chunked restart (staged=%u expected=%u chunk=%u)", tmg_asm_len, expected, len);
        /* stale header / overflow: fall through and try to start fresh below */
    }

    /* Case 3 — start a NEW transfer. The first chunk must look like a plausible
     * header that still needs more bytes; an exactly-complete one hit Case 1. */
    if (len < TMG_WIRE_HDR) {
        tmg_asm_len = 0;
        LOG_WRN("tmg write: %u bytes, too short for a header; rejected", len);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    uint16_t expected = tmg_expected_len(data);
    if (expected == 0 || len >= expected) {
        tmg_asm_len = 0;
        LOG_WRN("tmg write: not a valid header start nor a continuation; rejected");
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    memcpy(tmg_asm_buf, data, len); /* offset == 0 */
    tmg_asm_len = len;
    tmg_asm_last_ms = now;
    return len;
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(tmg_svc,
    BT_GATT_PRIMARY_SERVICE(&tmg_svc_uuid),
    BT_GATT_CHARACTERISTIC(&tmg_cfg_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT |
                               BT_GATT_PERM_PREPARE_WRITE,
                           tmg_read_cfg, tmg_write_cfg, NULL),
);
/* clang-format on */

#endif /* CONFIG_ZMK_TIMING_CONFIG_BLE */
