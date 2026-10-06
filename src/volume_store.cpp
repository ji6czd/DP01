// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_STORE
#define LOG_MODULE_TAG "STORE"
#include "volume_store.h"

#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include "log_config.h"

namespace {

constexpr char kNamespace[] = "m5rajiru";
constexpr char kKeyVolume[] = "volume";

Preferences s_prefs;
bool s_dirty = false;
uint8_t s_pendingVolume = 0;
uint32_t s_lastChangeMs = 0;

}  // namespace

uint8_t volumeStoreLoad(uint8_t defaultVolume) {
  s_prefs.begin(kNamespace, /*readOnly=*/true);
  uint8_t volume = s_prefs.getUChar(kKeyVolume, defaultVolume);
  s_prefs.end();
  return volume;
}

void volumeStoreNotifyChanged(uint8_t volume) {
  s_pendingVolume = volume;
  s_dirty = true;
  s_lastChangeMs = millis();
}

void volumeStoreTick() {
  if (!s_dirty) {
    return;
  }
  if (millis() - s_lastChangeMs < config::kVolumeSaveDebounceMs) {
    return;
  }
  s_prefs.begin(kNamespace, /*readOnly=*/false);
  s_prefs.putUChar(kKeyVolume, s_pendingVolume);
  s_prefs.end();
  s_dirty = false;
  LOGI("saved volume=0x%02X", s_pendingVolume);
}
