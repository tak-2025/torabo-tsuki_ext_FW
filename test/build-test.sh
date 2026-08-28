#!/bin/bash
#
# Compile-test harness for torabo-tsuki_ext_FW (the combined feature module).
#
# Builds a torabo-tsuki central firmware that pulls THIS module via
# ZMK_EXTRA_MODULES, with both features enabled (test/ztc_test.conf), to prove
# the module links into a real ZMK build. The parent config repo is mounted
# READ-ONLY at /hub and never modified.
#
# Usage:
#   ./test/build-test.sh                 # build left (default)
#   SHIELD=torabo_tsuki_lp_right ./test/build-test.sh
#   ./test/build-test.sh --clean         # remove .zmk-workspace
#   ./test/build-test.sh --update        # force west update first

set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(cd "${TEST_DIR}/.." && pwd)"
HUB_DIR="$(cd "${MODULE_DIR}/../tako-custom" && pwd)"
WORKSPACE_DIR="${MODULE_DIR}/.zmk-workspace"
FIRMWARE_DIR="${MODULE_DIR}/firmware"
DOCKER_IMAGE="zmkfirmware/zmk-build-arm:stable"

BOARD="bmp_boost"
SHIELD="${SHIELD:-torabo_tsuki_lp_left}"
SIDE="${SHIELD##*_}"   # left | right
# Distribution path: torabo-trackball snippet supplies feature A (overlay+conf)
# from THIS module, so the keyboard body stays unmodified. The macro feature
# (&dmac node + CONFIG_ZMK_DYNAMIC_KEYMAP) comes from the mounted user config's
# keymap/shield .conf here, so we do NOT add torabo-macros (would duplicate the
# node). A pristine-body user would instead add `torabo-macros` too.
SNIPPET="studio-rpc-usb-uart split-central input-trackball input-listener torabo-trackball ${EXTRA_SNIPPET:-}"
EXTRA_BUILD_ARGS="${EXTRA_BUILD_ARGS:-}"

run_docker() { MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" docker "$@"; }

FORCE_UPDATE=0
case "${1:-}" in
    --clean) echo "Removing ${WORKSPACE_DIR}"; rm -rf "${WORKSPACE_DIR}"; exit 0 ;;
    --update) FORCE_UPDATE=1 ;;
esac

echo "MODULE_DIR = ${MODULE_DIR}"
echo "HUB_DIR    = ${HUB_DIR}"
echo "SHIELD     = ${SHIELD}"

mkdir -p "${WORKSPACE_DIR}"
rm -rf "${WORKSPACE_DIR}/config"
mkdir -p "${WORKSPACE_DIR}/config"
cp -R "${HUB_DIR}/config/." "${WORKSPACE_DIR}/config/"

ART="tb_ext_${SIDE}_central"

run_docker run --rm \
    -v "${WORKSPACE_DIR}:/workspace" \
    -v "${HUB_DIR}:/hub:ro" \
    -v "${MODULE_DIR}:/torabo:ro" \
    -w /workspace \
    --entrypoint "" \
    "${DOCKER_IMAGE}" \
    bash -c "
        set -ex
        if [ ! -f /workspace/.west/config ]; then
            west init -l /workspace/config
            west update --fetch-opt=--filter=tree:0
        elif [ '${FORCE_UPDATE}' = '1' ]; then
            west update --fetch-opt=--filter=tree:0
        fi
        west zephyr-export
        BUILD_DIR=/tmp/build-${ART}
        rm -rf \"\${BUILD_DIR}\"
        west build -s zmk/app -d \"\${BUILD_DIR}\" -b ${BOARD} -S \"${SNIPPET}\" -- \
            -DSHIELD=\"${SHIELD}\" \
            -DZMK_CONFIG=/workspace/config \
            -DZMK_EXTRA_MODULES='/hub;/torabo' ${EXTRA_BUILD_ARGS}
        mkdir -p /workspace/artifacts
        if [ -f \"\${BUILD_DIR}/zephyr/zmk.uf2\" ]; then
            cp \"\${BUILD_DIR}/zephyr/zmk.uf2\" \"/workspace/artifacts/${ART}.uf2\"
            echo \">>> OK: ${ART}.uf2\"
        else
            echo '>>> FAIL: no uf2'; exit 2
        fi
    "

mkdir -p "${FIRMWARE_DIR}"
cp -v "${WORKSPACE_DIR}/artifacts/"*.uf2 "${FIRMWARE_DIR}/" 2>/dev/null || true
echo "Done. Firmware in ${FIRMWARE_DIR}/"
