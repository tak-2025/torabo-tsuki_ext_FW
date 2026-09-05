/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * BLE GATT window for the trackball settings (v2/v3). docs/DESIGN_v2.md §11.E.
 * One encrypted characteristic carrying the packed versioned wire (§4).
 *   READ  -> ztc_encode_wire(live snapshot)
 *   WRITE -> ztc_apply_wire(): validates magic+version+length, clamps every
 *            field into a shadow, then publishes via a single atomic swap; only
 *            then persists. Invalid writes are rejected and change nothing.
 * The write path NEVER touches the live store directly, so the lockless input
 * reader can never observe a half-applied or out-of-range config.
 *
 * UUIDs (unchanged from v1 so the app keeps working):
 *   service e1f4a900-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 *   config  e1f4a901-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk_trackball_config/config.h>

LOG_MODULE_DECLARE(ztc_config, CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_TRACKBALL_CONFIG_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

/* After LOG_MODULE_DECLARE above: the assembler's LOG_WRN calls bind to this
 * file's log module. */
#include <torabo_common/wire_asm.h>
#include <torabo_common/window_read.h>
#include <torabo_common/window_read_gatt.h>

#define ZTC_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4a900, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define ZTC_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4a901, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 ztc_svc_uuid = BT_UUID_INIT_128(ZTC_BT_UUID_SVC);
static struct bt_uuid_128 ztc_cfg_uuid = BT_UUID_INIT_128(ZTC_BT_UUID_CFG);

/* Client-driven windowed READ (torabo_common/window_read.h, 2026-09-05). The
 * trackball wire stays a few hundred bytes, so it is readable on Android as it
 * is; the window is carried anyway so all seven settings characteristics answer
 * the control frame identically. Zero = disarmed = whole blob, as always. */
static struct torabo_window_read ztc_window;

static ssize_t ztc_read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    /* On the RX-thread stack, as this feature's READ always has: the trackball
     * wire is a few hundred bytes at most (8 + 12*layers + 4). A Read Long
     * re-enters this for each offset; we re-encode each time (cheap, and always
     * reflects the current live snapshot).
     *
     * TORABO_WINDOW_READ_HDR spare bytes IN FRONT of the wire let a windowed
     * response be stamped in place rather than copied elsewhere. */
    uint8_t scratch[TORABO_WINDOW_READ_HDR + ZTC_WIRE_CAP];
    uint8_t *wire = &scratch[TORABO_WINDOW_READ_HDR];
    uint16_t wlen = 0;
    if (ztc_encode_wire(wire, (uint16_t)ZTC_WIRE_CAP, &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    if (ztc_window.armed) {
        return torabo_window_read_gatt_serve(&ztc_window, conn, attr, buf, len, offset, scratch,
                                             wlen);
    }
    /* bt_gatt_attr_read handles Read Blob / offset slicing for us. */
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

/* Wire reassembly for BOTH write transports, exactly as trackpad and timing do
 * it (torabo_common/wire_asm.h; DESIGN-trackpad-v2.md §4.5 for the rationale).
 *
 * WHY THE TRACKBALL NEEDS IT (2026-09-05, found by Studio's on-device probe):
 * this wire grows with the keymap. ZTC_WIRE_CAP = 8 + 12*ZMK_KEYMAP_LAYERS_LEN
 * + 4, so the field build's 10 keymap layers + -DTORABO_RESERVED_LAYERS=10 = 20
 * layers make it 252 B. A single ATT Write can carry at most ATT_MTU-3 = 244 B,
 * so every host (Chrome, WinRT, Android) has to split it — either into a proper
 * ATT Write Long at rising offsets, or, on WinRT, into a run of ordinary Write
 * Requests that ALL carry offset 0. The old simple handler rejected offset != 0
 * outright, so BLE writes failed on every client while READ still worked (Read
 * Blob needs no cooperation) and the USB tunnel still worked (no ATT involved) —
 * which is why the symptom was so hard to place.
 *
 * The assembler only FRAMES. All validation stays in ztc_apply_wire
 * (magic/version/exact length/clamp, atomic publish), which sees the completed
 * blob before anything is applied. Framing uses ztc_expected_len()
 * (config_state.c, declared in config.h) rather than a second copy of the length
 * arithmetic — it is the same function ztc_apply_wire uses for its own
 * exact-length check, so the two can never disagree about where a wire ends. */
static uint8_t ztc_asm_buf[ZTC_WIRE_CAP];

static struct torabo_wire_asm ztc_asm = {
    .buf = ztc_asm_buf,
    .cap = sizeof(ztc_asm_buf),
    .hdr_len = ZTC_WIRE_HDR,
    .expected_len = ztc_expected_len,
    .apply = ztc_apply_wire,
    .save = ztc_save,
    .tag = "ztc",
};

static ssize_t ztc_write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    /* A 4-byte [0xFF]['W'][offset] frame arms the next READ instead of being a
     * settings write: ztc is magic 0x7A74 LE => buf[0] == 0x74, never 0xFF. Not
     * armed while a chunked transfer is staged — see the trackpad service for
     * why that guard exists. */
    if (!torabo_wire_asm_assembling(&ztc_asm, k_uptime_get()) &&
        torabo_window_read_gatt_arm(&ztc_window, buf, len, offset, flags)) {
        return len;
    }

    switch (torabo_wire_asm_feed(&ztc_asm, (const uint8_t *)buf, len, offset,
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

/* COMPATIBILITY (docs/COMPATIBILITY.md §8): still exactly the two entries
 * TORABO_GATT_SIMPLE_SERVICE_DEFINE used to expand to, in the same order and
 * with the same properties — [0] primary service, [1] characteristic. Only
 * BT_GATT_PERM_PREPARE_WRITE is added, which is a permission bit on an existing
 * attribute, not an attribute. Handle order is unchanged. */
/* clang-format off */
BT_GATT_SERVICE_DEFINE(ztc_svc,
    BT_GATT_PRIMARY_SERVICE(&ztc_svc_uuid),
    BT_GATT_CHARACTERISTIC(&ztc_cfg_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT |
                               BT_GATT_PERM_PREPARE_WRITE,
                           ztc_read_cfg, ztc_write_cfg, NULL),
);
/* clang-format on */

#endif /* CONFIG_ZMK_TRACKBALL_CONFIG_BLE */
