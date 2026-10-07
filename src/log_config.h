#pragma once

#include <esp_log.h>

#include <cstdlib>  // abort() (ASSERT用)

// モジュールごとのコンパイル時ログレベル。
//
// 出力はESP-IDFのesp_log_write()に流すので、タイムスタンプ・タグ・ランタイムの
// タグ別フィルタはIDFのものがそのまま効く。ただしマクロ名はESP_LOGxではなく
// 独自のLOGE/LOGW/LOGI/LOGD/LOGVを使う。理由:
//
//   arduino-esp32のcores/esp32/esp32-hal-log.h(Arduino.h経由で必ず入る)が
//   ESP_LOGE/W/I/D/Vを#undefして自前のlog_x()へ再定義してしまう。その定義は
//   CORE_DEBUG_LEVELだけで制御され、LOG_LOCAL_LEVELを完全に無視する。
//   つまりArduinoフレームワーク上ではESP_LOGxにファイル単位のレベルを効かせられない。
//
// 独自名なら#undefされないので、include順にも依存しない。
//
// 使い方(このヘッダのincludeより前に2行を定義する):
//
//   #define LOG_MODULE_LEVEL LOG_LEVEL_SPK
//   #define LOG_MODULE_TAG "SPK"
//   #include "log_config.h"
//
//   LOGI("sample rate changed: %u -> %uHz", from, to);
//
// レベルで無効になったマクロは #if で ((void)0) に置き換わるので、引数の評価も
// フォーマット文字列のFlash常駐も残らない(最適化任せではない)。

#define LOG_LEVEL_NONE 0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN 2
#define LOG_LEVEL_INFO 3
#define LOG_LEVEL_DEBUG 4
#define LOG_LEVEL_VERBOSE 5

// 各モジュールの既定レベル。platformio.iniのbuild_flagsで個別に上書きできる。
//   -DLOG_LEVEL_SPK=LOG_LEVEL_DEBUG
//
// 既定をINFOにしてあるモジュールでは、毎秒/毎セグメント出るような高頻度のログを
// DEBUG以下に置いてある。
//
// 特にLOG_LEVEL_SPKは、I2Sライタタスク(優先度2、実時間)から出るログを含むので
// 安易に上げないこと。main.cppがSerial.setTxTimeoutMs(0)を呼んでいるためUSB
// CDCの 書き込みでブロックはしない(溢れた分は捨てられる)が、実時間タスクでの
// vsnprintf+書き込みのコストは残るし、他タスクのログを押し出してしまう。

#ifndef LOG_LEVEL_MAIN
#define LOG_LEVEL_MAIN LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_HLS  // radio_stream.cpp(プレイリスト追跡・セグメント再生)
#define LOG_LEVEL_HLS LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_PLS  // hls_playlist.cpp(プレイリストのHTTP取得・パース)
#define LOG_LEVEL_PLS LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_SPK  // i2s_speaker.cpp(実時間タスクを含む。上のコメント参照)
#define LOG_LEVEL_SPK LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_AAC  // aac_player.cpp
#define LOG_LEVEL_AAC LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_MRS  // morse_code.cpp(モールス符号の通知音)
#define LOG_LEVEL_MRS LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_MP3  // mp3_player.cpp(SDからのMP3再生)
#define LOG_LEVEL_MP3 LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_DSY  // daisy_player.cpp(ncc/SMILの解析・再生位置の管理・本の選択)
#define LOG_LEVEL_DSY LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_RDK  // radiko_client.cpp / radiko_channel_source.cpp
#define LOG_LEVEL_RDK LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_NHK  // nhk_channel_source.cpp
#define LOG_LEVEL_NHK LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_WIFI  // wifi_manager.cpp
#define LOG_LEVEL_WIFI LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_STORE  // channel_store.cpp / volume_store.cpp
#define LOG_LEVEL_STORE LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_TIME  // time_sync.cpp
#define LOG_LEVEL_TIME LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_HW  // es8311.cpp / tca8418.cpp
#define LOG_LEVEL_HW LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_WS  // websocket_client.cpp
#define LOG_LEVEL_WS LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_JCBA  // jcba_client.cpp(select_stream API / Ogg
                        // ページの流し読み)
#define LOG_LEVEL_JCBA LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_OPUS  // opus_player.cpp
#define LOG_LEVEL_OPUS LOG_LEVEL_INFO
#endif

#ifndef LOG_LEVEL_TS  // ts_demux.cpp(ListenRadio等のMPEG-TSセグメントからの音声ES抽出)
#define LOG_LEVEL_TS LOG_LEVEL_INFO
#endif

// 呼び出し元が指定しなかった場合の既定。
#ifndef LOG_MODULE_LEVEL
#define LOG_MODULE_LEVEL LOG_LEVEL_INFO
#endif
#ifndef LOG_MODULE_TAG
#define LOG_MODULE_TAG "APP"
#endif

// IDFの既定書式 "<letter> (<ms>) <tag>: <message>" に合わせてある。
#define LOG_WRITE_(lvl, letter, fmt, ...)                                                                         \
  esp_log_write(static_cast<esp_log_level_t>(lvl), LOG_MODULE_TAG, letter " (%lu) " LOG_MODULE_TAG ": " fmt "\n", \
                static_cast<unsigned long>(esp_log_timestamp()), ##__VA_ARGS__)

#if LOG_MODULE_LEVEL >= LOG_LEVEL_ERROR
#define LOGE(fmt, ...) LOG_WRITE_(LOG_LEVEL_ERROR, "E", fmt, ##__VA_ARGS__)
#else
#define LOGE(fmt, ...) ((void)0)
#endif

#if LOG_MODULE_LEVEL >= LOG_LEVEL_WARN
#define LOGW(fmt, ...) LOG_WRITE_(LOG_LEVEL_WARN, "W", fmt, ##__VA_ARGS__)
#else
#define LOGW(fmt, ...) ((void)0)
#endif

#if LOG_MODULE_LEVEL >= LOG_LEVEL_INFO
#define LOGI(fmt, ...) LOG_WRITE_(LOG_LEVEL_INFO, "I", fmt, ##__VA_ARGS__)
#else
#define LOGI(fmt, ...) ((void)0)
#endif

#if LOG_MODULE_LEVEL >= LOG_LEVEL_DEBUG
#define LOGD(fmt, ...) LOG_WRITE_(LOG_LEVEL_DEBUG, "D", fmt, ##__VA_ARGS__)
#else
#define LOGD(fmt, ...) ((void)0)
#endif

#if LOG_MODULE_LEVEL >= LOG_LEVEL_VERBOSE
#define LOGV(fmt, ...) LOG_WRITE_(LOG_LEVEL_VERBOSE, "V", fmt, ##__VA_ARGS__)
#else
#define LOGV(fmt, ...) ((void)0)
#endif

// 内部インバリアント(「成立していて当然、崩れていたらロジックバグ」)の検査。
//
// 使い分け:
//   - 外部要因で失敗しうるもの(I2C/ネットワーク/NVS/ユーザー入力など)は
//     LOGE + 通常の分岐で回復させる。ASSERTは使わない。
//   -
//   呼び出し規約や不変条件(「この時点でindexは必ず範囲内」等)はASSERTで表明する。
//     実行時チェックを各所に散らす代わりに、意図をコードに残しつつリリースでは消せる。
//
// 失敗するとfile:line付きでログを出してabort()する(パニックハンドラがバックトレースを
// 吐く。monitor_filters=esp32_exception_decoderでデコードされる)。
// build_flagsに -DM5RAJIRU_NDEBUG を足すと ((void)0)
// に潰れ、条件式も評価されない (副作用を書かないこと)。
#ifdef M5RAJIRU_NDEBUG
#define ASSERT(cond) ((void)sizeof(cond))
#else
#define ASSERT(cond)                                                                                        \
  do {                                                                                                      \
    if (!(cond)) {                                                                                          \
      esp_log_write(ESP_LOG_ERROR, LOG_MODULE_TAG, "E (%lu) " LOG_MODULE_TAG ": ASSERT(%s) failed %s:%d\n", \
                    static_cast<unsigned long>(esp_log_timestamp()), #cond, __FILE__, __LINE__);            \
      abort();                                                                                              \
    }                                                                                                       \
  } while (0)
#endif

// このアプリのタグについて、ESP-IDFのランタイムフィルタを全開にする。
//
// esp_log_write()はタグ別のランタイムレベル(既定はCORE_DEBUG_LEVEL相当)でも
// 絞られるため、コンパイル時に通したDEBUG/VERBOSEがここで落ちてしまう。自前の
// タグだけを開けておけば、実効的な制御はコンパイル時レベル一本になる
// ("*"を開けるとIDF内部の大量のログまで出るのでやらない)。
//
// setup()のSerial.begin()直後に1回呼ぶこと。
void logInit();
