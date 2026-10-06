#pragma once

// SDカード上のMP3(config::kMp3Directory直下のa*.mp3、名前順)を順に再生する。
// 読み出し・デコードは専用タスクが行い、PCMはi2s_speakerへenqueueする(単一プロデューサ)。
// 操作系(Prev/Next/TogglePause)はノンブロッキングで、タスクへコマンドを渡すだけ。

// SD初期化・プレイリスト作成・プレーヤータスク起動。i2sSpeakerBegin()の後に呼ぶこと。
// SD未挿入・対象ファイル無しの場合はログを出してfalseを返す(以降の操作APIは何もしない)。
bool mp3PlayerBegin();

// 前/次のファイルへ移って再生する(停止中でも再生状態になる)。先頭の前は末尾、末尾の次は先頭へ巡回。
void mp3PlayerPrev();
void mp3PlayerNext();

// 再生⇔一時停止。再開は停止した位置の続きから。
void mp3PlayerTogglePause();
