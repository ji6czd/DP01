#pragma once

// DAISY 2.02の図書を、SDカードから再生するプレーヤー。
// 「どの本の、どこを、どの順に鳴らすか」を受け持つ。ncc.html/SMILの解析、再生位置(SMIL・
// クリップ)、見出しレベルとフレーズ/見出し/本の移動、しおりがここにある。音を出すのは
// mp3_player(クリップを渡されて鳴らすだけ)。
// SDのルート直下にある、ncc.htmlを持つフォルダを1冊として扱う。
// 読み出し・デコードは専用タスクが行い、操作系はノンブロッキングでタスクへコマンドを渡すだけ。

// SDマウント・本の一覧作成・プレーヤータスク起動。i2sSpeakerBegin()の後に呼ぶこと。
// SD未挿入・本が1冊も無い場合はログを出してfalseを返す(以降の操作APIは何もしない)。
bool daisyPlayerBegin();

// 前/次の本へ移して(しおりがあればその続きから)再生する。直前の本の位置はしおりへ保存する。
// 一覧の両端では何もしない。開けない本(ncc.html/SMILが読めない)は飛ばす。
void daisyPlayerPrevBook();
void daisyPlayerNextBook();

// 前/次のフレーズ(クリップ)へ移って再生する(停止中でも再生状態になる)。
// SMILの先頭クリップでのPrevは前のSMILの最後のクリップへ、SMILの最後のクリップでのNextは
// 次のSMILの先頭へ。本の最初でのPrevはそのクリップの頭出し、本の最後でのNextは何もしない。
void daisyPlayerPrevPhrase();
void daisyPlayerNextPhrase();

// 前/次の見出しの先頭へ移って再生する(停止中でも再生状態になる)。
// 最初の見出しでのPrevはその先頭、最後の見出しでのNextは何もしない。
void daisyPlayerPrevHeading();
void daisyPlayerNextHeading();

// 今再生している見出しの先頭へ戻って再生する。
void daisyPlayerHeadingStart();

// 見出し移動(上の3つ)の対象を、H1〜Hnのnで切り替える。Upで浅く(最小H1)、Downで深く
// (最大H6=全見出し)。再生位置は変わらない。
void daisyPlayerHeadingLevelUp();
void daisyPlayerHeadingLevelDown();

// 再生⇔一時停止。再開は停止した位置の続きから。一時停止の位置はしおりへ保存する。
void daisyPlayerTogglePause();

// 再生速度を1段遅く/速くする(mp3_playerの話速変換。音程は変わらない)。
void daisyPlayerSlower();
void daisyPlayerFaster();
