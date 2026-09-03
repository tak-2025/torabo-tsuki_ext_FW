#!/usr/bin/env python3
"""
OPTIONAL, LOCAL-ONLY cross-check: replay a real Torabo backup JSON through the
firmware's own wire codecs.

WHY IT IS NOT A FIXTURE
A backup contains the owner's keymap, macro contents and combo definitions. That
is personal data and must never land in a public repository, so the committed
fixtures are synthetic (hand-built dummy bytes) and this script stays opt-in:

    TORABO_BACKUP_JSON=~/torabo-backup-2026-07-28.json ./test/wire/run-tests.sh

It reads the file in place, never copies it anywhere, and prints only lengths,
versions and pass/fail - never blob contents.

WHAT IT CHECKS
Each section's `wireBase64` is a verbatim READ blob from a real keyboard, so it
is the strongest possible statement of "what the firmware actually emits".
Because READ always emits the LATEST version while a stored blob may be older
(a v2 trackball wire, say), byte equality is not the right invariant. What must
hold is CONVERGENCE:

    apply(blob) -> encode = A      (accepted, upgraded to the current version)
    apply(A)    -> encode = A      (idempotent from there on)

A wire that fails that has either lost information on the way in or is not
stable across a save/restore cycle - which is exactly what breaks a backup.

The trackball / trackpad / encoder wire LENGTH depends on the build's
ZMK_KEYMAP_LAYERS_LEN, so the helper is rebuilt per section with the layer count
read out of the blob's own header.
"""

import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.abspath(os.path.join(HERE, "..", ".."))

# JSON section -> (feature key for the helper, how to read layer_count from the blob)
SECTIONS = {
    "trackball": ("ztc", lambda b: b[3]),
    "trackpad": ("tp", lambda b: b[4]),
    "macros": ("dm", lambda b: None),
    "combos": ("cb", lambda b: None),
    "timing": ("tmg", lambda b: None),
    "led": ("led", lambda b: None),
    "encoder": ("enc", lambda b: b[3]),
}


def find_cc():
    for name in (os.environ.get("CC"), "cc", "gcc", "clang"):
        if name and shutil.which(name):
            return name
    return None


def build_helper(cc, outdir, layers):
    out = os.path.join(outdir, "roundtrip_l%s" % layers)
    if os.path.exists(out):
        return out
    cmd = [
        cc, "-std=c11", "-O1", "-Wall", "-Wno-unused-parameter",
        "-include", os.path.join(HERE, "stubs", "torabo_test_config.h"),
        "-DZMK_KEYMAP_LAYERS_LEN=%d" % layers,
        "-I" + HERE, "-I" + os.path.join(HERE, "stubs"),
    ]
    for feat in ("caps", "trackball", "trackpad", "timing", "led", "encoder", "macros",
                 "combos", "live_feed"):
        cmd.append("-I" + os.path.join(MODULE, "features", feat, "include"))
    for src in (
        "caps/src/caps.c", "trackball/src/config_state.c", "trackpad/src/config_state.c",
        "timing/src/config_state.c", "led/src/config_state.c", "encoder/src/config_state.c",
        "macros/src/config_state.c", "combos/src/combo_state.c",
    ):
        cmd.append(os.path.join(MODULE, "features", src))
    cmd += [os.path.join(HERE, "roundtrip_main.c"), os.path.join(HERE, "host_support.c")]
    cmd += ["-o", out]
    subprocess.run(cmd, check=True)
    return out


def main(argv):
    if len(argv) != 2:
        print("usage: check-local-backup.py <backup.json>", file=sys.stderr)
        return 2
    path = argv[1]
    cc = find_cc()
    if not cc:
        print("no C compiler; skipping", file=sys.stderr)
        return 0

    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    print("backup format=%r version=%r" % (doc.get("format"), doc.get("version")))

    failures = 0
    checked = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (feat, layers_of) in SECTIONS.items():
            sect = doc.get(name)
            if not isinstance(sect, dict) or "wireBase64" not in sect:
                continue
            blob = base64.b64decode(sect["wireBase64"])
            layers = layers_of(blob) if blob else None
            if layers is None:
                layers = 10
            if not (1 <= layers <= 32):
                print("  %-10s SKIP  implausible layer_count %r" % (name, layers))
                continue
            try:
                helper = build_helper(cc, tmp, layers)
            except subprocess.CalledProcessError:
                print("  %-10s FAIL  helper build failed at %d layers" % (name, layers))
                failures += 1
                continue
            proc = subprocess.run(
                [helper, feat],
                input=blob.hex().encode("ascii"),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            out = proc.stdout.decode("ascii", "replace").strip()
            checked += 1
            status = "ok  " if proc.returncode == 0 else "FAIL"
            if proc.returncode != 0:
                failures += 1
            print("  %-10s %s  %d B in, layers=%d :: %s" % (name, status, len(blob), layers, out))

    if checked == 0:
        print("  (no recognised wire sections in this backup)")
    print("backup cross-check: %d section(s), %d failure(s)" % (checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
