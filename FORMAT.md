# std-t99-receiver: 出力ファイル（暫定 v0）

`.burst`はstd-t98系と同じ形式です。`.jsonl`は初回の実機試験で「何が取れるか」を確かめるための暫定形式で、
扱いやすい形は実データを見てから決めます。そのため復号結果に加えて生ビットを必ず残します。

## .burst

std-t98-receiverと同じレコード形式。ヘッダーなし、1レコード = 1フレーム（80 ms）= 41 byte。

`FF (48 + 9-byte payload) × 4`

- 書くのはRICHが「TCH / TCH」（F=10, O=11）のフレームだけです。
  FACCHが半分を使うフレーム（O=01/10）と同期バースト・通話終了フレームは書きません（`.jsonl`にはすべて残ります）。
- 72-bitは規格書第5章のTCHブロックをそのままの送信bit順で保存します（TOKUDER: 音声48 + 誤り訂正24 bit / 20 ms。内部配置は未規定）。
- 隣接レコードがRF上で連続している保証はありません。`.jsonl`の`burst`欄（レコード番号）と`t`で対応を取れます。

## .llr（`--llr`）

std-t98-receiverと同じ。1レコード288 byte = 4 × 72個のsigned int8、`.burst`と同じbit順。正=1、負=0、絶対値=信頼度。

## .jsonl（常に出力）

受信したフレームごとに1行のJSON。主な欄:

| 欄 | 内容 |
|---|---|
| `t`, `sample` | 入力先頭からの秒・IQサンプル番号（同期語の先頭） |
| `sync`, `sse` | `ok`（同期語あり）/`flywheel`（予測位置で取得）、同期語の二乗誤差 |
| `kind` | `SB`（同期バースト）/`SC`（通信チャネル） |
| `rich` | F, M, O, D, パリティ |
| `sacch` | CRC、ビタビのパス誤り数、構造識別、グループ番号、18-bit単位 |
| `halves` | 前半・後半の種別（PICH/FACCH/TCH）。制御はCRCと80-bitメッセージ、TCHは72-bit × 2のhex |
| `messages` | このフレームで復号できたメッセージ（下表）。SACCHのスーパーフレームは4単位揃ったフレームに出ます |
| `name` | CRC-32が一致した発信者名（UTF-8） |
| `burst` | `.burst`のレコード番号、書かなかったフレームは`null` |
| `raw` | デホワイト後の SW / RICH / SACCH / 前半 / 後半 のhex |

`messages[].ch`は`PICH1`・`PICH2`・`SACCH`・`SACCH(SF)`・`FACCH1`・`FACCH2`。

| type | 主な欄 |
|---|---|
| Identification（PICH1） | `extension`（20 bit）, `caller`, `called` |
| Voice / End call / Remote TX request / Location request / Device ID request | `comm`, `caller`, `called`, `call_control`, `voice_mode` |
| GPS data 1、およびPICH2（型によらず位置の配置） | `lat`, `lon`（度、10進）, `lat_raw`/`lon_raw`（ddmmmmmm / dddmmmmmm）, `alt_m`, `gps_valid` |
| GPS data 2 | `gps_time`（JST）, `time_raw`（hhmmss）, `date_raw`（ddmmyy）, `speed_01kn`, `direction_deg` |
| Idle | `maker` |
| Caller name | `name_part`（1/2）, `name_bytes`（Shift_JIS hex）, `name_check`（2/2のCRC-32） |

機器IDは「メーカー識別3桁-個別ID 7桁」。宛先`000-0000000`はグループ/一斉呼出です。

## 録音（`--record-iq`）

RTL-SDRの生サンプルをそのまま保存したcu8（I/Q交互のunsigned 8-bit）。`--input FILE.cu8`で再生できます。
`--input`はcf32（little-endian float32 I/Q）も受け付けます。
