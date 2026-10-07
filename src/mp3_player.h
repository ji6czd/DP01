#pragma once

#include <cstdint>
#include <string>

// MP3の1クリップ(ファイルの[beginMs, endMs)区間)を再生するエンジン。再生だけを受け持つ。
// DAISYのncc/SMIL・しおり・見出しなど「何をどの順に鳴らすか」は知らない(daisy_playerの仕事)。
// タスクもコマンドキューも持たない。呼び出し側のタスクが mp3PlayerPump() を回して駆動する。
// すべての関数は同じタスクから呼ぶこと(排他は取らない)。PCMはi2s_speakerへenqueueする。

// endMsがこれなら、ファイル末尾まで。
inline constexpr uint32_t kMp3ClipToEnd = UINT32_MAX;

struct Mp3Clip {
  std::string path;  // SD上の絶対パス
  uint32_t beginMs = 0;
  uint32_t endMs = kMp3ClipToEnd;
};

// 再生中のクリップが終わったときに呼ばれ、続きのクリップがあればoutへ入れてtrueを返す。
// falseなら再生は止まり、mp3PlayerPump()がkEndedを返す。mp3PlayerPump()の中(デコーダの
// コールバック)から呼ばれるので、重い処理(SD読み出しなど)はしないこと。
using Mp3NextClipFn = bool (*)(Mp3Clip* out);

// デコーダの準備。i2sSpeakerBegin()の後に呼ぶ。SDは呼び出し側がマウント済みであること。
void mp3PlayerBegin(Mp3NextClipFn nextClip);

// clipを頭から再生する。flushAudio=trueはユーザ操作(旧クリップの残りPCMを捨てて即切替)、
// falseは自然な遷移(旧クリップ末尾は切らずに鳴らし切る)。開けない/MP3でなければfalse。
bool mp3PlayerStart(const Mp3Clip& clip, bool flushAudio);

// 再生を止め、鳴りかけの音も捨てる。SMILの読み込みなど時間のかかる処理の前に呼ぶと、
// 旧位置の音が残らない。
void mp3PlayerStop();

// 本の最後まで再生し終えたとき、話速変換に残った末尾を出し切る。
void mp3PlayerFinish();

enum class Mp3PumpResult : uint8_t {
  kPlaying,  // 再生中(もう一度呼ぶ)
  kEnded,    // 次のクリップが無く再生が終わった(EOFでクリップの途中で終わった場合も含む)
  kIdle,     // 何も再生していない
};

// 1回分(SDから最大kMp3ReadChunkBytes読んでデコード)進める。i2s_speakerのリングが満杯だと
// ブロックするので、読み出しのペースはそれで律速される。
Mp3PumpResult mp3PlayerPump();

// 再生速度を1段遅く/速くする(config::kPlaybackSpeeds、両端で止まる)。音程は変わらない。
// 変更はリングバッファに積まれている分(100〜200ms程度)の後から効く。戻り値は変わったか。
bool mp3PlayerSlower();
bool mp3PlayerFaster();

// 今の再生速度(倍率)。
float mp3PlayerSpeed();

// 合図の音を挟むなど、再生を一度止めて同じ所から続けるための再開位置。今のクリップの
// コピーで、beginMsを「耳に届いている位置」の見積もり(デコード位置からリングなどの分を
// 戻した位置。クリップの頭より前には戻らない)にしたもの。止める前(mp3PlayerStop()の前)に
// 呼び、止めたあとmp3PlayerStart(*out, false)へ渡す。再生中でなければfalse。
bool mp3PlayerResumePoint(Mp3Clip* out);
