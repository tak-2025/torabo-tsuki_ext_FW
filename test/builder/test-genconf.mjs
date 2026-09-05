#!/usr/bin/env node
/*
 * firmware-builder/index.html's genConf() — module-declaration smoke test
 * (PLAN-ext-fw-refactor.md phase 9, re-redesigned 2026-09-03).
 *
 * The builder has no test harness of its own (it's a single static HTML file
 * with an inline <script>, no build step, no DOM framework). This script:
 *   1. Syntax-checks the WHOLE file's inline script with `new Function(...)`
 *      (parses, never executes -- top-level code wires up `document.*`
 *      event listeners that don't exist outside a browser).
 *   2. Extracts just the pure, DOM-free slice (state/sideDevices/genConf and
 *      what they call) and runs genConf() against three hand-built `state`
 *      objects, asserting the four CONFIG_TORABO_SLOT_* / CENTRAL_SIDE lines
 *      it emits.
 *
 * Usage: node test/builder/test-genconf.mjs
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const BUILDER_HTML = join(HERE, '..', '..', 'firmware-builder', 'index.html');

let passed = 0, failed = 0;
const ok = (cond, what) => { if (cond) { passed++; console.log(`  ok   ${what}`); } else { failed++; console.log(`  FAIL ${what}`); } };
const eq = (got, want, what) => ok(got === want, `${what} (got ${JSON.stringify(got)}, want ${JSON.stringify(want)})`);

const html = readFileSync(BUILDER_HTML, 'utf8');
const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
if (!scriptMatch) { console.error('no <script> block found in firmware-builder/index.html'); process.exit(1); }
const fullScript = scriptMatch[1];

console.log('== builder: syntax check (new Function over the whole inline script) ==');
try {
  // eslint-disable-next-line no-new-func
  new Function(fullScript);
  ok(true, 'inline script parses with no SyntaxError');
} catch (e) {
  ok(false, `inline script parses with no SyntaxError -- ${e.message}`);
}

// ---------------------------------------------------------------------------
// Extract the DOM-free slice: `const state = {` ... end of `function genConf`.
// This span also drags in isPointing/sideDevices/peripheralPointing/buildModel/
// validate/TP_META/TP_SIDE/TP_CONN/TP_KIND/tpDeviceMeta -- buildModel/validate
// are defined but never called (they need the SNIP object this slice doesn't
// include), which is harmless since JS only errors on CALLING them, not on
// parsing/defining an unused function.
const sliceStart = fullScript.indexOf('const state = {');
const sliceEnd = fullScript.indexOf('function genWest(');
if (sliceStart < 0 || sliceEnd < 0) { console.error('could not locate the state..genConf slice; index.html structure changed'); process.exit(1); }
const slice = fullScript.slice(sliceStart, sliceEnd);

// Sandbox: run the slice plus a harness function that resets `state` per case
// and calls genConf({central: state.central}), returning the generated text.
const harness = new Function(
  'sides', 'central', 'features',
  `
  ${slice}
  state.central = central;
  Object.assign(state.sides.left, sides.left);
  Object.assign(state.sides.right, sides.right);
  if (features) Object.assign(state.features, features);
  return genConf({ central });
  `
);

function runCase(name, { central, sides, expect }) {
  console.log(`\n== builder case: ${name} ==`);
  const out = harness(sides, central, undefined);
  const lines = out.split('\n');
  const get = (key) => {
    const line = lines.find(l => l.startsWith(key + '='));
    return line ? line.slice(key.length + 1) : undefined;
  };
  eq(get('CONFIG_TORABO_CENTRAL_SIDE'), String(expect.central), 'CENTRAL_SIDE');
  eq(get('CONFIG_TORABO_SLOT_LEFT_STD'), String(expect.leftStd), 'SLOT_LEFT_STD');
  eq(get('CONFIG_TORABO_SLOT_LEFT_EXT'), String(expect.leftExt), 'SLOT_LEFT_EXT');
  eq(get('CONFIG_TORABO_SLOT_RIGHT_STD'), String(expect.rightStd), 'SLOT_RIGHT_STD');
  eq(get('CONFIG_TORABO_SLOT_RIGHT_EXT'), String(expect.rightExt), 'SLOT_RIGHT_EXT');
}

// ---- case 1: current real hardware (right central, right ball std, left
// encoder std, both sides extension pad) -- same fixture as
// test/wire/stubs/torabo_test_config_decl.h and torabo-studio's shared golden
// (MODULES caps word 0x2129). ----------------------------------------------
runCase('1) real hw: right-central, right-ball, left-encoder, both ext pad', {
  central: 'right',
  sides: {
    left:  { std: 'encoder', ext: 'led', extDev: 'pad' },
    right: { std: 'ball',    ext: 'led', extDev: 'pad' },
  },
  expect: { central: 2, leftStd: 9, leftExt: 2, rightStd: 1, rightExt: 2 },
});

// ---- case 2: double ball + both extension encoders (stress fixture, matches
// test/wire/stubs/torabo_test_config_decl2.h's double config). Note the ext
// BOARD field ('fpc'/'led') must be non-'none' for sideDevices() to report
// extDev at all -- 'fpc' (no LED) is used here since this fixture has no LED. --
runCase('2) double ball + both ext encoders', {
  central: 'left',
  sides: {
    left:  { std: 'ball', ext: 'fpc', extDev: 'encoder' },
    right: { std: 'ball', ext: 'led', extDev: 'encoder' },
  },
  expect: { central: 1, leftStd: 1, leftExt: 9, rightStd: 1, rightExt: 9 },
});

// ---- case 3: nothing at all (all four slots explicitly "none"). ----------
runCase('3) nothing populated', {
  central: 'right',
  sides: {
    left:  { std: 'none', ext: 'none', extDev: 'none' },
    right: { std: 'none', ext: 'none', extDev: 'none' },
  },
  expect: { central: 2, leftStd: 15, leftExt: 15, rightStd: 15, rightExt: 15 },
});

console.log(`\n---------------------------------------------`);
console.log(`passed: ${passed}   failed: ${failed}`);
process.exit(failed ? 1 : 0);
