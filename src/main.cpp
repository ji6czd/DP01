#include <Arduino.h>

#include <algorithm>

#include "config.h"
#include "es8311.h"
#include "i2s_speaker.h"
#include "log_config.h"
#include "morse_code.h"
#include "mp3_player.h"
#include "tca8418.h"
#include "volume_store.h"

namespace {

i2s_chan_handle_t txHandle = nullptr;

uint8_t clampVolume(int volume) {
  return static_cast<uint8_t>(std::clamp(volume,
                                         static_cast<int>(config::kVolumeMin),
                                         static_cast<int>(config::kVolumeMax)));
}

// 音量をdelta段(config::kVolumeStep単位)動かす。範囲の端では止まる。
// NVSへの保存はvolumeStoreTick()がデバウンスしてから行う。
void stepVolume(int delta) {
  const uint8_t current = es8311GetVolume();
  const uint8_t volume =
      clampVolume(static_cast<int>(current) + delta * config::kVolumeStep);
  if (volume == current) {
    return;
  }
  es8311SetVolume(volume);
  volumeStoreNotifyChanged(volume);
  Serial.printf("volume: 0x%02X\n", volume);
}

void handleKeyEvent(const KeyEvent& keyEvent) {
  if (keyEvent.state) {
    Serial.printf("key: %02d\n", keyEvent.key);
    switch (keyEvent.key) {
      case config::kKeyPrevPhrase:
        mp3PlayerPrevPhrase();
        break;
      case config::kKeyNextPhrase:
        mp3PlayerNextPhrase();
        break;
      case config::kKeyPrevHeading:
        mp3PlayerPrevHeading();
        break;
      case config::kKeyNextHeading:
        mp3PlayerNextHeading();
        break;
      case config::kKeyHeadingStart:
        mp3PlayerHeadingStart();
        break;
      case config::kKeyLevelUp:
        mp3PlayerHeadingLevelUp();
        break;
      case config::kKeyLevelDown:
        mp3PlayerHeadingLevelDown();
        break;
      case config::kKeyPlayStop:
        mp3PlayerTogglePause();
        break;
      case config::kKeySlower:
        mp3PlayerSlower();
        break;
      case config::kKeyFaster:
        mp3PlayerFaster();
        break;
      case config::kKeyVolumeDown:
        stepVolume(-1);
        break;
      case config::kKeyVolumeUp:
        stepVolume(+1);
        break;
    }
  }
}

}  // namespace

void setup() {
  using namespace config;
  Serial.begin(115200);
  Serial.setTxTimeoutMs(
      0);     // USB CDCにホストが居なくてもログ出力でタスクをブロックしない
  logInit();  // 自前タグのランタイムログフィルタを開ける
  // 範囲外の保存値(範囲を狭めた後など)は範囲内へ寄せる。
  es8311Begin(clampVolume(volumeStoreLoad(kSpeakerVolume)));
  txHandle = es8311CreateI2sTxChannel();
  i2sSpeakerBegin(txHandle);
  morseBegin();
  tca8418Begin();
  mp3PlayerBegin();
  morsePlay("S");
}

void loop() {
  KeyEvent keyEvent = tca8418ReadKeyEvent();
  handleKeyEvent(keyEvent);
  volumeStoreTick();
  delay(50);
}
