/*
 * Runtime binding synthesis for the encoder (same trick as the trackpad's
 * binding.h). Thin encoder-specific wrapper over the shared synthesiser in
 * torabo_common/binding.h (refactor phase 4, PLAN-ext-fw-refactor.md
 * §Phase 4) — see that header for the full mechanism and the &kp-encoding
 * doc-comment. `struct enc_binding` stays here (wire decoders touch it
 * directly, and the two features are meant to stay independent per the
 * original design note); only the DT-extraction/&kp-encoding machinery moved
 * out to avoid a byte-identical copy.
 *
 * A DT node lists the standard ZMK behaviors once; we keep their device NAMES
 * and build a zmk_behavior_binding with an arbitrary param at runtime. That is
 * what lets the app assign any keycode without a compile-time palette.
 */

#pragma once

#include <zephyr/types.h>
#include <zmk/behavior.h>
#include <torabo_common/binding.h>
#include <zmk_encoder_config/config.h>

/* Reference slots, in the order the node's `behaviors` property must list
 * them. Numerically identical to torabo_common/binding.h's `enum
 * torabo_beh_ref` (frozen DT phandle-array index) — kept as an encoder-local
 * alias so existing ENC_REF_* call sites need no change. */
enum enc_beh_ref {
    ENC_REF_NONE = TORABO_REF_NONE,
    ENC_REF_KP = TORABO_REF_KP,
    ENC_REF_MO = TORABO_REF_MO,
    ENC_REF_TO = TORABO_REF_TO,
    ENC_REF_TOG = TORABO_REF_TOG,
    ENC_REF_COUNT = TORABO_REF_COUNT,
};

/* Descriptor -> real binding. Unknown/NONE => &none with param 0, so a config we
 * don't understand does nothing rather than firing something wrong. */
static inline struct zmk_behavior_binding enc_make_binding(const char *const *refs,
                                                           const struct enc_binding *d) {
    return torabo_make_binding(refs, d->behavior, d->mods, d->param);
}

/* Pull one behavior device name out of the node's `behaviors` phandle-array. */
#define ENC_BEH_REF_EXTRACT(idx, drv_inst) TORABO_BEH_REF_EXTRACT(idx, drv_inst)

/* Define `name[ENC_REF_COUNT]` of const char* from DT instance n's `behaviors`.
 * The YAML must set `specifier-space: binding`, otherwise dtc looks for
 * #behavior-cells and every reference fails. */
#define ENC_BEH_REFS_DEFINE(name, n) TORABO_BEH_REFS_DEFINE(name, n)
