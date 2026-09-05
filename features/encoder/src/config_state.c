/*
 * Encoder runtime store: defaults -> wire codec -> lock-free publish -> NVS.
 * Shaped after trackball/src/config_state.c (the simplest sibling); the wire is
 * small enough that no long-write reassembly is needed.
 *
 * wire: magic u16 | version u8 | layer_count u8, then per layer cw/ccw/btn,
 *       each { behavior u8, mods u8, param u16 } — all little-endian.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <dt-bindings/zmk/keys.h>
#include <zmk_encoder_config/config.h>
#include <zmk_encoder_config/diag.h>

LOG_MODULE_REGISTER(enc_config, CONFIG_ZMK_ENCODER_CONFIG_LOG_LEVEL);

/* ---- diagnostic counters (Torabo-Float §13) -------------------------------
 * Bumped from the rotation router and the button processor; read by live_feed via
 * enc_diag_get(). atomic so the RX-thread button path and the sysworkq sensor path
 * never tear a count. These are pure liveness telemetry — they drive nothing. */
static atomic_t diag_cw = ATOMIC_INIT(0);
static atomic_t diag_ccw = ATOMIC_INIT(0);
static atomic_t diag_btn = ATOMIC_INIT(0);

void enc_diag_note_rotate(bool cw) { atomic_inc(cw ? &diag_cw : &diag_ccw); }

void enc_diag_note_button(void) { atomic_inc(&diag_btn); }

bool enc_diag_get(uint16_t *cw, uint16_t *ccw, uint16_t *btn) {
    if (cw) {
        *cw = (uint16_t)atomic_get(&diag_cw);
    }
    if (ccw) {
        *ccw = (uint16_t)atomic_get(&diag_ccw);
    }
    if (btn) {
        *btn = (uint16_t)atomic_get(&diag_btn);
    }
    return true; /* present: this strong def replaces live_feed's __weak fallback */
}

/* ---- little-endian helpers ------------------------------------------------ */

static inline void wr16(uint8_t *p, uint16_t v) { sys_put_le16(v, p); }
static inline uint16_t rd16(const uint8_t *p) { return sys_get_le16(p); }

/* ---- defaults -------------------------------------------------------------
 * Volume on rotation, mute on click: useful the moment the encoder is plugged in,
 * and harmless if it isn't. Every layer gets it; the app overrides per layer. */
static void fill_defaults(struct enc_snapshot *s) {
    memset(s, 0, sizeof(*s));
    s->layer_count = ENC_MAX_LAYERS;
    for (int i = 0; i < ENC_MAX_LAYERS; i++) {
        s->layers[i].cw = (struct enc_binding){.behavior = ENC_BEH_CP, .param = 0xe9};  /* vol up */
        s->layers[i].ccw = (struct enc_binding){.behavior = ENC_BEH_CP, .param = 0xea}; /* vol dn */
        s->layers[i].btn = (struct enc_binding){.behavior = ENC_BEH_CP, .param = 0xe2}; /* mute */
    }
}

/* ---- double buffer --------------------------------------------------------
 * Readers (sensor behavior / input processor) take enc_live() once per event.
 * A writer builds into a shadow and publishes with one atomic index swap, so no
 * reader ever observes a half-written snapshot and nobody needs a lock. */

static struct enc_snapshot snap[2];
static atomic_t live_idx = ATOMIC_INIT(0);
static atomic_t initialized = ATOMIC_INIT(0);

/* Static, not stack: this runs on the BT RX thread from the GATT write callback,
 * whose stack is small (the trackpad hit a hard fault doing this on the stack). */
static struct enc_snapshot apply_shadow;

static void ensure_init(void) {
    if (atomic_cas(&initialized, 0, 1)) {
        fill_defaults(&snap[0]);
        fill_defaults(&snap[1]);
    }
}

const struct enc_snapshot *enc_live(void) {
    ensure_init();
    return &snap[atomic_get(&live_idx)];
}

static void publish(const struct enc_snapshot *built) {
    ensure_init();
    int next = 1 - (int)atomic_get(&live_idx);
    snap[next] = *built; /* completes before the swap below */
    atomic_set(&live_idx, next);
}

/* ---- wire codec ----------------------------------------------------------- */

uint16_t enc_wire_len_for(uint8_t layer_count) {
    return (uint16_t)(ENC_WIRE_HDR + (uint32_t)layer_count * ENC_WIRE_LAYER);
}

/* Total wire length a blob starting with this header claims, or 0 if the header
 * is not a plausible start of one (bad magic / unknown version / a layer_count
 * this build cannot hold). The SOLE place that turns a header into a byte
 * length: enc_apply_wire() below calls it on the buffer it is validating, and
 * the GATT write assembler (gatt_service.c, torabo_common/wire_asm.h) calls it
 * for chunk framing on a possibly-incomplete header, so the two can never
 * disagree about where a wire ends. Unlike the trackball's, the encoder wire IS
 * sized by its declared layer_count, so that byte has to be checked here. */
uint16_t enc_expected_len(const uint8_t *hdr) {
    if (!hdr || rd16(&hdr[0]) != ENC_WIRE_MAGIC || hdr[2] != ENC_WIRE_VERSION) {
        return 0;
    }
    const uint8_t layer_count = hdr[3];
    if (layer_count == 0 || layer_count > ENC_MAX_LAYERS) {
        return 0;
    }
    return enc_wire_len_for(layer_count);
}

/* A descriptor we don't understand is dropped to NONE rather than fired blindly. */
static void decode_bind(const uint8_t *p, struct enc_binding *b) {
    b->behavior = p[0];
    b->mods = p[1];
    b->param = rd16(&p[2]);
    if (b->behavior > ENC_BEH_MAX) {
        *b = (struct enc_binding){0};
    }
}

static void encode_bind(uint8_t *p, const struct enc_binding *b) {
    p[0] = b->behavior;
    p[1] = b->mods;
    wr16(&p[2], b->param);
}

int enc_apply_wire(const uint8_t *buf, uint16_t len) {
    if (!buf || len < ENC_WIRE_HDR) {
        return -EINVAL;
    }
    /* Single source of truth: 0 means bad magic, unknown version, or a
     * layer_count of 0 / above this build's maximum — exactly the three
     * rejections that used to be spelled out here. */
    const uint16_t want = enc_expected_len(buf);
    if (want == 0) {
        return -EINVAL;
    }
    /* Deliberately >=, not ==, as it has always been: a blob carrying trailing
     * bytes past its declared layers is truncated, not refused. */
    if (len < want) {
        return -EINVAL;
    }
    const uint8_t layer_count = buf[3];

    ensure_init();
    memset(&apply_shadow, 0, sizeof(apply_shadow));
    apply_shadow.layer_count = layer_count;

    uint32_t o = ENC_WIRE_HDR;
    for (uint8_t i = 0; i < layer_count; i++) {
        decode_bind(&buf[o], &apply_shadow.layers[i].cw);
        decode_bind(&buf[o + ENC_WIRE_BIND], &apply_shadow.layers[i].ccw);
        decode_bind(&buf[o + ENC_WIRE_BIND * 2u], &apply_shadow.layers[i].btn);
        o += ENC_WIRE_LAYER;
    }

    publish(&apply_shadow);
    LOG_INF("enc applied: %u layers", layer_count);
    return 0;
}

int enc_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    const struct enc_snapshot *s = enc_live();
    const uint8_t layer_count =
        (s->layer_count <= ENC_MAX_LAYERS) ? s->layer_count : ENC_MAX_LAYERS;
    const uint16_t need = enc_wire_len_for(layer_count);
    if (!buf || cap < need) {
        return -ENOMEM;
    }
    memset(buf, 0, need);
    wr16(&buf[0], ENC_WIRE_MAGIC);
    buf[2] = ENC_WIRE_VERSION;
    buf[3] = layer_count;

    uint32_t o = ENC_WIRE_HDR;
    for (uint8_t i = 0; i < layer_count; i++) {
        encode_bind(&buf[o], &s->layers[i].cw);
        encode_bind(&buf[o + ENC_WIRE_BIND], &s->layers[i].ccw);
        encode_bind(&buf[o + ENC_WIRE_BIND * 2u], &s->layers[i].btn);
        o += ENC_WIRE_LAYER;
    }
    if (out_len) {
        *out_len = need;
    }
    return 0;
}

/* ---- NVS persistence (re-validated on load) ------------------------------- */

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>

#define ENC_KEY "enc"
#define ENC_VAL "wire"

int enc_save(void) {
    static uint8_t buf[ENC_WIRE_CAP]; /* static: runs on the BT RX thread */
    uint16_t len = 0;
    int rc = enc_encode_wire(buf, sizeof(buf), &len);
    if (rc) {
        return rc;
    }
    rc = settings_save_one(ENC_KEY "/" ENC_VAL, buf, len);
    LOG_INF("enc save: %d", rc);
    return rc;
}

static int enc_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, ENC_VAL, &next) || next) {
        return -ENOENT;
    }
    static uint8_t buf[ENC_WIRE_CAP];
    if (len > sizeof(buf)) {
        LOG_WRN("enc nvs size %u > cap %u; keeping defaults", (unsigned)len, (unsigned)sizeof(buf));
        return 0;
    }
    ssize_t r = read_cb(cb_arg, buf, sizeof(buf));
    if (r < 0) {
        return (int)r;
    }
    /* Never trust NVS: the layer count can change across firmware, so run the
     * blob back through the same validation the wire gets. Reject => defaults. */
    if (enc_apply_wire(buf, (uint16_t)r) == 0) {
        LOG_INF("enc loaded from NVS");
    } else {
        LOG_WRN("enc NVS blob rejected; keeping defaults");
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(enc, ENC_KEY, NULL, enc_settings_set, NULL, NULL);

#else
int enc_save(void) { return 0; }
#endif

BUILD_ASSERT(ENC_WIRE_HDR == 4, "wire header must be 4 bytes");
BUILD_ASSERT(ENC_WIRE_BIND == 4, "wire binding must be 4 bytes");
BUILD_ASSERT(ENC_WIRE_LAYER == 12, "wire layer must be cw+ccw+btn = 12 bytes");
