/*
 * Host-test stub for <zephyr/devicetree.h>.
 *
 * combos/src/combo_state.c resolves each combo target type to a behavior device
 * NAME at compile time via DT. On the host there is no devicetree, so we pretend
 * every standard behavior node exists and hand back a stable synthetic name —
 * enough to prove the target_type -> device mapping and the fail-open rules,
 * without claiming anything about a real build's node set.
 *
 * Define TORABO_TEST_DT_BEHAVIORS=0 to simulate a build where none of them exist
 * (every combo must then come back disabled).
 */
#pragma once

#ifndef TORABO_TEST_DT_BEHAVIORS
#define TORABO_TEST_DT_BEHAVIORS 1
#endif

#define DT_HAS_COMPAT_STATUS_OKAY(compat) TORABO_TEST_DT_BEHAVIORS

#define DT_INST(inst, compat) compat

#define TORABO_TEST_STR_(x) #x
#define TORABO_TEST_STR(x) TORABO_TEST_STR_(x)
#define DEVICE_DT_NAME(node_id) TORABO_TEST_STR(node_id)

/*
 * Minimal stand-ins for the DT phandle-array extraction used by
 * torabo_common/binding.h's TORABO_BEH_REFS_DEFINE (refactor phase 4). A real
 * build resolves DT_PHANDLE_BY_IDX/DT_INST_PROP_LEN/DT_DRV_INST from
 * devicetree_generated.h; on the host there is no devicetree, so this fakes a
 * single synthetic "processor" instance whose `behaviors` property always has
 * exactly TORABO_REF_COUNT (5) entries — the one shape binding.h ever needs
 * (the BUILD_ASSERT it emits IS the check that this stays true). Each phandle
 * index gets a distinct, stable synthetic device name, so test/wire/test_binding.c
 * can tell the reference slots apart without a real overlay.
 */
#define DT_DRV_INST(inst) torabo_test_drv_inst
#define DT_INST_PROP_LEN(inst, prop) 5
#define DT_PHANDLE_BY_IDX(node_id, prop, idx) node_id##_##prop##_##idx

/*
 * Fixed-arity stand-in for Zephyr's LISTIFY(n, f, sep, ...), good for n==5
 * only (the one value TORABO_BEH_REFS_DEFINE ever passes — see
 * DT_INST_PROP_LEN above). `sep` arrives pre-parenthesized (e.g. `(, )`), so
 * "gluing" the macro name directly in front of the parameter — no parens of
 * our own in between — lets its own parens supply the call, exactly like
 * upstream Zephyr's __DEBRACKET trick.
 */
#define TORABO_TEST_DEBRACKET(...) __VA_ARGS__
/* Indirected concatenation so `n` (e.g. DT_INST_PROP_LEN(...)) is fully
 * macro-expanded to a bare number BEFORE the ## paste, not after: a paste
 * operand is never expanded at the same macro level it's pasted at. */
#define TORABO_TEST_CAT(a, b) TORABO_TEST_CAT_(a, b)
#define TORABO_TEST_CAT_(a, b) a##b
#define LISTIFY(n, f, sep, ...) TORABO_TEST_CAT(TORABO_TEST_LISTIFY_, n)(f, sep, __VA_ARGS__)
#define TORABO_TEST_LISTIFY_5(f, sep, ...)                                                         \
    f(0, __VA_ARGS__)                                                                              \
    TORABO_TEST_DEBRACKET sep f(1, __VA_ARGS__)                                                    \
    TORABO_TEST_DEBRACKET sep f(2, __VA_ARGS__)                                                    \
    TORABO_TEST_DEBRACKET sep f(3, __VA_ARGS__)                                                    \
    TORABO_TEST_DEBRACKET sep f(4, __VA_ARGS__)
