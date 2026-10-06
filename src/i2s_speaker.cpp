// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_SPK
#define LOG_MODULE_TAG "SPK"
#include "i2s_speaker.h"

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "log_config.h"

namespace {
i2s_chan_handle_t s_txHandle = nullptr;
uint32_t s_currentSampleRateHz = 0;
bool g_firstPlaybackLogged = false;

// I2Sチャンネルが現在enable状態か。サンプルレート再設定はdisable→reconfig→enableの
// 手順が必要で、enableに失敗したまま放置するとi2s_channel_write()が以降ずっと
// ESP_ERR_INVALID_STATEを返し、ログも出ないまま永久に無音になる。状態を持っておき、
// i2sSpeakerWrite()側からリトライできるようにする。
bool s_txEnabled = false;

bool i2sChannelEnable() {
  esp_err_t err = i2s_channel_enable(s_txHandle);
  if (err != ESP_OK) {
    LOGE("channel enable failed: %d", static_cast<int>(err));
    s_txEnabled = false;
    return false;
  }
  s_txEnabled = true;
  return true;
}

// I2SのDMAバッファに既に積まれているPCM(i2s_channel_write()を通過済みの分)を捨てる。
// リングバッファをいくら正しく破棄しても、ここに残った分はそのまま鳴ってしまう。
// I2S_CHANNEL_DEFAULT_CONFIGはdma_desc_num=6 × dma_frame_num=240 = 1440フレームなので、
// 24kHzステレオで約60ms、48kHzで約30msに相当する。
// disable→enableでDMAの読み書きポインタがリセットされ、積まれた内容は破棄される。
// writerTaskからのみ呼ぶこと(i2s_channel_write()と同じタスクである必要がある)。
void i2sDropDmaBuffer() {
  if (s_txHandle == nullptr) {
    return;
  }
  esp_err_t err = i2s_channel_disable(s_txHandle);
  if (err != ESP_OK) {
    LOGE("disable for DMA drop failed: %d", static_cast<int>(err));
    return;  // disableできていないのでenableのままでよい
  }
  s_txEnabled = false;
  i2sChannelEnable();  // 失敗してもi2sSpeakerWrite()側がリトライする
}

bool i2sSpeakerSetSampleRate(uint32_t sampleRateHz) {
  if (s_txHandle == nullptr || sampleRateHz == 0) {
    return false;
  }
  if (sampleRateHz == s_currentSampleRateHz) {
    return true;
  }

  const uint32_t previousRateHz = s_currentSampleRateHz;

  esp_err_t err = i2s_channel_disable(s_txHandle);
  if (err != ESP_OK) {
    // disableできなかった場合、チャンネルはenableのままなので再生自体は継続できる。
    // レートを更新しないでおけば次のフレームで再試行される。
    LOGE("channel disable failed: %d (keeping %uHz)", static_cast<int>(err), static_cast<unsigned>(previousRateHz));
    return false;
  }
  s_txEnabled = false;

  i2s_std_clk_config_t clkCfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRateHz);
  err = i2s_channel_reconfig_std_clock(s_txHandle, &clkCfg);
  if (err != ESP_OK) {
    LOGE("reconfig clock to %uHz failed: %d (reverting to %uHz)", static_cast<unsigned>(sampleRateHz),
         static_cast<int>(err), static_cast<unsigned>(previousRateHz));
    i2s_std_clk_config_t revertCfg = I2S_STD_CLK_DEFAULT_CONFIG(previousRateHz);
    if (i2s_channel_reconfig_std_clock(s_txHandle, &revertCfg) != ESP_OK) {
      LOGE("revert to previous sample rate also failed");
    }
    // ここでenableし忘れるとチャンネルがdisableのまま残る。失敗しても
    // i2sSpeakerWrite()側がリトライするので戻り値は無視してよい。
    i2sChannelEnable();
    return false;
  }

  // reconfig自体は成功しているのでハードウェアは新レートになっている。enableに
  // 失敗してもレートは更新しておく(更新しないとdisable/reconfigを毎フレーム
  // 繰り返してしまう。enableのリトライはi2sSpeakerWrite()側が行う)。
  s_currentSampleRateHz = sampleRateHz;
  LOGI("sample rate changed: %u -> %uHz", static_cast<unsigned>(previousRateHz), static_cast<unsigned>(sampleRateHz));
  return i2sChannelEnable();
}

// 同じ失敗が続く間ログを1回に抑えるためのフラグ(書き込み成功でクリアする)。
bool s_writeErrorLogged = false;

bool i2sSpeakerWrite(const int16_t* pcm, size_t sampleCount) {
  if (s_txHandle == nullptr) {
    return false;
  }
  if (!s_txEnabled && !i2sChannelEnable()) {
    // enableできない間ここがビジーループになると、他タスクを圧迫したうえに
    // ログも溢れる。実時間ペースのブロッキングが効かないので明示的に待つ。
    delay(10);
    return false;
  }

  size_t bytesToWrite = sampleCount * sizeof(int16_t);
  size_t bytesWritten = 0;
  esp_err_t err = i2s_channel_write(s_txHandle, pcm, bytesToWrite, &bytesWritten, portMAX_DELAY);
  if (err != ESP_OK) {
    if (!s_writeErrorLogged) {
      LOGE("channel write failed: %d", static_cast<int>(err));
      s_writeErrorLogged = true;
    }
    if (err == ESP_ERR_INVALID_STATE) {
      s_txEnabled = false;  // 次回の呼び出しでenableし直す
    }
    delay(10);
    return false;
  }
  s_writeErrorLogged = false;
  return bytesWritten == bytesToWrite;
}

// aac_player.cpp::onAacFrame()からenqueueされたPCMバイト列。
uint8_t s_pcmRingStorage[config::kPcmRingBufferBytes + 1];
StaticStreamBuffer_t s_pcmRingBufferStruct;
StreamBufferHandle_t s_pcmRingBuffer = nullptr;

// i2s_writerタスクのTCBとスタックも静的確保にする(s_pcmRingBufferと同じ理由)。
// 起動時に1回作って二度と消さないタスクなので、ヒープに置く意味が無く、
// xTaskCreateStaticPinnedToCoreなら確保失敗そのものが起こらない。
// StackType_t=uint8_tなのでスタック長の単位はバイト。
StackType_t s_writerTaskStack[config::kI2sWriterTaskStackBytes];
StaticTask_t s_writerTaskBuffer;

// チャンネル切り替え時、旧チャンネル分を読み捨てるためのフラグ。
//
// 当初の脱出条件は「リングが一瞬空になること」だったが、これは成立しないことがある。
// writerStep()の引き抜きは実時間ペース(1024バイトを約10ms)なのに対し、
// xStreamBufferSend()でブロックしているデコードタスク(core0)は起床即メモリコピーで
// 詰め直すため、両者が並列に走るとリングは永久に空にならない。実機で36秒間滞留し
// 3.4MBを捨て続けた(その間ずっと無音)。
//
// そこで「空になるまで待つ」のをやめ、破棄モードに入った時点でリングにあったバイト数を
// 数え、その分だけ捨てて抜けるようにした。StreamBufferはFIFOなので、この時点より前に
// 入っていたバイト = 旧チャンネル分、後から入るバイト = 新チャンネル分と正確に分かれる。
// 詰め直しの速さに一切依存しないので、レースそのものが無くなる。
std::atomic<bool> s_pcmDiscardMode{false};

// i2sSpeakerEnqueue()を受け付けるか。i2sSpeakerFlush()でfalse、i2sSpeakerResume()でtrue。
// 破棄バイト数を確定した後に旧チャンネルのフレームが着地するのを防ぐためのもの。
// 詳細はi2s_speaker.hのi2sSpeakerFlush()のコメントを参照。
std::atomic<bool> s_acceptEnqueue{true};

// 最後に実PCM(無音埋めではない)をI2Sへ書き込んだmillis()。0はまだ一度も再生していない。
// writerTaskが書き、i2sSpeakerIsPlaying()が読む。
std::atomic<uint32_t> s_lastRealWriteMs{0};

// チャンネル内では不変という前提(切り替え時のみ変化)。ミッドストリームでのレート変化には
// 対応していない(その場合、リングバッファ末尾の未書き込みPCMが新レートで再生され、
// 一瞬ピッチがずれる)。
std::atomic<uint32_t> s_pendingSampleRateHz{0};

// --- 診断用の統計(LOG_LEVEL_SPKがDEBUG以上のときだけ動く) ---
// 「PCMは実時間ペースで作られているのに無音」という状態が、
//   (1) 破棄モードに居座って無音だけを書いている
//   (2) 実音として書いてはいるが中身が無音(ピーク≒0)
// のどちらなのかを1行で判別するためのもの。1秒に1行出す。
//
// 集計そのもの(特にstatNotePeakのサンプル走査)も#ifで囲ってある。ESP_LOGDは
// レベルで消えるが、消えるのはログ出力だけで集計のコストは残ってしまうため。
#define SPK_STATS_ENABLED (LOG_MODULE_LEVEL >= LOG_LEVEL_DEBUG)

#if SPK_STATS_ENABLED
uint32_t s_statWindowStartMs = 0;
uint32_t s_statRealBytes = 0;
uint32_t s_statSilenceBytes = 0;
uint32_t s_statDiscardedBytes = 0;
int32_t s_statPeak = 0;

void statNotePeak(const int16_t* pcm, size_t sampleCount) {
  int32_t peak = s_statPeak;
  for (size_t i = 0; i < sampleCount; ++i) {
    int32_t v = pcm[i];  // -32768の符号反転がint16で溢れるのでint32で持つ
    if (v < 0) {
      v = -v;
    }
    if (v > peak) {
      peak = v;
    }
  }
  s_statPeak = peak;
}

void statReport() {
  uint32_t now = millis();
  if (s_statWindowStartMs == 0) {
    s_statWindowStartMs = now;
    return;
  }
  if (now - s_statWindowStartMs < 1000) {
    return;
  }
  LOGD("1s: real=%uB silence=%uB discarded=%uB peak=%d rate=%uHz ring=%uB", s_statRealBytes, s_statSilenceBytes,
       s_statDiscardedBytes, static_cast<int>(s_statPeak), static_cast<unsigned>(s_currentSampleRateHz),
       static_cast<unsigned>(xStreamBufferBytesAvailable(s_pcmRingBuffer)));
  s_statWindowStartMs = now;
  s_statRealBytes = 0;
  s_statSilenceBytes = 0;
  s_statDiscardedBytes = 0;
  s_statPeak = 0;
}

#define SPK_STAT_ADD(counter, n) ((counter) += (n))
#define SPK_STAT_PEAK(pcm, samples) statNotePeak((pcm), (samples))
#define SPK_STAT_REPORT() statReport()
#else
#define SPK_STAT_ADD(counter, n) ((void)0)
#define SPK_STAT_PEAK(pcm, samples) ((void)0)
#define SPK_STAT_REPORT() ((void)0)
#endif

// 破棄モードの進行状態(writerTaskからのみ触る)。
bool s_discardInProgress = false;
uint32_t s_discardStartMs = 0;
uint32_t s_discardTotalBytes = 0;
size_t s_discardRemainingBytes = 0;

// s_pcmRingBufferを実際のI2S出力へ渡す専用タスク。ここだけが実時間ペースでブロックする。
void writerStep() {
  static uint8_t buf[config::kPcmWriterChunkBytes];
  static size_t carry = 0;  // 4byte(1ステレオint16フレーム)未満の端数
  static const int16_t kSilence[config::kPcmWriterChunkBytes / sizeof(int16_t)] = {0};

  if (s_pcmDiscardMode.load(std::memory_order_relaxed)) {
    if (!s_discardInProgress) {
      s_discardInProgress = true;
      s_discardStartMs = millis();
      s_discardTotalBytes = 0;
      // ここで確定させた分だけを捨てる。これ以降にenqueueされるのは新チャンネルの
      // PCMなので残す。
      //
      // サンプル境界について: 今までにenqueueされた総バイト数E、I2Sへ書いた総バイト数W、
      // 手元の端数carryとすると、リング内バイト数は P = E - W - carry。EとWはどちらも
      // 4の倍数なので P + carry も4の倍数になる。したがって「carryを捨てて、Pをちょうど
      // 捨てる」と、次に読むバイトは必ず新しいサンプルの先頭に揃う。
      s_discardRemainingBytes = xStreamBufferBytesAvailable(s_pcmRingBuffer);
      carry = 0;
      // DMAに積まれた旧チャンネル分(24kHzで約60ms)も併せて捨てる。これをやらないと、
      // リングを正しく破棄しても切り替え直後に旧チャンネルの尻尾が鳴る。
      i2sDropDmaBuffer();
      LOGD("pcm discard start (ring=%uB)", static_cast<unsigned>(s_discardRemainingBytes));
    }

    size_t want = s_discardRemainingBytes < sizeof(buf) ? s_discardRemainingBytes : sizeof(buf);
    size_t n = want > 0 ? xStreamBufferReceive(s_pcmRingBuffer, buf, want, 0) : 0;
    s_discardTotalBytes += n;
    SPK_STAT_ADD(s_statDiscardedBytes, n);
    s_discardRemainingBytes -= n;

    const uint32_t elapsedMs = millis() - s_discardStartMs;
    const bool timedOut = elapsedMs >= config::kPcmDiscardTimeoutMs;
    if (s_discardRemainingBytes == 0 || timedOut) {
      s_pcmDiscardMode.store(false, std::memory_order_relaxed);
      if (timedOut) {
        // 正常時はリング1杯分を捨てれば抜けるので、ここには来ない。来た場合は
        // 捨て残しの分だけサンプル境界がずれるが、無音が続くよりはましなので打ち切る。
        LOGE("pcm discard timed out: %uB in %ums, %uB left, forcing exit", s_discardTotalBytes,
             static_cast<unsigned>(elapsedMs), static_cast<unsigned>(s_discardRemainingBytes));
      } else {
        LOGI("pcm discard done: %uB in %ums", s_discardTotalBytes, static_cast<unsigned>(elapsedMs));
      }
      s_discardRemainingBytes = 0;
      s_discardInProgress = false;
    }
    // 破棄中(n>0でまだ旧チャンネルの音が残っている間)も、破棄完了直後(n==0)も、
    // 実音の代わりに無音を書く。ここで何も書き込まないと、I2S DMAはflush直前に
    // 書き込んだ旧チャンネルの音を無音を挟まずそのままループ送信し続けてしまう
    // (ハードウェアのアンダーラン挙動)。i2sSpeakerWrite()はportMAX_DELAYで
    // ブロックして実時間ペースになるので、ここでの破棄も自然に実時間ペースになる。
    i2sSpeakerWrite(kSilence, sizeof(kSilence) / sizeof(int16_t));
    SPK_STAT_ADD(s_statSilenceBytes, sizeof(kSilence));
    return;
  }

  // タイムアウトは0にすること。ここで待つと、待っている間I2Sへ何も書かれず、DMAは
  // 直前に書き込んだバッファを無音を挟まずループ送信し続ける(ハードウェアのアンダー
  // ラン挙動)。以前は50ms待ってから1024バイトの無音を書いていたが、24kHzステレオでは
  // 1024バイト=10.6ms分しかなく、時間の約8割はDMAのループが鳴っていた。チャンネル
  // 切り替え直後にこれが起きると、旧チャンネルの断片が短いループで聞こえる。
  // タイムアウト0なら、データが無い間はi2sSpeakerWrite()のブロッキング(実時間ペース)
  // だけがループを律速するので、無音が途切れなく供給される。
  size_t want = sizeof(buf) - carry;
  size_t n = xStreamBufferReceive(s_pcmRingBuffer, buf + carry, want, 0);

  size_t have = carry + n;
  size_t alignedLen = have - (have % 4);  // 1ステレオint16フレーム = 4byte単位でしか書けない
  if (alignedLen == 0) {
    // データが無い、または1サンプルに満たない端数しか無い。DMAのループ元を無音で
    // 上書きし続ける。端数(have)はcarryとして次回に持ち越す。
    i2sSpeakerWrite(kSilence, sizeof(kSilence) / sizeof(int16_t));
    SPK_STAT_ADD(s_statSilenceBytes, sizeof(kSilence));
    carry = have;
    return;
  }

  {
    i2sSpeakerSetSampleRate(s_pendingSampleRateHz.load(std::memory_order_relaxed));
    SPK_STAT_PEAK(reinterpret_cast<const int16_t*>(buf), alignedLen / sizeof(int16_t));
    bool played = i2sSpeakerWrite(reinterpret_cast<const int16_t*>(buf), alignedLen / sizeof(int16_t));
    if (played) {
      s_lastRealWriteMs.store(millis(), std::memory_order_relaxed);
      SPK_STAT_ADD(s_statRealBytes, alignedLen);
    }
    if (played && !g_firstPlaybackLogged) {
      LOGI("i2sSpeakerWrite: playback started");
      g_firstPlaybackLogged = true;
    }
  }
  carry = have - alignedLen;
  if (carry > 0) {
    memmove(buf, buf + alignedLen, carry);
  }
}

void writerTaskFn(void* /*param*/) {
  for (;;) {
    writerStep();
    // writerStep()には早期returnが複数あるので、集計の出力はここでまとめて行う。
    SPK_STAT_REPORT();
  }
}

}  // namespace

bool i2sSpeakerBegin(i2s_chan_handle_t txHandle) {
  if (txHandle == nullptr) {
    return false;
  }
  s_txHandle = txHandle;
  if (!i2sChannelEnable()) {
    return false;
  }
  // txHandle生成時のクロック設定値は呼び出し元(ハードウェア初期化側)の都合で決まった
  // 仮の値でしかないため、未確定として扱う。最初の実データがenqueueされた時点で
  // i2sSpeakerSetSampleRate()が実際のレートへ自然にreconfigする。
  s_currentSampleRateHz = 0;

  s_pcmRingBuffer = xStreamBufferCreateStatic(config::kPcmRingBufferBytes, 1, s_pcmRingStorage, &s_pcmRingBufferStruct);
  if (s_pcmRingBuffer == nullptr) {
    LOGE("PCM ring xStreamBufferCreateStatic failed (should not happen; static alloc)");
    return false;
  }

  xTaskCreateStaticPinnedToCore(writerTaskFn, "i2s_writer", config::kI2sWriterTaskStackBytes, nullptr,
                                config::kI2sWriterTaskPriority, s_writerTaskStack, &s_writerTaskBuffer,
                                config::kI2sWriterTaskCore);
  return true;
}

bool i2sSpeakerEnqueue(const int16_t* pcm, size_t sampleCount, uint32_t sampleRateHz) {
  s_pendingSampleRateHz.store(sampleRateHz, std::memory_order_relaxed);

  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pcm);
  size_t len = sampleCount * sizeof(int16_t);

  // 1フレームは「全部書く」か「1バイトも書かない」かのどちらかにすること。
  // xStreamBufferSendはバイト単位なので、途中まで書いて中断するとリングに
  // 「4バイト(1ステレオサンプル)の途中まで」が残り、以降のPCM全体が1〜3バイト
  // ずれてint16として解釈される。上下バイトが入れ替わった波形はホワイトノイズになる。
  //
  // Producerはこのデコードタスク1本だけなので、空きを確認してから書けば
  // xStreamBufferSendが途中で止まることはない(空きは読み出し側が増やす一方)。
  while (xStreamBufferSpacesAvailable(s_pcmRingBuffer) < len) {
    if (!s_acceptEnqueue.load(std::memory_order_relaxed)) {
      return false;  // まだ1バイトも書いていないので、フレームごと安全に捨てられる
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (!s_acceptEnqueue.load(std::memory_order_relaxed)) {
    return false;
  }

  return xStreamBufferSend(s_pcmRingBuffer, bytes, len, portMAX_DELAY) == len;
}

void i2sSpeakerFlush() {
  LOGD("flush requested (ring=%uB)", static_cast<unsigned>(xStreamBufferBytesAvailable(s_pcmRingBuffer)));
  // 先に受け付けを止める。これ以降、旧チャンネルのフレームはリングへ入らない。
  s_acceptEnqueue.store(false, std::memory_order_relaxed);
  // 捨てるバイト数の確定はwriterStep()側が行う。ここで数えてしまうと、writerTaskが
  // i2s_channel_write()でブロックしている間(最大約10ms)に進んだ分とズレる。
  s_pcmDiscardMode.store(true, std::memory_order_relaxed);
}

void i2sSpeakerResume() { s_acceptEnqueue.store(true, std::memory_order_relaxed); }

bool i2sSpeakerIsPlaying() {
  if (s_pcmDiscardMode.load(std::memory_order_relaxed)) {
    return false;
  }
  const uint32_t last = s_lastRealWriteMs.load(std::memory_order_relaxed);
  if (last == 0) {
    return false;  // まだ一度も実PCMを書いていない
  }
  return (millis() - last) < config::kSpeakerPlayingTimeoutMs;
}
