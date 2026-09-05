/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * torabo_common/wire_asm.h — GATT WRITE reassembler for the two features whose
 * wire can outgrow one ATT MTU (trackpad, timing).
 *
 * Extracted VERBATIM in refactor phase 5 (B-1, PLAN-ext-fw-refactor.md) from the
 * two near-identical copies that lived in features/trackpad/src/gatt_service.c
 * and features/timing/src/gatt_service.c. Not one branch, acceptance condition,
 * reset condition or ordering is changed: this is field-proven code that exists
 * to absorb real-world client behavior, and test/wire/test_wire_asm.c proves the
 * equivalence by driving this implementation and a byte-for-byte copy of the old
 * one in lockstep over tens of thousands of events.
 *
 * ---------------------------------------------------------------------------
 * WHY IT EXISTS (DESIGN-trackpad-v2.md §4.5)
 *
 * A full trackpad wire is ~1.5 KB, and timing's fixed 96 B still does not fit a
 * not-yet-negotiated 23-byte MTU. Two very different clients reach the write
 * callback and both must be served from one staging buffer:
 *
 *  (A) ATT Write Long (a proper long write). Zephyr delivers Prepare Write
 *      chunks (flag PREPARE, queued, not committed) and then replays every chunk
 *      on Execute with the flag clear and a RISING offset (0, then the
 *      accumulated length, ...). A single small Write Request also lands here
 *      once, with offset == 0.
 *
 *  (B) Plain chunked writes. The desktop app (bluest 0.6.x on Windows/WinRT)
 *      does NOT emit prepare/execute — WinRT refuses to promote an oversized
 *      payload into an ATT Write Long. The app splits the wire itself and sends
 *      a sequence of ordinary Write Requests, so EVERY chunk arrives with
 *      offset == 0. There is no offset to say "this continues the previous
 *      write", so we frame by parsing the staged header for the expected total
 *      length and accumulating consecutive offset-0 chunks until we reach it.
 *
 * The assembler only FRAMES. All validation stays in the feature's own
 * apply callback (magic / version / length / clamp, atomic publish), which sees
 * the completed blob before anything is applied. Cap overflow, an offset
 * discontinuity or a garbled restart drops the partial buffer and rejects.
 *
 * Framing uses the feature's own expected_len callback (tp_expected_len /
 * tmg_expected_len) rather than a second copy of the length arithmetic — it is
 * the same function the apply path uses for its own exact-length check, so the
 * two can never disagree about where a wire ends.
 * ---------------------------------------------------------------------------
 *
 * HOST TESTABILITY: this header deliberately depends on nothing but <string.h>,
 * <stdint.h>, <stdbool.h> and LOG_WRN. The wall clock is a parameter, not a
 * k_uptime_get() call, and rejections are returned as an enum that the caller
 * maps to ATT error codes (a 1:1 switch, see either gatt_service.c). That keeps
 * the Bluetooth stack out of the tested code without changing what it does.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>

/* Max gap between two plain (offset-0) chunks before a partial transfer is
 * considered abandoned. Both features used 2000 ms; it is shared, not
 * per-feature, so the two can never drift apart by accident. */
#define TORABO_WIRE_ASM_TIMEOUT_MS 2000

/**
 * Outcome of one fed chunk. The caller maps these onto ATT errors:
 *   ACCEPTED       -> return len
 *   REJECT_LEN     -> BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN)
 *   REJECT_OFFSET  -> BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET)
 *   REJECT_VALUE   -> BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED)
 */
enum torabo_wire_asm_res {
    TORABO_WIRE_ASM_ACCEPTED = 0,
    TORABO_WIRE_ASM_REJECT_LEN = 1,
    TORABO_WIRE_ASM_REJECT_OFFSET = 2,
    TORABO_WIRE_ASM_REJECT_VALUE = 3,
};

/**
 * One feature's reassembly window: its configuration (set once in the
 * initialiser and never written afterwards) followed by the running state.
 *
 * Declare exactly one per feature, at file scope in its gatt_service.c.
 */
struct torabo_wire_asm {
    /* ---- configuration ---------------------------------------------------- */

    /** Staging buffer, at least @ref cap bytes (the feature's WIRE_CAP). */
    uint8_t *buf;
    /** sizeof(*buf). A chunk whose offset+len exceeds this is refused outright. */
    uint16_t cap;
    /** Shortest prefix that expected_len can decide on (the feature's WIRE_HDR).
     *  A first chunk shorter than this is rejected without staging. */
    uint16_t hdr_len;
    /** Total length the blob starting with this header claims, or 0 when the
     *  header is not a plausible start of one. At least @ref hdr_len bytes are
     *  readable at @p hdr. MUST NOT have side effects. */
    uint16_t (*expected_len)(const uint8_t *hdr);
    /** Validate + publish atomically; 0 on success, negative on rejection (in
     *  which case it must have changed nothing). */
    int (*apply)(const uint8_t *buf, uint16_t len);
    /** Persist the now-live settings. Called only after a successful apply.
     *  Must be non-NULL (both users have one). */
    int (*save)(void);
    /** Feature name for the rejection logs ("tp", "tmg"). */
    const char *tag;

    /* ---- running state (zero-initialised) --------------------------------- */

    /** Bytes currently staged in @ref buf. */
    uint16_t len;
    /** Uptime (ms) of the last staged chunk, for the plain-chunk timeout. */
    int64_t last_ms;
};

/**
 * @brief Is a plain-chunked transfer currently in flight?
 *
 * True exactly when torabo_wire_asm_feed() would treat the next offset-0 chunk
 * as a CONTINUATION (Case 2 below) rather than as a fresh transfer — i.e. bytes
 * are staged AND the previous chunk is younger than the timeout.
 *
 * Added 2026-09-05 for the windowed READ (torabo_common/window_read.h): the
 * 4-byte control frame is intercepted BEFORE the assembler sees it, and this
 * lets the write callback decline to do that while a chunked settings write is
 * mid-flight — the one situation where a 4-byte payload could legitimately be
 * blob content rather than a control frame. Read-only; the assembler's own
 * behaviour is unchanged.
 */
static inline bool torabo_wire_asm_assembling(const struct torabo_wire_asm *a, int64_t now) {
    return a->len > 0 && (now - a->last_ms) <= TORABO_WIRE_ASM_TIMEOUT_MS;
}

/**
 * @brief Feed one GATT write chunk into the reassembler.
 *
 * @param a          The feature's window.
 * @param data       Chunk payload (the callback's `buf`).
 * @param len        Chunk length.
 * @param offset     The callback's `offset`.
 * @param is_prepare `(flags & BT_GATT_WRITE_FLAG_PREPARE) != 0`.
 * @param now        k_uptime_get() — passed in so this stays host-testable.
 *
 * @return see @ref torabo_wire_asm_res.
 */
static inline enum torabo_wire_asm_res torabo_wire_asm_feed(struct torabo_wire_asm *a,
                                                            const uint8_t *data, uint16_t len,
                                                            uint16_t offset, bool is_prepare,
                                                            int64_t now) {
    if ((uint32_t)offset + len > a->cap) {
        a->len = 0; /* drop any partial: this transfer can't fit */
        return TORABO_WIRE_ASM_REJECT_LEN;
    }
    if (is_prepare) {
        /* Queue phase of an ATT Write Long: validate bounds, commit nothing. */
        return TORABO_WIRE_ASM_ACCEPTED;
    }

    /* ---- Transport (A): ATT Write Long continuation (offset > 0) ------------
     * A real long write replays chunks with a rising offset that must equal the
     * running length. apply() only accepts an exact-length blob, so trying it
     * after every append applies exactly when the final chunk lands. */
    if (offset > 0) {
        if (offset != a->len) {
            LOG_WRN("%s write-long discontinuity (offset=%u expected=%u)", a->tag, offset, a->len);
            a->len = 0;
            return TORABO_WIRE_ASM_REJECT_OFFSET;
        }
        memcpy(&a->buf[offset], data, len);
        a->len = (uint16_t)(offset + len);
        a->last_ms = now;
        if (a->apply(a->buf, a->len) == 0) {
            (void)a->save();
            a->len = 0;
        }
        return TORABO_WIRE_ASM_ACCEPTED;
    }

    /* ---- offset == 0: single write OR a plain chunked transport (B) --------- */

    /* Case 1 — FAST PATH: this write ALONE is a complete, valid wire. Covers
     * every config that fits in one MTU. */
    if (a->apply(data, len) == 0) {
        (void)a->save();
        a->len = 0;
        return TORABO_WIRE_ASM_ACCEPTED;
    }

    /* Case 2 — continuation of a plain chunked transfer already in progress: the
     * previous chunk was recent, the staged header parses to an expected total,
     * and this chunk still fits. Append at the running length. */
    if (a->len > 0 && (now - a->last_ms) <= TORABO_WIRE_ASM_TIMEOUT_MS) {
        uint16_t expected = a->expected_len(a->buf);
        if (expected != 0 && (uint32_t)a->len + len <= expected) {
            memcpy(&a->buf[a->len], data, len);
            a->len = (uint16_t)(a->len + len);
            a->last_ms = now;
            if (a->len < expected) {
                return TORABO_WIRE_ASM_ACCEPTED; /* still assembling; await more chunks */
            }
            /* Reached the expected length: re-validate the whole blob. */
            if (a->apply(a->buf, a->len) == 0) {
                (void)a->save();
                a->len = 0;
                return TORABO_WIRE_ASM_ACCEPTED;
            }
            LOG_WRN("%s chunked wire complete but rejected; dropping", a->tag);
            a->len = 0;
            return TORABO_WIRE_ASM_REJECT_VALUE;
        }
        LOG_WRN("%s chunked restart (staged=%u expected=%u chunk=%u)", a->tag, a->len, expected,
                len);
        /* stale header / overflow: fall through and try to start fresh below */
    }

    /* Case 3 — start a NEW transfer. This first chunk must look like a plausible
     * header (magic + known version) that needs more bytes to complete; otherwise
     * reject without staging. (An exactly-complete header would have hit Case 1.) */
    if (len < a->hdr_len) {
        a->len = 0;
        LOG_WRN("%s write: %u bytes, too short for a header; rejected", a->tag, len);
        return TORABO_WIRE_ASM_REJECT_VALUE;
    }
    uint16_t expected = a->expected_len(data);
    if (expected == 0 || len >= expected) {
        a->len = 0;
        LOG_WRN("%s write: not a valid header start nor a continuation; rejected", a->tag);
        return TORABO_WIRE_ASM_REJECT_VALUE;
    }
    memcpy(a->buf, data, len); /* offset == 0 */
    a->len = len;
    a->last_ms = now;
    return TORABO_WIRE_ASM_ACCEPTED;
}
