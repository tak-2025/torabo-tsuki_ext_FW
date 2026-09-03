/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Runtime binding synthesis (DESIGN-trackpad-v2.md §4.2/§4.3). ZMK behaviors are
 * static devicetree nodes, but their param is free: reference the standard
 * behaviors once in the processor node's `behaviors` property, keep their device
 * *names*, and build a `struct zmk_behavior_binding` with an arbitrary param at
 * runtime — exactly how Studio's keymap editor fires behaviors.
 *
 * The `behaviors` property MUST list the references in this fixed order:
 *     behaviors = <&none>, <&kp 0>, <&mo 0>, <&to 0>, <&tog 0>;
 * (&kp doubles for both TP_BEH_KP and TP_BEH_CP — mainline ZMK has no &cp; a
 *  consumer usage is just &kp with the consumer usage page in the encoded param.)
 *
 * &kp param encoding matches ZMK exactly (keycode_state_changed_from_encoded):
 *     param1 = (mods << 24) | (usage_page << 16) | usage_id
 * with page = HID_USAGE_KEY(0x07) for TP_BEH_KP, HID_USAGE_CONSUMER(0x0C) for
 * TP_BEH_CP. mods use MOD_L* order (bits: LCTL/LSFT/LALT/LGUI ...).
 */

#pragma once

#include <zephyr/types.h>
#include <zmk/behavior.h>
#include <dt-bindings/zmk/hid_usage_pages.h> /* ZMK_HID_USAGE, HID_USAGE_KEY/CONSUMER */
#include <zmk_trackpad_config/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reference slots, in the order the processor's `behaviors` property must list
 * them. Populate from DT with TP_BEH_REF_EXTRACT below. */
enum tp_beh_ref {
    TP_REF_NONE = 0,
    TP_REF_KP = 1,
    TP_REF_MO = 2,
    TP_REF_TO = 3,
    TP_REF_TOG = 4,
    TP_REF_COUNT = 5,
};

/* Build the runtime binding for a descriptor. `refs` are the behavior device
 * names extracted from the node (TP_REF_* order). Unknown/NONE => &none, param 0
 * (fail-open). Returned by value; the field types match zmk_behavior_binding. */
static inline struct zmk_behavior_binding tp_make_binding(const char *const *refs,
                                                          const struct tp_binding *d) {
    struct zmk_behavior_binding b = {
        .behavior_dev = refs[TP_REF_NONE],
        .param1 = 0,
        .param2 = 0,
    };
    switch (d->behavior) {
    case TP_BEH_KP:
        b.behavior_dev = refs[TP_REF_KP];
        b.param1 = ((uint32_t)d->mods << 24) | ((uint32_t)HID_USAGE_KEY << 16) | d->param;
        break;
    case TP_BEH_CP:
        b.behavior_dev = refs[TP_REF_KP]; /* consumer = &kp with consumer page */
        b.param1 = ((uint32_t)d->mods << 24) | ((uint32_t)HID_USAGE_CONSUMER << 16) | d->param;
        break;
    case TP_BEH_MO:
        b.behavior_dev = refs[TP_REF_MO];
        b.param1 = d->param;
        break;
    case TP_BEH_TO:
        b.behavior_dev = refs[TP_REF_TO];
        b.param1 = d->param;
        break;
    case TP_BEH_TOG:
        b.behavior_dev = refs[TP_REF_TOG];
        b.param1 = d->param;
        break;
    case TP_BEH_NONE:
    default:
        break; /* refs[TP_REF_NONE], param 0 */
    }
    return b;
}

/* Extract one behavior-device name from the node's `behaviors` phandle-array.
 * Mirrors ZMK_KEYMAP_EXTRACT_BINDING but keeps only the device name. */
#define TP_BEH_REF_EXTRACT(idx, drv_inst) DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(drv_inst, behaviors, idx))

/* Define `name[TP_REF_COUNT]` of const char* from DT instance n's `behaviors`. */
#define TP_BEH_REFS_DEFINE(name, n)                                                                \
    static const char *const name[] = {                                                            \
        LISTIFY(DT_INST_PROP_LEN(n, behaviors), TP_BEH_REF_EXTRACT, (, ), DT_DRV_INST(n))};        \
    BUILD_ASSERT(ARRAY_SIZE(name) == TP_REF_COUNT,                                                  \
                 "behaviors must be <&none>, <&kp 0>, <&mo 0>, <&to 0>, <&tog 0>")

#ifdef __cplusplus
}
#endif
