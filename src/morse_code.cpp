// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_MRS
#define LOG_MODULE_TAG "MRS"
#include "morse_code.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "config.h"
#include "i2s_speaker.h"
#include "log_config.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ---- 設定 ----
// 欧文モールス符号の通知音。液晶を持たない端末の状態通知・操作の合図に使う。
// PCMは実行時に合成してi2s_speakerへ書き込む(i2s_speakerは単一プロデューサ前提)。
constexpr uint32_t kMorseSampleRateHz = 24000;
// 音階。pitch 1〜kMorsePitchCountが、A5(kMorseReferenceHz)からの半音数で決まる平均律の音に
// 対応する(低いほうから C5 D5 E5 F5 G5 A5 B5 C6 D6 E6)。pitch 0(省略)はA5。
// 周波数はトーンごとに位相を進めてsinfで合成するので、サンプルレートやWPMとは無関係。
constexpr float kMorseReferenceHz = 880.0f;
constexpr int8_t kMorseSemitones[] = {-9, -7, -5, -4, -2, 0, 2, 3, 5, 7};
static_assert(sizeof(kMorseSemitones) / sizeof(kMorseSemitones[0]) == kMorsePitchCount,
              "kMorsePitchCount (morse_code.h) must match kMorseSemitones");
// 速度(words per minute)。1単位(短点)の長さ = 1200 / WPM ミリ秒。30WPMで40ms。
constexpr uint32_t kMorseWpm = 30;
// 各トーン要素の頭と尻に掛けるraised-cosineのアタック/リリース長。キークリック抑制。
constexpr uint32_t kMorseRampMs = 5;
// 合成PCMのint16ピーク振幅(フルスケール32767に対し余裕を持たせる)。ES8311の
// ハードウェアボリューム(config::kSpeakerVolume)とは別。実機で聴きながら調整すること。
constexpr int16_t kMorsePeakAmplitude = 13000;
// i2sSpeakerEnqueue()1回あたりのステレオフレーム数(256フレーム=1KB、
// config::kPcmWriterChunkBytesと同じ粒度)。
constexpr size_t kMorseChunkFrames = 256;
// i2sSpeakerFlush()で旧音声のPCM末尾を捨て切るのを待つ時間。リング満杯でも
// config::kPcmRingBufferBytes分(24kHzステレオで約170ms)なので、それを上回る値にする。
// この待ちのあとにi2sSpeakerResume()してモールスのenqueueを始める。
constexpr uint32_t kMorseFlushSettleMs = 100;
// 最終enqueueのあと、末尾が実際に鳴り終えるのを待つ時間(enqueueはバッファ投入で戻る)。
constexpr uint32_t kMorseTailDrainMs = 200;
// 1回のmorsePlay()で受け付ける最大文字数(終端NUL含む)。超過分は切り捨てる。
constexpr size_t kMorseMaxTextLen = 24;
// 再生リクエストキューの段数。溢れたリクエストは捨てる。
constexpr size_t kMorseQueueDepth = 2;
constexpr uint32_t kMorseTaskStackBytes = 3072;
constexpr int kMorseTaskPriority = 1;
constexpr int kMorseTaskCore = 1;

// 1単位(短点)のサンプル数。長点=3単位、点間=1単位、文字間=3単位、語間=7単位。
constexpr size_t kUnitSamples = kMorseSampleRateHz * 1200 / (kMorseWpm * 1000);
// トーン要素の頭と尻に掛けるアタック/リリースのサンプル数。
constexpr size_t kRampSamples = kMorseSampleRateHz * kMorseRampMs / 1000;

static_assert(kUnitSamples > 2 * kRampSamples, "ramp must fit within a single dit");

// '.'/'-' の並びでモールス符号を表す。文字列リテラルなのでフラッシュ常駐、RAMは食わない。
struct MorseEntry {
  char ch;
  const char* code;
};

constexpr MorseEntry kMorseTable[] = {
    {'A', ".-"},    {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},    {'E', "."},     {'F', "..-."},   {'G', "--."},
    {'H', "...."},  {'I', ".."},     {'J', ".---"},   {'K', "-.-"},    {'L', ".-.."},  {'M', "--"},     {'N', "-."},
    {'O', "---"},   {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},    {'S', "..."},   {'T', "-"},      {'U', "..-"},
    {'V', "...-"},  {'W', ".--"},    {'X', "-..-"},   {'Y', "-.--"},   {'Z', "--.."},  {'0', "-----"},  {'1', ".----"},
    {'2', "..---"}, {'3', "...--"},  {'4', "....-"},  {'5', "....."},  {'6', "-...."}, {'7', "--..."},  {'8', "---.."},
    {'9', "----."}, {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."}, {'-', "-....-"}, {'=', "-...-"},
    {'+', ".-.-."}, {'@', ".--.-."}, {':', "---..."},
};

// s_chunkはenqueue1回ぶんのステレオPCM。静的。
int16_t s_chunk[kMorseChunkFrames * 2];
size_t s_chunkFill = 0;

struct MorseRequest {
  char text[kMorseMaxTextLen];
  float toneHz;
};

uint8_t s_queueStorage[kMorseQueueDepth * sizeof(MorseRequest)];
StaticQueue_t s_queueStruct;
QueueHandle_t s_queue = nullptr;
// 積まれた/再生中のリクエスト数。morsePlay()が増やし、タスクが再生と末尾の待ちを終えたら減らす。
std::atomic<uint8_t> s_outstanding{0};

// morseタスクのTCBとスタックも静的確保にする(s_queueと同じ理由)。
// 起動時に1回作って二度と消さないタスクなので確保失敗経路を無くす。
// StackType_t=uint8_tなのでスタック長の単位はバイト。
StackType_t s_morseTaskStack[kMorseTaskStackBytes];
StaticTask_t s_morseTaskBuffer;

const char* morseLookup(char c) {
  for (const MorseEntry& e : kMorseTable) {
    if (e.ch == c) {
      return e.code;
    }
  }
  return nullptr;
}

void flushChunk() {
  if (s_chunkFill == 0) {
    return;
  }
  i2sSpeakerEnqueue(s_chunk, s_chunkFill * 2, kMorseSampleRateHz);
  s_chunkFill = 0;
}

// モノ1サンプルを左右へ複製して詰める。満杯になったらenqueueする。
void pushFrame(int16_t v) {
  s_chunk[s_chunkFill * 2] = v;
  s_chunk[s_chunkFill * 2 + 1] = v;
  if (++s_chunkFill == kMorseChunkFrames) {
    flushChunk();
  }
}

void emitSilence(size_t nSamples) {
  for (size_t i = 0; i < nSamples; ++i) {
    pushFrame(0);
  }
}

// toneHzの正弦波をnSamplesサンプル出す。位相0から始め、頭と尻のランプで振幅0に絞るので、
// 周期が割り切れなくても継ぎ目にクリックは出ない。
void emitTone(size_t nSamples, float toneHz) {
  const float phaseStep = 2.0f * kPi * toneHz / static_cast<float>(kMorseSampleRateHz);
  float phase = 0.0f;
  for (size_t i = 0; i < nSamples; ++i) {
    float env = 1.0f;
    if (i < kRampSamples) {
      env = 0.5f - 0.5f * cosf(kPi * static_cast<float>(i) / static_cast<float>(kRampSamples));
    } else if (i >= nSamples - kRampSamples) {
      size_t m = nSamples - 1 - i;
      env = 0.5f - 0.5f * cosf(kPi * static_cast<float>(m) / static_cast<float>(kRampSamples));
    }
    pushFrame(static_cast<int16_t>(lroundf(static_cast<float>(kMorsePeakAmplitude) * sinf(phase) * env)));
    phase += phaseStep;
    if (phase >= 2.0f * kPi) {
      phase -= 2.0f * kPi;
    }
  }
}

// text全体をモールス符号のPCMにして順にenqueueする。
// 文字間は各文字の前に3単位、語間(入力の空白)は7単位。先頭には無音を置かない。
void playText(const char* text, float toneHz) {
  s_chunkFill = 0;
  bool haveEmittedLetter = false;
  bool wordGapPending = false;

  for (const char* p = text; *p != '\0'; ++p) {
    unsigned char uc = static_cast<unsigned char>(*p);
    if (std::isspace(uc)) {
      if (haveEmittedLetter) {
        wordGapPending = true;
      }
      continue;
    }

    const char* code = morseLookup(static_cast<char>(std::toupper(uc)));
    if (code == nullptr) {
      continue;  // 未対応文字は読み飛ばす
    }

    if (haveEmittedLetter) {
      emitSilence(kUnitSamples * (wordGapPending ? 7 : 3));
    }
    wordGapPending = false;
    haveEmittedLetter = true;

    for (const char* s = code; *s != '\0'; ++s) {
      if (s != code) {
        emitSilence(kUnitSamples);  // 点間
      }
      emitTone(*s == '-' ? kUnitSamples * 3 : kUnitSamples, toneHz);
    }
  }

  flushChunk();
}

void morseTaskFn(void* /*param*/) {
  MorseRequest req;
  for (;;) {
    if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    req.text[kMorseMaxTextLen - 1] = '\0';  // 念のため
    LOGI("playing \"%s\" %.1f Hz", req.text, static_cast<double>(req.toneHz));

    // 残っているものがあったら消去する
    i2sSpeakerFlush();
    vTaskDelay(pdMS_TO_TICKS(kMorseFlushSettleMs));
    i2sSpeakerResume();

    playText(req.text, req.toneHz);

    // enqueueはバッファ投入で戻るので、末尾が鳴り終えるまで待ってからラジオを戻す。
    vTaskDelay(pdMS_TO_TICKS(kMorseTailDrainMs));
    s_outstanding.fetch_sub(1);
  }
}

}  // namespace

bool morseBegin() {
  s_queue = xQueueCreateStatic(kMorseQueueDepth, sizeof(MorseRequest), s_queueStorage, &s_queueStruct);
  if (s_queue == nullptr) {
    LOGE("queue create failed (should not happen; static alloc)");
    return false;
  }

  // 静的確保なので生成は失敗しない(戻り値は常に有効ハンドル)。
  xTaskCreateStaticPinnedToCore(morseTaskFn, "morse", kMorseTaskStackBytes, nullptr, kMorseTaskPriority,
                                s_morseTaskStack, &s_morseTaskBuffer, kMorseTaskCore);

  LOGI("init done (unit=%u samples @ %uHz)", static_cast<unsigned>(kUnitSamples),
       static_cast<unsigned>(kMorseSampleRateHz));
  return true;
}

bool morsePlay(const char* text, uint8_t pitch) {
  if (s_queue == nullptr || text == nullptr || pitch > kMorsePitchCount) {
    return false;
  }

  MorseRequest req;
  req.toneHz = kMorseReferenceHz * exp2f(static_cast<float>(pitch == 0 ? 0 : kMorseSemitones[pitch - 1]) / 12.0f);
  size_t i = 0;
  for (; text[i] != '\0' && i < kMorseMaxTextLen - 1; ++i) {
    req.text[i] = text[i];
  }
  req.text[i] = '\0';

  s_outstanding.fetch_add(1);
  if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
    s_outstanding.fetch_sub(1);
    return false;
  }
  return true;
}

bool morseWaitIdle(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (s_outstanding.load() != 0) {
    if (millis() - t0 >= timeoutMs) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}
