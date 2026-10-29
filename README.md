# std-t99-receiver

Windows PC + RTL-SDRで、ARIB STD-T99 Annex 2（デジタル小電力コミュニティ無線、通称デジコミ）の1チャネルを受信し、
音声符号を`.burst`へ、識別情報・位置情報などを含む全フレームを`.jsonl`へ保存するCLIです。
DSPはstd-t98-receiverと同じSPEAR由来の構成です。音声の復号（TOKUDER）はしません。

## 主な機能

- **RTL-SDR受信**：CH1〜18（CH18は呼出チャネル 146.984375 MHz）から指定した1チャネルを復調します。
- **情報の取り出し**：発信者・宛先の機器ID（例 `127-0123456`）、識別コードの拡張部、通話種別（個別/グループ/一斉）、
  グループ番号、緊急通信フラグ、音声方式、緯度・経度・高度・測位状態、GPS時刻・速度・方位、発信者名（Shift_JIS、CRC-32検査）。
- **音声符号の保存**：TCH/TCHのフレームを`.burst`（72-bit × 4 / 80 ms、std-t98-receiverと同じレコード形式）へ。
- **全フレームの記録**：同期バースト、FACCHだけのフレーム、TCH/FACCH混在フレームも含め、生ビット・CRC・復号結果を`.jsonl`へ1行ずつ。
- **画面表示**：項目ごとに取得元チャネル（PICH1/PICH2/SACCH/FACCH）・受信回数・経過時間を表示し、何が正しく取れているかを一目で確認できます。
- **IQの録音と再生**：`--record-iq`でRTL-SDRの生データ（cu8）を保存し、`--input`で後から何度でも再処理できます。
- **周波数ずれの自動追従**：無線機の許容差±2.5 ppm（147 MHzで±370 Hz）の範囲を含め、合成信号では±700 Hzまで補正なしで受信できています。推定値も表示します。

## セットアップ

64-bit WindowsとMinGW（C++20対応）が必要です。
USBドライバーは[Zadig](https://zadig.akeo.ie/)でRTL-SDRのInterface 0にWinUSBを設定します。

```powershell
.\build.ps1        # FFTWの取得、ビルド、試験
.\setup-rtl.ps1    # RTL-SDR公式ランタイムをbuildへ
```

## 使い方

```powershell
.\build\std-t99.exe --list-devices
.\build\std-t99.exe --list-channels
# 呼出チャネル（CH18）を受信。capture.burst と capture.jsonl を保存
.\build\std-t99.exe --channel 18 -o capture
# 初回の実機試験向け: 生IQも録音（1.2 Mspsで2.4 MB/s）。soft decisionも保存
.\build\std-t99.exe --channel 18 -o test1 --llr --record-iq test1.cu8
# 録音を再処理（--center-freq は録音時の中心 = チャネル周波数 - 250 kHz）
.\build\std-t99.exe --input test1.cu8 --sample-rate 1200000 --center-freq 146734375 --channel 18 -o test1-replay
```

画面はコンソールでは全画面表示、リダイレクト時は1フレーム1行です。`--ui lines`で常に1行表示にできます。
`Carrier offset`が大きいときは`--freq-error HZ`（表示値をそのまま）か`--ppm`（ドングル）で補正できます。
RF設定は`receiver.example.conf`を参考に`--config FILE`で読み込めます（`device`・`gain`・`ppm`・`freq-error`・`sample-rate`・`pfb`）。

## 実機なしでの確認

規格書7.7.2の試験信号（個別音声通話・発信者名付き位置通知・緊急通信・発信者名付きグループ通話）からIQを合成して再生できます。

```powershell
.\build\t99-tests.exe --write-iq build\spec-test.cu8
.\build\std-t99.exe --input build\spec-test.cu8 --sample-rate 1200000 --center-freq 146734375 --channel 18 -o build\spec-replay
```

4送信・29フレームがすべてCRC正常で表示され、発信者名「無線試験No.9」、N35°40.2565' E139°45.0235'などが出れば正常です。

[ファイル形式](FORMAT.md) / [由来と規格対応](PROVENANCE.md)

ライセンス: [GPL-3.0](LICENSE)
