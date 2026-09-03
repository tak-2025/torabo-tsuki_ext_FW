/*
 * Encoder liveness counters for the Torabo-Float diagnostic mode
 * (PLAN-torabo-float.md §13).
 *
 * The rotation router (enc_behavior.c) and the button processor (enc_button.c)
 * bump these whenever the encoder produces an event — BEFORE the per-layer
 * binding is resolved, so an UNASSIGNED knob/click still counts. That is the whole
 * point of the diagnostic: "I pressed it and btn went up" proves the hardware and
 * the split path are alive even when the current layer maps it to nothing (or to a
 * silent action like mute — the exact trap that made the button look dead).
 *
 * live_feed reads these through enc_diag_get(). It declares that symbol __weak and
 * defaulting to "absent", so a central build WITHOUT the encoder module still
 * links; this module provides the strong definition when it is compiled in.
 */

#pragma once

#include <zephyr/types.h>
#include <stdbool.h>

/* Count one rotation detent. cw=true clockwise, false counter-clockwise. */
void enc_diag_note_rotate(bool cw);

/* Count one button press (the down edge; releases are not counted). */
void enc_diag_note_button(void);

/*
 * Read the current counters. Returns true if the encoder module is present (this
 * strong definition), false via the __weak fallback in live_feed when it isn't.
 * Any out pointer may be NULL.
 */
bool enc_diag_get(uint16_t *cw, uint16_t *ccw, uint16_t *btn);
