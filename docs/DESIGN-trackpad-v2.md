# 設計 v2 — torabo-tsuki トラックパッド 拡張（タップ/ジェスチャ割当・任意キー割当）

> v1（[DESIGN-trackpad.md](DESIGN-trackpad.md)）＝レイヤー×デバイスごとに **軸(X/Y)** の役割（Move/Scroll/Off/Volume/Brightness/Zoom/Browser）を割当。実装済み・左パッド動作確認済み。
> **v2 で追加する2軸**：
> 1. **タップ/ジェスチャ動作設定** … 単タップ・2本指タップ・長押し（＋任意でダブルタップ）に、レイヤー×デバイスごとの任意アクションを割当。
> 2. **任意キー割り当て** … 離散役割・ジェスチャの発火先を、固定 consumer コードから **任意 behavior＋任意キーコード** に拡張。
>
> v1 の安全鉄則（フェイルオープン・検証付き固定長wire・ロックレス公開・入力スレッド非ブロッキング）を**そのまま継承**。既存 v1 wire は後方互換で受理する。
> **実装はこの設計の合意後に着手する。**

---

## 0. リソース前提 — 「マイコンの限界」への回答

コア = **nRF52840**（`nordic,nrf52840-qiaa`）: **Flash 1MB / RAM 256KB**。

| 項目 | 現状(v1) | v2 追加見積り | 残余 |
|---|---|---|---|
| 設定スナップショット RAM | ~400B（4dev×8層×ダブルバッファ） | +ジェスチャ次元・binding descriptor で ~2倍 → **~1KB** | 256KB に対し誤差 |
| wire blob（GATT 1本） | ~60B | ジェスチャ＋descriptor 化で **~150〜250B** | ATT 512B / MTU 内。分割不要 |
| コード flash | 数KB | tp_keys processor＋descriptor 発火で **+2〜4KB** | 1MB に対し誤差 |
| NVS 設定 | 数十B | 上記 wire 相当 | 設定パーティション内 |

**結論：ハード資源は全く問題なし。まだ数倍の機能を盛れる。** v2 の実質的な制約は (a) BLE GATT の1 characteristic サイズ（数百Bまで余裕）、(b) ZMK の behavior がデバイスツリー静的である点＝§4 の「実行時 binding 合成」で回避、(c) 入力スレッドの非ブロッキング＝既存 msgq/work 作法を踏襲、の3点のみ。いずれも設計側の話でハード限界ではない。

---

## 1. スコープ / 追加する次元（v2）

### 1.1 ジェスチャ次元（新規）

IQS7211E ドライバ（[iqs7211e.c](https://github.com/tak-2025/zmk-driver-iqs7211e/blob/torabo-tsuki/src/iqs7211e.c)）は**ジェスチャ検出を既に実装済み**で、現状は固定でボタンを報告している：

| ドライバ現状 | 出力 | v2 での扱い |
|---|---|---|
| 単タップ | `INPUT_BTN_0`（左click） | **設定可能アクション**へ差し替え |
| 2本指タップ | `INPUT_BTN_1`（右click） | **設定可能アクション**へ差し替え |
| ダブルタップ＆ホールド | ドラッグ（BTN_0 押しっぱ） | 既定維持（ドラッグは有用）。任意でアクション化 |
| 長押し(press&hold, gestureビット3) | ―（現状未マップ） | **設定可能アクション**を新設 |

v2 のジェスチャ役割セット（gesture slot）：

| slot | トリガ | 既定 |
|---|---|---|
| `GST_TAP` | 単タップ | 左click（BTN_0）※現状維持 |
| `GST_TAP2` | 2本指タップ | 右click（BTN_1）※現状維持 |
| `GST_HOLD` | 長押し | 何もしない（`&none`） |
| `GST_DTAP`（任意/後段） | ダブルタップ単独 | 何もしない（ドライバ小追加が必要、§4.4） |

各 slot はレイヤー×デバイスごとに **1つの binding descriptor（§2.2）** を持つ。フェイルオープン：未設定＝ドライバ既定（tap→左click, tap2→右click）を素通し。

### 1.2 任意キー割り当て（軸・ジェスチャ共通）

v1 の離散役割（Volume/Brightness/Zoom/Browser）は「固定 consumer コードを +/- で発火」だった。v2 では発火先を **binding descriptor** に一般化し、次を割当可能にする：

- `&kp <keycode|mods+keycode>`（任意キー・修飾付き）
- `&cp <consumer>`（音量/輝度/再生/ブラウザ等の consumer コード）
- `&mo/&to/&tog <layer>`（レイヤー操作）
- `&none`（無効）

v1 の Volume/Brightness/Zoom/Browser は「よく使う descriptor のプリセット」に降格でき、UI ではワンクリック選択肢として残す（後方互換・使い勝手のため）。

---

## 2. データモデル

### 2.1 軸設定（v2：離散役割を ENCODER に一般化）

ミニパッドは実質1軸＝縦スワイプなので、離散動作は必ず**スワイプ上/下の2方向**を持つ。v1 は固定役割（Volume/…）ごとに +/- 2発火だった。v2 では役割を集約し、**方向ごとに任意 binding を持たせる**：

```c
struct tp_axis {                 // 3B + 8B = 11B
    uint8_t role;                // MOVE / SCROLL / OFF / ENCODER
    uint8_t direction;           // 0 normal, 1 reverse（pos/neg を入替）
    uint8_t step;                // 分周(連続) / 積算閾値(ENCODER) 1..32
    struct tp_binding bind_pos;  // 正方向スワイプで発火（§2.2）
    struct tp_binding bind_neg;  // 負方向スワイプで発火
};
enum tp_role_v2 { TP_ROLE_MOVE=0, TP_ROLE_SCROLL=1, TP_ROLE_OFF=2, TP_ROLE_ENCODER=3 };
```

- 連続系（MOVE/SCROLL/OFF）は bind_pos/neg を無視（NONE）。
- **v1 の Volume/Brightness/Zoom/Browser は「プリセット」＝アプリ側の概念に降格**：選ぶと `role=ENCODER` かつ pos/neg を既定 descriptor で埋めるだけ（例 Volume → pos=`&cp C_VOL_UP`, neg=`&cp C_VOL_DN`）。FW は特別扱いせず ENCODER として pos/neg を発火するので、**任意キー割当がそのまま効く**（例 上=PageUp / 下=PageDown）。
- v1 wire（role 3〜6）復号時は上記マッピングで pos/neg を補完（§3 後方互換）。

### 2.2 binding descriptor（新規・共通型）

発火先1つを表す固定長レコード。**アプリとFWの唯一の契約**。

```c
struct tp_binding {          // 4B
    uint8_t  behavior;       // enum tp_behavior（どの ZMK behavior か）
    uint8_t  _rsv;           // 予約（アライン/将来 mods 拡張）
    uint16_t param;          // param1（keycode / consumer / layer 番号）LE
};

enum tp_behavior {           // FW の behavior 参照テーブル index（§4.2）
    TP_BEH_NONE = 0,         // &none（何もしない）
    TP_BEH_KP   = 1,         // &kp   param=HID keycode(修飾は上位ビット規約 §4.3)
    TP_BEH_CP   = 2,         // &cp   param=consumer usage
    TP_BEH_MO   = 3,         // &mo   param=layer
    TP_BEH_TO   = 4,         // &to   param=layer
    TP_BEH_TOG  = 5,         // &tog  param=layer
};
#define TP_BEH_MAX TP_BEH_TOG
```

不明 behavior / 範囲外 param ＝ `TP_BEH_NONE` 扱い（フェイルオープン）。

### 2.3 ジェスチャ設定（新規）

デバイス×レイヤーごとに gesture slot 配列を持つ：

```c
struct tp_gestures {                 // slot 数 × 4B
    struct tp_binding tap;           // GST_TAP
    struct tp_binding tap2;          // GST_TAP2
    struct tp_binding hold;          // GST_HOLD
    // struct tp_binding dtap;       // GST_DTAP（P?で追加、§4.4）
};
```

まとめると v2 のデバイス設定は「レイヤーごとに { 軸X, 軸Y, ジェスチャ } 」。

---

## 3. wire protocol v2（packed・LE・固定長・versioned）

**version を 1→2 に上げ**、magic は据え置き（`0x7470`）。長さは手計算で検証（v1 と同じ規律）。

```
Header (6B): magic u16=0x7470, version u8=2, device_count u8, layer_count u8, flags u8
  flags bit0 = ジェスチャ節あり(1) / なし(0)   ← 将来の節追加も flags で表現
device_count 回:
  device_id u8, _rsv u8                                              (2B)
  layers[layer_count]:
    axis x { role u8, dir u8, step u8, bind_pos(4B), bind_neg(4B) }  (11B)
    axis y { 同上 }                                                   (11B)
    gesture { tap(4B), tap2(4B), hold(4B) }                          (12B)  ← flags bit0=1 のとき
  → 1層 = 22 + 12 = 34B（ジェスチャ有）/ 22B（無）
tp_binding(4B) = behavior u8, _rsv/mods u8, param u16(LE)
長さ = 6 + device_count*(2 + layer_count*layer_stride)
```

> **サイズ注意**：2デバイス×多レイヤーで wire は数百B に達し得る（例 2dev×5層ジェスチャ有 = 6+2*(2+5*34)=350B）。現行 GATT WRITE は `offset!=0` を拒否＝単発 write のみ（[gatt_service.c](../features/trackpad/src/gatt_service.c)）なので MTU を超えると書けない。→ **§4.5 で GATT に Write Long（offset 分割受信）対応を追加**し、サイズ上限を撤廃する（flash1回方針なので同時に入れる）。READ は既に offset 対応済み。

**後方互換の要**：
- FW は READ で **常に version=2** を返す（現行値を v2 形で表現）。
- WRITE 受理時、`version==1` なら **v1 レイアウトとして復号**し、軸の binding は「役割から導出した既定 descriptor」で補完（＝旧アプリ／旧バックアップがそのまま通る）。`version==2` は v2 で復号。それ以外・長さ不一致・magic 不一致は**全拒否→既定維持**。
- 各値クランプ：role≤6、behavior≤`TP_BEH_MAX`、step 1..32、layer<layer_count。範囲外は安全側（軸→MOVE、descriptor→NONE）。

TS/C 同一定義（[tpConfig.ts](https://github.com/tak-2025/Torabo-Studio/blob/main/src/trackpad/tpConfig.ts) を v2 拡張。`decodeTp` は version 分岐、`encodeTp` は常に v2 出力）。

---

## 4. FW 設計

### 4.1 tp_keys — ジェスチャ用リマップ processor（新規）

[tp_pointer.c](../features/trackpad/src/tp_pointer.c) の兄弟。`INPUT_EV_KEY`（`BTN_0`/`BTN_1` 等）を対象に、`device-id` と現在レイヤーから gesture slot を引き、対応する **binding descriptor を実行時 binding 化して発火**（§4.2）、元イベントは STOP で消費。フェイルオープン：slot 未設定 or descriptor=NONE なら **CONTINUE**（＝ドライバ既定の click を素通し）。発火は tp_pointer と同じ **msgq→system workqueue**（入力スレッド＝BLE RX 文脈をブロックしない）。

- 押下/離しの対応：タップ系は press→release を1回（既存 `tp_tap_work_cb` を流用）。長押し(hold)は「押し続け→指離しで release」を扱えるよう、hold は down/up をイベント値に追従させる（BTN の 1/0 を binding の press/release に橋渡し）。
- listener への挿し位置：`&pointing_listener*` の base processor に `<&tp_pointer …>, <&tp_keys …>` の順で追加。tp_pointer は REL のみ、tp_keys は KEY のみを見るので競合しない。

### 4.2 実行時 binding 合成（任意キーの肝）

ZMK の `struct zmk_behavior_binding` は値型 `{ const char* behavior_dev; uint32_t param1; uint32_t param2; }`。DT で標準 behavior を一度だけ参照し、その `device` 名を保持したテーブルを持てば、**任意 param の binding を実行時に構築して `zmk_behavior_invoke_binding` へ渡せる**（Studio のキーマップ編集と同一原理。パレット方式より自由度が高く、実装も軽い）。

```c
/* overlay 側（snippet）: 参照だけ確保 */
tp_keys: … { behaviors = <&none &kp &cp &mo &to &tog>; };
/* C 側: descriptor -> binding */
static struct zmk_behavior_binding tp_make(const struct tp_binding *d){
    struct zmk_behavior_binding b = { .behavior_dev = beh_table[d->behavior], .param1 = d->param };
    return b; /* NONE は beh_table[0]=&none。範囲外は NONE に矯正済み */
}
```

`beh_table` は `enum tp_behavior` 順の `const char*`（`DEVICE_DT_NAME`）。範囲外 index は index 0（none）へ矯正。

### 4.3 修飾キー規約（&kp）

`param` u16 に「上位=修飾ビット / 下位=HID usage」を載せるか、`_rsv` バイトを mods に転用する（`LC/LS/LA/LG…`）。ZMK の `ZMK_HID_USAGE` と mod の合成規約に合わせる。**決定事項**：v2 は `_rsv` を `mods` バイトに転用し、`&kp (mods<<24 | usage)` 相当を組む（param1 のエンコードは ZMK `&kp` の実装に一致させる）。→ 実装時に ZMK 版の `kp` param エンコードを確認して確定。

### 4.4 ダブルタップ単独（GST_DTAP・tp_keys 内で検出、ドライバ非改修）

**方針変更**：vendored driver（`zmk-driver-iqs7211e`）は触らない。ダブルタップは **tp_keys が単タップ（`INPUT_BTN_0`）の時間窓連続を自前検出**する（driver は単タップを従来どおり報告するだけ）。

- tp_keys は per-device に「直近タップ時刻」を持ち、`GST_DTAP` 窓（既定 ~250ms、Kconfig 可）内の2連タップを DTAP とみなす。
- **単タップ確定の遅延**：その layer/device に **DTAP binding が設定されている時だけ**、単タップの発火を DTAP 窓ぶん遅延（2発目が来なければ単タップ、来れば DTAP）。DTAP 未設定なら**遅延なし**で従来の即クリック（＝レイテンシ影響を設定時のみに限定）。
- 遅延発火は既存の msgq/`k_work_delayable` で行い、入力スレッドをブロックしない。ドラッグ（driver の double_tap_hold）は driver 側で完結しており DTAP 検出と独立。
- フェイルオープン：DTAP 判定に迷ったら単タップとして扱う（アクションを失わない）。

→ DTAP は **ドライバ改修なし**、自前モジュール内で完結（BTN_0 の時間窓検出）。

### 4.6 GST_HOLD（press&hold → BTN_2、ドライバ小改修）

**方針変更（2026-07-10 合意）**：HOLD は「指を押したまま保持」を表すが、vendored driver は BTN_0/BTN_1（タップ）しか出さず「押しっぱなし」イベントが無い。tp_keys は KEY イベントしか見えないので自前検出は不可 → **ドライバに press&hold 検出を小追加**する（flash1回方針なので同時に入れる）。

- `zmk-driver-iqs7211e/src/iqs7211e.c` に `k_work_delayable hold_work` を追加（既存 `click_work` と同流儀・同じ system wq なので motion_work と直列＝競合なし）。
- 単指タッチ開始で `IQS7211E_HOLD_MS`(既定350ms) 後に予約 → 発火時に単指が残っていれば `INPUT_BTN_2` press、指離しで release。移動が `IQS7211E_HOLD_MOVE_MAX`(60) 超なら（hold前は）キャンセル＝通常の移動/ドラッグに。click/`double_tap_hold`(既存ドラッグ) 中は発火しない＝**既存のタップ/スクロール/ドラッグに無干渉（フェイルセーフ）**。
- tp_keys が `INPUT_BTN_2` を GST_HOLD binding に down/up 追従でマップ（§4.1）。
- **解決済（2026-08-15）**：改変はワークスペース内の未コミット差分としてしか存在せず、`west update` で消える／CI ビルドに入らない／GPL-3.0 の改変ソース提供義務、の3点が同時に問題だった。**`tak-2025/zmk-driver-iqs7211e` の `torabo-tsuki` ブランチに fork（GPL-3.0 のまま）してコミット済み**。`config/west.yml` は `remote: tak-2025 / revision: torabo-tsuki` を指す。firmware-builder もパッド構成ではこの置き換えを出力する。

### 4.5 config store / GATT（Write Long 対応を追加）

v1 の [config_state.c](../features/trackpad/src/config_state.c) / [gatt_service.c](../features/trackpad/src/gatt_service.c) を v2 レイアウトに拡張（ダブルバッファ・ロックレス公開・NVS 再検証は不変）。GATT UUID `e1f4ac00/ac01` は**据え置き**（version で世代管理）、characteristic も単一のまま。

**GATT WRITE を MTU 超に対応させる — 2つの書込みトランスポートを両対応**（wire が数百B＝MTU 超になるため）。実装は当初「btleplug の long-write 自動分割に任せる」想定だったが、デスクトップアプリは実際には **bluest 0.6.x**（[Cargo.toml](https://github.com/tak-2025/Torabo-Studio/blob/main/src-tauri/Cargo.toml)）を使い、`Characteristic::write()` は **単発の ATT Write** に落ちる。**Windows/WinRT はこの単発 write を ATT Write Long に昇格してくれない**ため、MTU 超ペイロードは黙って落ちる（READ は Read Long で動くが WRITE は動かない、という実測バグ）。そこで **OS 非依存のアプリ側チャンク分割** ＋ **FW 側の二経路再組立** で確実に通す：

- **アプリ側**（[trackpad.rs](https://github.com/tak-2025/Torabo-Studio/blob/main/src-tauri/src/transport/trackpad.rs) / 共有ヘルパ `transport::write_chunked`）：`Characteristic::max_write_len()`（Windows では **交渉済み ATT MTU − 3**。取得不能/0 なら 180B にフォールバック）を1チャンク上限とし、ペイロードをその上限で分割して **応答付き write を順送**する。write の応答往復がチャンクを直列化する（順序保証＋フロー制御）ので **遅延挿入は不要**。MTU 内の小さな config は従来どおり単発 write。
- **FW 側**（[gatt_service.c](../features/trackpad/src/gatt_service.c) の `tp_write_cfg`）：`TP_WIRE_CAP` の静的バッファ1本で **2トランスポートを両対応** する再組立器：
  - **(A) ATT Write Long**（本来の long write。Zephyr は PREPARE キュー→Execute で offset 昇順に replay、または単発 Write Request が offset==0 で1回）。PREPARE は bounds のみ、`offset>0` は「連続 offset で連結し、`tp_apply_wire` は毎回試行（完全長のみ受理）」。
  - **(B) プレーン・チャンク書込み**（bluest/Windows。**全チャンクが offset==0** で来る）。offset で継続判定できないので、`offset==0` を次の3分岐で捌く：**①Fast path**＝そのチャンク単体が完全 wire（`tp_apply_wire(buf,len)==0`）なら適用・保存・完了（全小 config／v1 wire を吸収）。**②継続**＝再組立中で、直近チャンクが **フレッシュ（`k_uptime_get()` で 2000ms 以内）**、ステージ済みヘッダが期待全長 `tp_expected_len()` に解釈でき、`staged+len<=expected` なら末尾に連結。`==expected` で `tp_apply_wire` フル検証→成功で保存、失敗で破棄＋ATT エラー。**③新規開始**＝それ以外は破棄し、先頭が妥当ヘッダ（magic 0x7470＋既知 version）なら新規ステージ、さもなくば `BT_ATT_ERR_VALUE_NOT_ALLOWED` で拒否（ステージしない）。
- **検証は従来どおり `tp_apply_wire` に一元化**（magic/version/length/clamp・アトミック公開）。組立器はフレーミングのみで、完成 blob は必ず `tp_apply_wire` で再検証。offset 不連続・cap 超過・不整合な restart は破棄＆拒否。半端な blob は length 不一致で通らない＝安全。

---

## 5. アプリ（Torabo-Studio）設計

- [tpConfig.ts](https://github.com/tak-2025/Torabo-Studio/blob/main/src/trackpad/tpConfig.ts)：`TpBinding` 型・`TpBehavior`/`TpGestureSlot` enum を追加。`decodeTp` は version 分岐（v1→軸のみ＋既定descriptor補完 / v2→フル）、`encodeTp` は常に v2。round-trip を node で検証（P1）。
- [TrackpadSettings.tsx](https://github.com/tak-2025/Torabo-Studio/blob/main/src/trackpad/TrackpadSettings.tsx)：
  - 既存の「レイヤー×軸」テーブルに **binding 列**（behavior ドロップダウン＋キー/consumer/layer ピッカー）を追加。離散役割選択時のみ活性。
  - **ジェスチャ節**を新カード追加：レイヤー行 × { 単タップ / 2本指タップ / 長押し } の binding ピッカー。
  - キーピッカーは Torabo Studio の既存 HID usage 選択 UI（キーマップ編集のキーピッカー相当）を再利用。
  - Read→編集→Write の即反映フローは不変。
- `MainPanels.tsx` / `i18n/messages.ts`：ラベル追記のみ。Studio RPC/protobuf は**無改変**（Tauri Rust が独自GATTを read/write、v1 と同じ）。

---

## 6. バックアップ後方互換

[backupFormat.ts](https://github.com/tak-2025/Torabo-Studio/blob/main/src/backup/backupFormat.ts)：`trackpad` 任意節は **wireBase64 のまま**（中身が v2 wire になるだけ）。`BACKUP_VERSION` は据え置き可（trackpad 節の中身はFW/appの wire version が吸収）。旧バックアップ（v1 wire or trackpad無し）は §3 の version 分岐でそのまま復元可。**回帰テスト必須**：v1 wire を含む旧バックアップが v2 アプリ/FW で通ること。

---

## 7. フェーズ計画 — **FW flash は1回に集約**（書き込み回数制約）

> 方針：nRF52840 内蔵 flash の書換寿命／再flash 手間を抑えるため、**FW への書き込みは原則1回**にする。そのため「アプリ側・設計・レビューで固められるものは全て flash 前に潰し、FW 変更は全機能を1ビルドに束ねて一度だけ焼く」。反復 flash は原則バグ修正時のみ（避けられない場合）。
>
> 結果として **GST_DTAP（単独ダブルタップ）も初回1ビルドに含める**（後段に回すと2回目 flash になり方針と矛盾するため）。ドライバ小改修のリスクは「後回し」でなく「flash 前の検証」で吸収する。

### flash 前（FW を焼かずに進める）

- **P0**：本設計の合意（役割セット・binding descriptor・&kp mods 規約・hold 挙動・DTAP を含める）。
- **P1（app 先行・FW無し）**：`tpConfig.ts` v2 実装＋v1/v2 双方向 round-trip を node 検証。型・wire を確定。
- **P2（FW コード実装・まだ焼かない）**：以下を**まとめて1ブランチに実装**し、コードレビュー＋（可能なら）ビルドのみ通す：
  - binding descriptor＋実行時 binding 合成（§4.2）
  - 軸離散役割の descriptor 化（挙動は v1 と同一＝無回帰を意図）
  - `tp_keys` ジェスチャ processor（単タップ/2本指タップ/長押し、§4.1）
  - GST_DTAP を tp_keys 内で時間窓検出（**ドライバ非改修**、§4.4）
  - v2 store / GATT 拡張（§4.5）
- **P3（app UI・FW無し）**：binding 列＋ジェスチャカードを実装。P1 の型で先に組む。

### flash 1回

- **P4（単一 FW ビルド → 1回 flash）**：P2 全部入りを `docker-build.sh 0` でビルドし central へ**一度だけ**焼く。実機で一括検証：
  - 回帰ゲート①：USB有線でカーソルが死なない（フェイルオープン）。
  - 回帰ゲート②：未設定ジェスチャでドライバ既定 click が素通し。
  - 回帰ゲート③：v1 wire（旧アプリ/旧バックアップ）がそのまま通る。
  - 機能確認：軸任意キー／単・2本指・長押し・ダブルタップの割当が即反映。

### flash 後（FW 追加 flash 不要）

- **P5（app/backup）**：Read→編集→保存→即反映の実機確認、v1 wire 復元の回帰テスト。アプリ更新は flash 不要。

> 追加 flash が要るのは (a) P4 でバグ発覚時、(b) 将来の役割/behavior 追加時のみ。設計・wire は §3 の version 分岐で将来拡張を吸収するので、**アプリだけの機能追加は再 flash 不要**にしておく。

---

## 8. 決定事項 / 未決

- ✅ ハード資源：nRF52840（1MB/256KB）に対し v2 追加は誤差。限界ではない（§0）。
- ✅ 任意キーは**パレットでなく binding descriptor（behavior参照＋実行時param）**で実現（§2.2/§4.2）。
- ✅ **軸の離散役割は ENCODER に集約し、スワイプ pos/neg に各1つの binding**（Volume 等はアプリのプリセット）（§2.1）。
- ✅ ジェスチャ検出はドライバ既存（tap/tap2/hold）を活用。既定 click はフェイルオープンで維持。
- ✅ wire は version 2、magic 据え置き、v1 を後方互換受理（§3）。
- ✅ **FW flash は1回に集約**。GST_DTAP も初回ビルドに含める（§7）。**tp_keys 内で時間窓検出しドライバは非改修**（§4.4）。
- ✅ **GATT WRITE を Write Long 対応**にし wire サイズ上限を撤廃（§4.5）。
- ✅ **hold の意味＝押しっぱなし（down/up 追従）**（2026-07-10 合意）。修飾キー/レイヤー保持に使える。
- ✅ **HOLD 有効化のため iqs7211e ドライバに press&hold→BTN_2 を小追加**（§4.6、2026-07-10 合意）。tap/scroll/drag に無干渉。改変は `tak-2025/zmk-driver-iqs7211e` の `torabo-tsuki` ブランチに fork 済み（2026-08-15、GPL-3.0 のまま）。
- ✅ **DTAP を wire に追加**（gesture 12B→16B、`{tap,tap2,hold,dtap}`）。P1 codec の取りこぼしを修正し double-tap を有効化。round-trip 30/30。
- ❓ **&kp 修飾の param エンコード**：`_rsv`→mods 転用で確定予定、実装時に ZMK `kp` 実装へ突き合わせて最終化（§4.3）。→ 実装時確定。
