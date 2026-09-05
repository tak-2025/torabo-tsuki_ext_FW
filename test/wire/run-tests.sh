#!/usr/bin/env bash
#
# Host wire golden tests for torabo-tsuki_ext_FW (PLAN-ext-fw-refactor.md phase 0).
#
# Compiles the REAL wire codecs — each feature's config_state.c, unmodified —
# against the minimal Zephyr/ZMK stubs in stubs/, and runs them natively. No
# Zephyr, no west, no board: just a C compiler.
#
# Usage:
#   ./test/wire/run-tests.sh                 # build + run at every layer count
#   ./test/wire/run-tests.sh --docker        # same, inside a container with gcc
#                                            # (for hosts with no native cc)
#   ./test/wire/run-tests.sh --clean         # remove the build directory
#   CC=clang ./test/wire/run-tests.sh        # pick the compiler
#   LAYERS="10" ./test/wire/run-tests.sh     # restrict the layer-count sweep
#
# Optional, LOCAL ONLY (never in CI, never committed): replay a real backup JSON
# through the codecs as an extra cross-check —
#   TORABO_BACKUP_JSON=/path/to/torabo-backup-*.json ./test/wire/run-tests.sh
# See check-local-backup.py; the file is read, never copied into the repo.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(cd "${HERE}/../.." && pwd)"
BUILD_DIR="${HERE}/.build"
DOCKER_IMAGE="${TORABO_TEST_IMAGE:-zmkfirmware/zmk-build-arm:stable}"

# ZMK_KEYMAP_LAYERS_LEN is devicetree-derived in a real build and is the ONE
# thing that changes the ztc / tp / enc wire LENGTH (PLAN §0.4). 10 is the field
# value; the others prove nothing is hard-coded to it.
LAYERS="${LAYERS:-10 4 20}"

case "${1:-}" in
--clean)
    echo "Removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
    exit 0
    ;;
--docker)
    # Re-run this same script inside a container that definitely has a C
    # compiler. The module is mounted read-write ONLY so the build dir can be
    # written; nothing else is touched.
    echo ">>> running the host tests inside ${DOCKER_IMAGE}"
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" docker run --rm \
        -v "${MODULE_DIR}:/torabo" \
        -w /torabo \
        --entrypoint "" \
        "${DOCKER_IMAGE}" \
        bash -lc "LAYERS='${LAYERS}' ./test/wire/run-tests.sh"
    exit $?
    ;;
esac

# ---- pick a compiler --------------------------------------------------------
if [ -z "${CC:-}" ]; then
    for candidate in cc gcc clang; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            CC="${candidate}"
            break
        fi
    done
fi
if [ -z "${CC:-}" ]; then
    echo "ERROR: no C compiler found (tried \$CC, cc, gcc, clang)." >&2
    echo "       On a machine without one, run:  ./test/wire/run-tests.sh --docker" >&2
    exit 127
fi
echo ">>> CC = ${CC}  ($(${CC} --version 2>&1 | head -1))"

mkdir -p "${BUILD_DIR}"

# ---- the production sources under test --------------------------------------
# Only pure wire/state code. GATT services, tunnel bridges, input processors and
# behaviors are NOT compiled here: they need the Bluetooth / input / behavior
# subsystems, which cannot be stubbed without either changing production code or
# writing a fake big enough to test itself instead.
#
# The parts of them that WERE extracted into header-only helpers are tested
# directly by the test files below, without their .c hosts:
# torabo_common/coast.h (test_coast.c), binding.h (test_binding.c) and
# torabo_common/wire_asm.h (test_wire_asm.c).
SOURCES=(
    "${MODULE_DIR}/features/caps/src/caps.c"
    "${MODULE_DIR}/features/trackball/src/config_state.c"
    "${MODULE_DIR}/features/trackpad/src/config_state.c"
    "${MODULE_DIR}/features/timing/src/config_state.c"
    "${MODULE_DIR}/features/led/src/config_state.c"
    "${MODULE_DIR}/features/encoder/src/config_state.c"
    "${MODULE_DIR}/features/macros/src/config_state.c"
    "${MODULE_DIR}/features/combos/src/combo_state.c"
)

TESTS=(
    "${HERE}/main.c"
    "${HERE}/host_support.c"
    "${HERE}/test_contracts.c"
    "${HERE}/test_caps.c"
    "${HERE}/test_live_feed.c"
    "${HERE}/test_trackball.c"
    "${HERE}/test_trackpad.c"
    "${HERE}/test_timing.c"
    "${HERE}/test_led.c"
    "${HERE}/test_encoder.c"
    "${HERE}/test_macros.c"
    "${HERE}/test_combos.c"
    "${HERE}/test_coast.c"
    "${HERE}/test_binding.c"
    "${HERE}/test_wire_asm.c"
)

INCLUDES=(
    "-I${HERE}"
    "-I${HERE}/stubs"
    "-I${MODULE_DIR}/features/caps/include"
    "-I${MODULE_DIR}/features/trackball/include"
    "-I${MODULE_DIR}/features/trackpad/include"
    "-I${MODULE_DIR}/features/timing/include"
    "-I${MODULE_DIR}/features/led/include"
    "-I${MODULE_DIR}/features/encoder/include"
    "-I${MODULE_DIR}/features/macros/include"
    "-I${MODULE_DIR}/features/combos/include"
    "-I${MODULE_DIR}/features/live_feed/include"
    "-I${MODULE_DIR}/features/common/include"
)

CFLAGS=(
    -std=c11
    -O1
    -g
    -Wall
    -Wno-unused-parameter
    -fno-strict-aliasing
    -include "${HERE}/stubs/torabo_test_config.h"
)

rc=0
for n in ${LAYERS}; do
    bin="${BUILD_DIR}/wire_tests_l${n}"
    echo
    echo "============================================================="
    echo ">>> building with ZMK_KEYMAP_LAYERS_LEN=${n}"
    echo "============================================================="
    "${CC}" "${CFLAGS[@]}" "-DZMK_KEYMAP_LAYERS_LEN=${n}" \
        "${INCLUDES[@]}" "${SOURCES[@]}" "${TESTS[@]}" -o "${bin}"
    if ! "${bin}"; then
        rc=1
    fi
done

# ---- PLAN-ext-fw-refactor.md phase 9: the "declared" module-layout fixtures -
# CONFIG_TORABO_CENTRAL_SIDE / _SLOT_LEFT_STD / _LEFT_EXT / _RIGHT_STD / _RIGHT_EXT
# (re-redesigned 2026-09-03 into one MODULES row of four 4-bit slot values)
# only affect caps.c and are independent of ZMK_KEYMAP_LAYERS_LEN, so these are
# two extra builds (not part of the LAYERS sweep above), each compiling only
# caps.c against the baseline config PLUS its own override header (the second
# -include applies its #undef/#define after the baseline's). Deliberately two
# separate binaries/mains (main_decl.c / main_decl2.c), so test_caps.c's
# all-zero golden vector, the primary real-hardware fixture and the secondary
# double-config fixture never compile against each other's overrides.
decl_bin="${BUILD_DIR}/wire_tests_decl"
echo
echo "============================================================="
echo ">>> building the PLAN phase 9 'declared' caps fixture (primary: real hw)"
echo "============================================================="
"${CC}" "${CFLAGS[@]}" "-DZMK_KEYMAP_LAYERS_LEN=10" \
    -include "${HERE}/stubs/torabo_test_config_decl.h" \
    "${INCLUDES[@]}" \
    "${MODULE_DIR}/features/caps/src/caps.c" \
    "${HERE}/main_decl.c" "${HERE}/test_caps_decl.c" \
    -o "${decl_bin}"
if ! "${decl_bin}"; then
    rc=1
fi

decl2_bin="${BUILD_DIR}/wire_tests_decl2"
echo
echo "============================================================="
echo ">>> building the PLAN phase 9 'declared' caps fixture (secondary: double config)"
echo "============================================================="
"${CC}" "${CFLAGS[@]}" "-DZMK_KEYMAP_LAYERS_LEN=10" \
    -include "${HERE}/stubs/torabo_test_config_decl2.h" \
    "${INCLUDES[@]}" \
    "${MODULE_DIR}/features/caps/src/caps.c" \
    "${HERE}/main_decl2.c" "${HERE}/test_caps_decl2.c" \
    -o "${decl2_bin}"
if ! "${decl2_bin}"; then
    rc=1
fi

# ---- optional: replay a REAL local backup (never committed, never in CI) -----
if [ -n "${TORABO_BACKUP_JSON:-}" ]; then
    echo
    echo "============================================================="
    echo ">>> optional local cross-check against ${TORABO_BACKUP_JSON}"
    echo "============================================================="
    if command -v python3 >/dev/null 2>&1; then
        PY=python3
    elif command -v python >/dev/null 2>&1; then
        PY=python
    else
        echo "python not found; skipping the backup cross-check" >&2
        PY=""
    fi
    if [ -n "${PY}" ]; then
        if ! "${PY}" "${HERE}/check-local-backup.py" "${TORABO_BACKUP_JSON}"; then
            rc=1
        fi
    fi
fi

echo
if [ "${rc}" -eq 0 ]; then
    echo ">>> ALL GREEN"
else
    echo ">>> FAILURES (see above)"
fi
exit "${rc}"
