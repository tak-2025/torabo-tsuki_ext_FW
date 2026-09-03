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
