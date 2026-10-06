// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_MP3
#define LOG_MODULE_TAG "MP3"
#include "mp3_player.h"

#include <Arduino.h>
#include <FS.h>
#include <MP3DecoderHelix.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "config.h"
#include "i2s_speaker.h"
#include "log_config.h"

namespace {

enum class Command : uint8_t { kPrev, kNext, kTogglePause };

QueueHandle_t s_commandQueue = nullptr;
String s_directory;
std::vector<String> s_tracks;
size_t s_trackIndex = 0;
File s_file;
bool s_paused = false;
libhelix::MP3DecoderHelix s_decoder;
uint8_t s_readBuf[config::kMp3ReadChunkBytes];

// モノ→ステレオ複製用。1回のenqueueあたりのステレオフレーム数。
constexpr size_t kStereoChunkFrames = 256;
int16_t s_stereoBuf[kStereoChunkFrames * 2];

// Helixのデコード結果。pcmLenはチャンネルを含む総サンプル数(info.outputSamps)。
// i2s_speakerはLRインターリーブのステレオを期待するので、モノは左右へ複製する。
void onPcm(MP3FrameInfo& info, short* pcm, size_t pcmLen, void*) {
  const uint32_t rate = static_cast<uint32_t>(info.samprate);
  if (info.nChans == 2) {
    i2sSpeakerEnqueue(pcm, pcmLen, rate);
    return;
  }
  size_t pos = 0;
  while (pos < pcmLen) {
    const size_t frames = std::min(kStereoChunkFrames, pcmLen - pos);
    for (size_t i = 0; i < frames; i++) {
      s_stereoBuf[i * 2] = pcm[pos + i];
      s_stereoBuf[i * 2 + 1] = pcm[pos + i];
    }
    i2sSpeakerEnqueue(s_stereoBuf, frames * 2, rate);
    pos += frames;
  }
}

// directory直下のa*.mp3を名前順に列挙してs_tracksへ入れる。openTrack()のためdirectoryはs_directoryに保持する。
bool buildPlaylist(const char* directory) {
  s_directory = directory;
  s_tracks.clear();
  File dir = SD.open(directory);
  if (!dir || !dir.isDirectory()) {
    LOGE("directory not found: %s", directory);
    return false;
  }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) {
      continue;
    }
    String name = f.name();  // 新しいコアではパスを含まないファイル名
    name.toLowerCase();
    if (name.length() > 0 && name[0] == config::kMp3FilePrefix &&
        name.endsWith(".mp3")) {
      s_tracks.push_back(String(f.name()));
    }
  }
  dir.close();
  std::sort(
      s_tracks.begin(), s_tracks.end(),
      [](const String& a, const String& b) { return a.compareTo(b) < 0; });
  LOGI("%u tracks in %s", static_cast<unsigned>(s_tracks.size()), directory);
  return !s_tracks.empty();
}

void closeTrack() {
  if (s_file) {
    s_file.close();
  }
}

// s_trackIndexのファイルを開き、デコーダを初期状態にする。
bool openTrack() {
  closeTrack();
  s_decoder.end();
  String path = s_directory + "/" + s_tracks[s_trackIndex];
  s_file = SD.open(path, FILE_READ);
  if (!s_file) {
    LOGE("open failed: %s", path.c_str());
    return false;
  }
  s_decoder.begin();
  LOGI("play [%u/%u] %s (%u bytes)", static_cast<unsigned>(s_trackIndex + 1),
       static_cast<unsigned>(s_tracks.size()), path.c_str(),
       static_cast<unsigned>(s_file.size()));
  return true;
}

// ユーザ操作による曲送り。旧曲の残りPCMを捨て、新曲のPCMだけが流れるようにする。
void changeTrack(int delta) {
  const size_t n = s_tracks.size();
  s_trackIndex = (s_trackIndex + n + delta) % n;
  // Flush後はResumeまでenqueueが捨てられるので、デコーダのend()が吐く旧曲の残りも鳴らない。
  i2sSpeakerFlush();
  openTrack();
  i2sSpeakerResume();
  s_paused = false;
}

void playerTaskFn(void*) {
  vTaskDelay(pdMS_TO_TICKS(config::kMp3StartDelayMs));
  openTrack();

  for (;;) {
    const bool playing = s_file && !s_paused;
    Command cmd;
    if (xQueueReceive(s_commandQueue, &cmd, playing ? 0 : portMAX_DELAY) ==
        pdTRUE) {
      switch (cmd) {
        case Command::kPrev:
          changeTrack(-1);
          break;
        case Command::kNext:
          changeTrack(+1);
          break;
        case Command::kTogglePause:
          s_paused = !s_paused;
          LOGI("%s", s_paused ? "paused" : "resumed");
          break;
      }
      continue;
    }
    if (!playing) {
      continue;
    }

    const int n = s_file.read(s_readBuf, sizeof(s_readBuf));
    if (n > 0) {
      s_decoder.write(s_readBuf, n);
      continue;
    }
    // EOF。末尾のPCMは切らずに(Flushせず)次の曲へ進む。
    s_trackIndex = (s_trackIndex + 1) % s_tracks.size();
    openTrack();
  }
}

void sendCommand(Command cmd) {
  if (s_commandQueue != nullptr) {
    xQueueSend(s_commandQueue, &cmd, 0);  // 満杯なら捨てる
  }
}

}  // namespace

bool mp3PlayerBegin() {
  SPI.begin(config::kSdSckPin, config::kSdMisoPin, config::kSdMosiPin,
            config::kSdCsPin);
  if (!SD.begin(config::kSdCsPin, SPI, config::kSdSpiHz)) {
    LOGE("SD mount failed (card not inserted?)");
    return false;
  }
  if (!buildPlaylist(config::kMp3Directory)) {
    return false;
  }
  s_decoder.setDataCallback(onPcm);

  s_commandQueue = xQueueCreate(config::kMp3CommandQueueDepth, sizeof(Command));
  if (s_commandQueue == nullptr) {
    LOGE("queue create failed");
    return false;
  }
  if (xTaskCreatePinnedToCore(playerTaskFn, "mp3", config::kMp3TaskStackBytes,
                              nullptr, config::kMp3TaskPriority, nullptr,
                              config::kMp3TaskCore) != pdPASS) {
    LOGE("task create failed");
    return false;
  }
  return true;
}

void mp3PlayerPrev() { sendCommand(Command::kPrev); }
void mp3PlayerNext() { sendCommand(Command::kNext); }
void mp3PlayerTogglePause() { sendCommand(Command::kTogglePause); }
