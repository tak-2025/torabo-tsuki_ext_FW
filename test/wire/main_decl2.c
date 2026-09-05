/*
 * torabo-tsuki_ext_FW — PLAN-ext-fw-refactor.md phase 9 SECOND "declared"
 * fixture (double configuration).
 *
 * A third, separate binary from the main wire_tests_l* runs and from
 * wire_tests_decl (see main_decl.c): compiles ONLY caps.c, against
 * torabo_test_config.h + stubs/torabo_test_config_decl2.h, and runs the one
 * golden test that cares about it (test_caps_decl2.c). Kept apart on purpose:
 * neither test_caps.c's all-zero vector nor test_caps_decl.c's primary
 * (real-hardware) vector must ever see this fixture's overrides.
 */

#include <stdio.h>

#include "torabo_test.h"

int torabo_test_passed = 0;
int torabo_test_failed = 0;
const char *torabo_test_group = "?";

void torabo_test_begin(const char *group) {
    torabo_test_group = group;
    printf("\n== %s ==\n", group);
}

void torabo_test_report_pass(const char *what) {
    torabo_test_passed++;
    printf("  ok   %s\n", what);
}

void torabo_test_report_fail(const char *what, const char *detail) {
    torabo_test_failed++;
    printf("  FAIL %s  (%s)\n", what, detail ? detail : "");
}

void torabo_test_hexdiff(const uint8_t *got, const uint8_t *want, size_t len) {
    size_t shown = 0;
    for (size_t i = 0; i < len && shown < 16; i++) {
        if (got[i] != want[i]) {
            printf("       byte %zu: got 0x%02x want 0x%02x\n", i, got[i], want[i]);
            shown++;
        }
    }
}

int main(void) {
    printf("torabo wire golden tests — PLAN phase 9 double-config declared fixture\n");

    test_caps_decl2();

    printf("\n---------------------------------------------\n");
    printf("passed: %d   failed: %d\n", torabo_test_passed, torabo_test_failed);
    if (torabo_test_failed) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
