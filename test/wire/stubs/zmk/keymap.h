/*
 * Host-test stub for <zmk/keymap.h>.
 *
 * ZMK_KEYMAP_LAYERS_LEN is a devicetree-derived constant upstream, and it is the
 * ONE thing that changes the trackball / trackpad / encoder wire LENGTH (see
 * PLAN-ext-fw-refactor.md §0.4). The runner therefore compiles the suite at
 * several layer counts; 10 is the value the field firmware uses and is the one
 * the pinned byte vectors are stated for.
 */
#pragma once

#include <stdint.h>

#ifndef ZMK_KEYMAP_LAYERS_LEN
#define ZMK_KEYMAP_LAYERS_LEN 10
#endif
