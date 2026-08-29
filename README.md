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
**BLE または USB 経由でライブ編集**できます（USB は `torabo-rpc-tunnel` を含む FW が必要。
ビルダーは常に入れます）。

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
| **6** | 焼いた後の設定値は **[Torabo Studio](https://tak-2025.github.io/Torabo-Studio/)** から BLE / USB でライブ編集する（再フラッシュ不要） |

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

ここでやることは3つです。**追記が1つ、置き換えが2つ**あります。

1. `remotes:` に `tak-2025` を **追記**。
2. `projects:` の `zmk` を **tak-2025 の fork（`dev`）に置き換え**。
3. `projects:` の `torabo-tsuki_ext_FW` を **追記**。トラックパッドを積む構成では
   `zmk-driver-iqs7211e` も **tak-2025 の fork（`torabo-tsuki`）に置き換え**。

> ⚠ **`remotes:` / `projects:` を2つ作らないこと。** YAML の重複キーで zmk 等が消えます。
> ⚠ **置き換えは「既存の行を消してから」入れること。** `- name: zmk` や
> `- name: zmk-driver-iqs7211e` が**同名で2つある**と、west が
> `Malformed manifest`（*used twice*）で落ちます。追記して終わりにしないでください。

**なぜ zmk 本体まで差し替えるのか** — ビルダーが常に入れる `torabo-rpc-tunnel`（USB でも
独自設定を読み書きするトンネル）は RPC subsystem を、`torabo-timing`（キーの効きの調整）は
`<zmk/torabo_timing.h>` のフックを、それぞれ **zmk 本体側**に持っています。上流 v0.3 には
どちらも無いため、上流のままだと必ずビルドエラーになります。この fork の `app/west.yml` が
`torabo.proto` 入りの `zmk-studio-messages` fork も連れてくるので、messages を自分で
書き足す必要はありません。`dev` 固定です（fork の `main` は上流追従用に空けてあります）。

```yaml
manifest:
  remotes:
    # …既存の zmkfirmware / sekigon-gonnoc はそのまま…
    - name: tak-2025                      # ← この1行を追加
      url-base: https://github.com/tak-2025
  projects:
    # …既存の bmp-boost / drivers… はそのまま…

    # ↓ 既存の  - name: zmk / remote: zmkfirmware / revision: v0.3  を削除して置き換え
    - name: zmk
      remote: tak-2025
      revision: dev
      import: app/west.yml

    # ↓ トラックパッドを積む構成のみ。既存の
    #    - name: zmk-driver-iqs7211e / remote: sekigon-gonnoc  を削除して置き換え
    #    （press&hold → INPUT_BTN_2 を出す改変入り fork。GPL-3.0 のまま。
    #      上流のままだとビルドは通るのに Studio の「長押し」割当だけが無反応になる）
    - name: zmk-driver-iqs7211e
      remote: tak-2025
      revision: torabo-tsuki

    # ↓ このモジュール（追記）
    - name: torabo-tsuki_ext_FW
      remote: tak-2025
      revision: main
  self:
    path: config
```

この内容は [firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/) の
`config/west.yml` 出力と同じものです（構成に応じて iqs7211e の行が出ない場合があります）。
迷ったらビルダーの出力をそのまま貼り替えてください。詳しくは
[`docs/SNIPPETS.md`](docs/SNIPPETS.md) 冒頭も参照。

#### 手順2: build.yaml

fork のルートにある `build.yaml` を開き、対象エントリの `snippet:` 行の**末尾にスニペット名を足す**だけです。
スニペットが overlay も CONFIG も供給するので、`.conf` の手編集は基本不要です
（例外は **予約レイヤーの枚数申告・デバイス名・拡張LEDの実在フラグ・トラックパッドのデバイス素性メタ**
の4つで、どれもビルダーが `boards/shields/torabo_tsuki_lp/<central shield>.conf` 側に出力します）。

```diff
  - board: bmp_boost
    shield: torabo_tsuki_lp_right
-   snippet: "studio-rpc-usb-uart split-central input-trackball input-listener"
+   snippet: "studio-rpc-usb-uart split-central input-trackball input-listener torabo-trackball torabo-macros torabo-combos torabo-reserved-layers torabo-caps torabo-rpc-tunnel torabo-timing"
+   cmake-args: -DDTS_EXTRA_CPPFLAGS="-DTORABO_RESERVED_LAYERS=10"
    artifact-name: torabo_tsuki_lp_right_central
  - board: bmp_boost
    shield: torabo_tsuki_lp_left
-   snippet: "studio-rpc-usb-uart"
+   snippet: "studio-rpc-usb-uart torabo-timing-split"
    artifact-name: torabo_tsuki_lp_left_peripheral
```

**ビルダーが常に入れる3つ**（迷ったら手で組むときも入れてください）:

- `torabo-caps` — FW が「自分は何ができるか」を申告する。アプリのタブ出し分けの前提。
- `torabo-rpc-tunnel` — 独自設定を Studio RPC にも流す。**これがあると USB 接続でも全タブが使え**、
  USB 接続の Torabo Float にライブフィードが届く。
- `torabo-timing` ＋ 相手側の `torabo-timing-split` — キーの効き（hold-tap とデバウンス）の調整。
  central に本体、peripheral に受け側を**対で**載せます。

`torabo-rpc-tunnel` / `torabo-timing` は zmk 本体側の実体に依存するので、
**[手順1](#手順1-configwestyml) の zmk fork 差し替えが必須**です。

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
| `torabo-reserved-layers` | 予約レイヤーを N 枚追加。**枚数は build.yaml の `cmake-args` で指定**（下記） |
| `torabo-caps` | 機能記述子。app にタブを出させるため central に足す（ビルダーは常に入れる） |
| `torabo-rpc-tunnel` | 独自設定を Studio RPC にも流す汎用トンネル。**USB 接続でも全タブが使える**ようになる。central 専用（ビルダーは常に入れる） |
| `torabo-live-feed` | 押下キー／レイヤーを配信（Torabo Float 用）。central 専用・未購読時ゼロコスト |
| `torabo-timing` | キーの効き（`&mt` / `&lt` の hold-tap パラメータ＋ kscan デバウンス）をライブ調整。central に足す（ビルダーは常に入れる） |
| `torabo-timing-split` | 上の相方。**peripheral に足す**とデバウンスが反対側の半身にも効く |

機能を有効にする多くのスニペットは **central（本体を統括する側）のエントリ**に足します。
トラックパッド/エンコーダ/LED の物理配線は構成で必要なスニペットが変わるため、
迷う場合は [firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/) に任せてください。

> **全一覧は [`docs/SNIPPETS.md`](docs/SNIPPETS.md)** にあります（機能／配線スニペットの
> 種類・役割・依存関係・排他関係）。上の表は代表的なものだけです。

`torabo-reserved-layers` を使う場合だけ、枚数（**1〜10、既定 10**）を**2か所**に書きます。
**両方必要**です。

1. **本体（実際にレイヤーを作る側）** — `build.yaml` の対象エントリに `cmake-args` を足す:

   ```yaml
     - board: bmp_boost
       shield: torabo_tsuki_lp_right
       snippet: "... torabo-reserved-layers"
       cmake-args: -DDTS_EXTRA_CPPFLAGS="-DTORABO_RESERVED_LAYERS=6"
   ```

2. **アプリへの申告用** — `boards/shields/torabo_tsuki_lp/<central shield>.conf` に同じ値を書く:

   ```ini
   CONFIG_TORABO_RESERVED_LAYERS=6
   ```

> ⚠ **`conf` の `CONFIG_TORABO_RESERVED_LAYERS` はレイヤーを作りません。** Zephyr は
> devicetree を Kconfig より先に処理し、DTS の前処理に `autoconf.h` を渡さないため、
> overlay の中では `CONFIG_*` が常に未定義になります。枚数を決めるのは `cmake-args` の方で、
> `conf` の値は `torabo-caps` がアプリに枚数を伝えるためだけのものです。
> 値がズレるとアプリの表示だけが食い違います。ビルダーは両方を揃えて出力します。

#### 手順3: config/keymap.keymap（マクロを使うときだけ）

`torabo-macros` を足したら、マクロを出したいキーに `&dmac 0` 〜 `&dmac 19` を置きます。
番号がマクロのスロット番号で、中身（押すキー列）は Torabo Studio の「マクロ」タブから
BLE / USB で書き込みます（再フラッシュ不要）。

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

書き込んだあとの設定値（マクロの中身・速度・LED ルール・キーの効きなど）は、再フラッシュせずに
**[Torabo Studio](https://tak-2025.github.io/Torabo-Studio/)** から BLE / USB でライブ編集します
（USB で独自設定まで届くのは `torabo-rpc-tunnel` 入りの FW のときです）。

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
| **拡張ステータスLED（ルール設定式）** | `ZMK_LED_CONFIG` ／ `torabo-led-live` | 拡張基盤の3色LEDを「Xが起きたら色C・パターンPで表示」のルール表で左右別々に設定。central が全ルールを保持し、自分のLEDを駆動しつつ相手側のLED表示を split で押し込む（peripheral 側は設定不要） |
| **拡張ステータスLED（固定動作・旧式）** | `TORABO_STATUS_LED_EXT` ／ `torabo-status-led-ext` | BLE プロファイル切替の色フラッシュ＋相方切断中の赤点灯。central 専用・動作固定。`torabo-led-live` の前身で、**同時有効化は不可**（同じ GPIO を奪い合う） |
| **タイミング（タップ反応）** | `ZMK_TIMING_CONFIG` ／ `torabo-timing`（＋ peripheral に `torabo-timing-split`） | キーの「効き」を再フラッシュせず調整。`&mt`（mod_tap）/ `&lt`（layer_tap）の tapping-term・flavor・quick-tap・require-prior-idle と positional 系、および kscan の**デバウンス**（press/release）。アプリ上のタブ名は**「タップ反応」**。設定はノード単位（`&mt` を使う全キーに効く）。デバウンスは相手側に `torabo-timing-split` を載せると左右両方に届く（caps `TIMING_SPLIT_DEBOUNCE`）。**zmk 本体の fork が必須** |
| **USB トンネル（RPC）** | `ZMK_STUDIO_TORABO_TUNNEL` ／ `torabo-rpc-tunnel` | 上記の独自設定を、BLE GATT に加えて **Studio RPC の土管にも流す**汎用トンネル。ワイヤは GATT と 1 バイトも同じ。これが入っていると **USB 接続でも全タブが読み書きでき**、USB 接続の Torabo Float にライブフィードが届く。載っている機能のぶんだけ自動で口が開く。central 専用・**zmk 本体の fork が必須** |
| **ライブフィード（キー/レイヤー通知）** | `ZMK_LIVE_FEED` ／ `torabo-live-feed` | central のキー押下・レイヤー状態・スナップショットを 16 バイトの packed イベントで暗号化 GATT NOTIFY（`e1f4af00`）。トンネル対応 FW なら USB でも届く。PC の [Torabo Float](https://github.com/tak-2025/Torabo-Float) オーバーレイが購読して表示。診断用の第2キャラクタリスティック `e1f4af02` あり。central 専用・未購読時はゼロコスト |
| **予約レイヤー** | `TORABO_RESERVED_LAYERS=N` ／ `torabo-reserved-layers` | 空の予約レイヤーを N 枚（**1〜10、既定 10**）追加。ZMK Studio が実行時に `add_layer` で確保できる空き枠。再フラッシュ不要。枚数は build.yaml の `cmake-args` で指定（conf の CONFIG は申告用） |
| **機能記述子（caps）** | `TORABO_CAPS` ／ `torabo-caps` | 「このFWは何ができるか」（FWバージョン・搭載機能・各機能のワイヤバージョン・機能ビット）を read-only GATT で自己申告。app は接続時にこれを読み、**存在するタブだけ**を表示する。central に載せる |

> トラックパッド・エンコーダ・LED は接続する物理デバイスや central/peripheral の配置で必要な overlay が変わります。
> 手で選ぶ代わりに **[firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/)（上記）** が構成に応じた正しいスニペットの組み合わせを生成します。

---

## 設定・表示に使うソフトウェア

本モジュールと通信する、対になる 2 つの PC アプリがあります。
関連プロジェクトの入口は紹介ポータル **<https://tak-2025.github.io/torabo-fun/>**
（[tak-2025/torabo-fun](https://github.com/tak-2025/torabo-fun)）にまとめてあります。
いずれも [ZMK Project](https://zmk.dev/) とは提携・承認関係にない非公式ツールです。

接続経路は 2 つあります。

- **BLE** — 本モジュールが公開する暗号化 GATT サービスに直接つなぐ。追加の FW 要件なし。
- **USB（RPC トンネル）** — `torabo-rpc-tunnel` を含む FW なら、同じワイヤが Studio RPC
  （USB シリアル）でも読み書きできます。ビルダーはこのスニペットを常に入れます。
  トンネル非対応の古い FW を USB でつなぐと、標準のキーマップ編集だけになります。

> ⚠ **Torabo Studio と Torabo Float の同時利用はできません。** USB はシリアルポートを
> 占有しますし、BLE でもキーボードは一度に 1 つのアプリとしか話せません。
> 片方を閉じてからもう片方をつないでください。

### [Torabo Studio](https://github.com/tak-2025/Torabo-Studio) — 設定・キーマップ編集アプリ

[ZMK Studio](https://github.com/zmkfirmware/zmk-studio) の非公式フォーク。標準のキーマップ／レイヤー編集に加え、
本モジュールの各機能を**再フラッシュせず BLE / USB 経由でライブ編集**するタブを追加します。

- **トラックボール／トラックパッド** — レイヤー・軸ごとの移動／スクロール／速度・ジェスチャ・オートマウスレイヤー。
- **エンコーダ** — 回転（CW/CCW）・押し込みにレイヤーごとの behavior を割当。
- **LED** — 左右の拡張LEDを「条件 → 色・点灯パターン」のルール表で設定（`torabo-led-live`）。
- **マクロ／コンボ** — `&dmac` ダイナミックマクロ・位置コンボを NVS 保存でライブ編集。
- **タップ反応** — `&mt` / `&lt` の tapping-term・flavor などとキースキャンのデバウンスを調整（`torabo-timing`）。
- **バックアップ** — 設定・マクロ・コンボ・キーマップを 1 ファイル（.json）でエクスポート／インポート。
- 接続時に**機能記述子（`torabo-caps`）を読み、そのFWが実際に持つ機能のタブだけ**を表示します。
- **Web 版（<https://tak-2025.github.io/Torabo-Studio/>）が本命**です。WebBluetooth / WebSerial が要るので
  **Chrome / Edge 系ブラウザ限定**（Safari・Firefox では動きません）。
  デスクトップ版（Tauri／Windows・macOS・Linux）もソースにはありますが、
  **ビルド済みバイナリの配布はしていません**（使いたい場合は fork して自分でビルドしてください。
  CI の artifact はリリースではありません）。

ライブ編集を使うには、編集したい機能のスニペット（＋`torabo-caps`）を含めてビルドした FW が必要です。
USB でも独自設定を触りたい場合は `torabo-rpc-tunnel` も必要です。

### [Torabo Float](https://github.com/tak-2025/Torabo-Float) — キー入力オーバーレイ

透過・最前面のフローティングウィンドウに、**いま押しているキー**と**アクティブレイヤー**をリアルタイム表示する
Windows 常駐アプリ（Tauri v2 + React）。配信・操作説明のデモ・多レイヤーキーマップの確認用です。

- キー押下もレイヤー変更も **central だけが知る**情報のため、`torabo-live-feed`（16 バイトの
  packed イベント）から受信します。
  - **BLE では暗号化 GATT の NOTIFY（`e1f4af00`）が基本経路**。RPC に依存しないため、
    **キーボードがロック中でも動作**します。
  - **USB では `torabo-rpc-tunnel` 経由**で同じイベントが push されます（トンネル対応 FW のみ）。
  - 接続の生死やデバイスごとの疎通を見る**診断キャラクタリスティック `e1f4af02`** もあります
    （USB ではトンネルの診断イベントとして届きます）。既定では動かず、購読/有効化したときだけ発火します。
- 使うには `torabo-live-feed` スニペットを含めてビルドした central FW が必要です
  （USB で使うなら `torabo-rpc-tunnel` も）。

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
├── timing/                  # タイミング調整（hold-tap + kscan デバウンス、split 伝搬）
├── live_feed/               # ライブフィード（キー/レイヤーの NOTIFY・診断）
├── caps/                    # 機能記述子（firmware self-description）
├── layers/                  # 予約レイヤー注入
├── snippets/                # 各機能を build に足す snippet 群
├── test/                    # ローカルのビルド確認スクリプト
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

本モジュール（`torabo-tsuki_ext_FW`）は **MIT**（[LICENSE](LICENSE)）です。本体（上流の
[zmk-keyboard-torabo-tsuki-lp](https://github.com/sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp)）は
**GPL-3.0** で、これは `west.yml` から参照するだけでソースを取り込みません。そのため本モジュールの
コード自体は MIT のまま保てます。

トラックパッドを積む構成では、もう1つ GPL-3.0 のコンポーネントが入ります。
**IQS7211E ドライバの fork**（[tak-2025/zmk-driver-iqs7211e](https://github.com/tak-2025/zmk-driver-iqs7211e)
の **`torabo-tsuki` ブランチ**）です。上流
[sekigon-gonnoc/zmk-driver-iqs7211e](https://github.com/sekigon-gonnoc/zmk-driver-iqs7211e)（GPL-3.0）の派生で、
press&hold を `INPUT_BTN_2` として報告するよう改変してあります（Studio の「長押し」割当に必要）。
**ライセンスは GPL-3.0 のまま**で、改変ソースは上記の公開リポジトリでそのまま入手できます。

ただし **ビルドして出来上がるファームウェア（`.uf2`）は、GPL-3.0 のコンポーネントと静的リンクで
結合されるため、結合物全体として GPL-3.0** になります。構成は:

| 部品 | ライセンス | 備考 |
|---|---|---|
| ZMK | MIT | `west.yml` では [tak-2025/zmk](https://github.com/tak-2025/zmk) の `dev`（上流 fork） |
| キーボード本体（[zmk-keyboard-torabo-tsuki-lp](https://github.com/sekigon-gonnoc/zmk-keyboard-torabo-tsuki-lp)） | **GPL-3.0** | fork して使う |
| IQS7211E ドライバ fork（[tak-2025/zmk-driver-iqs7211e](https://github.com/tak-2025/zmk-driver-iqs7211e) `torabo-tsuki`） | **GPL-3.0** | **トラックパッド構成のときだけ**入る |
| 本モジュール（`torabo-tsuki_ext_FW`） | MIT | これ |

MIT は GPL 互換なのでこの結合自体は合法です。`.uf2` を第三者に配布する場合は GPL-3.0 の条件
（対応ソースの入手手段の提供など）に従ってください。**各自が fork してローカルでビルドする分には、
配布が発生しないため GPL の義務は生じません。**

設定・表示アプリ（[Torabo Studio](https://github.com/tak-2025/Torabo-Studio) / [Torabo Float](https://github.com/tak-2025/Torabo-Float)）は
どちらも **BLE / USB 越しにファームと通信する別プログラム**であり、GPL-3.0 のファームを取り込んで
（リンクして）いません。したがって上記の GPL-3.0 は**これらのアプリには及びません**（両者とも Apache-2.0。
Torabo Studio が Apache-2.0 なのは zmk-studio のフォークだからで、本体の GPL とは無関係です）。
