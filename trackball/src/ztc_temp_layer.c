/*
 * Copyright (c) 2024 The ZMK Contributors   (verbatim base)
 * Copyright (c) 2026 tak-2025 (ztc changes)
 * SPDX-License-Identifier: MIT
 *
 * FAITHFUL COPY of ZMK v0.3 app/src/pointing/input_processor_temp_layer.c, with
 * ONLY the changes mandated by docs/DESIGN_v2.md §11.B so that the proven state
 * machine + self-correcting zmk_layer_state_changed subscription are preserved:
 *   1. DT_DRV_COMPAT -> zmk_input_processor_ztc_temp_layer
 *   2. ZMK_LISTENER/ZMK_SUBSCRIPTION renamed (avoid global-symbol collision) and
 *      module-owned CONFIG_ZTC_TEMP_LAYER_MAX_ACTION_EVENTS
 *   3. toggle_layer treated as an INDEX consistently: every keymap-API call wraps
 *      it with zmk_keymap_layer_index_to_id (array subscripts stay index)
 *   4. handle_event: target+timeout come from the validated RAM store; target is
 *      clamped < MAX_LAYERS (never the -EINVAL/negative path); an early enable
 *      gate returns CONTINUE on layers where temp-layer is disabled. Always
 *      returns ZMK_INPUT_PROC_CONTINUE for the movement event.
 * Single ownership: the build overlay references <&ztc_temp_layer ...> instead of
 * <&zip_temp_layer ...>, so stock temp_layer is /omit-if-no-ref/ and NOT compiled.
 */

#define DT_DRV_COMPAT zmk_input_processor_ztc_temp_layer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

#include <zmk_trackball_config/config.h>

LOG_MODULE_DECLARE(ztc_config, CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL);

#define MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

struct temp_layer_config {
    int16_t require_prior_idle_ms;
    const uint16_t *excluded_positions;
    size_t num_positions;
};

struct temp_layer_state {
    uint8_t toggle_layer; /* layer INDEX */
    bool is_active;
    int64_t last_tapped_timestamp;
};

struct temp_layer_data {
    const struct device *dev;
    struct k_mutex lock;
    struct temp_layer_state state;
};

static struct k_work_delayable ztc_layer_disable_works[MAX_LAYERS];

static bool position_is_excluded(const struct temp_layer_config *config, uint32_t position) {
    if (!config->excluded_positions || !config->num_positions) {
        return false;
    }
    const uint16_t *end = config->excluded_positions + config->num_positions;
    for (const uint16_t *pos = config->excluded_positions; pos < end; pos++) {
        if (*pos == position) {
            return true;
        }
    }
    return false;
}

static bool should_quick_tap(const struct temp_layer_config *config, int64_t last_tapped,
                             int64_t current_time) {
    return (last_tapped + config->require_prior_idle_ms) > current_time;
}

/* toggle_layer is an INDEX; convert to id for the keymap API (reordering-safe). */
static void update_layer_state(struct temp_layer_state *state, bool activate) {
    if (state->is_active == activate) {
        return;
    }
    state->is_active = activate;
    if (activate) {
        zmk_keymap_layer_activate(zmk_keymap_layer_index_to_id(state->toggle_layer));
        LOG_DBG("Layer idx %d activated", state->toggle_layer);
    } else {
        zmk_keymap_layer_deactivate(zmk_keymap_layer_index_to_id(state->toggle_layer));
        LOG_DBG("Layer idx %d deactivated", state->toggle_layer);
    }
}

struct layer_state_action {
    uint8_t layer; /* INDEX */
    bool activate;
};

K_MSGQ_DEFINE(ztc_temp_layer_action_msgq, sizeof(struct layer_state_action),
              CONFIG_ZTC_TEMP_LAYER_MAX_ACTION_EVENTS, 4);

static void ztc_layer_action_work_cb(struct k_work *work) {
    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        LOG_ERR("Error locking for updating %d", ret);
        return;
    }

    struct layer_state_action action;
    while (k_msgq_get(&ztc_temp_layer_action_msgq, &action, K_MSEC(10)) >= 0) {
        if (!action.activate) {
            if (zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(action.layer))) {
                update_layer_state(&data->state, false);
            }
        } else {
            update_layer_state(&data->state, true);
        }
    }

    k_mutex_unlock(&data->lock);
}

static K_WORK_DEFINE(ztc_layer_action_work, ztc_layer_action_work_cb);

static void layer_disable_callback(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    int layer_index = ARRAY_INDEX(ztc_layer_disable_works, d_work);

    struct layer_state_action action = {.layer = layer_index, .activate = false};
    (void)k_msgq_put(&ztc_temp_layer_action_msgq, &action, K_MSEC(10));
    k_work_submit(&ztc_layer_action_work);
}

static int handle_layer_state_changed(const struct device *dev, const zmk_event_t *eh) {
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    if (!zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(data->state.toggle_layer))) {
        LOG_DBG("Deactivating layer that was activated by this processor");
        data->state.is_active = false;
        k_work_cancel_delayable(&ztc_layer_disable_works[data->state.toggle_layer]);
    }
    ret = k_mutex_unlock(&data->lock);
    if (ret < 0) {
        return ret;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_position_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    const struct temp_layer_config *cfg = dev->config;
    if (data->state.is_active && cfg->excluded_positions && cfg->num_positions > 0) {
        if (!position_is_excluded(cfg, ev->position)) {
            LOG_DBG("Position not excluded, deactivating layer");
            update_layer_state(&data->state, false);
        }
    }
    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_keycode_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    data->state.last_tapped_timestamp = ev->timestamp;
    ret = k_mutex_unlock(&data->lock);
    if (ret < 0) {
        return ret;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_state_changed_dispatcher(const struct device *dev, const zmk_event_t *eh) {
    if (as_zmk_layer_state_changed(eh) != NULL) {
        return handle_layer_state_changed(dev, eh);
    } else if (as_zmk_position_state_changed(eh) != NULL) {
        return handle_position_state_changed(dev, eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return handle_keycode_state_changed(dev, eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

#define DISPATCH_EVENT(inst)                                                                       \
    {                                                                                              \
        int err = handle_state_changed_dispatcher(DEVICE_DT_INST_GET(inst), eh);                   \
        if (err < 0) {                                                                             \
            return err;                                                                            \
        }                                                                                          \
    }

static int handle_event_dispatcher(const zmk_event_t *eh) {
    DT_INST_FOREACH_STATUS_OKAY(DISPATCH_EVENT)
    return 0;
}

/* Driver: target + timeout come from the validated RAM store; never -EINVAL. */
static int temp_layer_handle_event(const struct device *dev, struct input_event *event,
                                   uint32_t param1, uint32_t param2,
                                   struct zmk_input_processor_state *state) {
    const struct ztc_snapshot *zs = ztc_live();

    /* enable gate: if temp-layer is off for the currently-active layer, do
     * nothing. Out-of-range active index => fail open (behave like stock). */
    uint8_t active = zmk_keymap_highest_layer_active();
    if (active < ZTC_MAX_LAYERS && !zs->layers[active].temp_enable) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* validated target (INDEX) + timeout from RAM (fallback to DT cells) */
    uint8_t target = zs->temp_target;
    if (target >= MAX_LAYERS) {
        target = (uint8_t)((param1 < MAX_LAYERS) ? param1 : 0);
    }
    uint16_t timeout = zs->temp_timeout_ms; /* clamped >=50 in the store */
    if (timeout == 0) {
        timeout = (uint16_t)param2;
    }

    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ZMK_INPUT_PROC_CONTINUE; /* never drop the movement event */
    }

    const struct temp_layer_config *cfg = dev->config;

    /* Only adopt a (possibly runtime-changed) target while IDLE. A target change
     * mid-activation is deferred to the next activation, so the currently-held
     * layer always keeps a matching pending disable and can never latch stuck
     * (the v1-class sticky-layer bug). */
    if (!data->state.is_active) {
        data->state.toggle_layer = target;
    }
    uint8_t active_layer = data->state.toggle_layer; /* < MAX_LAYERS (target clamped) */

    if (!data->state.is_active &&
        !should_quick_tap(cfg, data->state.last_tapped_timestamp, k_uptime_get())) {
        struct layer_state_action action = {.layer = active_layer, .activate = true};
        (void)k_msgq_put(&ztc_temp_layer_action_msgq, &action, K_MSEC(10));
        k_work_submit(&ztc_layer_action_work);
    }

    /* Always (re)schedule a disable; never leave an activation un-timed. */
    if (timeout < ZTC_TIMEOUT_MIN) {
        timeout = ZTC_TIMEOUT_MIN;
    }
    k_work_reschedule(&ztc_layer_disable_works[active_layer], K_MSEC(timeout));

    k_mutex_unlock(&data->lock);
    return ZMK_INPUT_PROC_CONTINUE;
}

static int temp_layer_init(const struct device *dev) {
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    k_mutex_init(&data->lock);
    for (int i = 0; i < MAX_LAYERS; i++) {
        k_work_init_delayable(&ztc_layer_disable_works[i], layer_disable_callback);
    }
    return 0;
}

static const struct zmk_input_processor_driver_api temp_layer_driver_api = {
    .handle_event = temp_layer_handle_event,
};

#define NEEDS_POSITION_HANDLERS(n, ...) DT_INST_PROP_HAS_IDX(n, excluded_positions, 0)
#define NEEDS_KEYCODE_HANDLERS(n, ...) (DT_INST_PROP_OR(n, require_prior_idle_ms, 0) > 0)

ZMK_LISTENER(ztc_processor_temp_layer, handle_event_dispatcher);
ZMK_SUBSCRIPTION(ztc_processor_temp_layer, zmk_layer_state_changed);

#if DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_POSITION_HANDLERS, ||)
ZMK_SUBSCRIPTION(ztc_processor_temp_layer, zmk_position_state_changed);
#endif

#if DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_KEYCODE_HANDLERS, ||)
ZMK_SUBSCRIPTION(ztc_processor_temp_layer, zmk_keycode_state_changed);
#endif

#define TEMP_LAYER_INST(n)                                                                         \
    static struct temp_layer_data processor_temp_layer_data_##n = {};                              \
    static const uint16_t excluded_positions_##n[] = DT_INST_PROP(n, excluded_positions);          \
    static const struct temp_layer_config processor_temp_layer_config_##n = {                      \
        .require_prior_idle_ms = DT_INST_PROP_OR(n, require_prior_idle_ms, 0),                     \
        .excluded_positions = excluded_positions_##n,                                              \
        .num_positions = DT_INST_PROP_LEN(n, excluded_positions),                                  \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, temp_layer_init, NULL, &processor_temp_layer_data_##n,                \
                          &processor_temp_layer_config_##n, POST_KERNEL,                           \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &temp_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TEMP_LAYER_INST)
