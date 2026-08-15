/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * ztc_profile_led — blink the gpio status_led (index + 1) times whenever the
 * active BLE profile changes (BT_SEL 0..4 / BT_NXT / BT_PRV):
 *   profile 0 => 1 blink, profile 1 => 2 blinks, ... profile 4 => 5 blinks.
 *
 * Non-blocking: the burst runs on a k_work_delayable, so the ZMK event thread is
 * never held by k_sleep. Central-only by nature (BLE profiles are a central
 * concept); on a peripheral the event never fires.
 *
 * Note: the same status_led is also driven by the zmk-feature-status-led module
 * (advertising / connection / battery). A profile-change burst is a brief
 * one-shot; any visual overlap with that module is cosmetic only.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

#include <zmk_trackball_config/config.h>

LOG_MODULE_DECLARE(ztc_config, CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL);

#define ZTC_STATUS_LED_NODE DT_NODELABEL(status_led)

#if DT_NODE_EXISTS(ZTC_STATUS_LED_NODE)

static const struct gpio_dt_spec ztc_led = GPIO_DT_SPEC_GET(ZTC_STATUS_LED_NODE, gpios);

#define ZTC_LED_ON_MS 150
#define ZTC_LED_OFF_MS 220

static struct k_work_delayable ztc_blink_work;
static atomic_t ztc_blinks_left = ATOMIC_INIT(0); /* remaining ON phases */
static bool ztc_led_on;

/* Toggle the LED one half-step per call: OFF->ON (schedule ON_MS) or
 * ON->OFF (consume one blink, schedule OFF_MS if more remain). */
static void ztc_blink_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!ztc_led_on) {
        if (atomic_get(&ztc_blinks_left) <= 0) {
            return; /* burst finished, LED already off */
        }
        gpio_pin_set_dt(&ztc_led, 1);
        ztc_led_on = true;
        k_work_schedule(&ztc_blink_work, K_MSEC(ZTC_LED_ON_MS));
    } else {
        gpio_pin_set_dt(&ztc_led, 0);
        ztc_led_on = false;
        if (atomic_dec(&ztc_blinks_left) > 1) {
            /* atomic_dec returns the value BEFORE decrement; >1 => still >0 left */
            k_work_schedule(&ztc_blink_work, K_MSEC(ZTC_LED_OFF_MS));
        }
    }
}

static int ztc_profile_changed_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *ev = as_zmk_ble_active_profile_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    uint8_t blinks = (uint8_t)(ev->index + 1); /* 0 => 1 ... 4 => 5 */
    LOG_DBG("ztc profile %u -> %u blink(s)", ev->index, blinks);

    /* (Re)start the burst from a known OFF state. */
    gpio_pin_set_dt(&ztc_led, 0);
    ztc_led_on = false;
    atomic_set(&ztc_blinks_left, blinks);
    k_work_reschedule(&ztc_blink_work, K_MSEC(30));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ztc_profile_led, ztc_profile_changed_listener);
ZMK_SUBSCRIPTION(ztc_profile_led, zmk_ble_active_profile_changed);

static int ztc_profile_led_init(void) {
    if (!gpio_is_ready_dt(&ztc_led)) {
        LOG_WRN("ztc profile LED: gpio not ready");
        return -ENODEV;
    }
    /* Harmless if zmk-feature-status-led already configured the same pin. */
    (void)gpio_pin_configure_dt(&ztc_led, GPIO_OUTPUT_INACTIVE);
    k_work_init_delayable(&ztc_blink_work, ztc_blink_work_handler);
    return 0;
}

SYS_INIT(ztc_profile_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_NODE_EXISTS(ZTC_STATUS_LED_NODE) */
