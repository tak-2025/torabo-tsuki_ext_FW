/*
 * Encoder rotation router — a ZMK *sensor* behavior.
 *
 * Same shape as ZMK's `zmk,behavior-sensor-rotate-var` (&inc_dec_kp), except the
 * clockwise/counter-clockwise bindings are not fixed in devicetree: we look them
 * up in the runtime store for the layer the event arrived on. So the keymap holds
 * one immutable entry —
 *
 *     sensor-bindings = <&enc_cfg>;
 *
 * — and what the knob actually does is edited live in the app.
 *
 * Detent accumulation is copied from behavior_sensor_rotate_common.c: the EC11
 * driver reports either raw ticks (val1 == 0) or degrees, and degrees must be
 * accumulated against triggers-per-rotation to avoid dropping partial turns.
 *
 * Runs on the CENTRAL: for an encoder on the peripheral half, ZMK relays the
 * sensor event across the split and the keymap applies the binding here, so HID
 * output happens on the half that actually talks to the host.
 */

#define DT_DRV_COMPAT torabo_behavior_encoder_config

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/virtual_key_position.h>

#include <zmk_encoder_config/config.h>
#include <zmk_encoder_config/binding.h>
#include <zmk_encoder_config/diag.h>

LOG_MODULE_DECLARE(enc_config, CONFIG_ZMK_ENCODER_CONFIG_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct enc_beh_config {
    const char *const *beh_refs;
    int tap_ms;
};

struct enc_beh_data {
    /* per (sensor, layer), like ZMK's own rotate behavior */
    struct sensor_value remainder[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
    int triggers[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
};

/* Accumulate this event's rotation into a whole number of detents. Verbatim logic
 * from ZMK's behavior_sensor_rotate_common.c — the encoder can report degrees, and
 * a naive int divide would silently swallow slow turns. */
static int enc_accept_data(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event,
                           const struct zmk_sensor_config *sensor_config,
                           size_t channel_data_size,
                           const struct zmk_sensor_channel_data *channel_data) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (!dev || channel_data_size < 1) {
        return -EINVAL;
    }
    struct enc_beh_data *data = dev->data;

    const struct sensor_value value = channel_data[0].value;
    const int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);
    if (sensor_index < 0 || sensor_index >= ZMK_KEYMAP_SENSORS_LEN) {
        return -EINVAL;
    }

    int triggers;
    if (value.val1 == 0) {
        triggers = value.val2; /* legacy EC11: whole ticks in val2 */
    } else {
        struct sensor_value remainder = data->remainder[sensor_index][event.layer];
        remainder.val1 += value.val1;
        remainder.val2 += value.val2;
        if (remainder.val2 >= 1000000 || remainder.val2 <= 1000000) {
            remainder.val1 += remainder.val2 / 1000000;
            remainder.val2 %= 1000000;
        }
        const int trigger_degrees = 360 / sensor_config->triggers_per_rotation;
        triggers = remainder.val1 / trigger_degrees;
        remainder.val1 %= trigger_degrees;
        data->remainder[sensor_index][event.layer] = remainder;
    }

    data->triggers[sensor_index][event.layer] = triggers;
    return 0;
}

static int enc_process(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event,
                       enum behavior_sensor_binding_process_mode mode) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (!dev) {
        return -EINVAL;
    }
    const struct enc_beh_config *cfg = dev->config;
    struct enc_beh_data *data = dev->data;

    const int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);
    if (sensor_index < 0 || sensor_index >= ZMK_KEYMAP_SENSORS_LEN) {
        return -EINVAL;
    }

    /* Not the layer that gets to act: drop what we accumulated for it, and let a
     * lower layer have the event. */
    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER) {
        data->triggers[sensor_index][event.layer] = 0;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    int triggers = data->triggers[sensor_index][event.layer];
    if (triggers == 0) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    /*
     * Resolve against the ACTIVE layer, not event.layer.
     *
     * ZMK walks the keymap's sensor rows top-down and only calls a behavior on
     * layers that actually have a sensor-binding. We install exactly one, on
     * layer 0, so event.layer is ALWAYS 0 — keying the store off it would make
     * every layer act like layer 0 and quietly throw away the whole per-layer
     * editor. (The button already does it this way, and the two must agree.)
     */
    const uint8_t layer = zmk_keymap_highest_layer_active();

    const struct enc_snapshot *s = enc_live();
    struct enc_binding desc;
    if (triggers > 0) {
        /* Count BEFORE the assigned check: an unassigned detent still proves the
         * knob turned (Torabo-Float §13 liveness). */
        for (int i = 0; i < triggers; i++) {
            enc_diag_note_rotate(true);
        }
        desc = enc_binding_for(s, layer, ENC_CW);
    } else {
        triggers = -triggers;
        for (int i = 0; i < triggers; i++) {
            enc_diag_note_rotate(false);
        }
        desc = enc_binding_for(s, layer, ENC_CCW);
    }

    /* Unassigned on this layer: fall through so a lower layer can handle it. */
    if (!enc_binding_active(&desc)) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    struct zmk_behavior_binding fire = enc_make_binding(cfg->beh_refs, &desc);

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif

    /* zmk_behavior_queue_add already defers to a work queue, so unlike the input
     * processor path there is no msgq needed here. */
    for (int i = 0; i < triggers; i++) {
        zmk_behavior_queue_add(&event, fire, true, cfg->tap_ms);
        zmk_behavior_queue_add(&event, fire, false, 0);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api enc_beh_api = {
    .sensor_binding_accept_data = enc_accept_data,
    .sensor_binding_process = enc_process,
};

#define ENC_BEH_INST(n)                                                                            \
    ENC_BEH_REFS_DEFINE(enc_beh_refs_##n, n);                                                       \
    static struct enc_beh_data enc_beh_data_##n = {};                                               \
    static const struct enc_beh_config enc_beh_config_##n = {                                       \
        .beh_refs = enc_beh_refs_##n,                                                               \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                          \
    };                                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &enc_beh_data_##n, &enc_beh_config_##n, POST_KERNEL,     \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &enc_beh_api);

DT_INST_FOREACH_STATUS_OKAY(ENC_BEH_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
