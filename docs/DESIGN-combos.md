# 設計 — torabo-tsuki ダイナミックコンボ（ライブ編集 combo）

`zmk-feature-dynamic-keymap` の一機能として **再フラッシュなしで combo を編集できる**仕組みを
提供する。マクロ（DESIGN-macros.md）／トラックボール（DESIGN-trackball.md）と同じ
「**NVS 一次 + カスタム GATT 窓 + アプリ編集 + 即適用・永続**」モデルを踏襲する。

本ドキュメントは **実装前の技術調査（フィージビリティ）＋設計**。実装着手はこのドキュメントの
レビュー後。実機事故（router FW がトラックボールを殺した v1 事故）の教訓から、**コンパイル
green ≠ 正しさ**として、フェイルセーフ回帰ゲート（§9）を通過するまで実機フラッシュ禁止。

---

## 0. フィージビリティ結論（先に結論）

**実現可能。難易度＝中。** 純正 combo エンジンを逐語コピーし、「定義配列の読み元を
`const`（DT）→ RAM（NVS）」に差し替える方針が成立する根拠を、ZMK 本体
`.zmk-workspace/zmk/app/src/combo.c` の精読で確認した。

### 根拠（combo.c の構造）
- combo 判定エンジンは **完全に配列駆動**。実行時に devicetree を参照せず、
  `static const struct combo_cfg combos[]`（combo.c:100）と、init で 1 回構築する
  `combo_lookup[position][bitmask]`（combo.c:118, 140-148, 530-532）だけを読む。
  → この 2 つを **RAM 化＋再構築可能**にすれば、ロジック本体は無改変でランタイム編集できる。
- 1 combo の定義 `struct combo_cfg`（combo.c:48-58）は素直な POD：
  `key_positions[]` / `key_position_len` / `timeout_ms` / `require_prior_idle_ms` /
  `layer_mask` / `slow_release` / `behavior`。すべて wire 化できる固定長フィールド。
- 発火は `struct zmk_behavior_binding { const char *behavior_dev; u32 param1; u32 param2; }`
  （`app/include/zmk/behavior.h:16`）を `zmk_behavior_invoke_binding()` に渡すだけ
  （combo.c:281-307）。→ **enum→behavior デバイス名文字列**に変換すれば実行時に組み立て可能。
- 純正 combo.c は `#if DT_HAS_COMPAT_STATUS_OKAY(zmk_combos)`（combo.c:28）でガードされる。
  keymap に `compatible="zmk,combos"` ノードを**置かなければ丸ごとコンパイルされない**。
  → ZMK 本体を 1 行も触らずに、自前実装を**単一所有者**にできる。

### 解決すべき 3 つの技術論点（いずれも解決策あり）
| 論点 | 純正の前提 | 本実装の解 |
|---|---|---|
| **配列が `const`/DT 固定長** | `MAX_COMBO_KEYS`・`BYTES_FOR_COMBOS_MASK` を DT 子ノードから算出（combo.c:46,108） | 自前で**固定上限**を `#define`（例 P=6, M=16）。`combos[]` を可変 RAM 配列に。変更時 `combo_lookup` を作り直す |
| **behavior が DT phandle** | `ZMK_KEYMAP_EXTRACT_BINDING`（combo.c:85） | 小 enum（KP/MO/TO/TOG/DMAC）→ 各 behavior の `DEVICE_DT_NAME(DT_NODELABEL(...))` 文字列に変換して `binding.behavior_dev` に設定。任意 behavior は非対応（割り切り） |
| **編集とエンジンの競合** | 単一スレッド（DT 固定なので競合なし） | **アイドル時のみ atomic publish**（§5.2）。combo 進行中（`pressed_keys_count>0` または `active_combo_count>0`）は適用を遅延 |

### 残リスク検証結果（2026-06-25 ZMK ソース精読で確定）

**3 件とも解決。設計を阻むブロッカーは無し。** 検証の根拠と確定した解：

1. **virtual key position の名前空間 — 解決（自前予約レンジ）。**
   - 確認：`ZMK_VIRTUAL_KEY_POSITION_COMBO(idx) = ZMK_KEYMAP_LEN + ZMK_KEYMAP_SENSORS_LEN + idx`
     （`virtual_key_position.h:27`）。純正無効化で `ZMK_COMBOS_LEN=0`（`combos.h:13-16`）になると、
     **input-processor 用の虚配置が `…COMBO(0)` 起点に降りてきて、純正 combo レンジと重なる**
     （`virtual_key_position.h:31`）。torabo は input-processor（トラックボール）を使うので、
     素朴に `…COMBO(idx)` を使うと**衝突する**＝確かに実在するリスクだった。
   - 解：combo の虚配置は **発火 behavior の `event.position`（＝per-position 状態の不透明ハンドル）
     にしか使われない**。位置で添字される配列はすべて `[ZMK_KEYMAP_LEN]`（実キーのみ。combo.c:118,
     keymap.c:67/76）で、虚配置は添字に使われない。behavior queue も msgq で position 上限なし
     （`behavior_queue.c:26`）。よって **ZMK の虚配置空間より上に自前ベースを取る**
     （例 `DYN_COMBO_POS_BASE = ZMK_KEYMAP_LEN + 0x100`、`+ idx`）だけで衝突ゼロ。
     純正は既に `ZMK_KEYMAP_LEN` 超の位置で behavior を叩いており、より大きい値でも同じ扱いで安全。
     split でも combo は central ローカル発火（`SOURCE_LOCAL`）で位置は relay されない。

2. **`CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS` — 解決（依存なし・両対応）。**
   - 確認：`zmk_behavior_invoke_binding` は **`behavior_dev`（文字列）で解決**する
     （`behavior.c:73, 242` が `zmk_behavior_get_binding(binding->behavior_dev)`）。**local_id は発火に不要**。
   - 確認：ZMK Studio が `ZMK_BEHAVIOR_LOCAL_IDS` を select（`studio/Kconfig:43`）、choice 既定の
     SETTINGS_TABLE が `…IN_BINDINGS` を select（`Kconfig.behaviors:21-24`）。torabo は Studio 有効なので
     **本ビルドでは `local_id` フィールドが存在する公算大**。
   - 解：binding 構築時に `behavior_dev` を必ず設定。`#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)`
     のときだけ `binding.local_id = zmk_behavior_get_local_id(name)`（`behavior.h:69`）も埋める。
     ON/OFF どちらでも壊れない（発火自体は `behavior_dev` のみで成立）。

3. **進行中スワップの安全性 — 解決（リスナ文脈 swap + GATT 境界だけ spinlock）。**
   - 確認：`event_manager` はリスナを **同期呼び出し**（`event_manager.c:28-29` が
     `listener->callback(event)` を直接ループ実行。**workqueue 経由なし**）。**stock combo.c は
     ロックを一切持たない**＝ZMK は position イベントを**直列化**前提でエンジンを実質シングルスレッドとして扱う。
   - 解：**swap をリスナ文脈（エンジンと同一直列）で行えば、`candidates`/`active_combos`/`combos[]`/
     `combo_lookup` のエンジン読み取りと swap は競合しない（追加ロック不要）**。クロススレッド共有は
     「GATT 書込スレッド → pending バッファ」の 1 点だけなので、そこだけ短い `k_spinlock`。
     GATT write = 検証して pending へコピー＋フラグ。リスナはアイドル（`pressed_keys_count==0 &&
     active_combo_count==0`、＝最初のキー押下処理の冒頭）で pending→live を反映し `rebuild_lookup()`。

### その他の確定事項

- **split 動作位置**：combo は **central（=右・トラックボール側）で処理**。マクロ／トラックボールと
  同じ central snippet に同居。peripheral 側は触らない。

---

## 1. スコープ
- **対象**：キー位置の同時押し → 1 behavior 発火、という純正 combo 相当を**ライブ編集**化。
  per-combo の timeout / require-prior-idle / layer-mask / slow-release を含む。
- **発火ターゲット**：`KP`（キーコード）/ `MO`・`TO`・`TOG`（レイヤー）/ `DMAC`（マクロ slot）の
  小 enum に限定。任意 behavior・behavior 列・パラメータ化は**対象外**（割り切り）。
- **対象外**：combo の入れ子・combo→combo、タイミング詳細チューニング UI、peripheral 側 combo。
- キー配置／レイヤー定義は既存 ZMK Studio がライブ編集するので本機能の対象外。
- 純正 combo（DT `zmk,combos`）は**使わない**（単一所有・§5.5）。

## 2. 設計契約（鉄則）
1. **フェイルセーフ**：未ロード・不正・空・無効 combo は「**combo 無し**」として扱う
   （キーイベントを食わない＝通常タイピングを壊さない）。範囲外 position/keycode は無視。
2. **検証は FW 側で完結**：GATT 書き込みは magic/version/長さ/範囲を検証し、ダメなら一切変更しない（部分適用なし）。
3. **NVS 一次・RAM ミラー**：エンジンは RAM の検証済みコピーだけを読む（NVS 直読みしない）。
4. **アイドル publish**：combo 進行中は新設定を適用しない（半適用を観測させない）。
5. **単一所有**：純正 combo を無効化し、本実装が唯一の position リスナ combo として走る。両立させない。
6. **逐語コピー**：combo.c の判定・タイミング・重なり・タイムアウト判定は**一切いじらない**。
   変更は「配列の読み元」「固定上限」「behavior 解決」「アイドルスワップ」の 4 点に限定。
7. **マクロ実装を流用**：`config_state.c`（RAM/NVS・wire codec）と `gatt_service.c`（GATT 窓）の
   構造をコピー。ゼロから作らない。

## 3. 全体アーキテクチャ（3層）
```
[アプリ Torabo-Studio]
  「コンボ」タブ: 物理レイアウトでキー位置選択 → ターゲット/timeout/layer 指定
        │  READ(全件) / WRITE(1コンボ)
        │  Tauri cmd: combo_read_all / combo_write_slot
        ▼  カスタム GATT characteristic（§4 UUID）
[FW gatt_service] ── 検証 ──> [FW store: RAM mirror(ダブルバッファ) + NVS]
        ▲                              │ アイドル時 publish → combo_lookup 再構築
[FW dyn_combo.c (combo.c 逐語コピー)] ─ 読み取り ┘
        └ position_state_changed を購読し combo 判定・発火（central のみ）
```

## 4. データモデル / ワイヤ protocol（UI と FW の唯一の正）
リトルエンディアン。explicit byte offset（パック構造体の未アライン参照をしない）。
マクロ（`dmacConfig.ts`）と同じ READ=全件 Blob / WRITE=1件単発の方式。

### 4.1 定数
| 名前 | 値（仮） | 意味 |
|---|---|---|
| `CB_MAGIC` | `0x6263`（"cb"） | マジック |
| `CB_VERSION` | `1` | バージョン |
| `CB_SLOTS` (M) | `16` | combo 数（S サイズ想定で調整可） |
| `CB_MAX_POS` (P) | `6` | 1 combo 最大キー位置数 |

### 4.2 1 combo スロット（固定長）
```
enabled        u8        // 0=無効（combo 無しとして無視）
position_count u8        // 0..P
positions      u8[P]     // キー位置（未使用は 0、count で判定）
layer_mask     u32       // 0=全レイヤー、それ以外は BIT(layer) の OR
timeout_ms     u16
require_prior_idle_ms u16
flags          u8        // bit0=slow_release（残りは予約=0）
target_type    u8        // enum: 0=KP 1=MO 2=TO 3=TOG 4=DMAC
target_param1  u32       // KP=keycode(page<<16|id, 上位8bit修飾子) / MO,TO,TOG=layer / DMAC=slot
target_param2  u32       // 予約（0）
```
1 スロット = 1+1+P+4+2+2+1+1+4+4 = **P=6 で 26 バイト**。

### 4.3 READ（全件・Read Blob。FW→アプリ）
```
hdr(4): magic u16, version u8, slot_count u8
then M slots, each 26 bytes（§4.2）
```
総サイズ = 4 + 16*26 = **420 バイト**。Read Blob（オフセット読み）で取得。

### 4.4 WRITE（1 combo ずつ・単発。アプリ→FW）
```
version u8       // == CB_VERSION
slot    u8       // 0..M-1
<§4.2 の 1 スロット 26 バイト>
```
最大 = 2 + 26 = **28 バイト**（MTU 247 内で単発・long-write 不使用）。
FW は検証 → 当該スロットのみ shadow 更新 → アイドル時 publish → NVS 保存。

## 5. FW 設計

### 5.1 エンジン `dyn_combo.c`（combo.c の逐語コピー＋4点改修）
- ファイルは combo.c をコピーし以下だけ変更：
  1. `ZMK_LISTENER(combo, …)` → `ZMK_LISTENER(dyn_combo, …)`（名前衝突回避。純正は無効化済だが念のため）。
  2. `MAX_COMBO_KEYS` を DT 算出から `#define CB_MAX_POS 6` の固定値へ。
     `BYTES_FOR_COMBOS_MASK` を `DIV_ROUND_UP(CB_SLOTS, 32)` 固定へ。
  3. `static const struct combo_cfg combos[]` → `static struct combo_cfg combos[CB_SLOTS]`（RAM・可変）。
     `ARRAY_SIZE(combos)` 参照は `active_combo_n`（有効数）に置換、または全 M 走査＋`enabled` で弾く。
  4. **`combo_lookup` 再構築関数** `rebuild_lookup()` を追加（combo.c:140-148, 530-532 の init ループを関数化）。
- それ以外（candidate 探索 / filter / timeout / activate / release / 重なり処理）は**無改変**。
- フェイルオープン：`enabled=0` や `position_count=0` は lookup に載せない＝候補にならない（キーを食わない）。

### 5.2 ストア `combo_state.c`（macros の `config_state.c` 流用）
- `struct cb_store { struct combo_cfg slots[CB_SLOTS]; }`、**ダブルバッファ + atomic publish**。
- 静的初期値＝**全スロット enabled=0**（フェイルセーフ）。
- `cb_apply_write_wire(buf,len)`：§4.4 検証 → shadow に該当 slot を構築 → **pending フラグ**を立てる。
- **アイドルゲート**：`pressed_keys_count==0 && active_combo_count==0` を満たす瞬間
  （リスナ処理の末尾＝event 文脈）に `publish()`：shadow を live にスワップ → `rebuild_lookup()`。
  進行中なら publish せず pending を保持し、次のアイドルで適用。
- enum→binding 変換：`target_type` を `behavior_dev` 文字列へ。
  - KP → `DEVICE_DT_NAME(DT_NODELABEL(key_press))`, param1=keycode
  - MO → `…(mo)` / TO → `…(to)` / TOG → `…(tog)`, param1=layer
  - DMAC → `…(dmac)`（torabo-macros が定義）, param1=slot
  - 該当 nodelabel が build に存在しない場合はその combo を **enabled=0 扱い**（フェイルセーフ）。
  - `CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS` 有効時は `binding.local_id` を解決して埋める（要確認・§0残リスク2）。
- `cb_encode_read_wire(buf,cap,&len)`：§4.3 を生成。NVS は Zephyr settings（key 例 `zdk/cb`）。

### 5.3 GATT 窓（macros の `gatt_service.c` 流用）
- combo 用 characteristic を 1 本追加。マクロ用と**別 UUID**（同一 service でも別 characteristic でも可）。
- `READ=cb_encode_read_wire`（Read Blob 自動対応 / `bt_gatt_attr_read`）、`WRITE=cb_apply_write_wire`（単発・offset!=0 拒否）。
- 暗号化必須（既存 trackball/macros と同じ要件。Windows は事前ペアリング必須）。

### 5.4 devicetree / Kconfig / snippet
- 新 Kconfig `CONFIG_ZMK_DYNAMIC_COMBOS`（と `_BLE`）。`default n`。
- 純正と衝突しない自前 compatible（例 `zmk,dynamic-combos`）の**プレースホルダノード 1 個**を snippet が供給
  （virtual key position の予約レンジ確保＋エンジン有効化のため。§0残リスク1 の解決手段）。
- 新 snippet `torabo-combos`（README の torabo-macros/torabo-trackball と同列）。central エントリにだけ足す。
  BLE MTU 拡張は trackball snippet と共通化。
- keymap 編集は**不要**（combo はキーに割り当てる behavior ではなく、位置同時押しで発火するため）。

### 5.5 純正 combo の無効化（単一所有）
- 親 keymap に `combos { compatible="zmk,combos"; … }` を**置かない**ことで純正 combo.c は
  `DT_HAS_COMPAT_STATUS_OKAY` が false になりコンパイルされない（combo.c:28）。
- 既存 torabo-tsuki が純正 combo を使っている場合は、その定義を本機能へ移行してから無効化する
  （移行は §7 取り込みで支援）。**両方同時に有効化しない**。

### 5.6 🚨 リスナ実行順序（実機検証 #1・実装レビューで判明）
**最重要の未確定リスク。** combo がキーを横取り（capture）して個別キー入力を抑止できるのは、
position_state_changed リスナ列で **combo が keymap より前に走るとき**だけ。順序が逆だと
keymap が position を即 behavior 発火（キー押下）して消費するため、**combo キーを押すと
コンボ動作＋個別キーが二重に出る**。
- 機構：`zmk-events.ld` は `KEEP(*(".event_subscription"))` で **SORT 無し＝リンク順**
  （`event_manager.c:23-29` が単純にセクション順で callback を回す）。純正は ZMK app 内で
  combo.c が keymap.c より前にソース列挙され先行（`app/CMakeLists.txt:69 vs 76`）。
- 本実装は **別ライブラリ（torabo モジュール）**。Zephyr はモジュールの CMakeLists を app
  本体より先に処理するのが通例なので**モジュールのオブジェクトが先にリンク＝ dyn_combo が
  keymap より前に来る公算が高い**（＝正しく動く想定）。だが SORT が無い以上**保証ではない**。
- **検証**：ビルドした `zephyr.map` で
  `zmk_event_sub_dyn_combozmk_position_state_changed` の配置アドレスが
  `zmk_event_sub_keymapzmk_position_state_changed` より**小さい（前）こと**を確認。
  実機では「コンボ用キーを単押し→個別キーが出る／コンボ発火時に余分なキーが出ない」を確認。
- **もし逆だった場合の対処**：(a) 本モジュールを west マニフェストでより先に並べてリンク順を前出しする、
  (b) 最終手段として engine TU を keymap.c より前にリンクされる場所（本体 config 側で combo.c を
  含めるより前の add_subdirectory）に置く。いずれも順序を前に出すだけで挙動は不変。

## 6. アプリ / UI（Torabo-Studio）
- **「コンボ」タブ**新設（`CombosPanel.tsx`、MacrosPanel.tsx を雛形に）：
  - 開いた時に **READ(全件)** → M スロット表示。
  - 各 combo：① 物理レイアウト上でキー位置を複数選択（既存 keyboard レイアウト描画を流用）、
    ② ターゲット（type 選択 + パラメータ：KP は HID ピッカー流用 / MO・TO・TOG はレイヤー番号 / DMAC は slot）、
    ③ timeout / require-prior-idle / layer-mask（レイヤー複数選択）/ slow-release、④ enabled トグル。
  - 「保存」= 当該 combo を **WRITE(1件)** → アイドル時即適用・NVS。
- Rust: `combo_read_all` / `combo_write_slot`（`trackball.rs` / `dmac` 系を複製、`tauri/combo.ts`）。
- TS codec: `comboConfig.ts`（§4 に一致。`dmacConfig.ts` と同じ explicit DataView 方式）。
- `MainPanels.tsx` にタブ追加。i18n（`i18n/messages.ts`）に文言追加。

## 7. 取り込み / バックアップ（整合の橋）
- **取り込み**：keymap.keymap の純正 `combos{ … }` をパースし、`key-positions` / `timeout-ms` /
  `layers` / `bindings`（`&kp`/`&mo`/`&to`/`&tog`/`&dmac` のみ）→ スロットへロード。
  非対応 binding はスキップ＋警告。これで純正→本機能の移行を支援。
- **バックアップ**：combo 内容を既存 backup JSON に追加（消失対策・可搬性）。
  `settings_reset` や非互換時はここから復元。
- 注意：combo 内容は **NVS が一次**で keymap.keymap には出ない（純正ノードを置かないため）。

## 8. フェイルセーフ・チェックリスト（フラッシュ前・回帰ゲート）
- [ ] **🚨 リスナ順序（§5.6）**：`zephyr.map` で dyn_combo の subscription が keymap より前。
      実機で「コンボ用キー単押し→個別キーが出るのに二重入力にならない／コンボ発火時に余分なキーが出ない」
- [ ] 全スロット enabled=0（初期状態）で **通常タイピングが一切変化しない**（候補に載らない）
- [ ] position_count=0 / 範囲外 position = lookup に載らず無視
- [ ] 不正 wire（magic/version/len/範囲）= 一切変更しない（部分適用なし）
- [ ] combo 進行中（押下中／active）に WRITE しても**半適用が観測されない**（アイドルゲート）
- [ ] WRITE 最大長が MTU 内（単発・28B）
- [ ] 未解決 behavior（nodelabel 不在）combo は enabled=0 扱いで**暴発しない**
- [ ] 純正 combo が**コンパイルされていない**こと（map/シンボルで確認・単一所有）
- [ ] split 再ペア後も central で combo が正しく発火（左右遅延・ずれが出ない）

### 実装状況（2026-06-25）
**FW・アプリとも実装完了。** FW = `features/combos/`（`combo_engine.c`＝combo.c 逐語コピー＋4改修 /
`combo_state.c` / `gatt_service.c` / Kconfig / CMakeLists / snippet `torabo-combos` /
binding `zmk,dynamic-combos.yaml`）。アプリ = `comboConfig.ts`（wire codec）/ `combo.rs`＋登録 /
`tauri/combo.ts` / `CombosPanel.tsx` / MainPanels タブ / i18n。
**検証**：アプリは `tsc --noEmit` green・`cargo check` green。FW は当環境に Docker/west 無く
**未コンパイル**——代わりに使用 ZMK API を全シンボル静的検証済み（エンジンは逐語コピーで API 整合は保証、
改修部のみ個別確認）。**残＝ユーザー実機での §8 回帰**（特に §5.6 のリスナ順序と空スロット安全）。
FW ビルド = `./test/build-test.sh`（要 Docker。`EXTRA_SNIPPET=torabo-combos` を付与）。

## 9. フェーズ
1. **調査クローズ**：§0 残リスク 1（virtual position レンジ）・2（local_id）を実コードで確定。
2. **FW**：`dyn_combo.c`（逐語コピー＋4改修）+ `combo_state.c` + `gatt_service` char + snippet/Kconfig。
   **空スロットで通常タイピングが壊れないこと最優先**で回帰（§8）。一度フラッシュ。
3. **アプリ**：コンボタブ + read/write。READ E2E → WRITE→即反映 E2E（実機目視）。
4. **取り込み**：keymap.keymap の純正 combo → スロット。
5. **バックアップ**統合。
6. 各フェーズ後にフェイルセーフ回帰ゲート（§8）。**通過まで実機常用フラッシュ禁止。**

## 10. 決定事項（仮・レビューで確定）
M=16 / P=6 / target enum=KP,MO,TO,TOG,DMAC / NVS 一次 + 取り込み + backup /
`zmk-feature-dynamic-keymap` 内の新機能（`CONFIG_ZMK_DYNAMIC_COMBOS`）/ 純正 combo は無効化（単一所有）/
keymap 編集不要 / アイドル時 atomic publish。

> 旧 DESIGN-macros.md §11（combo ライブ化 Phase 2）の発展版。本ドキュメントが combo の正とする。
