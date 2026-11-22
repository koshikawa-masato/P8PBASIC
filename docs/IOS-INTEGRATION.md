# iOS 統合設計（Phase 6）

RetroDOS Player（PC98PLAYER-iOS）に P8PBASIC の器を組み込み、
PC-88 Disk BASIC のディスク（MA2026A.D88）を P88SR なしで
直接起動・演奏できるようにするための設計。

## 全体像

```
RetroDOS Player (iOS)
  ios/P8EngineBridge.mm   ← 新規: C API の ObjC++ ラッパ
        │  C API (src/engine.h)
P8PBASIC engine (このリポジトリ)
  src/engine.c    ライフサイクル＋実行スレッド＋仮想時計
  src/halp8.c     埋め込み HAL（画面バッファ/キー/メモリ）
  src/p88conv.c   トークン形式 → BASIC テキスト変換（C 実装）
  既存: インタプリタ一式 + vessel.c（Z80）+ opna_ymfm.cpp（ymfm）
```

## エンジン API（src/engine.h）

```c
typedef struct P8Engine P8Engine;

P8Engine *p8_create(const char *d88_path);   /* ディスクイメージ */
void      p8_destroy(P8Engine *e);
int       p8_boot(P8Engine *e);              /* D88 から MENU.ALP を
                                                変換・ロードして RUN */
void      p8_start(P8Engine *e);             /* 実行スレッド開始 */
void      p8_stop(P8Engine *e);
int       p8_running(P8Engine *e);

/* 入力: 文字キー (KC_* 含む) をキューへ、キーマトリクスは生の値を書く
 * (HAL がチャタリングを除去してからゲストに見せる。アプリ側の
 * デバウンスは不要: docs/APPROXIMATIONS.md 項目 34) */
void      p8_push_key(P8Engine *e, int code);
void      p8_set_matrix(P8Engine *e, int idx, unsigned char v);

/* 画面: テキスト+グラフィックを合成した 640x400 RGBA を返す */
void      p8_frame(P8Engine *e, unsigned char *rgba /*640*400*4*/);

/* 音声: ymfm の出力をリングバッファから取り出す (44.1kHz s16le stereo) */
int       p8_audio_read(P8Engine *e, short *out, int frames);
```

## 時間と音声のモデル

- エンジンは仮想時計（エミュレーションサイクル）を持つ。
  実行スレッドはインタプリタを走らせ、`hal_idle` / `hal_ticks` と
  vessel のポンプが仮想時計を消費する。実時間より先行したら
  hal_idle が短時間ブロックして実時間に追いつかせる（ペーシング）。
- 音声は ymfm のレンダリング区間（opna_advance が生成する
  サンプル列）をリングバッファへ書く「シンク」方式に変える。
  従来の WAV ファイル出力はシンクの一実装として残す（P8PWAV）。
- アプリ側は CoreAudio のレンダリングコールバックから
  p8_audio_read で吸い出す。アンダーラン時は無音で埋める。

## ファイル供給（D88 バック）

- BASIC のファイルアクセス（LOAD/RUN/BLOAD/OPEN）は fileio.c の
  フック経由でエンジンが受ける。エンジンは D88 のディレクトリを
  引き、(a) 生バイト（BLOAD 用: NEOMSC 等）、(b) トークン形式の
  プログラムは p88conv でテキスト化してからインタプリタへ渡す。
- XDE（器のディスク読み）は従来どおり D88 を直接読む。

## 画面モデル

- halp8 は halhost と同じインメモリモデル: テキスト VRAM
  80x25（コード+属性）とグラフィック 640x400（パレット番号、
  2 ページ）。合成は p8_frame が行う（テキスト優先、属性の
  色/反転/ブリンクは近似でよい）。
- XDD（FFDD 表示サービス）は現状トレースのみ。NEOMSC が書く
  先（テキスト VRAM 相当アドレス）は Phase 6 後段で halp8 の
  バッファへ写像する（別項目）。

## アプリ側の統合点

- CMake: iOS ターゲットに P8PBASIC のソース群（C/C++）を追加。
  配置は RetroDOS リポジトリ側で third_party 相当の取り込み。
- 起動経路: ゲームフォルダの PC98PLAYER.INI に `Engine=P8` が
  ある場合、PC-98 コアの代わりに P8 エンジンを起動する。
  画面（既存の Metal/描画面）と音声（既存の AudioOut）と
  入力（キーボード/ゲームパッド/タッチ）を橋渡しする。
- 設定が無い既存ゲームには一切影響しない。

## 検証

1. エンジン単体: embed テスト（ホスト）が D88 だけで起動し、
   キースクリプトで曲が進み、WAV と画面ダンプが取れること。
2. アプリ: シミュレータで MA2026 が起動し、曲一覧が出て
   演奏音が鳴ること（スクリーンショット＋音声キャプチャ）。
3. 実機/TestFlight はユーザーの判断で別途。

## 実装結果（2026-11-18、検証 1・2 完了）

- アプリ側は `ios/P8Bridge.{h,mm}`（P8Session）が engine.h を
  包み、tick・入力（updateScan）・ポーズ・停止を既存の
  PlayerController に接続。CMake は `p8pbasic` 静的ライブラリを
  追加（要 `enable_language(C)`——プロジェクトが CXX のみ宣言の
  ため、C ソースは黙ってリンクから落ちる）。
- iOS 固有の知見:
  - 実体化ファイル名: コンテナ FS は case を畳むが、既存ファイルと
    case だけが違う名前の新規作成は EEXIST で失敗する。拡張子付き
    トークンプログラム（MENU.ALP）は `MENU_ALP.BAS` として実体化し、
    拡張子なしの別プログラム（menu→menu.BAS）と衝突させない。
  - `-[PlayerController startAudio]` は `player->ps.sample_rate`
    を無条件参照する。P8 モード（player==nil）では 44100 を使う
    ガードが必要（無いと起動即クラッシュ）。
- シミュレータ実測（専用デバイス P8Test、Debug ビルド）:
  起動 20 秒で曲リスト 34 曲が色・漢字つきで表示。音声プローブで
  アプリ経由の PCM 13.4 秒を確認（無音の起動〜読込後に対象曲が
  ピーク 14k 級で再生、ポーズ中はフレームカウンタが完全停止し
  再開時にバーストしない）。入力はエンジン単体でマトリクス
  SPACE→次曲送りを対照実験つきで確認。
