# torabo-tsuki_ext_FW

torabo-tsuki 用の **拡張ファームウェア機能モジュール**（[Torabo Studio](https://github.com/tak-2025/Torabo-Studio) から設定する側の ZMK モジュール）。

> **はじめての方へ** — 全体像・関連プロジェクトの入口は紹介ポータル
> **<https://tak-2025.github.io/torabo-fun/>**（[tak-2025/torabo-fun](https://github.com/tak-2025/torabo-fun)）にまとめてあります。

キーボード本体（キー配列・マトリクス定義）は含みません。本体は上流の
[zmk-keyboard-torabo-tsuki-lp](https://github.com/sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp)
を各自で fork / clone し、この **モジュールだけ** を `west.yml` から参照して合体させます。
（キーボードそのものの製品情報は [sekigon-gonnoc/torabo-tsuki-lp](https://github.com/sekigon-gonnoc/torabo-tsuki-lp) を参照。
ファームウェアの fork 元は上記の `zmk-keyboard-` 付きの方です。）

各機能は独立していて、**ビルド時にスニペット（snippet）を足すことで個別に ON/OFF** します。
焼いた後の値（マクロ内容・速度・LED ルール・キーの効きなど）は再フラッシュ不要で
**BLE または USB 経由でライブ編集**できます。

必要なファイル一式はブラウザで開くだけの **firmware-builder** で生成できます
→ <https://tak-2025.github.io/torabo-tsuki_ext_FW/>

> **注意**: 各機能は central/peripheral の配置や接続するデバイスの組み合わせで多数の構成パターンを取ります。
> **そのすべてを実機で動作検証しているわけではありません。** 未検証の構成では動作しない・想定と異なる可能性があります。
> **生成AIを用いて作成しています。** 各自の構成で必ず動作確認のうえご利用ください。問題が起きた場合は
> [上流の標準ファームウェア](https://github.com/sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp)に戻してください。
>
> 実機で動作確認しているのは、次の構成です（設定アプリ側の確認は Windows のみ）。
>
> - 右側 central ／ 拡張 HW なし
> - 右側 central ／ 左右にトラックパッド追加
> - 右側 central ／ 左右にトラックパッド追加 ＋ ロータリーエンコーダ（torabo-tsuki の標準構成外）

---

## 使い方

このモジュールは単体では動きません。**上流のキーボード本体リポジトリを fork し、その
fork の中の数ファイルに追記する**ことで使えます。

### 全体の流れ（はじめての方はここから）

| 手順 | やること |
|---|---|
| **1** | 本体を自分の GitHub に **fork** する → [sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp](https://github.com/sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp)（例 `you/zmk-keyboard-torabo-tsuki-lp`） |
| **2** | **[firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/)** をブラウザで開き、自分の構成と使いたい機能を選ぶ |
| **3** | 生成された `config/west.yml` / `build.yaml` / `<shield>.conf` の追記・更新内容を、fork の同じ場所に**貼り替えて commit** |
| **4** | commit すると **GitHub Actions が自動でビルド**する。完了したら Actions の成果物（Artifacts）から `.uf2` をダウンロード |
| **5** | キーボードをブートローダーモードにして `.uf2` を**書き込む**（central 用をトラックボール側／統括側に、peripheral 用を反対側に） |
| **6** | 焼いた後の設定値は **[Torabo Studio](https://tak-2025.github.io/Torabo-Studio/)** から BLE / USB でライブ編集する（再フラッシュ不要） |

### firmware-builder で生成する

Bluetooth での接続名、HW 構成、追加レイヤーの予約など、利用する機能に関する設定を左ペインで選択します。
選択内容に応じて `build.yaml` / `boards/shields/torabo_tsuki_lp/<shield>.conf` / `config/west.yml` の
更新内容を右ペインに表示します。表示された内容に従って、各ファイルの内容を更新してください。



> ⚠ `config/west.yml` は追記だけでなく**置き換えが要る行**があります（`zmk`、トラックパッドを積む構成では
> `zmk-driver-iqs7211e`）。**既存の行を消してから**貼ってください。
> 名称の重複、`remotes:` / `projects:` の二重化をしてしまうと、FWの作成が失敗します。
> `config/west.yml`の設定内容がよくわからない場合は、本アプリのテスト用で使用しているリポジトリの[west.yml](https://github.com/tak-2025/fw-test/blob/35fe3c0eb14f9b6bcfa8ce70868e05601ab61c3e/config/west.yml)をご参照ください。


対応する物理パターンの一覧は [`firmware-builder/PATTERN-MATRIX.md`](firmware-builder/PATTERN-MATRIX.md) を参照。

---

## 機能

すべて `default n`。ビルドターゲット（shield）ごとに、必要な機能のスニペットだけを足して有効化します。
ライブ編集する機能は各自 **暗号化 GATT サービス**を1本ずつ持ち、`torabo-rpc-tunnel` を載せると
**同じワイヤがそのまま Studio RPC（＝USB 接続）でも読み書きできる**ようになります。

| 機能 | 有効化 CONFIG / snippet | 中身 |
|---|---|---|
| **カスタムマクロ** | `ZMK_DYNAMIC_KEYMAP` ／ `torabo-macros` | `&dmac <slot>` で呼ぶ NVS 保存マクロ。BLE / USB でライブ編集。空/無効スロットは何もしない（フェイルセーフ） |
| **ダイナミックコンボ** | `ZMK_DYNAMIC_COMBOS` ／ `torabo-combos` | キー位置の同時押しで behavior を発火。NVS 保存＋ライブ編集。純正 combo エンジンを RAM 定義化した置き換えで、位置コンボの唯一の所有者（`zmk,combos` ノードは置かないこと） |
| **トラックボール設定 / オートマウスレイヤー** | `ZMK_TRACKBALL_CONFIG` ／ `torabo-trackball` | レイヤー/軸ごとの move・scroll・向き・速度と temp-layer（オートマウスレイヤーの戻り時間・切替先）をライブ設定。**慣性スクロール（coast）** — 指を離したあとスクロールが滑って止まる長さ・開始しきい値も設定可（ワイヤ v3 / caps `ZTC_COAST`）。不正/空設定でも動きが止まらない fail-open 入力プロセッサ |
| **トラックパッド設定** | `ZMK_TRACKPAD_CONFIG` ／ `torabo-trackpad*` | レイヤー/デバイスごとの move・scroll・off と離散エンコーダ役割（音量/明るさ/ズーム/ブラウザ）・向き・ステップを設定。**慣性スクロール（coast）をデバイスごとに設定可**（ワイヤ v3 / caps `TP_COAST`）。拡張パッド構成に対応 |
| **ロータリーエンコーダ** | `ZMK_ENCODER_CONFIG` ／ `torabo-encoder-live` | レイヤーごとの CW / CCW / 押し込みをライブ割当。回転は ZMK の sensor 経路、ボタンは input 経路に乗るため **keymap 編集・マトリクス変換・物理レイアウト登録は不要** |
| **拡張ステータスLED** | `ZMK_LED_CONFIG` ／ `torabo-led-live` | 拡張基盤の3色LEDを「Xが起きたら色C・パターンPで表示」のルール表で左右別々に設定。central が全ルールを保持し、自分のLEDを駆動しつつ相手側のLED表示を split で押し込む（peripheral 側は設定不要） |
| **タイミング（タップ反応）** | `ZMK_TIMING_CONFIG` ／ `torabo-timing`（＋ peripheral に `torabo-timing-split`） | キーの「効き」を再フラッシュせず調整。`&mt`（mod_tap）/ `&lt`（layer_tap）の tapping-term・flavor・quick-tap・require-prior-idle と positional 系、および kscan の**デバウンス**（press/release）。アプリ上のタブ名は**「タップ反応」**。設定はノード単位（`&mt` を使う全キーに効く）。デバウンスは相手側に `torabo-timing-split` を載せると左右両方に届く（caps `TIMING_SPLIT_DEBOUNCE`）。**zmk 本体の fork が必須** |
| **USB トンネル（RPC）** | `ZMK_STUDIO_TORABO_TUNNEL` ／ `torabo-rpc-tunnel` | 上記の独自設定を、BLE GATT に加えて **Studio RPC の土管にも流す**汎用トンネル。ワイヤは GATT と 1 バイトも同じ。これが入っていると **USB 接続でも全タブが読み書きでき**、USB 接続の Torabo Float にライブフィードが届く。載っている機能のぶんだけ自動で口が開く。central 専用・**zmk 本体の fork が必須** |
| **ライブフィード（キー/レイヤー通知）** | `ZMK_LIVE_FEED` ／ `torabo-live-feed` | central のキー押下・レイヤー状態・スナップショットを 16 バイトの packed イベントで暗号化 GATT NOTIFY（`e1f4af00`）。トンネル対応 FW なら USB でも届く。PC の [Torabo Float](https://github.com/tak-2025/Torabo-Float) オーバーレイが購読して表示。診断用の第2キャラクタリスティック `e1f4af02` あり。central 専用・未購読時はゼロコスト |
| **予約レイヤー** | `TORABO_RESERVED_LAYERS=N` ／ `torabo-reserved-layers` | 空の予約レイヤーを N 枚（**1〜10、既定 10**）追加。ZMK Studio が実行時に `add_layer` で確保できる空き枠。再フラッシュ不要。枚数は build.yaml の `cmake-args` で指定（conf の CONFIG は申告用） |
| **機能記述子（caps）** | `TORABO_CAPS` ／ `torabo-caps` | 「このFWは何ができるか」（FWバージョン・搭載機能・各機能のワイヤバージョン・機能ビット）を read-only GATT で自己申告。app は接続時にこれを読み、**存在するタブだけ**を表示する。central に載せる |

> トラックパッド・エンコーダ・LED は接続する物理デバイスや central/peripheral の配置で必要な overlay が変わります。
> 手で選ぶ代わりに **[firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/)（上記）** が構成に応じた正しいスニペットの組み合わせを生成します。

## ディレクトリ構成

```
torabo-tsuki_ext_FW/
├── zephyr/module.yml        # Zephyr モジュール宣言（dts_root: .）
├── Kconfig                  # 各機能サブディレクトリの Kconfig を読み込む
├── CMakeLists.txt           # 各機能を add_subdirectory（CONFIG でガード）
├── dts/                     # behavior / input_processor の binding と dtsi
├── features/                # 機能本体（1機能＝1サブディレクトリ・独立ビルド）
│   ├── macros/              # カスタムマクロ（zmk_dynamic_keymap）
│   ├── combos/              # ダイナミックコンボ（zmk_dynamic_combos）
│   ├── trackball/           # トラックボール設定 + オートマウスレイヤー
│   ├── trackpad/            # トラックパッド設定（tp_pointer / tp_keys）
│   ├── encoder/             # ロータリーエンコーダ設定
│   ├── led/                 # 拡張LED ルール設定（zmk_led_config）
│   ├── timing/              # タイミング調整（hold-tap + kscan デバウンス、split 伝搬）
│   ├── live_feed/           # ライブフィード（キー/レイヤーの NOTIFY・診断）
│   ├── caps/                # 機能記述子（firmware self-description）
│   └── layers/              # 予約レイヤー注入
├── snippets/                # 各機能を build に足す snippet 群
├── test/                    # ホスト wire ゴールデンテストとビルド確認スクリプト
├── firmware-builder/        # build.yaml / conf / west.yml を作る GUI ジェネレータ
└── docs/                    # 各機能の設計・仕様ドキュメント
```

> ローカルでビルドすると `firmware/` が作られますが、これは **ビルド成果物の置き場で
> `.gitignore` 済み**です。clone しても存在しません。

---

## ドキュメント

各機能の設計・仕様は [`docs/`](docs/) にあります（トラックボール / トラックパッド / コンボ /
マクロ / 拡張LED など）。まず読むならこの2つです。

- [`docs/SNIPPETS.md`](docs/SNIPPETS.md) — **全スニペットの一覧**（機能／配線、依存・排他、
  どちらの側に載せるか）。手で `build.yaml` を組むときの早見表。
- [`docs/DESIGN-timing.md`](docs/DESIGN-timing.md) — タイミング（タップ反応）の設計。
  hold-tap のワイヤ仕様、デバウンスの split 伝搬、zmk fork 側のフック。

## ライセンス

本モジュール（`torabo-tsuki_ext_FW`）は **MIT**（[LICENSE](LICENSE)）です。ただし、これを組み込んで
ビルドしたファームウェア（`.uf2`）には GPL-3.0 のコンポーネントが静的リンクされます。

| 部品 | ライセンス |
|---|---|
| 本モジュール（`torabo-tsuki_ext_FW`） | MIT |
| ZMK（`west.yml` では [tak-2025/zmk](https://github.com/tak-2025/zmk) の `dev`） | MIT |
| キーボード本体（[zmk-keyboard-torabo-tsuki-lp](https://github.com/sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp)） | **GPL-3.0** |
| IQS7211E ドライバ fork（[tak-2025/zmk-driver-iqs7211e](https://github.com/tak-2025/zmk-driver-iqs7211e) の `torabo-tsuki`） | **GPL-3.0**（トラックパッド構成のときだけ入る） |

- ドライバ fork は上流 [sekigon-gonnoc/zmk-driver-iqs7211e](https://github.com/sekigon-gonnoc/zmk-driver-iqs7211e)（GPL-3.0）
  の派生で、**GPL-3.0 のまま**公開しています（改変ソースは上記リポジトリでそのまま入手できます）。
- したがって **`.uf2` は結合物全体として GPL-3.0** です。第三者に配布する場合は GPL-3.0 の条件
  （対応ソースの入手手段の提供など）に従ってください。各自が fork してビルドし自分で使う分には、
  配布が発生しないため GPL の義務は生じません。
- 設定・表示アプリ（[Torabo Studio](https://github.com/tak-2025/Torabo-Studio) /
  [Torabo Float](https://github.com/tak-2025/Torabo-Float)）は BLE / USB 越しに通信する別プログラムで、
  ファームウェアをリンクしていないため、この GPL-3.0 は及びません（どちらも Apache-2.0）。
