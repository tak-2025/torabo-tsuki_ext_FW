/*
 * binding synthesis (torabo_common/binding.h) — host golden tests
 * (refactor phase 4, PLAN-ext-fw-refactor.md §Phase 4).
 *
 * What is checked:
 *
 *  1. Frozen numbering: the reference-slot enum (torabo_beh_ref / tp_beh_ref /
 *     enc_beh_ref) and the behavior-kind numbers each feature's `behavior`
 *     byte carries (tp_behavior / enc_behavior) are pinned as literal
 *     integers. This IS the public contract: the DT `behaviors` property
 *     order (`<&none>, <&kp 0>, <&mo 0>, <&to 0>, <&tog 0>`) and the wire's
 *     `behavior` byte both depend on these numbers never moving.
 *
 *  2. Golden PARITY against `ref_tp_make_binding` / `ref_enc_make_binding`,
 *     byte-for-byte copies of the PRE-extraction functions as they stood on
 *     refactor/phase3-coast-engine (before trackpad/encoder shared this code
 *     via torabo_common/binding.h rather than by copy-paste):
 *       git show refactor/phase3-coast-engine:features/trackpad/include/zmk_trackpad_config/binding.h
 *       git show refactor/phase3-coast-engine:features/encoder/include/zmk_encoder_config/binding.h
 *     Every behavior kind (NONE/KP/CP/MO/TO/TOG, plus two out-of-range
 *     values to exercise the fail-open default) is driven across
 *     representative params: a normal keyboard usage, a consumer usage, mods
 *     combinations (none / all four MOD_L* bits / a full 0xFF byte), and
 *     layer-number boundaries (0, MAX-1, and the 0xFFFF param ceiling). The
 *     resulting `zmk_behavior_binding` (behavior_dev name + param1 + param2)
 *     must match exactly for every case, for both features.
 *
 *  3. Golden PARITY on the DT-extraction macros: `TP_BEH_REF_EXTRACT` /
 *     `ENC_BEH_REF_EXTRACT` (now one line forwarding to
 *     `TORABO_BEH_REF_EXTRACT`) against `ref_tp_beh_ref_extract` /
 *     `ref_enc_beh_ref_extract`, the same pre-extraction macro bodies copied
 *     under REF_-prefixed names (macro names collide otherwise). Also runs
 *     `TP_BEH_REFS_DEFINE`/`ENC_BEH_REFS_DEFINE` (today: one line forwarding
 *     to `TORABO_BEH_REFS_DEFINE`) against `REF_TP_BEH_REFS_DEFINE`/
 *     `REF_ENC_BEH_REFS_DEFINE` (old, standalone bodies) through a synthetic
 *     DT stub (test/wire/stubs/zephyr/devicetree.h) that fakes a single
 *     "processor" instance with a 5-entry `behaviors` phandle-array — proving
 *     the `BUILD_ASSERT(ARRAY_SIZE(name) == *_REF_COUNT)` guard still fires
 *     for the right count, and that the extracted device-name order is
 *     unchanged (none, kp, mo, to, tog).
 *
 * `struct tp_binding` / `struct enc_binding` are untouched by phase 4 (the
 * wire decoders still touch them directly), so this file uses the real,
 * current definitions from each feature's config.h for both the "new" and
 * "old" code paths — only the binding.h synthesis machinery moved.
 */

#include <stdio.h>
#include <string.h>

/* A real (Zephyr) build has these DT macros available globally by the time
 * any *_BEH_REFS_DEFINE() invocation is reached, without binding.h including
 * devicetree.h itself. The host stub has no such implicit global include, so
 * pull it in explicitly here — before the file-scope REFS_DEFINE invocations
 * below, which is all that matters (macro bodies are only resolved at their
 * invocation site, not at their #define site). */
#include <zephyr/devicetree.h>

#include <zmk_trackpad_config/binding.h>
#include <zmk_trackpad_config/config.h>

#include <zmk_encoder_config/binding.h>
#include <zmk_encoder_config/config.h>

#include "torabo_test.h"

/* ---- 1. frozen numbering --------------------------------------------------
 * These are the public contract this whole phase must not disturb: the DT
 * phandle-array index (torabo_beh_ref and its two per-feature aliases) and
 * the wire `behavior` byte (tp_behavior / enc_behavior). Pinned as literal
 * integers, not just "equal to each other", so a future edit that moves BOTH
 * sides together still fails this test. */
static void test_binding_enum_literals(void) {
    /* torabo_common/binding.h: enum torabo_beh_ref (frozen DT index). */
    T_EQ_INT(TORABO_REF_NONE, 0, "TORABO_REF_NONE == 0");
    T_EQ_INT(TORABO_REF_KP, 1, "TORABO_REF_KP == 1");
    T_EQ_INT(TORABO_REF_MO, 2, "TORABO_REF_MO == 2");
    T_EQ_INT(TORABO_REF_TO, 3, "TORABO_REF_TO == 3");
    T_EQ_INT(TORABO_REF_TOG, 4, "TORABO_REF_TOG == 4");
    T_EQ_INT(TORABO_REF_COUNT, 5, "TORABO_REF_COUNT == 5");

    /* trackpad's enum tp_beh_ref: must stay numerically identical. */
    T_EQ_INT(TP_REF_NONE, 0, "TP_REF_NONE == 0");
    T_EQ_INT(TP_REF_KP, 1, "TP_REF_KP == 1");
    T_EQ_INT(TP_REF_MO, 2, "TP_REF_MO == 2");
    T_EQ_INT(TP_REF_TO, 3, "TP_REF_TO == 3");
    T_EQ_INT(TP_REF_TOG, 4, "TP_REF_TOG == 4");
    T_EQ_INT(TP_REF_COUNT, 5, "TP_REF_COUNT == 5");

    /* encoder's enum enc_beh_ref: must stay numerically identical. */
    T_EQ_INT(ENC_REF_NONE, 0, "ENC_REF_NONE == 0");
    T_EQ_INT(ENC_REF_KP, 1, "ENC_REF_KP == 1");
    T_EQ_INT(ENC_REF_MO, 2, "ENC_REF_MO == 2");
    T_EQ_INT(ENC_REF_TO, 3, "ENC_REF_TO == 3");
    T_EQ_INT(ENC_REF_TOG, 4, "ENC_REF_TOG == 4");
    T_EQ_INT(ENC_REF_COUNT, 5, "ENC_REF_COUNT == 5");

    /* behavior-kind numbers (the wire `behavior` byte) — untouched by phase 4,
     * pinned anyway since torabo_make_binding's switch depends on them. */
    T_EQ_INT(TP_BEH_NONE, 0, "TP_BEH_NONE == 0");
    T_EQ_INT(TP_BEH_KP, 1, "TP_BEH_KP == 1");
    T_EQ_INT(TP_BEH_CP, 2, "TP_BEH_CP == 2");
    T_EQ_INT(TP_BEH_MO, 3, "TP_BEH_MO == 3");
    T_EQ_INT(TP_BEH_TO, 4, "TP_BEH_TO == 4");
    T_EQ_INT(TP_BEH_TOG, 5, "TP_BEH_TOG == 5");

    T_EQ_INT(ENC_BEH_NONE, 0, "ENC_BEH_NONE == 0");
    T_EQ_INT(ENC_BEH_KP, 1, "ENC_BEH_KP == 1");
    T_EQ_INT(ENC_BEH_CP, 2, "ENC_BEH_CP == 2");
    T_EQ_INT(ENC_BEH_MO, 3, "ENC_BEH_MO == 3");
    T_EQ_INT(ENC_BEH_TO, 4, "ENC_BEH_TO == 4");
    T_EQ_INT(ENC_BEH_TOG, 5, "ENC_BEH_TOG == 5");
}

/* ---- 2. reference implementation: verbatim pre-phase-4 arithmetic --------
 * Copied from features/trackpad/include/zmk_trackpad_config/binding.h and
 * features/encoder/include/zmk_encoder_config/binding.h as they stood on
 * refactor/phase3-coast-engine. Renamed to ref_ / REF_ prefixes only — not one cast,
 * shift or case label is changed. Uses the REAL (current, untouched)
 * `struct tp_binding` / `struct enc_binding` from each feature's config.h.
 */

/* -- trackpad, verbatim -- */
static inline struct zmk_behavior_binding ref_tp_make_binding(const char *const *refs,
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

#define REF_TP_BEH_REF_EXTRACT(idx, drv_inst)                                                     \
    DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(drv_inst, behaviors, idx))

#define REF_TP_BEH_REFS_DEFINE(name, n)                                                           \
    static const char *const name[] = {                                                          \
        LISTIFY(DT_INST_PROP_LEN(n, behaviors), REF_TP_BEH_REF_EXTRACT, (, ), DT_DRV_INST(n))};  \
    BUILD_ASSERT(ARRAY_SIZE(name) == TP_REF_COUNT,                                                \
                 "behaviors must be <&none>, <&kp 0>, <&mo 0>, <&to 0>, <&tog 0>")

/* -- encoder, verbatim -- */
static inline struct zmk_behavior_binding ref_enc_make_binding(const char *const *refs,
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

#define REF_ENC_BEH_REF_EXTRACT(idx, drv_inst)                                                    \
    DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(drv_inst, behaviors, idx))

#define REF_ENC_BEH_REFS_DEFINE(name, n)                                                          \
    static const char *const name[] = {                                                          \
        LISTIFY(DT_INST_PROP_LEN(n, behaviors), REF_ENC_BEH_REF_EXTRACT, (, ), DT_DRV_INST(n))}; \
    BUILD_ASSERT(ARRAY_SIZE(name) == ENC_REF_COUNT,                                               \
                 "behaviors must be <&none>, <&kp 0>, <&mo 0>, <&to 0>, <&tog 0>")

/* ---- 3. DT extraction parity: NEW (forwarding) vs OLD (standalone) -------
 * Invoked at file scope, exactly how tp_pointer.c / enc_button.c use it, via
 * the synthetic single-instance DT stub in stubs/zephyr/devicetree.h. */
TP_BEH_REFS_DEFINE(g_new_tp_refs, 0);
REF_TP_BEH_REFS_DEFINE(g_old_tp_refs, 0);
ENC_BEH_REFS_DEFINE(g_new_enc_refs, 0);
REF_ENC_BEH_REFS_DEFINE(g_old_enc_refs, 0);

static void test_binding_dt_extract(void) {
    /* Bare macro parity (TP_BEH_REF_EXTRACT / ENC_BEH_REF_EXTRACT now forward
     * to TORABO_BEH_REF_EXTRACT; must expand identically to the old,
     * standalone body for every reference slot). */
    for (int idx = 0; idx < TP_REF_COUNT; idx++) {
        const char *new_name = NULL;
        const char *old_name = NULL;
        switch (idx) {
        case 0:
            new_name = TP_BEH_REF_EXTRACT(0, torabo_test_drv_inst);
            old_name = REF_TP_BEH_REF_EXTRACT(0, torabo_test_drv_inst);
            break;
        case 1:
            new_name = TP_BEH_REF_EXTRACT(1, torabo_test_drv_inst);
            old_name = REF_TP_BEH_REF_EXTRACT(1, torabo_test_drv_inst);
            break;
        case 2:
            new_name = TP_BEH_REF_EXTRACT(2, torabo_test_drv_inst);
            old_name = REF_TP_BEH_REF_EXTRACT(2, torabo_test_drv_inst);
            break;
        case 3:
            new_name = TP_BEH_REF_EXTRACT(3, torabo_test_drv_inst);
            old_name = REF_TP_BEH_REF_EXTRACT(3, torabo_test_drv_inst);
            break;
        default:
            new_name = TP_BEH_REF_EXTRACT(4, torabo_test_drv_inst);
            old_name = REF_TP_BEH_REF_EXTRACT(4, torabo_test_drv_inst);
            break;
        }
        T_CHECK(strcmp(new_name, old_name) == 0, "TP_BEH_REF_EXTRACT matches the pre-phase-4 macro");
    }
    T_CHECK(strcmp(ENC_BEH_REF_EXTRACT(0, torabo_test_drv_inst),
                   REF_ENC_BEH_REF_EXTRACT(0, torabo_test_drv_inst)) == 0,
           "ENC_BEH_REF_EXTRACT matches the pre-phase-4 macro (idx 0)");
    T_CHECK(strcmp(ENC_BEH_REF_EXTRACT(4, torabo_test_drv_inst),
                   REF_ENC_BEH_REF_EXTRACT(4, torabo_test_drv_inst)) == 0,
           "ENC_BEH_REF_EXTRACT matches the pre-phase-4 macro (idx 4)");

    /* REFS_DEFINE parity: same length, same order, same device names. */
    T_EQ_INT(ARRAY_SIZE(g_new_tp_refs), ARRAY_SIZE(g_old_tp_refs),
             "TP_BEH_REFS_DEFINE: new/old array length matches");
    for (size_t i = 0; i < ARRAY_SIZE(g_new_tp_refs); i++) {
        T_CHECK(strcmp(g_new_tp_refs[i], g_old_tp_refs[i]) == 0,
               "TP_BEH_REFS_DEFINE: new/old device name matches at this slot");
    }
    T_EQ_INT(ARRAY_SIZE(g_new_enc_refs), ARRAY_SIZE(g_old_enc_refs),
             "ENC_BEH_REFS_DEFINE: new/old array length matches");
    for (size_t i = 0; i < ARRAY_SIZE(g_new_enc_refs); i++) {
        T_CHECK(strcmp(g_new_enc_refs[i], g_old_enc_refs[i]) == 0,
               "ENC_BEH_REFS_DEFINE: new/old device name matches at this slot");
    }

    /* And the slot order itself is none/kp/mo/to/tog, per the DT contract. */
    T_CHECK(strcmp(g_new_tp_refs[TP_REF_NONE], "torabo_test_drv_inst_behaviors_0") == 0,
           "trackpad refs[NONE] is DT phandle-array index 0");
    T_CHECK(strcmp(g_new_tp_refs[TP_REF_KP], "torabo_test_drv_inst_behaviors_1") == 0,
           "trackpad refs[KP] is DT phandle-array index 1");
    T_CHECK(strcmp(g_new_tp_refs[TP_REF_MO], "torabo_test_drv_inst_behaviors_2") == 0,
           "trackpad refs[MO] is DT phandle-array index 2");
    T_CHECK(strcmp(g_new_tp_refs[TP_REF_TO], "torabo_test_drv_inst_behaviors_3") == 0,
           "trackpad refs[TO] is DT phandle-array index 3");
    T_CHECK(strcmp(g_new_tp_refs[TP_REF_TOG], "torabo_test_drv_inst_behaviors_4") == 0,
           "trackpad refs[TOG] is DT phandle-array index 4");
}

/* ---- 4. make_binding parity: every behavior kind x representative params -- */

struct binding_case {
    const char *label;
    uint8_t behavior;
    uint8_t mods;
    uint16_t param;
};

/* Representative params, shared by both features (same struct layout, same
 * behavior-kind numbering): a normal keyboard usage, a consumer usage, mods
 * combinations, and the layer-number boundaries the MO/TO/TOG kinds carry.
 * Two out-of-range `behavior` values are included to pin the fail-open
 * default (NONE, param 0) for both the new and the old implementation. */
static const struct binding_case k_cases[] = {
    {"NONE, mods=0, param=0", TP_BEH_NONE, 0x00, 0x0000},
    {"NONE, mods=0x0F, param=0x1234 (still NONE: ignored)", TP_BEH_NONE, 0x0F, 0x1234},

    {"KP, mods=0, normal key 'a' (0x04)", TP_BEH_KP, 0x00, 0x0004},
    {"KP, mods=LCTL (0x01), key '1' (0x1E)", TP_BEH_KP, 0x01, 0x001E},
    {"KP, mods=all 4 MOD_L bits (0x0F), key 'z' (0x1D)", TP_BEH_KP, 0x0F, 0x001D},
    {"KP, mods=0xFF (full byte boundary), param=0xFFFF (param ceiling)", TP_BEH_KP, 0xFF, 0xFFFF},

    {"CP, mods=0, consumer Volume Up (0x00E9)", TP_BEH_CP, 0x00, 0x00E9},
    {"CP, mods=LSFT (0x02), consumer Volume Down (0x00EA)", TP_BEH_CP, 0x02, 0x00EA},
    {"CP, mods=0xFF, param=0xFFFF (both boundaries)", TP_BEH_CP, 0xFF, 0xFFFF},

    {"MO, layer 0 (boundary: lowest)", TP_BEH_MO, 0x00, 0},
    {"MO, layer ENC_MAX_LAYERS-1 (boundary: highest configured)", TP_BEH_MO, 0x00,
     (uint16_t)(ENC_MAX_LAYERS - 1)},
    {"MO, layer 0xFFFF (param ceiling, mods ignored for MO)", TP_BEH_MO, 0xAA, 0xFFFF},

    {"TO, layer 0 (boundary: lowest)", TP_BEH_TO, 0x00, 0},
    {"TO, layer TP_MAX_LAYERS-1 (boundary: highest configured)", TP_BEH_TO, 0x00,
     (uint16_t)(TP_MAX_LAYERS - 1)},

    {"TOG, layer 0 (boundary: lowest)", TP_BEH_TOG, 0x00, 0},
    {"TOG, layer 0xFFFF (param ceiling)", TP_BEH_TOG, 0x00, 0xFFFF},

    {"out-of-range behavior=6 (one past TOG): fail-open to NONE", 6, 0x0F, 0x1234},
    {"out-of-range behavior=255: fail-open to NONE", 255, 0xFF, 0xFFFF},
};

static void expect_binding_eq(const struct zmk_behavior_binding *got,
                              const struct zmk_behavior_binding *want, const char *label) {
    char what[320];
    snprintf(what, sizeof(what), "%s: behavior_dev matches old implementation", label);
    T_CHECK(strcmp(got->behavior_dev, want->behavior_dev) == 0, what);
    snprintf(what, sizeof(what), "%s: param1 matches old implementation", label);
    T_EQ_INT(got->param1, want->param1, what);
    snprintf(what, sizeof(what), "%s: param2 matches old implementation", label);
    T_EQ_INT(got->param2, want->param2, what);
}

static void test_binding_make_binding_parity(void) {
    for (size_t i = 0; i < ARRAY_SIZE(k_cases); i++) {
        const struct binding_case *c = &k_cases[i];

        /* trackpad */
        struct tp_binding tpd = {.behavior = c->behavior, .mods = c->mods, .param = c->param};
        struct zmk_behavior_binding tp_new = tp_make_binding(g_new_tp_refs, &tpd);
        struct zmk_behavior_binding tp_old = ref_tp_make_binding(g_old_tp_refs, &tpd);
        char label[256];
        snprintf(label, sizeof(label), "trackpad make_binding[%s]", c->label);
        expect_binding_eq(&tp_new, &tp_old, label);

        /* encoder — same case table: the two behavior-kind enums share the
         * exact numbering (frozen; pinned in test_binding_enum_literals). */
        struct enc_binding encd = {.behavior = c->behavior, .mods = c->mods, .param = c->param};
        struct zmk_behavior_binding enc_new = enc_make_binding(g_new_enc_refs, &encd);
        struct zmk_behavior_binding enc_old = ref_enc_make_binding(g_old_enc_refs, &encd);
        snprintf(label, sizeof(label), "encoder make_binding[%s]", c->label);
        expect_binding_eq(&enc_new, &enc_old, label);
    }
}

void test_binding(void) {
    torabo_test_begin("binding synthesis (torabo_common/binding.h, refactor phase 4)");
    test_binding_enum_literals();
    test_binding_dt_extract();
    test_binding_make_binding_parity();
}
