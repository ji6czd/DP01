#include <Arduino.h>

#include "config.h"
#include "es8311.h"
#include "i2s_speaker.h"
#include "morse_code.h"
#include "mp3_player.h"
#include "tca8418.h"

namespace {

i2s_chan_handle_t txHandle = nullptr;
}  // namespace
void setup() {
  using namespace config;
  Serial.begin(115200);
  delay(1000);  // Wait for serial port to initialize
  es8311Begin(kSpeakerVolume);
  txHandle = es8311CreateI2sTxChannel();
  i2sSpeakerBegin(txHandle);
  morseBegin();
  morsePlay("S");
  tca8418Begin();
  mp3PlayerBegin();
}

void loop() {
  KeyEvent keyEvent;
  keyEvent = tca8418ReadKeyEvent();
  if (keyEvent.state) {
    Serial.printf("key: %02d\n", keyEvent.key);
    switch (keyEvent.key) {
      case config::kKeyPrevTrack:
        mp3PlayerPrev();
        break;
      case config::kKeyPlayStop:
        mp3PlayerTogglePause();
        break;
      case config::kKeyNextTrack:
        mp3PlayerNext();
        break;
    }
  }
}