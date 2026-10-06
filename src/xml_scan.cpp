#include "xml_scan.h"

namespace {

constexpr size_t kReadBufBytes = 512;
constexpr size_t kTagBufBytes = 256;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
char toLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

}  // namespace

void xmlScanTags(XmlReadFn read, void* readCtx, XmlTagFn onTag, void* tagCtx) {
  uint8_t buf[kReadBufBytes];
  char tag[kTagBufBytes];
  size_t tagLen = 0;
  bool inTag = false;
  bool overflow = false;

  for (;;) {
    const size_t n = read(readCtx, buf, sizeof(buf));
    if (n == 0) {
      break;
    }
    for (size_t k = 0; k < n; k++) {
      const char c = static_cast<char>(buf[k]);
      if (!inTag) {
        if (c == '<') {
          inTag = true;
          tagLen = 0;
          overflow = false;
        }
        continue;
      }
      if (c == '>') {
        inTag = false;
        if (!overflow) {
          onTag(tagCtx, tag, tagLen);
        }
      } else if (c == '<') {
        tagLen = 0;  // 壊れたタグ。新しい'<'からやり直す
        overflow = false;
      } else if (tagLen < sizeof(tag)) {
        tag[tagLen++] = c;
      } else {
        overflow = true;
      }
    }
  }
}

size_t xmlTagNameLen(const char* tag, size_t len) {
  size_t i = (len > 0 && tag[0] == '/') ? 1 : 0;
  while (i < len && !isSpace(tag[i]) && tag[i] != '/') {
    i++;
  }
  return i;
}

bool xmlNameEquals(const char* s, size_t len, const char* name) {
  size_t i = 0;
  for (; name[i] != '\0'; i++) {
    if (i >= len || toLower(s[i]) != name[i]) {
      return false;
    }
  }
  return i == len;
}

bool xmlTagIs(const char* tag, size_t len, const char* name) {
  return xmlNameEquals(tag, xmlTagNameLen(tag, len), name);
}

bool xmlGetAttr(const char* tag, size_t len, const char* name, const char** value, size_t* valueLen) {
  size_t i = xmlTagNameLen(tag, len);
  while (i < len) {
    while (i < len && (isSpace(tag[i]) || tag[i] == '/')) {
      i++;
    }
    const size_t nameStart = i;
    while (i < len && !isSpace(tag[i]) && tag[i] != '=' && tag[i] != '/') {
      i++;
    }
    const size_t nameLen = i - nameStart;
    while (i < len && isSpace(tag[i])) {
      i++;
    }
    if (i >= len || tag[i] != '=') {
      continue;  // 値の無い属性
    }
    i++;
    while (i < len && isSpace(tag[i])) {
      i++;
    }
    if (i >= len || (tag[i] != '"' && tag[i] != '\'')) {
      continue;
    }
    const char quote = tag[i++];
    const size_t valueStart = i;
    while (i < len && tag[i] != quote) {
      i++;
    }
    const size_t vlen = i - valueStart;
    i++;  // 閉じ引用符

    if (xmlNameEquals(tag + nameStart, nameLen, name)) {
      *value = tag + valueStart;
      *valueLen = vlen;
      return true;
    }
  }
  return false;
}
