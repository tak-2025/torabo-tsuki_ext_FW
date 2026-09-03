/*
 * Minimal assertion framework for the torabo wire golden tests.
 *
 * No dependencies beyond libc so the suite builds with a bare `cc *.c`.
 * Every check reports pass/fail individually; the process exit code is the
 * number of failures (clamped), so CI just checks $? == 0.
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int torabo_test_passed;
extern int torabo_test_failed;
extern const char *torabo_test_group;

void torabo_test_begin(const char *group);
void torabo_test_report_pass(const char *what);
void torabo_test_report_fail(const char *what, const char *detail);
void torabo_test_hexdiff(const uint8_t *got, const uint8_t *want, size_t len);

#define T_CHECK(cond, what)                                                                        \
    do {                                                                                           \
        if (cond) {                                                                                \
            torabo_test_report_pass(what);                                                         \
        } else {                                                                                   \
            torabo_test_report_fail(what, #cond);                                                  \
        }                                                                                          \
    } while (0)

#define T_EQ_INT(got, want, what)                                                                  \
    do {                                                                                           \
        long long g_ = (long long)(got), w_ = (long long)(want);                                   \
        if (g_ == w_) {                                                                            \
            torabo_test_report_pass(what);                                                         \
        } else {                                                                                   \
            char buf_[128];                                                                        \
            snprintf(buf_, sizeof(buf_), "got %lld, want %lld", g_, w_);                           \
            torabo_test_report_fail(what, buf_);                                                   \
        }                                                                                          \
    } while (0)

#define T_EQ_MEM(got, want, len, what)                                                             \
    do {                                                                                           \
        if (memcmp((got), (want), (len)) == 0) {                                                   \
            torabo_test_report_pass(what);                                                         \
        } else {                                                                                   \
            torabo_test_report_fail(what, "byte mismatch");                                        \
            torabo_test_hexdiff((const uint8_t *)(got), (const uint8_t *)(want), (len));           \
        }                                                                                          \
    } while (0)

/* Per-feature entry points (one file each). */
void test_trackball(void);
void test_trackpad(void);
void test_timing(void);
void test_led(void);
void test_encoder(void);
void test_macros(void);
void test_combos(void);
void test_caps(void);
void test_live_feed(void);
void test_contracts(void);
void test_coast(void);
void test_binding(void);
void test_wire_asm(void);
