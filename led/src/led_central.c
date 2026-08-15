/*
 * The brain. Runs on the CENTRAL only.
 *
 * Watches everything worth showing, decides what EACH side's LED should display,
 * drives its own directly and pushes the other side's over the split. The
 * peripheral holds no config and makes no decisions (see config.h).
 *
 * Arbitration: rules are tried in order and the first whose condition holds wins,
 * so the app decides precedence by ordering them (warnings first, steady states
 * last). "Something changed" rules (profile/layer/endpoint) are one-shot flashes
 * layered on top; when the flash ends we fall back to whatever steady rule was
 * showing.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/conn.h>

#include <zmk/battery.h>
#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
/* Caps/Num/Scroll only exist when ZMK's HID indicator plumbing is compiled in.
 * Without it the event symbols aren't emitted at all, so guard everything that
 * touches them and let the CAPS_LOCK rule simply never match. */
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/events/hid_indicators_changed.h>
#endif
#include <zmk/events/layer_state_changed.h>
#include <zmk/split/central.h>

#include <zmk_led_config/config.h>

LOG_MODULE_DECLARE(led_config, CONFIG_ZMK_LED_CONFIG_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Which array slot is us. The other side is reached over the split. */
#if IS_ENABLED(CONFIG_ZMK_LED_CONFIG_CENTRAL_IS_LEFT)
#define SIDE_SELF LED_SIDE_LEFT
#define SIDE_OTHER LED_SIDE_RIGHT
#else
#define SIDE_SELF LED_SIDE_RIGHT
#define SIDE_OTHER LED_SIDE_LEFT
#endif

/* The peripheral renders whatever we invoke this behavior with. Its device name
 * is the DT node name, and the split payload only carries char[16] — a longer
 * name would be silently truncated and never resolve on the far side. */
#define LED_EXT_BEHAVIOR_DEV "led_ext"
BUILD_ASSERT(sizeof(LED_EXT_BEHAVIOR_DEV) <= 16, "behavior name must fit the split payload");

/* HID indicator bits (USB HID keyboard LED page). */
#define HID_IND_CAPS 0x02

/* Colour per index for LED_COLOUR_AUTO (profile 0,1,2... / layer 0,1,2...).
 * A fixed colour there would make every profile look identical. */
static const uint8_t auto_colour[LED_COLOUR_COUNT] = {
    LED_CH_GRN,
    LED_CH_YG,
    LED_CH_RED,
    LED_CH_RED | LED_CH_GRN,
    LED_CH_RED | LED_CH_YG,
    LED_CH_GRN | LED_CH_YG,
    LED_CH_RED | LED_CH_YG | LED_CH_GRN,
};

/* ---- observed state ------------------------------------------------------- */

static bool peer_connected;               /* is the other half there */
static uint8_t batt_self = 100;           /* our own cell */
static uint8_t batt_peer = 100;           /* the other half's, via the split proxy */
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
static zmk_hid_indicators_t hid_inds;     /* caps/num/scroll */
#endif
static uint8_t mods_held;                 /* current explicit modifiers */

static struct k_work_delayable restore_work[LED_SIDES]; /* end of a one-shot flash */

/* While a one-shot notice is showing, a steady re-evaluation must not paint over
 * it — otherwise any modifier press or battery report during the 1.5 s profile
 * flash cuts it to a blink. restore_work puts the steady display back when the
 * flash is genuinely done. */
static int64_t notice_until[LED_SIDES];

/* ---- rendering ------------------------------------------------------------ */

static void push_to_peer(uint8_t colour, uint8_t pattern) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = LED_EXT_BEHAVIOR_DEV,
        .param1 = led_render_encode(colour, pattern),
        .param2 = 0,
    };
    struct zmk_behavior_binding_event ev = {.position = 0, .timestamp = k_uptime_get()};
    /* Peripheral 0: this keyboard only ever has one. */
    (void)zmk_split_central_invoke_behavior(0, &binding, ev, true);
}

static void show(int side, uint8_t colour, uint8_t pattern) {
    if (side == SIDE_SELF) {
        led_render_set(colour, pattern);
    } else if (peer_connected) {
        /* If the link is down we cannot reach it — and it falls back to its own
         * "I lost my partner" rule anyway, which is exactly what should show. */
        push_to_peer(colour, pattern);
    }
}

/* ---- rule evaluation ------------------------------------------------------ */

static uint8_t battery_for(int side) { return (side == SIDE_SELF) ? batt_self : batt_peer; }

/* Does this steady-state rule currently hold? "Something changed" rules are not
 * states, so they never match here — they are fired directly by their event. */
static bool rule_holds(int side, const struct led_rule *r) {
    switch (r->usecase) {
    case LED_UC_LINK_LOST:
        return !peer_connected;
    case LED_UC_BATTERY_LOW:
        return battery_for(side) <= r->param;
    case LED_UC_CAPS_LOCK:
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        return (hid_inds & HID_IND_CAPS) != 0;
#else
        return false; /* firmware built without HID indicators */
#endif
    case LED_UC_MODIFIER:
        return r->param != 0 && (mods_held & r->param) != 0;
    default:
        return false;
    }
}

/* Resting display: the first steady rule that holds, else dark. */
static void apply_steady(int side) {
    if (k_uptime_get() < notice_until[side]) {
        return; /* a notice flash owns the LED right now */
    }
    const struct led_snapshot *s = led_live();
    const struct led_side_cfg *sc = &s->sides[side];

    for (int i = 0; i < sc->rule_count && i < LED_MAX_RULES; i++) {
        const struct led_rule *r = &sc->rules[i];
        if (rule_holds(side, r)) {
            show(side, r->colour, r->pattern);
            return;
        }
    }
    show(side, 0, LED_PAT_SOLID); /* nothing to say: off */
}

static void apply_all(void) {
    for (int s = 0; s < LED_SIDES; s++) {
        apply_steady(s);
    }
}

static void restore_side(int side) {
    notice_until[side] = 0; /* the flash is over; the steady display owns it again */
    apply_steady(side);
}
static void restore_left_cb(struct k_work *work) {
    ARG_UNUSED(work);
    restore_side(LED_SIDE_LEFT);
}
static void restore_right_cb(struct k_work *work) {
    ARG_UNUSED(work);
    restore_side(LED_SIDE_RIGHT);
}

/* One-shot notice ("the profile changed"), layered over the resting display.
 * `index` selects the colour when the rule asked for LED_COLOUR_AUTO. */
static void fire_notice(uint8_t usecase, uint8_t index) {
    const struct led_snapshot *s = led_live();

    for (int side = 0; side < LED_SIDES; side++) {
        const struct led_side_cfg *sc = &s->sides[side];
        for (int i = 0; i < sc->rule_count && i < LED_MAX_RULES; i++) {
            const struct led_rule *r = &sc->rules[i];
            if (r->usecase != usecase) {
                continue;
            }
            uint8_t colour =
                (r->colour == LED_COLOUR_AUTO) ? auto_colour[index % LED_COLOUR_COUNT] : r->colour;
            show(side, colour, r->pattern);

            /* Hold the LED for the flash, then fall back to the resting display. */
            uint32_t ms = (r->pattern == LED_PAT_FLASH_LONG) ? 1600 : 1100;
            notice_until[side] = k_uptime_get() + ms;
            k_work_reschedule(&restore_work[side], K_MSEC(ms));
            break;
        }
    }
}

/* ---- event sources -------------------------------------------------------- */

static int led_state_listener(const zmk_event_t *eh) {
    bool dirty = false;

    const struct zmk_battery_state_changed *bs = as_zmk_battery_state_changed(eh);
    if (bs) {
        batt_self = bs->state_of_charge;
        dirty = true;
    }

    const struct zmk_peripheral_battery_state_changed *pbs =
        as_zmk_peripheral_battery_state_changed(eh);
    if (pbs) {
        batt_peer = pbs->state_of_charge;
        dirty = true;
    }

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    const struct zmk_hid_indicators_changed *hi = as_zmk_hid_indicators_changed(eh);
    if (hi) {
        hid_inds = hi->indicators;
        dirty = true;
    }
#endif

    /* Modifiers: zmk_modifiers_state_changed exists as a type but ZMK never raises
     * it (its own comment in behavior_hold_tap.c admits as much), so watching it
     * would wait forever. Read the live HID modifier mask on any keycode event
     * instead — that also catches mod-taps and sticky keys, which chasing
     * individual keycodes would miss. */
    if (as_zmk_keycode_state_changed(eh)) {
        uint8_t m = zmk_hid_get_explicit_mods();
        if (m != mods_held) {
            mods_held = m;
            dirty = true;
        }
    }

    if (dirty) {
        apply_all();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_state, led_state_listener);
ZMK_SUBSCRIPTION(led_state, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(led_state, zmk_peripheral_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(led_state, zmk_hid_indicators_changed);
#endif
ZMK_SUBSCRIPTION(led_state, zmk_keycode_state_changed);

/* --- one-shot notices --- */

static int led_notice_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *prof = as_zmk_ble_active_profile_changed(eh);
    if (prof) {
        fire_notice(LED_UC_PROFILE_CHANGED, (uint8_t)prof->index);
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_layer_state_changed(eh)) {
        fire_notice(LED_UC_LAYER_CHANGED, (uint8_t)zmk_keymap_highest_layer_active());
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_endpoint_changed(eh)) {
        fire_notice(LED_UC_ENDPOINT_CHANGED, 0);
        return ZMK_EV_EVENT_BUBBLE;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_notice, led_notice_listener);
ZMK_SUBSCRIPTION(led_notice, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(led_notice, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(led_notice, zmk_endpoint_changed);

/* --- split link ---
 * zmk_split_peripheral_status_changed is raised ONLY inside the peripheral image
 * (split/bluetooth/peripheral.c), so a central-side listener for it never fires —
 * status_led_ext learned this the hard way and left the note. Watch the raw
 * connection instead: on the central, our link to the peripheral is the one where
 * WE are the BLE central. */

static void link_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;
    if (err || bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }
    peer_connected = true;
    apply_all();
}

static void link_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }
    peer_connected = false;
    /* Only our own LED can be updated now — the other half is unreachable and
     * shows its own local fallback. */
    apply_steady(SIDE_SELF);
}

static struct bt_conn_cb led_conn_cb = {
    .connected = link_connected,
    .disconnected = link_disconnected,
};

/* The rule table changed under us (GATT write / NVS load): drop any notice that is
 * mid-flight — it was decided by rules that no longer exist — and repaint. */
void led_repaint(void) {
    for (int s = 0; s < LED_SIDES; s++) {
        notice_until[s] = 0;
        k_work_cancel_delayable(&restore_work[s]);
    }
    apply_all();
}

static int led_central_init(void) {
    k_work_init_delayable(&restore_work[LED_SIDE_LEFT], restore_left_cb);
    k_work_init_delayable(&restore_work[LED_SIDE_RIGHT], restore_right_cb);
    bt_conn_cb_register(&led_conn_cb);
    batt_self = zmk_battery_state_of_charge();
    apply_all();
    return 0;
}

SYS_INIT(led_central_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
