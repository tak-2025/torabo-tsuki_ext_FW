# 設計 — torabo-tsuki トラックパッド ライブ設定（レイヤーごと機能割当）

> 目的：ミニトラックパッド（実質1軸＝縦スワイプ）を **レイヤーごとに機能割当**（スクロール/音量/輝度/ズーム/ブラウザ戻る進む/カーソル移動/OFF）できるようにし、Torabo-Studio から編集→保存で即反映する。
> トラックボール側の [DESIGN-trackball.md](DESIGN-trackball.md) の安全鉄則（フェイルオープン・検証付き wire・ロックレス公開・非ブロッキング）をそのまま継承する。
> **実装はこの設計の合意後に着手する。**

---

## 1. スコープ / 役割セット（v1）

トラックボールの `ztc_axis`（役割/向き/速度）モデルを再利用し、**役割 enum を拡張**する。

| role | 種別 | 挙動 | HID |
|---|---|---|---|
| 0 MOVE | 連続 | カーソル移動 | REL_X/Y |
| 1 SCROLL | 連続 | スクロール | X→HWHEEL / Y→WHEEL |
| 2 OFF | — | 無効（値0、STOPしない） | — |
| 3 VOLUME | 離散 | 音量 up/down | C_VOL_UP / C_VOL_DN |
| 4 BRIGHTNESS | 離散 | 輝度 up/down | C_BRI_UP / C_BRI_DN |
| 5 ZOOM | 離散 | ズーム in/out | **既定 Ctrl+ホイール**（設定で Ctrl+`=`/`-` に切替可） |
| 6 BROWSER | 離散 | ブラウザ 進む/戻る | AC Forward / AC Back（＝ consumer。代替 Alt+→/←） |

- **連続系(0/1/2)** … 既存 `ztc_pointer` 経路（向き→速度分周→役割）。
- **離散系(3〜6)** … 新規 `ztc_encoder` 経路：軸 delta を積算し、閾値 `step` ごとに **press→release を1回**発火（非ブロッキング）。移動は殺す（値0・CONTINUE、STOP禁止）。方向は軸の向きで up/down（in/out・進む/戻る）を決める。
- **フェイルオープン厳守**：未設定/不明 role は MOVE、離散系は何も発火しない。入力スレッドはロックレス read のみ。

---

## 2. データモデル（デバイス次元を追加）

トラックパッドは最大 **4 デバイス**想定（左右それぞれに pad/ball を任意に付ける4パターン）。各デバイスは **id** で識別し、レイヤーごとに X/Y 軸の設定を持つ。

```c
struct tp_axis  { uint8_t role; uint8_t direction; uint8_t step; }; // 3B（stepは分周/閾値）
struct tp_layer { struct tp_axis x; struct tp_axis y; };            // 6B
// デバイス: id + 各レイヤー
```

**既定値（＝現状の固定 overlay 挙動を再現）**：層0/1=MOVE、層2=横SCROLL、層3=縦SCROLL（[input-trackpad-ext.overlay](../snippets/input-trackpad-ext/input-trackpad-ext.overlay) 相当）。ゼロ/未設定＝MOVE 寄り安全側。

デバイス id（安定・拡張可）：`0=左パッド(std)` / `1=右extパッド(I2C1)` / 2,3=予備。

---

## 3. wire protocol（packed・LE・固定長・versioned）— UIとFWの唯一の契約

```
Header (6B): magic u16=0x7470("tp"), version u8=1, device_count u8, layer_count u8, _rsv u8
device_count 回くり返し:
  device_id u8, _rsv u8                                   (2B)
  layers[layer_count] each: x{role,dir,step} y{role,dir,step}  (6B/層)
```
- 長さ＝`6 + device_count*(2 + layer_count*6)`。**手計算で検証**（flexible array の sizeof に依存しない）。
- magic/version/長さ不一致＝**全拒否→既定維持**。各値はクランプ（role<=6、step 1..32、不明はMOVE）。
- **TS(app) と C(FW) で同一定義**。READ=現状を wire で返す / WRITE=検証→shadow→atomic swap→NVS。

---

## 4. FW 設計

- **GATT**：トラックボール(e1f4a900)・マクロ(aa)・コンボ(ab)と同様、**専用サービスを新設**（`e1f4ac00/ac01`）。既存3サービスは無改変（＝低リスク）。ENCRYPT 必須・単一 characteristic・offset/len/magic/version 検証。
- **`ztc_encoder`（新規 input processor）**：離散役割を担当。軸 delta 積算→`step` 到達で keycode の press→release を `k_work`/msgq で発火（入力スレッドをブロックしない、temp_layer と同流儀）。`&pointing_listener_ext` の base processor に `<&ztc_pointer>, <&ztc_encoder>, <&ztc_temp_layer 1 500>` の順で挿す（pointer が先、OFF/scroll を処理、encoder が離散役割）。
- **config store（デバイス次元）**：`ztc` の config_state を一般化、または独立 `tp_config` を新設。ダブルバッファ＋ロックレス公開＋NVS 再検証（DESIGN-trackball §11.D/E 準拠）。
- **役割の consumer コード**：`C_VOL_UP/DN`, `C_BRI_UP/DN`, ブラウザは `AC_FORWARD/AC_BACK`、ズームは Ctrl+WHEEL（別 role param で `=`/`-` 切替）。
- central が split 経由で左右両パッドを処理するので、**1サービスで左右両パッドを収容**。

---

## 5. アプリ（Torabo-Studio）設計

既存の「機能ごとに独自GATT＋専用タブ」パターン（[MainPanels.tsx](../../zmk-studio/src/MainPanels.tsx)）を踏襲：
- `src/trackpad/tpConfig.ts` … §3 の decode/encode（[ztcConfig.ts](../../zmk-studio/src/trackball/ztcConfig.ts) が雛形）
- `src-tauri/src/transport/trackpad.rs` … [trackball.rs](../../zmk-studio/src-tauri/src/transport/trackball.rs) 複製・UUID差替 → `main.rs` の `generate_handler!` 登録
- `src/tauri/trackpad.ts` … invoke ラッパ
- `src/trackpad/TrackpadSettings.tsx` … **デバイス選択（左/右）＋レイヤー行に役割ドロップダウン**（§1）＋向き＋step。レイヤー数は RPC `keymap.getKeymap` から取得
- `MainPanels.tsx` TABS ＋ `i18n/messages.ts` にラベル追加
- Studio RPC/protobuf は**無改変**（Tauri Rust が独自GATTを read/write）

---

## 6. バックアップ後方互換（必須要件）

[backupFormat.ts](../../zmk-studio/src/backup/backupFormat.ts) の既存方式（版番号＋任意節）を踏襲：
- `BACKUP_VERSION` 2 → **3**。`trackpad?: { wireBase64 } | null` を **任意節**として追加。
- `validateBackup`：`version <= 3` を受理（**古い v1/v2 はそのまま通る**）。必須は従来どおり trackball+keymap のみ。trackpad は任意。
- `onExport`：他節と同様 try/catch で trackpad を READ（サービス無ければスキップ）。
- `onImport`：`if (file.trackpad?.wireBase64) trackpadWriteConfig(...)`（在るものだけ復元）。
- **結果：過去のバックアップ（trackpad 無し）は完全に動作し、新バックアップだけ trackpad を含む。** 逆に新バックアップを旧アプリで開くと version>2 で丁寧に拒否（既存の挙動）。

---

## 7. フェーズ計画（各フェーズに fail-open 回帰ゲート）

- **P0**：本設計の合意（役割セット・wire・ズーム既定＝Ctrl+ホイール）。
- **P1（app 先行・FWモック無しで型を固める）**：`tpConfig.ts` 実装＋encode/decode round-trip を node で検証。
- **P2（FW）**：`ztc_encoder` ＋ config store ＋ 新GATT。単デバイス（右extパッド）から。USB有線でカーソルが死なない回帰ゲート必須。
- **P3（app UI）**：`trackpad.rs`＋`TrackpadSettings.tsx`＋タブ登録。実機 Read→編集→保存→即反映。
- **P4（backup v3）**：backupFormat/BackupPanel 拡張。**旧バックアップ復元の回帰テスト**（v1/v2 ファイルが通ること）。
- **P5**：多デバイス（左パッド＋右パッド）／ブラウザ・ズーム微調整。

---

## 8. 決定事項 / 未決
- ✅ デバイス数：最大4パターン対応（左右それぞれ ball/pad 任意）
- ✅ 役割セット：スクロール/音量/輝度/ズーム/ブラウザ戻る進む/移動/OFF
- ✅ バックアップ後方互換：任意節＋version 3（本書 §6）
- ❓ **ズーム既定**：Ctrl+ホイール（推奨・多くのアプリで有効） vs Ctrl+`=`/`-`。→ 既定は Ctrl+ホイール、role param で切替可とする（合意で確定）。
