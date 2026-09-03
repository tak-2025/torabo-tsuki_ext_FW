/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * torabo_common/gatt_simple.h — the "one characteristic, whole blob" BLE
 * settings window shared by the five SIMPLE features (refactor phase 5, A-6:
 * PLAN-ext-fw-refactor.md).
 *
 * trackball / led / encoder / macros / combos all expose the exact same GATT
 * shape, and until phase 5 each kept its own verbatim copy of it:
 *
 *   READ  -> <feature>_encode_wire(live snapshot) into a scratch buffer, handed
 *            to bt_gatt_attr_read() which does Read Blob / offset slicing.
 *   WRITE -> reject offset != 0 (the blob fits one ATT MTU, so a fragmented
 *            write means something is wrong), then <feature>_apply_wire(), which
 *            does ALL validation and publishes atomically — it changes nothing
 *            on rejection — then persist.
 *
 * The two features whose wire can outgrow one MTU (trackpad, timing) are NOT
 * served by this header: their WRITE callback has to reassemble chunks (see
 * torabo_common/wire_asm.h) and their characteristic carries an extra
 * BT_GATT_PERM_PREPARE_WRITE permission. They keep their own hand-written
 * BT_GATT_SERVICE_DEFINE so that the highest-risk services stay literal.
 *
 * ---------------------------------------------------------------------------
 * COMPATIBILITY (PLAN §0.8): the GATT attribute list is the BLE-visible public
 * surface — attribute order IS handle order. TORABO_GATT_SIMPLE_SERVICE_DEFINE
 * below expands to exactly two attributes, in exactly the order every one of the
 * five services already had:
 *
 *      [0] BT_GATT_PRIMARY_SERVICE(&<svc>_uuid)
 *      [1] BT_GATT_CHARACTERISTIC(&<val>_uuid.uuid,
 *                                 BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
 *                                 BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
 *                                 read_fn, write_fn, NULL)
 *
 * (BT_GATT_CHARACTERISTIC itself expands to the declaration + value pair, so the
 * generated table is the same 3 bt_gatt_attr entries as before.) There is no
 * parameter that can add, drop or reorder an attribute, and none that can change
 * the properties or the permissions: they are baked into the macro. The UUIDs
 * stay spelled out in each feature's own file, where they are easy to audit.
 * ---------------------------------------------------------------------------
 *
 * Requires the includer to have a LOG_MODULE_REGISTER/DECLARE in scope (every
 * gatt_service.c has one, shared with its config module), because the generated
 * WRITE callback logs a rejection.
 */
#pragma once

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

/* Storage class for the READ scratch buffer, passed as `storage` below.
 *
 * STATIC is what a feature wants whenever the wire is more than a couple of
 * hundred bytes: GATT callbacks are serialised on the BT RX thread, so one
 * shared buffer is safe and keeps the blob off that thread's stack. AUTO (an
 * ordinary automatic array) is kept for the trackball, whose wire is small and
 * which has always used the stack — the choice is per feature so this macro
 * cannot silently move anyone's bytes between .bss and the RX stack. */
#define TORABO_GATT_WIRE_AUTO
#define TORABO_GATT_WIRE_STATIC static

/* `save_call` for a feature whose apply_fn already persists (macros, combos). */
#define TORABO_GATT_NO_SAVE ((void)0)

/**
 * @brief Define the READ/WRITE callback pair for a simple settings window.
 *
 * Expands to two complete `static ssize_t` functions named
 * `<prefix>_gatt_read` and `<prefix>_gatt_write`, matching bt_gatt_attr's
 * read/write signatures. No trailing `;` needed.
 *
 * @param prefix    Identifier prefix for the generated functions (e.g. `ztc`).
 * @param storage   TORABO_GATT_WIRE_STATIC or TORABO_GATT_WIRE_AUTO — where the
 *                  READ scratch buffer lives.
 * @param wire_cap  Size of that buffer; the feature's own WIRE_CAP constant, so
 *                  it can never drift behind a wire bump.
 * @param encode_fn `int encode_fn(uint8_t *buf, uint16_t cap, uint16_t *out_len)`
 *                  — encodes the live snapshot; non-zero => ATT "unlikely error".
 * @param apply_fn  `int apply_fn(const uint8_t *buf, uint16_t len)` — validates
 *                  and publishes atomically; changes nothing on rejection.
 * @param save_call A statement expression persisting the applied settings, e.g.
 *                  `(void)ztc_save()`, or TORABO_GATT_NO_SAVE when apply_fn
 *                  already persisted. Evaluated ONLY after a successful apply.
 * @param log_tag   String literal naming the feature in the rejection log (e.g.
 *                  "ztc", "dmac") — concatenated into the format at compile
 *                  time, so each feature keeps its existing log wording.
 */
#define TORABO_GATT_SIMPLE_HANDLERS(prefix, storage, wire_cap, encode_fn, apply_fn, save_call,     \
                                    log_tag)                                                       \
    static ssize_t prefix##_gatt_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,       \
                                      void *buf, uint16_t len, uint16_t offset) {                  \
        storage uint8_t wire[wire_cap];                                                            \
        uint16_t wlen = 0;                                                                         \
        if (encode_fn(wire, sizeof(wire), &wlen) != 0) {                                           \
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);                                               \
        }                                                                                          \
        /* bt_gatt_attr_read handles Read Blob / offset slicing for us. */                         \
        return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);                        \
    }                                                                                              \
                                                                                                   \
    static ssize_t prefix##_gatt_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,      \
                                       const void *buf, uint16_t len, uint16_t offset,             \
                                       uint8_t flags) {                                            \
        ARG_UNUSED(conn);                                                                          \
        ARG_UNUSED(attr);                                                                          \
        ARG_UNUSED(flags);                                                                         \
        if (offset != 0) {                                                                         \
            /* the blob fits one MTU; a fragmented write means something is wrong */               \
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);                                         \
        }                                                                                          \
        if (apply_fn((const uint8_t *)buf, len) != 0) {                                            \
            LOG_WRN(log_tag " GATT write rejected (len=%u)", len);                                 \
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);                                      \
        }                                                                                          \
        save_call;                                                                                 \
        return len;                                                                                \
    }

/**
 * @brief Define the two-attribute GATT service for a simple settings window.
 *
 * See the COMPATIBILITY note at the top of this file: the attribute list,
 * the characteristic properties and the permissions are fixed here and are not
 * parameterisable, precisely so no feature can drift its BLE surface.
 *
 * @param svc_sym      Service symbol (e.g. `ztc_svc`).
 * @param svc_uuid_var `struct bt_uuid_128` holding the service UUID.
 * @param val_uuid_var `struct bt_uuid_128` holding the characteristic UUID.
 * @param read_fn      READ callback (from TORABO_GATT_SIMPLE_HANDLERS).
 * @param write_fn     WRITE callback (ditto).
 */
/* clang-format off */
#define TORABO_GATT_SIMPLE_SERVICE_DEFINE(svc_sym, svc_uuid_var, val_uuid_var, read_fn, write_fn)  \
    BT_GATT_SERVICE_DEFINE(svc_sym,                                                                \
        BT_GATT_PRIMARY_SERVICE(&svc_uuid_var),                                                    \
        BT_GATT_CHARACTERISTIC(&val_uuid_var.uuid,                                                 \
                               BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,                             \
                               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,             \
                               read_fn, write_fn, NULL),                                           \
    )
/* clang-format on */
