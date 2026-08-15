/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * status_led_ext — drive the 3-colour LED on the bmp-boost LED extender to show
 * BLE profile + split-link status on the RIGHT (central) half.
 *
 * Channels (common-anode array; the anode rides the trackpad-ext POW rail
 * P0.24, so the LED only lights while the ext pointing device is powered):
 *   red = P1.11   yellow-green = P1.10   "green" = P1.02 (datasheet Y)
 * Each colour is cathode-driven, so the devicetree marks the gpio
 * GPIO_ACTIVE_LOW and gpio_pin_set_dt(_, 1) lights it.
 *
 * Behaviour:
 *   - On a BLE profile change, flash that profile's colour combo for SHOW_MS,
 *     then return to idle:
 *       0 = green   1 = yellow-green   2 = red+green
 *       3 = red+yellow-green           4 = green+yellow-green
 *   - While the left (peripheral) split link is down, red stays ON until it
 *     reconnects.
 *
 * Link status is tracked via a raw bt_conn_cb (connected/disconnected +
 * bt_conn_get_info().role == BT_CONN_ROLE_CENTRAL), NOT
 * zmk_split_peripheral_status_changed: that ZMK event is only ever raised by
 * split/bluetooth/peripheral.c, which is compiled into the LEFT (peripheral)
 * image, so a central-side listener for it never fires and red would stay on
 * forever regardless of actual link state. See board.c's
 * is_split_peripheral_conn() for the same pattern.
 *
 * Modelled on ztc_profile_led.c; non-blocking (the timed clear runs on a
 * k_work_delayable, never k_sleep on the event thread).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

LOG_MODULE_REGISTER(torabo_status_led_ext, LOG_LEVEL_WRN);

#define TSLE_RED_NODE DT_NODELABEL(tsle_red)
#define TSLE_YG_NODE  DT_NODELABEL(tsle_yg)
#define TSLE_GRN_NODE DT_NODELABEL(tsle_grn)

#if DT_NODE_EXISTS(TSLE_RED_NODE) && DT_NODE_EXISTS(TSLE_YG_NODE) && DT_NODE_EXISTS(TSLE_GRN_NODE)

enum { CH_RED = 0, CH_YG, CH_GRN, CH_COUNT };

static const struct gpio_dt_spec tsle_leds[CH_COUNT] = {
    [CH_RED] = GPIO_DT_SPEC_GET(TSLE_RED_NODE, gpios),
    [CH_YG]  = GPIO_DT_SPEC_GET(TSLE_YG_NODE, gpios),
    [CH_GRN] = GPIO_DT_SPEC_GET(TSLE_GRN_NODE, gpios),
};

#define M_RED BIT(CH_RED)
#define M_YG  BIT(CH_YG)
#define M_GRN BIT(CH_GRN)

/* Colour combo per BLE profile index. */
static const uint8_t tsle_profile_mask[5] = {
    M_GRN,         /* 0: green */
    M_YG,          /* 1: yellow-green */
    M_RED | M_GRN, /* 2: red + green */
    M_RED | M_YG,  /* 3: red + yellow-green */
    M_GRN | M_YG,  /* 4: green + yellow-green */
};

static struct k_work_delayable tsle_clear_work;
static struct k_work_delayable tsle_mux_work; /* time-multiplex driver (>=2 colours) */
static bool tsle_left_lost = true; /* assume down until the link reports up */
static bool tsle_show;             /* a profile flash is currently displayed */
static uint8_t tsle_show_mask;     /* channels held by the flash */
static uint8_t tsle_mux_pos;       /* last channel index lit by the mux handler */

/*
 * The 3-colour array shares ONE anode fed from a GPIO (the trackpad-ext POW
 * rail, P0.24), which cannot source enough current to light two colours at
 * once. So whenever the effective state needs >=2 channels we never drive them
 * simultaneously: the mux handler lights ONE channel at a time and cycles fast
 * (TSLE_MUX_STEP_MS per channel) so persistence-of-vision blends them. Peak
 * current therefore stays at the single-channel budget. 0/1 channel is driven
 * statically (no flicker, no timer).
 */
#define TSLE_MUX_STEP_MS 4 /* per-channel dwell; 2 colours => ~8ms cycle (~125Hz) */

/* Effective channel mask: active flash OR-ed with the disconnect-red overlay. */
static uint8_t tsle_effective_mask(void) {
    uint8_t mask = tsle_show ? tsle_show_mask : 0;
    if (tsle_left_lost) {
        mask |= M_RED;
    }
    return mask;
}

/* Drive exactly one channel on (ch in 0..CH_COUNT-1), all others off. */
static void tsle_drive_one(int ch) {
    for (int i = 0; i < CH_COUNT; i++) {
        gpio_pin_set_dt(&tsle_leds[i], (i == ch) ? 1 : 0);
    }
}

/* Statically drive whatever channels are set in mask (used for 0/1 channel). */
static void tsle_drive_static(uint8_t mask) {
    for (int i = 0; i < CH_COUNT; i++) {
        gpio_pin_set_dt(&tsle_leds[i], (mask & BIT(i)) ? 1 : 0);
    }
}

/* Cycle to the next set channel; reschedule while >=2 channels are requested.
 * Re-reads live state each tick so it self-heals back to the correct static
 * state if the request drops to <=1 channel (no simultaneous multi-drive). */
static void tsle_mux_handler(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t mask = tsle_effective_mask();
    if (POPCOUNT(mask) < 2) {
        tsle_drive_static(mask);
        return;
    }
    int ch = tsle_mux_pos;
    for (int step = 0; step < CH_COUNT; step++) {
        ch = (ch + 1) % CH_COUNT;
        if (mask & BIT(ch)) {
            break;
        }
    }
    tsle_mux_pos = (uint8_t)ch;
    tsle_drive_one(ch);
    k_work_reschedule(&tsle_mux_work, K_MSEC(TSLE_MUX_STEP_MS));
}

/* Apply the current state: static for 0/1 channel, time-multiplex for >=2. */
static void tsle_apply(void) {
    uint8_t mask = tsle_effective_mask();
    if (POPCOUNT(mask) >= 2) {
        k_work_reschedule(&tsle_mux_work, K_NO_WAIT); /* start/continue muxing */
    } else {
        (void)k_work_cancel_delayable(&tsle_mux_work);
        tsle_drive_static(mask);
    }
}

static void tsle_clear_handler(struct k_work *work) {
    ARG_UNUSED(work);
    tsle_show = false;
    tsle_show_mask = 0;
    tsle_apply();
}

static int tsle_profile_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *ev = as_zmk_ble_active_profile_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->index < ARRAY_SIZE(tsle_profile_mask)) {
        LOG_DBG("profile %u -> mask 0x%02x", ev->index, tsle_profile_mask[ev->index]);
        tsle_show_mask = tsle_profile_mask[ev->index];
        tsle_show = true;
        tsle_apply();
        k_work_reschedule(&tsle_clear_work, K_MSEC(CONFIG_TORABO_STATUS_LED_EXT_SHOW_MS));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(tsle_profile, tsle_profile_listener);
ZMK_SUBSCRIPTION(tsle_profile, zmk_ble_active_profile_changed);

/* True for the bt_conn on which this device is CENTRAL, i.e. the link to the
 * left (peripheral) half. */
static bool tsle_is_peripheral_conn(struct bt_conn *conn) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0) {
        return false;
    }
    return (info.role == BT_CONN_ROLE_CENTRAL && info.type == BT_CONN_TYPE_LE);
}

static void tsle_bt_connected_cb(struct bt_conn *conn, uint8_t err) {
    if (err || !tsle_is_peripheral_conn(conn)) {
        return;
    }
    LOG_DBG("split peripheral connected");
    tsle_left_lost = false;
    tsle_apply();
}

static void tsle_bt_disconnected_cb(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    if (!tsle_is_peripheral_conn(conn)) {
        return;
    }
    LOG_DBG("split peripheral lost");
    tsle_left_lost = true;
    tsle_apply();
}

static struct bt_conn_cb tsle_conn_callbacks = {
    .connected = tsle_bt_connected_cb,
    .disconnected = tsle_bt_disconnected_cb,
};

static int tsle_init(void) {
    for (int i = 0; i < CH_COUNT; i++) {
        if (!gpio_is_ready_dt(&tsle_leds[i])) {
            LOG_WRN("status_led_ext: gpio ch%d not ready", i);
            return -ENODEV;
        }
        (void)gpio_pin_configure_dt(&tsle_leds[i], GPIO_OUTPUT_INACTIVE);
    }
    k_work_init_delayable(&tsle_clear_work, tsle_clear_handler);
    k_work_init_delayable(&tsle_mux_work, tsle_mux_handler);
    bt_conn_cb_register(&tsle_conn_callbacks);
    tsle_apply(); /* boot: red on until the left half connects */
    return 0;
}

SYS_INIT(tsle_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* all three LED nodes exist */
