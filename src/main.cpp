#include <Arduino.h>

#include "config.h"
#include "es8311.h"
#include "i2s_speaker.h"
#include "log_config.h"
#include "morse_code.h"
#include "mp3_player.h"
#include "tca8418.h"

namespace {

i2s_chan_handle_t txHandle = nullptr;

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
  es8311Begin(kSpeakerVolume);
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
  delay(50);
}
