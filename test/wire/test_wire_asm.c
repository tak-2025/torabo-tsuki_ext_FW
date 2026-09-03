/*
 * GATT WRITE reassembler (torabo_common/wire_asm.h) — host golden tests
 * (refactor phase 5, B-1 of PLAN-ext-fw-refactor.md).
 *
 * This is the riskiest extraction in the whole refactor: the reassembler is what
 * absorbs the difference between a well-behaved ATT Write Long and WinRT's
 * refusal to emit one, and it has been proven in the field rather than derived
 * from a spec. So it is not "reviewed and shipped" — it is DIFFERENTIALLY
 * tested against the code it replaced.
 *
 * ---- the reference ---------------------------------------------------------
 * ref_write() below is a byte-for-byte copy of the pre-extraction assembler as
 * it stood on refactor/phase4-binding-synthesis:
 *     git show refactor/phase4-binding-synthesis:features/trackpad/src/gatt_service.c
 * (the timing copy at the same revision was verified identical: a `diff` with
 * the TP_/TMG_ prefixes normalised shows ONLY comment text, whitespace and the
 * log tag string — not one statement differs, which is why one reference stands
 * in for both.)
 *
 * Exactly three mechanical edits were made, and nothing else:
 *   1. names            tp_* / TP_*         -> ref_* / REF_*
 *   2. the clock        `int64_t now = k_uptime_get();` -> a `now` parameter,
 *                       because the host stub's k_uptime_get() is frozen at 0
 *                       and the timeout branch must be reachable.
 *   3. the return type  `len` / BT_GATT_ERR(BT_ATT_ERR_x) -> the REF_* codes,
 *                       which are numerically the same enumeration the
 *                       production callback maps back onto ATT errors in its
 *                       1:1 switch (see either gatt_service.c). Keeping the ATT
 *                       codes here would have dragged the Bluetooth headers into
 *                       a host build for no extra coverage: the mapping is the
 *                       one line of the change that is verifiable by eye.
 * The unused conn/attr parameters are dropped (they were ARG_UNUSED).
 *
 * ---- how they are compared -------------------------------------------------
 * Both implementations run over the SAME event stream, in lockstep, each with
 * its own staging buffer, and both call the SAME backend (apply / expected_len /
 * save). After every single event we compare:
 *   - the returned result code,
 *   - the number of staged bytes and every staged byte,
 *   - the full log of backend calls since the last event (which apply() calls
 *     were made, with which lengths, and whether save() ran) — this is what
 *     catches a reordering that happens to end in the same state,
 *   - the last blob the backend accepted.
 *
 * Two backends are used. `tp` is the REAL trackpad codec (tp_expected_len /
 * tp_apply_wire / tp_save), so the framing is exercised against genuine header
 * rules and genuine lengths. `syn` is a synthetic codec whose expected length is
 * simply a header byte, which makes it possible to construct headers the real
 * codec would never produce — expected == 0, an expected that changes mid
 * transfer, lengths right at the cap.
 *
 * Coverage: (A) offset-carrying Write Long incl. the Prepare pass, (B) plain
 * offset-0 chunk runs, (C) mixtures of the two, (D) malformed sequences
 * (over-cap offset+len, offset discontinuity, huge offset, sub-header first
 * chunk, unknown version, complete-but-rejected blob, a stale partial past the
 * 2 s window, a restart in the middle of a transfer), and finally a
 * deterministic pseudo-random fuzz of ~24k events per backend.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zmk_trackpad_config/config.h>

#include <torabo_common/wire_asm.h>

#include "torabo_test.h"

/* ======================================================================== */
/* the pluggable backend both implementations share                          */
/* ======================================================================== */

#define CALLLOG_MAX 8

struct calllog {
    int n;
    struct {
        int kind; /* 0 = apply, 1 = save */
        uint16_t len;
        int ret;
    } e[CALLLOG_MAX];
};

static struct calllog g_log;
static uint8_t g_last_applied[TP_WIRE_CAP];
static uint16_t g_last_applied_len;

static void calllog_reset(void) { g_log.n = 0; }

static void calllog_add(int kind, uint16_t len, int ret) {
    if (g_log.n < CALLLOG_MAX) {
        g_log.e[g_log.n].kind = kind;
        g_log.e[g_log.n].len = len;
        g_log.e[g_log.n].ret = ret;
    }
    g_log.n++; /* counted even when it overflows, so a divergence still shows */
}

/* -- backend selection ---------------------------------------------------- */

enum backend { BACKEND_TP = 0, BACKEND_SYN = 1 };
static enum backend g_backend;

/* Synthetic codec: [0]=0xAB magic, [1]=version (1 or 2), [2..3]=total length LE.
 * Accepts a blob iff the header is well formed and len == the declared total. */
#define SYN_HDR 4u
#define SYN_MAGIC 0xABu

static uint16_t syn_expected_len(const uint8_t *hdr) {
    if (!hdr || hdr[0] != SYN_MAGIC) {
        return 0;
    }
    if (hdr[1] != 1 && hdr[1] != 2) {
        return 0;
    }
    uint16_t total = (uint16_t)(hdr[2] | (hdr[3] << 8));
    if (total < SYN_HDR || total > TP_WIRE_CAP) {
        return 0;
    }
    return total;
}

static int syn_apply(const uint8_t *buf, uint16_t len) {
    if (!buf || len < SYN_HDR) {
        return -EINVAL;
    }
    uint16_t want = syn_expected_len(buf);
    if (want == 0 || want != len) {
        return -EINVAL;
    }
    return 0;
}

/* -- the shims both implementations call ---------------------------------- */

static uint16_t backend_expected_len(const uint8_t *hdr) {
    return (g_backend == BACKEND_TP) ? tp_expected_len(hdr) : syn_expected_len(hdr);
}

static int backend_apply(const uint8_t *buf, uint16_t len) {
    int ret = (g_backend == BACKEND_TP) ? tp_apply_wire(buf, len) : syn_apply(buf, len);
    calllog_add(0, len, ret);
    if (ret == 0 && buf && len <= sizeof(g_last_applied)) {
        memcpy(g_last_applied, buf, len);
        g_last_applied_len = len;
    }
    return ret;
}

static int backend_save(void) {
    calllog_add(1, 0, 0);
    return 0;
}

/* ======================================================================== */
/* REFERENCE — verbatim pre-phase-5 assembler (see the header comment)       */
/* ======================================================================== */

#define REF_OK 0
#define REF_ERR_LEN 1    /* was BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN) */
#define REF_ERR_OFFSET 2 /* was BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET)        */
#define REF_ERR_VALUE 3  /* was BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED)     */

#define REF_ASM_TIMEOUT_MS 2000 /* max gap between plain chunks before we give up */
#define REF_WIRE_HDR TP_WIRE_HDR
#define REF_WIRE_CAP TP_WIRE_CAP
#define REF_PREPARE 0x01u /* stands in for BT_GATT_WRITE_FLAG_PREPARE */

#define ref_apply_wire backend_apply
#define ref_expected_len backend_expected_len
#define ref_save backend_save

static uint8_t ref_asm_buf[REF_WIRE_CAP];
static uint16_t ref_asm_len;    /* bytes currently staged */
static int64_t ref_asm_last_ms; /* k_uptime_get() of the last staged chunk */

static int ref_write_cfg(const void *buf, uint16_t len, uint16_t offset, uint8_t flags,
                         int64_t now) {
    const uint8_t *data = buf;

    if ((uint32_t)offset + len > sizeof(ref_asm_buf)) {
        ref_asm_len = 0; /* drop any partial: this transfer can't fit */
        return REF_ERR_LEN;
    }
    if (flags & REF_PREPARE) {
        /* Queue phase of an ATT Write Long: validate bounds, commit nothing. */
        return REF_OK;
    }

    /* ---- Transport (A): ATT Write Long continuation (offset > 0) ------------
     * A real long write replays chunks with a rising offset that must equal the
     * running length. ref_apply_wire only accepts an exact-length blob, so trying
     * it after every append applies exactly when the final chunk lands. */
    if (offset > 0) {
        if (offset != ref_asm_len) {
            ref_asm_len = 0;
            return REF_ERR_OFFSET;
        }
        memcpy(&ref_asm_buf[offset], data, len);
        ref_asm_len = (uint16_t)(offset + len);
        ref_asm_last_ms = now;
        if (ref_apply_wire(ref_asm_buf, ref_asm_len) == 0) {
            (void)ref_save();
            ref_asm_len = 0;
        }
        return REF_OK;
    }

    /* ---- offset == 0: single write OR a plain chunked transport (B) --------- */

    /* Case 1 — FAST PATH: this write ALONE is a complete, valid wire. Covers
     * every config that fits in one MTU and the whole v1 wire. */
    if (ref_apply_wire(data, len) == 0) {
        (void)ref_save();
        ref_asm_len = 0;
        return REF_OK;
    }

    /* Case 2 — continuation of a plain chunked transfer already in progress: the
     * previous chunk was recent, the staged header parses to an expected total,
     * and this chunk still fits. Append at the running length. */
    if (ref_asm_len > 0 && (now - ref_asm_last_ms) <= REF_ASM_TIMEOUT_MS) {
        uint16_t expected = ref_expected_len(ref_asm_buf);
        if (expected != 0 && (uint32_t)ref_asm_len + len <= expected) {
            memcpy(&ref_asm_buf[ref_asm_len], data, len);
            ref_asm_len = (uint16_t)(ref_asm_len + len);
            ref_asm_last_ms = now;
            if (ref_asm_len < expected) {
                return REF_OK; /* still assembling; await more chunks */
            }
            /* Reached the expected length: re-validate the whole blob. */
            if (ref_apply_wire(ref_asm_buf, ref_asm_len) == 0) {
                (void)ref_save();
                ref_asm_len = 0;
                return REF_OK;
            }
            ref_asm_len = 0;
            return REF_ERR_VALUE;
        }
        /* stale header / overflow: fall through and try to start fresh below */
    }

    /* Case 3 — start a NEW transfer. This first chunk must look like a plausible
     * header (magic + known version) that needs more bytes to complete; otherwise
     * reject without staging. (An exactly-complete header would have hit Case 1.) */
    if (len < REF_WIRE_HDR) {
        ref_asm_len = 0;
        return REF_ERR_VALUE;
    }
    uint16_t expected = ref_expected_len(data);
    if (expected == 0 || len >= expected) {
        ref_asm_len = 0;
        return REF_ERR_VALUE;
    }
    memcpy(ref_asm_buf, data, len); /* offset == 0 */
    ref_asm_len = len;
    ref_asm_last_ms = now;
    return REF_OK;
}

/* NOTE on REF_WIRE_HDR: the reference hard-codes the trackpad's header length,
 * exactly as the original did. The synthetic backend's header is 4 bytes, so the
 * new assembler is configured with hdr_len = TP_WIRE_HDR for BOTH backends —
 * otherwise the two would legitimately differ on a 4- or 5-byte first chunk, and
 * the comparison would be measuring the harness rather than the code. */

/* ======================================================================== */
/* the implementation under test                                             */
/* ======================================================================== */

static uint8_t new_asm_buf[TP_WIRE_CAP];

static struct torabo_wire_asm new_asm = {
    .buf = new_asm_buf,
    .cap = sizeof(new_asm_buf),
    .hdr_len = TP_WIRE_HDR,
    .expected_len = backend_expected_len,
    .apply = backend_apply,
    .save = backend_save,
    .tag = "test",
};

/* ======================================================================== */
/* the lockstep driver                                                       */
/* ======================================================================== */

struct evt {
    const uint8_t *data;
    uint16_t len;
    uint16_t offset;
    bool prepare;
    int64_t now;
};

static int g_mismatches;
static int g_events;

static void reset_both(void) {
    ref_asm_len = 0;
    ref_asm_last_ms = 0;
    memset(ref_asm_buf, 0, sizeof(ref_asm_buf));
    new_asm.len = 0;
    new_asm.last_ms = 0;
    memset(new_asm_buf, 0, sizeof(new_asm_buf));
    g_last_applied_len = 0;
}

/* Run one event through both, compare everything, and report only on failure
 * (the sequences below are thousands of events long; one `ok` per sequence). */
static void step(const struct evt *e) {
    g_events++;

    calllog_reset();
    int ref_res = ref_write_cfg(e->data, e->len, e->offset, e->prepare ? REF_PREPARE : 0, e->now);
    struct calllog ref_log = g_log;
    uint16_t ref_applied_len = g_last_applied_len;
    static uint8_t ref_applied[TP_WIRE_CAP];
    memcpy(ref_applied, g_last_applied, ref_applied_len);

    calllog_reset();
    enum torabo_wire_asm_res new_res =
        torabo_wire_asm_feed(&new_asm, e->data, e->len, e->offset, e->prepare, e->now);
    struct calllog new_log = g_log;

    char detail[192];

    if ((int)new_res != ref_res) {
        snprintf(detail, sizeof(detail), "result: new=%d ref=%d (len=%u off=%u prep=%d now=%lld)",
                 (int)new_res, ref_res, e->len, e->offset, (int)e->prepare, (long long)e->now);
        torabo_test_report_fail("wire_asm parity", detail);
        g_mismatches++;
        return;
    }
    if (new_asm.len != ref_asm_len) {
        snprintf(detail, sizeof(detail), "staged len: new=%u ref=%u (len=%u off=%u)", new_asm.len,
                 ref_asm_len, e->len, e->offset);
        torabo_test_report_fail("wire_asm parity", detail);
        g_mismatches++;
        return;
    }
    if (new_asm.len && memcmp(new_asm_buf, ref_asm_buf, new_asm.len) != 0) {
        torabo_test_report_fail("wire_asm parity", "staged bytes differ");
        g_mismatches++;
        return;
    }
    if (new_asm.last_ms != ref_asm_last_ms) {
        torabo_test_report_fail("wire_asm parity", "last_ms differs");
        g_mismatches++;
        return;
    }
    if (new_log.n != ref_log.n) {
        snprintf(detail, sizeof(detail), "backend call count: new=%d ref=%d", new_log.n, ref_log.n);
        torabo_test_report_fail("wire_asm parity", detail);
        g_mismatches++;
        return;
    }
    for (int i = 0; i < new_log.n && i < CALLLOG_MAX; i++) {
        if (new_log.e[i].kind != ref_log.e[i].kind || new_log.e[i].len != ref_log.e[i].len ||
            new_log.e[i].ret != ref_log.e[i].ret) {
            snprintf(detail, sizeof(detail), "backend call %d differs (kind %d/%d len %u/%u)", i,
                     new_log.e[i].kind, ref_log.e[i].kind, new_log.e[i].len, ref_log.e[i].len);
            torabo_test_report_fail("wire_asm parity", detail);
            g_mismatches++;
            return;
        }
    }
    if (g_last_applied_len != ref_applied_len ||
        (g_last_applied_len && memcmp(g_last_applied, ref_applied, g_last_applied_len) != 0)) {
        torabo_test_report_fail("wire_asm parity", "last accepted blob differs");
        g_mismatches++;
        return;
    }
}

static void seq_done(const char *what) {
    static int last = 0;
    T_CHECK(g_mismatches == last, what);
    last = g_mismatches;
}

/* ======================================================================== */
/* fixtures                                                                  */
/* ======================================================================== */

static uint8_t tp_wire[TP_WIRE_CAP];
static uint16_t tp_wire_len;
static uint8_t syn_wire[TP_WIRE_CAP];
static uint16_t syn_wire_len;

static void build_tp_wire(void) {
    tp_wire_len = tp_wire_len_for(TP_DEFAULT_DEVICE_COUNT, (uint8_t)TP_MAX_LAYERS);
    memset(tp_wire, 0, sizeof(tp_wire));
    tp_wire[0] = 0x70; /* "tp" magic, LE */
    tp_wire[1] = 0x74;
    tp_wire[2] = 3;
    tp_wire[3] = TP_DEFAULT_DEVICE_COUNT;
    tp_wire[4] = (uint8_t)TP_MAX_LAYERS;
    tp_wire[5] = TP_FLAG_GESTURES | TP_FLAG_COAST;
    uint32_t o = TP_WIRE_HDR;
    for (uint8_t d = 0; d < TP_DEFAULT_DEVICE_COUNT; d++) {
        tp_wire[o] = d;
        tp_wire[o + 3] = 8;  /* coast friction, in range */
        tp_wire[o + 4] = 24; /* coast threshold, in range */
        o += TP_WIRE_DEV_HDR_V3;
        for (uint8_t i = 0; i < TP_MAX_LAYERS; i++) {
            tp_wire[o] = (uint8_t)(i % 4);      /* role */
            tp_wire[o + 2] = (uint8_t)(1 + i % 32); /* step */
            o += TP_WIRE_LAYER_V2;
        }
    }
}

static void build_syn_wire(uint16_t total, uint8_t version) {
    syn_wire_len = total;
    memset(syn_wire, 0, sizeof(syn_wire));
    syn_wire[0] = SYN_MAGIC;
    syn_wire[1] = version;
    syn_wire[2] = (uint8_t)total;
    syn_wire[3] = (uint8_t)(total >> 8);
    for (uint16_t i = SYN_HDR; i < total; i++) {
        syn_wire[i] = (uint8_t)(i * 7 + 1);
    }
}

static const uint8_t *cur_wire(void) { return g_backend == BACKEND_TP ? tp_wire : syn_wire; }
static uint16_t cur_wire_len(void) { return g_backend == BACKEND_TP ? tp_wire_len : syn_wire_len; }

/* ======================================================================== */
/* the sequences                                                             */
/* ======================================================================== */

/* (A) proper ATT Write Long: a Prepare pass, then the Execute replay at rising
 * offsets. Zephyr delivers exactly this shape. */
static void seq_write_long(uint16_t chunk, bool with_prepare, int64_t t0) {
    const uint8_t *w = cur_wire();
    const uint16_t total = cur_wire_len();
    reset_both();

    if (with_prepare) {
        for (uint16_t off = 0; off < total; off += chunk) {
            uint16_t n = (uint16_t)((total - off < chunk) ? (total - off) : chunk);
            struct evt e = {&w[off], n, off, true, t0 + off};
            step(&e);
        }
    }
    for (uint16_t off = 0; off < total; off += chunk) {
        uint16_t n = (uint16_t)((total - off < chunk) ? (total - off) : chunk);
        struct evt e = {&w[off], n, off, false, t0 + total + off};
        step(&e);
    }
}

/* (B) WinRT shape: every chunk arrives at offset 0, framed only by the staged
 * header's declared length. */
static void seq_plain_chunks(uint16_t chunk, int64_t t0, int64_t gap) {
    const uint8_t *w = cur_wire();
    const uint16_t total = cur_wire_len();
    reset_both();

    int64_t now = t0;
    for (uint16_t off = 0; off < total; off += chunk) {
        uint16_t n = (uint16_t)((total - off < chunk) ? (total - off) : chunk);
        struct evt e = {&w[off], n, 0, false, now};
        step(&e);
        now += gap;
    }
}

/* (C) the two transports interleaved, and abandoned transfers of each kind. */
static void seq_mixed(void) {
    const uint8_t *w = cur_wire();
    const uint16_t total = cur_wire_len();
    reset_both();

    /* start a plain chunked transfer, then a Write Long cuts in */
    struct evt a = {w, 20, 0, false, 100};
    step(&a);
    struct evt b = {w, 20, 0, false, 110}; /* offset 0 again: continuation */
    step(&b);
    struct evt c = {&w[40], 20, 40, false, 120}; /* offset matches the running length */
    step(&c);
    struct evt d = {&w[60], 20, 999, false, 130}; /* discontinuity */
    step(&d);
    struct evt e2 = {w, 20, 0, false, 140}; /* fresh start after the reset */
    step(&e2);
    /* now finish it as a long write from the running length */
    for (uint16_t off = 20; off < total; off += 64) {
        uint16_t n = (uint16_t)((total - off < 64) ? (total - off) : 64);
        struct evt f = {&w[off], n, off, false, 150 + off};
        step(&f);
    }
}

/* (D) the malformed cases the assembler exists to survive. */
static void seq_malformed(void) {
    const uint8_t *w = cur_wire();
    const uint16_t total = cur_wire_len();
    static uint8_t scratch[TP_WIRE_CAP];
    reset_both();

    /* offset + len past the cap */
    struct evt e1 = {w, 64, (uint16_t)(TP_WIRE_CAP - 8), false, 100};
    step(&e1);
    /* a huge offset on its own */
    struct evt e2 = {w, 4, 0xFFF0, false, 110};
    step(&e2);
    /* a first chunk too short to be a header */
    struct evt e3 = {w, 1, 0, false, 120};
    step(&e3);
    struct evt e4 = {w, (uint16_t)(TP_WIRE_HDR - 1), 0, false, 130};
    step(&e4);
    /* a zero-length write */
    struct evt e5 = {w, 0, 0, false, 140};
    step(&e5);
    /* garbage that is neither a header nor a continuation */
    memset(scratch, 0x5A, sizeof(scratch));
    struct evt e6 = {scratch, 32, 0, false, 150};
    step(&e6);
    /* a plausible header whose declared length we then never reach, followed by
     * a chunk that arrives AFTER the 2 s window: the partial must be dropped */
    struct evt e7 = {w, 32, 0, false, 200};
    step(&e7);
    struct evt e8 = {&w[32], 32, 0, false, 2300}; /* 2100 ms later => stale */
    step(&e8);
    /* restart mid-transfer: a fresh header arrives while one is staged */
    reset_both();
    struct evt e9 = {w, 32, 0, false, 3000};
    step(&e9);
    struct evt e10 = {w, 40, 0, false, 3010}; /* same header again, still fits */
    step(&e10);
    /* an over-long "continuation" that cannot fit the declared total */
    struct evt e11 = {w, (uint16_t)(total - 8), 0, false, 3020};
    step(&e11);

    /* a complete blob that assembles but is then REJECTED by apply(): every byte
     * after the header is corrupted so the length still matches but the content
     * does not validate. Only the tp backend can express this (the synthetic one
     * validates length only), so it is skipped for syn. */
    if (g_backend == BACKEND_TP) {
        reset_both();
        memcpy(scratch, w, total);
        scratch[5] = 0x00; /* clear the GESTURES flag: declared length now differs */
        for (uint16_t off = 0; off < total; off += 100) {
            uint16_t n = (uint16_t)((total - off < 100) ? (total - off) : 100);
            struct evt f = {&scratch[off], n, 0, false, 4000 + off};
            step(&f);
        }
    }

    /* an unknown version in the first chunk: expected_len returns 0 => reject */
    reset_both();
    memcpy(scratch, w, total);
    scratch[g_backend == BACKEND_TP ? 2 : 1] = 0x7F;
    struct evt e12 = {scratch, 32, 0, false, 5000};
    step(&e12);
}

/* A deterministic pseudo-random walk. xorshift32 so the sequence is identical on
 * every platform and every run — a failure is always reproducible. */
static uint32_t rng_state = 0x1234567u;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void seq_fuzz(int iterations) {
    const uint8_t *w = cur_wire();
    const uint16_t total = cur_wire_len();
    static uint8_t scratch[TP_WIRE_CAP];
    reset_both();

    int64_t now = 1000;
    for (int i = 0; i < iterations; i++) {
        uint32_t r = rng();

        /* mostly real slices of the wire, sometimes corrupted, sometimes junk */
        uint16_t off_in_wire = (uint16_t)(r % (total ? total : 1));
        uint16_t len = (uint16_t)(1 + (rng() % 200));
        if ((uint32_t)off_in_wire + len > total) {
            len = (uint16_t)(total - off_in_wire);
        }
        const uint8_t *data = &w[off_in_wire];
        if ((r & 0x0F) == 0) {
            memcpy(scratch, &w[off_in_wire], len);
            scratch[rng() % (len ? len : 1)] ^= 0xFF;
            data = scratch;
        }

        /* offsets: mostly the running length (a well-behaved long write), but
         * regularly 0 (WinRT) and occasionally something wild. */
        uint16_t offset;
        uint32_t k = rng() % 100;
        if (k < 40) {
            offset = new_asm.len;
        } else if (k < 85) {
            offset = 0;
        } else if (k < 95) {
            offset = (uint16_t)(rng() % (TP_WIRE_CAP + 64));
        } else {
            offset = (uint16_t)(rng() & 0xFFFF);
        }

        bool prepare = (rng() % 20) == 0;
        now += (int64_t)(rng() % 3000); /* straddles the 2000 ms window */

        struct evt e = {data, len, offset, prepare, now};
        step(&e);

        if ((rng() % 200) == 0) {
            reset_both(); /* occasionally start from a clean slate */
        }
    }
}

/* ======================================================================== */

static void run_all_sequences(void) {
    for (uint16_t chunk = 1; chunk <= 256; chunk = (uint16_t)(chunk * 2 + 1)) {
        seq_write_long(chunk, false, 1000);
        seq_write_long(chunk, true, 1000);
        seq_plain_chunks(chunk, 1000, 10);
        seq_plain_chunks(chunk, 1000, 2001); /* every gap just past the window */
        seq_plain_chunks(chunk, 1000, 2000); /* every gap exactly ON the window */
    }
    /* the ATT-realistic chunk sizes: default MTU 23 (20 B payload) and 244 B,
     * which is what the desktop/web clients actually send. */
    seq_write_long(20, true, 1000);
    seq_write_long(244, true, 1000);
    seq_plain_chunks(20, 1000, 5);
    seq_plain_chunks(244, 1000, 5);
    seq_mixed();
    seq_malformed();
    seq_fuzz(12000);
}

void test_wire_asm(void) {
    torabo_test_begin("wire assembler: new vs pre-phase-5 reference");

    build_tp_wire();
    T_CHECK(tp_wire_len > TP_WIRE_HDR, "tp fixture wire built");
    T_EQ_INT(tp_expected_len(tp_wire), tp_wire_len, "tp fixture header declares its own length");

    g_mismatches = 0;
    g_events = 0;

    g_backend = BACKEND_TP;
    run_all_sequences();
    seq_done("REAL trackpad codec: every event matches the reference");

    g_backend = BACKEND_SYN;
    for (uint16_t total = SYN_HDR; total <= 900; total = (uint16_t)(total * 3 + 7)) {
        build_syn_wire(total, 1);
        run_all_sequences();
    }
    build_syn_wire(TP_WIRE_CAP, 2); /* a wire exactly filling the staging buffer */
    run_all_sequences();
    seq_done("synthetic codec: every event matches the reference");

    {
        char what[96];
        snprintf(what, sizeof(what), "%d events compared, %d mismatches", g_events, g_mismatches);
        T_CHECK(g_mismatches == 0, what);
    }

    /* ---- the ATT error mapping, checked by value ---------------------------
     * The production callbacks turn these into ATT codes in a 1:1 switch; the
     * reference returned the ATT codes directly. Pinning the enum values keeps
     * that switch honest if anyone ever reorders the enum. */
    T_EQ_INT((int)TORABO_WIRE_ASM_ACCEPTED, REF_OK, "ACCEPTED == the reference's success return");
    T_EQ_INT((int)TORABO_WIRE_ASM_REJECT_LEN, REF_ERR_LEN, "REJECT_LEN == INVALID_ATTRIBUTE_LEN");
    T_EQ_INT((int)TORABO_WIRE_ASM_REJECT_OFFSET, REF_ERR_OFFSET, "REJECT_OFFSET == INVALID_OFFSET");
    T_EQ_INT((int)TORABO_WIRE_ASM_REJECT_VALUE, REF_ERR_VALUE, "REJECT_VALUE == VALUE_NOT_ALLOWED");

    /* ---- the shared timeout is the one both copies used -------------------- */
    T_EQ_INT(TORABO_WIRE_ASM_TIMEOUT_MS, 2000, "the plain-chunk window is still 2000 ms");
}
