#include "log_config.h"

#include <esp_log.h>

void logInit() {
  // log_config.hの各モジュールで使っているタグ。ここに挙げ漏れると、そのタグの
  // DEBUG/VERBOSEはコンパイル時に通ってもランタイムフィルタで落ちる。
  static const char* kTags[] = {"MAIN", "HLS",  "PLS",   "SPK", "AAC",  "MRS", "MP3",  "DSY",  "RDK",
                                "NHK",  "WIFI", "STORE", "HW",  "TIME", "WS",  "JCBA", "OPUS", "TS"};
  for (const char* tag : kTags) {
    esp_log_level_set(tag, ESP_LOG_VERBOSE);
  }
}
