/*
 * torabo-tsuki_ext_FW — wire golden tests (host).
 *
 * Compiles the REAL wire codecs (each feature's config_state.c) against a set of
 * minimal Zephyr/ZMK stubs and exercises them natively. Phase 0 of
 * PLAN-ext-fw-refactor.md: prove the wire formats are frozen BEFORE anything is
 * refactored, so a later phase that quietly changes a byte cannot pass CI.
 *
 * Fixtures are SYNTHETIC ONLY — hand-built dummy byte sequences. No personal
 * backup data is committed. See check-local-backup.py for the opt-in path that
 * additionally replays a real local backup JSON.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zmk/keymap.h>

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
    printf("torabo wire golden tests (ZMK_KEYMAP_LAYERS_LEN=%d)\n", ZMK_KEYMAP_LAYERS_LEN);

    test_contracts();
    test_caps();
    test_live_feed();
    test_trackball();
    test_trackpad();
    test_timing();
    test_led();
    test_encoder();
    test_macros();
    test_combos();
    test_coast();
    test_binding();
    test_wire_asm();
    test_window_read();

    printf("\n---------------------------------------------\n");
    printf("passed: %d   failed: %d\n", torabo_test_passed, torabo_test_failed);
    if (torabo_test_failed) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
