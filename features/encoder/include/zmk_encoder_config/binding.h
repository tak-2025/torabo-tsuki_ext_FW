/*
 * Runtime binding synthesis for the encoder (same trick as the trackpad's
 * binding.h, kept separate so the two features stay independent).
 *
 * A DT node lists the standard ZMK behaviors once; we keep their device NAMES and
 * build a zmk_behavior_binding with an arbitrary param at runtime. That is what
 * lets the app assign any keycode without a compile-time palette.
 */

#pragma once

#include <zephyr/types.h>
#include <zmk/behavior.h>
#include <dt-bindings/zmk/hid_usage_pages.h> /* HID_USAGE_KEY / HID_USAGE_CONSUMER */
#include <zmk_encoder_config/config.h>

/* Reference slots, in the order the node's `behaviors` property must list them. */
enum enc_beh_ref {
    ENC_REF_NONE = 0,
    ENC_REF_KP = 1,
    ENC_REF_MO = 2,
    ENC_REF_TO = 3,
    ENC_REF_TOG = 4,
    ENC_REF_COUNT = 5,
};

/* Descriptor -> real binding. Unknown/NONE => &none with param 0, so a config we
 * don't understand does nothing rather than firing something wrong. */
static inline struct zmk_behavior_binding enc_make_binding(const char *const *refs,
                                                           const struct enc_binding *d) {
    struct zmk_behavior_binding b = {
        .behavior_dev = refs[ENC_REF_NONE],
        .param1 = 0,
        .param2 = 0,
    };
    switch (d->behavior) {
    case ENC_BEH_KP:
        b.behavior_dev = refs[ENC_REF_KP];
        b.param1 = ((uint32_t)d->mods << 24) | ((uint32_t)HID_USAGE_KEY << 16) | d->param;
        break;
    case ENC_BEH_CP:
        /* a consumer usage is &kp with the consumer usage page */
        b.behavior_dev = refs[ENC_REF_KP];
        b.param1 = ((uint32_t)d->mods << 24) | ((uint32_t)HID_USAGE_CONSUMER << 16) | d->param;
        break;
    case ENC_BEH_MO:
        b.behavior_dev = refs[ENC_REF_MO];
        b.param1 = d->param;
        break;
    case ENC_BEH_TO:
        b.behavior_dev = refs[ENC_REF_TO];
        b.param1 = d->param;
        break;
    case ENC_BEH_TOG:
        b.behavior_dev = refs[ENC_REF_TOG];
        b.param1 = d->param;
        break;
    case ENC_BEH_NONE:
    default:
        break;
    }
    return b;
}

/* Pull one behavior device name out of the node's `behaviors` phandle-array. */
#define ENC_BEH_REF_EXTRACT(idx, drv_inst) DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(drv_inst, behaviors, idx))

/* Define `name[ENC_REF_COUNT]` of const char* from DT instance n's `behaviors`.
 * The YAML must set `specifier-space: binding`, otherwise dtc looks for
 * #behavior-cells and every reference fails. */
#define ENC_BEH_REFS_DEFINE(name, n)                                                               \
    static const char *const name[] = {                                                            \
        LISTIFY(DT_INST_PROP_LEN(n, behaviors), ENC_BEH_REF_EXTRACT, (, ), DT_DRV_INST(n))};       \
    BUILD_ASSERT(ARRAY_SIZE(name) == ENC_REF_COUNT,                                                \
                 "behaviors must be <&none>, <&kp 0>, <&mo 0>, <&to 0>, <&tog 0>")
