/*
 * Encoder push button -> runtime binding.
 *
 * The button is a plain GPIO, not a matrix key. Zephyr's `gpio-keys` driver turns
 * it into an INPUT_EV_KEY event; this input processor picks that event up and
 * fires whatever the store says the current layer's button should do.
 *
 * Why an input processor and not a direct GPIO callback: on a split keyboard the
 * button usually sits on the PERIPHERAL half, which cannot talk to the host. Input
 * events cross the split (that is how the left trackpad's taps already reach the
 * central), so we let `zmk,input-split` relay the key event and run this processor
 * on the central, where invoking a behavior actually produces HID.
 *
 * Press and release fire the SAME binding: we latch what we resolved on press, so
 * a layer change while the button is held cannot release a different behavior than
 * the one that went down (which would strand a modifier or a momentary layer).
 */

#define DT_DRV_COMPAT torabo_input_processor_enc_button

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>

#include <drivers/input_processor.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

#include <zmk_encoder_config/config.h>
#include <zmk_encoder_config/binding.h>
#include <zmk_encoder_config/diag.h>

LOG_MODULE_DECLARE(enc_config, CONFIG_ZMK_ENCODER_CONFIG_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ENC_BTN_QUEUE_LEN 8

struct enc_btn_config {
    uint8_t index;         /* processor index, for the virtual key position */
    uint16_t code;         /* which INPUT_EV_KEY code is our button */
    const char *const *beh_refs;
};

struct enc_btn_data {
    struct zmk_behavior_binding held; /* what we fired on press */
    uint32_t held_position;
    bool is_held;
};

/*
 * A press MUST always be matched by a release, or a &mo leaves the keyboard stuck
 * on a layer and a &kp leaves a key down. Two ways the release can go missing:
 *
 *   - the split link drops mid-press. ZMK flushes matrix POSITIONS on peripheral
 *     disconnect but not relayed input events, so our key-up never arrives;
 *   - the bounded input/msgq queues drop it under a burst of pointer traffic.
 *
 * So we never rely on the release arriving: a new press first releases whatever is
 * still held, and a lost split link releases it too. Only one encoder exists per
 * keyboard (the builder enforces it), so a single slot is enough.
 */
static struct enc_btn_data *held_owner;

static void enc_btn_enqueue(const struct zmk_behavior_binding *binding, uint32_t position,
                            bool pressed);

static void release_held(void) {
    struct enc_btn_data *d = held_owner;
    if (!d || !d->is_held) {
        return;
    }
    enc_btn_enqueue(&d->held, d->held_position, false);
    d->is_held = false;
    held_owner = NULL;
}

/* ---- deferral -------------------------------------------------------------
 * This handler runs on the input-processor thread — for a split peripheral device
 * that is the BLE RX context, where a synchronous behavior invoke (which ends in a
 * HID send) can deadlock or blow the stack. Hand the work to the system queue. */

struct enc_btn_req {
    struct zmk_behavior_binding binding;
    uint32_t position;
    bool pressed;
};

K_MSGQ_DEFINE(enc_btn_msgq, sizeof(struct enc_btn_req), ENC_BTN_QUEUE_LEN, 4);

static void enc_btn_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct enc_btn_req req;
    while (k_msgq_get(&enc_btn_msgq, &req, K_NO_WAIT) == 0) {
        struct zmk_behavior_binding_event ev = {
            .position = req.position,
            .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
        };
        (void)zmk_behavior_invoke_binding(&req.binding, ev, req.pressed);
    }
}

static K_WORK_DEFINE(enc_btn_work, enc_btn_work_cb);

static void enc_btn_enqueue(const struct zmk_behavior_binding *binding, uint32_t position,
                            bool pressed) {
    struct enc_btn_req req = {.binding = *binding, .position = position, .pressed = pressed};
    (void)k_msgq_put(&enc_btn_msgq, &req, K_NO_WAIT); /* bounded; drop if full */
    (void)k_work_submit(&enc_btn_work);
}

static int enc_btn_handle_event(const struct device *dev, struct input_event *event,
                                uint32_t param1, uint32_t param2,
                                struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);

    const struct enc_btn_config *cfg = dev->config;
    struct enc_btn_data *data = dev->data;

    if (event->type != INPUT_EV_KEY || event->code != cfg->code) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const uint32_t position =
        ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(state->input_device_index, cfg->index);

    if (event->value) {
        /* Count every press up front — before resolving the binding — so a click
         * registers in diagnostics even when this layer maps it to nothing or to a
         * silent action like mute (Torabo-Float §13; the exact "looks dead" trap). */
        enc_diag_note_button();

        /* A previous press whose release never arrived would otherwise be latched
         * over and stranded forever (stuck layer / stuck key). Let it go first. */
        release_held();

        /* Resolve now and latch, so the release fires the SAME binding even if the
         * layer changes while the button is down. */
        const uint8_t layer = zmk_keymap_highest_layer_active();
        const struct enc_binding desc = enc_binding_for(enc_live(), layer, ENC_BTN);
        if (!enc_binding_active(&desc)) {
            return ZMK_INPUT_PROC_STOP; /* unassigned: consume, do nothing */
        }
        data->held = enc_make_binding(cfg->beh_refs, &desc);
        data->held_position = position;
        data->is_held = true;
        held_owner = data;
        enc_btn_enqueue(&data->held, position, true);
    } else {
        /* Release even if this instance thinks it isn't holding: the press may have
         * been latched and then dropped from the queue. release_held() is a no-op
         * when there is nothing down. */
        release_held();
    }

    return ZMK_INPUT_PROC_STOP; /* the button is ours; don't pass it on as a click */
}

/* The button usually lives on the peripheral, so a dropped split link means its
 * key-up is gone for good. Release here rather than leave the keyboard stuck.
 * (On the central, our link to the peripheral is the connection where WE are the
 * BLE central — the host connection has us as the peripheral.) */
static void enc_btn_link_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }
    release_held();
}

static struct bt_conn_cb enc_btn_conn_cb = {
    .disconnected = enc_btn_link_disconnected,
};

static int enc_btn_init(const struct device *dev) {
    struct enc_btn_data *data = dev->data;
    data->is_held = false;
    bt_conn_cb_register(&enc_btn_conn_cb);
    return 0;
}

static const struct zmk_input_processor_driver_api enc_btn_api = {
    .handle_event = enc_btn_handle_event,
};

#define ENC_BTN_INST(n)                                                                            \
    ENC_BEH_REFS_DEFINE(enc_btn_beh_refs_##n, n);                                                   \
    static struct enc_btn_data enc_btn_data_##n;                                                    \
    static const struct enc_btn_config enc_btn_config_##n = {                                       \
        .index = n,                                                                                 \
        .code = DT_INST_PROP(n, code),                                                              \
        .beh_refs = enc_btn_beh_refs_##n,                                                           \
    };                                                                                              \
    DEVICE_DT_INST_DEFINE(n, enc_btn_init, NULL, &enc_btn_data_##n, &enc_btn_config_##n,            \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &enc_btn_api);

DT_INST_FOREACH_STATUS_OKAY(ENC_BTN_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
