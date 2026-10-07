#include <Arduino.h>

#include <algorithm>

#include "config.h"
#include "daisy_player.h"
#include "es8311.h"
#include "i2s_speaker.h"
#include "log_config.h"
#include "morse_code.h"
#include "tca8418.h"
#include "volume_store.h"

namespace {

i2s_chan_handle_t txHandle = nullptr;

uint8_t clampVolume(int volume) {
  return static_cast<uint8_t>(
      std::clamp(volume, static_cast<int>(config::kVolumeMin), static_cast<int>(config::kVolumeMax)));
}

// 音量をdelta段(config::kVolumeStep単位)動かす。範囲の端では止まる。
// NVSへの保存はvolumeStoreTick()がデバウンスしてから行う。
void stepVolume(int delta) {
  const uint8_t current = es8311GetVolume();
  const uint8_t volume = clampVolume(static_cast<int>(current) + delta * config::kVolumeStep);
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
      case config::kKeyPrevFolder:
        daisyPlayerPrevBook();
        break;
      case config::kKeyNextFolder:
        daisyPlayerNextBook();
        break;
      case config::kKeyPrevPhrase:
        daisyPlayerPrevPhrase();
        break;
      case config::kKeyNextPhrase:
        daisyPlayerNextPhrase();
        break;
      case config::kKeyPrevHeading:
        daisyPlayerPrevHeading();
        break;
      case config::kKeyNextHeading:
        daisyPlayerNextHeading();
        break;
      case config::kKeyHeadingStart:
        daisyPlayerHeadingStart();
        break;
      case config::kKeyLevelUp:
        daisyPlayerHeadingLevelUp();
        break;
      case config::kKeyLevelDown:
        daisyPlayerHeadingLevelDown();
        break;
      case config::kKeyPlayStop:
        daisyPlayerTogglePause();
        break;
      case config::kKeySlower:
        daisyPlayerSlower();
        break;
      case config::kKeyFaster:
        daisyPlayerFaster();
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
  Serial.setTxTimeoutMs(0);  // USB CDCにホストが居なくてもログ出力でタスクをブロックしない
  logInit();                 // 自前タグのランタイムログフィルタを開ける
  // 範囲外の保存値(範囲を狭めた後など)は範囲内へ寄せる。
  es8311Begin(clampVolume(volumeStoreLoad(kSpeakerVolume)));
  txHandle = es8311CreateI2sTxChannel();
  i2sSpeakerBegin(txHandle);
  morseBegin();
  tca8418Begin();
  daisyPlayerBegin();
  morsePlay("S");
}

void loop() {
  KeyEvent keyEvent = tca8418ReadKeyEvent();
  handleKeyEvent(keyEvent);
  volumeStoreTick();
  delay(50);
}
