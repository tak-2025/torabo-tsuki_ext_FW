/*
 * LED rule store: defaults -> wire codec -> lock-free publish -> NVS.
 * Same shape as the encoder/trackball stores. Lives on the CENTRAL (it owns the
 * rules and drives both LEDs); the peripheral keeps no config at all.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk_led_config/config.h>

LOG_MODULE_REGISTER(led_config, CONFIG_ZMK_LED_CONFIG_LOG_LEVEL);

/* Overridden by led_central.c on the central. The peripheral holds no rules, so
 * there is nothing for it to re-evaluate. */
__weak void led_repaint(void) {}

static inline void wr16(uint8_t *p, uint16_t v) { sys_put_le16(v, p); }
static inline uint16_t rd16(const uint8_t *p) { return sys_get_le16(p); }

/* ---- capabilities ---------------------------------------------------------
 * The app renders only what this build can actually do, so it must hear it from
 * here rather than assume. Filled in by the firmware builder per hardware pattern. */
static uint8_t led_caps(void) {
    uint8_t c = 0;
#if IS_ENABLED(CONFIG_ZMK_LED_CONFIG_LEFT_PRESENT)
    c |= LED_CAP_LEFT_PRESENT;
#endif
#if IS_ENABLED(CONFIG_ZMK_LED_CONFIG_RIGHT_PRESENT)
    c |= LED_CAP_RIGHT_PRESENT;
#endif
#if IS_ENABLED(CONFIG_ZMK_LED_CONFIG_CENTRAL_IS_LEFT)
    c |= LED_CAP_CENTRAL_IS_LEFT;
#endif
    return c;
}

/* ---- defaults -------------------------------------------------------------
 * Reproduce today's behaviour on the central (profile flash + "the other half is
 * gone"), except the disconnect warning blinks instead of holding red solid —
 * a steady red on a keyboard left disconnected is the single worst battery drain
 * in the current firmware. The left starts with the same warnings, which is
 * already more than it does today (nothing). */
static void fill_defaults(struct led_snapshot *s) {
    memset(s, 0, sizeof(*s));

    struct led_side_cfg *r = &s->sides[LED_SIDE_RIGHT];
    r->rules[0] = (struct led_rule){LED_UC_LINK_LOST, LED_CH_RED, LED_PAT_BLINK_SLOW, 0};
    r->rules[1] = (struct led_rule){LED_UC_BATTERY_LOW, LED_CH_RED, LED_PAT_DOUBLE, 15};
    r->rules[2] =
        (struct led_rule){LED_UC_PROFILE_CHANGED, LED_COLOUR_AUTO, LED_PAT_FLASH_LONG, 0};
    r->rule_count = 3;

    struct led_side_cfg *l = &s->sides[LED_SIDE_LEFT];
    l->rules[0] = (struct led_rule){LED_UC_LINK_LOST, LED_CH_RED, LED_PAT_BLINK_SLOW, 0};
    l->rules[1] = (struct led_rule){LED_UC_BATTERY_LOW, LED_CH_RED, LED_PAT_DOUBLE, 15};
    l->rule_count = 2;
}

/* ---- double buffer (readers never see a half-written table) --------------- */

static struct led_snapshot snap[2];
static atomic_t live_idx = ATOMIC_INIT(0);
static atomic_t initialized = ATOMIC_INIT(0);
static struct led_snapshot apply_shadow; /* static: the GATT write runs on the BT RX thread */

static void ensure_init(void) {
    if (atomic_cas(&initialized, 0, 1)) {
        fill_defaults(&snap[0]);
        fill_defaults(&snap[1]);
    }
}

const struct led_snapshot *led_live(void) {
    ensure_init();
    return &snap[atomic_get(&live_idx)];
}

static void publish(const struct led_snapshot *built) {
    ensure_init();
    int next = 1 - (int)atomic_get(&live_idx);
    snap[next] = *built;
    atomic_set(&live_idx, next);
}

/* ---- wire codec ----------------------------------------------------------- */

/* A rule we can't interpret is dropped rather than acted on. */
static void decode_rule(const uint8_t *p, struct led_rule *r) {
    r->usecase = p[0];
    r->colour = p[1] & LED_CH_MASK;
    r->pattern = p[2];
    r->param = p[3];
    if (r->usecase > LED_UC_MAX || r->pattern > LED_PAT_MAX) {
        *r = (struct led_rule){0};
    }
}

static void encode_rule(uint8_t *p, const struct led_rule *r) {
    p[0] = r->usecase;
    p[1] = r->colour;
    p[2] = r->pattern;
    p[3] = r->param;
}

int led_apply_wire(const uint8_t *buf, uint16_t len) {
    if (!buf || len < LED_WIRE_HDR) {
        return -EINVAL;
    }
    if (rd16(&buf[0]) != LED_WIRE_MAGIC || buf[2] != LED_WIRE_VERSION) {
        return -EINVAL;
    }
    if (len < LED_WIRE_CAP) {
        return -EINVAL;
    }
    /* buf[3] = caps, buf[4] = rule_max: both FW-authoritative. Ignore what the app
     * echoes back so it can't talk us into a shape we don't have. */

    ensure_init();
    memset(&apply_shadow, 0, sizeof(apply_shadow));

    uint32_t o = LED_WIRE_HDR;
    for (int s = 0; s < LED_SIDES; s++) {
        uint8_t n = buf[o++];
        if (n > LED_MAX_RULES) {
            n = LED_MAX_RULES;
        }
        apply_shadow.sides[s].rule_count = n;
        for (int i = 0; i < LED_MAX_RULES; i++) {
            if (i < n) {
                decode_rule(&buf[o], &apply_shadow.sides[s].rules[i]);
            }
            o += LED_WIRE_RULE;
        }
    }

    publish(&apply_shadow);
    LOG_INF("led applied: L=%u R=%u rules", apply_shadow.sides[LED_SIDE_LEFT].rule_count,
            apply_shadow.sides[LED_SIDE_RIGHT].rule_count);
    /* Show the new rules NOW. Covers the GATT write and the NVS load alike. */
    led_repaint();
    return 0;
}

int led_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    if (!buf || cap < LED_WIRE_CAP) {
        return -ENOMEM;
    }
    const struct led_snapshot *s = led_live();
    memset(buf, 0, LED_WIRE_CAP);

    wr16(&buf[0], LED_WIRE_MAGIC);
    buf[2] = LED_WIRE_VERSION;
    buf[3] = led_caps(); /* the app renders only what we advertise here */
    buf[4] = LED_MAX_RULES;
    buf[5] = 0;

    uint32_t o = LED_WIRE_HDR;
    for (int i = 0; i < LED_SIDES; i++) {
        const struct led_side_cfg *sc = &s->sides[i];
        uint8_t n = (sc->rule_count <= LED_MAX_RULES) ? sc->rule_count : LED_MAX_RULES;
        buf[o++] = n;
        for (int r = 0; r < LED_MAX_RULES; r++) {
            encode_rule(&buf[o], &sc->rules[r]);
            o += LED_WIRE_RULE;
        }
    }

    if (out_len) {
        *out_len = LED_WIRE_CAP;
    }
    return 0;
}

/* ---- NVS ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>

#define LED_KEY "ledx"
#define LED_VAL "wire"

int led_save(void) {
    static uint8_t buf[LED_WIRE_CAP];
    uint16_t len = 0;
    int rc = led_encode_wire(buf, sizeof(buf), &len);
    if (rc) {
        return rc;
    }
    rc = settings_save_one(LED_KEY "/" LED_VAL, buf, len);
    LOG_INF("led save: %d", rc);
    return rc;
}

static int led_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, LED_VAL, &next) || next) {
        return -ENOENT;
    }
    static uint8_t buf[LED_WIRE_CAP];
    if (len > sizeof(buf)) {
        LOG_WRN("led nvs size %u > cap %u; keeping defaults", (unsigned)len, (unsigned)sizeof(buf));
        return 0;
    }
    ssize_t r = read_cb(cb_arg, buf, sizeof(buf));
    if (r < 0) {
        return (int)r;
    }
    /* Never trust NVS: run it back through the same validation the wire gets. */
    if (led_apply_wire(buf, (uint16_t)r) == 0) {
        LOG_INF("led loaded from NVS");
    } else {
        LOG_WRN("led NVS blob rejected; keeping defaults");
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ledx, LED_KEY, NULL, led_settings_set, NULL, NULL);

#else
int led_save(void) { return 0; }
#endif

BUILD_ASSERT(LED_WIRE_HDR == 6, "wire header must be 6 bytes");
BUILD_ASSERT(LED_WIRE_RULE == 4, "wire rule must be 4 bytes");
BUILD_ASSERT(LED_WIRE_CAP == 6 + 2 * (1 + 8 * 4), "wire cap mismatch");
