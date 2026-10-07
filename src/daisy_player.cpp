// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_DSY
#define LOG_MODULE_TAG "DSY"
#include "daisy_player.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bookmark_store.h"
#include "config.h"
#include "log_config.h"
#include "morse_code.h"
#include "mp3_player.h"
#include "ncc_parser.h"
#include "smil_parser.h"

namespace {

enum class Command : uint8_t {
  kPrevBook,
  kNextBook,
  kPrevPhrase,
  kNextPhrase,
  kPrevHeading,
  kNextHeading,
  kHeadingStart,
  kLevelUp,
  kLevelDown,
  kTogglePause,
  kSlower,
  kFaster,
};

QueueHandle_t s_commandQueue = nullptr;

// SDのルート直下にある本(ncc.htmlを持つフォルダ)。名前順。
std::vector<std::string> s_books;
size_t s_bookIndex = 0;  // s_booksの添字。今開いている本

// 今開いている本。
std::string s_directory;  // 本のフォルダ(SDルートからの絶対パス)
NccBook s_ncc;            // 本全体(SMILの再生順と見出し)
uint32_t s_bookId = 0;    // しおりでこの本を特定するID(bookIdOf(s_ncc))
SmilBook s_smil;          // 今再生中のSMIL
size_t s_smilIndex = 0;   // s_ncc.smilsの添字(s_smilの元)
size_t s_clipIndex = 0;   // s_smil.clipsの添字。mp3_playerが処理中(またはこれから鳴らす)クリップ

bool s_active = false;  // mp3_playerへクリップを渡して再生中(本の最後・エラーで落ちる)
bool s_paused = false;
uint8_t s_headingLevel = config::kHeadingLevelDefault;  // 見出し移動の対象はH1〜これ

struct FileReader {
  File* file;
};

size_t readFileCallback(void* ctx, uint8_t* buf, size_t len) {
  const int n = static_cast<FileReader*>(ctx)->file->read(buf, len);
  return n > 0 ? static_cast<size_t>(n) : 0;
}

// ---- 本の一覧 ----

// SDルート直下のフォルダのうち、ncc.htmlを持つものをs_booksへ集める。
void scanBooks() {
  s_books.clear();
  File root = SD.open("/");
  if (!root) {
    LOGE("SD root open failed");
    return;
  }
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (!entry.isDirectory()) {
      continue;
    }
    const std::string dir = std::string("/") + entry.name();
    if (SD.exists((dir + "/" + config::kNccFile).c_str())) {
      s_books.push_back(dir);
    }
  }
  std::sort(s_books.begin(), s_books.end());
  LOGI("%u books", static_cast<unsigned>(s_books.size()));
  for (const std::string& dir : s_books) {
    LOGI("  %s", dir.c_str());
  }
}

// ---- ncc.html / SMILの読み込み ----

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

// ncc.htmlを読んでoutへ入れる。SMILが1つも取れなければfalse。
bool loadNcc(const std::string& path, NccBook& out) {
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) {
    LOGE("NCC not found: %s", path.c_str());
    return false;
  }
  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t t0 = millis();
  FileReader reader{&f};
  const bool ok = nccParse(readFileCallback, &reader, f.size(), out);
  LOGI("NCC %s: %u smils, %u headings, %u bytes in %u ms, heap %u -> %u", path.c_str(),
       static_cast<unsigned>(out.smils.size()), static_cast<unsigned>(out.headings.size()),
       static_cast<unsigned>(f.size()), static_cast<unsigned>(millis() - t0), static_cast<unsigned>(heapBefore),
       static_cast<unsigned>(ESP.getFreeHeap()));
  f.close();
  if (!ok) {
    LOGE("no SMIL link in %s", path.c_str());
  }
  return ok;
}

// SMILを読んでs_smilへ入れる。クリップが1つも取れなければfalse(s_smilは空になる)。
bool loadSmil(const std::string& path) {
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) {
    LOGE("SMIL not found: %s", path.c_str());
    s_smil = SmilBook();
    return false;
  }
  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t t0 = millis();
  FileReader reader{&f};
  const bool ok = smilParse(readFileCallback, &reader, f.size(), s_smil);
  const uint32_t elapsed = millis() - t0;
  LOGI(
      "SMIL %s: %u clips, %u anchors, %u sources, %u skipped, %u bytes in %u "
      "ms, heap %u -> %u, stack free min %u",
      path.c_str(), static_cast<unsigned>(s_smil.clips.size()), static_cast<unsigned>(s_smil.anchors.size()),
      static_cast<unsigned>(s_smil.sources.size()), static_cast<unsigned>(s_smil.skipped),
      static_cast<unsigned>(f.size()), static_cast<unsigned>(elapsed), static_cast<unsigned>(heapBefore),
      static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  f.close();
  if (!ok) {
    LOGE("no <audio> clip in %s", path.c_str());
    s_smil = SmilBook();
  }
  return ok;
}

// ---- 再生位置 ----

// s_smilのindex番目のクリップを、mp3_playerへ渡す形にする。
Mp3Clip clipAt(size_t index) {
  const SmilClip& c = s_smil.clips[index];
  Mp3Clip clip;
  clip.path = s_directory + "/" + s_smil.sources[c.srcIndex];
  clip.beginMs = c.beginMs;
  clip.endMs = c.endMs == kSmilClipToEnd ? kMp3ClipToEnd : c.endMs;
  return clip;
}

// mp3_playerが現在クリップを鳴らし終えたときに呼ばれる。今のSMILに続きがあれば、位置を
// 進めてそのクリップを渡す。SMILの切り替えはここではせず(SDを読むので)、falseを返して
// mp3_playerを止め、タスクがkEndedを見てから行う。
bool nextClipForPlayer(Mp3Clip* out) {
  if (s_clipIndex + 1 >= s_smil.clips.size()) {
    return false;
  }
  s_clipIndex++;
  *out = clipAt(s_clipIndex);
  return true;
}

// 今のSMILのindexのクリップを再生する。flushAudio=trueはユーザ操作(旧クリップの残りPCMを
// 捨てる)、falseは自然な遷移。
bool playClip(size_t index, bool flushAudio) {
  s_clipIndex = index;
  s_active = mp3PlayerStart(clipAt(index), flushAudio);
  return s_active;
}

// fragment(ncc.htmlの見出しが指す<text id>)に対応する、今のSMILのクリップの添字。
// 見つからなければ先頭。
size_t anchorClip(const std::string& fragment) {
  if (s_smil.clips.empty()) {
    return 0;
  }
  for (const SmilAnchor& a : s_smil.anchors) {
    if (a.id == fragment) {
      return std::min<size_t>(a.clipIndex, s_smil.clips.size() - 1);
    }
  }
  return 0;
}

// smilIndexのSMILを読み込み、fragment(nullptrなら先頭)から再生を始める。
// startAtLastClip=trueならfragmentに関わらずSMILの最後のクリップから始める。
// startClipIndexが0以上なら、fragmentに関わらずそのクリップ(範囲外なら先頭)から始める。
// ユーザ操作(flushAudio=true)では旧SMILの音をすぐ止め、残りPCMを捨てる。
bool playSmil(size_t smilIndex, const char* fragment, bool flushAudio, bool startAtLastClip = false,
              int startClipIndex = -1) {
  if (flushAudio) {
    mp3PlayerStop();  // SMILを読んでいる間、旧位置の音を残さない
  }
  s_active = false;
  s_smilIndex = smilIndex;
  if (!loadSmil(s_directory + "/" + s_ncc.smils[smilIndex])) {
    return false;
  }
  LOGI("smil %u/%u %s", static_cast<unsigned>(smilIndex + 1), static_cast<unsigned>(s_ncc.smils.size()),
       s_ncc.smils[smilIndex].c_str());
  const size_t first = startAtLastClip ? s_smil.clips.size() - 1
                       : startClipIndex >= 0 && static_cast<size_t>(startClipIndex) < s_smil.clips.size()
                           ? static_cast<size_t>(startClipIndex)
                       : fragment != nullptr ? anchorClip(fragment)
                                             : 0;
  return playClip(first, false);  // 旧音声はstop済み(またはmp3_playerが鳴らし切った後)
}

// firstから順に、再生できるSMILが見つかるまで試す。無ければ本の終わり。
void playSmilsFrom(size_t first) {
  for (size_t i = first; i < s_ncc.smils.size(); i++) {
    if (playSmil(i, nullptr, false)) {
      return;
    }
    LOGW("skip %s", s_ncc.smils[i].c_str());
  }
  s_active = false;
  mp3PlayerFinish();         // 本の最後の数十msが話速変換に残らないよう出し切る
  bookmarkRemove(s_bookId);  // 読み終えた本は次回また先頭から
  LOGI("finished");
}

// ---- しおり ----

void saveBookmark() { bookmarkSave(s_bookId, static_cast<uint16_t>(s_smilIndex), static_cast<uint16_t>(s_clipIndex)); }

// しおりがあればそこから、無ければ(読めなければ)本の先頭から再生を始める。
void resumeFromBookmark() {
  uint16_t smil = 0;
  uint16_t clip = 0;
  if (bookmarkLoad(s_bookId, &smil, &clip) && smil < s_ncc.smils.size()) {
    LOGI("resume from bookmark smil=%u clip=%u", static_cast<unsigned>(smil), static_cast<unsigned>(clip));
    if (playSmil(smil, nullptr, false, false, clip)) {
      return;
    }
    LOGW("bookmark unusable, start from the beginning");
  }
  playSmilsFrom(0);
}

// ---- 本の選択 ----

// s_books[index]を開いて(しおりがあればその続きから)再生する。ncc.htmlが読めなければ
// 今の本のままfalseを返す。
bool openBook(size_t index) {
  const std::string directory = s_books[index];
  NccBook ncc;
  if (!loadNcc(directory + "/" + config::kNccFile, ncc)) {
    return false;
  }
  if (s_active) {
    saveBookmark();  // 直前の本の位置
  }
  mp3PlayerStop();
  s_active = false;
  s_bookIndex = index;
  s_directory = directory;
  s_ncc = std::move(ncc);
  s_smil = SmilBook();
  s_smilIndex = 0;
  s_clipIndex = 0;
  s_bookId = bookIdOf(s_ncc);
  s_paused = false;
  lastBookSave(directory);
  LOGI("book %u/%u %s id=%08X identifier=\"%s\"", static_cast<unsigned>(index + 1),
       static_cast<unsigned>(s_books.size()), directory.c_str(), static_cast<unsigned>(s_bookId),
       s_ncc.identifier.c_str());
  resumeFromBookmark();
  return true;
}

// delta=-1:前の本、+1:次の本。開けない本は飛ばし、一覧の端では何もしない。
void stepBook(int delta) {
  const int count = static_cast<int>(s_books.size());
  for (int i = static_cast<int>(s_bookIndex) + delta; i >= 0 && i < count; i += delta) {
    if (openBook(static_cast<size_t>(i))) {
      return;
    }
    LOGW("skip %s", s_books[i].c_str());
  }
  LOGI("%s", delta > 0 ? "no next book" : "no previous book");
}

// 最初に開く本。前回開いた本(一覧に無ければ先頭)から、開ける本が出るまで一覧を回って試す。
void openFirstBook() {
  const auto last = std::find(s_books.begin(), s_books.end(), lastBookLoad());
  const size_t start = last != s_books.end() ? static_cast<size_t>(last - s_books.begin()) : 0;
  for (size_t n = 0; n < s_books.size(); n++) {
    if (openBook((start + n) % s_books.size())) {
      return;
    }
  }
}

// ---- 見出し・フレーズの移動 ----

// 移動対象(レベルがs_headingLevel以下)の見出しか。
bool isTargetHeading(size_t i) { return s_ncc.headings[i].level <= s_headingLevel; }

// 今のクリップを含む見出しの添字。先頭の見出しより前なら-1。移動対象の見出しだけを見る。
// 見出しはncc.html順(=SMILの添字が単調非減少)で、同じSMIL内ではアンカーの位置で判定する。
int currentHeading() {
  int cur = -1;
  for (size_t i = 0; i < s_ncc.headings.size(); i++) {
    const NccHeading& h = s_ncc.headings[i];
    if (!isTargetHeading(i)) {
      continue;
    }
    if (h.smilIndex < s_smilIndex || (h.smilIndex == s_smilIndex && anchorClip(h.fragment) <= s_clipIndex)) {
      cur = static_cast<int>(i);
    }
  }
  return cur;
}

void gotoHeading(size_t index) {
  const NccHeading& h = s_ncc.headings[index];
  LOGI("heading %u/%u -> %s#%s", static_cast<unsigned>(index + 1), static_cast<unsigned>(s_ncc.headings.size()),
       s_ncc.smils[h.smilIndex].c_str(), h.fragment.c_str());
  if (h.smilIndex == s_smilIndex && !s_smil.clips.empty()) {
    playClip(anchorClip(h.fragment), true);  // 同じSMIL内なら読み込み直さない
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
    playClip(0, true);
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

// ---- 操作の合図 ----
// 見出しレベルと再生速度の変更は、モールス(morse_code)の短い音で知らせる。
//   変わった: 短音1つ("e")  変わらなかった(端): 短音2つ("i")  速度が等倍になった: 長音("t")
// 見出しレベルはH1〜H6を音階のC D E F G A 、速度は音階のGで鳴らす。
constexpr uint8_t kLevelPitch[] = {1, 2, 3, 4, 5, 6};  // 添字は見出しレベル-1
static_assert(sizeof(kLevelPitch) == config::kHeadingLevelMax - config::kHeadingLevelMin + 1,
              "one pitch per heading level");
constexpr uint8_t kSpeedPitch = 5;  // G5。レベル(1〜6)と音域が重なるが、キーが別なので許容
constexpr uint32_t kCueTimeoutMs = 2000;

// 合図を鳴らす。再生していたら止めて合図の間は無音にし、鳴り終えてから、耳に届いていた
// 位置の少し手前から再生を続ける(一時停止中なら、その位置まで戻して止めたままにする)。
void playCue(const char* morse, uint8_t pitch) {
  Mp3Clip resume;
  const bool resumable = s_active && mp3PlayerResumePoint(&resume);
  if (resumable) {
    mp3PlayerStop();
  }
  morsePlay(morse, pitch);
  morseWaitIdle(kCueTimeoutMs);
  if (resumable) {
    s_active = mp3PlayerStart(resume, false);
  }
}

// 再生速度を変えた結果(changed=変わったか)を合図で知らせる。
void changeSpeed(bool changed) {
  const bool normal = mp3PlayerSpeed() == 1.0f;
  playCue(normal ? "t" : changed ? "e" : "i", kSpeedPitch);
}

// 見出しレベルをdelta動かす(端で止まる)。変わったかを合図で知らせる。
void changeHeadingLevel(int delta) {
  const uint8_t before = s_headingLevel;
  const int level = std::clamp(static_cast<int>(s_headingLevel) + delta, static_cast<int>(config::kHeadingLevelMin),
                               static_cast<int>(config::kHeadingLevelMax));
  s_headingLevel = static_cast<uint8_t>(level);
  LOGI("heading level H%u", static_cast<unsigned>(s_headingLevel));
  playCue(s_headingLevel != before ? "e" : "i", kLevelPitch[s_headingLevel - config::kHeadingLevelMin]);
}

// ユーザ操作によるフレーズ(クリップ)送り。
// SMILの最後のクリップの次は次のSMILの先頭へ、SMILの先頭クリップの前は前のSMILの最後の
// クリップへ。本の最初の前は先頭クリップの頭出し、本の最後の次は何もしない。
void stepPhrase(int delta) {
  if (s_smil.clips.empty()) {
    return;
  }
  const int target = static_cast<int>(s_clipIndex) + delta;
  if (target >= static_cast<int>(s_smil.clips.size())) {
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
    playClip(static_cast<size_t>(std::max(target, 0)), true);
  }
  s_paused = false;
}

// ---- タスク ----

void handleCommand(Command cmd) {
  switch (cmd) {
    case Command::kPrevBook:
      stepBook(-1);
      break;
    case Command::kNextBook:
      stepBook(+1);
      break;
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
    case Command::kSlower:
      changeSpeed(mp3PlayerSlower());
      break;
    case Command::kFaster:
      changeSpeed(mp3PlayerFaster());
      break;
    case Command::kTogglePause:
      s_paused = !s_paused;
      LOGI("%s", s_paused ? "paused" : "resumed");
      if (s_paused && s_active) {
        saveBookmark();
      }
      break;
  }
}

void playerTaskFn(void*) {
  vTaskDelay(pdMS_TO_TICKS(config::kDaisyStartDelayMs));
  openFirstBook();

  for (;;) {
    const bool running = s_active && !s_paused;
    Command cmd;
    if (xQueueReceive(s_commandQueue, &cmd, running ? 0 : portMAX_DELAY) == pdTRUE) {
      handleCommand(cmd);
      continue;
    }
    if (!running) {
      continue;
    }
    switch (mp3PlayerPump()) {
      case Mp3PumpResult::kPlaying:
        break;
      case Mp3PumpResult::kEnded:  // 今のSMILを鳴らし終えた。次のSMILへ、無ければ本の終わり
        playSmilsFrom(s_smilIndex + 1);
        break;
      case Mp3PumpResult::kIdle:
        s_active = false;
        break;
    }
  }
}

void sendCommand(Command cmd) {
  if (s_commandQueue != nullptr) {
    xQueueSend(s_commandQueue, &cmd, 0);  // 満杯なら捨てる
  }
}

}  // namespace

bool daisyPlayerBegin() {
  SPI.begin(config::kSdSckPin, config::kSdMisoPin, config::kSdMosiPin, config::kSdCsPin);
  if (!SD.begin(config::kSdCsPin, SPI, config::kSdSpiHz)) {
    LOGE("SD mount failed (card not inserted?)");
    return false;
  }
  scanBooks();
  if (s_books.empty()) {
    LOGE("no book (folder with %s) on the SD card", config::kNccFile);
    return false;
  }
  mp3PlayerBegin(nextClipForPlayer);

  s_commandQueue = xQueueCreate(config::kDaisyCommandQueueDepth, sizeof(Command));
  if (s_commandQueue == nullptr) {
    LOGE("queue create failed");
    return false;
  }
  if (xTaskCreatePinnedToCore(playerTaskFn, "daisy", config::kDaisyTaskStackBytes, nullptr, config::kDaisyTaskPriority,
                              nullptr, config::kDaisyTaskCore) != pdPASS) {
    LOGE("task create failed");
    return false;
  }
  return true;
}

void daisyPlayerPrevBook() { sendCommand(Command::kPrevBook); }
void daisyPlayerNextBook() { sendCommand(Command::kNextBook); }
void daisyPlayerPrevPhrase() { sendCommand(Command::kPrevPhrase); }
void daisyPlayerNextPhrase() { sendCommand(Command::kNextPhrase); }
void daisyPlayerPrevHeading() { sendCommand(Command::kPrevHeading); }
void daisyPlayerNextHeading() { sendCommand(Command::kNextHeading); }
void daisyPlayerHeadingStart() { sendCommand(Command::kHeadingStart); }
void daisyPlayerHeadingLevelUp() { sendCommand(Command::kLevelUp); }
void daisyPlayerHeadingLevelDown() { sendCommand(Command::kLevelDown); }
void daisyPlayerTogglePause() { sendCommand(Command::kTogglePause); }
void daisyPlayerSlower() { sendCommand(Command::kSlower); }
void daisyPlayerFaster() { sendCommand(Command::kFaster); }
