#pragma once

#include <driver/i2s_std.h>

#include <cstddef>
#include <cstdint>

// I2S出力を担当する汎用モジュール(特定のコーデックやボード配線には依存しない)。
// チャンネルミキシングは持たない(常に1本のストリームのみ再生する前提)。
// PCMは内部の静的リングバッファへenqueueするだけで、実際のI2S書き込み(実時間ペースの
// ブロッキング呼び出し)は専用タスクが行う。呼び出し元(aac_player.cpp)のデコードを
// 実時間書き込みのペースから切り離すためにこの構成にしている。

// std_mode初期化済み・enable前のI2S TXチャンネルハンドルを受け取り、enable/disable・
// サンプルレート変更・DMAバッファ管理を含むPCMリングバッファと書き込み専用タスクを
// 起動する。txHandleの生成(GPIO配線等、ハードウェア固有の設定)は呼び出し元の責務。
// txHandleを生成したコーデック/ボード側の初期化が完了した後に呼ぶこと。
bool i2sSpeakerBegin(i2s_chan_handle_t txHandle);

// PCM(int16 LRインターリーブ, sampleCount = 要素数)をリングバッファへenqueueする。
// sampleRateHzはHelixデコーダの申告値(info.sampRateOut)をそのまま渡す。チャンネル内では
// 不変という前提(切り替え時のみ変化する)で、書き込み専用タスク側が次の書き込み前に適用する。
// リングバッファが満杯の間はブロックするが、待っている最中にi2sSpeakerFlush()が呼ばれたら
// 残りを捨てて即座に戻る(falseを返す)。呼び出し元は戻り値を無視してよい。
bool i2sSpeakerEnqueue(const int16_t* pcm, size_t sampleCount, uint32_t sampleRateHz);

// リングバッファに残っている(まだ書き込まれていない)PCMを破棄し、以降のenqueueの
// 受け付けを停止する。チャンネル切り替え時、旧チャンネル分の音が新チャンネルの頭に
// 混ざらないようにするために呼ぶ。
// 受け付けを止めるのは、この時点でデコードタスクがまだ旧チャンネルのフレームを
// デコード中でありうるため(リング満杯でブロックしていると、破棄で空いた後に
// 着地して新チャンネル扱いで再生されてしまう)。
// 停止したままだと無音が続くので、必ずi2sSpeakerResume()と対で使うこと。
void i2sSpeakerFlush();

// i2sSpeakerFlush()で止めたenqueueの受け付けを再開する。新チャンネルのデコーダを
// 初期化し終えた直後(= 以降に来るPCMは新チャンネルのものだと確定した時点)に呼ぶ。
void i2sSpeakerResume();

// 実PCM(データ切れの無音埋めではない実データ)を直近kSpeakerPlayingTimeoutMs以内に
// I2Sへ書き込んでいればtrue。データ切れで無音だけを書いている間や、チャンネル切り替えの
// 破棄モード中、まだ一度も再生していない起動直後はfalse。どのタスクからでも呼べる。
bool i2sSpeakerIsPlaying();
