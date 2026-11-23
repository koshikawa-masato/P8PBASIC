# P8PBASIC

P8PBASIC は、Imaha486 氏の [P9PBASIC](https://github.com/Imaha486/P9PBASIC)（PC-9801 / MS-DOS 用の N88日本語BASIC(86) 互換 BASIC インタプリタ）をフォークし、PC-8801（PC-88）向けに作り替えたものです。
BASIC インタプリタに「Z80 の器（うつわ）」を加えてあり、NEOMSC や PMD といったドライバは再実装せず、元の Z80 コードをそのまま実行します。
ROM やゲームのデータは一切含みません。利用者が自分で用意した D88 ディスクイメージを読み込んで使います。

## 特徴
- BASIC インタプリタ本体は P9PBASIC 由来（C 言語）
- Z80 の器：NEOMSC / PMD のバイナリを、利用者のディスクイメージから読み込んで Z80 コアの上で実行します。
  Z80 コアは [superzazu/z80](https://github.com/superzazu/z80) を無改変で同梱しています。
  器の側は、その周囲の PC-88 ハードウェア（I/O ポート、割り込み、D88 ディスク層）を受け持ちます
- OPNA（YM2608）の音は [ymfm](https://github.com/aarongiles/ymfm) で生成
- D88 ディスクイメージに対応
- PC-88 のキーボード・グラフィック・テキスト画面のモデル
- ホスト用 CLI と、組み込み用エンジンライブラリ（iOS の RetroDOS Player に組み込まれています）
- `tests/` に回帰テスト一式（`tests/run_regress.sh`）

## クリーンルームについて
**上流の P9PBASIC（インタプリタ部分）について**、上流の README は次のように宣言しています。

> P9PBASIC は、PC-98 の BIOS ROM の逆アセンブルや、N88日本語BASIC(86) のコード盗用は一切行っていません。
> すべて C 言語で新規に書いたプログラムです。

これは上流作者 Imaha486 氏による、上流インタプリタについての宣言です。

**このフォークで追加した部分について**は、次の方法で作っています。

- NEC の BIOS / BASIC の ROM コードおよび ROM データは、このリポジトリのどこにも含まれていません
- 器と、命令レベルのサービスは、観察した動作の取り決め（コントラクト）と公開資料をもとに書きました。参照したのは次のものです
  - KAJA 氏の PMD88 v3.7 のソース（地図として使用。正としたのは実際に配布されている PMD のバイナリです）
  - QUASI88（winterheart/quasi88）：動作の参考
  - NEC の PC-8801mkIISR のマニュアル
  - 公開されている PC-88 ディスクサブシステムの資料
- 詳細と出典は [docs/NEOMSC-CONTRACT.md](docs/NEOMSC-CONTRACT.md) と [docs/APPROXIMATIONS.md](docs/APPROXIMATIONS.md) に記録しています

**そのまま含めているものが 1 つだけあります。** NEOMSC（MEGAFEPPS「MUSIC ARENA 2026」の一部）の 30 バイトのバイト列を `src/vessel.c` に埋め込んでいます。
用途は NEOMSC が読み込まれたことを検出するための指紋（照合用）だけで、比較に使うのみで実行はしません。出所と目的はその場所のコメントに書いてあります。

**フォント**は東雲フォント（/efont/ Project、パブリックドメイン）の字形です。`tools/fonts/` の BDF から `tools/gen_kanjifont.py` で `src/kanjifont.c` を生成しています。NEC の漢字 ROM のデータは含みません。

## ビルド
- `./build-host.sh` … ホスト用 CLI をビルドします
- `./build-embed.sh` … 組み込み用エンジンライブラリをビルドします

`build.sh` / `build.bat`（Open Watcom による 16 ビット DOS 用 `P9PBASIC.EXE` のビルド）は上流から引き継いだもので、このフォークでは対応していません。
器には DOS 向けのターゲットがないためです。`build.sh` は実行すると理由を表示して停止します。

## フォルダ構成
| パス | 内容 |
|---|---|
| `src/` | インタプリタ本体（上流由来）と、フォークで追加した器・エンジン・HAL（`vessel.c`、`engine.c`、`halp8.c`、`materialize.c`、`p88conv.c`、`p88disk.c`、`opna_ymfm.cpp`、`kanjifont.c`） |
| `third_party/z80/` | 同梱の Z80 コア（superzazu/z80） |
| `third_party/ymfm/` | 同梱の ymfm（YM2608 / OPNA 関連のファイルのみ） |
| `tools/` | 変換ツール、フォント生成スクリプト、テスト |
| `tests/` | 回帰テスト（`tests/run_regress.sh`）とプローブ |
| `docs/` | 設計・仕様などの文書（下記） |
| `samples/` | 上流のサンプルプログラム |

## ドキュメント
- [docs/DESIGN.md](docs/DESIGN.md) … 設計
- [docs/NEOMSC-CONTRACT.md](docs/NEOMSC-CONTRACT.md) … NEOMSC との動作の取り決め
- [docs/APPROXIMATIONS.md](docs/APPROXIMATIONS.md) … 実機の動作と異なる既知の点を、項目ごとにすべて記載
- [docs/TOKEN-FORMAT.md](docs/TOKEN-FORMAT.md) … トークン形式
- [docs/MANUAL.md](docs/MANUAL.md) … 上流（P9PBASIC）のマニュアル
- [docs/HOST-CLI.md](docs/HOST-CLI.md) … ホスト用 CLI
- [docs/IOS-INTEGRATION.md](docs/IOS-INTEGRATION.md) … iOS への組み込み

## ライセンス
MIT License（[LICENSE](LICENSE)）。
同梱している第三者のコンポーネント（z80、ymfm、東雲フォントなど）の表示は [NOTICE](NOTICE) にまとめています。

上流の README にある「実行ファイルには Open Watcom の C ランタイムライブラリが含まれます」という注記は、上流の DOS 用ビルドだけに当てはまります。

## 作者
- 上流（P9PBASIC）：Imaha486
- フォーク（P8PBASIC）：Masato Koshikawa（GitHub: [koshikawa-masato](https://github.com/koshikawa-masato)）
