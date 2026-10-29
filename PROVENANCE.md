# 由来と規格対応

## 規格

ARIB STD-T99 v4.1（英語版）Annex 2 デジタル小電力コミュニティ無線。

- 物理層はstd-t98-receiverが扱うフレームと同系統です: 4値FSK 4.8 kbps（±315/±945 Hz）、ロールオフ0.2、
  同期語CDF59h、80 ms = 192シンボル、RICH/SACCH/PICH/FACCH/TCH、K=5 R=1/2畳み込み符号と同じパンクチャ、
  CRC-6/CRC-12、PN(9,5)ホワイトニング（初期値011100100）。
- T99固有: チャネル表（142.934375 MHz〜、146.934375 MHz〜、CH18が呼出）、RICHの配置（F/M/O/予備/パリティ）、
  SACCHのW（構造識別2 + グループ番号6）と4フレームのスーパーフレーム、4.2節のメッセージ。
- 規格書7.7.2の試験信号（53フレーム）を`tests/gen_spec_vectors.py`で`tests/native/spec_vectors.hpp`に変換し、
  全フレームのCRC・パス誤り0・メッセージ内容を試験しています。
- 規格書から確定できなかった点と、試験信号で確かめた点:
  - 発信者名の「Check bit 32bit」は CRC-32/MPEG-2（poly 04C11DB7、初期値FFFFFFFF、反転なし）で12 byteの名前に一致。
  - GPS data 1のA/Vは本文に「Bit 4」、図ではoctet 10のbit 5。図に従います（試験信号は両方0で区別できません）。
  - RICHのパリティ範囲は本文どおり上位4 bit（F, M）で検査します。試験信号では7 bit全体とも区別できません。
  - 試験信号のTCH hexは各フレーム最初の72-bitブロックだけ表2-7-1と4 bit異なります（印刷上の差と判断）。
- 実機（メーカーコード34、2026-10-29 CH1で受信）で確かめた点:
  - GPS未測位時はPICH2/GPS data 1のbit 75（図の位置）が1、緯度経度0、GPS data 2の日付が0。A/Vは図の位置で合っていると判断。
  - 緊急キーの送信は図Annex 2-4-45どおり（同期バースト×6でGPS data 1 → FACCHでGPS data 2 + End call）。
    緊急フラグはEnd callの呼制御付加情報だけで、PICH2のbit 78-79は0のまま。
  - 送信終了直後や雑音でCRC-6が偶然通るフレームが出たため、CRC-12が通るかRICHが有効な組み合わせのフレームだけを出力。
- 受信に関係しない項目は実装していません: 送信側要件、キャリアセンス、送信時間制限、選択呼出の着信判定、
  38-bit同期語（プリアンブル後半を含む捕捉）、メーカー定義（フォーマット2）の中身、Annex 1（登山者検知）。

## コード

基準: SPEAR `d1611fd408939ad238372d58828639e8fde28400`（`git show`で原本から取得）。

- `vendor/spear`: FIR・PFB・段の規約・FFTW planner、RRC設計、Gardnerシンボル同期。原本と同一で、
  FIRのWindows用`target_clones`ガードのみ変更（std-t98-receiverと同じ）。`tests/test_spear_sources.py`で検査します。
- `src/frame_sync.*`: SPEARの`apps/std_t98/dsp/sync_correlator`（同期語検出と状態機械）を複製し、
  同期語のSSEを「窓の平均を引いた値」で測るよう変更。周波数ずれはシンボルのDCになり、±370 Hzで1レベルを超えるため。
  窓の平均を候補のDCとして判定前に差し引きます。
- `src/receiver.*`: std-t98-receiverの単一チャネル受信器と同じ段構成（中心へ回転、間引き、bin 0のみのPFB、
  ×10補間、FM検波、受信フィルタH/P、シンボル同期、同期状態機械）。T99向けに次を追加:
  - 捕捉経路: フィルタ後の62.5 kHz信号で同期語を全サンプル位相で直接探し、CRC-12（PICH/FACCH）が通るフレームを出す。
    送信開始時にシンボル同期の引き込みが間に合わず同期バースト（ID・位置）を落とすのを防ぐ。同期中は同期バーストのみ。
  - 従来経路のフレームでCRCが落ちたときは同じ位置を最適サンプル位相で取り直し、CRCが多く通る方を採用。
  - 同じフレームの二重出力は時刻（半シンボル以内）で除去。
- `src/center_channelizer.hpp`, `src/iq_queue.hpp`, `src/input.*`, `src/config.hpp`: std-t98-receiverから流用
  （入力にcu8を追加）。`src/dashboard.*`, `src/output.*`, `src/main.cpp`はstd-t98-receiverを基に作り直し。
- `src/protocol.*`, `src/messages.*`, `src/session.*`: T99用に新規（ビタビ・CRC・デホワイトはstd-t98-receiverのprotocolと同じ方式）。

合成IQ試験の変調器（`tests/native/test_main.cpp`）は規格書3.3の送信フィルタH(f)·P(f)と偏移をそのまま使う試験専用のものです。
実機での受信感度・相互運用は未確認です。
