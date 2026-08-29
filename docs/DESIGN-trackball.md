# 統一設計 v2 — torabo-tsuki トラックボール ライブ設定

> 目的：ZMK Studio 相当のUIから、**トラックボールの「軸ごとの挙動」と「temp_layer（戻り時間・切替先）」を編集 → 保存で即反映**できるようにする。
> v1 は実機をブリックさせた（トラックボール停止・キー誤動作・BLE崩壊）。本書は根本原因とエージェント・レビューの安全則を踏まえ、UI / 受け渡し / FW を一気通貫で安全に設計し直すもの。実装はこの設計の合意後に着手する。
>
> **決定済み（ユーザー合意）**：①編集可能FW構成へ ②速度も編集可 ③**軸ごと(役割/向き/速度)モデル** ④temp_layer は**時間も切替先レイヤーも編集可**。

---

## 0. v1 がなぜ壊れたか（設計に効く要点）

1. **設定値が「動きを止める経路」に入っていた**（routerが全パイプライン置換、設定次第で `STOP`/ゼロ倍）。
2. **`mode=0`＝停止(NONE)＝ゼロ初期値**だった（フェイルクローズ）→ 空/壊れ設定で即死。
3. **NVS無検証**（同サイズのゴミblobを丸ごと採用）→ USB有線でも死ぬ・再起動で復活しない。
4. **temp_layer 自作**がレイヤー自己再トリガで居座り → キー誤動作。

→ §2 の鉄則に直結。

---

## 1. スコープ（編集可能にするもの）

**軸ごと（各レイヤーの X軸・Y軸 を独立に）：**
- **役割 role**：`MOVE`(カーソル移動) / `SCROLL`(その軸方向のスクロール = X→水平/Y→垂直) / `OFF`(無効)
- **向き direction**：通常 / 反転（順回転・逆回転）
- **速度 speed**：分周 1..32（1=最速、大きいほど遅い。**0は作らない**）

**レイヤーごと：**
- **temp_enable**：そのレイヤーでボール操作時に temp_layer 切替を起こすか（既定：層0,1=ON、他=OFF）

**全体（temp_layer）：**
- **切替先レイヤー temp_target**：ボール操作で一時的に切替わる先（既定1、編集可・実在レイヤー数未満に検証）
- **戻り時間 temp_timeout_ms**：放置後に元へ戻るまで（既定500、編集可・50..30000にクランプ）

> 現状の挙動はこのモデルで完全再現可能（§4 の既定値表）。

---

## 2. 安全の鉄則（v2 の設計契約・厳守）

1. **フェイルオープン**：設定が空/未設定/範囲外/壊れ/未ロードなら**必ず「素のマウス移動(MOVE)」に倒す**。movement を落とす経路をデフォルト/エラー側に置かない。
2. **`STOP`・MOVE時のゼロ倍を作らない**。`OFF`(×0) は「**検証済みで明示的に OFF を選んだ軸**」のときだけ。未設定/不明 role は必ず MOVE。
3. **`role=0`＝MOVE**（停止ではない）。ゼロ初期化/消去NVS＝素の移動。「停止」という編集可能値は持たない（OFFは別の非ゼロ値）。
4. **speed は 1..32 にクランプ**。MOVE経路に ×0 を絶対に出さない。
5. **temp_layer は ZMK純正コードを逐語コピー**して使う（§5.2）。状態機械・`layer_state_changed`購読（自己補正）はそのまま。変更は「時間・切替先・enable を検証済みRAMから読む」点だけ。ゼロから再実装しない。**単一所有**（純正と本コピーを同時に走らせない）。
6. **入力スレッドは設定をロックレス read のみ**。BLE/NVS は「検証→shadow組立→一括公開」。半適用・ブロッキングを入力経路に持ち込まない。
7. **NVS/BLE入力は厳格検証**：固定長＋magic＋version は**不一致なら丸ごと拒否→既定**。各値は範囲クランプ。生blob採用は廃止。
8. **モジュール無効(Kconfig n)時は素のパイプラインが残る**こと（stock overlay 無改変、置換はモジュール有効時のオーバーレイ側で行う）。movement経路の置換 `ztc_pointer` は**それ自体がフェイルオープン**であること（stock children を fallback に頼らない）。
9. 本モジュールは temp_layer 以外で `zmk_keymap_layer_activate/deactivate` を呼ばない。レイヤー index/id は純正同様 `zmk_keymap_layer_index_to_id` で変換（Studioのレイヤー並べ替えに耐える）。

---

## 3. 全体アーキテクチャ（3層・一気通貫）

```
┌──────────────── アプリ (Torabo Studio) ────────────────────┐
│ ZMK Studio fork。Keymapタブ(既存・無改変) + 「Trackball」タブ │
│   開く=Read→デコード→UI / 編集→[保存]→エンコード→Write     │
│ Tauri(Rust/bluest)が独自GATTをread/write（Studio RPCは無改変）│
└───────────────────────────┬────────────────────────────────┘
                            │ BLE / 独自GATT(暗号化) / packed wire 1個
┌───────────────────────────┴───── FW (zmk-feature-trackball-config) ┐
│ GATT窓 ──検証──▶ RAMストア ──▶ NVS永続化                          │
│                    ▲ロックレスread                                 │
│ ztc_pointer (軸ごと・フェイルオープン) ── 純正コピー ztc_temp_layer │
└────────────────────────────────────────────────────────────────────┘
```

トランスポートは**独自GATTのまま**（Studio RPC/protobuf は拡張しない＝本体・messages repo 無改変で事故範囲最小）。UIのUXは「Studioと同等」にできる。

---

## 4. データモデル / ワイヤ protocol（UIとFWの契約・唯一の正）

**packed・little-endian・固定長・versioned**（v1の生struct/padding依存を廃止）。

```c
struct ztc_axis {        // 4B
    uint8_t role;        // 0=MOVE,1=SCROLL,2=OFF（不明/未設定=MOVE扱い）
    uint8_t direction;   // 0=通常,1=反転
    uint8_t speed_div;   // 1..32（0が来ても1にクランプ＝ゼロ倍を作らない）
    uint8_t _rsv;        // 0
};
struct ztc_layer { struct ztc_axis x; struct ztc_axis y; uint8_t temp_enable; uint8_t _rsv[3]; }; // 12B
struct ztc_wire {
    uint16_t magic;            // 0x7A74。不一致=全拒否
    uint8_t  version;          // 2。不一致=全拒否
    uint8_t  layer_count;      // N（>ZTC_MAX_LAYERS なら拒否）
    struct ztc_layer layers[/*N*/];
    uint8_t  temp_target;      // 切替先（実在レイヤー数未満に検証、外れたら既定1）
    uint8_t  _rsv;
    uint16_t temp_timeout_ms;  // 50..30000 にクランプ
} __packed;
```

- **TS(アプリ)とC(FW)で同一定義を共有**。magic/version/長さ=拒否、各値=クランプを区別。

**既定値（＝現状の挙動を完全再現）：**

| レイヤー | X軸 (role/向き/速度) | Y軸 | temp_enable |
|---|---|---|---|
| 0,1 | MOVE / 反転 / 1 | MOVE / 反転 / 1 | ON |
| 2 | SCROLL(水平) / 反転 / 8 | OFF | OFF |
| 3 | OFF | SCROLL(垂直) / 通常 / 8 | OFF |

temp: target=1, timeout=500。

---

## 5. FW 設計

### 5.1 `ztc_pointer`（軸ごと処理・フェイルオープン）
- パイプライン上に1個（モジュール有効時の overlay 内で listener 子に配置）。毎イベント：
  1. 軸（X か Y）を判定。
  2. active layer index→`zmk_keymap_layer_index_to_id` 経由で、その層・その軸の `{role,direction,speed_div}` を**絶対に失敗しない取得関数**で取る（範囲外/未設定→MOVE/通常/既定）。
  3. **向き**：reverse なら `value=-value`。
  4. **速度**：`value/=speed_div`（≥1、剰余保持）。**MOVE経路に ×0 は存在しない。**
  5. **役割**：
     - `MOVE` → code 不変（REL_X/REL_Y）。`CONTINUE`。
     - `SCROLL` → code を X→`REL_HWHEEL`/Y→`REL_WHEEL` に。`CONTINUE`。
     - `OFF`（検証済みで明示時のみ）→ `value=0` にして `CONTINUE`（STOPは使わない＝listenerに握り潰されない値ベース抑制）。
- **temp_layer はここで触らない**（§5.2 が所有）。`role` の不明/ゼロは MOVE＝フェイルオープン。

### 5.2 `ztc_temp_layer`（純正の逐語コピー＋3点だけ可変）
- **ZMK v0.3 `app/src/pointing/input_processor_temp_layer.c` を逐語コピー**して本モジュールに収め、compat を `zmk,input-processor-ztc-temp-layer` にする。
- **保持（無改変）**：`layer_disable_works` 状態機械、`temp_layer_action_msgq`、`ZMK_LISTENER/ZMK_SUBSCRIPTION(zmk_layer_state_changed)` による自己補正、`zmk_keymap_layer_index_to_id` 変換。
- **変更点は3つだけ**（すべて検証済みRAMから）：
  - **timeout**（param2 の代わりにクランプ済みRAM値、未設定はDT既定）
  - **target**（param1 の代わりに検証済みRAM値＝実在レイヤー数未満、外れたらDT既定）
  - **enable**：handle_event 冒頭で「active layer の temp_enable が false なら何もせず `CONTINUE`」を1行追加（スクロール層で誤発火させない）。
- overlay で `<&zip_temp_layer 1 500>` を `<&ztc_temp_layer 1 500>` に差し替え＝**単一所有**。param1/param2 はDT既定値（RAM未設定時のフォールバック）。
- 返り値はクランプ：movement イベントに対し `STOP`/負値を**出さない**（param1範囲を冒頭で保証し、純正同様 `CONTINUE`）。

### 5.3 RAMストア + NVS（`config_state.c` 全面改修）
- 小さなスカラ配列：`axis[ZTC_MAX_LAYERS][2]`（role/dir/speed）, `temp_enable[ZTC_MAX_LAYERS]`, `temp_target`, `temp_timeout`。
- **静的初期値＝§4既定値（素の挙動）**。ゼロ/未ロードでも MOVE 寄り安全側。
- 取得関数は**失敗しない**（範囲外→MOVE/既定）。
- NVS handler：`len==sizeof && magic && version` でなければ**拒否→既定維持**。満たせば各値クランプして一括適用。生blob memcpy 廃止。

### 5.4 GATT窓（`gatt_service.c` 改修）
- 1 characteristic（READ/WRITE, **ENCRYPT必須**）。WRITE：`offset!=0`/`len!=sizeof`/magic/version を検証 → stack shadow にクランプ組立 → **成功時のみ** `ztc_apply()`＋`ztc_save()`。失敗は `VALUE_NOT_ALLOWED` で無変更。READ は現状を wire で返す。入力スレッドが読む値はスカラ単位で公開。

### 5.5 devicetree / 撤去
- stock `torabo_tsuki_lp_right.overlay` は**無改変**。置換はモジュール有効時の overlay/snippet で `<&ztc_pointer>, <&ztc_temp_layer 1 500>` を listener 子に入れる形で行う（最小形は実装設計で確定。`ztc_pointer` がフェイルオープンなので安全）。
- **撤去**：`input_processor_router.c`/`input_processor_tb_spike.c`/`router_test.overlay`/`spike_test.overlay`＋CMake/binding。`grep tb_router; grep delete-node` が build path から消えること（実装の**最初の工程**）。

---

## 6. アプリ / UI 設計（[Torabo Studio](https://github.com/tak-2025/Torabo-Studio)）
- 「Trackball」タブ（既存流用・拡張）。
  - **レイヤーごとに X行 / Y行**：役割(Move/Scroll/Off ドロップダウン) ＋ 向き(通常/反転トグル) ＋ 速度。
  - レイヤー行に **temp_enable** トグル。
  - 全体に **切替先レイヤー** と **戻り時間(ms)**。
  - **[Read]**＝開いた時に取得→反映、**[保存]**＝エンコード(§4)→GATT write→FWが検証・即適用・NVS保存。
- Rust(`trackball_read/write_config`) と TS codec(`ztcConfig.ts`) を §4 の packed wire に一致させる。Studio RPC は無改変。

---

## 7. フェイルオープン保証（チェックリスト）
- [ ] NVS空/消去/旧version/壊れ → 既定 → **MOVE通過**（USB有線で動く）。
- [ ] BLE不正書込（長さ/magic/version/範囲外）→ 拒否 → 無変更・動作継続。
- [ ] `ztc_pointer` 全経路で MOVE時に `STOP`/×0 が出ない（OFFは明示検証時のみ）。
- [ ] temp_layer は `ztc_temp_layer` 単一所有。純正の `layer_state_changed` 自己補正を保持。movement に STOP/負値を返さない。
- [ ] index/id 変換を純正同様に実施（並べ替え耐性）。

---

## 8. テスト & 即ロールバック計画
1. **撤去先行**：router/spike を build path から消し `grep` で0件確認。
2. **未設定パリティ**：`CONFIG_ZMK_TRACKBALL_CONFIG=y` かつ NVS未書込で**素と挙動同一**を確認。
3. **“USB有線で死なない”回帰ゲート（最重要）**：BLE off・NVS未書込／ゴミ書込→拒否後 の両方で**カーソルが動く**ことを先に確認。
4. **`STOCK_*_central.uf2` と `settings_reset.uf2` を常備**。ロールバック＝bootloader(電池ON・リセット1回起動/2回DFU)へ stock を drag。NVS破損時 settings_reset。
5. BLE：Read→既定確認 → 軸/向き/速度を変更→即反映 → 再起動後も保持＆動作。
6. 負値テスト：長さ/magic/version/範囲外 を各々書込→全拒否・動作継続。
7. ストレス：30秒連続操作→temp層が戻る・キー正常・BLE維持（単一所有 temp_layer 確認）。target を別の実在レイヤーに変えて切替先が変わること、範囲外targetは既定に倒れることを確認。

---

## 9. フェーズ計画
- **Phase A**：`ztc_pointer`（軸ごと役割/向き/速度）＋ ストア/NVS/GATT/アプリUI。temp は DT既定（target1/500）固定で**先に通す**。
- **Phase B**：`ztc_temp_layer`（純正コピー＋時間/切替先/enable を可変）。A が実機で安定後。
- 各フェーズ：撤去→実装→**コンパイルgreen＆grep検査**→（合意後）USB有線検証→BLE検証。

---

## 12. 実装後コードレビュー反映（3エージェント・実コード照合）

- Lens1（pointer/config_state）= **PASS**。fail-open contract 成立。minor のみ（剰余は §11.C の `state->remainder` ではなく**driver-data の per-axis `rem_x/rem_y`** を採用＝単一device/単一listenerで安全・低速も潰れない＝こちらを正とする／ダブルバッファは厳密RCUでないが両バッファ常に有効値で fail-open／INT16_MIN 否定は防御追加済）。
- Lens2（temp_layer）= 要修正→**修正済**：①**target ライブ変更で旧レイヤー居座り**（v1級）→「**target はアイドル時のみ採用**・操作中は据置」で根治。②**msgq溢れで deactivate 消失**→キュー深さ default 16、timeout は常に下限以上で必ず disable を再スケジュール。
- Lens3（wire/gatt/build）= **PASS**。codec バイト一致・対称、GATT検証/原子適用、単一所有 OK、NVS再検証 OK。注意点：
  - **ワイヤは固定長**（`len==8+ZTC_MAX_LAYERS*12`）。アプリは**フル長で送る**こと（Read→編集→同数Write で round-trip）。短い blob は拒否（fail-open で動作は継続するが編集は無反映）。
  - **MTU**（モジュール外）：ATT MTU ≥ wire長+3 が必要。torabo は4層＝56B＝小さく問題になりにくいが、親config で確認。
  - **出荷時**：親リポジトリの overlay でも `<&zip_temp_layer>` 参照を全て撤去し、最終 `.config` に `CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER` が無いこと（単一所有 grep ゲート）。

---

## 11. 実装仕様（最終敵対レビュー反映・これが実装の正）

> 3観点レビュー（実ZMK v0.3ソース照合）の must-fix を確定仕様化。§1-9 と齟齬があれば**本節を優先**。

### 11.A temp_layer 単一所有 と overlay（鉄則8の例外を明文化）
- stock `torabo_tsuki_lp_right.overlay` は `mouse_mode` で `<&zip_temp_layer 1 500>` を参照している。**これを残すと純正temp_layerもコンパイルされ二重所有＝衝突**。
- → **モジュール有効ビルドでは、listener の該当 input-processors を override** して `zip_temp_layer` 参照を消し `<&ztc_temp_layer 1 500>` にする（モジュール用 overlay 側で。stock ファイル自体は無改変のまま、ビルド時 overlay で上書き）。
- **ビルド検査ゲート**：モジュール有効時の最終 `.config` に `CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER` が**立っていない**ことを確認（grepで自動チェック）。立っていたら単一所有が崩れている。
- `ZTC_MAX_LAYERS` は手書き定数禁止。**`ZMK_KEYMAP_LAYERS_LEN` に一致**させる（temp_layer内部の `MAX_LAYERS` と一致必須）。

### 11.B `ztc_temp_layer`（純正逐語コピー＋確定変更点）
コピー元：`input_processor_temp_layer.c`。変更は以下のみ：
1. **`DT_DRV_COMPAT` を `zmk_input_processor_ztc_temp_layer`** に。
2. **ファイル静的シンボルを全改名**（必須・defense）：`ZMK_LISTENER/ZMK_SUBSCRIPTION(processor_temp_layer→ztc_processor_temp_layer)`、`temp_layer_action_msgq→ztc_..`、`layer_disable_works→ztc_..`、`layer_action_work(_cb)→ztc_..`。`CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_MAX_ACTION_EVENTS` は**モジュール自前の `CONFIG_ZTC_TEMP_LAYER_MAX_ACTION_EVENTS`** に置換。
3. **target を index 基準で一貫化**：`toggle_layer` は INDEX として扱う。`update_layer_state` の `zmk_keymap_layer_activate/deactivate(toggle_layer)` を **`...(zmk_keymap_layer_index_to_id(toggle_layer))`** に変更（配列添字・self-correction は INDEX のまま）。これで reordering 耐性を持たせつつ全consumerを index 統一。
4. **`handle_event` の `param1>=MAX_LAYERS` で `-EINVAL` を返さない**：target は **RAM値を `[0, MAX_LAYERS)` かつ live layer_count 未満にクランプ**（外れたら DT既定1）。movement経路は**常に `CONTINUE`**（`input_listener` は負値でイベントdrop＝禁止）。
5. **timeout は RAM値（50..30000にクランプ・0禁止）**、未設定は DT param2(500)。`param2>0` 分岐が必ず発火するよう 0 を作らない。
6. **enable ゲート**：`handle_event` 冒頭で `idx=zmk_keymap_highest_layer_active(); if (idx<ZTC_MAX_LAYERS && !ztc_temp_enable(idx)) return CONTINUE;`（範囲外＝stock動作のフェイルオープン、ロックレス read）。
7. **DT一式を新規作成**：binding YAML（`ip_two_param.yaml` include・`#input-processor-cells=<2>`・`excluded-positions`/`require-prior-idle-ms` プロパティも宣言）、`/omit-if-no-ref/ ztc_temp_layer` ノード、`CONFIG_ZTC_..`（`DT_HAS_..._ENABLED` gate）、CMake `target_sources_ifdef`。
8. timeout/target/enable の RAM read は**ダブルバッファの公開スナップショットを1回読む**（11.D）。

### 11.C `ztc_pointer`（軸ごと・確定仕様）
- **listener の base processor（常時実行）に挿す**（override子ではない）。`&pointing_listener` の input-processors に入れる。これで全イベントが必ず通り、層に依らずフェイルオープン。
- 演算順：**①向き(invert: `v=-v`) → ②速度(除算・剰余保持) → ③役割**。常に `CONTINUE`、`STOP`厳禁、負値厳禁。
- **速度は純正 scaler の剰余機構を踏襲**：ノードに `track-remainders` を立て、`state->remainder`（listenerが**code毎=軸毎**に確保）を使い `scale_val` を逐語踏襲（`num=v*1+rem; out=num/div; rem=num-out*div;`）。**生の `v/=div` 禁止**（低速が0に潰れる）。div は `speed_div`(1..32)。
- 役割（active layer index を `zmk_keymap_highest_layer_active()` で取得、`axis[idx]` を**index基準**で引く・範囲外→MOVE）：
  - `MOVE`(0/不明)：code 不変。
  - `SCROLL`：剰余割算の**後で** code を X→`REL_HWHEEL`/Y→`REL_WHEEL`。
  - `OFF`：`value=0` にして code は REL_X/Y のまま `CONTINUE`（**STOP不可**。0値RELは無害＝listenerはmode==RELで報告、姉妹軸を潰さない）。
- **前提（要確認・現keymapは満たす）**：レイヤーマスクが**互いに素**（0/1, 2, 3）で single-highest-layer 選択で足りる。`process_next`/重なりが要るならこのモデルは要再検討。
- 注意：`CONFIG_ZMK_POINTING_SMOOTH_SCROLLING` 有効時 wheel に追加除算が入る。スクロール既定値はそれ込みで調整。
- `ztc_pointer` は `ztc_temp_layer` より**前**に置く（OFFの0化を先に）。

### 11.D データ公開（ロックレス・確定）
- **ダブルバッファ**：`ztc_snapshot snap[2]` ＋ `atomic_t live_idx`。書込側は非liveバッファに検証済み全フィールドを組み立て、最後に `atomic_set(&live_idx, n)` の**単一swap**で公開。入力スレッドは `atomic_get(&live_idx)` で1回読み、そのスナップショットを参照（半更新を見ない）。フィールド毎の生書込み禁止。
- 静的初期値（両バッファ）＝§4既定（素の挙動）。

### 11.E ワイヤ / NVS / GATT（確定）
- **ワイヤ並び替え**（flexible array は末尾必須）：`magic, version, layer_count, temp_target, _rsv, temp_timeout_ms, layers[N]`。長さ検証は **`header + layer_count*sizeof(ztc_layer)` を手計算**（flexible array 入り struct の sizeof に依存しない）。`BUILD_ASSERT(sizeof(ztc_axis)==4)`, `sizeof(ztc_layer)==12)`、pad無し。
- **MTU**：ワイヤ全長を1 ATT書込（offset==0・単発・full length）に収める。chunk/long-write は拒否。収まらない場合は size/CRC を持たせ検証。`offset!=0` 拒否、`len!=計算長` 拒否、`magic/version` 不一致拒否。
- **検証→shadow→公開**：GATT/NVS とも **stack shadow に読み込み**、magic+version+長さ+全フィールド範囲クランプ → 成功時のみ 11.D の swap で公開 → `ztc_save()`（**クランプ済みのみ永続化**）。失敗は無変更（既定維持）、GATT は `VALUE_NOT_ALLOWED`。
- **NVS load も再検証**：boot の `settings_set` は live store に直接 `read_cb` せず shadow→検証→swap。ビットロット対策に読込時も再検証。

### 11.F フェイルオープン回帰ゲート（フラッシュ前に必ず）
- モジュール有効・NVS未書込／ゴミ書込→拒否後 の両方で、**BLE off の USB有線でカーソルが動く**こと。`speed_div=32`＋微小移動でも最終的に動く（剰余確認）。`grep` で `STOP`/`-EINVAL`/`zmk_keymap_layer_activate`(temp_layer外) が movement経路に無いこと。

---

## 10. 決定事項（確定）
1. 編集可能FW構成へ移行 — ✅
2. 速度も編集対象 — ✅（軸ごと speed_div）
3. 軸ごと(役割/向き/速度)モデル — ✅
4. temp_layer は**時間・切替先レイヤーともに編集可** — ✅（純正逐語コピー方式で安全に。target は実在レイヤー数未満に検証）
5. キーマップ編集は既存 Studio タブで充足済み（本機能の対象外）。
