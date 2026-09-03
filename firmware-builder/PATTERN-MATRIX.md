# torabo-tsuki Hardware-Configuration Pattern Space — Firmware-Builder Spec

> 作成: 2026-07-11（Fable モデルで全空間を整理 → Opus レビュー済み）／
> 更新: 2026-08-29（§5 のバックログ4件がすべて実装済みになったため、`*` = MISSING 表記を解消）。
> ビルダー UI のバリデーション＋buildability ロジックの **実装 source-of-truth**。
>
> **現状: 144 構成すべてが BUILDABLE-NOW です。** 以下の表に残る `MISSING` / `*` 付きトークン・
> `ENC` / `PEXT` / `REG1` の tier 列は、**当時の実装状況の記録**として残してあります
> （`index.html` の `MISSING_LABEL` は空で、ビルド不可バッジはどの構成でも出ません）。
> ただし **ビルドできる＝実機検証済みではありません** — 多くの構成は未実機検証です。

Authoritative enumeration of every valid hardware configuration for the torabo-tsuki split keyboard (ZMK, nRF52840 "bmp_boost"), the snippet composition each requires, and the missing-firmware backlog.

---

## 0. Conventions & legend

**Device codes:** `—` none · `B` trackball · `P` mini trackpad · `E` rotary encoder (EC11)
**Snippet tokens** — かつて `*` を付けていたものも **すべて実装済み**（2026-08 時点）。`*` は
以下の表・tier 列に歴史的記録として残っています。

| Token | Snippet | Status |
|---|---|---|
| `base` | `studio-rpc-usb-uart` + `split-central` (central side, always) | EXISTING |
| `tb` | `input-trackball` | EXISTING |
| `tpS` | `input-trackpad-mini` | EXISTING |
| `tpX` | `input-trackpad-ext` (central; listener built in) | EXISTING |
| `lst` | `input-listener` (central-local pointing device) | EXISTING |
| `spl0` | `input-split` (hardcoded reg=<0>) | EXISTING |
| `rcv0` | `input-split-listener` (hardcoded reg=<0>) | EXISTING |
| `tpXspl` | `input-trackpad-ext-split` — split-export of extension trackpad（peripheral 側。listener を持たず `zmk,input-split` reg=0 で central へ転送。central は既存 `rcv0` で受ける）| **EXISTING**（2026-07-12 実装, 未実機検証）|
| `spl1` / `rcv1` | reg=<1> twins。peripheral 側は `input-trackpad-ext-split-reg1`（拡張パッドを reg=1 で出す）、central 側は `input-split-listener-reg1` | **EXISTING**（実装済, 未実機検証。2個目の pointing が拡張パッドである前提のため、汎用 `input-split` reg=1 は用意していない）|
| `encS` | `input-encoder` (standard-FFC EC11 回転; central 自身に載る場合はローカル。peripheral に載る場合は central に `encRecv` を併用) | **EXISTING**（2026-07-11 実装, 未実機検証）|
| `encRecv` | `input-encoder-recv` (central 受け口: encoder device disabled + keymap-sensors ノードで LEN/index 確保。peripheral に標準エンコーダがある central 側に付ける) | **EXISTING**（2026-07-11 実装, 未実機検証）|
| `encX` | `input-encoder-ext`（＋ central 受けの `input-encoder-ext-recv`）extension-FFC EC11; same split-sensor semantics | **EXISTING**（実装済, 未実機検証）|
| `btn` | エンコーダ押しボタン。**`kscan-gpio-direct` ではなく input 経路**で実装した（キー位置を消費しないので transform / keymap は無改造）。配置別に `torabo-encoder-btn-local`(central P0.20) / `-local-ext`(central P0.31) / `-split`(periph P0.20) / `-split-ext`(periph P0.31) ＋ central 側の `torabo-encoder-btn-recv` | **EXISTING**（実装済, 未実機検証）|
| `LED` | `torabo-led-live` (central; rule table + GATT, drives both sides) ＋ `torabo-led-ext-periph` (peripheral display side) | EXISTING |

**Tier codes:** `BN` = BUILDABLE-NOW · `ENC` = needs-encoder-FW · `PEXT` = needs-peripheral-extension-FW · `REG1` = needs-reg1-twin. Tiers combine (`ENC+PEXT` etc.).
**⚠ 2026-08-29 現在、`ENC` / `PEXT` / `REG1` の依存はすべて実装済みで、実効的には全 144 構成が `BN` です。** 以下の tier 列は当時の記録です。
**Dep codes:** `E-std` = `encS*`+`btn*` · `E-ext` = `encX*`+`btn*` · `X-split` = `tpXspl*` (+ central receive wiring) · `reg1` = `spl1*`+`rcv1*`.

**Symmetry:** all tables below fix **central = RIGHT, peripheral = LEFT**. The `central = LEFT` case is the identical table with the left/right columns swapped; the builder should canonicalize to (centralSide, peripheralSide) before lookup.

---

## 1. Slot model & valid per-side device sets

### 1.1 Per-side model

```
Side = {
  standardDevice : none | ball | pad | encoder        // standard FFC: P0.18/P0.16, IRQ P0.20, POW P0.08
  extenderType   : none | fpc | fpc-led               // extension FFC add-on board
  extensionDevice: none | pad | encoder               // extension FFC: P0.17/P0.21, RDY P0.31, POW P0.24
}
```

### 1.2 Validation rules (hard constraints)

1. **Bus exclusivity:** at most ONE device per connector (all devices contend for the connector's pin pair). Max 2 devices per side.
2. **No trackball on extension** — extension FFC has no SPI. `extensionDevice ∈ {none, pad, encoder}` only.
3. **Extension device requires an extender:** `extensionDevice ≠ none` ⇒ `extenderType ≠ none`.
4. `extenderType ≠ none` with `extensionDevice = none` is **legal** (passthrough / LED-only).
5. `extenderType = fpc-led` adds the LED snippets (docs/SNIPPETS.md §3/§4): the central gets `torabo-led-live` (rule table + GATT, drives both sides), and a peripheral with `extenderType = fpc-led` additionally gets `torabo-led-ext-periph` (receives (color, pattern) from the central over split). The legacy fixed-behaviour `torabo-status-led-ext` was removed in favour of this one path.
6. Encoder is allowed on BOTH connectors (confirmed decision). It uses only GPIO (no POW rail).

### 1.3 Enumeration of valid per-side states

4 standard × 7 extension states (`none/none`, `{fpc, fpc-led} × {none, pad, encoder}`) = **28 valid states per side**. Collapsing extenderType (it never affects buildability; it only gates rules 3–5) gives the **12 canonical device sets** used everywhere below:

| Set | std | ext | Notes |
|---|---|---|---|
| S1 | — | — | Keys-only side. Legal. |
| S2 | — | P | **Odd but legal** — extension pad, empty standard connector |
| S3 | — | E | **Odd but legal** — extension encoder, empty standard |
| S4 | B | — | |
| S5 | B | P | Two pointing devices on one side |
| S6 | B | E | |
| S7 | P | — | |
| S8 | P | P | Double pad on one side |
| S9 | P | E | |
| S10 | E | — | |
| S11 | E | P | |
| S12 | E | E | **Odd but legal** — double encoder one side; composite kscan must carry both buttons (P0.20 + P0.31) |

Invalid (builder must reject): trackball in `ext`; any `ext` device with `extenderType = none`; >1 device per connector.

---

## 2. Role-aware snippet composition table

Every (device × connector × role) cell. "Central adds" = fragments that go on the **central** side because of a **peripheral** device.

| Device | Conn | Role | Fragments on that side | Central adds | Status |
|---|---|---|---|---|---|
| — always — | — | central | `base` = `studio-rpc-usb-uart` + `split-central` | — | EXISTING |
| — LED — | ext (extender=FPC+LED) | central | `torabo-led-live` | — | EXISTING |
| — LED — | ext (extender=FPC+LED) | peripheral | `torabo-led-ext-periph` | `torabo-led-live` | EXISTING |
| Trackball | standard | central | `tb` + `lst` | — | EXISTING |
| Trackball | standard | peripheral | `tb` + `spl0` | `rcv0` | EXISTING |
| Trackball | extension | any | **INVALID** (no SPI on extension) | — | n/a |
| Mini pad | standard | central | `tpS` + `lst` | — | EXISTING |
| Mini pad | standard | peripheral | `tpS` + `spl0` | `rcv0` | EXISTING |
| Mini pad | extension | central | `tpX` — listener built in | — | EXISTING |
| Mini pad | extension | peripheral | `tpXspl` (slot 0) / `tpXspl1` = `input-trackpad-ext-split-reg1` (slot 1) | `rcv0` if slot 0, `rcv1` = `input-split-listener-reg1` if slot 1 | EXISTING（未実機検証）|
| Encoder | standard | central | `encS` + `encLive` + `btn` = `torabo-encoder-btn-local` (P0.20) | — | EXISTING（未実機検証）|
| Encoder | standard | peripheral | `encS` (split sensor) + `btn` = `torabo-encoder-btn-split` (P0.20) | `encRecv` = `input-encoder-recv`（§0; sensor-index/LEN reservation on the central。No reg slot: ZMK relays sensors natively）＋ `encLive` ＋ `torabo-encoder-btn-recv` | EXISTING（未実機検証）|
| Encoder | extension | central | `encX` + `encLive` + `btn` = `torabo-encoder-btn-local-ext` (P0.31) | — | EXISTING（未実機検証）|
| Encoder | extension | peripheral | `encX` (split sensor) + `btn` = `torabo-encoder-btn-split-ext` (P0.31) | `encXRecv` = `input-encoder-ext-recv` ＋ `encLive` ＋ `torabo-encoder-btn-recv` | EXISTING（未実機検証）|
| — 2nd peripheral pointing dev — | ext pad | peripheral | `tpXspl1` instead of a second `spl0` | `rcv1` | EXISTING（未実機検証）|

> **§2 の補足（2026-08-29）** — エンコーダのライブ設定 `torabo-encoder-live` は、回転が
> central・peripheral どちらにあっても **central に1つだけ**載ります（ストア／GATT／回転ルーターは
> ホストと話す側にしか置けないため）。ボタンも同様に、peripheral 側にあれば central が
> `torabo-encoder-btn-recv` で受けます。実装の正は `index.html` の `buildModel()` です。

**Reg-slot assignment rule (peripheral pointing devices only; encoders never consume a slot):**
standard-connector pointing device → reg <0>; extension pad → reg <0> if the standard slot holds no pointing device, else reg <1>. どちらのスロットも実装済みです。

---

## 3. Buildability classifier

**Input:** `left: Side`, `right: Side`, `centralSide ∈ {left, right}`. Canonicalize: `C` = central side config, `Pф` = peripheral side config.

**Algorithm:**

1. **Validate** both sides against rules 1.2. Reject invalid.
2. **Snippet list, central side:** start with `base`; + `LED` if `C.extenderType = fpc-led`; + central fragment for `C.standardDevice` and `C.extensionDevice` (table §2, role=central); + one receiver (`rcv0` / `rcv1*`) per peripheral **pointing** device per the reg-slot rule.
3. **Snippet list, peripheral side:** peripheral fragments for `Pф.standardDevice` and `Pф.extensionDevice` (table §2, role=peripheral), with `spl0`/`spl1*`/`tpXspl*` per the reg-slot rule.
4. **Missing-dep set `M`:**
   - any encoder on any side/connector → add `E-std` and/or `E-ext` (each includes `btn*`);
   - `Pф.extensionDevice = pad` → add `X-split`;
   - `Pф` has **two pointing devices** (std ∈ {B,P} AND ext = P) → add `reg1`.
   Note: encoders never trigger `reg1` (sensor path, not input-split).
5. **Tiers** (non-exclusive; a config carries every matching tag):
   - **BUILDABLE-NOW** ⇔ `M = ∅`
   - **needs-encoder-FW** ⇔ `E-std ∈ M` or `E-ext ∈ M`
   - **needs-peripheral-extension-FW** ⇔ `X-split ∈ M`
   - **needs-reg1-twin** ⇔ `reg1 ∈ M`

---

## 4. Complete enumerated pattern table

Fixed: **central = RIGHT** (swap columns for central = LEFT — identical results). 12 left sets × 12 right sets = **144 configurations**, grouped by left set. Central side implicitly includes `base` (+`LED` when its extender is FPC+LED — tier-neutral). Extender type is collapsed per §1.3.

Right-side snippet contribution is identical in every group:

| rightStd/rightExt | R adds | R deps |
|---|---|---|
| —/— | — | — |
| —/P | `tpX` | — |
| —/E | `encX*·btn*` | E-ext |
| B/— | `tb·lst` | — |
| B/P | `tb·lst·tpX` | — |
| B/E | `tb·lst·encX*·btn*` | E-ext |
| P/— | `tpS·lst` | — |
| P/P | `tpS·lst·tpX` | — |
| P/E | `tpS·lst·encX*·btn*` | E-ext |
| E/— | `encS*·btn*` | E-std |
| E/P | `encS*·btn*·tpX` | E-std |
| E/E | `encS*·btn*·encX*·btn*` | E-std, E-ext |

### Group 1 — left = —/— (rows 1–12) · L adds: nothing

| # | Lstd | Lext | Rstd | Rext | Snippets (beyond base) | Missing deps | Tier |
|---|---|---|---|---|---|---|---|
| 1 | — | — | — | — | — | — | BN |
| 2 | — | — | — | P | R:tpX | — | BN |
| 3 | — | — | — | E | R:encX*·btn* | E-ext | ENC |
| 4 | — | — | B | — | R:tb·lst | — | BN |
| 5 | — | — | B | P | R:tb·lst·tpX | — | BN |
| 6 | — | — | B | E | R:tb·lst·encX*·btn* | E-ext | ENC |
| 7 | — | — | P | — | R:tpS·lst | — | BN |
| 8 | — | — | P | P | R:tpS·lst·tpX | — | BN |
| 9 | — | — | P | E | R:tpS·lst·encX*·btn* | E-ext | ENC |
| 10 | — | — | E | — | R:encS*·btn* | E-std | ENC |
| 11 | — | — | E | P | R:encS*·btn*·tpX | E-std | ENC |
| 12 | — | — | E | E | R:encS*·btn*·encX*·btn* | E-std, E-ext | ENC |

### Group 2 — left = —/P (rows 13–24) · L adds: `tpXspl0*`; R adds `rcv0` · base deps {X-split}

| # | Lstd | Lext | Rstd | Rext | Snippets | Missing deps | Tier |
|---|---|---|---|---|---|---|---|
| 13 | — | P | — | — | L:tpXspl0* · R:rcv0 | X-split | PEXT |
| 14 | — | P | — | P | L:tpXspl0* · R:rcv0·tpX | X-split | PEXT |
| 15 | — | P | — | E | L:tpXspl0* · R:rcv0·encX*·btn* | X-split, E-ext | PEXT+ENC |
| 16 | — | P | B | — | L:tpXspl0* · R:rcv0·tb·lst | X-split | PEXT |
| 17 | — | P | B | P | L:tpXspl0* · R:rcv0·tb·lst·tpX | X-split | PEXT |
| 18 | — | P | B | E | L:tpXspl0* · R:rcv0·tb·lst·encX*·btn* | X-split, E-ext | PEXT+ENC |
| 19 | — | P | P | — | L:tpXspl0* · R:rcv0·tpS·lst | X-split | PEXT |
| 20 | — | P | P | P | L:tpXspl0* · R:rcv0·tpS·lst·tpX | X-split | PEXT |
| 21 | — | P | P | E | L:tpXspl0* · R:rcv0·tpS·lst·encX*·btn* | X-split, E-ext | PEXT+ENC |
| 22 | — | P | E | — | L:tpXspl0* · R:rcv0·encS*·btn* | X-split, E-std | PEXT+ENC |
| 23 | — | P | E | P | L:tpXspl0* · R:rcv0·encS*·btn*·tpX | X-split, E-std | PEXT+ENC |
| 24 | — | P | E | E | L:tpXspl0* · R:rcv0·encS*·btn*·encX*·btn* | X-split, E-std, E-ext | PEXT+ENC |

### Group 3 — left = —/E (rows 25–36) · L adds: `encX*·btn*` · base deps {E-ext}

| # | Lstd | Lext | Rstd | Rext | Snippets | Missing deps | Tier |
|---|---|---|---|---|---|---|---|
| 25 | — | E | — | — | L:encX*·btn* | E-ext | ENC |
| 26 | — | E | — | P | L:encX*·btn* · R:tpX | E-ext | ENC |
| 27 | — | E | — | E | L:encX*·btn* · R:encX*·btn* | E-ext | ENC |
| 28 | — | E | B | — | L:encX*·btn* · R:tb·lst | E-ext | ENC |
| 29 | — | E | B | P | L:encX*·btn* · R:tb·lst·tpX | E-ext | ENC |
| 30 | — | E | B | E | L:encX*·btn* · R:tb·lst·encX*·btn* | E-ext | ENC |
| 31 | — | E | P | — | L:encX*·btn* · R:tpS·lst | E-ext | ENC |
| 32 | — | E | P | P | L:encX*·btn* · R:tpS·lst·tpX | E-ext | ENC |
| 33 | — | E | P | E | L:encX*·btn* · R:tpS·lst·encX*·btn* | E-ext | ENC |
| 34 | — | E | E | — | L:encX*·btn* · R:encS*·btn* | E-ext, E-std | ENC |
| 35 | — | E | E | P | L:encX*·btn* · R:encS*·btn*·tpX | E-ext, E-std | ENC |
| 36 | — | E | E | E | L:encX*·btn* · R:encS*·btn*·encX*·btn* | E-ext, E-std | ENC |

### Group 4 — left = B/— (rows 37–48) · L adds: `tb·spl0`; R adds `rcv0` · base deps {}

| # | Lstd | Lext | Rstd | Rext | Snippets | Missing deps | Tier |
|---|---|---|---|---|---|---|---|
| 37 | B | — | — | — | L:tb·spl0 · R:rcv0 | — | BN |
| 38 | B | — | — | P | L:tb·spl0 · R:rcv0·tpX | — | BN |
| 39 | B | — | — | E | L:tb·spl0 · R:rcv0·encX*·btn* | E-ext | ENC |
| **40** | **B** | **—** | **B** | **—** | L:tb·spl0 · R:rcv0·tb·lst | — | **BN ★ pure double-trackball** |
| 41 | B | — | B | P | L:tb·spl0 · R:rcv0·tb·lst·tpX | — | BN |
| 42 | B | — | B | E | L:tb·spl0 · R:rcv0·tb·lst·encX*·btn* | E-ext | ENC |
| 43 | B | — | P | — | L:tb·spl0 · R:rcv0·tpS·lst | — | BN |
| 44 | B | — | P | P | L:tb·spl0 · R:rcv0·tpS·lst·tpX | — | BN |
| 45 | B | — | P | E | L:tb·spl0 · R:rcv0·tpS·lst·encX*·btn* | E-ext | ENC |
| 46 | B | — | E | — | L:tb·spl0 · R:rcv0·encS*·btn* | E-std | ENC |
| 47 | B | — | E | P | L:tb·spl0 · R:rcv0·encS*·btn*·tpX | E-std | ENC |
| 48 | B | — | E | E | L:tb·spl0 · R:rcv0·encS*·btn*·encX*·btn* | E-std, E-ext | ENC |

### Group 5 — left = B/P (rows 49–60) · L adds: `tb·spl0·tpXspl1*`; R adds `rcv0·rcv1*` · base deps {X-split, reg1}

| # | Lstd | Lext | Rstd | Rext | Snippets | Missing deps | Tier |
|---|---|---|---|---|---|---|---|
| 49 | B | P | — | — | L:tb·spl0·tpXspl1* · R:rcv0·rcv1* | X-split, reg1 | PEXT+REG1 |
| 50 | B | P | — | P | + R:tpX | X-split, reg1 | PEXT+REG1 |
| 51 | B | P | — | E | + R:encX*·btn* | X-split, reg1, E-ext | PEXT+REG1+ENC |
| 52 | B | P | B | — | + R:tb·lst | X-split, reg1 | PEXT+REG1 |
| 53 | B | P | B | P | + R:tb·lst·tpX | X-split, reg1 | PEXT+REG1 |
| 54 | B | P | B | E | + R:tb·lst·encX*·btn* | X-split, reg1, E-ext | PEXT+REG1+ENC |
| 55 | B | P | P | — | + R:tpS·lst | X-split, reg1 | PEXT+REG1 |
| 56 | B | P | P | P | + R:tpS·lst·tpX | X-split, reg1 | PEXT+REG1 |
| 57 | B | P | P | E | + R:tpS·lst·encX*·btn* | X-split, reg1, E-ext | PEXT+REG1+ENC |
| 58 | B | P | E | — | + R:encS*·btn* | X-split, reg1, E-std | PEXT+REG1+ENC |
| 59 | B | P | E | P | + R:encS*·btn*·tpX | X-split, reg1, E-std | PEXT+REG1+ENC |
| 60 | B | P | E | E | + R:encS*·btn*·encX*·btn* | X-split, reg1, E-std, E-ext | PEXT+REG1+ENC |

("+ R:…" = row-49 snippets plus the listed right fragments.)

### Group 6 — left = B/E (rows 61–72) · L adds: `tb·spl0·encX*·btn*`; R adds `rcv0` · base deps {E-ext}

| # | Lstd | Lext | Rstd | Rext | Snippets | Missing deps | Tier |
|---|---|---|---|---|---|---|---|
| 61 | B | E | — | — | L:tb·spl0·encX*·btn* · R:rcv0 | E-ext | ENC |
| 62 | B | E | — | P | + R:tpX | E-ext | ENC |
| 63 | B | E | — | E | + R:encX*·btn* | E-ext | ENC |
| 64 | B | E | B | — | + R:tb·lst | E-ext | ENC |
| 65 | B | E | B | P | + R:tb·lst·tpX | E-ext | ENC |
| 66 | B | E | B | E | + R:tb·lst·encX*·btn* | E-ext | ENC |
| 67 | B | E | P | — | + R:tpS·lst | E-ext | ENC |
| 68 | B | E | P | P | + R:tpS·lst·tpX | E-ext | ENC |
| 69 | B | E | P | E | + R:tpS·lst·encX*·btn* | E-ext | ENC |
| 70 | B | E | E | — | + R:encS*·btn* | E-ext, E-std | ENC |
| 71 | B | E | E | P | + R:encS*·btn*·tpX | E-ext, E-std | ENC |
| 72 | B | E | E | E | + R:encS*·btn*·encX*·btn* | E-ext, E-std | ENC |

### Group 7 — left = P/— (rows 73–84) · L adds: `tpS·spl0`; R adds `rcv0` · base deps {}

Structurally identical to Group 4 with `tpS` in place of `tb` on the left:

| # | Lstd | Lext | Rstd | Rext | Snippets | Missing deps | Tier |
|---|---|---|---|---|---|---|---|
| 73 | P | — | — | — | L:tpS·spl0 · R:rcv0 | — | BN |
| 74 | P | — | — | P | + R:tpX | — | BN |
| 75 | P | — | — | E | + R:encX*·btn* | E-ext | ENC |
| 76 | P | — | B | — | + R:tb·lst | — | BN |
| 77 | P | — | B | P | + R:tb·lst·tpX | — | BN |
| 78 | P | — | B | E | + R:tb·lst·encX*·btn* | E-ext | ENC |
| 79 | P | — | P | — | + R:tpS·lst | — | BN ★ double-pad, both standard |
| 80 | P | — | P | P | + R:tpS·lst·tpX | — | BN |
| 81 | P | — | P | E | + R:tpS·lst·encX*·btn* | E-ext | ENC |
| 82 | P | — | E | — | + R:encS*·btn* | E-std | ENC |
| 83 | P | — | E | P | + R:encS*·btn*·tpX | E-std | ENC |
| 84 | P | — | E | E | + R:encS*·btn*·encX*·btn* | E-std, E-ext | ENC |

### Group 8 — left = P/P (rows 85–96) · L adds: `tpS·spl0·tpXspl1*`; R adds `rcv0·rcv1*` · base deps {X-split, reg1}

Structurally identical to Group 5 with `tpS` in place of `tb` on the left. Rows 85–96 map 1:1 onto rows 49–60: same right fragments, same missing deps, same tiers (`PEXT+REG1`, plus `ENC` when the right side has any encoder, i.e. rows 87, 90, 93, 94, 95, 96).

### Group 9 — left = P/E (rows 97–108) · L adds: `tpS·spl0·encX*·btn*`; R adds `rcv0` · base deps {E-ext}

Structurally identical to Group 6 with `tpS` in place of `tb` on the left. Rows 97–108 map 1:1 onto rows 61–72: all tier `ENC`; deps `E-ext` plus `E-std` when rightStd = E (rows 106–108).

### Group 10 — left = E/— (rows 109–120) · L adds: `encS*·btn*` · base deps {E-std}

| # | Lstd | Lext | Rstd | Rext | Snippets | Missing deps | Tier |
|---|---|---|---|---|---|---|---|
| 109 | E | — | — | — | L:encS*·btn* | E-std | ENC |
| 110 | E | — | — | P | + R:tpX | E-std | ENC |
| 111 | E | — | — | E | + R:encX*·btn* | E-std, E-ext | ENC |
| 112 | E | — | B | — | + R:tb·lst | E-std | ENC |
| 113 | E | — | B | P | + R:tb·lst·tpX | E-std | ENC |
| 114 | E | — | B | E | + R:tb·lst·encX*·btn* | E-std, E-ext | ENC |
| 115 | E | — | P | — | + R:tpS·lst | E-std | ENC |
| 116 | E | — | P | P | + R:tpS·lst·tpX | E-std | ENC |
| 117 | E | — | P | E | + R:tpS·lst·encX*·btn* | E-std, E-ext | ENC |
| 118 | E | — | E | — | + R:encS*·btn* | E-std | ENC |
| 119 | E | — | E | P | + R:encS*·btn*·tpX | E-std | ENC |
| 120 | E | — | E | E | + R:encS*·btn*·encX*·btn* | E-std, E-ext | ENC |

### Group 11 — left = E/P (rows 121–132) · L adds: `encS*·btn*·tpXspl0*`; R adds `rcv0` · base deps {E-std, X-split}

Note: the ext pad is the **only** peripheral pointing device (encoder consumes no reg slot) → it takes reg <0> and uses the EXISTING `rcv0`; **no reg1 needed**.

| # | Lstd | Lext | Rstd | Rext | Snippets | Missing deps | Tier |
|---|---|---|---|---|---|---|---|
| 121 | E | P | — | — | L:encS*·btn*·tpXspl0* · R:rcv0 | E-std, X-split | ENC+PEXT |
| 122 | E | P | — | P | + R:tpX | E-std, X-split | ENC+PEXT |
| 123 | E | P | — | E | + R:encX*·btn* | E-std, X-split, E-ext | ENC+PEXT |
| 124 | E | P | B | — | + R:tb·lst | E-std, X-split | ENC+PEXT |
| 125 | E | P | B | P | + R:tb·lst·tpX | E-std, X-split | ENC+PEXT |
| 126 | E | P | B | E | + R:tb·lst·encX*·btn* | E-std, X-split, E-ext | ENC+PEXT |
| 127 | E | P | P | — | + R:tpS·lst | E-std, X-split | ENC+PEXT |
| 128 | E | P | P | P | + R:tpS·lst·tpX | E-std, X-split | ENC+PEXT |
| 129 | E | P | P | E | + R:tpS·lst·encX*·btn* | E-std, X-split, E-ext | ENC+PEXT |
| 130 | E | P | E | — | + R:encS*·btn* | E-std, X-split | ENC+PEXT |
| 131 | E | P | E | P | + R:encS*·btn*·tpX | E-std, X-split | ENC+PEXT |
| 132 | E | P | E | E | + R:encS*·btn*·encX*·btn* | E-std, X-split, E-ext | ENC+PEXT |

### Group 12 — left = E/E (rows 133–144) · L adds: `encS*·btn*·encX*·btn*` (one composite kscan, both pins) · base deps {E-std, E-ext}

All 12 rows are tier `ENC`; deps = {E-std, E-ext} regardless of right side (right fragments per the right-contribution table). Rows 133–144 follow the standard right ordering (—/—, —/P, —/E, B/—, B/P, B/E, P/—, P/P, P/E, E/—, E/P, E/E).

### 4.x Tally (144 configs, central = right)

> **これは 2026-07 時点の集計です。** バックログ（§5）が全件完了した現在は
> **144/144 が BUILDABLE-NOW** で、下表の `ENC` / `PEXT` / `REG1` 行は「当時どの依存で
> 止まっていたか」の記録として読んでください。

| Tier | Count | Rows |
|---|---|---|
| BUILDABLE-NOW | 18 | Groups 1/4/7, rights without encoder |
| ENC only | 78 | Groups 3, 6, 9, 10, 12 fully + encoder-rights of groups 1/4/7 |
| PEXT only | 6 | Group 2, rights without encoder |
| ENC+PEXT | 18 | Group 2 encoder-rights (6) + Group 11 fully (12) |
| PEXT+REG1 | 12 | Groups 5/8, rights without encoder |
| ENC+PEXT+REG1 | 12 | Groups 5/8, encoder-rights |
| **Total** | **144** | |

---

## 5. Backlog — **すべて完了（144/144 BUILDABLE-NOW）**

> **進捗 (2026-08-29 時点): 4件すべて実装済み。未実装依存によるビルド不可構成はもうありません。**
> `index.html` の `MISSING_LABEL` は空オブジェクトで、ビルド不可バッジはどの構成でも出ません。
> **ただし、ほぼすべてが未実機検証（ビルド＆フラッシュでの確認が済んでいない）です。**

| Pri | Item | 成果物 | 状態 |
|---|---|---|---|
| 1a | 標準エンコーダ回転 | `input-encoder`（物理側）＋ `input-encoder-recv`（central 受け）。ZMK 公式テスト peripheral-encoder 準拠。`&layer_0` に静的既定 `sensor-bindings`（音量±）も注入 | ✅ 実装済（2026-07-11、未実機検証）|
| 1b | エンコーダ押しボタン | `torabo-encoder-btn-local` / `-local-ext` / `-split` / `-split-ext` ＋ central 受けの `-recv`。**`kscan-gpio-direct` ではなく input 経路**で実装したため、当初案にあった transform への1キー追加は**不要**になった | ✅ 実装済（未実機検証）|
| 1c | 拡張FFCエンコーダ | `input-encoder-ext` ＋ `input-encoder-ext-recv`（P0.17/21、ボタン P0.31）| ✅ 実装済（未実機検証）|
| 2 | 周辺拡張パッドの split エクスポート | `input-trackpad-ext-split`（reg=0）。central は既存 `input-split-listener` で受ける | ✅ 実装済（2026-07-12、未実機検証）|
| 3 | reg=<1> 版 | `input-trackpad-ext-split-reg1`（peripheral の2個目 pointing）＋ `input-split-listener-reg1`（central 受け）。2個目の pointing は必ず拡張パッドなので、汎用 `input-split` の reg=1 版は作っていない | ✅ 実装済（未実機検証）|

> ※ これらの snippet は当初 fork 側（`torabo-tsuki-config/snippets/`）に置いていたが、
> fork 側の更新対象を増やさないため **2026-08-15 に `torabo-tsuki_ext_FW/snippets/` へ移設**した。

Notes:

- 現在の未解決事項は **実装**ではなく **実機検証**です。特に reg=1 の二重ポインティング・
  拡張FFCエンコーダが未確認。拡張LEDのライブ設定は実機検証済み。
- Orthogonal, no backlog impact: extender FPC vs FPC+LED, central-side choice (pure left/right label swap)。
- **左 central 構成**はコード上の半身依存が無い一方、検証は右 central ビルドのみ。

### 5.1 常時出力される snippet（構成に依存しない）

ビルダーは物理構成にかかわらず、central 行に `torabo-caps` / `torabo-rpc-tunnel` /
`torabo-timing` を、peripheral 行に `torabo-timing-split` を必ず出力します。後ろ2つ
（トンネル・タイミング）は **zmk 本体を `tak-2025/zmk` の `dev` に差し替えること**が前提で、
`genWest()` はこれを検出して `config/west.yml` の zmk 置き換えを出力します。
