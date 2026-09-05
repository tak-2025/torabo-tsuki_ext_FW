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

/* After LOG_MODULE_DECLARE above: the assembler's LOG_WRN calls bind to this
 * file's log module. */
#include <torabo_common/wire_asm.h>
#include <torabo_common/window_read.h>
#include <torabo_common/window_read_gatt.h>

#define TMG_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4b000, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define TMG_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4b001, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 tmg_svc_uuid = BT_UUID_INIT_128(TMG_BT_UUID_SVC);
static struct bt_uuid_128 tmg_cfg_uuid = BT_UUID_INIT_128(TMG_BT_UUID_CFG);

/* Client-driven windowed READ (torabo_common/window_read.h, 2026-09-05). The
 * 96-byte timing wire is far inside Android's 512 B read ceiling, so this window
 * is never needed here in practice; it is carried anyway so all seven settings
 * characteristics answer the control frame identically and an app never has to
 * keep a table of which ones do. Zero = disarmed = whole blob, as always. */
static struct torabo_window_read tmg_window;

static ssize_t tmg_read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    /* Re-encoded on every (re-)entry so a Read Long always reflects the current
     * live snapshot. GATT callbacks are serialised on the BT RX thread, so one
     * shared static buffer is safe and keeps 96 B off that thread's stack.
     *
     * TORABO_WINDOW_READ_HDR spare bytes IN FRONT of the wire let a windowed
     * response be stamped in place rather than copied elsewhere. */
    static uint8_t scratch[TORABO_WINDOW_READ_HDR + TMG_WIRE_CAP];
    uint8_t *wire = &scratch[TORABO_WINDOW_READ_HDR];
    uint16_t wlen = 0;
    if (tmg_encode_wire(wire, (uint16_t)TMG_WIRE_CAP, &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    if (tmg_window.armed) {
        return torabo_window_read_gatt_serve(&tmg_window, conn, attr, buf, len, offset, scratch,
                                             wlen);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

/* Wire reassembly (same problem, and now literally the same code, as the
 * trackpad service): 96 bytes fits one ATT write only once the MTU has been
 * negotiated up. On the default 23-byte MTU, and on Windows/WinRT clients that
 * refuse to promote an oversized payload into an ATT Write Long at all, the blob
 * arrives in pieces — either at rising offsets or as a run of ordinary writes
 * that ALL carry offset 0. torabo_common/wire_asm.h frames both; it is a
 * verbatim extraction of the code that used to sit right here (refactor phase 5
 * / B-1), proved equivalent in test/wire/test_wire_asm.c.
 *
 * The assembler only frames. All validation stays in tmg_apply_wire, which sees
 * the completed blob before anything is applied. */
static uint8_t tmg_asm_buf[TMG_WIRE_CAP];

static struct torabo_wire_asm tmg_asm = {
    .buf = tmg_asm_buf,
    .cap = sizeof(tmg_asm_buf),
    .hdr_len = TMG_WIRE_HDR,
    .expected_len = tmg_expected_len,
    .apply = tmg_apply_wire,
    .save = tmg_save,
    .tag = "tmg",
};

static ssize_t tmg_write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    /* A 4-byte [0xFF]['W'][offset] frame arms the next READ instead of being a
     * settings write: the timing wire starts with a version byte (== 1), never
     * 0xFF. Not armed while a chunked transfer is staged — see the trackpad
     * service for why that guard exists. */
    if (!torabo_wire_asm_assembling(&tmg_asm, k_uptime_get()) &&
        torabo_window_read_gatt_arm(&tmg_window, buf, len, offset, flags)) {
        return len;
    }

    switch (torabo_wire_asm_feed(&tmg_asm, (const uint8_t *)buf, len, offset,
                                 (flags & BT_GATT_WRITE_FLAG_PREPARE) != 0, k_uptime_get())) {
    case TORABO_WIRE_ASM_ACCEPTED:
        return len;
    case TORABO_WIRE_ASM_REJECT_LEN:
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    case TORABO_WIRE_ASM_REJECT_OFFSET:
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    default:
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
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
