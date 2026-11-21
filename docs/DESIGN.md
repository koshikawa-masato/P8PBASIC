# P8PBASIC 設計方針

作成: 2026-11-17（MA2026A.D88 の実物解析とデトークナイズ完了を受けて）

根拠資料: `docs/MA2026-ANALYSIS.md`（ディスク形式）、
`docs/TOKEN-FORMAT.md`（言語仕様の実測）、`docs/HOST-CLI.md`（動作形態）。

## 目標

MA2026A.D88 が、P88SR + PC88.ROM なしで動くこと。
土台は P9PBASIC のインタプリタ本体、形態はまず Mac のヘッドレス CLI。

## 言語仕様として実装する範囲（実測ベース）

MA2026 の 2 プログラム（MENU.ALP / menu、全 334 行）が使うものだけを
対象にする。「PC-88 BASIC の全部」にはしない。

- 文: FOR/NEXT、IF/THEN/ELSE、WHILE/WEND、GOTO/GOSUB/RETURN（ラベル対応）、
  ON STOP GOSUB / STOP ON、READ/DATA/RESTORE（行番号・ラベル両方）、
  DIM、DEFSNG、PRINT、LOCATE、COLOR（通常形＋パレット形）、CLS [n]、
  SCREEN、LINE（BF・ラインパターン含む）、CLEAR ,上限アドレス、
  BLOAD "file",R、RUN "file"、END、POKE、OUT
- 関数: INP、PEEK、LEN、STR$、VAL、HEX$、RIGHT$（推定）、INT（推定）
- ラベル: 定義 `*名前`、参照は GOTO/GOSUB/RESTORE
- コメント: `'`（8F E9 形式）と REM
- 数値形式: トークン内の定数エンコーディング（0x0F/0x11–1A/0x1C/0x1D/0x1F）

P9PBASIC 本体との差分は主に:
1. トークナイザ／プログラム格納形式が PC-88 形式（リンク＋行番号）
2. ラベル対応（本体に無い）
3. WHILE/WEND、LOCATE、COLOR パレット形、CLS 引数、SCREEN の意味
4. ファイルが PC-88 Disk BASIC 形式ディスク上にあること

## メモリとデバイスのモデル

プログラムは 64KB 空間を PEEK/POKE/OUT/INP で直接触る。実機の
メモリマップを再現するのではなく、プログラムが実際に使う領域だけを
意味のある状態として持つ:

| 領域 | 役割 |
|---|---|
| &HC800– | 曲テーブル（4 バイト/曲）。MENU.ALP が構築、menu が参照 |
| &HCA00 / &H5500 / &H2000 / &H1000 / &HAA00 | FFDE で読み込まれるデータ置き場（MML オブジェクト、PCM、画像、PMD 本体相当） |
| &HFFF8 / &HFFF9 | 曲選択の状態（プログラム間共有） |
| &HFFFC / &HFFFD | ドライバモード（&HB1=YM2203 / &HB2=SB2） |
| &HAA20 / &HAA21 | PMD 再生状態（演奏中・ループ検出） |
| テキスト VRAM | TXR=&HF418 起点の直書き（1 行 120 バイト計算）を受ける |
| INP(0..10) | キーボードマトリクス（アクティブ Low） |
| OUT/INP ポート | SB 判定プローブ（&H44/&H45、&HA8/&HA9）と OUT 83,1（0x53） |

## NEOMSC と拡張命令の扱い（設計上の核心）

NEOMSC は Z80 機械語ドライバで、BLOAD ,R で実行され、FFDD/FFDE/FFDF の
拡張命令と PMD 再生を提供しているとみられる。

**方針（2026-11-17 ユーザー決定）: Z80 をエミュレートして NEOMSC と
PMD を実コードのまま動かす。** 拡張命令をネイティブに再実装する案は採らない。

- 器（vessel）= MIT ライセンスの Z80 エミュレータコア + 64KB メモリ +
  ポート入出力 + 割り込みを、インタプリタの HAL 側に置く。
- 採用コア: **superzazu/z80**（C99、MIT、© 2019 Nicolas Allemand。
  ライセンスは 2026-11-17 に原典で確認、採用は同日ユーザーが確定）。
  メモリ/ポートはコールバック式で器への組み込みが容易。
  候補として検証した他の MIT コア:
  suzukiplan/z80（C++ 単一ヘッダ、ZEXDOC/ZEXALL 合格）、
  kosarev/z80（C++11、サイクル精度）。floooh/chips は zlib ライセンスの
  ため「MIT」指定には合わない。
- 実装の設計規則（2026-11-17 ユーザー指示）: P9PBASIC と同じ手法、
  すなわちクリーンな C 言語・外部依存なし・HAL 分離を守る。
  器も新規コードもこの流儀で書き、同梱する Z80 コアも無改変で
  third_party に置く。
- BLOAD "NEOMSC",R は実ロード＋ Z80 実行開始とする（現状はロードのみ）。
  ロードアドレスとエントリは NEOMSC の逆アセンブルで確定する。
- FFDD/FFDE/FFDF は、NEOMSC が登録したフック規約（要逆アセンブル）で
  Z80 側へディスパッチする。段階1で入れた XDD/XDE/XDF スタブが
  その接続点になる。
- PMD 本体（予約領域から &HAA00 へ読まれる Z80 コード）も器の上で
  そのまま動かす。ポート書き込みを YM2608（ymfm）＋リズム音源へ橋渡しし、
  タイマ割り込みで演奏を進める。
- FFDE のディスク直接読みは、器のメモリへ D88 セクタを供給する形で
  NEOMSC 自身が行う（BASIC 側で肩代わりしない）。

## 最終形態: RetroDOS Player の器として

P8PBASIC は PC-88 インタプリタとして機能させ、器ごと RetroDOS Player
（iOS）に載せる（2026-11-17 ユーザー決定）。つまり P8PBASIC が
「RetroDOS Player が動かす器」になる。HAL 分離（halhost / hal98）が
そのまま iOS 側の受け口になる。

## 音楽再生（PMD は実コードを器で動かす）

- 演奏データは PMD のコンパイル済み MML オブジェクト（&HCA00 等に配置）。
  プレイヤ本体（PMD の Z80 コード）も器の上でそのまま動かすため、
  互換プレイヤの新規実装はしない。
- 音源合成は YM2608（OPNA）相当: ymfm を `third_party/ymfm` に同梱し
  （BSD 3-Clause、RetroDOS コアで実績あり）、`src/opna_ymfm.cpp` が
  器のポートモデルのレジスタ書き込み列を受けて描画する。駆動方式は
  RetroDOS のレンダラと同じ（OPN_FIDELITY_MED、プリスケーラ書き換えで
  ネイティブレート再取得、区間平均リサンプル、SSG は 0.20 で混合）。
  ホスト版は `P8PWAV` 環境変数で WAV（44.1kHz ステレオ）へ出力する。
  PCM データの供給（FFDE サービス1）は段階5で実装済み: 予約領域の
  ADPCM ブロックを SB2 の ADPCM-B DRAM（256KiB、ymfm の
  ACCESS_ADPCM_B の裏側）へ一括ロードし、PMD の PCM ドライバが
  バンク1レジスタ経由で鳴らす（2026-11-20 訂正: 当初 ADPCM-A 側へ
  誤ってロードしており、MA2026 の実測ではデータは一度も読まれて
  いなかった。PMD の PCM は ADPCM-B で、ADPCM-A はチップ内の
  リズム ROM（別供給前提）。
- PMD の割り込み契約（PMD88 v3.7 ソースで確定、2026-11-17。MA2026
  同梱は v3.3 だが実測で同型）: 演奏は2つの割り込みで進む。
  Z80 IM 2 ベクタテーブル（I レジスタのページ）の +2 が VRTC
  （フェード／効果音、約60Hz、BASIC の VRTC ハンドラ 0xE808 へ
  チェーン）、+8 が OPNA 割り込み（演奏メイン）。テンポは OPNA
  Timer B が作る（カウンタ reg 0x26、制御 reg 0x27 = 0x3A で有効化）。
  器は I に空きページをプリセットし、VRTC と Timer B の両割り込みを
  配送する（`src/vessel.c` の割り込みモデル、BASIC 実行中は
  `vessel_statement()` がポンプする）。
- リズム音（ADPCM-A）は当面 `2608_rhythm.zip` の WAV 6 点を前提とし、
  供給部を差し替え可能な分離点にする（`docs/MA2026-ANALYSIS.md` §5）。
  WAV 自体は権利確認までリポジトリに入れない。
- ADPCM-B（PCM データ）は &H5500 配置のデータをそのまま使う。

## 段階計画

1. **言語コア**（完了 2026-11-17）: 変換器 `tools/p88_to_bas.py` で
   PC-88 トークン形式を読み込み、ラベル・WHILE/WEND 等は本体機能で実行。
   XDD/XDE/XDF スタブと BLOAD ",R" 修正を実装。MENU.ALP は
   サウンドボード判定の壁まで、menu はキー待ちループまで実行を確認。
2. **器の骨格と NEOMSC 契約の確定**: NEOMSC を逆アセンブルし、
   ロードアドレス／エントリ／拡張命令フック規約／使用ポートを確定。
   superzazu/z80 を組み込み、BLOAD ,R で初期化が走ること。
3. **起動〜メニュー**: FFDE のディスク読みが器経由で働き、画面描画と
   キー入力で曲選択 UI が操作できること（無音でよい）。
   サウンドボード判定プローブ（&H44/&HA8 系）への応答もここで実装。
4. **演奏**: PMD が器の上で動き、FFDF 制御で FM/SSG が鳴ること。
   （完了 2026-11-17: 割り込みモデルで XDF P/S が完走しシーケンサが
   進行。ymfm レンダリングで MENU のデモ曲が WAV として鳴ることを
   実測——ステレオ・音程のある音・セクション毎の音量変化を確認。）
5. **リズム／ADPCM**: リズム音（暫定 WAV）と PCM を含めて MA2026 が
   実機相当に演奏できること。
   （PCM は 2026-11-20 訂正: サービス1のロード先が ADPCM-A 側に
   誤っており、「PCM パートが鳴ることを実測」は誤認だった。
   ロード先を ADPCM-B DRAM へ修正済み（H-2、回帰テスト
   tools/dramload_test.c）。PCM を使う曲での演奏実測は今後の
   課題。リズム（ADPCM-A）はチップ内 ROM で、固定リズム ROM
   （暫定 WAV 6 点から組む）の供給は器の汎用機能として分離点
   だけ残す。）
6. **iOS 統合**: RetroDOS Player に器ごと組み込む。
   （完了 2026-11-18: 埋め込みエンジン `src/engine.{c,h}` が
   D88 を実体化して起動し、RetroDOS Player（PC98PLAYER-iOS）に
   `Engine=P8` の INI で接続。シミュレータで MA2026 の曲リスト
   表示・演奏（アプリの音声経路で実測）・ポーズ連動・キー入力を
   確認。統合の詳細と iOS 固有の知見（case 違いファイル生成の
   EEXIST、startAudio の player 前提）は docs/IOS-INTEGRATION.md。
   残件: XDD/FFDD 表示サービスの画面反映（選択カーソル含む）。
   （解決 2026-11-18: 根本原因は 3 つあった。(1) NEOMSC の
   BSAVE ヘッダを無視して 0xFC 低く置いていたため、EEA フック
   先の内部参照がすべてずれ、「描画本体が無い」ように見えた —
   インストーラは存在しない。(2) XDD の描画は PC-88 の VRAM
   ウィンドウ（0xC000–0xFFFF、ポート 5C–5F バンク＋32/34/35 の
   ALU 路）へ書くため、器にバンキングが無いと曲テーブル
   （0xC800）と曲データ（0xCA00）を破壊していた。(3) 割り込み
   配送にポート 0xE4 のレベルゲートが無く、XDF S で止めたはずの
   シーケンサが XDD 8 の CG 展開と並行して走っていた。あわせて
   文中継の第1引数 priming（(0xEC41)）と、器のコールスタックを
   ウィンドウ外（最終的に 0x98F0。0xBF00 は PMD のロード範囲内だったため訂正）へ移す修正を行い、MENU.BAS 全体が
   ステップキャップ 0 件で循環し、タイトル CG が VRAM に
   描画され p8_frame に合成されることを確認した。実機ビルド 91
   で報告された P8 エンジンの音声ノイズも、同じ (1)(2) が原因の
   可能性が高い。詳細は docs/NEOMSC-CONTRACT.md。）

各段階は CLI の画面ダンプ／キー入力スクリプトで検証する
（`docs/HOST-CLI.md` の仕組みを使う）。
