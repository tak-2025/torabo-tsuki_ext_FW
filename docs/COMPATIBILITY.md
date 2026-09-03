# COMPATIBILITY — torabo-tsuki_ext_FW の互換性境界

Status: PLAN-ext-fw-refactor.md フェーズ7で新設。§0（絶対に変えてはいけないもの）を、
**現在のコード**の file:line 根拠付きで成文化したもの。フェーズ0〜6（features/ 再編・
common/ 共通化・Kconfig テンプレート化・caps 16 枠化まで）を反映したツリーを基準にする。

この文書自体が「凍結」対象。ここに書かれた file:line がずれたら、まずこの文書を更新する
（コードとドキュメントのどちらが正しいか揉めないように）。

file:line はすべて `torabo-tsuki_ext_FW` リポジトリのルートからの相対パス。★印は
このリポジトリの外（zmk fork）にあるため file:line の凍結対象外だが、契約の起点として
参照するもの。

---

## 1. トンネル feature_id（= GATT UUID 下位バイトと同一の規約）

各 `features/*/src/tunnel_bridge.c` が `TORABO_TUNNEL_FEATURE(_SUB)` に渡す
`feature_id` 定義行。GATT UUID は `e1f4a900` のように「下位バイト = feature_id」の
規則で採番されている（コメントに明記済み）。

| feature_id | 機能 | GATT UUID (下位byte) | 定義 file:line |
|---|---|---|---|
| 0x00 | caps | e1f4a000 | `features/caps/src/tunnel_bridge.c:20` |
| 0x09 | trackball (ztc) | e1f4a900 | `features/trackball/src/tunnel_bridge.c:22` |
| 0x0A | macros (dm) | e1f4aa00 | `features/macros/src/tunnel_bridge.c:21` |
| 0x0B | combos (cb) | e1f4ab00 | `features/combos/src/tunnel_bridge.c:18` |
| 0x0C | trackpad (tp) | e1f4ac00 | `features/trackpad/src/tunnel_bridge.c:36` |
| 0x0D | encoder (enc) | e1f4ad00 | `features/encoder/src/tunnel_bridge.c:21` |
| 0x0E | led | e1f4ae00 | `features/led/src/tunnel_bridge.c:21` |
| 0x0F | live_feed (lf) | e1f4af00 | `features/live_feed/src/tunnel_bridge.c:46` |
| 0x10 | timing (tmg) | e1f4b000 | `features/timing/src/tunnel_bridge.c:26` |

**再採番禁止・append-only。** 次の空き番号は 0x11。

**上限 32 の根拠**（★リポジトリ外・zmk fork）: `torabo_tunnel_feature.feature_id` の
契約コメント — 「subscription set is a bitmask」につき feature_id は 32 未満でなければ
ならない。定義は zmk fork の `app/include/zmk/studio/torabo_tunnel.h:29`
（west モジュールとして取り込まれる、この west ワークスペースでは
`tako-custom/.zmk-workspace/zmk/app/include/zmk/studio/torabo_tunnel.h` に実体がある。
`torabo_tunnel_notify()` の購読状態はビットマスクで管理されるため、この上限を超える
feature_id を採番すると購読が別の feature と衝突する）。同ファイルの
`TORABO_TUNNEL_FEATURE` / `TORABO_TUNNEL_FEATURE_SUB` マクロが各 tunnel_bridge.c から
呼ばれる登録エントリポイントで、`torabo_common/tunnel_wrap.h`（`features/common/include/
torabo_common/tunnel_wrap.h`）はその上に乗る WRITE ラッパ（apply→save 共通化、フェーズ2
A-1）であり、feature_id/GATT/wire そのものには触れない。

---

## 2. caps 記述子の契約

定義: `features/caps/include/zmk_torabo_caps/caps.h`

| 項目 | 値 | file:line |
|---|---|---|
| magic | `0x4354`（"TC" LE） | `caps.h:31` |
| desc_ver | `1` | `caps.h:32` |
| ヘッダ長 | 8B 固定（`TORABO_CAPS_HDR`） | `caps.h:108` |
| 行長 | 4B 固定（`TORABO_CAPS_FEAT`） | `caps.h:109` |
| 最大枠数 | 16（`TORABO_CAPS_MAX_FEATURES`、フェーズ6 B-4 で 10→16） | `caps.h:110` |
| wire cap | `8 + 16*4 = 72B` | `caps.h:111` |
| feature id 列挙 | append-only、`TORABO_FEAT_TRACKBALL=1`〜`TORABO_FEAT_TIMING=10` | `caps.h:35-46` |
| wire レイアウト解説コメント | ヘッダ/行の各フィールド、ATT Read Long への言及 | `caps.h:95-107` |

エンコード実装: `features/caps/src/caps.c`
- `build_features()`（`caps.c:65-117`）— Kconfig の `#if IS_ENABLED(...)` だけで
  組み立てるため、実バイナリと記述子が乖離しない設計。現在 **10/16 枠使用**
  （trackball, macros, combos, trackpad, encoder, led, reserved_layers, live_feed,
  rpc_tunnel, timing の 10 個。`caps.c:68-114`）。
- オーバーラン防止ガード `add_feat()`（フェーズ2 A-5 で追加、`caps.c:46-60`）—
  `*n >= TORABO_CAPS_MAX_FEATURES` で弾いて `LOG_ERR` するのみ。17機能目を足すのに
  `TORABO_CAPS_MAX_FEATURES` を上げ忘れても **スタック破壊はしない**（ログに出て黙って
  1機能落ちるだけ）が、caps が実際より少ない機能数を報告する事故にはなる。
- 実際のエンコード: `torabo_caps_encode()`（`caps.c:119-149`）。
- caps GATT サービス（read-only, UUID e1f4a000/a001）: `caps.c:157-189`。
  UUID 対応コメント: `caps.c:151-155`。

**「10/10 満杯」は FW 側の自己制約であり、アプリ側の壁ではない**。アプリ側デコーダ
（`torabo-studio/src/caps/toraboCaps.ts:36-64`）は `feature_count` 駆動でパースし、
desc_ver・feature_count 双方について前方互換の契約 a〜d を明文化している
（同ファイル冒頭コメント、特に 35-68 行）:
- (a) ヘッダの意味はフィールド単位で不変。`feature_count` は「4B 行の個数」という素朴な
  u8 カウントであり続けること。
- (b) 特徴テーブルはオフセット8開始・1行4Bのまま。行を広げるのは禁止（広げるなら
  新テーブルを追記）。
- (c) 新しいヘッダレベル情報は `_rsv` バイトか、テーブルの**後ろ**に置く。ヘッダとテーブル
  の間に挿すのも、テーブルをオフセット8からずらすのも禁止。
- (d) feature id は安定・append-only。既存 id の意味を変えるのは禁止。

※ `toraboCaps.ts` 冒頭コメント（32-33行）はパスを `torabo-tsuki_ext_FW/caps/include/...`
と書いているが、これはフェーズ1（`git mv` で `features/` 配下へ移動）以前の記述。
現在の実パスは `features/caps/include/zmk_torabo_caps/caps.h` および
`features/caps/src/caps.c`。torabo-studio 側は本フェーズのスコープ外のため未修正
（保留事項として §13 に記載）。

ゴールデンテスト: `test/wire/test_caps.c` が現行 10-feature ビルドの 48B 記述子を
バイト単位で固定（`test_caps.c:21-41`）。

---

## 3. live_feed の凍結

定義: `features/live_feed/include/zmk_live_feed/live_feed.h`

| 項目 | 値 | file:line |
|---|---|---|
| PROTO_VER | `1`（上げない） | `live_feed.h:23` |
| エンベロープ長 | 16B 固定、`BUILD_ASSERT` | `live_feed.h:43-55`（struct 43-53, assert 55） |
| diag レコード長 | 16B 固定、`BUILD_ASSERT`（同エンベロープ共有） | `live_feed.h:128-140` |
| UUID | service `e1f4af00` / feed `e1f4af01` / diag `e1f4af02` | `gatt_service.c:16-19`（解説）, `gatt_service.c:38-40`（定義） |

Torabo-Float は `protoVer !== 1` を全捨てするため、PROTO_VER は上げない。拡張は
新 `evt_type`（現在 1=KEY, 2=LAYER, 3=SNAPSHOT, 4=DIAG。`live_feed.h:26-28, 97`）か、
新キャラクタリスティック（af02 diag の前例）で行う。

**地雷: `gatt_service.c:139-148` が `attrs[1]` / `attrs[4]` をハードコードしている**。

```c
// live_feed_gatt_notify() — attrs[1] = feed characteristic の value 属性
int live_feed_gatt_notify(const struct live_feed_evt *evt) {
    return bt_gatt_notify(NULL, &live_feed_svc.attrs[1], evt, sizeof(*evt));
}
// live_feed_diag_notify() — attrs[4] = diag characteristic の value 属性
int live_feed_diag_notify(const struct live_feed_diag *d) {
    return bt_gatt_notify(NULL, &live_feed_svc.attrs[4], d, sizeof(*d));
}
```

`BT_GATT_SERVICE_DEFINE(live_feed_svc, ...)`（`gatt_service.c:118-133`）の属性順は
`[0]=service, [1]=feed decl, [2]=feed value, [3]=feed CCC, [4]=diag decl, [5]=diag value,
[6]=diag CCC`（コメント `gatt_service.c:143-145` に明記）。**このサービスに属性を1つでも
挿入・並べ替えすると、上記2つの notify がインデックスミスで別の属性に書き込む**
（最悪 CCC ハンドルに書いて未定義動作）。live_feed だけ他の5サービスと違い
`torabo_common/gatt_simple.h` を使わず手書きの `BT_GATT_SERVICE_DEFINE` を持つのはこの
理由による。属性挿入禁止。

---

## 4. 各機能の wire フォーマット（magic / version / レイアウト定数）

NVS キー・compatible 文字列は §5, §6 で別掲。ここでは wire のバイト形状のみ。

### trackball (ztc) — v3

| 定数 | 値 | file:line |
|---|---|---|
| `ZTC_WIRE_MAGIC` | `0x7A74` | `features/trackball/src/config_state.c:30` |
| `ZTC_WIRE_HDR` | 8B | `features/trackball/include/zmk_trackball_config/config.h:143` |
| `ZTC_WIRE_LAYER` | 12B/layer | `config.h:144` |
| `ZTC_WIRE_COAST` | 4B（v3トレーラ） | `config.h:145` |
| `ZTC_WIRE_CAP` | `HDR + N*LAYER + COAST` | `config.h:148` |
| `ZTC_MAX_LAYERS` | `= ZMK_KEYMAP_LAYERS_LEN` | `config.h:33` |

WRITE は v2（coast無し）・v3 の両方を受理、READ は常に v3 で返す（`config.h:128-137`）。

### trackpad (tp) — v1/v2/v3

| 定数 | 値 | file:line |
|---|---|---|
| `TP_WIRE_MAGIC` | `0x7470` | `features/trackpad/include/zmk_trackpad_config/config.h:305` |
| `TP_WIRE_HDR` | 6B | `config.h:306` |
| `TP_WIRE_DEV_HDR`（v1/v2） | 2B | `config.h:307` |
| `TP_WIRE_DEV_HDR_V3` | 5B | `config.h:310` |
| `TP_WIRE_BIND` / `TP_WIRE_AXIS` / `TP_WIRE_GEST` | 4B / 11B / 16B | `config.h:311-313` |
| `TP_WIRE_LAYER_V2` | 38B (axis*2+gest) | `config.h:316` |
| `TP_WIRE_AXIS_V1` / `TP_WIRE_LAYER_V1`（WRITE専用の旧形式） | 3B / 6B | `config.h:318-319` |
| `TP_FLAG_GESTURES` / `TP_FLAG_COAST` | 0x01 / 0x02 | `config.h:322, 326` |
| `TP_WIRE_CAP` | v3 device header基準の上限 | `config.h:330-332` |
| `TP_MAX_LAYERS` | `= ZMK_KEYMAP_LAYERS_LEN` | `config.h:47` |

長さ計算の一本化（フェーズ2 A-4）: `tp_expected_len()`（宣言 `config.h:302`）が
「このヘッダで始まるならバイト列は何B」を返す唯一の場所。`tp_apply_wire()` の
厳密長チェックと GATT WRITE の chunk framing（`torabo_common/wire_asm.h`）の両方が
これを使うため、両者の解釈がズレることがない。

**B-1 バグ修正（フェーズ5、docs/BACKLOG.md）**: `tp_read_fits()`（`config.h:292-294`）が
「その `device_count` で READ したら `TUNNEL_BLOB_MAX_SIZE` を超えないか」を判定し、
`tp_apply_wire()` 内の実際のガードは
`features/trackpad/src/config_state.c:404-408`（`CONFIG_ZMK_STUDIO_TORABO_TUNNEL_BLOB_MAX_SIZE`
依存部）。超える device_count の WRITE は拒否 — 恒久 READ エラー状態を予防する
挙動変更（3-4デバイス構成での実利用有無は要確認、PLAN §フェーズ5）。

### timing (tmg) — v1、96B 固定、magicなし

| 定数 | 値 | file:line |
|---|---|---|
| `TMG_WIRE_VERSION` | `1`（オフセット0、magicの代わり） | `features/timing/include/zmk_timing_config/config.h:47` |
| `TMG_WIRE_HDR` | 8B | `config.h:48` |
| `TMG_HT_POS_SLOTS` | 32 | `config.h:50` |
| `TMG_WIRE_LEN` | `HDR + NODES*BLOCK = 96` | `config.h:52` |
| `TMG_WIRE_CAP` | `= TMG_WIRE_LEN` | `config.h:53` |

長さは**固定**（`TMG_WIRE_LEN` と完全一致しないバイト列は拒否）。version チェックは
`features/timing/src/config_state.c:116`、encode は同 `:215`。他の全機能は
`magic u16` から始まるが timing だけ `version u8` がオフセット0（torabo-studio
`src/backup/sections.ts:75-79` のコメントが per-section `wireVerOffset` を持つ理由）。

### live_feed (le) — v1、16B（§3参照）

### encoder (en) — v1

| 定数 | 値 | file:line |
|---|---|---|
| `ENC_WIRE_MAGIC` | `0x6E65` | `features/encoder/include/zmk_encoder_config/config.h:71` |
| `ENC_WIRE_VERSION` | `1` | `config.h:72` |
| `ENC_WIRE_HDR` | 4B | `config.h:73` |
| `ENC_WIRE_CAP` | `HDR + N*LAYER` | `config.h:76` |
| `ENC_MAX_LAYERS` | `= ZMK_KEYMAP_LAYERS_LEN` | `config.h:25` |

magic/version チェック: `features/encoder/src/config_state.c:123`。

### macros (dm) — v1

| 定数 | 値 | file:line |
|---|---|---|
| `DM_MAGIC` | `0x6D64` | `features/macros/include/zmk_dynamic_keymap/dmac.h:21` |
| `DM_VERSION` | `1` | `dmac.h:22` |
| `DM_SLOTS` | 20 | `dmac.h:23` |
| `DM_READ_HDR` / `DM_READ_WIRE_LEN` | 4B / `HDR+20*81` | `dmac.h:34, 36` |
| `DM_WRITE_HDR` / `DM_WRITE_MAX` | 3B / … | `dmac.h:38-39` |

WRITE は1スロットずつ、magicなし・`buf[0]==DM_VERSION` で検証
（`features/macros/src/config_state.c:85-91`、チェック自体は `:89`）。READ は magic 付き
フルダンプ（`config_state.c:127` 付近）。

### combos (cb) — v1

| 定数 | 値 | file:line |
|---|---|---|
| `CB_MAGIC` | `0x6263` | `features/combos/include/zmk_dynamic_keymap/dcombo.h:30` |
| `CB_VERSION` | `1` | `dcombo.h:31` |
| `CB_SLOTS` | 16 | `dcombo.h:32` |
| `CB_READ_HDR` / `CB_READ_WIRE_LEN` | 4B / … | `dcombo.h:62-63` |
| `CB_WRITE_HDR` / `CB_WRITE_MAX` | 2B / … | `dcombo.h:65-66` |

WRITE 検証: `features/combos/src/combo_state.c:74-77`（`buf[0]==CB_VERSION`）。

### led (le/LED_WIRE) — v1

| 定数 | 値 | file:line |
|---|---|---|
| `LED_WIRE_MAGIC` | `0x656C` | `features/led/include/zmk_led_config/config.h:133` |
| `LED_WIRE_VERSION` | `1` | `config.h:134` |
| `LED_WIRE_HDR` | 6B | `config.h:135` |
| `LED_WIRE_CAP` | `HDR + SIDES*SIDE` | `config.h:138` |

magic/version チェック: `features/led/src/config_state.c:113`。

### ZMK_KEYMAP_LAYERS_LEN 依存の地雷（既知仕様、触らない）

`ZTC_MAX_LAYERS` / `TP_MAX_LAYERS` / `ENC_MAX_LAYERS` はいずれも `ZMK_KEYMAP_LAYERS_LEN`
（devicetree 由来のレイヤー数）に連動する。予約レイヤー数（`TORABO_RESERVED_LAYERS`、
§8/C-5 参照）を変えると総レイヤー数が変わり、この3機能の wire 長が変わって
**NVS に保存済みの blob が長さ不一致で捨てられる**（読み込み時に長さチェックで弾かれ
デフォルトへフォールバック）。フェーズ0の wire ゴールデンテストが
`LAYERS="10 4 20"`（`test/wire/run-tests.sh:32`）で3つの層数を掃引しているのはこのため。

---

## 5. NVS settings キー表

**リネーム絶対禁止**（=全実機の設定消失）。

| キー | KEY/VAL 定義 file:line | 保存呼び出し |
|---|---|---|
| `ztc/wire` | `features/trackball/src/config_state.c:239-240` | `:249` |
| `tp/wire` | `features/trackpad/src/config_state.c:485-486` | `:497` |
| `enc/wire` | `features/encoder/src/config_state.c:182-183` | `:192` |
| `ledx/wire` | `features/led/src/config_state.c:183-184` | `:193` |
| `tmg/wire` | `features/timing/src/config_state.c:306-307` | `:316` |
| `dmk/sN`（スロット別） | `features/macros/src/config_state.c:150` | `:166-167` |
| `cmb/sN`（スロット別） | `features/combos/src/combo_state.c:224` | `:235-236` |

`dmk/sN` / `cmb/sN` は `snprintf(key, ..., KEY "/s%u", slot)` によるリテラル連結
（`config_state.c:165-166`, `combo_state.c:234-235`）。`KEY` 文字列を1文字変えるだけで
既存 NVS の全スロットが読めなくなる。

---

## 6. DT ノード名（behavior device name → local_id 個体採番のキー）と compatible 文字列

### 凍結ノード名（char[16] split payload 制限あり）

| ノード名 | 用途 | 宣言 file:line |
|---|---|---|
| `dynamic_macro` | `&dmac` の親ノード | `snippets/torabo-macros/torabo-macros.overlay:12` |
| `enc_cfg` | エンコーダ設定ノード（`sensor-bindings` 経由で参照） | `snippets/torabo-encoder-live/torabo-encoder-live.overlay:21`（参照: `:34`） |
| `led_ext` | 拡張LED behavior ノード | `snippets/torabo-led-ext-periph/torabo-led-ext-periph.overlay:35` |

`led_ext` は名前を**短く保つ**契約（split payload が device name を `char[16]` に詰めて
運ぶため、長い名前は黙って切り詰められ、以後解決不能になり LED が無反応になるだけで
エラーは出ない）。契約コメント: `snippets/torabo-led-ext-periph/torabo-led-ext-periph.overlay:7-9`。

`tsle_*`（status_led_ext 由来のノード名）はフェーズ1で本体ごと削除済みのため凍結対象外
（`status_led_ext/`・`snippets/torabo-status-led-ext/` は現存しない。ただし
`features/led/{include/zmk_led_config/config.h, Kconfig, src/led_central.c,
src/led_render.c}` に "status_led_ext keeps working untouched" 系の**古いコメントが
残置**されている — 実体は既に無いので誤解を招くが、本フェーズのスコープ外につき未修正。
将来の軽微クリーンアップ候補として記録のみ）。

ノード名変更＝既存 keymap のバインディング解決不能（local_id 個体採番の基準がノード名
であるため）。

### compatible 文字列（既存は不変・新規は `torabo,` プレフィックス）

| compatible | 種別 | file |
|---|---|---|
| `torabo,behavior-encoder-config` | behavior | `dts/bindings/behaviors/torabo,behavior-encoder-config.yaml:13` |
| `torabo,behavior-led-ext` | behavior | `dts/bindings/behaviors/torabo,behavior-led-ext.yaml:13` |
| `torabo,input-processor-enc-button` | input processor | `dts/bindings/input_processors/torabo,input-processor-enc-button.yaml:13` |
| `zmk,behavior-dynamic-macro` | behavior（既存・不変） | `dts/bindings/behaviors/zmk,behavior-dynamic-macro.yaml:6` |
| `zmk,dynamic-combos` | node（既存・不変） | `dts/bindings/zmk,dynamic-combos.yaml:11` |
| `zmk,input-processor-tp-keys` / `tp-pointer` | input processor（既存・不変） | `dts/bindings/input_processors/zmk,input-processor-tp-{keys,pointer}.yaml` |
| `zmk,input-processor-ztc-encoder` / `ztc-pointer` / `ztc-temp-layer` | input processor（既存・不変） | `dts/bindings/input_processors/zmk,input-processor-ztc-*.yaml` |

既存資産（ユーザーの overlay）が `zmk,` プレフィックスの compatible に依存しているため、
これらは変更不可。新規追加分だけ `torabo,` を使う設計が既にこの表の通り実践されている。

---

## 7. Kconfig シンボル凍結

既存シンボルは**リネーム禁止**（ユーザーの `.conf` / `build.yaml` / firmware-builder
出力が直接参照するため、リネーム＝機能が黙って無効化される）。新規は `TORABO_*` のみ。

### menuconfig ルート（機能トグル）

| シンボル | rsource 元 |
|---|---|
| `ZMK_DYNAMIC_KEYMAP`（macros） | `features/macros/Kconfig:1` |
| `ZMK_DYNAMIC_COMBOS`（combos） | `features/combos/Kconfig:1` |
| `ZMK_TRACKBALL_CONFIG` | `features/trackball/Kconfig:1` |
| `ZMK_TRACKPAD_CONFIG` | `features/trackpad/Kconfig:1` |
| `TORABO_RESERVED_LAYERS`（int, layers） | `features/layers/Kconfig:1` |
| `ZMK_ENCODER_CONFIG` | `features/encoder/Kconfig:1` |
| `TORABO_CAPS` | `features/caps/Kconfig:1` |
| `ZMK_LED_CONFIG` | `features/led/Kconfig:1` |
| `ZMK_LIVE_FEED` | `features/live_feed/Kconfig:1` |
| `ZMK_TIMING_CONFIG` / `ZMK_TIMING_SPLIT_PERIPHERAL` | `features/timing/Kconfig:1, 67` |

各機能配下のサブオプション（`ZTC_POINTER`, `ZMK_LED_CONFIG_LEFT_PRESENT`,
`ZMK_TRACKPAD_CONFIG_DEV{0-3}_META`, `ZMK_TIMING_CONFIG_DEBOUNCE_{PRESS,RELEASE}_MS`
等）も同様に凍結。ルート `Kconfig` の `rsource` 一覧は `Kconfig:19-28`。

### テンプレート化された `_BLE` / `_TUNNEL` サブオプション（フェーズ6 B-3）

`config $(module)_BLE` / `config $(module)_TUNNEL` は
`features/common/Kconfig.template.torabo_ble` / `Kconfig.template.torabo_tunnel` に
共通化されているが、**展開後のシンボル名は1文字も変わっていない**
（`ZMK_TRACKBALL_CONFIG_BLE` 等）。受け入れ基準は「生成 `.config` の before/after 機械
diff が空」（PLAN フェーズ6）。

**実装ノート（フェーズ6 申し送り、今回検証で再確認）**:
- Kconfig の `rsource` は**必須**。テンプレートは `module` / `torabo-thing` という
  2つの Kconfig 変数を呼び出し側が直前にセットしてから `rsource
  "../common/Kconfig.template.torabo_ble"` する形（例:
  `features/trackball/Kconfig:11, 17-18`）。
- **help ブロックは `$(var)` を展開しない**（kconfiglib で検証済み、
  `Kconfig.template.torabo_ble:11-13` のコメントに明記）。単行プロパティ
  （`bool "..."` / `depends on` / `default`）だけが `$(var)` 展開される。そのため
  UUID・wire サイズ等の機能固有詳細は、テンプレート側の help には書けず、各機能の
  `Kconfig` 側で `rsource` する行の直前に `#` コメントとして残されている
  （例: `features/timing/Kconfig:25-31`）。新しくテンプレート呼び出しを追加する時も
  この形を踏襲すること。

---

## 8. GATT 属性列の凍結

`torabo_common/gatt_simple.h`（`features/common/include/torabo_common/gatt_simple.h`）
が trackball / led / encoder / macros / combos の5機能に共通の「1属性・READ+WRITE」
GATT サービス形状を提供する（フェーズ5 A-6）。

- `TORABO_GATT_SIMPLE_SERVICE_DEFINE`（`gatt_simple.h:139-146`）は必ず
  **[0]=service, [1]=characteristic** の2属性ちょうどに展開され、プロパティ・
  パーミッションも固定（`BT_GATT_CHRC_READ|WRITE`, `PERM_READ_ENCRYPT|WRITE_ENCRYPT`）。
  増減・並べ替えできるパラメータは存在しない（コメント `gatt_simple.h:26-42`）。
- trackpad / timing は wire がMTUを超えうるため、この共通マクロを使わず**手書きの
  `BT_GATT_SERVICE_DEFINE`** を維持している（`features/trackpad/src/gatt_service.c:103-111`,
  `features/timing/src/gatt_service.c:96-102`）。属性は同じく service+characteristic の
  2つで、`BT_GATT_PERM_PREPARE_WRITE` が追加されるだけ（属性は増えない）。
- live_feed は §3 の理由で3属性+CCC×2の手書き（`features/live_feed/src/gatt_service.c:118-133`）。
- caps は read-only1属性（`features/caps/src/caps.c:180-186`）。

**属性列 = ハンドル順 = BLE 上の公開面**。1要素も増減させない。

---

## 9. バックアップ v5 との関係

torabo-studio のバックアップ機構（`torabo-studio/src/backup/sections.ts`）は
各セクションを「READ wire を base64 のまま格納 / 復元時にそのまま WRITE」という
素通し設計（`sections.ts:1-19` のコメント参照）。セクション表は
`trackball, macros, combos, trackpad, encoder, led, timing`
（`sections.ts:51-58` の `SectionKey`）。

このため **本ドキュメントの wire 凍結（§4）さえ守られていれば、バックアップ v5 は
自動的に安全**。wire レイアウトが変わらない限り、バックアップに書き出したバイト列は
将来のファームウェアでもそのまま WRITE として通る。逆に言えば、wire を割る変更は
バックアップの互換性も同時に割る。`wireVerOffset`（`sections.ts:72-79`）は
「ファイルに保存されているのがどの wire バージョンか」を読み取るためのオフセットで、
機能ごとに異なる（§4 timing の「magicなし・version先頭」参照）。

macros/combos は1ブロックの素通しではなく「デコード→スロット単位で v1 steps op を
再生」方式（PLAN フェーズ8 §9 に詳細）だが、これは将来の dm v2 名前拡張のための設計で
あり、現行 v1 の範囲では実質的に素通しと同じ結果になる。

---

## 10. 命名規約

- **既存の `ZMK_*` Kconfig シンボル、`zmk,` compatible 文字列はリネームしない。**
  新規追加は `TORABO_*`（Kconfig）/ `torabo,`（compatible）に統一する。
  現状の実例は §6, §7 の表の通り（新しい behavior/input-processor は既に `torabo,`
  で採番されている）。
- **LOG モジュール名は既存据え置き、新規は揃える。** 現状は下記のように不統一
  （命名規則の後付け適用は破壊的なため据え置き。新規機能を足すときは
  `<feature>_config` パターンに揃えること）:

  | 機能 | LOG_MODULE 名 | 備考 |
  |---|---|---|
  | trackball | `ztc_config` | `<feature>_config` パターン |
  | trackpad | `tp_config` | 同上 |
  | encoder | `enc_config` | 同上 |
  | led | `led_config` | 同上 |
  | macros | `dmac_config` | 同上（`dm_config` ではなく `dmac_config`） |
  | combos | `dcombo_config` | 同上（`cb_config` ではなく `dcombo_config`） |
  | timing | `tmg_config` / `tmg_split` | 2モジュールに分裂（central/peripheral 用） |
  | caps | `torabo_caps` | パターン外（`caps_config` ではない） |
  | live_feed | `live_feed` | パターン外（`_config` サフィックスなし） |
  | trackball の `ztc_encoder.c` | `ztc_encoder`（レベルも `CONFIG_ZMK_LOG_LEVEL` 共通） | 他の trackball ファイルは `ztc_config`/`CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL` を使うのに、この1ファイルだけ別モジュール・共通ログレベル |

  （根拠: `grep -rn "LOG_MODULE_REGISTER\|LOG_MODULE_DECLARE" features/*/src/*.c` の
  出力を突き合わせ。2026-09-03 時点。）

---

## 11. C-5 / C-6 — 二重管理の警告（本フェーズで Kconfig help に追記済み）

### C-5: `TORABO_RESERVED_LAYERS` と `DTS_EXTRA_CPPFLAGS` の二重管理

`features/layers/Kconfig` の `TORABO_RESERVED_LAYERS`（Kconfig の int 値、
caps が報告する予約レイヤー数の元）と、devicetree プリプロセッサに渡す
`-DTORABO_RESERVED_LAYERS=N`（`cmake-args: -DDTS_EXTRA_CPPFLAGS="-DTORABO_RESERVED_LAYERS=6"`
の形で build.yaml に書く、実際にオーバーレイへレイヤーを注入する側）は、
**ビルドシステム上つながっていない独立した2つの数値**。ズレると:

- devicetree は実際に N1 個の予約レイヤーを注入する。
- `caps.c:91-96`（`TORABO_FEAT_RESERVED_LAYERS` エントリ）は Kconfig 側の N2 を
  そのまま報告する。
- アプリはこの数値をそのまま信じる以外に検証手段がない（caps はレイヤー数を
  自己申告するだけで、実際のレイヤー構成を照合する仕組みが無い）。

→ `features/layers/Kconfig` の help に警告文を追加済み（本フェーズ、`Kconfig:9-29`）。
CMake/ビルド時の自動整合チェックは追加していない（PLAN の「無理に複雑化しない」判断に
従い、help 明記のみ。理由: `DTS_EXTRA_CPPFLAGS` は cmake-args としてのみ渡り、この
モジュールの `CMakeLists.txt` からは可視性が保証されないため、チェックを足すこと自体が
「ドキュメント/Kconfig help 以外のプロダクションコード変更禁止」という本フェーズの
制約に触れる。CMakeLists.txt はいずれにせよ変更していない）。

### C-6: `DEBOUNCE_PRESS_MS` / `DEBOUNCE_RELEASE_MS` と shield kscan ノードの手動同期

`features/timing/Kconfig` の `ZMK_TIMING_CONFIG_DEBOUNCE_PRESS_MS` /
`_RELEASE_MS` は、shield オーバーレイの kscan ノードの `debounce-press-ms` /
`debounce-release-ms` を**手で複製した値**（devicetree のドライバ設定は private で
実行時に読み戻せないため、READ の初期値・「デフォルトに戻す」の基準値として持って
いる）。ズレても実行は壊れない（実際の debounce は devicetree の値が効き続ける）が、
アプリの「現在値」表示が実機と食い違う。caps には一切乗らない値なので、C-5 と違い
「caps が嘘をつく」ことはない — 気づく手段がアプリの表示と shield overlay の目視比較
以外に無い、という違いがある。

→ `features/timing/Kconfig` の help に警告文を追加済み（本フェーズ、`Kconfig:55-60`）。

---

## 12. 変更時チェックリスト

### wire を触るとき
1. §4 の該当機能の magic/version/レイアウト定数の file:line がこの文書と一致しているか
   確認してから着手する（ズレていたらまずこの文書を直す）。
2. wire を**割る**変更（レイアウト変更・フィールド追加でバイト位置がずれる）は禁止。
   append-only な拡張（バージョンを上げて末尾に追記、trackball v2→v3 の coast トレーラ
   や trackpad v2→v3 のデバイスヘッダ拡張が前例）にする。
3. `test/wire/test_<feature>.c` のゴールデンバイト列を更新する
   （更新手順は次項）。`test_contracts.c` に横断的な不変条件があれば併せて確認。
4. `bash ./test/wire/run-tests.sh --docker`（コンパイラが無い環境では必須。ある環境は
   `--docker` 無しでも可）を **LAYERS のデフォルト "10 4 20" のまま**流し、
   全 green を確認する。1つの層数だけ通しても §4 末尾の
   「ZMK_KEYMAP_LAYERS_LEN 依存の地雷」を再現できない。
5. caps の `wire_ver`（`caps.c` の `build_features()` 内、該当機能の entry）を
   実際に上げたか確認する。上げ忘れるとアプリが古い codec で新wireを書き込みに行き、
   データ破損の恐れがある（`toraboCaps.ts` コメントが名指しする「wireがappのcodecを
   追い越すケース」）。
6. 実機で BLE と USB トンネルの両方の READ/WRITE を確認する（tunnel と GATT は同じ
   wire を共有するが実装は別、フェーズ5 でも「最も慎重に」と念押しされている箇所）。

### ゴールデンテスト（test/wire）更新手順
1. 変更前の `test/wire/test_<feature>.c` にある `*_golden` 配列とコメントを読み、
   どのフィールドが変わるか整理する。
2. 実装（`config_state.c` 等）を直してから、`bash ./test/wire/run-tests.sh --docker`
   を一度流し、**失敗した diff**（期待値 vs 実際値）を見て新しい正しいバイト列を
   起こす。手で計算し直さない — 実装が正であるように直したあとの出力を新golden
   にする（実装が事故で golden に引きずられて壊れるのを防ぐ）。
3. 個人のキーマップ・マクロ内容を含む実データ（`TORABO_BACKUP_JSON` 経由の
   ローカル照合、`test/wire/check-local-backup.py`）は**コミットしない**
   （PLAN フェーズ0の「公開/ローカルの切り分け」原則）。合成フィクスチャ（ダミー値）
   だけをリポジトリに置く。
4. `LAYERS="10 4 20"`（デフォルト）で通し、`docs/COMPATIBILITY.md` の該当箇所と
   `.github/workflows/ci.yml` のビルドマトリクスも整合するか確認する。

### 機能を足すとき
1. feature_id は §1 表の続き（現在の次空き番号は 0x11）を append-only で採番する。
2. caps に載せるなら `caps.h` の `enum torabo_feature_id` に append-only で追加し、
   `caps.c` の `build_features()` に1エントリ追加する。16枠中、現在10使用
   （§2）。17機能目を追加する時だけ `TORABO_CAPS_MAX_FEATURES` を上げる
   （フェーズ6 B-4 の前例、desc_ver は上げない）。
3. NVS キーは新規の短い prefix を選ぶ（既存キーの文字列に前方一致しないこと。
   `settings_save_one()` はプレフィックスマッチで読むため、既存キーの延長線上に
   見える新キーは事故のもと）。
4. DT ノード名・compatible を新設するなら、compatible は `torabo,` プレフィックス
   （§6, §10）。ノード名は split 経由で使うなら char[16] 制限を意識する
   （§6 の `led_ext` 前例）。
5. Kconfig シンボルは新規 `TORABO_*`。`_BLE`/`_TUNNEL` サブオプションが要るなら
   §7 のテンプレート `rsource` パターンに従う（help に `$(var)` を書かない）。
6. LOG モジュール名は `<feature>_config` パターンに揃える（§10）。
7. `test/wire/run-tests.sh` の `SOURCES` / `TESTS` / `INCLUDES` 配列
   （`run-tests.sh:83-125`）に新機能のファイルを追加し、ゴールデンテストを書く。

---

## 13. 保留中の検討事項（本フェーズでは着手しない）

- **C-2: combos の公開ヘッダ移動**
  `features/combos/include/zmk_dynamic_keymap/dcombo.h` は、macros と同じ
  `zmk_dynamic_keymap` 名前空間の下に置かれている（歴史的経緯。combos 専用の
  `zmk_dynamic_combos/dcombo.h` へ移す案が PLAN フェーズ7の原案にあった）。
  **外部 fork がこのヘッダを `#include` しているかどうかが未確認**のため、
  ユーザー確認が取れるまで保留。現状維持（インクルードパスは変更しない）。
- **live_feed attrs ハードコードの名前解決化**
  §3 の `attrs[1]` / `attrs[4]` を、インデックス決め打ちでなく名前ベース
  （例えば `BT_GATT_SERVICE_DEFINE` が返すハンドルを名前で引く仕組み）に
  置き換える改修。新しいキャラクタリスティックを live_feed に追加する必要が
  実際に生じるまで着手しない（PLAN 見送りリスト）。
- ~~**A-7（2026-09-03 発見）**: `features/encoder/src/enc_behavior.c` の剰余正規化
  条件が転記ミス（`<= 1000000`、正しくは `<= -1000000`）で常時真になっていた~~
  → **修正済み（2026-09-03、実害なし: 正規化が毎回走るだけで計算結果は同一）**。
- **status_led_ext 由来の古いコメント**（§6 内に記載）: `features/led/` 配下4ファイルに
  "status_led_ext keeps working untouched" という、実体が既にフェーズ1で削除済みの
  機能を指す古いコメントが残っている。ミスリードだが実害はないため、本フェーズでは
  未修正（将来の軽微クリーンアップ候補）。
- **torabo-studio 側 `toraboCaps.ts` のパスコメント更新**（§2 内に記載）:
  フェーズ1のディレクトリ移動を反映していない。torabo-studio リポジトリ側の変更は
  本フェーズのスコープ外。
