/*
 * PLAN-ext-fw-refactor.md phase 9 fixture: a SECOND "declared" build, a
 * double configuration (ball on BOTH standard connectors, encoder on BOTH
 * extension connectors) -- not a real fw-test/builder pattern like
 * torabo_test_config_decl.h's, but a deliberate stress fixture: with the same
 * slot value repeated on both sides of each nibble pair, a shift-by-one or a
 * mask that leaks into the neighboring 4 bits shows up as a wrong hex digit
 * rather than an accidental match (which a single-instance fixture could
 * hide).
 *
 *   left std  = TORABO_CAPS_SLOT_BALL    (1) -> bits0-3
 *   left ext  = TORABO_CAPS_SLOT_ENCODER (9) -> bits4-7
 *   right std = TORABO_CAPS_SLOT_BALL    (1) -> bits8-11
 *   right ext = TORABO_CAPS_SLOT_ENCODER (9) -> bits12-15
 *   0x1 | (0x9<<4) | (0x1<<8) | (0x9<<12) = 0x9191
 *
 * central = left (1), the opposite of torabo_test_config_decl.h's right, so
 * the two fixtures' _rsv bytes also differ.
 *
 * Separate binary (main_decl2.c + test_caps_decl2.c) for the same reason as
 * the primary decl fixture: it must never be compiled against test_caps.c's
 * all-zero golden vector, nor against the primary decl fixture's vector.
 */
#pragma once

#undef CONFIG_TORABO_CENTRAL_SIDE
#undef CONFIG_TORABO_SLOT_LEFT_STD
#undef CONFIG_TORABO_SLOT_LEFT_EXT
#undef CONFIG_TORABO_SLOT_RIGHT_STD
#undef CONFIG_TORABO_SLOT_RIGHT_EXT

#define CONFIG_TORABO_CENTRAL_SIDE 1     /* left */
#define CONFIG_TORABO_SLOT_LEFT_STD 1    /* ball */
#define CONFIG_TORABO_SLOT_LEFT_EXT 9    /* encoder */
#define CONFIG_TORABO_SLOT_RIGHT_STD 1   /* ball */
#define CONFIG_TORABO_SLOT_RIGHT_EXT 9   /* encoder */
