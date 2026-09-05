# スニペット一覧

`build.yaml` の各エントリの `snippet:` に足す部品の一覧。**どれを足すと何が起きるか**の早見表です。

スニペットには2種類あります。

- **機能スニペット** — 機能を有効化して使うもの（マクロ・トラックボール・LED など）。主に **central**（本体を統括し、ホスト／アプリと話す側）に足す。
- **配線スニペット** — デバイスを物理的に繋ぐ土台（I2C・GPIO の overlay）。**トポロジー別**で、単体では「機能」になりません。トラックパッド／エンコーダ／LED は「機能＋配線」の**組**で使います（§4）。

> 構成（central はどちら側か・各側のデバイス）に応じた正しい組み合わせは、
> [firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/) が自動で選びます。この一覧は手で組む人・中身を知りたい人向けです。

**この一覧のスニペットは全部 `torabo-tsuki_ext_FW` に入っていて、`config/west.yml` がこのモジュールを参照した時点で自動的に使えるようになります。**
fork 側にスニペットのファイルをコピーする必要はありません。fork 後に編集するのは次の3ファイルだけです。

| ファイル | いつ編集するか |
|---|---|
| `build.yaml`（ルート） | 毎回（どのスニペットで焼くか） |
| `boards/shields/torabo_tsuki_lp/<central shield>.conf` | 予約レイヤー枚数・デバイス名など、ビルダーが出したときだけ |
| `config/west.yml` | 初回のみ（モジュール参照と、zmk 本体／ドライバの fork 指定） |

⚠ `torabo-rpc-tunnel` を載せる構成（＝ビルダーが出す構成すべて）は、**zmk 本体も fork
（`tak-2025/zmk` の `dev`）に差し替える**必要があります。トンネルの RPC subsystem は
zmk 本体側にあり、上流 v0.3 には存在しないためです。ビルダーの `config/west.yml` 出力が
そのぶんも含んだ形になっているので、そのまま貼り替えてください。既存の
`- name: zmk / remote: zmkfirmware` は**消してから**入れること（同名 project が2つあると
west が Malformed manifest で落ちます）。この fork の `app/west.yml` が `torabo.proto` 入りの
`zmk-studio-messages` fork も連れてくるので、messages を自分で書き足す必要はありません。

---

## 1. 早見表

| snippet | 種別 | 側 | 何をする | 依存 / 排他 |
|---|---|---|---|---|
| `torabo-macros` | 機能 | central | カスタムマクロ `&dmac N`（NVS＋BLEライブ編集）。`dmac` behavior を定義 | — |
| `torabo-combos` | 機能 | central | ダイナミックコンボ（NVS＋BLEライブ編集） | keymap に `zmk,combos` を置かない |
| `torabo-trackball` | 機能 | central | トラックボール設定＋オートマウスレイヤー。listener を `ztc_pointer`/`ztc_temp_layer` に差し替え | — |
| `torabo-trackpad-config` | 機能 | central | トラックパッド設定の**ストア＋GATT のみ**（入力経路の差し替えなし） | 実挙動は別 overlay 依存 |
| `torabo-trackpad-live-ext` | 機能＋配線 | central | 右拡張パッド（device 1）のライブ設定。`tp_pointer`/`tp_keys` を配線 | 土台に `input-trackpad-ext` |
| `torabo-trackpad-live-split` | 機能＋配線 | central | split で届く反対側パッド（device 0）のライブ設定 | 相手側に `input-trackpad-ext-split` |
| `torabo-trackpad` | 機能（固定） | central | ライブ設定なしの**固定 overlay** トラックパッド（音量/スクロール層をハードコード） | 土台に `input-trackpad-ext` |
| `torabo-encoder-live` | 機能 | central | ロータリーエンコーダのライブ設定（回転 behavior＋ボタン processor） | ボタン配線スニペットと併用 |
| `torabo-led-live` | 機能 | central | 拡張LED をルール表で左右別に設定。両LEDを制御し peripheral 分を split で押し込む | 相手側に `torabo-led-ext-periph` |
| `torabo-timing` | 機能 | central | `&mt`/`&lt` の hold-tap パラメータと kscan デバウンスをライブ調整 | zmk fork の `<zmk/torabo_timing.h>` 必須。相手側に `torabo-timing-split` |
| `torabo-timing-split` | 機能 | peripheral | central が送るデバウンス値を自分の kscan に反映（受け側だけ） | 相手側に `torabo-timing` |
| `torabo-reserved-layers` | 機能 | central | 空の予約レイヤーを N 枚追加（Studio が実行時に確保）。無いと LAYER「+」が押せない | 枚数は build.yaml の `cmake-args` |
| `torabo-caps` | 機能（裏方） | central | 機能記述子。FW が構成を自己申告し、アプリがタブを出し分ける | — |
| `torabo-live-feed` | 機能（裏方） | central | 押下キー/レイヤーを BLE NOTIFY で配信（Torabo-Float 用オーバーレイ） | — |
| `torabo-rpc-tunnel` | 機能（裏方） | central | 独自設定を Studio RPC にも流す汎用トンネル。**USB 接続でも全タブが使える** | — |
| `input-trackpad-ext` | 配線 | パッド側 | 拡張トラックパッドの I2C1 土台（IQS7211E＋スクロール慣性） | — |
| `input-trackpad-ext-diy` | 配線 | パッド側 | 上記の DIY 版（別 init シンボル）。`input-trackpad-ext` の上に重ねる。firmware-builder からは生成されません（ビルダーは公式 init のみ対応） | `input-trackpad-ext` |
| `input-trackpad-ext-split` | 配線 | peripheral | 拡張パッドを peripheral 側に繋ぎ split で送る土台 | — |
| `input-trackpad-ext-split-reg1` | 配線 | peripheral | 上記の split レジスタ1版 | — |
| `input-split-listener-reg1` | 配線 | central | peripheral の**2個目**の pointing（reg=1）を受ける。reg=0 版と併用 | 相手に `input-trackpad-ext-split-reg1` |
| `input-encoder` | 配線 | エンコーダ側 | 標準FFC の EC11 回転。sensor 経路なので keymap の `sensor-bindings` で割当 | peripheral に載せたら central に `input-encoder-recv` |
| `input-encoder-ext` | 配線 | エンコーダ側 | 同上の拡張FFC版 | peripheral に載せたら central に `input-encoder-ext-recv` |
| `input-encoder-recv` | 配線 | central | peripheral の標準FFCエンコーダを受ける（device は disabled、sensor index/LEN のみ確保） | 相手に `input-encoder` |
| `input-encoder-ext-recv` | 配線 | central | 同上の拡張FFC版 | 相手に `input-encoder-ext` |
| `torabo-encoder-btn-local` | 配線 | central | エンコーダ押しボタン（central・P0.20） | `torabo-encoder-live` |
| `torabo-encoder-btn-local-ext` | 配線 | central | エンコーダ押しボタン（central・P0.31＝拡張） | `torabo-encoder-live` |
| `torabo-encoder-btn-split` | 配線 | peripheral | エンコーダ押しボタン（peripheral・P0.20）を split 中継 | central に `-recv` |
| `torabo-encoder-btn-split-ext` | 配線 | peripheral | 同上（P0.31＝拡張） | central に `-recv` |
| `torabo-encoder-btn-recv` | 配線 | central | split 中継されたボタンを central 側で受ける（input-split reg 2） | 相手に `-split*` |
| `torabo-led-ext-periph` | 配線 | peripheral | peripheral 側の拡張LED。ルールは持たず central の描画を受ける | central に `torabo-led-live` |
| `torabo-logdiag` | ユーティリティ | 任意 | 起動ログ取りこぼし対策（大バッファ＋ログ処理遅延）。zmk-usb-logging と併用 | — |

---

## 2. 機能スニペット

BLE でライブ編集する機能は、それぞれ暗号化 GATT サービスを1本持ちます（caps=`e1f4a000` / trackpad=`e1f4ac00` / encoder=`e1f4ad00` / led=`e1f4ae00` / live-feed=`e1f4af00` / timing=`e1f4b000`。macros/combos/trackball も専用サービスあり）。

- **`torabo-macros`** — `&dmac N` で呼ぶ NVS 保存マクロ。中身は Studio の「マクロ」タブから BLE 書き込み（再フラッシュ不要）。`&dmac` はレンジ metadata 付きなので、キーへの割当も Studio 上で完結する（keymap の手編集は不要）。
- **`torabo-combos`** — キー位置の同時押しで behavior を発火するコンボを NVS 保存＋ライブ編集。位置コンボの唯一の所有者なので、keymap に `zmk,combos` ノードを置かないこと（二重所有になる）。
- **`torabo-trackball`** — レイヤー/軸ごとの move/scroll/向き/速度と temp-layer（オートマウスレイヤー）をライブ設定。pointing listener を `ztc_pointer` / `ztc_temp_layer` に差し替える。fail-open なので不正設定でも動きが止まらない。
- **`torabo-trackpad-config`** — トラックパッド設定のストアと GATT だけを有効化（入力経路は差し替えない）。ライブ配線込みで使うなら `torabo-trackpad-live-*` を使う。
- **`torabo-trackpad-live-ext` / `-live-split`** — トラックパッドのライブ設定＋入力配線。`-ext` は右拡張パッド（device 1）、`-split` は split 経由で届く反対側パッド（device 0）。v2 ワイヤは1 MTU を超えうるので prepared write（`CONFIG_BT_ATT_PREPARE_COUNT`）も有効化。
- **`torabo-trackpad`** — ライブ設定を使わない固定 overlay 版。レイヤー2＝音量、レイヤー3＝縦スクロール等をハードコード。
- **`torabo-encoder-live`** — ロータリーエンコーダのレイヤーごとの CW/CCW/押し込みをライブ割当。回転は sensor 経路・ボタンは input 経路に乗るため keymap 編集は不要。**ボタン配線スニペットと必ず併用**（§4）。
- **`torabo-led-live`** — 拡張LED を「Xが起きたら色C・パターンP」のルール表で左右別に設定。central が全ルールを保持し、自分のLEDを駆動しつつ peripheral 分を split で押し込む。`CONFIG_ZMK_HID_INDICATORS` も有効化（Caps Lock 表示用）。
- **`torabo-timing`** — キーの「効き」を決める数値をライブ調整。`&mt`（mod_tap）と `&lt`（layer_tap）の tapping-term / flavor / quick-tap / require-prior-idle と positional 系（hold-trigger-key-positions ほか）、および matrix kscan のデバウンス（press/release）。設定は**ノード単位**なので、`&mt` を使う全キーにまとめて効きます。tak-2025/zmk fork の `<zmk/torabo_timing.h>` フック（`__weak`＝既定は従来動作）に強実装を与える形なので、このスニペットを外せば挙動は完全に元通り。デバウンスは相手側に **`torabo-timing-split`** を付ければ左右両方に効きます（付けない場合は central 側のみ）。
- **`torabo-timing-split`** — `torabo-timing` の相方で、**peripheral 行に付けます**。キースキャンは各半身がローカルに回すため、これが無いと central に書いたデバウンス値は反対側に届きません。受け側だけの薄いスニペットで、保存も設定画面も持ちません（central が接続のたびに送り直すので、正は常に1つ）。リンクが繋がるまでの数秒だけ、この半身は devicetree の値で走ります。
- **`torabo-reserved-layers`** — 空の予約レイヤーを N 枚（1〜10、既定10）追加。ZMK Studio が実行時に `add_layer` で確保できる空き枠で、**これが無いと Studio の LAYER「+」が押せません**。枚数は `build.yaml` で指定します:
  ```yaml
    - board: bmp_boost
      shield: torabo_tsuki_lp_right
      snippet: "... torabo-reserved-layers"
      cmake-args: -DDTS_EXTRA_CPPFLAGS="-DTORABO_RESERVED_LAYERS=6"
  ```
  `conf` の `CONFIG_TORABO_RESERVED_LAYERS` は **枚数を作りません**（アプリへ枚数を申告する `torabo-caps` 用）。Zephyr は devicetree を Kconfig より先に処理し、DTS の前処理に `autoconf.h` を渡さないため、overlay の中では `CONFIG_*` が常に未定義になるからです。ビルダーは両方を揃えて出力します。
- **`torabo-caps`** — 「このFWは何ができるか」（バージョン・搭載機能・各機能のワイヤ版・機能ビット）を read-only GATT で自己申告する裏方。アプリは接続時にこれを読み、存在するタブだけを表示する。書き込むものは無い。
  - **相乗り（2026-09-05）**: BLE のバッファ/スタック 7 値（`CONFIG_BT_L2CAP_TX_BUF_COUNT=16` ほか）もこの `.conf` の末尾に入っています。「Studio を BLE で繋いだまま左右 split が張られていると右 central がフリーズする」件の対処で、central に必ず入るスニペットがこれしか無いためです。理由と各値の意味は conf 内のコメントと `docs/COMPATIBILITY.md` §7 に。RAM を約 11KB 使います。`CONFIG_BT_CTLR_DATA_LENGTH_MAX` は **244 が上限**（251 にすると Studio の RPC 応答が ATT MTU 超過で落ちます）。
- **`torabo-live-feed`** — 押下キーとレイヤーを BLE NOTIFY で配信し、デスクトップオーバーレイ Torabo-Float がリアルタイム表示する。クライアントが購読中のみ発火するので未接続なら無コスト。central 専用（レイヤー状態と全体キー位置を知るのは central だけ）。
- **`torabo-rpc-tunnel`** — 独自設定（トラックボール/トラックパッド/マクロ/コンボ/エンコーダ/LED/ライブフィード）を、BLE GATT サービスに加えて **Studio RPC の土管にも流す**。ワイヤは GATT と 1 バイトも変わらないので、アプリ側のコーデックはそのまま使い回せる。これが入っていると **USB 接続でも全タブが読み書きでき**、USB 接続の Torabo-Float にライブフィードが届く。
  - 入れるのは central だけ（RPC の実体は central にしかない）。GATT 側は無変更のまま残るので、既存の BLE クライアントには一切影響しない。
  - この 1 行だけで足りる。各機能の口は `CONFIG_ZMK_*_TUNNEL`（`default y`、このスニペットに `depends on`）なので、載っている機能のぶんだけ自動で開き、載っていない機能は勝手に消える。
  - `torabo-caps` の記述子に `RpcTunnel`（機能 id 9）が増え、アプリはこれを見て経路の有無を判定する。
  - コスト実測（トラックボール central）: FLASH +約1KB / RAM +約6KB（読み出し用 2KB のステージングバッファと、リクエストをその場でデコードするぶん大きくした RPC スレッドスタック）。

---

## 3. 配線スニペット（トポロジー別の土台）

デバイスをハードに繋ぐだけの overlay。単体では機能になりません。

### トラックパッドの土台
- **`input-trackpad-ext`** — 拡張トラックパッド（IQS7211E）を I2C1（SDA P0.17 / SCL P0.21）で接続。スクロール慣性込み。
- **`input-trackpad-ext-diy`** — 上記の DIY 版（別 init シンボルを使う）。`input-trackpad-ext` の上に重ねる。
  firmware-builder からは生成されません（ビルダーは公式 init のみ対応。使う場合は手で `build.yaml` に足してください）。
- **`input-trackpad-ext-split` / `-split-reg1`** — 拡張パッドを peripheral 側に繋ぎ split で central へ送る土台。

### エンコーダのボタン配線
エンコーダの**回転**は `torabo-encoder-live` が扱い、**押しボタン**はこの配線スニペットが GPIO を定義します。トポロジーで使い分け:

| ボタンの位置 | ピン | 使うスニペット | central 側で必要 |
|---|---|---|---|
| central | P0.20 | `torabo-encoder-btn-local` | — |
| central（拡張） | P0.31 | `torabo-encoder-btn-local-ext` | — |
| peripheral | P0.20 | `torabo-encoder-btn-split` | `torabo-encoder-btn-recv` |
| peripheral（拡張） | P0.31 | `torabo-encoder-btn-split-ext` | `torabo-encoder-btn-recv` |

peripheral 側にボタンがある場合だけ、central に `torabo-encoder-btn-recv`（split 受信）を足します。

### LED（peripheral 側）
- **`torabo-led-ext-periph`** — peripheral 側の拡張LED（GPIO P1.11/P1.10/P1.02）。ルールは持たず、central が `torabo-led-live` で描画した (色, パターン) を split で受けて光る。

---

## 4. 「機能＋配線」の組で使うもの

トラックパッド・エンコーダ・LED は、**機能スニペット単体では動きません**。物理配線とセットです。

- **トラックパッド** = 土台（`input-trackpad-ext*`）＋ 機能（`torabo-trackpad` か `torabo-trackpad-live-*`）
- **エンコーダ** = 回転（`torabo-encoder-live`）＋ ボタン配線（`torabo-encoder-btn-*`、split なら central に `-recv`）
- **拡張LED** = central（`torabo-led-live`）＋ peripheral 側があれば（`torabo-led-ext-periph`）

この組み合わせと左右の割り当ては構成ごとに変わります。手で間違えやすいので、迷ったら
[firmware-builder](https://tak-2025.github.io/torabo-tsuki_ext_FW/) に生成させてください（対応パターンは
[`firmware-builder/PATTERN-MATRIX.md`](../firmware-builder/PATTERN-MATRIX.md)）。

---

## 5. ユーティリティ

- **`torabo-logdiag`** — 起動直後のログ取りこぼし対策。ドライバの早期初期化（例: IQS7211E は POST_KERNEL で ~0.5–1.5s）は USB CDC 接続前に走り、既定 1KB のログバッファから溢れる。ログバッファを 32KB に拡大し、ログ処理スレッドを 8 秒遅延させて、ホストがポートを開くまで何も捨てない。`zmk-usb-logging` と併用。
