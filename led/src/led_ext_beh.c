/*
 * The peripheral's LED — a dumb display, plus one local fallback.
 *
 * Normally the central decides everything and pushes a rendered (colour, pattern)
 * here by invoking this behavior over the split, so the peripheral keeps no config
 * and nothing needs syncing.
 *
 * The exception is the one thing the central provably cannot help with: when the
 * split link drops, it can no longer tell us anything. So we also watch our own
 * link locally and, while it is down, show the "I lost my partner" warning
 * ourselves. As soon as the central is back it resumes control.
 *
 * NOTE the node name must stay short: the split payload carries the behavior's
 * device name in a char[16], and a longer one is silently truncated and then
 * fails to resolve here (zmk/split/transport/types.h).
 *
 * KNOWN LOG NOISE (ZMK v0.3.0, harmless): every invoke that reaches us also
 * produces "Unhandled command type 1" + "Failed to invoke behavior led_ext:
 * -134" on the peripheral. That is NOT this behavior failing — our invoke has
 * already returned 0 by then. ZMK's zmk_split_transport_peripheral_command_handler
 * (app/src/split/peripheral.c) is missing a break/return after the
 * INVOKE_BEHAVIOR case, so it falls through into default:, returns -ENOTSUP,
 * and split_svc_run_behavior (bluetooth/service.c) logs that as a failure.
 * Upstream main already fixes it with `return err;`. The tell that the real
 * invoke succeeded: the handler's own "Failed to invoke" line (which would
 * come BEFORE the "Unhandled command" warning) never appears.
 */

#define DT_DRV_COMPAT torabo_behavior_led_ext

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>

#include <zmk_led_config/config.h>

LOG_MODULE_DECLARE(led_config, CONFIG_ZMK_LED_CONFIG_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* What to show while we are on our own. Compile-time, because a config we can't
 * be told about is no use: this is precisely the state where the central is
 * unreachable. Slow blink, not solid — a keyboard left disconnected overnight
 * with a steady LED is the worst battery drain the old firmware had. */
#define FALLBACK_COLOUR LED_CH_RED
#define FALLBACK_PATTERN LED_PAT_BLINK_SLOW

static bool linked; /* are we hearing from the central */

static int led_ext_pressed(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    /* While the link is down the central cannot be talking to us, so anything
     * arriving here is stale; ignore it rather than fight the fallback. */
    if (!linked) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    uint8_t colour, pattern;
    led_render_decode(binding->param1, &colour, &pattern);
    led_render_set(colour, pattern);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int led_ext_released(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    /* The central only ever "presses" — a release carries no new state. */
    return ZMK_BEHAVIOR_OPAQUE;
}

/* Our own link state. Unlike the central, the peripheral genuinely does raise
 * this event for itself (split/bluetooth/peripheral.c), so we can just listen. */
static int led_ext_link_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    linked = ev->connected;
    if (!linked) {
        led_render_set(FALLBACK_COLOUR, FALLBACK_PATTERN);
    } else {
        /* Go dark and wait: the central will push the real state immediately. */
        led_render_set(0, LED_PAT_SOLID);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_ext_link, led_ext_link_listener);
ZMK_SUBSCRIPTION(led_ext_link, zmk_split_peripheral_status_changed);

static int led_ext_init(const struct device *dev) {
    ARG_UNUSED(dev);
    /* Assume down until the link says otherwise, so a half that never connects
     * still tells you something is wrong. */
    linked = false;
    led_render_set(FALLBACK_COLOUR, FALLBACK_PATTERN);
    return 0;
}

static const struct behavior_driver_api led_ext_api = {
    .binding_pressed = led_ext_pressed,
    .binding_released = led_ext_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL, /* unused: the central invokes us explicitly */
};

#define LED_EXT_INST(n)                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, led_ext_init, NULL, NULL, NULL, POST_KERNEL,                        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &led_ext_api);

DT_INST_FOREACH_STATUS_OKAY(LED_EXT_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
