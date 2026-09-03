# torabo-tsuki ファームウェアビルダー (v1)

**→ <https://tak-2025.github.io/torabo-tsuki_ext_FW/>**

ブラウザで開くだけで使えます（ビルド・インストール不要）。オフラインで使いたい場合は
[`index.html`](index.html) をダウンロードしてダブルクリックでも同じものが動きます（外部通信ゼロの単一 HTML）。

> **はじめての方へ** — 全体像・関連プロジェクトの入口は紹介ポータル
> **<https://tak-2025.github.io/torabo-fun/>**（[tak-2025/torabo-fun](https://github.com/tak-2025/torabo-fun)）にまとめてあります。

各サイドの**現物のハードウェア構成**（標準FFC・拡張基盤・拡張FFC に付けるデバイス）と、
使いたい**機能**（トラックボール/トラックパッド設定・マクロ・コンボ・ライブフィード・予約レイヤーなど）を
画面で選ぶと、fork に置き換える `build.yaml` / `boards/shields/torabo_tsuki_lp/<shield>.conf` / `config/west.yml`
を生成する**ビルド時の機能登録ジェネレータ**です。

## 使い方

1. 上記の URL をブラウザで開く。
2. 上から順に選ぶ:
   - **構成** — central（トラックボール／設定を統括する側）が左右どちらか。
   - **デバイス名** — ペアリング時に表示される BLE/USB 名。
   - **ハードウェア構成（各サイド）** — 標準FFC 1本 と（拡張基盤を付ければ）拡張FFC 1本に付ける現物。
     標準FFCは1デバイス排他（ボール/パッド/エンコーダは同じピンを奪い合う）。トラックボールは標準FFCのみ（拡張にSPIなし）。
     拡張基盤が FPC+LED なら拡張LEDの使用可否と制御方式（従来固定 / ライブ設定）も選べます。
   - **機能（central に登録）** — トラックボール設定・マクロ・コンボ・トラックパッドのライブ機能割当・
     ライブフィード（Torabo Float 用）・予約レイヤー枚数。
3. 右側に出る生成物を、fork の各ファイルへコピペで置き換える。
   **編集するのはこの3つだけです**（スニペットの実体は `torabo-tsuki_ext_FW` 側にあるので、fork にファイルを足す必要はありません）:
   - `build.yaml`（ルート）
   - `boards/shields/torabo_tsuki_lp/<central shield>.conf`
     — 予約レイヤーの枚数申告・デバイス名・拡張LEDの実在フラグ・トラックパッドのデバイス素性メタ。
     いずれかが必要な構成のときだけ中身が出ます（不要なら「追記は不要」と表示されます）。
   - `config/west.yml`（初回のみ）。**追記だけでは足りません。**`remotes:` への `tak-2025` 追加と
     `torabo-tsuki_ext_FW` の追記に加えて、**既存エントリの置き換えが 1〜2 か所**あります:

     ```yaml
     # zmk 本体 — 既存の  - name: zmk / remote: zmkfirmware / revision: v0.3  を削除して置き換え。
     # 常時有効の torabo-rpc-tunnel（RPC subsystem）と torabo-timing
     # （<zmk/torabo_timing.h> のフック）が zmk 本体側の実体を要求するため、上流 v0.3 では
     # 必ずビルドエラーになります。この fork が torabo.proto 入りの zmk-studio-messages も
     # 連れてくるので、messages を自分で書き足す必要はありません。
     - name: zmk
       remote: tak-2025
       revision: dev
       import: app/west.yml

     # IQS7211E ドライバ — トラックパッドを積む構成のみ。
     # 既存の  - name: zmk-driver-iqs7211e / remote: sekigon-gonnoc  を削除して置き換え。
     # press&hold 対応の fork（GPL-3.0 のまま）。上流のままだとビルドは通るのに
     # Studio の「長押し」割当だけが無反応になります。
     - name: zmk-driver-iqs7211e
       remote: tak-2025
       revision: torabo-tsuki
     ```

     > ⚠ **同名の project を2つ書かないこと。** 既存行を消さずに足すと west が
     > `Malformed manifest`（*used twice*）で落ちます。
4. push して GitHub Actions でビルド（ローカルに Zephyr / west の環境がある人は `west build` でも同じものが作れます）。

## このツールの位置づけ

- これは「どの機能をFWに**焼くか**」と「どのスニペットの組み合わせでビルドするか」を決めるもの。
  値（コンボ内容・速度・LEDルール・キーの効きなど）は焼いた後に **BLE / USB でライブ編集**します
  （[Torabo Studio](https://github.com/tak-2025/Torabo-Studio)）。
- 焼く機能そのものを変える／ハード構成を変えるときだけ**再ビルド＋再フラッシュ**が必要です。
- 構成に応じた overlay/CONFIG の組み合わせ（トポロジー）を自動で解決するので、手作業のスニペット選定を省けます。
  対応する物理パターンの全列挙・buildability の source-of-truth は [`PATTERN-MATRIX.md`](PATTERN-MATRIX.md)。

## ビルド可否バッジ（Tier）

出力の先頭に、選んだ構成がいまビルドできるかのバッジが出ます。

**現時点では、UI で作れるすべての構成が BUILDABLE-NOW（今すぐ焼ける）です。**
バックログにあった未実装スニペット（拡張FFCエンコーダ・エンコーダボタン・reg=1 版一式など）は
すべて実装済みで、「FW未実装に依存（ビルド不可）」バッジが出る組み合わせはもうありません
（バッジの仕組みは、将来また未実装の依存が増えたときのために残してあります）。

> ⚠ **ビルドできる＝実機検証済み、ではありません。** 多くの構成は**未実機検証**です。
> 検証状況は [`PATTERN-MATRIX.md`](PATTERN-MATRIX.md) を参照してください。

## 対応している機能・構成（v1）

v0 から拡張され、以下が実装済みです（いずれも `default n`、スニペットで個別に ON/OFF）:

- **ポインティングデバイス** — トラックボール／ミニトラックパッド（標準FFC・拡張FFC）。
  central 直結・peripheral からの split 中継（拡張パッドの split エクスポート含む、reg=0 / reg=1）に対応。
- **ロータリーエンコーダ** — 回転（標準/拡張FFC）＋ボタン押下を、central 直結・peripheral split の両方で。
  回転は sensor 経路、ボタンは input 経路に乗るため keymap/transform の改造は不要。レイヤーごとの割当は Studio でライブ編集。
- **トラックボール／トラックパッド設定** — レイヤー/軸ごとの move・scroll・速度・離散エンコーダ役割・慣性スクロール（coast）などを BLE / USB でライブ設定。
- **拡張ステータスLED** — ルール設定式（`torabo-led-live`＋相手側に `torabo-led-ext-periph`）。Studio の LED タブでライブ編集。
- **カスタムマクロ（`&dmac`）／ダイナミックコンボ** — NVS 保存＋ライブ編集。
- **ライブフィード（`torabo-live-feed`）** — キー押下・レイヤー状態を PC の [Torabo Float](https://github.com/tak-2025/Torabo-Float) オーバーレイへ push（BLE NOTIFY／トンネル対応 FW なら USB でも）。central 専用・未購読時ゼロコスト。
- **予約レイヤー** — 空レイヤーを N 枚（1〜10、既定 10）追加（ZMK Studio の実行時 add_layer 用の空き枠）。
  枚数は `build.yaml` の `cmake-args`（本体）と `.conf` の `CONFIG_TORABO_RESERVED_LAYERS`（申告用）に
  同じ値で出力されます。**両方必要**です。

**チェックボックスに出ない「常時有効」の3つ**（構成によらず必ず出力されます）:

- **機能記述子（`torabo-caps`）** — 「このFWは何ができるか」を read-only GATT で自己申告。
  Torabo Studio が接続時に読み、**存在する機能のタブだけ**を表示する。
- **USB トンネル（`torabo-rpc-tunnel`）** — 独自設定を Studio RPC にも流す汎用トンネル。
  これがあると **USB 接続でも全タブが読み書きでき**、USB 接続の Torabo Float にライブフィードが届く。
  BLE GATT は無変更のまま並存。載っている機能のぶんだけ自動で口が開きます。central 専用。
- **タイミング／タップ反応（`torabo-timing` ＋ peripheral の `torabo-timing-split`）** —
  `&mt` / `&lt` の tapping-term・flavor・quick-tap・require-prior-idle と positional 系、
  および kscan のデバウンスを Studio からライブ調整。central に本体、peripheral に受け側を
  対で出力するので、デバウンスは**左右どちらの半身にも**効きます。

後ろ2つは **zmk 本体の fork（`tak-2025/zmk` の `dev`）が必須**です。ビルダーの `config/west.yml`
出力にその置き換えも含まれています。

## 仕組み（拡張ポイント）

`index.html` 内の **snippet レジストリ**（`SNIP` / トポロジー合成ロジック）と buildability 分類器で全出力が決まります。
これは [`PATTERN-MATRIX.md`](PATTERN-MATRIX.md) §0/§2/§3 と一対一で対応しており、機能や配線を増やすときは
両方を合わせて更新します。ロジックは将来 Torabo Studio 側のタブへ移植できる構造です。

## v1 時点の注意

- **UI で作れる構成はすべてビルドできますが、多くは未実機検証です。**
  特に一部のエンコーダ配線・reg=1 の二重ポインティング・拡張LEDのライブ設定は実装のみで、
  実機での確認が済んでいません。詳細は [`PATTERN-MATRIX.md`](PATTERN-MATRIX.md)。
- マクロの `&dmac N` 配置は手動（keymap 編集は別途）。トラックボール/パッド/エンコーダ/LED/予約レイヤーは keymap 改造不要。
- **生成AIを用いて作成しており、動作を保証しません。** 各自の構成で必ず動作確認のうえご利用ください。
