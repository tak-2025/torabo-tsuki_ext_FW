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

/* After LOG_MODULE_DECLARE above: the assembler's LOG_WRN calls bind to this
 * file's log module. */
#include <torabo_common/wire_asm.h>
#include <torabo_common/window_read.h>
#include <torabo_common/window_read_gatt.h>

#define TP_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4ac00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define TP_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4ac01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 tp_svc_uuid = BT_UUID_INIT_128(TP_BT_UUID_SVC);
static struct bt_uuid_128 tp_cfg_uuid = BT_UUID_INIT_128(TP_BT_UUID_CFG);

/* Client-driven windowed READ (torabo_common/window_read.h, 2026-09-05): a fully
 * populated trackpad wire is ~1.5 KB, and an Android client's GATT stack truncates
 * ANY characteristic read at 512 B. Armed by the 4-byte [0xFF]['W'][offset]
 * control frame in tp_write_cfg below; zero = disarmed = every READ returns the
 * whole blob, exactly as it always has. */
static struct torabo_window_read tp_window;

static ssize_t tp_read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
    /* Static (not on the BLE-callback stack): the v2 wire can be ~1KB, and GATT
     * callbacks are serialised on the BT RX thread, so a shared buffer is safe.
     * A Read Long re-enters this for each offset; we re-encode each time (cheap,
     * and always reflects the current live snapshot).
     *
     * TORABO_WINDOW_READ_HDR spare bytes IN FRONT of the wire let a windowed
     * response be stamped in place instead of copied into a second 512 B buffer. */
    static uint8_t scratch[TORABO_WINDOW_READ_HDR + TP_WIRE_CAP];
    uint8_t *wire = &scratch[TORABO_WINDOW_READ_HDR];
    uint16_t wlen = 0;
    if (tp_encode_wire(wire, (uint16_t)TP_WIRE_CAP, &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    if (tp_window.armed) {
        return torabo_window_read_gatt_serve(&tp_window, conn, attr, buf, len, offset, scratch,
                                             wlen);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

/* Wire reassembly for BOTH write transports (DESIGN-trackpad-v2.md §4.5): a full
 * v2/v3 wire can exceed one ATT MTU, so it arrives either as a proper ATT Write
 * Long (rising offsets) or — from WinRT clients that refuse to promote an
 * oversized payload — as a run of ordinary Write Requests that ALL carry
 * offset 0. Both are framed by torabo_common/wire_asm.h, which is a verbatim
 * extraction of the code that used to sit right here (refactor phase 5 / B-1);
 * see that header for the full rationale, and test/wire/test_wire_asm.c for the
 * lockstep proof that the extraction changed nothing.
 *
 * All validation stays centralised in tp_apply_wire (magic/version/length/clamp,
 * atomic publish); the assembler only frames. Framing uses tp_expected_len()
 * (config_state.c, declared in config.h) rather than a second copy of the length
 * arithmetic — it is the same function tp_apply_wire uses for its own
 * exact-length check, so the two can never disagree about where a wire ends. */
static uint8_t tp_asm_buf[TP_WIRE_CAP];

static struct torabo_wire_asm tp_asm = {
    .buf = tp_asm_buf,
    .cap = sizeof(tp_asm_buf),
    .hdr_len = TP_WIRE_HDR,
    .expected_len = tp_expected_len,
    .apply = tp_apply_wire,
    .save = tp_save,
    .tag = "tp",
};

static ssize_t tp_write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                            uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    /* A 4-byte [0xFF]['W'][offset] frame arms the next READ instead of being a
     * settings write: no wire this firmware accepts starts with 0xFF (tp is
     * magic 0x7470 LE => buf[0] == 0x70). The assembler-in-flight guard is the
     * one case where a 4-byte 0xFF payload could legitimately be blob content —
     * the tail chunk of a plain-chunked transfer — so while one is staged the
     * bytes go to the assembler exactly as before. */
    if (!torabo_wire_asm_assembling(&tp_asm, k_uptime_get()) &&
        torabo_window_read_gatt_arm(&tp_window, buf, len, offset, flags)) {
        return len;
    }

    switch (torabo_wire_asm_feed(&tp_asm, (const uint8_t *)buf, len, offset,
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
