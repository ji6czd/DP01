// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_MP3
#define LOG_MODULE_TAG "MP3"
#include "mp3_player.h"

#include <Arduino.h>
#include <FS.h>
#include <MP3DecoderHelix.h>
#include <SD.h>
#include <sonic.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include "config.h"
#include "i2s_speaker.h"
#include "log_config.h"

namespace {

// 再生エンジンの状態。onPcm()もデコーダ経由で同じタスクから呼ばれるので排他は不要。
enum class State : uint8_t {
  kIdle,         // 何も再生していない
  kSeeking,      // シーク/オープン中。この間にデコーダが吐くPCM(end()の残り等)は捨てる
  kPlaying,      // クリップを再生中
  kJumpPending,  // 現在クリップが終わり、次(s_pending)は連続していない。Pumpがstartする
  kEnded,        // 次のクリップが無く終わった。次のPumpがkEndedを返してkIdleへ戻る
};

Mp3NextClipFn s_nextClip = nullptr;
State s_state = State::kIdle;
Mp3Clip s_clip;     // デコーダが処理中のクリップ
Mp3Clip s_pending;  // kJumpPendingの次のクリップ

File s_file;
String s_openPath;           // s_fileが開いているパス
uint32_t s_dataStart = 0;    // 最初のフレーム同期の位置(ID3v2タグの後)
uint32_t s_bytesPerSec = 0;  // CBR前提のバイト/秒

libhelix::MP3DecoderHelix s_decoder;
uint8_t s_readBuf[config::kMp3ReadChunkBytes];

// デコード位置(ファイル先頭からのサンプル数)。シーク時はs_baseMsから始め、レートは最初の
// PCMで分かるのでs_needInitでその時に初期化する。
uint32_t s_baseMs = 0;
bool s_needInit = false;
int64_t s_posSamples = 0;
uint32_t s_lastRate = 0;  // 直近にデコードしたPCMのサンプルレート(位置のms換算用)

// I2SのDMAバッファとSonicの内部に溜まる分の見積もり(ms)。リング(config::kPcmRingBufferBytes)
// とは別に、再開位置を戻すときに足す。
constexpr uint32_t kAudioLatencyMs = 100;

// モノ→ステレオ複製用。1回のenqueueあたりのステレオフレーム数。
constexpr size_t kStereoChunkFrames = 256;
int16_t s_stereoBuf[kStereoChunkFrames * 2];

int64_t msToSamples(uint32_t ms, uint32_t rate) { return static_cast<int64_t>(ms) * rate / 1000; }

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

// 話速変換(Sonic)。クリップから切り出したPCMはここを通してから出力する。音程を保った
// まま速度だけを変える。ストリームはサンプルレート/チャンネル数が変わったときだけ作り直す。
// 内部に最大で約2ピッチ周期分(65Hzで約30ms)の入力を溜めるが、自然なクリップの
// 継ぎ目ではそのまま次の音声へ繋げて鳴らす(位置の追跡はs_posSamplesで元のPCM側を数える)。
sonicStream s_sonic = nullptr;
uint32_t s_sonicRate = 0;
int s_sonicChans = 0;
size_t s_speedIndex = config::kPlaybackSpeedDefault;

// Sonicからの取り出し1回あたりのフレーム数。
constexpr size_t kSonicReadFrames = 256;
int16_t s_sonicOut[kSonicReadFrames * 2];

float currentSpeed() { return config::kPlaybackSpeeds[s_speedIndex]; }

// Sonicの出力に溜まった分を全部出力する。
void drainSonicOutput() {
  int n;
  while ((n = sonicReadShortFromStream(s_sonic, s_sonicOut, kSonicReadFrames)) > 0) {
    emitFrames(s_sonicOut, static_cast<size_t>(n), s_sonicChans, s_sonicRate);
  }
}

// Sonicの入力に溜まった残りも変換して出し切る(本の終わり、フォーマットが変わる前)。
void flushSonic() {
  if (s_sonic == nullptr) {
    return;
  }
  sonicFlushStream(s_sonic);
  drainSonicOutput();
}

// Sonicに溜まった旧位置のPCMを鳴らさずに捨てる(ユーザ操作で再生位置が飛ぶとき)。
// 内部状態を空に戻すAPIが無いので、flushで出し切って読み捨てる。
void discardSonic() {
  if (s_sonic == nullptr) {
    return;
  }
  sonicFlushStream(s_sonic);
  while (sonicReadShortFromStream(s_sonic, s_sonicOut, kSonicReadFrames) > 0) {
  }
}

// frames個のフレームを現在の速度に変換して出力する。
void emitSpeech(const int16_t* pcm, size_t frames, int nChans, uint32_t rate) {
  if (rate != s_sonicRate || nChans != s_sonicChans) {
    flushSonic();  // 旧フォーマットの残りは旧フォーマットのまま鳴らし切る
    if (s_sonic != nullptr) {
      sonicDestroyStream(s_sonic);
    }
    s_sonic = sonicCreateStream(static_cast<int>(rate), nChans);
    s_sonicRate = rate;
    s_sonicChans = nChans;
    if (s_sonic == nullptr) {
      LOGE("sonic create failed (%u Hz, %d ch), speed control disabled", static_cast<unsigned>(rate), nChans);
    } else {
      sonicSetSpeed(s_sonic, currentSpeed());
      LOGI("sonic %u Hz, %d ch", static_cast<unsigned>(rate), nChans);
    }
  }
  if (s_sonic == nullptr) {
    emitFrames(pcm, frames, nChans, rate);  // 作れなければ等速で素通し
    return;
  }
  if (!sonicWriteShortToStream(s_sonic, pcm, static_cast<int>(frames))) {
    LOGE("sonic write failed (out of memory)");
  }
  drainSonicOutput();
}

void logClip(const char* what) {
  LOGI("%s %s %u-%d ms", what, s_clip.path.c_str(), static_cast<unsigned>(s_clip.beginMs),
       s_clip.endMs == kMp3ClipToEnd ? -1 : static_cast<int>(s_clip.endMs));
}

// 現在クリップの再生が終わった。次クリップを呼び出し側へ尋ね、そのまま流せるならtrueを返す。
// 続きが無ければkEnded、同一ファイルでも遠い/戻る場合はkJumpPendingにしてfalseを返す。
bool advanceClip() {
  Mp3Clip next;
  if (s_nextClip == nullptr || !s_nextClip(&next)) {
    s_state = State::kEnded;
    return false;
  }
  const bool sameStream = next.path == s_clip.path && s_clip.endMs != kMp3ClipToEnd && next.beginMs >= s_clip.endMs &&
                          next.beginMs - s_clip.endMs <= config::kMp3SkipThroughMs;
  if (!sameStream) {
    s_pending = std::move(next);
    s_state = State::kJumpPending;
    return false;
  }
  s_clip = std::move(next);
  logClip("next");
  return true;
}

// Helixのデコード結果。pcmLenはチャンネルを含む総サンプル数(info.outputSamps)。
// デコード位置を数え、現在クリップの [beginMs, endMs) に入る部分だけを出力する。
void onPcm(MP3FrameInfo& info, short* pcm, size_t pcmLen, void*) {
  if (s_state != State::kPlaying) {
    return;
  }
  const int nChans = info.nChans > 0 ? info.nChans : 1;
  const uint32_t rate = static_cast<uint32_t>(info.samprate);
  s_lastRate = rate;
  const size_t frames = pcmLen / nChans;

  if (s_needInit) {
    s_posSamples = msToSamples(s_baseMs, rate);
    s_needInit = false;
  }
  const int64_t blockStart = s_posSamples;
  s_posSamples += frames;

  size_t from = 0;
  while (from < frames) {
    const int64_t begin = msToSamples(s_clip.beginMs, rate);
    const int64_t end = s_clip.endMs == kMp3ClipToEnd ? INT64_MAX : msToSamples(s_clip.endMs, rate);
    const int64_t cur = blockStart + static_cast<int64_t>(from);
    if (cur < begin) {  // プリロール/クリップ間の捨て区間
      from += static_cast<size_t>(std::min<int64_t>(begin - cur, static_cast<int64_t>(frames - from)));
      continue;
    }
    // curがendを越えていることもある(1ブロックが複数クリップにまたがる場合)ので0で下限をとる。
    const size_t n =
        static_cast<size_t>(std::max<int64_t>(0, std::min<int64_t>(end - cur, static_cast<int64_t>(frames - from))));
    emitSpeech(pcm + from * nChans, n, nChans, rate);
    from += n;
    if (cur + static_cast<int64_t>(n) >= end && !advanceClip()) {
      return;  // 以降のフレームは次クリップのものではないので捨てる
    }
  }
}

// 先頭のID3v2を飛ばして最初のMPEG Audio Layer
// IIIフレームヘッダを探し、s_dataStartと s_bytesPerSec(CBR前提)を設定する。
bool probeMp3(File& f) {
  static const uint16_t kBitrateKbpsMpeg1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
  static const uint16_t kBitrateKbpsMpeg2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
  uint8_t buf[512];
  uint32_t offset = 0;
  f.seek(0);
  int n = f.read(buf, sizeof(buf));
  if (n >= 10 && memcmp(buf, "ID3", 3) == 0) {
    offset = 10 + ((buf[6] & 0x7f) << 21 | (buf[7] & 0x7f) << 14 | (buf[8] & 0x7f) << 7 | (buf[9] & 0x7f));
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
    const uint32_t kbps = version == 3 ? kBitrateKbpsMpeg1[bitrateIndex] : kBitrateKbpsMpeg2[bitrateIndex];
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
  s_openPath = "";
}

// pathのファイルをs_fileに開く(既に開いていれば何もしない)。
bool openSource(const std::string& path) {
  if (s_file && s_openPath == path.c_str()) {
    return true;
  }
  closeSource();
  s_file = SD.open(path.c_str(), FILE_READ);
  if (!s_file) {
    LOGE("open failed: %s", path.c_str());
    return false;
  }
  if (!probeMp3(s_file)) {
    LOGE("not an MP3 (Layer III) file: %s", path.c_str());
    s_file.close();
    return false;
  }
  s_openPath = path.c_str();
  LOGI("open %s (%u bytes, %u bytes/s, data@%u)", path.c_str(), static_cast<unsigned>(s_file.size()),
       static_cast<unsigned>(s_bytesPerSec), static_cast<unsigned>(s_dataStart));
  return true;
}

// clipを始める。必要ならファイルを開き、クリップ開始のプリロール分手前へシークして
// デコーダを初期化し直す。呼び出し時はstate==kSeekingで、デコーダはend()済み。
// 成功したらkPlaying、失敗したらkIdleにする。
bool beginClip(const Mp3Clip& clip) {
  s_clip = clip;
  bool ok = openSource(clip.path);
  if (ok) {
    const uint32_t startMs = clip.beginMs > config::kMp3SeekPreRollMs ? clip.beginMs - config::kMp3SeekPreRollMs : 0;
    // CBR前提でバイト位置を求める。同期語は次のフレーム境界で見つかるので、実際の開始は
    // 最大1フレーム(約26ms)後ろにずれる(クリップ頭が最大1フレーム遅れうる)。
    const uint32_t startByte =
        s_dataStart + static_cast<uint32_t>(static_cast<uint64_t>(startMs) * s_bytesPerSec / 1000);
    ok = s_file.seek(startByte);
    s_baseMs = startMs;
  }
  if (!ok) {
    s_state = State::kIdle;
    return false;
  }
  s_needInit = true;
  s_decoder.begin();
  s_state = State::kPlaying;
  logClip("start");
  return true;
}

// Sonicの出力を捨ててから、再生を止める準備(kSeekingでデコーダの残りを捨てる)をする。
void abandonCurrent(bool flushAudio) {
  if (flushAudio) {
    // Flush後はResumeまでenqueueが捨てられるので、デコーダのend()が吐く旧クリップの残りも鳴らない。
    i2sSpeakerFlush();
    discardSonic();
  }
  s_state = State::kSeeking;  // end()が内部バッファの残りをonPcmへ吐くので、その分は無視する
  s_decoder.end();
}

// 再生速度をconfig::kPlaybackSpeedsの段階でdelta段動かす。両端では止まる(戻り値は変わったか)。
// Sonicはこれ以降に書き込む入力から新しい速度を使う(再生位置は変わらない)。
bool stepSpeed(int delta) {
  const size_t before = s_speedIndex;
  s_speedIndex = static_cast<size_t>(
      std::clamp(static_cast<int>(s_speedIndex) + delta, 0, static_cast<int>(config::kPlaybackSpeedCount) - 1));
  if (s_sonic != nullptr) {
    sonicSetSpeed(s_sonic, currentSpeed());
  }
  LOGI("speed x%.2f", static_cast<double>(currentSpeed()));
  return s_speedIndex != before;
}

}  // namespace

void mp3PlayerBegin(Mp3NextClipFn nextClip) {
  s_nextClip = nextClip;
  s_decoder.setDataCallback(onPcm);
}

bool mp3PlayerStart(const Mp3Clip& clip, bool flushAudio) {
  abandonCurrent(flushAudio);
  const bool ok = beginClip(clip);
  if (flushAudio) {
    i2sSpeakerResume();
  }
  return ok;
}

void mp3PlayerStop() {
  abandonCurrent(true);
  closeSource();
  i2sSpeakerResume();
  s_state = State::kIdle;
}

void mp3PlayerFinish() { flushSonic(); }

Mp3PumpResult mp3PlayerPump() {
  switch (s_state) {
    case State::kIdle:
    case State::kSeeking:
      return Mp3PumpResult::kIdle;
    case State::kEnded:
      s_state = State::kIdle;
      return Mp3PumpResult::kEnded;
    case State::kJumpPending:
      if (!mp3PlayerStart(s_pending, false)) {
        return Mp3PumpResult::kEnded;
      }
      return Mp3PumpResult::kPlaying;
    case State::kPlaying:
      break;
  }

  const int n = s_file.read(s_readBuf, sizeof(s_readBuf));
  if (n > 0) {
    s_decoder.write(s_readBuf, n);
  } else {
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
      s_state = State::kEnded;
    }
  }
  if (s_state == State::kEnded) {
    s_state = State::kIdle;
    return Mp3PumpResult::kEnded;
  }
  return Mp3PumpResult::kPlaying;
}

bool mp3PlayerSlower() { return stepSpeed(-1); }
bool mp3PlayerFaster() { return stepSpeed(+1); }

float mp3PlayerSpeed() { return currentSpeed(); }

bool mp3PlayerResumePoint(Mp3Clip* out) {
  switch (s_state) {
    case State::kJumpPending:
      *out = s_pending;  // 現在クリップは鳴らし終えていて、次のクリップの頭から
      return true;
    case State::kPlaying:
      break;
    default:
      return false;
  }
  // デコード位置は、耳に届いている位置よりリング(+DMAとSonic)の分だけ先にいる。リングは
  // 出力レートでの時間なので、元の音声の時間にするには再生速度を掛ける。
  const uint32_t rate = s_lastRate != 0 ? s_lastRate : 1;
  const uint32_t decodedMs = s_needInit ? s_baseMs : static_cast<uint32_t>(s_posSamples * 1000 / rate);
  const uint32_t ringMs = static_cast<uint32_t>(static_cast<uint64_t>(config::kPcmRingBufferBytes) * 1000 / (rate * 4));
  const uint32_t backMs = static_cast<uint32_t>(static_cast<float>(ringMs + kAudioLatencyMs) * currentSpeed());
  *out = s_clip;
  const uint32_t from = decodedMs > backMs ? decodedMs - backMs : 0;
  if (from > s_clip.beginMs && (s_clip.endMs == kMp3ClipToEnd || from < s_clip.endMs)) {
    out->beginMs = from;  // 前のクリップの末尾までは戻らない(クリップの頭が下限)
  }
  return true;
}
