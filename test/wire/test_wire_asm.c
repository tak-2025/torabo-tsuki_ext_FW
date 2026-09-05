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
 * Four backends are used. `tp` is the REAL trackpad codec (tp_expected_len /
 * tp_apply_wire), `ztc` the REAL trackball codec and `enc` the REAL encoder
 * codec, so the framing is exercised against genuine header rules and genuine
 * lengths. `syn` is a synthetic codec whose expected length is simply a header
 * byte, which makes it possible to construct headers the real codec would never
 * produce — expected == 0, an expected that changes mid transfer, lengths right
 * at the cap.
 *
 * ztc and enc joined on 2026-09-05, when their GATT services moved off
 * torabo_common/gatt_simple.h (which refused offset != 0) onto this assembler:
 * at 20 layers the trackball wire is 252 B and the encoder wire is 244 B, so
 * both are at or past the 244-byte ATT single-write limit and every BLE client
 * has to split the write. They exercise two framing shapes the trackpad does
 * not: ztc's expected length does NOT depend on the declared layer_count (so a
 * wild count assembles in full and is rejected only at the end), and enc's DOES
 * (so a wild count is refused at framing time). Their header lengths differ from
 * the trackpad's too (8 and 4 vs 6) — see the REF_WIRE_HDR note below for why
 * the harness still configures both implementations with TP_WIRE_HDR.
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
#include <zmk_trackball_config/config.h>
#include <zmk_encoder_config/config.h>

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

enum backend { BACKEND_TP = 0, BACKEND_SYN = 1, BACKEND_ZTC = 2, BACKEND_ENC = 3 };
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
    switch (g_backend) {
    case BACKEND_TP:
        return tp_expected_len(hdr);
    case BACKEND_ZTC:
        return ztc_expected_len(hdr);
    case BACKEND_ENC:
        return enc_expected_len(hdr);
    default:
        return syn_expected_len(hdr);
    }
}

static int backend_apply(const uint8_t *buf, uint16_t len) {
    int ret;
    switch (g_backend) {
    case BACKEND_TP:
        ret = tp_apply_wire(buf, len);
        break;
    case BACKEND_ZTC:
        ret = ztc_apply_wire(buf, len);
        break;
    case BACKEND_ENC:
        ret = enc_apply_wire(buf, len);
        break;
    default:
        ret = syn_apply(buf, len);
        break;
    }
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
 * exactly as the original did. The other backends' headers are 4 bytes (syn,
 * enc) and 8 bytes (ztc), so the new assembler is configured with
 * hdr_len = TP_WIRE_HDR for ALL of them — otherwise the two implementations
 * would legitimately differ on a short first chunk, and the comparison would be
 * measuring the harness rather than the code. hdr_len only gates "is this first
 * chunk long enough to ask expected_len about"; the production services each
 * pass their own (ZTC_WIRE_HDR / ENC_WIRE_HDR), and expected_len is NULL-safe
 * and reads at most those many bytes either way. */

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
/* Sized on TP_WIRE_CAP like syn_wire, not on their own (smaller) caps: the
 * malformed sequence deliberately feeds slices near TP_WIRE_CAP, and the staging
 * buffers on both sides are TP_WIRE_CAP too. Only the first *_wire_len bytes of
 * each carry the real wire. */
static uint8_t ztc_wire[TP_WIRE_CAP];
static uint16_t ztc_wire_bytes;
static uint8_t enc_wire[TP_WIRE_CAP];
static uint16_t enc_wire_bytes;

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

/* A valid, fully populated v3 trackball wire: hdr(8) + ZTC_MAX_LAYERS*12 +
 * coast(4). Every field in range, so ztc_apply_wire accepts the assembled blob
 * and the ACCEPT paths (not just the reject paths) are exercised. */
static void build_ztc_wire(void) {
    ztc_wire_bytes = ztc_wire_len();
    memset(ztc_wire, 0, sizeof(ztc_wire));
    ztc_wire[0] = 0x74; /* "zt" magic 0x7A74, LE */
    ztc_wire[1] = 0x7A;
    ztc_wire[2] = 3; /* version */
    ztc_wire[3] = (uint8_t)ZTC_MAX_LAYERS;
    ztc_wire[4] = (ZTC_MAX_LAYERS > 1) ? 1 : 0; /* temp_target */
    ztc_wire[6] = 0xE8;                         /* temp_timeout_ms = 1000 LE */
    ztc_wire[7] = 0x03;
    for (uint8_t i = 0; i < ZTC_MAX_LAYERS; i++) {
        uint8_t *lp = &ztc_wire[ZTC_WIRE_HDR + (uint32_t)i * ZTC_WIRE_LAYER];
        lp[0] = (uint8_t)(i % 3);              /* x.role */
        lp[1] = (uint8_t)(i & 1);              /* x.direction */
        lp[2] = (uint8_t)(1 + (i % 32));       /* x.speed_div, in range */
        lp[4] = (uint8_t)((i + 1) % 3);        /* y.role */
        lp[5] = (uint8_t)((i + 1) & 1);        /* y.direction */
        lp[6] = (uint8_t)(1 + ((i + 5) % 32)); /* y.speed_div */
        lp[8] = (uint8_t)(i < 2 ? 1 : 0);      /* temp_enable */
    }
    uint8_t *cp = &ztc_wire[ztc_wire_bytes - ZTC_WIRE_COAST];
    cp[0] = 1;  /* coast enable */
    cp[1] = 12; /* friction, in 1..32 */
    cp[2] = 90; /* threshold, in 1..255 */
}

/* A valid v1 encoder wire: hdr(4) + ENC_MAX_LAYERS * {cw ccw btn}. */
static void build_enc_wire(void) {
    const uint8_t layers = (uint8_t)ENC_MAX_LAYERS;
    enc_wire_bytes = enc_wire_len_for(layers);
    memset(enc_wire, 0, sizeof(enc_wire));
    enc_wire[0] = 0x65; /* "en" magic 0x6E65, LE */
    enc_wire[1] = 0x6E;
    enc_wire[2] = ENC_WIRE_VERSION;
    enc_wire[3] = layers;
    for (uint8_t i = 0; i < layers; i++) {
        uint8_t *lp = &enc_wire[ENC_WIRE_HDR + (uint32_t)i * ENC_WIRE_LAYER];
        for (uint8_t w = 0; w < 3; w++) {
            uint8_t *bp = &lp[(uint32_t)w * ENC_WIRE_BIND];
            bp[0] = (uint8_t)(1 + ((i + w) % 5)); /* behavior, in 1..5 */
            bp[1] = (uint8_t)(i & 0x0F);          /* mods */
            bp[2] = (uint8_t)(0x10 + i);          /* param LE */
            bp[3] = (uint8_t)(w + 1);
        }
    }
}

static const uint8_t *cur_wire(void) {
    switch (g_backend) {
    case BACKEND_TP:
        return tp_wire;
    case BACKEND_ZTC:
        return ztc_wire;
    case BACKEND_ENC:
        return enc_wire;
    default:
        return syn_wire;
    }
}

static uint16_t cur_wire_len(void) {
    switch (g_backend) {
    case BACKEND_TP:
        return tp_wire_len;
    case BACKEND_ZTC:
        return ztc_wire_bytes;
    case BACKEND_ENC:
        return enc_wire_bytes;
    default:
        return syn_wire_len;
    }
}

/* Where the version byte sits in each backend's header (syn puts magic in [0]
 * and version in [1]; the three real wires are magic u16 then version). */
static uint16_t cur_version_off(void) { return g_backend == BACKEND_SYN ? 1u : 2u; }

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

    /* a blob that frames cleanly but is then REJECTED by apply(). Two backends
     * can express it, each for a different reason, and neither is reachable with
     * syn or enc (whose apply and expected_len agree on exactly the same
     * checks, so anything that frames also applies):
     *   tp  — clearing the GESTURES flag changes the length the header declares,
     *         so the staged bytes stop matching it,
     *   ztc — a layer_count past the build's maximum leaves ztc_expected_len
     *         (which does not look at that byte) framing the full 252 B, and
     *         ztc_apply_wire rejects the completed blob at the very end. That is
     *         the whole point of keeping layer_count out of expected_len, so it
     *         is worth a sequence of its own. */
    if (g_backend == BACKEND_TP || g_backend == BACKEND_ZTC) {
        reset_both();
        memcpy(scratch, w, total);
        if (g_backend == BACKEND_TP) {
            scratch[5] = 0x00; /* clear the GESTURES flag: declared length now differs */
        } else {
            scratch[3] = 0xFF; /* layer_count above ZTC_MAX_LAYERS */
        }
        for (uint16_t off = 0; off < total; off += 100) {
            uint16_t n = (uint16_t)((total - off < 100) ? (total - off) : 100);
            struct evt f = {&scratch[off], n, 0, false, 4000 + off};
            step(&f);
        }
    }

    /* a layer_count the build cannot hold, in the FIRST chunk. For enc this is a
     * framing rejection (the count sizes the wire, so expected_len returns 0);
     * for ztc it stages normally and is caught by apply at the end (covered just
     * above). Either way the two implementations must agree. */
    if (g_backend == BACKEND_ENC) {
        reset_both();
        memcpy(scratch, w, total);
        scratch[3] = 0xFF;
        struct evt f = {scratch, 32, 0, false, 4500};
        step(&f);
        scratch[3] = 0; /* layer_count 0 is refused too */
        struct evt f0 = {scratch, 32, 0, false, 4510};
        step(&f0);
    }

    /* an unknown version in the first chunk: expected_len returns 0 => reject */
    reset_both();
    memcpy(scratch, w, total);
    scratch[cur_version_off()] = 0x7F;
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

    build_ztc_wire();
    T_EQ_INT(ztc_expected_len(ztc_wire), ztc_wire_bytes,
             "ztc fixture header declares its own length");
    T_CHECK(ztc_wire_bytes < TP_WIRE_CAP, "ztc fixture fits the shared staging buffer");

    build_enc_wire();
    T_EQ_INT(enc_expected_len(enc_wire), enc_wire_bytes,
             "enc fixture header declares its own length");
    T_CHECK(enc_wire_bytes < TP_WIRE_CAP, "enc fixture fits the shared staging buffer");

    g_mismatches = 0;
    g_events = 0;

    g_backend = BACKEND_TP;
    run_all_sequences();
    seq_done("REAL trackpad codec: every event matches the reference");

    /* The two services that moved onto the assembler on 2026-09-05. Same driver,
     * same sequences (ATT Write Long incl. the Prepare pass, WinRT offset-0 runs,
     * mixtures, malformed sequences, 12k fuzz events), against the real codecs. */
    g_backend = BACKEND_ZTC;
    run_all_sequences();
    seq_done("REAL trackball codec: every event matches the reference");

    g_backend = BACKEND_ENC;
    run_all_sequences();
    seq_done("REAL encoder codec: every event matches the reference");

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
