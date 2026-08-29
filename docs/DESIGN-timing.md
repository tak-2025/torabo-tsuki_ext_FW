# DESIGN-timing — タイミング調整タブ (hold-tap + positional + debounce)

Status: 実装済み・実機検証済み (2026-08-27 採用 / 2026-08-28 デバウンスの split 伝搬を追加、
同日 hold-tap・positional・debounce・split 伝搬まで実機確認してクローズ)。
バックログ3件 (ht-tab / ht-positional / debounce) を1つの timing モジュール +
Studio 1タブに統合する。

## スコープ

ランタイム(再ビルド不要)で調整可能にするもの:

1. Hold-Tap 基本4パラメータ — `tapping-term-ms` / `flavor` / `quick-tap-ms` /
   `require-prior-idle-ms`。対象はキーマップが使う既製2ノード `mt`(mod_tap) と
   `lt`(layer_tap) のみ(カスタム hold-tap ノードは存在しない。計7バインディングが
   この2ノードを共有する — ノード単位設定であり、キー単位ではない)。
2. Hold-Tap positional 系 — `hold-trigger-key-positions`(最大32位置)/
   `hold-trigger-on-release` / `retro-tap` / `hold-while-undecided`。
   (`hold-while-undecided-linger` は対象外、DT 値のまま)
3. kscan デバウンス — `debounce-press-ms` / `debounce-release-ms`(現状 DT デフォルト 5/5ms)。

アーキテクチャは trackpad/combos と同型:
**wire blob ⇄ 純関数 API ⇄ (GATT characteristic + Studio RPC トンネル) ⇄ backends 抽象化 ⇄ MainPanels タブ**。

## 採番(確定値 — FW / Studio 両側で一致させること)

| 項目 | 値 |
|---|---|
| GATT service | `e1f4b000-1c2d-4b6e-9f3a-0a1b2c3d4e5f` |
| GATT characteristic | `e1f4b001-1c2d-4b6e-9f3a-0a1b2c3d4e5f` (READ/WRITE, ENCRYPT) |
| トンネル feature id | `0x10`(= 既存規則: UUID 第3オクテット − 0xa0) |
| caps feature id | `TORABO_FEAT_TIMING = 10`(`TORABO_CAPS_MAX_FEATURES` を 10 へ) |
| caps wire_ver | 1 |
| settings キー | `tmg/wire`(blob 一発、tp と同型) |

## Wire v1 — 96 bytes 固定、little-endian

```
header (8B):
  0    u8  version = 1
  1    u8  ht_node_count = 2
  2    u8  ht_pos_slots = 32
  3    u8  debounce_press_ms    (clamp 1..100)
  4    u8  debounce_release_ms  (clamp 1..100)
  5-7  reserved = 0

ht block × 2 (各44B) — block0 = mt(mod_tap), block1 = lt(layer_tap):
  +0   u16 tapping_term_ms       (clamp 10..2000)
  +2   u16 quick_tap_ms          (0xFFFF = 無効 → 内部では -1)
  +4   u16 require_prior_idle_ms (0xFFFF = 無効 → 内部では -1)
  +6   u8  flavor  (0=hold-preferred, 1=balanced, 2=tap-preferred, 3=tap-unless-interrupted)
  +7   u8  flags   (bit0=retro-tap, bit1=hold-trigger-on-release, bit2=hold-while-undecided)
  +8   u8  pos_count (0 = positional 無効; ≤32)
  +9   u8  reserved
  +10  u8[32] positions (キー位置。未使用スロットは 0)
  +42  u16 reserved
```

READ は常に「現在の有効値」を返す(未書込みなら DT 由来のデフォルト)。
WRITE は全体 blob を検証・クランプして適用し `settings_save_one("tmg/wire")`。
不正 version / 長さ不足は reject(fail-closed)。将来の拡張は version 繰り上げ +
reserved 消費で行う。

## FW 側構成

### fork (tak-2025/zmk) への最小フック — 新ヘッダ `zmk/torabo_timing.h`

behavior_hold_tap.c の config は DT 焼き込み static const のため、fork に
オーバーライド点を入れる。方針は「weak デフォルト = 従来動作、strong 実装は
ext_FW timing モジュール」。fork 単独でもビルド・動作すること。

- `struct zmk_torabo_ht_params` — wire の ht block と同内容の C 構造体。
- `bool zmk_torabo_ht_override(const struct device *dev, struct zmk_torabo_ht_params *out)`
  (weak: false を返す)。behavior_hold_tap.c は **押下時(hold-tap 捕捉時)に1回だけ**
  呼び、true なら per-node の effective config(behavior_hold_tap.c 内の static、
  positions 用に32要素の実領域を持つラッパー)に反映してそのポインタを
  active_hold_tap に載せる。判定途中でパラメータが変わらないこと(押下時ラッチ)が要件。
- `void zmk_torabo_ht_report_dt(const char *dev_name, const struct zmk_torabo_ht_params *dt)`
  (weak: no-op)。behavior init 時に DT デフォルトを timing モジュールへ通知
  (モジュールはこれで READ 用初期値を得る。behavior config 構造体は private のまま)。
- `const struct zmk_debounce_config *zmk_torabo_debounce_effective(const struct zmk_debounce_config *dt)`
  (weak: dt をそのまま返す)。kscan_gpio_matrix.c の `zmk_debounce_update()` 呼び出し点で
  config 参照をこれ経由にする(スキャン毎に読む → 即時反映)。

### ext_FW 新モジュール `timing/`

combos/trackpad の構成をテンプレートに:

- `include/zmk_timing_config/config.h` — wire 定数・構造体・純関数 API
  (`tmg_encode_wire` / `tmg_apply_wire` / `tmg_save`)。
- `src/config_state.c` — 有効値ストア。ht は押下時ラッチ前提なので tp 同様の
  ダブルバッファ(atomic swap)推奨。debounce 2値は atomic u32 パックで可。
  `SETTINGS_STATIC_HANDLER_DEFINE(tmg, ...)`、キー `tmg/wire`。
  strong 実装: `zmk_torabo_ht_override` / `zmk_torabo_ht_report_dt` /
  `zmk_torabo_debounce_effective`。node 対応は dev_name("mod_tap"/"layer_tap" —
  実名は要確認)→ block index。
- `src/gatt_service.c` — service `e1f4b000` / char `e1f4b001`。tp の
  分割書込みリアセンブル(WinRT 対策)を踏襲。
- `src/tunnel_bridge.c` — `TORABO_TUNNEL_FEATURE(timing, 0x10, tmg_encode_wire, tmg_tunnel_write)`。
- caps: `TORABO_FEAT_TIMING = 10` を caps.h に追加し記述子へ載せる(wire_ver=1)。
- Kconfig / CMakeLists / モジュール有効化は combos と同じ配線
  (torabo-tsuki-config 側の conf / snippet も同様に)。

## デバウンスの split 伝搬 (v2 — 2026-08-28)

キースキャンは各半身がローカルに回すため、v1 では app が書いた debounce が central の
kscan にしか効かなかった。v2 でこれを split BLE リンク越しに peripheral へ配る。
**wire は 96B v1 のまま**(同じ2バイトが遠くまで届くようになるだけ)なので、
バージョンではなく **caps ビット** `TORABO_CAPS_TIMING_SPLIT_DEBOUNCE = 0x0001` で表す。

### 方式 — 接続時再送(peripheral は保存しない)

| いつ | 誰が | 何を |
|---|---|---|
| Studio から書き込まれた時 | `tmg_apply_wire` → `zmk_torabo_debounce_split_push()` | 接続中の全 peripheral へ 2B write |
| split 接続の characteristic 発見時 | central.c の discovery | 同上(その peripheral へ) |

peripheral 側に NVS を置かない設計。**正は常に central 1つ**で、再フラッシュや設定
リセットで左右が食い違うことがない。代償は、リンクが確立するまでの数秒だけ peripheral が
DT 値(5/5ms)で走ること — チャタリング防止の閾値としては許容範囲。

### 採番

| 項目 | 値 |
|---|---|
| split GATT characteristic | `ZMK_SPLIT_BT_UPDATE_DEBOUNCE_UUID` = `ZMK_BT_SPLIT_UUID(0x000000a0)` |
| ペイロード | 2B: `[0]=debounce_press_ms`, `[1]=debounce_release_ms`(WRITE_WITHOUT_RESP / ENCRYPT) |
| fork Kconfig | `CONFIG_ZMK_SPLIT_BLE_DEBOUNCE_SYNC`(**両半身に必要**) |
| ext_FW Kconfig | `CONFIG_ZMK_TIMING_SPLIT_PERIPHERAL`(peripheral のみ) |
| caps ビット | `TORABO_FEAT_TIMING` の bit0 = split 伝搬あり |

上流は split UUID を 0x00000001.. と順に使うため、torabo 拡張は **0xa0 から**採番して
将来の上流追加と衝突しないようにする。characteristic は split サービスの**末尾**に置く
(service.c が notify で `attrs[1]` / `attrs[8]` を直に索いているため、位置をずらさない)。

### fork への追加シーム(いずれも `__weak` = 従来動作)

- `bool zmk_torabo_debounce_split_values(uint8_t *press, uint8_t *release)`
  — central が送る値を timing モジュールから貰う。未書込みなら false を返し、
  central は何も送らない(= peripheral は自分の DT 値のまま。central 側の
  `CONFIG_ZMK_TIMING_CONFIG_DEBOUNCE_*_MS` はこちらの半身の話でしかないため)。
- `void zmk_torabo_debounce_split_push(void)`
  — strong 実装は **central.c**(system workqueue へ submit)。モジュール → fork 方向。
- `void zmk_torabo_debounce_split_apply(uint8_t press, uint8_t release)`
  — strong 実装は ext_FW `timing/src/split_peripheral.c`。同ファイルが peripheral 側の
  `zmk_torabo_debounce_effective` も持つ(atomic index + ダブルバッファ、central と同型)。

`ZMK_TIMING_SPLIT_PERIPHERAL` は `depends on !ZMK_TIMING_CONFIG` — 両者が同じシーム
(`zmk_torabo_debounce_effective`)を strong 定義するため、同一ビルドには載せない。

### build.yaml

- central 行: `torabo-timing`(`ZMK_SPLIT_BLE_DEBOUNCE_SYNC` も同 snippet が立てる)
- peripheral 行: **`torabo-timing-split`**(新設。`ZMK_SPLIT_BLE_DEBOUNCE_SYNC` +
  `ZMK_TIMING_SPLIT_PERIPHERAL` の2行だけ)

新旧混在は両方向とも安全。旧 peripheral × 新 central は characteristic が見つからず
write されないだけ(discovery の早期終了が効かなくなる分だけ接続がわずかに遅い)。
新 peripheral × 旧 central は push が来ないので DT 値で走る。

## 既知の制約

- **caps ビットは central のビルドしか語れない** — peripheral に
  `torabo-timing-split` が入っているかは central から見えないため、bit0 は
  「central 側に送信機がある」ことの表明。両半身は同じ build.yaml から出るので
  実運用では一致する。
- **左 central 構成は未検証**。timing のコード自体に半身依存はない(snippet を左 central
  行に付ければ動く見込み)が、検証は右 central ビルドのみ。Studio のデバウンス注記は
  split 伝搬ありの場合は半身名を出さない文言にしてあるため左右どちらでも正しいが、
  伝搬なし(旧 FW)の文言は「central=右手側」前提のまま。
- mt / lt はノード単位共有 — 例えば mt の tapping-term 変更は mt を使う4キー全部に効く。
- **hold-tap は central のみ**(そこで判定されるため全キーに効く)。split で配るのは
  debounce だけ。
- ZMK Studio の keymap 保存とは独立(こちらは torabo 独自ストア)。

## Studio 側構成

- `src/timing/timingConfig.ts` — 上記 wire の decode/encode(exactLength=96)。
- `src/timing/TimingPanel.tsx` — 2セクション構成:
  1. **Hold-Tap**(mt / lt 個別): プリセット行(標準 / ロール誤爆防止 / ホームロウモッド)
     + 詳細展開(4パラメータのスライダー/セレクト、positional の flags と
     キー位置ピッカー — CombosPanel の物理レイアウトピッカー資産を流用)。
     「標準」プリセット値は DT デフォルト(mod_tap.dtsi / layer_tap.dtsi の実値)と一致させる。
  2. **デバウンス**: press / release スライダー2本 + 反映範囲の注記。文言は caps の
     `TimingCap.SplitDebounce`(0x0001)で出し分け — ビットあり=「左右どちらの半身にも
     反映」、なし=従来の「central のみ」警告。caps は `MainPanels` が既に読んでいるので
     `TimingPanel` は prop で受け取る(再読み込みしない)。i18n キーは
     `timing.debounce.bothHalves` / `timing.debounce.centralOnly`。
- backends: `webble/uuids.ts` の `CONFIG_SERVICES.timing`(exactLength 96)、
  `types.ts` に read/write、`index.ts` に pass-through、`rpc/config.ts` の
  `TunnelFeature.Timing = 0x10`。
- caps: `toraboCaps.ts` に `Feature.Timing = 10`、`SUPPORTED_WIRE` に 1、
  `TimingCap.SplitDebounce = 0x0001` と `hasSplitDebounce()`。
- `MainPanels.tsx` の `TABS` に `{ id: "timing", feature: Feature.Timing, group: "edit" }`、
  i18n `tab.timing`(ja: タイミング)。
