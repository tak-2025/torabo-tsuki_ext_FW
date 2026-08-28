# torabo-tsuki ファームウェアビルダー (v1)

**→ <https://tak-2025.github.io/torabo-tsuki_ext_FW/>**

ブラウザで開くだけで使えます（ビルド・インストール不要）。オフラインで使いたい場合は
[`index.html`](index.html) をダウンロードしてダブルクリックでも同じものが動きます（外部通信ゼロの単一 HTML）。

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
     ライブフィード（Torabo-Float 用）・予約レイヤー枚数。
3. 右側に出る生成物を、fork の各ファイルへコピペで置き換える。
   **編集するのはこの3つだけです**（スニペットの実体は `torabo-tsuki_ext_FW` 側にあるので、fork にファイルを足す必要はありません）:
   - `build.yaml`（ルート）
   - `boards/shields/torabo_tsuki_lp/<central shield>.conf`（予約レイヤーの枚数指定・デバイス名長超過時のみ）
   - `config/west.yml`（torabo-* 機能を使う初回のみ。既存の remotes/projects の末尾に追記する形）
     トラックパッドを積む構成では、既存の `zmk-driver-iqs7211e` エントリを
     `tak-2025 / torabo-tsuki`（press&hold 対応の fork。GPL-3.0 のまま）に**置き換え**ます。
     上流のままだとビルドは通るのに Studio の「長押し」割当だけが無反応になります。
4. push して GitHub Actions、またはローカル `west build`（[../../tako-custom/BUILD.md](../../tako-custom) 参照）でビルド。

## このツールの位置づけ

- これは「どの機能をFWに**焼くか**」と「どのスニペットの組み合わせでビルドするか」を決めるもの。
  値（コンボ内容・速度・LEDルールなど）は焼いた後に **BLE でライブ編集**します（[Torabo-Studio](https://github.com/tak-2025/Torabo-Studio)）。
- 焼く機能そのものを変える／ハード構成を変えるときだけ**再ビルド＋再フラッシュ**が必要です。
- 構成に応じた overlay/CONFIG の組み合わせ（トポロジー）を自動で解決するので、手作業のスニペット選定を省けます。
  対応する物理パターンの全列挙・buildability の source-of-truth は [`PATTERN-MATRIX.md`](PATTERN-MATRIX.md)。

## ビルド可否バッジ（Tier）

出力の先頭に、選んだ構成がいまビルドできるかのバッジが出ます。

- **BUILDABLE-NOW（今すぐ焼ける）** — 必要なスニペットがすべて実装済み。
- **FW未実装に依存（ビルド不可）** — 一部の物理構成は対応スニペットが未実装で、まだビルドできません。
  どの依存が足りないかは [`PATTERN-MATRIX.md` §5](PATTERN-MATRIX.md) の backlog を参照。

## 対応している機能・構成（v1）

v0 から拡張され、以下が実装済みです（いずれも `default n`、スニペットで個別に ON/OFF）:

- **ポインティングデバイス** — トラックボール／ミニトラックパッド（標準FFC・拡張FFC）。
  central 直結・peripheral からの split 中継（拡張パッドの split エクスポート含む、reg=0 / reg=1）に対応。
- **ロータリーエンコーダ** — 回転（標準/拡張FFC）＋ボタン押下を、central 直結・peripheral split の両方で。
  回転は sensor 経路、ボタンは input 経路に乗るため keymap/transform の改造は不要。レイヤーごとの割当は Studio でライブ編集。
- **トラックボール／トラックパッド設定** — レイヤー/軸ごとの move・scroll・速度・離散エンコーダ役割などを BLE ライブ設定。
- **拡張ステータスLED** — 従来固定動作（`torabo-status-led-ext`）／ルール設定式ライブ（`torabo-led-live`＋`torabo-led-ext-periph`）を選択（両者は排他）。
- **カスタムマクロ（`&dmac`）／ダイナミックコンボ** — NVS 保存＋BLE ライブ編集。
- **ライブフィード（`torabo-live-feed`）** — キー押下・レイヤー状態を BLE NOTIFY で PC の [Torabo-Float](https://github.com/tak-2025/Torabo-Float) オーバーレイへ push。central 専用・未購読時ゼロコスト。
- **機能記述子（`torabo-caps`）** — 「このFWは何ができるか」を read-only GATT で自己申告。Torabo-Studio が接続時に読み、**存在する機能のタブだけ**を表示する。
- **予約レイヤー** — 空レイヤーを N 枚（0〜10）追加（ZMK Studio の実行時 add_layer 用の空き枠）。

## 仕組み（拡張ポイント）

`index.html` 内の **snippet レジストリ**（`SNIP` / トポロジー合成ロジック）と buildability 分類器で全出力が決まります。
これは [`PATTERN-MATRIX.md`](PATTERN-MATRIX.md) §0/§2/§3 と一対一で対応しており、機能や配線を増やすときは
両方を合わせて更新します。ロジックは将来 Torabo-Studio 側のタブへ移植できる構造です。

## v1 時点の注意

- 一部の物理構成（一部のエンコーダ配線・reg=1 の二重ポインティング等）は**未実機検証**、または依存スニペット未実装で
  「ビルド不可」バッジになります。詳細は [`PATTERN-MATRIX.md` §5 backlog](PATTERN-MATRIX.md)。
- マクロの `&dmac N` 配置は手動（keymap 編集は別途）。トラックボール/パッド/エンコーダ/LED/予約レイヤーは keymap 改造不要。
- **生成AIを用いて作成しており、動作を保証しません。** 各自の構成で必ず動作確認のうえご利用ください。
