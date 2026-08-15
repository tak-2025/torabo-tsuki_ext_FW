# 設計 — torabo-tsuki ダイナミックマクロ（ライブ編集マクロ）

`zmk-feature-dynamic-macros`。**再フラッシュなしで内容を編集できるマクロ**を提供する。
トラックボール設定（`zmk-feature-trackball-config` / DESIGN_v2.md）と同じ
「NVS 一次 + カスタム GATT 窓 + アプリ編集 + 即適用・永続」モデルを踏襲する。

## 0. 目的・動機

- マクロを変えるたびの**再フラッシュを廃止**（内蔵フラッシュの消耗・手間を回避）。NVS は頻繁な小書き込み前提のウェアレベリング領域なのでこの用途に適切。
- **ツール統一**：日常のマクロ編集を Torabo-Studio 1本で完結（keymap-editor を日常運用から外す）。
- `&dmac N` 機能を**一度だけ焼けば**、以降スロット内容は無限にライブ編集可能。

## 1. スコープ

- **Phase 1（本体）**：`&dmac <slot>` という単一 behavior。スロット番号でひもづく**キーコード列**を再生する。
- **Phase 2（combo ライブ化）**：純正 combo エンジンを逐語コピーし、定義の読み元を NVS に差し替え（§11）。S サイズ移行でキーが減る場合に効く。
- **対象外**：任意 behavior のライブ化（パラメータ形がバラバラで費用対効果が悪い）、wait/タイミング細調整、マクロのパラメータ化。
- キー配置/レイヤーは既存 ZMK Studio がライブ編集するので本機能の対象外。
- モジュール名は macros + combos を包含するため **`zmk-feature-dynamic-keymap`**（旧 `-dynamic-macros`）に統一予定。

## 2. 設計契約（鉄則）

1. **フェイルセーフ**：未ロード・不正・空スロットは「**何もしない**」（暴発キー入力を絶対に出さない）。範囲外 slot/keycode は無視。
2. **検証は FW 側で完結**：GATT 書き込みは magic/version/長さ/範囲を検証し、**ダメなら一切変更しない**（部分適用なし）。
3. **NVS 一次・RAM ミラー**：再生は RAM の検証済みコピーから読む（NVS 直読みしない）。
4. **MTU 安全**（§4）：書き込みは常に単発で MTU 内。long-write は使わない。
5. **トラックボール実装を流用**：`config_state.c`（RAM/NVS・wire codec）と `gatt_service.c`（GATT 窓）の構造をコピーして使う。ゼロから作らない。

## 3. 全体アーキテクチャ（3層）

```
[アプリ Torabo-Studio]
  「マクロ」タブ: スロット編集 → READ(全件) / WRITE(1スロット)
        │  Tauri cmd: dmac_read_all / dmac_write_slot
        ▼  カスタム GATT (UUID は §4)
[FW gatt_service]  ── 検証 ──> [FW store (RAM mirror + NVS)]
        ▲                              │ ライブ反映
[FW behavior &dmac N] ──再生── 読み取り ┘
```

## 4. データモデル / ワイヤ protocol（UI と FW の唯一の正）

リトルエンディアン。**explicit byte offset**（パック構造体の未アライン参照をしない）。

### 4.1 定数

| 名前 | 値 | 意味 |
|---|---|---|
| `DM_MAGIC` | `0x6D64`（"dm"） | マジック |
| `DM_VERSION` | `1` | バージョン |
| `DM_SLOTS` (K) | `20` | スロット数 |
| `DM_STEPS` (L) | `16` | 1スロット最大ステップ |

### 4.2 ステップ（5バイト）

```
action : u8     // 0=TAP, 1=PRESS, 2=RELEASE
keycode: u32    // ZMK のフル usage（page<<16|id、上位8bitに修飾子。LC(C) 等を内包可）
```

### 4.3 READ（全件・Read Blob 可。FW→アプリ）

```
hdr(4): magic u16, version u8, slot_count u8
then K slots, each (1 + L*5 = 81 bytes):
    used_len u8                 // 有効ステップ数 (0..L)
    steps[L] : { action u8, keycode u32 }
```
総サイズ = 4 + 20*81 = **1624 バイト**。Read Blob（オフセット読み）で取得。

### 4.4 WRITE（1スロットずつ・単発書き込み。アプリ→FW）

```
version u8          // == DM_VERSION
slot    u8          // 0..K-1
used_len u8         // 0..L
steps[used_len] : { action u8, keycode u32 }
```
最大 = 3 + 16*5 = **83 バイト**（MTU 247 内で単発）。FW は検証 → 当該スロットのみ更新 → NVS 保存。

## 5. FW 設計

### 5.1 behavior `&dmac`（`src/dmac_behavior.c`）
- DT バインディング `zmk,behavior-dynamic-macro`、`#binding-cells = <1>`（param=slot）。
- `on_keymap_binding_pressed`：slot 範囲チェック → RAM の検証済みスロットを取得 → **ステップ列をキューに積んで非同期再生**。
- 再生エンジンは **ZMK の behavior queue（`zmk_behavior_queue_add`）を流用**し、各ステップを `&kp`（press/release/tap）として積む（behavior_macro.c の手口を踏襲）。固定 tap/inter-step 間隔。
- フェイルオープン：範囲外・空は何もせず `ZMK_BEHAVIOR_OPAQUE`。

### 5.2 ストア（`src/config_state.c` 流用改修）
- `struct dm_store { struct dm_slot slots[K]; }`、`dm_slot{ uint8_t len; struct dm_step steps[L]; }`。
- **ダブルバッファ + アトミック publish**（再生スレッドが半適用を観測しない）。
- 静的初期値＝**全スロット len=0（空＝無動作）**。フェイルセーフ。
- `dm_apply_write_wire(buf,len)`：§4.4 検証 → shadow 構築 → publish → `dm_save()`（NVS）。
- `dm_encode_read_wire(buf,cap,&len)`：§4.3 を生成。

### 5.3 GATT 窓（`src/gatt_service.c` 流用）
- service/char UUID は新規（トラックボールと別）。`READ=dm_encode_read_wire`、`WRITE=dm_apply_write_wire`。
- READ は `bt_gatt_attr_read`（Read Blob 自動対応）。WRITE は単発前提（§4.4 が MTU 内）。offset!=0 は拒否でよい。

### 5.4 devicetree / Kconfig
- `dts/bindings/behaviors/zmk,behavior-dynamic-macro.yaml`。
- Kconfig：`ZMK_DYNAMIC_MACROS`（と `_BLE`）。`DT_HAS_*` で behavior 自動有効化（トラックボールの流儀）。
- keymap で behavior ノードを 1 個インスタンス化し、`&dmac 0..K-1` を使う。

## 6. アプリ / UI（Torabo-Studio）

- **「マクロ」タブ**新設：
  - 開いた時に **READ(全件)** → 20スロット表示。
  - 各スロット：ステップ列を編集（追加/削除/並べ替え、各ステップ = action 選択 + キーコード（既存 HID ピッカー流用））。
  - 「保存」= 当該スロットを **WRITE(1スロット)** → 即適用・NVS。
- **Studio 連携**：`&dmac N` はレンジ metadata 付き behavior なので、キーへの割当は既存 Studio UI でライブに可能（D6）。
- Rust: `dmac_read_all` / `dmac_write_slot`（`trackball.rs` を複製）。TS codec: `dmacConfig.ts`（§4 に一致）。

## 7. 取り込み / バックアップ（整合の橋）

- **取り込み**：keymap.keymap をパースし、`Mx` マクロの `&kp` 列 → スロットへロード（既存 M0〜M7 移行用）。`&kp/&mt` 以外は取り込み不可（スキップ＋警告）。
- **バックアップ**：マクロ内容を既存 backup JSON に追加（消失対策・可搬性）。`settings_reset` や設定非互換時はここから復元。
- 注意：**マクロ内容は NVS が一次**で、keymap.keymap（keymap-editor の管轄）には出ない。`&dmac N` は不透明参照として残る。

## 8. フェイルセーフ・チェックリスト（フラッシュ前）

- [ ] 空スロット再生 = 無動作
- [ ] slot/keycode 範囲外 = 無視
- [ ] 不正 wire（magic/version/len）= 一切変更しない
- [ ] 再生中の WRITE で半適用が観測されない（ダブルバッファ）
- [ ] WRITE 最大長が MTU 内（単発）

## 9. フェーズ

1. **FW**：store + GATT + behavior（空スロットで暴発しないこと最優先）。一度フラッシュ。
2. **アプリ**：マクロタブ + read/write。ライブ編集確認。
3. **取り込み**：keymap.keymap → スロット。
4. **バックアップ**統合。

## 10. 決定事項（確定）

K=20 / L=16 / step={action,keycode} / NVS 一次 + 取り込み + backup / 新規モジュール / `&dmac N` ライブ割当可。
（L は定数。長尺マクロが要れば増やして再ビルド。WRITE が MTU 内に収まる範囲で L=32 まで可）

## 11. combo ライブ化（Phase 2）

S サイズ移行（キー減）に備え、combo を再フラッシュなしで編集可能にする。**マクロの NVS/GATT 土台を流用**し、GATT 特性を 1 本足す。

### 11.1 鉄則（最優先）
- **純正 `combo.c` の判定ロジックを逐語コピー**し、変更は「定義配列の読み元を const→RAM(NVS) にする」点のみ。タイミング・重なり・タイムアウト判定は一切いじらない＝通常タイピングの安全を保つ。
- **単一所有**：純正 combo は無効化（`CONFIG_ZMK_COMBO` 系を切る）し、本実装が唯一の所有者になる。両方を同時に走らせない。
- フェイルセーフ：不正/空スロットは「コンボ無し」として扱う（キーを食わない）。

### 11.2 データモデル（1 combo スロット）
- `enabled u8`
- `position_count u8`、`positions u8[P]`（P=最大キー位置数, 既定 6）
- `layer_mask u32`（0=全レイヤー）
- `timeout_ms u16`
- `target { type u8, param1 u32, param2 u32 }`
  - `type` ∈ `KP / MO / TO / TOG / DMAC`（小さな enum）。FW が enum→behavior device に変換し `zmk_behavior_invoke_binding` で発火。任意 behavior は非対応（割り切り）。
- オプション（slow-release / require-prior-idle 等）は v2 で。

### 11.3 転送（マクロと同方式）
- READ＝全 combo スロット一括（Read Blob）／WRITE＝1 combo スロットずつ（単発・MTU 内）。
- combo 用に別 UUID の characteristic を 1 本追加（マクロ用とは別キャラ、同一 service でも可）。

### 11.4 スロット数
- M（combo 数）＝**16** を仮置き（S サイズの想定で調整）。

### 11.5 アプリ UI
- 「コンボ」タブ：物理レイアウト上でキー位置を選択 → ターゲット(type+param) と timeout/layer を指定。READ 全件 / WRITE 1件。

### 11.6 実装リスク
- 純正 combo サブシステムの**置き換え**（無効化＋自前リスナの単一所有）が最大の注意点。逐語コピー＋フェイルセーフ回帰ゲートで担保。

## 12. フェーズ（全体）

1. **Phase 1**：マクロ（§5-9）。土台確立。一度フラッシュ。
2. **Phase 2**：combo（§11）。土台流用＋エンジン置換。再度フラッシュ。
3. 各 Phase 後にフェイルセーフ回帰ゲート（§8）。
