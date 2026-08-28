# torabo-tsuki_ext_FW

torabo-tsuki 用の **拡張ファームウェア機能モジュール**（[Torabo-Studio](https://github.com/tak-2025/Torabo-Studio) から設定する側の ZMK モジュール）。

キーボード本体（キー配列・マトリクス定義）は含みません。本体は上流の
[zmk-keyboard-torabo-tsuki-lp](https://github.com/sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp)
を各自で fork / clone し、この **モジュールだけ** を `west.yml` から参照して合体させます。

各機能は独立していて、**ビルド時にスニペット（snippet）を足すことで個別に ON/OFF** します。
焼いた後の値（マクロ内容・速度・LED ルールなど）は再フラッシュ不要で **BLE 経由でライブ編集**できます。

必要なファイル一式はブラウザで開くだけの **firmware-builder** で生成できます
→ <https://tak-2025.github.io/torabo-tsuki_ext_FW/>

> **注意**: 各機能は central/peripheral の配置や接続するデバイスの組み合わせで多数の構成パターンを取ります。
> **そのすべてを実機で動作検証しているわけではありません。** 未検証の構成では動作しない・想定と異なる可能性があります。
> **生成AIを用いて作成しています** 本ツールで問題が起きたとしても何も保証しません。
> 各自の構成で必ず動作確認のうえご利用ください。

---

## 使い方

このモジュールは単体では動きません。**上流のキーボード本体リポジトリを fork し、その
fork の中の数ファイルに追記するだけ**で使えます（本体のファイル自体は書き換えません）。

### 全体の流れ（はじめての方はここから）

PC に開発環境を入れる必要はありません。ブラウザと GitHub のアカウントだけで完結します。

| 手順 | やること |
|---|---|
| **1** | 本体を自分の GitHub に **fork** する → [sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp](https://github.com/sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp)（例 `you/zmk-keyboard-torabo-tsuki-lp`） |
| **2** | **[firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/)** をブラウザで開き、自分の構成と使いたい機能を選ぶ |
| **3** | 生成された `config/west.yml` / `build.yaml` / `<shield>.conf` を、fork の同じ場所に**貼り替えて commit** |
| **4** | commit すると **GitHub Actions が自動でビルド**する。完了したら Actions の成果物（Artifacts）から `.uf2` をダウンロード |
| **5** | キーボードをブートローダーモードにして `.uf2` を**書き込む**（central 用をトラックボール側／統括側に、peripheral 用を反対側に） |
| **6** | 焼いた後の設定値は **[Torabo-Studio](https://tak-2025.github.io/Torabo-Studio/)** から BLE でライブ編集する（再フラッシュ不要） |

以降は、この手順を細かく説明します。

### 推奨: firmware-builder で生成する

GUI ジェネレータを **[ブラウザで開く → firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/)**（インストール不要・入力は端末外に出ません）。
オフラインで使いたい場合は [`firmware-builder/index.html`](firmware-builder/index.html) をダウンロードしてダブルクリックでも同じものが動きます。

構成（central はどちら側か・各側のポインティングデバイス）と使いたい機能を選ぶと、fork に貼り替える
`build.yaml` / `boards/shields/torabo_tsuki_lp/<shield>.conf` / `config/west.yml` をそのまま生成します。
構成に応じた overlay/CONFIG の組み合わせ（トポロジー）を自動で解決するので、下記の手順1〜2 をまとめて自動化できます。

対応する物理パターンの一覧は [`firmware-builder/PATTERN-MATRIX.md`](firmware-builder/PATTERN-MATRIX.md) を参照。

### 手動で足す場合

fork 内で編集するのは次のファイルだけです（本体のファイルは書き換えません）:

| fork 内のファイル | やること |
|---|---|
| `config/west.yml` | このモジュールを依存に追加（[手順1](#手順1-configwestyml)） |
| `build.yaml` | 各エントリの `snippet:` にスニペットを足して機能を ON（[手順2](#手順2-buildyaml)） |
| `config/keymap.keymap` | `&dmac N` をキーに割り当て（マクロを使う場合のみ、[手順3](#手順3-configkeymapkeymap)） |

#### 手順1: config/west.yml

このモジュールを依存に追加します。**既存の** `remotes:` / `projects:` リストの末尾に
追記してください（`remotes:` や `projects:` を**2つ作らない**こと。YAML の重複キーで
zmk 等が消えてビルド不能になります）。

```yaml
manifest:
  remotes:
    # …既存の zmkfirmware / sekigon-gonnoc はそのまま…
    - name: tak-2025                      # ← この1行を追加
      url-base: https://github.com/tak-2025
  projects:
    # …既存の zmk / bmp-boost / drivers… はそのまま…
    - name: torabo-tsuki_ext_FW           # ← このブロックを追加
      remote: tak-2025
      revision: main
  self:
    path: config
```

#### 手順2: build.yaml

fork のルートにある `build.yaml` を開き、対象エントリの `snippet:` 行の**末尾にスニペット名を足す**だけです。
スニペットが overlay も CONFIG も供給するので、`.conf` の手編集は基本不要です（予約レイヤーの枚数指定を除く）。

```diff
  - board: bmp_boost
    shield: torabo_tsuki_lp_right
-   snippet: "studio-rpc-usb-uart split-central input-trackball input-listener"
+   snippet: "studio-rpc-usb-uart split-central input-trackball input-listener torabo-trackball torabo-macros torabo-caps"
    artifact-name: torabo_tsuki_lp_right_central
```

主なスニペット:

| snippet | 効果 |
|---|---|
| `torabo-macros` | カスタムマクロ（`&dmac` ノード定義）。追加設定不要 |
| `torabo-combos` | ダイナミックコンボ。keymap に `combos { compatible = "zmk,combos"; … }` を**置かない**こと（純正 combo と二重所有になる） |
| `torabo-trackball` | トラックボール設定 + オートマウスレイヤー。追加設定不要 |
| `torabo-trackpad` / `torabo-trackpad-live-*` | トラックパッド設定（固定 overlay / ライブ設定）。構成により使い分け |
| `torabo-encoder-live` | ロータリーエンコーダのライブ設定。エンコーダのボタン配線用スニペット（構成別）と併用 |
| `torabo-led-live` | 拡張LED ルール設定（左右別）。`torabo-status-led-ext` とは**排他** |
| `torabo-status-led-ext` | 拡張LED 固定動作（旧式・central 専用） |
| `torabo-reserved-layers` | 予約レイヤーを N 枚追加。**枚数だけ conf で指定**（下記） |
| `torabo-caps` | 機能記述子。app にタブを出させるため central に足す |

機能を有効にする多くのスニペットは **central（本体を統括する側）のエントリ**に足します。
トラックパッド/エンコーダ/LED の物理配線は構成で必要なスニペットが変わるため、
迷う場合は [firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/) に任せてください。

> 全スニペット（機能／配線）の種類・役割・依存関係の一覧は [`docs/SNIPPETS.md`](docs/SNIPPETS.md) にあります。

`torabo-reserved-layers` を使う場合だけ、枚数（0〜10）を
`boards/shields/torabo_tsuki_lp/<shield>.conf` に書きます:

```ini
CONFIG_TORABO_RESERVED_LAYERS=6
```

#### 手順3: config/keymap.keymap（マクロを使うときだけ）

`torabo-macros` を足したら、マクロを出したいキーに `&dmac 0` 〜 `&dmac 19` を置きます。
番号がマクロのスロット番号で、中身（押すキー列）は Torabo-Studio の「マクロ」タブから
BLE で書き込みます（再フラッシュ不要）。

```
# 例: キーマップのどこかのキーを「スロット0/1のマクロ」にする
…  &dmac 0  &dmac 1  …
```

トラックボール・トラックパッド・エンコーダ・LED・予約レイヤーは keymap の編集不要です。

> メモ: `dmac:` ノードは `torabo-macros` が自動で用意します。自分の keymap に `dmac:`
> ノードを手書きしている人だけは、二重定義を避けるため `torabo-macros` を使わず、
> `config/<shield>.conf` に `CONFIG_ZMK_DYNAMIC_KEYMAP=y` を書いて有効化してください。

### ビルドと書き込み

fork に commit / push すれば **GitHub Actions が自動でビルド**します（ローカル環境がある場合は
`west build` でも同じものが作れます）。

1. fork のページで **Actions** タブを開き、実行中のワークフローが緑のチェックになるまで待ちます（数分）。
2. その実行を開き、ページ下部の **Artifacts** から zip をダウンロードして展開すると `.uf2` が入っています。
3. キーボードをブートローダーモードにすると、PC に USB ドライブとして現れます。
   そこへ `.uf2` をコピーすると自動的に書き込まれ、再起動します。
   - **central 用**（`..._central` という名前のもの）→ トラックボール側（統括する側）
   - **peripheral 用** → 反対側

> ワークフローが赤くなった場合は、多くが `config/west.yml` の書き方（`remotes:` / `projects:` を
> 二重に作ってしまった等）です。[手順1](#手順1-configwestyml) を見直すか、
> [firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/) に生成し直させてください。

書き込んだあとの設定値（マクロの中身・速度・LED ルールなど）は、再フラッシュせずに
**[Torabo-Studio](https://tak-2025.github.io/Torabo-Studio/)** から BLE でライブ編集します。

---

## 機能

すべて `default n`。ビルドターゲット（shield）ごとに、必要な機能のスニペットだけを足して有効化します。
BLE で編集する機能は各自 **暗号化 GATT サービス**を1本ずつ持ちます。

| 機能 | 有効化 CONFIG / snippet | 中身 |
|---|---|---|
| **カスタムマクロ** | `ZMK_DYNAMIC_KEYMAP` ／ `torabo-macros` | `&dmac <slot>` で呼ぶ NVS 保存マクロ。BLE でライブ編集。空/無効スロットは何もしない（フェイルセーフ） |
| **ダイナミックコンボ** | `ZMK_DYNAMIC_COMBOS` ／ `torabo-combos` | キー位置の同時押しで behavior を発火。NVS 保存＋BLE ライブ編集。純正 combo エンジンを RAM 定義化した置き換えで、位置コンボの唯一の所有者（`zmk,combos` ノードは置かないこと） |
| **トラックボール設定 / オートマウスレイヤー** | `ZMK_TRACKBALL_CONFIG` ／ `torabo-trackball` | レイヤー/軸ごとの move・scroll・向き・速度と temp-layer（オートマウスレイヤーの戻り時間・切替先）を BLE でライブ設定。不正/空設定でも動きが止まらない fail-open 入力プロセッサ |
| **トラックパッド設定** | `ZMK_TRACKPAD_CONFIG` ／ `torabo-trackpad*` | レイヤー/デバイスごとの move・scroll・off と離散エンコーダ役割（音量/明るさ/ズーム/ブラウザ）・向き・ステップを BLE で設定。拡張パッド構成に対応 |
| **ロータリーエンコーダ** | `ZMK_ENCODER_CONFIG` ／ `torabo-encoder-live` | レイヤーごとの CW / CCW / 押し込みを BLE でライブ割当。回転は ZMK の sensor 経路、ボタンは input 経路に乗るため **keymap 編集・マトリクス変換・物理レイアウト登録は不要** |
| **拡張ステータスLED（ルール設定式）** | `ZMK_LED_CONFIG` ／ `torabo-led-live` | 拡張基盤の3色LEDを「Xが起きたら色C・パターンPで表示」のルール表で左右別々に BLE 設定。central が全ルールを保持し、自分のLEDを駆動しつつ相手側のLED表示を split で押し込む（peripheral 側は設定不要） |
| **拡張ステータスLED（固定動作・旧式）** | `TORABO_STATUS_LED_EXT` ／ `torabo-status-led-ext` | BLE プロファイル切替の色フラッシュ＋相方切断中の赤点灯。central 専用・動作固定。`torabo-led-live` の前身で、**同時有効化は不可**（同じ GPIO を奪い合う） |
| **ライブフィード（キー/レイヤー通知）** | `ZMK_LIVE_FEED` ／ `torabo-live-feed` | central のキー押下・レイヤー状態・スナップショットを 16 バイトの packed イベントで暗号化 GATT NOTIFY。PC の [Torabo-Float](https://github.com/tak-2025/Torabo-Float) オーバーレイが購読して表示。central 専用・未購読時はゼロコスト |
| **予約レイヤー** | `TORABO_RESERVED_LAYERS=N` ／ `torabo-reserved-layers` | 空の予約レイヤーを N 枚（0〜10）追加。ZMK Studio が実行時に `add_layer` で確保できる空き枠。再フラッシュ不要 |
| **機能記述子（caps）** | `TORABO_CAPS` ／ `torabo-caps` | 「このFWは何ができるか」（FWバージョン・搭載機能・各機能のワイヤバージョン・機能ビット）を read-only GATT で自己申告。app は接続時にこれを読み、**存在するタブだけ**を表示する。central に載せる |

> トラックパッド・エンコーダ・LED は接続する物理デバイスや central/peripheral の配置で必要な overlay が変わります。
> 手で選ぶ代わりに **[firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/)（上記）** が構成に応じた正しいスニペットの組み合わせを生成します。

---

## 設定・表示に使うソフトウェア

本モジュールが公開する暗号化 GATT サービスと通信する、対になる 2 つの PC アプリがあります。どちらも
ネイティブ BLE で接続し、GATT セッションは OS レベルで共有されるため**両アプリを同時起動できます**。
いずれも [ZMK Project](https://zmk.dev/) とは提携・承認関係にない非公式ツールです。

### [Torabo-Studio](https://github.com/tak-2025/Torabo-Studio) — 設定・キーマップ編集アプリ

[ZMK Studio](https://github.com/zmkfirmware/zmk-studio) の非公式フォーク。標準のキーマップ／レイヤー編集に加え、
本モジュールの各機能を**再フラッシュせず BLE 経由でライブ編集**するタブを追加します。

- **トラックボール／トラックパッド** — レイヤー・軸ごとの移動／スクロール／速度・ジェスチャ・オートマウスレイヤー。
- **エンコーダ** — 回転（CW/CCW）・押し込みにレイヤーごとの behavior を割当。
- **LED** — 左右の拡張LEDを「条件 → 色・点灯パターン」のルール表で設定（`torabo-led-live`）。
- **マクロ／コンボ** — `&dmac` ダイナミックマクロ・位置コンボを NVS 保存でライブ編集。
- **バックアップ** — 設定・マクロ・コンボ・キーマップを 1 ファイル（.json）でエクスポート／インポート。
- 接続時に**機能記述子（`torabo-caps`）を読み、そのFWが実際に持つ機能のタブだけ**を表示します。
- デスクトップ（Tauri／Windows・macOS・Linux、BLE・シリアル両対応）と WebBluetooth/WebSerial 対応ブラウザの Web 版。

ライブ編集を使うには、編集したい機能のスニペット（＋`torabo-caps`）を含めてビルドした FW が必要です。

### [Torabo-Float](https://github.com/tak-2025/Torabo-Float) — キー入力オーバーレイ

透過・最前面のフローティングウィンドウに、**いま押しているキー**と**アクティブレイヤー**をリアルタイム表示する
Windows 常駐アプリ（Tauri v2 + React）。配信・操作説明のデモ・多レイヤーキーマップの確認用です。

- キー押下もレイヤー変更も **central だけが知る**情報のため、`torabo-live-feed`（GATT NOTIFY／16 バイトの
  packed イベント）から受信します。ライブ表示は NOTIFY のみで成立し RPC に依存しないため、
  **キーボードがロック中でも動作**します。
- 使うには `torabo-live-feed` スニペットを含めてビルドした central FW が必要です。

---

## ディレクトリ構成

```
torabo-tsuki_ext_FW/
├── zephyr/module.yml        # Zephyr モジュール宣言（dts_root: .）
├── Kconfig                  # 各機能サブディレクトリの Kconfig を読み込む
├── CMakeLists.txt           # 各機能を add_subdirectory（CONFIG でガード）
├── dts/                     # behavior / input_processor の binding と dtsi
├── macros/                  # カスタムマクロ（zmk_dynamic_keymap）
├── combos/                  # ダイナミックコンボ（zmk_dynamic_combos）
├── trackball/               # トラックボール設定 + オートマウスレイヤー
├── trackpad/                # トラックパッド設定（tp_pointer / tp_keys）
├── encoder/                 # ロータリーエンコーダ設定
├── led/                     # 拡張LED ルール設定（zmk_led_config）
├── status_led_ext/          # 拡張LED 固定動作（旧式・led/ の前身）
├── caps/                    # 機能記述子（firmware self-description）
├── layers/                  # 予約レイヤー注入
├── snippets/                # 各機能を build に足す snippet 群
├── firmware/                # ビルド済み uf2（動作確認用の参考バイナリ）
├── firmware-builder/        # build.yaml / conf / west.yml を作る GUI ジェネレータ
└── docs/                    # 各機能の設計・仕様ドキュメント
```

---

## ドキュメント

各機能の設計・仕様は [`docs/`](docs/) にあります（トラックボール / トラックパッド / コンボ /
マクロ / 拡張LED など）。

## ライセンス

本モジュール（`torabo-tsuki_ext_FW`）は **MIT**（[LICENSE](LICENSE)）です。本体（上流の
[zmk-keyboard-torabo-tsuki-lp](https://github.com/sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp)）は
**GPL-3.0** で、これは `west.yml` から参照するだけでソースを取り込みません。そのため本モジュールの
コード自体は MIT のまま保てます。

ただし **ビルドして出来上がるファームウェア（`.uf2`）は、GPL-3.0 の本体と静的リンクで結合される
ため、結合物全体として GPL-3.0** になります（構成は ZMK(MIT) + 本体(GPL-3.0) + 本モジュール(MIT)。
MIT は GPL 互換なのでこの結合自体は合法です）。`.uf2` を第三者に配布する場合は GPL-3.0 の条件
（対応ソースの入手手段の提供など）に従ってください。**各自が fork してローカルでビルドする分には、
配布が発生しないため GPL の義務は生じません。**

設定・表示アプリ（[Torabo-Studio](https://github.com/tak-2025/Torabo-Studio) / [Torabo-Float](https://github.com/tak-2025/Torabo-Float)）は
どちらも **BLE / USB 越しにファームと通信する別プログラム**であり、GPL-3.0 のファームを取り込んで
（リンクして）いません。したがって上記の GPL-3.0 は**これらのアプリには及びません**（両者とも Apache-2.0。
Torabo-Studio が Apache-2.0 なのは zmk-studio のフォークだからで、本体の GPL とは無関係です）。
