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
#include <cstring>
#include <string>

#include "bookmark_store.h"
#include "config.h"
#include "i2s_speaker.h"
#include "log_config.h"
#include "ncc_parser.h"
#include "smil_parser.h"

namespace {

enum class Command : uint8_t {
  kPrevPhrase,
  kNextPhrase,
  kPrevHeading,
  kNextHeading,
  kHeadingStart,
  kLevelUp,
  kLevelDown,
  kTogglePause,
};

// プレーヤタスクの状態。onPcm()もデコーダ経由で同じタスクから呼ばれるので排他は不要。
enum class State : uint8_t {
  kSeeking,  // シーク/オープン中。この間にデコーダが吐くPCM(end()の残り等)は捨てる
  kPlaying,      // クリップを再生中
  kJumpPending,  // 現在クリップが終わり、次(s_clipIndex)は連続していない。タスクがstartClip()する
  kNextSmil,     // 現在のSMILが終わり、次のSMILがある。タスクが読み込んで始める
  kFinished,     // 本の最後まで再生した/エラー
};

QueueHandle_t s_commandQueue = nullptr;
String s_directory;
NccBook s_ncc;           // 本全体(SMILの再生順と見出し)
uint32_t s_bookId = 0;   // しおりでこの本を特定するID(bookIdOf(s_ncc))
SmilBook s_book;         // 今再生中のSMIL
size_t s_smilIndex = 0;  // s_ncc.smilsの添字(s_bookの元)
size_t s_clipIndex = 0;  // s_book.clipsの添字。デコーダが処理中のクリップ
State s_state = State::kFinished;
bool s_paused = false;
uint8_t s_headingLevel =
    config::kHeadingLevelDefault;  // 見出し移動の対象はH1〜これ

File s_file;
int s_openSource = -1;       // s_fileが開いているSmilBook::sourcesの添字
uint32_t s_dataStart = 0;    // 最初のフレーム同期の位置(ID3v2タグの後)
uint32_t s_bytesPerSec = 0;  // CBR前提のバイト/秒

libhelix::MP3DecoderHelix s_decoder;
uint8_t s_readBuf[config::kMp3ReadChunkBytes];

// デコード位置(ファイル先頭からのサンプル数)。シーク時はs_baseMsから始め、レートは最初の
// PCMで分かるのでs_needInitでその時に初期化する。
uint32_t s_baseMs = 0;
bool s_needInit = false;
int64_t s_posSamples = 0;

// モノ→ステレオ複製用。1回のenqueueあたりのステレオフレーム数。
constexpr size_t kStereoChunkFrames = 256;
int16_t s_stereoBuf[kStereoChunkFrames * 2];

int64_t msToSamples(uint32_t ms, uint32_t rate) {
  return static_cast<int64_t>(ms) * rate / 1000;
}

// frames個のフレーム(チャンネルあたり1サンプル)を出力する。
// i2s_speakerはLRインターリーブのステレオを期待するので、モノは左右へ複製する。
void emitFrames(const int16_t* pcm, size_t frames, int nChans, uint32_t rate) {
  if (nChans == 2) {
    i2sSpeakerEnqueue(pcm, frames * 2, rate);
    return;
  }
  size_t pos = 0;
  while (pos < frames) {
    const size_t n = std::min(kStereoChunkFrames, frames - pos);
    for (size_t i = 0; i < n; i++) {
      s_stereoBuf[i * 2] = pcm[pos + i];
      s_stereoBuf[i * 2 + 1] = pcm[pos + i];
    }
    i2sSpeakerEnqueue(s_stereoBuf, n * 2, rate);
    pos += n;
  }
}

void logClip(const char* what) {
  const SmilClip& c = s_book.clips[s_clipIndex];
  LOGI("%s clip %u/%u %s %u-%d ms", what,
       static_cast<unsigned>(s_clipIndex + 1),
       static_cast<unsigned>(s_book.clips.size()),
       s_book.sources[c.srcIndex].c_str(), static_cast<unsigned>(c.beginMs),
       c.endMs == kSmilClipToEnd ? -1 : static_cast<int>(c.endMs));
}

// 現在のSMILの再生が終わった(最終クリップの終端、またはEOF)。
// 次のSMILがあればkNextSmil、本の最後ならkFinishedにする。
void finishSmil() {
  if (s_smilIndex + 1 < s_ncc.smils.size()) {
    s_state = State::kNextSmil;
  } else {
    s_state = State::kFinished;
    LOGI("finished");
    bookmarkRemove(s_bookId);  // 読み終えた本は次回また先頭から
  }
}

// 現在クリップの再生が終わった。次クリップへ進み、そのまま流せるならtrueを返す。
// SMILの最後ならfinishSmil()、同一ファイルでも遠い/戻る場合はkJumpPendingにしてfalseを返す。
bool advanceClip() {
  const SmilClip prev = s_book.clips[s_clipIndex];
  if (s_clipIndex + 1 >= s_book.clips.size()) {
    finishSmil();
    return false;
  }
  s_clipIndex++;
  const SmilClip& next = s_book.clips[s_clipIndex];
  const bool sameStream =
      next.srcIndex == prev.srcIndex && prev.endMs != kSmilClipToEnd &&
      next.beginMs >= prev.endMs &&
      next.beginMs - prev.endMs <= config::kMp3SkipThroughMs;
  if (!sameStream) {
    s_state = State::kJumpPending;
    return false;
  }
  logClip("next");
  return true;
}

// Helixのデコード結果。pcmLenはチャンネルを含む総サンプル数(info.outputSamps)。
// デコード位置を数え、現在クリップの [clip-begin, clip-end)
// に入る部分だけを出力する。
void onPcm(MP3FrameInfo& info, short* pcm, size_t pcmLen, void*) {
  if (s_state != State::kPlaying) {
    return;
  }
  const int nChans = info.nChans > 0 ? info.nChans : 1;
  const uint32_t rate = static_cast<uint32_t>(info.samprate);
  const size_t frames = pcmLen / nChans;

  if (s_needInit) {
    s_posSamples = msToSamples(s_baseMs, rate);
    s_needInit = false;
  }
  const int64_t blockStart = s_posSamples;
  s_posSamples += frames;

  size_t from = 0;
  while (from < frames) {
    const SmilClip& clip = s_book.clips[s_clipIndex];
    const int64_t begin = msToSamples(clip.beginMs, rate);
    const int64_t end = clip.endMs == kSmilClipToEnd
                            ? INT64_MAX
                            : msToSamples(clip.endMs, rate);
    const int64_t cur = blockStart + static_cast<int64_t>(from);
    if (cur < begin) {  // プリロール/クリップ間の捨て区間
      from += static_cast<size_t>(
          std::min<int64_t>(begin - cur, static_cast<int64_t>(frames - from)));
      continue;
    }
    // curがendを越えていることもある(1ブロックが複数クリップにまたがる場合)ので0で下限をとる。
    const size_t n = static_cast<size_t>(std::max<int64_t>(
        0, std::min<int64_t>(end - cur, static_cast<int64_t>(frames - from))));
    emitFrames(pcm + from * nChans, n, nChans, rate);
    from += n;
    if (cur + static_cast<int64_t>(n) >= end && !advanceClip()) {
      return;  // 以降のフレームは次クリップのものではないので捨てる
    }
  }
}

// 先頭のID3v2を飛ばして最初のMPEG Audio Layer
// IIIフレームヘッダを探し、s_dataStartと s_bytesPerSec(CBR前提)を設定する。
bool probeMp3(File& f) {
  static const uint16_t kBitrateKbpsMpeg1[16] = {
      0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
  static const uint16_t kBitrateKbpsMpeg2[16] = {
      0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
  uint8_t buf[512];
  uint32_t offset = 0;
  f.seek(0);
  int n = f.read(buf, sizeof(buf));
  if (n >= 10 && memcmp(buf, "ID3", 3) == 0) {
    offset = 10 + ((buf[6] & 0x7f) << 21 | (buf[7] & 0x7f) << 14 |
                   (buf[8] & 0x7f) << 7 | (buf[9] & 0x7f));
    f.seek(offset);
    n = f.read(buf, sizeof(buf));
  }
  for (int i = 0; i + 4 <= n; i++) {
    if (buf[i] != 0xff || (buf[i + 1] & 0xe0) != 0xe0) {
      continue;
    }
    const int version = (buf[i + 1] >> 3) & 3;  // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
    const int layer = (buf[i + 1] >> 1) & 3;    // 1=Layer III
    const int bitrateIndex = buf[i + 2] >> 4;
    if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15) {
      continue;
    }
    const uint32_t kbps = version == 3 ? kBitrateKbpsMpeg1[bitrateIndex]
                                       : kBitrateKbpsMpeg2[bitrateIndex];
    s_dataStart = offset + i;
    s_bytesPerSec = kbps * 1000 / 8;
    return true;
  }
  return false;
}

void closeSource() {
  if (s_file) {
    s_file.close();
  }
  s_openSource = -1;
}

// srcIndexのファイルをs_fileに開く(既に開いていれば何もしない)。
bool openSource(int srcIndex) {
  if (s_file && s_openSource == srcIndex) {
    return true;
  }
  closeSource();
  const String path = s_directory + "/" + s_book.sources[srcIndex].c_str();
  s_file = SD.open(path, FILE_READ);
  if (!s_file) {
    LOGE("open failed: %s", path.c_str());
    return false;
  }
  if (!probeMp3(s_file)) {
    LOGE("not an MP3 (Layer III) file: %s", path.c_str());
    s_file.close();
    return false;
  }
  s_openSource = srcIndex;
  LOGI("open %s (%u bytes, %u bytes/s, data@%u)", path.c_str(),
       static_cast<unsigned>(s_file.size()),
       static_cast<unsigned>(s_bytesPerSec),
       static_cast<unsigned>(s_dataStart));
  return true;
}

// indexのクリップを始める。必要ならファイルを開き、クリップ開始のプリロール分手前へ
// シークしてデコーダを初期化し直す。呼び出し時はstate==kSeekingで、デコーダはend()済み。
// 成功したらkPlaying、失敗したらkFinishedにする。
bool beginClip(size_t index) {
  if (index >= s_book.clips.size()) {
    LOGE("clip %u out of range", static_cast<unsigned>(index));
    s_state = State::kFinished;
    return false;
  }
  s_clipIndex = index;
  const SmilClip& clip = s_book.clips[index];

  bool ok = openSource(clip.srcIndex);
  if (ok) {
    const uint32_t startMs = clip.beginMs > config::kMp3SeekPreRollMs
                                 ? clip.beginMs - config::kMp3SeekPreRollMs
                                 : 0;
    // CBR前提でバイト位置を求める。同期語は次のフレーム境界で見つかるので、実際の開始は
    // 最大1フレーム(約26ms)後ろにずれる(クリップ頭が最大1フレーム遅れうる)。
    const uint32_t startByte =
        s_dataStart + static_cast<uint32_t>(static_cast<uint64_t>(startMs) *
                                            s_bytesPerSec / 1000);
    ok = s_file.seek(startByte);
    s_baseMs = startMs;
  }
  if (!ok) {
    s_state = State::kFinished;
    return false;
  }
  s_needInit = true;
  s_decoder.begin();
  s_state = State::kPlaying;
  logClip("start");
  return true;
}

// 今のSMILのindexのクリップを再生する。
// flushAudio=trueはユーザ操作(旧クリップの残りPCMを捨てる)。falseは自然な遷移で、
// 旧クリップ末尾は切らずに鳴らし切る。
void startClip(size_t index, bool flushAudio) {
  if (flushAudio) {
    // Flush後はResumeまでenqueueが捨てられるので、デコーダのend()が吐く旧クリップの残りも鳴らない。
    i2sSpeakerFlush();
  }
  s_state = State::
      kSeeking;  // end()が内部バッファの残りをonPcmへ吐くので、その分は無視する
  s_decoder.end();
  beginClip(index);
  if (flushAudio) {
    i2sSpeakerResume();
  }
}

struct FileReader {
  File* file;
};

size_t readFileCallback(void* ctx, uint8_t* buf, size_t len) {
  const int n = static_cast<FileReader*>(ctx)->file->read(buf, len);
  return n > 0 ? static_cast<size_t>(n) : 0;
}

// SMILを読んでs_bookへ入れる。クリップが1つも取れなければfalse(s_bookは空になる)。
bool loadSmil(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) {
    LOGE("SMIL not found: %s", path.c_str());
    s_book = SmilBook();
    return false;
  }
  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t t0 = millis();
  FileReader reader{&f};
  const bool ok = smilParse(readFileCallback, &reader, f.size(), s_book);
  const uint32_t elapsed = millis() - t0;
  LOGI(
      "SMIL %s: %u clips, %u anchors, %u sources, %u skipped, %u bytes in %u "
      "ms, heap %u -> %u, stack free min %u",
      path.c_str(), static_cast<unsigned>(s_book.clips.size()),
      static_cast<unsigned>(s_book.anchors.size()),
      static_cast<unsigned>(s_book.sources.size()),
      static_cast<unsigned>(s_book.skipped), static_cast<unsigned>(f.size()),
      static_cast<unsigned>(elapsed), static_cast<unsigned>(heapBefore),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  f.close();
  if (!ok) {
    LOGE("no <audio> clip in %s", path.c_str());
    s_book = SmilBook();
  }
  return ok;
}

// fragment(ncc.htmlの見出しが指す<text
// id>)に対応する、今のSMILのクリップの添字。 見つからなければ先頭。
size_t anchorClip(const std::string& fragment) {
  if (s_book.clips.empty()) {
    return 0;
  }
  for (const SmilAnchor& a : s_book.anchors) {
    if (a.id == fragment) {
      return std::min<size_t>(a.clipIndex, s_book.clips.size() - 1);
    }
  }
  return 0;
}

// smilIndexのSMILを読み込み、fragment(nullptrなら先頭)から再生を始める。
// startAtLastClip=trueならfragmentに関わらずSMILの最後のクリップから始める。
// startClipIndexが0以上なら、fragmentに関わらずそのクリップ(範囲外なら先頭)から始める。
// ユーザ操作(flushAudio=true)ではFlushして旧SMILの残りPCMを捨てる。
bool playSmil(size_t smilIndex, const char* fragment, bool flushAudio,
              bool startAtLastClip = false, int startClipIndex = -1) {
  if (flushAudio) {
    i2sSpeakerFlush();
  }
  s_state = State::kSeeking;
  s_decoder
      .end();  // s_bookを差し替える前に、デコーダの残りをonPcmへ吐かせて捨てる
  closeSource();
  s_smilIndex = smilIndex;
  bool ok = loadSmil(s_directory + "/" + s_ncc.smils[smilIndex].c_str());
  if (ok) {
    LOGI("smil %u/%u %s", static_cast<unsigned>(smilIndex + 1),
         static_cast<unsigned>(s_ncc.smils.size()),
         s_ncc.smils[smilIndex].c_str());
    const size_t first =
        startAtLastClip ? s_book.clips.size() - 1
        : startClipIndex >= 0 &&
                static_cast<size_t>(startClipIndex) < s_book.clips.size()
            ? static_cast<size_t>(startClipIndex)
        : fragment != nullptr ? anchorClip(fragment)
                              : 0;
    ok = beginClip(first);
  } else {
    s_state = State::kFinished;
  }
  if (flushAudio) {
    i2sSpeakerResume();
  }
  return ok;
}

// firstから順に、再生できるSMILが見つかるまで試す。無ければ本の終わり。
void playSmilsFrom(size_t first) {
  for (size_t i = first; i < s_ncc.smils.size(); i++) {
    if (playSmil(i, nullptr, false)) {
      return;
    }
    LOGW("skip %s", s_ncc.smils[i].c_str());
  }
  s_state = State::kFinished;
  LOGI("finished");
}

// 本のID。dc:identifierとSMILファイル名の並びのFNV-1a。ncc.html全体は読み直さない。
uint32_t bookIdOf(const NccBook& book) {
  uint32_t h = 2166136261u;
  auto mix = [&h](const std::string& s) {
    for (const char ch : s) {
      h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
    }
    h = (h ^ 0u) * 16777619u;  // 区切り
  };
  mix(book.identifier);
  for (const std::string& smil : book.smils) {
    mix(smil);
  }
  return h;
}

// しおりがあればそこから、無ければ(読めなければ)本の先頭から再生を始める。
void resumeFromBookmark() {
  uint16_t smil = 0;
  uint16_t clip = 0;
  if (bookmarkLoad(s_bookId, &smil, &clip) && smil < s_ncc.smils.size()) {
    LOGI("resume from bookmark smil=%u clip=%u", static_cast<unsigned>(smil),
         static_cast<unsigned>(clip));
    if (playSmil(smil, nullptr, false, false, clip)) {
      return;
    }
    LOGW("bookmark unusable, start from the beginning");
  }
  playSmilsFrom(0);
}

// 移動対象(レベルがs_headingLevel以下)の見出しか。
bool isTargetHeading(size_t i) {
  return s_ncc.headings[i].level <= s_headingLevel;
}

// 今のクリップを含む見出しの添字。先頭の見出しより前なら-1。移動対象の見出しだけを見る。
// 見出しはncc.html順(=SMILの添字が単調非減少)で、同じSMIL内ではアンカーの位置で判定する。
int currentHeading() {
  int cur = -1;
  for (size_t i = 0; i < s_ncc.headings.size(); i++) {
    const NccHeading& h = s_ncc.headings[i];
    if (!isTargetHeading(i)) {
      continue;
    }
    if (h.smilIndex < s_smilIndex ||
        (h.smilIndex == s_smilIndex && anchorClip(h.fragment) <= s_clipIndex)) {
      cur = static_cast<int>(i);
    }
  }
  return cur;
}

void gotoHeading(size_t index) {
  const NccHeading& h = s_ncc.headings[index];
  LOGI("heading %u/%u -> %s#%s", static_cast<unsigned>(index + 1),
       static_cast<unsigned>(s_ncc.headings.size()),
       s_ncc.smils[h.smilIndex].c_str(), h.fragment.c_str());
  if (h.smilIndex == s_smilIndex && !s_book.clips.empty()) {
    startClip(anchorClip(h.fragment), true);  // 同じSMIL内なら読み込み直さない
  } else {
    playSmil(h.smilIndex, h.fragment.c_str(), true);
  }
  s_paused = false;
}

// 見出し単位の移動。delta=-1:前の見出し、+1:次の見出し、0:今の見出しの先頭。
// 先頭の見出しより前での前は先頭の見出し、最後の見出しでの次は何もしない。
// 対象はレベルがs_headingLevel以下の見出しだけ。
void stepHeading(int delta) {
  int first = -1;  // 最初の対象見出し
  for (size_t i = 0; i < s_ncc.headings.size() && first < 0; i++) {
    if (isTargetHeading(i)) {
      first = static_cast<int>(i);
    }
  }
  if (first < 0) {
    LOGI("no headings");
    return;
  }
  const int cur = currentHeading();
  if (cur < 0 && delta == 0) {  // 最初の見出しより前。SMILの頭へ
    startClip(0, true);
    s_paused = false;
    return;
  }
  int target = cur;
  if (delta != 0) {
    const int count = static_cast<int>(s_ncc.headings.size());
    target = -1;
    for (int i = cur + delta; i >= 0 && i < count; i += delta) {
      if (isTargetHeading(static_cast<size_t>(i))) {
        target = i;
        break;
      }
    }
    if (target < 0) {
      if (delta > 0) {
        LOGI("no next heading");
        return;
      }
      target = first;  // 先頭の見出しでの前は、その先頭
    }
  }
  gotoHeading(static_cast<size_t>(target));
}

void changeHeadingLevel(int delta) {
  const int level = std::clamp(static_cast<int>(s_headingLevel) + delta,
                               static_cast<int>(config::kHeadingLevelMin),
                               static_cast<int>(config::kHeadingLevelMax));
  s_headingLevel = static_cast<uint8_t>(level);
  LOGI("heading level H%u", static_cast<unsigned>(s_headingLevel));
}

// ユーザ操作によるフレーズ(クリップ)送り。
// SMILの最後のクリップの次は次のSMILの先頭へ、SMILの先頭クリップの前は前のSMILの最後の
// クリップへ。本の最初の前は先頭クリップの頭出し、本の最後の次は何もしない。
void stepPhrase(int delta) {
  if (s_book.clips.empty()) {
    return;
  }
  const int target = static_cast<int>(s_clipIndex) + delta;
  if (target >= static_cast<int>(s_book.clips.size())) {
    if (s_smilIndex + 1 >= s_ncc.smils.size()) {
      LOGI("no next clip");
      return;
    }
    playSmil(s_smilIndex + 1, nullptr, true);
  } else if (target < 0 && s_smilIndex > 0) {
    // 読み込めない(クリップの無い)SMILは飛ばしてさらに前へ。全部だめなら今のSMILへ戻す。
    const size_t current = s_smilIndex;
    size_t prev = current;
    bool ok = false;
    while (!ok && prev > 0) {
      prev--;
      ok = playSmil(prev, nullptr, true, /*startAtLastClip=*/true);
    }
    if (!ok) {
      playSmil(current, nullptr, true);
    }
  } else {
    startClip(static_cast<size_t>(std::max(target, 0)), true);
  }
  s_paused = false;
}

void playerTaskFn(void*) {
  vTaskDelay(pdMS_TO_TICKS(config::kMp3StartDelayMs));
  resumeFromBookmark();

  for (;;) {
    const bool active = s_state == State::kPlaying ||
                        s_state == State::kJumpPending ||
                        s_state == State::kNextSmil;
    const bool running = active && !s_paused;
    Command cmd;
    if (xQueueReceive(s_commandQueue, &cmd, running ? 0 : portMAX_DELAY) ==
        pdTRUE) {
      switch (cmd) {
        case Command::kPrevPhrase:
          stepPhrase(-1);
          break;
        case Command::kNextPhrase:
          stepPhrase(+1);
          break;
        case Command::kPrevHeading:
          stepHeading(-1);
          break;
        case Command::kNextHeading:
          stepHeading(+1);
          break;
        case Command::kHeadingStart:
          stepHeading(0);
          break;
        case Command::kLevelUp:
          changeHeadingLevel(-1);
          break;
        case Command::kLevelDown:
          changeHeadingLevel(+1);
          break;
        case Command::kTogglePause:
          s_paused = !s_paused;
          LOGI("%s", s_paused ? "paused" : "resumed");
          if (s_paused &&
              (s_state == State::kPlaying || s_state == State::kJumpPending ||
               s_state == State::kNextSmil)) {
            // kNextSmilは次のSMILの先頭を指すようにする(s_clipIndexは今のSMILの最後のまま)
            if (s_state == State::kNextSmil) {
              bookmarkSave(s_bookId, static_cast<uint16_t>(s_smilIndex + 1), 0);
            } else {
              bookmarkSave(s_bookId, static_cast<uint16_t>(s_smilIndex),
                           static_cast<uint16_t>(s_clipIndex));
            }
          }
          break;
      }
      continue;
    }
    if (!running) {
      continue;
    }

    if (s_state == State::kJumpPending) {
      startClip(s_clipIndex, false);
      continue;
    }
    if (s_state == State::kNextSmil) {
      playSmilsFrom(s_smilIndex + 1);
      continue;
    }

    const int n = s_file.read(s_readBuf, sizeof(s_readBuf));
    if (n > 0) {
      s_decoder.write(s_readBuf, n);
      continue;
    }
    // EOF。デコーダに残ったフレームを吐かせる(クリップ追跡は有効)。末尾のPCMは切らない。
    // Helixは内部バッファがMP3_MIN_FRAME_SIZE(1024B)に満たないとデコードせず、最後の書き込み
    // 後に最大9フレーム(約240ms)が残る。end()のflush()もclearArray()を二重に呼んで1フレームおきに
    // 読み飛ばすので、そのままでは最終クリップの末尾が欠ける。ゼロを足して通常のデコード経路で
    // 残りを処理させる(末尾のゼロは同期語が無いので無視される)。
    static_assert(config::kMp3ReadChunkBytes >= MP3_MIN_FRAME_SIZE,
                  "padding must reach the decoder's minimum frame buffer size");
    memset(s_readBuf, 0, sizeof(s_readBuf));
    s_decoder.write(s_readBuf, sizeof(s_readBuf));
    s_decoder.end();
    if (s_state == State::kPlaying) {
      LOGW("EOF before the end of the last clip");
      finishSmil();
    }
  }
}

void sendCommand(Command cmd) {
  if (s_commandQueue != nullptr) {
    xQueueSend(s_commandQueue, &cmd, 0);  // 満杯なら捨てる
  }
}

// ncc.htmlを読んでs_nccへ入れる。SMILが1つも取れなければfalse。
bool loadNcc(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) {
    LOGE("NCC not found: %s", path.c_str());
    return false;
  }
  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t t0 = millis();
  FileReader reader{&f};
  const bool ok = nccParse(readFileCallback, &reader, f.size(), s_ncc);
  LOGI("NCC %s: %u smils, %u headings, %u bytes in %u ms, heap %u -> %u",
       path.c_str(), static_cast<unsigned>(s_ncc.smils.size()),
       static_cast<unsigned>(s_ncc.headings.size()),
       static_cast<unsigned>(f.size()), static_cast<unsigned>(millis() - t0),
       static_cast<unsigned>(heapBefore),
       static_cast<unsigned>(ESP.getFreeHeap()));
  f.close();
  if (!ok) {
    LOGE("no SMIL link in %s", path.c_str());
  } else {
    s_bookId = bookIdOf(s_ncc);
    LOGI("book id=%08X identifier=\"%s\"", static_cast<unsigned>(s_bookId),
         s_ncc.identifier.c_str());
  }
  return ok;
}

}  // namespace

bool mp3PlayerBegin() {
  SPI.begin(config::kSdSckPin, config::kSdMisoPin, config::kSdMosiPin,
            config::kSdCsPin);
  if (!SD.begin(config::kSdCsPin, SPI, config::kSdSpiHz)) {
    LOGE("SD mount failed (card not inserted?)");
    return false;
  }
  s_directory = config::kBookDirectory;
  if (!loadNcc(s_directory + "/" + config::kNccFile)) {
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

void mp3PlayerPrevPhrase() { sendCommand(Command::kPrevPhrase); }
void mp3PlayerNextPhrase() { sendCommand(Command::kNextPhrase); }
void mp3PlayerPrevHeading() { sendCommand(Command::kPrevHeading); }
void mp3PlayerNextHeading() { sendCommand(Command::kNextHeading); }
void mp3PlayerHeadingStart() { sendCommand(Command::kHeadingStart); }
void mp3PlayerHeadingLevelUp() { sendCommand(Command::kLevelUp); }
void mp3PlayerHeadingLevelDown() { sendCommand(Command::kLevelDown); }
void mp3PlayerTogglePause() { sendCommand(Command::kTogglePause); }
