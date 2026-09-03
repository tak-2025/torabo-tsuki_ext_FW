/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * torabo_common/binding.h — shared runtime binding synthesis
 * (refactor phase 4, PLAN-ext-fw-refactor.md §Phase 4).
 *
 * Extracted from the two previously byte-identical copies in
 * features/trackpad/include/zmk_trackpad_config/binding.h and
 * features/encoder/include/zmk_encoder_config/binding.h. ZMK behaviors are
 * static devicetree nodes, but their param is free: reference the standard
 * behaviors once in the processor node's `behaviors` property, keep their
 * device *names*, and build a `struct zmk_behavior_binding` with an arbitrary
 * param at runtime — exactly how Studio's keymap editor fires behaviors.
 *
 * The `behaviors` property MUST list the references in this fixed order
 * (public contract — the DT overlay's property order IS the reference-slot
 * order; do not renumber):
 *     behaviors = <&none>, <&kp 0>, <&mo 0>, <&to 0>, <&tog 0>;
 * (&kp doubles for both the KP and CP behavior kinds — mainline ZMK has no
 *  &cp; a consumer usage is just &kp with the consumer usage page in the
 *  encoded param.)
 *
 * &kp param encoding matches ZMK exactly (keycode_state_changed_from_encoded):
 *     param1 = (mods << 24) | (usage_page << 16) | usage_id
 * with page = HID_USAGE_KEY(0x07) for KP, HID_USAGE_CONSUMER(0x0C) for CP.
 * mods use MOD_L* order (bits: LCTL/LSFT/LALT/LGUI ...).
 *
 * WHAT STAYS PER-FEATURE: each feature keeps its own `struct tp_binding` /
 * `struct enc_binding` (identical layout: behavior u8, mods u8, param u16)
 * and its own `enum tp_behavior` / `enum enc_behavior` behavior-kind values,
 * because the wire decoders (tp_apply_wire / enc_apply_wire) touch those
 * structs directly and must not gain a dependency on this header. This
 * header only synthesises the zmk_behavior_binding from already-unpacked
 * fields — callers pass behavior/mods/param, not a struct pointer.
 *
 * The behavior-kind NUMBERS below (NONE=0, KP=1, CP=2, MO=3, TO=4, TOG=5)
 * are the one thing this header assumes about the caller's own behavior
 * enum: `enum tp_behavior` and `enum enc_behavior` both already use this
 * exact numbering (and it must never change — it is the wire's `behavior`
 * byte), so passing d->behavior straight through is safe without either
 * feature's enum being visible here.
 */

#pragma once

#include <zephyr/types.h>
#include <zmk/behavior.h>
#include <dt-bindings/zmk/hid_usage_pages.h> /* ZMK_HID_USAGE, HID_USAGE_KEY/CONSUMER */

#ifdef __cplusplus
extern "C" {
#endif

/* Reference slots, in the order the processor's `behaviors` property must
 * list them. Populate from DT with TORABO_BEH_REF_EXTRACT below. Frozen:
 * this numbering is the DT phandle-array index, a public contract. */
enum torabo_beh_ref {
    TORABO_REF_NONE = 0,
    TORABO_REF_KP = 1,
    TORABO_REF_MO = 2,
    TORABO_REF_TO = 3,
    TORABO_REF_TOG = 4,
    TORABO_REF_COUNT = 5,
};

/* Behavior-kind numbering assumed of the caller's `behavior` field. Frozen:
 * this is also the wire's `behavior` byte (tp_behavior / enc_behavior). Not
 * used as a type anywhere — documented here only so the two enums it mirrors
 * can never drift apart without this comment being the thing that's wrong. */
enum torabo_beh_kind {
    TORABO_BEH_NONE = 0,
    TORABO_BEH_KP = 1,
    TORABO_BEH_CP = 2,
    TORABO_BEH_MO = 3,
    TORABO_BEH_TO = 4,
    TORABO_BEH_TOG = 5,
};

/* Build the runtime binding from already-unpacked descriptor fields. `refs`
 * are the behavior device names extracted from the node (TORABO_REF_* order).
 * Unknown/NONE/out-of-range => &none, param 0 (fail-open). Returned by value;
 * the field types match zmk_behavior_binding. */
static inline struct zmk_behavior_binding torabo_make_binding(const char *const *refs,
                                                              uint8_t behavior, uint8_t mods,
                                                              uint16_t param) {
    struct zmk_behavior_binding b = {
        .behavior_dev = refs[TORABO_REF_NONE],
        .param1 = 0,
        .param2 = 0,
    };
    switch (behavior) {
    case TORABO_BEH_KP:
        b.behavior_dev = refs[TORABO_REF_KP];
        b.param1 = ((uint32_t)mods << 24) | ((uint32_t)HID_USAGE_KEY << 16) | param;
        break;
    case TORABO_BEH_CP:
        b.behavior_dev = refs[TORABO_REF_KP]; /* consumer = &kp with consumer page */
        b.param1 = ((uint32_t)mods << 24) | ((uint32_t)HID_USAGE_CONSUMER << 16) | param;
        break;
    case TORABO_BEH_MO:
        b.behavior_dev = refs[TORABO_REF_MO];
        b.param1 = param;
        break;
    case TORABO_BEH_TO:
        b.behavior_dev = refs[TORABO_REF_TO];
        b.param1 = param;
        break;
    case TORABO_BEH_TOG:
        b.behavior_dev = refs[TORABO_REF_TOG];
        b.param1 = param;
        break;
    case TORABO_BEH_NONE:
    default:
        break; /* refs[TORABO_REF_NONE], param 0 */
    }
    return b;
}

/* Extract one behavior-device name from the node's `behaviors` phandle-array.
 * Mirrors ZMK_KEYMAP_EXTRACT_BINDING but keeps only the device name. */
#define TORABO_BEH_REF_EXTRACT(idx, drv_inst)                                                     \
    DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(drv_inst, behaviors, idx))

/* Define `name[TORABO_REF_COUNT]` of const char* from DT instance n's
 * `behaviors`. Use via a feature-local X_BEH_REFS_DEFINE wrapper so the
 * BUILD_ASSERT message can stay feature-specific if desired. */
#define TORABO_BEH_REFS_DEFINE(name, n)                                                           \
    static const char *const name[] = {                                                          \
        LISTIFY(DT_INST_PROP_LEN(n, behaviors), TORABO_BEH_REF_EXTRACT, (, ), DT_DRV_INST(n))};  \
    BUILD_ASSERT(ARRAY_SIZE(name) == TORABO_REF_COUNT,                                            \
                 "behaviors must be <&none>, <&kp 0>, <&mo 0>, <&to 0>, <&tog 0>")

#ifdef __cplusplus
}
#endif
