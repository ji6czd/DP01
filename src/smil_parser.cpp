#include "smil_parser.h"

#include <cstring>

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// srcの添字を返す。未登録なら追加する。255種類を超えたら-1。
int findOrAddSource(SmilBook& book, const char* src, size_t len) {
  for (size_t i = 0; i < book.sources.size(); i++) {
    const std::string& s = book.sources[i];
    if (s.size() == len && memcmp(s.data(), src, len) == 0) {
      return static_cast<int>(i);
    }
  }
  if (book.sources.size() >= UINT8_MAX) {
    return -1;
  }
  book.sources.emplace_back(src, len);
  return static_cast<int>(book.sources.size() - 1);
}

// <audio src= clip-begin= clip-end=>をクリップとして登録する。
void handleAudio(const char* tag, size_t len, SmilBook& book) {
  const char* src = nullptr;
  size_t srcLen = 0;
  if (!xmlGetAttr(tag, len, "src", &src, &srcLen) || srcLen == 0) {
    return;
  }
  // SMIL 1.0は"clip-begin"、SMIL 2.0は"clipBegin"。
  const char* begin = nullptr;
  size_t beginLen = 0;
  if (!xmlGetAttr(tag, len, "clip-begin", &begin, &beginLen)) {
    xmlGetAttr(tag, len, "clipbegin", &begin, &beginLen);
  }
  const char* end = nullptr;
  size_t endLen = 0;
  if (!xmlGetAttr(tag, len, "clip-end", &end, &endLen)) {
    xmlGetAttr(tag, len, "clipend", &end, &endLen);
  }

  uint32_t beginMs = 0;
  uint32_t endMs = kSmilClipToEnd;
  if ((begin != nullptr && !smilParseTime(begin, beginLen, beginMs)) ||
      (end != nullptr && !smilParseTime(end, endLen, endMs)) ||
      endMs <= beginMs) {
    book.skipped++;
    return;
  }
  const int srcIndex = findOrAddSource(book, src, srcLen);
  if (srcIndex < 0) {
    book.skipped++;
    return;
  }
  book.clips.push_back({static_cast<uint8_t>(srcIndex), beginMs, endMs});
}

void onTag(void* ctx, const char* tag, size_t len) {
  SmilBook& book = *static_cast<SmilBook*>(ctx);
  if (xmlTagIs(tag, len, "audio")) {
    handleAudio(tag, len, book);
  } else if (xmlTagIs(tag, len, "text")) {
    const char* id = nullptr;
    size_t idLen = 0;
    if (xmlGetAttr(tag, len, "id", &id, &idLen) && idLen > 0) {
      book.anchors.push_back(
          {std::string(id, idLen), static_cast<uint32_t>(book.clips.size())});
    }
  }
}

}  // namespace

bool smilParseTime(const char* s, size_t len, uint32_t& ms) {
  const char* p = s;
  const char* end = s + len;
  while (p < end && isSpace(*p)) {
    p++;
  }
  while (end > p && isSpace(end[-1])) {
    end--;
  }
  if (end - p >= 4 && xmlNameEquals(p, 4, "npt=")) {
    p += 4;
  }

  // 時:分:秒 / 分:秒 / 秒 (最大3フィールド)。最後のフィールドだけ小数を持てる。
  uint64_t fields[3];
  int fieldCount = 0;
  for (;;) {
    if (p >= end || !isDigit(*p)) {
      return false;
    }
    uint64_t v = 0;
    while (p < end && isDigit(*p)) {
      v = v * 10 + static_cast<uint64_t>(*p - '0');
      if (v > UINT32_MAX) {
        return false;
      }
      p++;
    }
    fields[fieldCount++] = v;
    if (p < end && *p == ':' && fieldCount < 3) {
      p++;
      continue;
    }
    break;
  }

  uint32_t fracMs = 0;
  if (p < end && *p == '.') {
    p++;
    uint32_t scale = 100;  // 小数第1位=100ms。4桁目以降は切り捨て
    while (p < end && isDigit(*p)) {
      fracMs += static_cast<uint32_t>(*p - '0') * scale;
      scale /= 10;
      p++;
    }
  }
  if (p < end && (*p == 's' || *p == 'S')) {
    p++;
  }
  if (p != end) {
    return false;
  }

  uint64_t seconds = 0;
  for (int i = 0; i < fieldCount; i++) {
    seconds = seconds * 60 + fields[i];
  }
  const uint64_t total = seconds * 1000 + fracMs;
  if (total >= kSmilClipToEnd) {
    return false;
  }
  ms = static_cast<uint32_t>(total);
  return true;
}

bool smilParse(SmilReadFn read, void* ctx, size_t sizeHint, SmilBook& out) {
  out.sources.clear();
  out.clips.clear();
  out.anchors.clear();
  out.skipped = 0;
  if (sizeHint > 0) {
    out.clips.reserve(sizeHint / 100);  // <audio>1行は約100バイト
  }
  xmlScanTags(read, ctx, onTag, &out);
  return !out.clips.empty();
}
