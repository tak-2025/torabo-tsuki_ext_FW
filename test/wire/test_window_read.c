/*
 * Client-driven WINDOWED READ — torabo_common/window_read.h.
 *
 * WHY THIS TEST EXISTS
 * The window is the only thing standing between an Android client and a
 * 1964-byte macros blob (Android's GATT stack truncates any characteristic read
 * at 512 B, with no public API to read past it). If the framing is off by one,
 * the app silently reassembles a corrupt config and writes it back. So this
 * pins the whole contract:
 *
 *   - a READ with no control frame in front of it is byte-for-byte the blob,
 *     exactly as every existing client already gets it;
 *   - control-frame detection accepts ONLY the exact 4-byte sentinel, and
 *     rejects every real settings wire this firmware speaks;
 *   - arming + reading in a loop reassembles the original blob exactly, at
 *     every boundary offset;
 *   - the window is ONE SHOT, and survives until the LAST ATT fragment of its
 *     own response (a windowed read is itself Read + Read Blob when the MTU is
 *     smaller than 512).
 *
 * The production code is header-only and Zephyr-free, so this drives the REAL
 * functions — not a copy. The only thing modelled here is bt_gatt_attr_read()'s
 * offset slicing (att_read below), which is four lines of Zephyr we cannot link
 * to on the host.
 */

#include <string.h>

#include <torabo_common/window_read.h>

#include "torabo_test.h"

/* Wire sizes the window actually has to carry, spelled out rather than pulled
 * from each feature's header: this test is about the FRAMING, and it must keep
 * working (and keep meaning the same thing) if a feature's cap moves. */
#define WR_MACROS_LEN 1964 /* DM_READ_WIRE_LEN, the whole reason this exists */
#define WR_TRACKPAD_LEN 1526 /* a fully populated 4-device trackpad v3 wire */

/* Largest blob any of the cases below builds. */
#define WR_MAX_BLOB 2048

/* What a real client can put in one ATT PDU: 247-byte MTU minus the opcode and
 * handle. Anything bigger comes back as Read Blob fragments. */
#define WR_MTU_PAYLOAD 244

/* ---------------------------------------------------------------------------
 * Zephyr's bt_gatt_attr_read(), modelled exactly (subsys/bluetooth/host/gatt.c):
 * an offset past the value is an ATT error, an offset AT the value is a legal
 * empty read (that is how a Read Blob loop terminates), and otherwise it copies
 * min(buf_len, value_len - offset).
 */
#define ATT_ERR_INVALID_OFFSET (-7) /* stands in for BT_GATT_ERR(...); only its sign matters */

static int att_read(uint8_t *dst, uint16_t dst_cap, uint16_t att_offset, const uint8_t *value,
                    uint16_t value_len) {
    if (att_offset > value_len) {
        return ATT_ERR_INVALID_OFFSET;
    }
    uint16_t n = (uint16_t)(value_len - att_offset);
    if (n > dst_cap) {
        n = dst_cap;
    }
    memcpy(dst, value + att_offset, n);
    return (int)n;
}

/* ---------------------------------------------------------------------------
 * The two READ paths, as the GATT callbacks run them.
 *
 * Both re-encode the wire into the scratch on EVERY entry, because the real
 * callbacks do: a Read Blob re-enters the callback once per fragment, and the
 * windowed response is stamped IN PLACE over four blob bytes, so the re-encode
 * is what makes that safe. Modelling it any other way would test something the
 * firmware does not do.
 */

struct wr_chr {
    struct torabo_window_read window;
    const uint8_t *blob; /* what encode_fn would produce */
    uint16_t blob_len;
    uint8_t scratch[TORABO_WINDOW_READ_HDR + WR_MAX_BLOB];
};

static void wr_encode(struct wr_chr *c) {
    memcpy(&c->scratch[TORABO_WINDOW_READ_HDR], c->blob, c->blob_len);
}

/* One ATT read fragment against the characteristic, disarmed or armed. */
static int wr_gatt_read(struct wr_chr *c, uint8_t *dst, uint16_t dst_cap, uint16_t att_offset) {
    wr_encode(c);

    if (c->window.armed) {
        uint8_t spill[TORABO_WINDOW_READ_HDR];
        const uint8_t *resp = NULL;
        uint16_t rlen =
            torabo_window_read_frame(c->scratch, c->blob_len, c->window.offset, spill, &resp);
        int rc = att_read(dst, dst_cap, att_offset, resp, rlen);
        torabo_window_read_served(&c->window, att_offset, rc, rlen);
        return rc;
    }

    return att_read(dst, dst_cap, att_offset, &c->scratch[TORABO_WINDOW_READ_HDR], c->blob_len);
}

/* A whole client-side read: ATT Read then Read Blob until the value runs out.
 * Returns the assembled length, or -1 if the stack refused a fragment. */
static int wr_client_read(struct wr_chr *c, uint8_t *out, uint16_t out_cap, uint16_t mtu) {
    uint16_t got = 0;
    for (;;) {
        uint8_t frag[512];
        uint16_t cap = mtu < sizeof(frag) ? mtu : (uint16_t)sizeof(frag);
        int n = wr_gatt_read(c, frag, cap, got);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return (int)got;
        }
        if ((uint16_t)(got + n) > out_cap) {
            return -1;
        }
        memcpy(out + got, frag, (size_t)n);
        got = (uint16_t)(got + n);
        /* A short fragment means the value ended exactly here. */
        if ((uint16_t)n < cap) {
            return (int)got;
        }
    }
}

/* Build the 4-byte control frame for @off. */
static void wr_ctrl(uint8_t *out, uint16_t off) {
    out[0] = TORABO_WINDOW_READ_TAG0;
    out[1] = TORABO_WINDOW_READ_TAG1;
    out[2] = (uint8_t)(off & 0xFFu);
    out[3] = (uint8_t)(off >> 8);
}

/* The WRITE callback's control-frame branch: returns true when the write was
 * consumed as a control frame. */
static bool wr_gatt_write(struct wr_chr *c, const uint8_t *buf, uint16_t len, uint16_t offset,
                          bool is_prepare) {
    if (!torabo_window_read_is_ctrl(buf, len, offset, is_prepare)) {
        return false;
    }
    torabo_window_read_arm(&c->window, buf);
    return true;
}

/* Deterministic pseudo-random filler: every byte value occurs, including 0xFF
 * and the 'W' tag, so a framing bug cannot hide behind a tidy pattern. */
static void wr_fill(uint8_t *b, uint16_t n, uint8_t seed) {
    uint8_t x = seed;
    for (uint16_t i = 0; i < n; i++) {
        x = (uint8_t)(x * 31u + 17u);
        b[i] = x;
    }
}

/* ---------------------------------------------------------------------------
 * The cases.
 */

static uint8_t blob[WR_MAX_BLOB];
static uint8_t assembled[WR_MAX_BLOB];

/* Read the whole blob through repeated windows, exactly as the app will:
 * arm at `off`, read back [off][total][data], advance by the data length.
 * Returns the assembled length, or -1 on any inconsistency. */
static int wr_window_walk(struct wr_chr *c, uint8_t *out, uint16_t mtu, uint16_t *windows) {
    uint16_t off = 0;
    uint16_t n_windows = 0;

    for (;;) {
        uint8_t ctrl[TORABO_WINDOW_READ_CTRL_LEN];
        wr_ctrl(ctrl, off);
        if (!wr_gatt_write(c, ctrl, sizeof(ctrl), 0, false)) {
            return -1;
        }

        uint8_t resp[TORABO_WINDOW_READ_MAX_RESP];
        int n = wr_client_read(c, resp, sizeof(resp), mtu);
        n_windows++;
        if (n < (int)TORABO_WINDOW_READ_HDR || n > (int)TORABO_WINDOW_READ_MAX_RESP) {
            return -1;
        }

        uint16_t echoed = (uint16_t)(resp[0] | ((uint16_t)resp[1] << 8));
        uint16_t total = (uint16_t)(resp[2] | ((uint16_t)resp[3] << 8));
        if (echoed != off || total != c->blob_len) {
            return -1;
        }

        uint16_t data = (uint16_t)(n - (int)TORABO_WINDOW_READ_HDR);
        if (data == 0) {
            break;
        }
        memcpy(out + off, &resp[TORABO_WINDOW_READ_HDR], data);
        off = (uint16_t)(off + data);
    }

    if (windows) {
        *windows = n_windows;
    }
    return (int)off;
}

static void case_constants(void) {
    T_EQ_INT(TORABO_WINDOW_READ_TAG0, 0xFF, "control frame byte0 is 0xFF");
    T_EQ_INT(TORABO_WINDOW_READ_TAG1, 0x57, "control frame byte1 is 'W'");
    T_EQ_INT(TORABO_WINDOW_READ_CTRL_LEN, 4, "control frame is exactly 4 bytes");
    T_EQ_INT(TORABO_WINDOW_READ_HDR, 4, "response header is [offset u16][total u16]");
    T_EQ_INT(TORABO_WINDOW_READ_MAX_RESP, 512, "response never exceeds Android's 512 B ceiling");
    T_EQ_INT(TORABO_WINDOW_READ_MAX_DATA, 508, "so at most 508 payload bytes per window");
}

static void case_ctrl_detection(void) {
    uint8_t f[8];

    wr_ctrl(f, 0);
    T_CHECK(torabo_window_read_is_ctrl(f, 4, 0, false), "the exact sentinel is a control frame");

    wr_ctrl(f, 1234);
    T_CHECK(torabo_window_read_is_ctrl(f, 4, 0, false), "...at any offset value");

    T_CHECK(!torabo_window_read_is_ctrl(f, 3, 0, false), "3 bytes is not a control frame");
    T_CHECK(!torabo_window_read_is_ctrl(f, 5, 0, false), "5 bytes is not a control frame");
    T_CHECK(!torabo_window_read_is_ctrl(f, 4, 4, false), "a non-zero ATT offset is not one");
    T_CHECK(!torabo_window_read_is_ctrl(f, 4, 0, true), "a Prepare Write chunk is not one");
    T_CHECK(!torabo_window_read_is_ctrl(NULL, 4, 0, false), "a NULL payload is not one");

    f[1] = 0x58;
    T_CHECK(!torabo_window_read_is_ctrl(f, 4, 0, false), "byte1 must be exactly 'W'");
    wr_ctrl(f, 7);
    f[0] = 0xFE;
    T_CHECK(!torabo_window_read_is_ctrl(f, 4, 0, false), "byte0 must be exactly 0xFF");

    /* The real first four bytes of every settings WRITE wire this firmware
     * accepts. None of them can be mistaken for a control frame -- which is the
     * safety argument the whole design rests on, so it is asserted, not assumed.
     * (Length alone already saves led/combos, whose writes are fixed at 72 B and
     * 28 B; these check the FIRST BYTE, which is the part that must hold even
     * for a 4-byte fragment of one.) */
    static const struct {
        const char *what;
        uint8_t b[4];
    } wires[] = {
        {"trackball magic 0x7A74 LE", {0x74, 0x7A, 0x02, 0x00}},
        {"trackpad magic 0x7470 LE", {0x70, 0x74, 0x03, 0x01}},
        {"encoder magic 0x6E65 LE", {0x65, 0x6E, 0x01, 0x0A}},
        {"led magic 0x656C LE", {0x6C, 0x65, 0x01, 0x03}},
        {"timing version byte", {0x01, 0x03, 0x20, 0x00}},
        {"macros v1 steps write", {0x01, 0x00, 0x02, 0x00}},
        {"macros v2 name write", {0x02, 0x00, 0x01, 0x05}},
        {"combos v1 slot write", {0x01, 0x00, 0x00, 0x00}},
    };
    for (size_t i = 0; i < sizeof(wires) / sizeof(wires[0]); i++) {
        T_CHECK(!torabo_window_read_is_ctrl(wires[i].b, 4, 0, false), wires[i].what);
    }
}

static void case_plain_read_unchanged(struct wr_chr *c) {
    memset(assembled, 0, sizeof(assembled));
    int n = wr_client_read(c, assembled, sizeof(assembled), WR_MTU_PAYLOAD);
    T_EQ_INT(n, c->blob_len, "un-armed READ still returns the whole blob length");
    T_EQ_MEM(assembled, c->blob, c->blob_len, "un-armed READ is byte-identical to the blob");
    T_CHECK(!c->window.armed, "a plain READ leaves the window disarmed");
}

static void case_window_walk(struct wr_chr *c, const char *label, uint16_t mtu) {
    memset(assembled, 0, sizeof(assembled));
    uint16_t windows = 0;
    int n = wr_window_walk(c, assembled, mtu, &windows);

    T_EQ_INT(n, c->blob_len, label);
    T_EQ_MEM(assembled, c->blob, c->blob_len, "...and every byte matches the whole-blob read");

    /* ceil(len / 508) windows to carry it, plus the final empty one that tells
     * the app it is done. */
    uint16_t want = (uint16_t)((c->blob_len + TORABO_WINDOW_READ_MAX_DATA - 1) /
                               TORABO_WINDOW_READ_MAX_DATA);
    T_EQ_INT(windows, want + 1, "...in the minimum number of windows (+1 empty terminator)");
    T_CHECK(!c->window.armed, "...and the window is released at the end");
}

/* Expected payload length of a window at @off over a blob of @total bytes:
 * min(508, total - off), and nothing at all once off has reached the end. */
static uint16_t wr_expect_data(uint16_t total, uint16_t off) {
    if (off >= total) {
        return 0;
    }
    uint16_t n = (uint16_t)(total - off);
    return n > TORABO_WINDOW_READ_MAX_DATA ? (uint16_t)TORABO_WINDOW_READ_MAX_DATA : n;
}

/* Arm one window at @off, read it back, and check the header and the slice.
 * Returns the response length. */
static int wr_one_window(struct wr_chr *c, uint16_t off, uint16_t mtu, const char *what) {
    uint8_t ctrl[TORABO_WINDOW_READ_CTRL_LEN];
    wr_ctrl(ctrl, off);
    T_CHECK(wr_gatt_write(c, ctrl, sizeof(ctrl), 0, false), "control frame accepted");

    uint8_t resp[TORABO_WINDOW_READ_MAX_RESP];
    int n = wr_client_read(c, resp, sizeof(resp), mtu);

    uint16_t want = wr_expect_data(c->blob_len, off);
    T_EQ_INT(n, (int)TORABO_WINDOW_READ_HDR + want, what);

    uint16_t echoed = (uint16_t)(resp[0] | ((uint16_t)resp[1] << 8));
    uint16_t reported = (uint16_t)(resp[2] | ((uint16_t)resp[3] << 8));
    T_EQ_INT(echoed, off, "...header echoes the requested offset");
    T_EQ_INT(reported, c->blob_len, "...header reports the full blob length");
    if (want > 0) {
        T_EQ_MEM(&resp[TORABO_WINDOW_READ_HDR], c->blob + off, want,
                 "...payload is the right slice of the blob");
    }
    T_CHECK(!c->window.armed, "...and the window is released (one shot)");
    return n;
}

static void case_boundaries(struct wr_chr *c) {
    const uint16_t total = c->blob_len;
    const struct {
        uint16_t off;
        const char *what;
    } cases[] = {
        {0, "offset 0"},
        {TORABO_WINDOW_READ_MAX_DATA, "offset 508 (exactly one window in)"},
        {(uint16_t)(total - 1), "offset total-1 returns exactly 1 byte"},
        {total, "offset == total returns the 4-byte header alone"},
        {(uint16_t)(total + 1), "offset total+1 returns the 4-byte header alone"},
        {0xFFFF, "a wildly out-of-range offset returns the header alone"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        (void)wr_one_window(c, cases[i].off, WR_MTU_PAYLOAD, cases[i].what);
    }

    /* The in-place header stamping must not survive into the next read: the
     * wire is re-encoded on every entry, so the four bytes it overwrote come
     * back. This is the one thing that could silently corrupt a blob. */
    case_plain_read_unchanged(c);
}

static void case_one_shot(struct wr_chr *c) {
    uint8_t ctrl[TORABO_WINDOW_READ_CTRL_LEN];
    const uint16_t off = (uint16_t)(c->blob_len / 2);

    wr_ctrl(ctrl, off);
    T_CHECK(wr_gatt_write(c, ctrl, sizeof(ctrl), 0, false), "arm a window mid-blob");
    T_CHECK(c->window.armed, "the window is armed before the read");

    uint8_t resp[TORABO_WINDOW_READ_MAX_RESP];
    int n = wr_client_read(c, resp, sizeof(resp), WR_MTU_PAYLOAD);
    T_EQ_INT(n, (int)TORABO_WINDOW_READ_HDR + wr_expect_data(c->blob_len, off),
             "the windowed read returns header + slice");
    T_CHECK(!c->window.armed, "serving the response releases the window (one shot)");

    /* And the very next read is a plain whole-blob read again. */
    case_plain_read_unchanged(c);
}

static void case_survives_fragmentation(struct wr_chr *c) {
    /* A 512-byte response over a 20-byte ATT payload is 26 fragments. If the
     * window were released on the first one, fragment 2 onwards would come from
     * the whole blob and the app would assemble garbage. */
    uint8_t ctrl[TORABO_WINDOW_READ_CTRL_LEN];
    wr_ctrl(ctrl, 0);
    T_CHECK(wr_gatt_write(c, ctrl, sizeof(ctrl), 0, false), "arm a window at 0");

    uint16_t want = wr_expect_data(c->blob_len, 0);
    uint8_t resp[TORABO_WINDOW_READ_MAX_RESP];
    int n = wr_client_read(c, resp, sizeof(resp), 20);
    T_EQ_INT(n, (int)TORABO_WINDOW_READ_HDR + want,
             "a windowed read reassembles across 20-byte ATT fragments");
    T_EQ_MEM(&resp[TORABO_WINDOW_READ_HDR], c->blob, want,
             "...and every fragment came from the window, not the whole blob");
    T_CHECK(!c->window.armed, "...released only after the LAST fragment");
}

static void case_rearm_overwrites(struct wr_chr *c) {
    uint8_t ctrl[TORABO_WINDOW_READ_CTRL_LEN];
    const uint16_t first = 100;
    const uint16_t second = (uint16_t)(c->blob_len > 400 ? c->blob_len - 400 : c->blob_len / 4);

    wr_ctrl(ctrl, first);
    T_CHECK(wr_gatt_write(c, ctrl, sizeof(ctrl), 0, false), "arm once");
    wr_ctrl(ctrl, second);
    T_CHECK(wr_gatt_write(c, ctrl, sizeof(ctrl), 0, false), "re-arm without reading in between");
    T_EQ_INT(c->window.offset, second, "the newest unread control frame wins");

    (void)wr_one_window(c, second, WR_MTU_PAYLOAD,
                        "the read serves the second window, echoing its offset");
}

static void case_empty_blob(void) {
    /* Nothing configured yet: total == 0. Every offset is past the end, so the
     * app gets the header alone and learns the length is 0 without a second
     * round trip. */
    static struct wr_chr c;
    memset(&c, 0, sizeof(c));
    c.blob = blob;
    c.blob_len = 0;

    uint8_t ctrl[TORABO_WINDOW_READ_CTRL_LEN];
    wr_ctrl(ctrl, 0);
    T_CHECK(wr_gatt_write(&c, ctrl, sizeof(ctrl), 0, false), "arm on a zero-length wire");

    uint8_t resp[TORABO_WINDOW_READ_MAX_RESP];
    int n = wr_client_read(&c, resp, sizeof(resp), WR_MTU_PAYLOAD);
    T_EQ_INT(n, (int)TORABO_WINDOW_READ_HDR, "a zero-length wire returns the header alone");
    T_EQ_INT((uint16_t)(resp[2] | ((uint16_t)resp[3] << 8)), 0, "...reporting total == 0");
    T_CHECK(!c.window.armed, "...and releases the window");
}

static void run_blob(uint16_t len, uint8_t seed, const char *walk_label) {
    static struct wr_chr c;
    memset(&c, 0, sizeof(c));
    wr_fill(blob, len, seed);
    c.blob = blob;
    c.blob_len = len;

    case_plain_read_unchanged(&c);
    case_window_walk(&c, walk_label, WR_MTU_PAYLOAD);
    case_boundaries(&c);
    case_one_shot(&c);
    case_survives_fragmentation(&c);
    case_rearm_overwrites(&c);
}

void test_window_read(void) {
    torabo_test_begin("windowed READ (Android 512B ceiling)");

    case_constants();
    case_ctrl_detection();
    case_empty_blob();

    /* The blob that forced this feature to exist. */
    run_blob(WR_MACROS_LEN, 0x5A, "macros 1964 B reassembles through windows");
    /* A big trackpad wire: a different remainder in the last window. */
    run_blob(WR_TRACKPAD_LEN, 0xC3, "trackpad 1526 B reassembles through windows");
    /* Exactly one full window, so the terminator is the only empty one. */
    run_blob(TORABO_WINDOW_READ_MAX_DATA, 0x11, "a 508 B wire is exactly one window");
    /* One byte over, so the last window carries a single byte. */
    run_blob(TORABO_WINDOW_READ_MAX_DATA + 1, 0x22, "a 509 B wire spills one byte into a second");
}
