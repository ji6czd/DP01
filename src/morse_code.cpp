// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_MRS
#define LOG_MODULE_TAG "MRS"
#include "morse_code.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "config.h"
#include "i2s_speaker.h"
#include "log_config.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

// トーン正弦波の1周期ぶんのサンプル数。kMorseToneHzがkMorseSampleRateHzを割り切る前提
// (割り切れるとテーブルをループさせても継ぎ目が連続し、クリックが出ない)。
constexpr size_t kWaveLen = config::kMorseSampleRateHz / config::kMorseToneHz;
// 1単位(短点)のサンプル数。長点=3単位、点間=1単位、文字間=3単位、語間=7単位。
constexpr size_t kUnitSamples = config::kMorseSampleRateHz * 1200 / (config::kMorseWpm * 1000);
// トーン要素の頭と尻に掛けるアタック/リリースのサンプル数。
constexpr size_t kRampSamples = config::kMorseSampleRateHz * config::kMorseRampMs / 1000;

static_assert(kWaveLen * config::kMorseToneHz == config::kMorseSampleRateHz,
              "kMorseToneHz must divide kMorseSampleRateHz for a click-free wavetable loop");
static_assert(kUnitSamples % kWaveLen == 0, "unit length must be a whole number of tone periods");
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

// morseBegin()で焼くトーン1周期。s_chunkはenqueue1回ぶんのステレオPCM。いずれも静的。
int16_t s_wave[kWaveLen];
int16_t s_chunk[config::kMorseChunkFrames * 2];
size_t s_chunkFill = 0;

struct MorseRequest {
  char text[config::kMorseMaxTextLen];
};

uint8_t s_queueStorage[config::kMorseQueueDepth * sizeof(MorseRequest)];
StaticQueue_t s_queueStruct;
QueueHandle_t s_queue = nullptr;

// morseタスクのTCBとスタックも静的確保にする(s_queueと同じ理由)。
// 起動時に1回作って二度と消さないタスクなので確保失敗経路を無くす。
// StackType_t=uint8_tなのでスタック長の単位はバイト。
StackType_t s_morseTaskStack[config::kMorseTaskStackBytes];
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
  i2sSpeakerEnqueue(s_chunk, s_chunkFill * 2, config::kMorseSampleRateHz);
  s_chunkFill = 0;
}

// モノ1サンプルを左右へ複製して詰める。満杯になったらenqueueする。
void pushFrame(int16_t v) {
  s_chunk[s_chunkFill * 2] = v;
  s_chunk[s_chunkFill * 2 + 1] = v;
  if (++s_chunkFill == config::kMorseChunkFrames) {
    flushChunk();
  }
}

void emitSilence(size_t nSamples) {
  for (size_t i = 0; i < nSamples; ++i) {
    pushFrame(0);
  }
}

void emitTone(size_t nSamples) {
  for (size_t i = 0; i < nSamples; ++i) {
    float env = 1.0f;
    if (i < kRampSamples) {
      env = 0.5f - 0.5f * cosf(kPi * static_cast<float>(i) / static_cast<float>(kRampSamples));
    } else if (i >= nSamples - kRampSamples) {
      size_t m = nSamples - 1 - i;
      env = 0.5f - 0.5f * cosf(kPi * static_cast<float>(m) / static_cast<float>(kRampSamples));
    }
    pushFrame(static_cast<int16_t>(lroundf(static_cast<float>(s_wave[i % kWaveLen]) * env)));
  }
}

// text全体をモールス符号のPCMにして順にenqueueする。
// 文字間は各文字の前に3単位、語間(入力の空白)は7単位。先頭には無音を置かない。
void playText(const char* text) {
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
      emitTone(*s == '-' ? kUnitSamples * 3 : kUnitSamples);
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
    req.text[config::kMorseMaxTextLen - 1] = '\0';  // 念のため
    LOGI("playing \"%s\"", req.text);

    // 残っているものがあったら消去する
    i2sSpeakerFlush();
    vTaskDelay(pdMS_TO_TICKS(config::kMorseFlushSettleMs));
    i2sSpeakerResume();

    playText(req.text);

    // enqueueはバッファ投入で戻るので、末尾が鳴り終えるまで待ってからラジオを戻す。
    vTaskDelay(pdMS_TO_TICKS(config::kMorseTailDrainMs));
  }
}

}  // namespace

bool morseBegin() {
  for (size_t i = 0; i < kWaveLen; ++i) {
    float s = sinf(2.0f * kPi * static_cast<float>(i) / static_cast<float>(kWaveLen));
    s_wave[i] = static_cast<int16_t>(lroundf(static_cast<float>(config::kMorsePeakAmplitude) * s));
  }

  s_queue = xQueueCreateStatic(config::kMorseQueueDepth, sizeof(MorseRequest), s_queueStorage, &s_queueStruct);
  if (s_queue == nullptr) {
    LOGE("queue create failed (should not happen; static alloc)");
    return false;
  }

  // 静的確保なので生成は失敗しない(戻り値は常に有効ハンドル)。
  xTaskCreateStaticPinnedToCore(morseTaskFn, "morse", config::kMorseTaskStackBytes, nullptr, config::kMorseTaskPriority,
                                s_morseTaskStack, &s_morseTaskBuffer, config::kMorseTaskCore);

  LOGI("init done (tone=%uHz, unit=%u samples @ %uHz)", static_cast<unsigned>(config::kMorseToneHz),
       static_cast<unsigned>(kUnitSamples), static_cast<unsigned>(config::kMorseSampleRateHz));
  return true;
}

bool morsePlay(const char* text) {
  if (s_queue == nullptr || text == nullptr) {
    return false;
  }

  MorseRequest req;
  size_t i = 0;
  for (; text[i] != '\0' && i < config::kMorseMaxTextLen - 1; ++i) {
    req.text[i] = text[i];
  }
  req.text[i] = '\0';

  return xQueueSend(s_queue, &req, 0) == pdTRUE;
}
