#pragma once

// DAISY 2.02の図書(config::kBookDirectory)を、SDカードのMP3から再生する。
// ncc.htmlに現れる順にSMILをたどり、各SMILの<audio>クリップ(src/clip-begin/clip-end)を
// 文書順に再生する。SMILの切り替えは自動(最後のSMILの終わりで停止)。
// 読み出し・デコードは専用タスクが行い、PCMはi2s_speakerへenqueueする(単一プロデューサ)。
// 操作系はノンブロッキングで、タスクへコマンドを渡すだけ。

// SD初期化・ncc.html解析・プレーヤータスク起動。i2sSpeakerBegin()の後に呼ぶこと。
// SD未挿入・ncc.html/SMIL無しの場合はログを出してfalseを返す(以降の操作APIは何もしない)。
bool mp3PlayerBegin();

// 前/次のフレーズ(クリップ)へ移って再生する(停止中でも再生状態になる)。
// SMILの先頭クリップでのPrevは前のSMILの最後のクリップへ、SMILの最後のクリップでのNextは
// 次のSMILの先頭へ。本の最初でのPrevはそのクリップの頭出し、本の最後でのNextは何もしない。
void mp3PlayerPrevPhrase();
void mp3PlayerNextPhrase();

// 前/次の見出しの先頭へ移って再生する(停止中でも再生状態になる)。
// 最初の見出しでのPrevはその先頭、最後の見出しでのNextは何もしない。
void mp3PlayerPrevHeading();
void mp3PlayerNextHeading();

// 今再生している見出しの先頭へ戻って再生する。
void mp3PlayerHeadingStart();

// 見出し移動(上の3つ)の対象を、H1〜Hnのnで切り替える。Upで浅く(最小H1)、Downで深く
// (最大H6=全見出し)。再生位置は変わらない。
void mp3PlayerHeadingLevelUp();
void mp3PlayerHeadingLevelDown();

// 再生⇔一時停止。再開は停止した位置の続きから。
void mp3PlayerTogglePause();
