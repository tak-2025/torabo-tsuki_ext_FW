/*
 * torabo-tsuki_ext_FW — PLAN-ext-fw-refactor.md phase 9 PRIMARY "declared"
 * fixture (the real right-central hardware pattern).
 *
 * A second, separate binary from the main wire_tests_l* runs (see main.c):
 * compiles ONLY caps.c, against torabo_test_config.h + the phase 9 override
 * header (stubs/torabo_test_config_decl.h — re-redesigned 2026-09-03 into the
 * single MODULES-row fixture), and runs the one golden test that cares about
 * it (test_caps_decl.c). Kept apart from main.c's test list on purpose —
 * test_caps.c's baseline golden vector must never see these non-zero Kconfig
 * overrides, and this binary must never run test_caps.c's all-zero one nor
 * test_caps_decl2.c's double-config one (see main_decl2.c).
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
    printf("torabo wire golden tests — PLAN phase 9 declared fixture\n");

    test_caps_decl();

    printf("\n---------------------------------------------\n");
    printf("passed: %d   failed: %d\n", torabo_test_passed, torabo_test_failed);
    if (torabo_test_failed) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
