/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * torabo_common/window_read.h — client-driven WINDOWED READ for the settings
 * characteristics whose blob can outgrow 512 bytes.
 *
 * ---------------------------------------------------------------------------
 * WHY IT EXISTS (2026-09-05)
 *
 * Android's `BluetoothGatt#readCharacteristic()` performs the ATT Read +
 * Read Blob sequence itself and stops at GATT_MAX_ATTR_LEN = 512 bytes. There is
 * no public API to continue past that: the app silently receives a 512-byte
 * truncation. Our Read Blob implementation is fine — `bt_gatt_attr_read()` slices
 * correctly at any offset, which is why Windows/Chrome (WinRT Read Long) and the
 * desktop btleplug backend have always read the whole blob — but the macros READ
 * wire is 1624 B (v1) / 1964 B (v2) and a fully populated trackpad wire is ~1.5 KB,
 * so from a Torabo-Key-App / Torabo-Studio-Android client those characteristics
 * were simply unreadable.
 *
 * The fix has to live on the firmware side, because the limit is in the client's
 * own stack. So the client asks for the slice it wants:
 *
 *   1. WRITE a 4-byte CONTROL FRAME to the same characteristic:
 *
 *          [0] 0xFF                 TORABO_WINDOW_READ_TAG0
 *          [1] 0x57 ('W')           TORABO_WINDOW_READ_TAG1
 *          [2] offset lo            u16 little-endian
 *          [3] offset hi
 *
 *   2. The very next READ of that characteristic returns
 *
 *          [0..1] offset u16 LE     echoed back, so the app can detect a race
 *          [2..3] total  u16 LE     the FULL blob length
 *          [4.. ] data              min(508, total - offset) bytes
 *
 *      i.e. at most 4 + 508 = 512 bytes — exactly what Android can read.
 *
 *   3. Serving that response RELEASES the window (one shot). The following READ
 *      is a plain whole-blob read again.
 *
 * A READ with no control frame in front of it behaves EXACTLY as before: the
 * whole blob, sliced by bt_gatt_attr_read(). Every existing client (Studio
 * desktop, Studio web, the USB tunnel — which never goes through ATT at all)
 * keeps working untouched, and the app learns that this firmware understands the
 * control frame from caps `_rsv` bit2 (TORABO_CAPS_HDR_WINDOW_READ).
 *
 * ---------------------------------------------------------------------------
 * WHY 0xFF IS A SAFE SENTINEL
 *
 * The first byte of EVERY write wire this firmware accepts is a constant that is
 * never 0xFF, so a control frame can never be mistaken for a settings write and
 * a settings write can never be mistaken for a control frame:
 *
 *   trackball  buf[0..1] = magic 0x7A74 LE  -> buf[0] == 0x74  (config_state.c)
 *   trackpad   buf[0..1] = magic 0x7470 LE  -> buf[0] == 0x70
 *   encoder    buf[0..1] = magic 0x6E65 LE  -> buf[0] == 0x65
 *   led        buf[0..1] = magic 0x656C LE  -> buf[0] == 0x6C
 *   timing     buf[0]    = version          -> buf[0] == 0x01
 *   macros     buf[0]    = version          -> buf[0] == 0x01 or 0x02
 *   combos     buf[0]    = version          -> buf[0] == 0x01 (and the length is
 *                                              exact: CB_WRITE_MAX)
 *
 * That covers the FIRST chunk of every transfer. The four features that
 * reassemble chunked writes (trackpad / timing / trackball / encoder,
 * torabo_common/wire_asm.h) also take CONTINUATION chunks that are raw blob
 * bytes at any position, and a 4-byte tail chunk whose first byte happens to be
 * 0xFF is not impossible. So those four gate the control-frame check on
 * torabo_wire_asm_assembling() being false: while a chunked settings write is in
 * flight, a 4-byte payload is blob content and goes to the assembler, exactly as
 * it did before. led / macros / combos have no assembler and no such case.
 *
 * The caps characteristic has no write callback at all (READ-only), so it cannot
 * take a control frame without gaining the WRITE property — an attribute-level
 * change that docs/COMPATIBILITY.md §8 freezes — and at 52 B it has no need of
 * one (it fits a single ATT read even on a 23-byte MTU with Read Blob).
 * live_feed is NOTIFY/16 B and is likewise out of scope.
 *
 * ---------------------------------------------------------------------------
 * SCRATCH LAYOUT (how the response is framed without a second 512-byte buffer)
 *
 * The response is `header ++ blob[offset .. offset+n)`. Rather than copying the
 * slice into a separate 512-byte buffer per feature (3.5 KB of .bss across the
 * seven characteristics), the READ handler encodes its wire into a scratch that
 * is TORABO_WINDOW_READ_HDR bytes LONGER than the wire cap, with the wire placed
 * at `scratch + TORABO_WINDOW_READ_HDR`:
 *
 *      scratch:  [ 4 spare ][ wire[0] wire[1] ... wire[wlen-1] ]
 *                            ^ blob byte i lives at scratch[4 + i]
 *
 * The 4 header bytes for window offset `off` are then written IN PLACE at
 * scratch[off .. off+3] — which is exactly the four blob bytes
 * [off-4, off), bytes the client already received in an earlier window — so the
 * response is the contiguous run starting at scratch[off]. The wire is re-encoded
 * from the live snapshot on every entry into the READ callback (as it always
 * has been), so this in-place stamping never leaks into a later read.
 *
 * ---------------------------------------------------------------------------
 * SCOPE OF THE STATE
 *
 * One window per characteristic (a `struct torabo_window_read` at file scope in
 * each gatt_service.c), not one shared slot for the whole firmware.
 *
 * The contract agreed with the app allowed a single GLOBAL slot shared by every
 * characteristic, with cross-characteristic collisions detected by the echoed
 * [offset][total] header and retried. Per-characteristic state is a STRICT
 * SUPERSET of that: a client always arms and then reads the SAME characteristic,
 * so the agreed sequence behaves identically, but a window armed on macros can
 * no longer make an unrelated whole-blob read of led come back framed. Two
 * clients windowing the SAME characteristic still race, which is exactly the
 * case the echoed header was agreed for. Nothing an app written against the
 * global-slot spec does can tell the difference.
 *
 * It is also what the module's shape allows: every feature is independently
 * optional (each feature's own CMakeLists.txt guards on its CONFIG), so there is no
 * translation unit that is always linked and could own a single global object.
 * A shared slot would mean a new always-on library just to hold four bytes,
 * where per-characteristic costs 4 B of .bss each and no build-system surface.
 *
 * GATT callbacks are serialised on the BT RX thread, so no locking is needed
 * either way.
 *
 * HOST TESTABILITY: everything here is a pure function over plain buffers, with
 * no Zephyr dependency, so test/wire/test_window_read.c drives the real code.
 * The three lines of Bluetooth glue that call it live next door in
 * torabo_common/window_read_gatt.h, which is the only part that needs Zephyr.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/** Control-frame sentinel: [0xFF]['W'][offset lo][offset hi]. */
#define TORABO_WINDOW_READ_TAG0 0xFFu
#define TORABO_WINDOW_READ_TAG1 0x57u /* 'W' */
/** A control frame is EXACTLY this long; any other length is a settings write. */
#define TORABO_WINDOW_READ_CTRL_LEN 4u

/** Response header: [offset u16 LE][total u16 LE]. */
#define TORABO_WINDOW_READ_HDR 4u
/** Hard ceiling for the whole response — Android's GATT_MAX_ATTR_LEN. */
#define TORABO_WINDOW_READ_MAX_RESP 512u
/** ...so this much payload per window. */
#define TORABO_WINDOW_READ_MAX_DATA (TORABO_WINDOW_READ_MAX_RESP - TORABO_WINDOW_READ_HDR) /* 508 */

/**
 * One characteristic's window. Zero-initialised (BSS) = disarmed = every READ
 * returns the whole blob, which is the pre-2026-09-05 behaviour.
 */
struct torabo_window_read {
    /** Offset requested by the last control frame. */
    uint16_t offset;
    /** A control frame is pending and the next READ must be windowed. */
    bool armed;
};

/**
 * @brief Is this GATT write the window-read control frame?
 *
 * Deliberately strict: only a non-prepare, offset-0, exactly-4-byte write whose
 * first two bytes are the sentinel. See "WHY 0xFF IS A SAFE SENTINEL" above for
 * why no settings wire can collide with it.
 */
static inline bool torabo_window_read_is_ctrl(const void *data, uint16_t len, uint16_t offset,
                                              bool is_prepare) {
    const uint8_t *p = (const uint8_t *)data;
    return !is_prepare && offset == 0 && len == TORABO_WINDOW_READ_CTRL_LEN && p != NULL &&
           p[0] == TORABO_WINDOW_READ_TAG0 && p[1] == TORABO_WINDOW_READ_TAG1;
}

/**
 * @brief Arm the window from a control frame (caller checked it with
 *        torabo_window_read_is_ctrl()).
 *
 * An already-armed, not-yet-served window is simply overwritten: the newest
 * request wins.
 */
static inline void torabo_window_read_arm(struct torabo_window_read *w, const void *data) {
    const uint8_t *p = (const uint8_t *)data;
    w->offset = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
    w->armed = true;
}

/**
 * @brief Frame the windowed response in place inside the READ scratch.
 *
 * @param scratch  The READ scratch, TORABO_WINDOW_READ_HDR + wire_cap bytes,
 *                 with the encoded wire at scratch + TORABO_WINDOW_READ_HDR.
 * @param wlen     Encoded wire length (the `total` reported to the client).
 * @param off      The armed window offset.
 * @param spill    A TORABO_WINDOW_READ_HDR-byte buffer, used only when
 *                 off > wlen (nothing to slice, so the header cannot be stamped
 *                 inside the scratch).
 * @param resp     [out] first byte of the response.
 *
 * @return response length: TORABO_WINDOW_READ_HDR .. TORABO_WINDOW_READ_MAX_RESP.
 *
 * Boundaries: `off >= wlen` yields the 4-byte header alone (empty data), and
 * `off < wlen` yields min(508, wlen - off) data bytes.
 */
static inline uint16_t torabo_window_read_frame(uint8_t *scratch, uint16_t wlen, uint16_t off,
                                                uint8_t *spill, const uint8_t **resp) {
    uint16_t n = 0;
    uint8_t *h;

    if (off <= wlen) {
        /* The header overwrites blob bytes [off-4, off) — already delivered in an
         * earlier window, and re-encoded fresh on the next entry. */
        n = (uint16_t)(wlen - off);
        if (n > TORABO_WINDOW_READ_MAX_DATA) {
            n = (uint16_t)TORABO_WINDOW_READ_MAX_DATA;
        }
        h = &scratch[off];
    } else {
        /* Past the end: header only, and scratch[off] may be out of bounds. */
        h = spill;
    }

    h[0] = (uint8_t)(off & 0xFFu);
    h[1] = (uint8_t)(off >> 8);
    h[2] = (uint8_t)(wlen & 0xFFu);
    h[3] = (uint8_t)(wlen >> 8);

    *resp = h;
    return (uint16_t)(TORABO_WINDOW_READ_HDR + n);
}

/**
 * @brief Release the window once the whole response has reached the client.
 *
 * "One READ" from the app's point of view is one ATT Read possibly followed by
 * several Read Blobs, so the window must survive until the LAST fragment has
 * been served — releasing it on the first fragment would make every windowed
 * read return the first 244 bytes of the window followed by the whole blob.
 *
 * @param w          The window.
 * @param att_offset The ATT read offset this fragment answered.
 * @param served     Bytes returned for it (negative = the stack rejected it).
 * @param resp_len   Total response length from torabo_window_read_frame().
 */
static inline void torabo_window_read_served(struct torabo_window_read *w, uint16_t att_offset,
                                             int served, uint16_t resp_len) {
    if (served < 0 || (uint32_t)att_offset + (uint32_t)served >= (uint32_t)resp_len) {
        w->armed = false;
    }
}
