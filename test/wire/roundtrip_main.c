/*
 * Helper binary for check-local-backup.py — NOT part of the committed test
 * suite's assertions.
 *
 *   roundtrip <feature>   with a hex-encoded wire blob on stdin
 *
 * Applies the blob, re-encodes, applies the result and re-encodes again, then
 * asserts the two encodings are identical (convergence — see the docstring in
 * check-local-backup.py). Prints a one-line summary; exits non-zero on failure.
 *
 * It prints LENGTHS and VERSIONS only, never blob contents, because the input is
 * somebody's real keymap.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zmk_dynamic_keymap/dcombo.h>
#include <zmk_dynamic_keymap/dmac.h>
#include <zmk_encoder_config/config.h>
#include <zmk_led_config/config.h>
#include <zmk_timing_config/config.h>
#include <zmk_trackball_config/config.h>
#include <zmk_trackpad_config/config.h>

#define MAX_WIRE 8192

static uint8_t in_buf[MAX_WIRE];
static uint8_t enc_a[MAX_WIRE];
static uint8_t enc_b[MAX_WIRE];

static int read_hex(uint8_t *out, size_t cap, size_t *out_len) {
    size_t n = 0;
    int hi = -1, c;
    while ((c = getchar()) != EOF) {
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            continue; /* whitespace / newlines */
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= cap) {
                return -1;
            }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    *out_len = n;
    return hi < 0 ? 0 : -1;
}

typedef int (*apply_fn)(const uint8_t *, uint16_t);
typedef int (*encode_fn)(uint8_t *, uint16_t, uint16_t *);

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: roundtrip <ztc|tp|tmg|led|enc|dm|cb>\n");
        return 2;
    }
    size_t in_len = 0;
    if (read_hex(in_buf, sizeof(in_buf), &in_len) != 0 || in_len == 0) {
        printf("bad hex input\n");
        return 1;
    }

    apply_fn apply = NULL;
    encode_fn encode = NULL;
    const char *feat = argv[1];
    int ver_off = -1; /* where the version byte lives, for the summary line */

    if (strcmp(feat, "ztc") == 0) {
        apply = ztc_apply_wire;
        encode = ztc_encode_wire;
        ver_off = 2;
    } else if (strcmp(feat, "tp") == 0) {
        apply = tp_apply_wire;
        encode = tp_encode_wire;
        ver_off = 2;
    } else if (strcmp(feat, "tmg") == 0) {
        apply = tmg_apply_wire;
        encode = tmg_encode_wire;
        ver_off = 0;
    } else if (strcmp(feat, "led") == 0) {
        apply = led_apply_wire;
        encode = led_encode_wire;
        ver_off = 2;
    } else if (strcmp(feat, "enc") == 0) {
        apply = enc_apply_wire;
        encode = enc_encode_wire;
        ver_off = 2;
    } else if (strcmp(feat, "dm") == 0) {
        /* macros: READ is all slots, WRITE is one slot at a time, so a stored
         * READ blob is replayed slot by slot the way a restore does it. */
        if (in_len != DM_READ_WIRE_LEN) {
            printf("dm READ blob is %zu B, this build expects %d B\n", in_len,
                   (int)DM_READ_WIRE_LEN);
            return 1;
        }
        for (uint8_t k = 0; k < DM_SLOTS; k++) {
            const uint8_t *sp = &in_buf[DM_READ_HDR + (uint32_t)k * DM_READ_SLOT];
            uint8_t used = sp[0];
            if (used > DM_STEPS) {
                printf("slot %u claims %u steps\n", k, used);
                return 1;
            }
            uint8_t w[DM_WRITE_MAX];
            w[0] = DM_VERSION;
            w[1] = k;
            w[2] = used;
            memcpy(&w[3], &sp[1], (size_t)used * DM_WIRE_STEP);
            int rc = dm_apply_write_wire(w, (uint16_t)(DM_WRITE_HDR + used * DM_WIRE_STEP));
            if (rc) {
                printf("slot %u rejected (%d)\n", k, rc);
                return 1;
            }
        }
        uint16_t la = 0;
        if (dm_encode_read_wire(enc_a, sizeof(enc_a), &la) != 0) {
            printf("re-encode failed\n");
            return 1;
        }
        if (la != in_len || memcmp(enc_a, in_buf, la) != 0) {
            printf("slot-by-slot restore did NOT reproduce the blob\n");
            return 1;
        }
        printf("v%u, %u slot(s) restored, byte-identical", in_buf[2], (unsigned)DM_SLOTS);
        printf("\n");
        return 0;
    } else if (strcmp(feat, "cb") == 0) {
        if (in_len != CB_READ_WIRE_LEN) {
            printf("cb READ blob is %zu B, this build expects %d B\n", in_len,
                   (int)CB_READ_WIRE_LEN);
            return 1;
        }
        for (uint8_t s = 0; s < CB_SLOTS; s++) {
            uint8_t w[CB_WRITE_MAX];
            w[0] = CB_VERSION;
            w[1] = s;
            memcpy(&w[CB_WRITE_HDR], &in_buf[CB_READ_HDR + (uint32_t)s * CB_WIRE_SLOT],
                   CB_WIRE_SLOT);
            int rc = cb_apply_write_wire(w, CB_WRITE_MAX);
            if (rc) {
                printf("slot %u rejected (%d)\n", s, rc);
                return 1;
            }
        }
        uint16_t la = 0;
        if (cb_encode_read_wire(enc_a, sizeof(enc_a), &la) != 0) {
            printf("re-encode failed\n");
            return 1;
        }
        if (la != in_len || memcmp(enc_a, in_buf, la) != 0) {
            printf("slot-by-slot restore did NOT reproduce the blob\n");
            return 1;
        }
        printf("v%u, %u slot(s) restored, byte-identical\n", in_buf[2], (unsigned)CB_SLOTS);
        return 0;
    } else {
        fprintf(stderr, "unknown feature %s\n", feat);
        return 2;
    }

    const unsigned in_ver = (ver_off >= 0) ? in_buf[ver_off] : 0;

    int rc = apply(in_buf, (uint16_t)in_len);
    if (rc != 0) {
        printf("REJECTED on apply (%d); stored v%u", rc, in_ver);
        printf("\n");
        return 1;
    }
    uint16_t la = 0, lb = 0;
    if (encode(enc_a, sizeof(enc_a), &la) != 0) {
        printf("re-encode failed\n");
        return 1;
    }
    if (apply(enc_a, la) != 0) {
        printf("the firmware's OWN output was rejected on re-apply\n");
        return 1;
    }
    if (encode(enc_b, sizeof(enc_b), &lb) != 0) {
        printf("second re-encode failed\n");
        return 1;
    }
    if (la != lb || memcmp(enc_a, enc_b, la) != 0) {
        printf("NOT convergent: two encodes differ (%u vs %u B)\n", la, lb);
        return 1;
    }
    printf("stored v%u -> emits v%u, %u B, convergent", in_ver,
           (ver_off >= 0) ? enc_a[ver_off] : 0, la);
    if (la == in_len && memcmp(enc_a, in_buf, la) == 0) {
        printf(" (and byte-identical to the stored blob)");
    }
    printf("\n");
    return 0;
}
