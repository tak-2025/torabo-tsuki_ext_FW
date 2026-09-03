/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Runtime binding synthesis (DESIGN-trackpad-v2.md §4.2/§4.3). Thin
 * trackpad-specific wrapper over the shared synthesiser in
 * torabo_common/binding.h (refactor phase 4, PLAN-ext-fw-refactor.md
 * §Phase 4) — see that header for the full mechanism and the encoding
 * doc-comment. `struct tp_binding` stays here (wire decoders touch it
 * directly); only the &kp-encoding/DT-extraction machinery moved out.
 *
 * The `behaviors` property MUST list the references in this fixed order
 * (unchanged, public DT contract):
 *     behaviors = <&none>, <&kp 0>, <&mo 0>, <&to 0>, <&tog 0>;
 */

#pragma once

#include <zephyr/types.h>
#include <zmk/behavior.h>
#include <torabo_common/binding.h>
#include <zmk_trackpad_config/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reference slots, in the order the processor's `behaviors` property must list
 * them. Numerically identical to torabo_common/binding.h's `enum
 * torabo_beh_ref` (frozen DT phandle-array index) — kept as a trackpad-local
 * alias so existing TP_REF_* call sites need no change. Populate from DT with
 * TP_BEH_REF_EXTRACT below. */
enum tp_beh_ref {
    TP_REF_NONE = TORABO_REF_NONE,
    TP_REF_KP = TORABO_REF_KP,
    TP_REF_MO = TORABO_REF_MO,
    TP_REF_TO = TORABO_REF_TO,
    TP_REF_TOG = TORABO_REF_TOG,
    TP_REF_COUNT = TORABO_REF_COUNT,
};

/* Build the runtime binding for a descriptor. `refs` are the behavior device
 * names extracted from the node (TP_REF_* order). Unknown/NONE => &none, param
 * 0 (fail-open). Returned by value; the field types match zmk_behavior_binding. */
static inline struct zmk_behavior_binding tp_make_binding(const char *const *refs,
                                                          const struct tp_binding *d) {
    return torabo_make_binding(refs, d->behavior, d->mods, d->param);
}

/* Extract one behavior-device name from the node's `behaviors` phandle-array. */
#define TP_BEH_REF_EXTRACT(idx, drv_inst) TORABO_BEH_REF_EXTRACT(idx, drv_inst)

/* Define `name[TP_REF_COUNT]` of const char* from DT instance n's `behaviors`. */
#define TP_BEH_REFS_DEFINE(name, n) TORABO_BEH_REFS_DEFINE(name, n)

#ifdef __cplusplus
}
#endif
