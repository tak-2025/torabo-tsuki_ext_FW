/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * torabo_common/window_read_gatt.h — the Bluetooth glue for the client-driven
 * WINDOWED READ. All the decisions live next door in torabo_common/window_read.h,
 * which is deliberately Zephyr-free so test/wire/test_window_read.c can drive the
 * real code; this header is only the two places those pure functions touch the
 * GATT API (bt_gatt_attr_read, BT_GATT_WRITE_FLAG_PREPARE).
 *
 * Read window_read.h first — it carries the whole rationale, the wire and the
 * proof that 0xFF cannot collide with any settings write.
 *
 * USAGE, in a feature's gatt_service.c:
 *
 *   static struct torabo_window_read xx_window;
 *
 *   static ssize_t xx_read_cfg(conn, attr, buf, len, offset) {
 *       static uint8_t scratch[TORABO_WINDOW_READ_HDR + XX_WIRE_CAP];
 *       uint8_t *wire = &scratch[TORABO_WINDOW_READ_HDR];
 *       uint16_t wlen = 0;
 *       if (xx_encode_wire(wire, XX_WIRE_CAP, &wlen) != 0) { ...UNLIKELY... }
 *       if (xx_window.armed) {
 *           return torabo_window_read_gatt_serve(&xx_window, conn, attr, buf, len,
 *                                                offset, scratch, wlen);
 *       }
 *       return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
 *   }
 *
 *   static ssize_t xx_write_cfg(conn, attr, buf, len, offset, flags) {
 *       if (torabo_window_read_gatt_arm(&xx_window, buf, len, offset, flags)) {
 *           return len;
 *       }
 *       ...the feature's normal write path...
 *   }
 *
 * A feature that reassembles chunked writes (torabo_common/wire_asm.h) must
 * additionally refuse to arm while a transfer is in flight:
 *
 *       if (!torabo_wire_asm_assembling(&xx_asm, k_uptime_get()) &&
 *           torabo_window_read_gatt_arm(&xx_window, buf, len, offset, flags)) {
 *           return len;
 *       }
 *
 * Requires the includer to have already included <zephyr/bluetooth/gatt.h>
 * (every gatt_service.c does).
 */
#pragma once

#include <sys/types.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/types.h>

#include <torabo_common/window_read.h>

/**
 * @brief Serve one ATT read fragment of an ARMED window.
 *
 * Call only when @p w->armed; the caller keeps its plain whole-blob
 * bt_gatt_attr_read() for the disarmed case, so nothing about an unwindowed READ
 * changes (not even an extra branch in the emitted code path that matters).
 *
 * @param w       The characteristic's window, armed by a control frame.
 * @param scratch The READ scratch: TORABO_WINDOW_READ_HDR spare bytes followed
 *                by the freshly encoded wire (see window_read.h).
 * @param wlen    Encoded wire length — the `total` reported to the client.
 *
 * The window is released once the LAST fragment of the response has been served,
 * not on the first: one client-side read is one ATT Read plus however many Read
 * Blobs the MTU needs, and releasing early would splice the whole blob onto the
 * end of the window.
 */
static inline ssize_t torabo_window_read_gatt_serve(struct torabo_window_read *w,
                                                    struct bt_conn *conn,
                                                    const struct bt_gatt_attr *attr, void *buf,
                                                    uint16_t len, uint16_t offset,
                                                    uint8_t *scratch, uint16_t wlen) {
    /* Only used when the window starts past the end of the wire, where there is
     * no room inside `scratch` to stamp the header in place. */
    uint8_t spill[TORABO_WINDOW_READ_HDR];
    const uint8_t *resp = NULL;
    uint16_t rlen = torabo_window_read_frame(scratch, wlen, w->offset, spill, &resp);

    ssize_t rc = bt_gatt_attr_read(conn, attr, buf, len, offset, resp, rlen);

    torabo_window_read_served(w, offset, (int)rc, rlen);
    return rc;
}

/**
 * @brief Consume a GATT write if it is the window-read control frame.
 *
 * @retval true  It was a control frame: @p w is now armed and the caller must
 *               return @p len WITHOUT running its settings-write path.
 * @retval false Not a control frame; the caller proceeds exactly as before.
 */
static inline bool torabo_window_read_gatt_arm(struct torabo_window_read *w, const void *buf,
                                               uint16_t len, uint16_t offset, uint8_t flags) {
    if (!torabo_window_read_is_ctrl(buf, len, offset,
                                    (flags & BT_GATT_WRITE_FLAG_PREPARE) != 0)) {
        return false;
    }
    torabo_window_read_arm(w, buf);
    return true;
}
