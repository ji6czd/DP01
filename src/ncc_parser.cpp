#include "ncc_parser.h"

namespace {

constexpr size_t kMaxSmils = UINT16_MAX;

struct Context {
  NccBook* book;
  bool inHeading = false;
};

bool isHeadingTag(const char* tag, size_t len, bool closing) {
  const size_t nameLen = xmlTagNameLen(tag, len);
  const size_t offset = closing ? 1 : 0;
  if (closing && !(len > 0 && tag[0] == '/')) {
    return false;
  }
  if (nameLen != 2 + offset) {
    return false;
  }
  const char h = tag[offset];
  const char digit = tag[offset + 1];
  return (h == 'h' || h == 'H') && digit >= '1' && digit <= '6';
}

bool endsWithSmil(const char* s, size_t len) {
  return len >= 5 && xmlNameEquals(s + len - 5, 5, ".smil");
}

// 登録済みならその添字、無ければ追加した添字。上限超えは-1。
int findOrAddSmil(NccBook& book, const char* name, size_t len) {
  // 連続する<a>は同じSMILを指すことが多いので、末尾から探す。
  for (size_t i = book.smils.size(); i > 0; i--) {
    const std::string& s = book.smils[i - 1];
    if (s.size() == len && s.compare(0, len, name, len) == 0) {
      return static_cast<int>(i - 1);
    }
  }
  if (book.smils.size() >= kMaxSmils) {
    return -1;
  }
  book.smils.emplace_back(name, len);
  return static_cast<int>(book.smils.size() - 1);
}

void onTag(void* ctx, const char* tag, size_t len) {
  Context& c = *static_cast<Context*>(ctx);
  if (isHeadingTag(tag, len, false)) {
    c.inHeading = true;
    return;
  }
  if (isHeadingTag(tag, len, true)) {
    c.inHeading = false;
    return;
  }
  if (!xmlTagIs(tag, len, "a")) {
    return;
  }
  const char* href = nullptr;
  size_t hrefLen = 0;
  if (!xmlGetAttr(tag, len, "href", &href, &hrefLen)) {
    return;
  }
  // "xxx.smil#fragment"を分ける。フラグメントが無いこともある。
  size_t fileLen = 0;
  while (fileLen < hrefLen && href[fileLen] != '#') {
    fileLen++;
  }
  if (!endsWithSmil(href, fileLen)) {
    return;
  }
  const int smilIndex = findOrAddSmil(*c.book, href, fileLen);
  if (smilIndex < 0) {
    return;
  }
  if (c.inHeading) {
    const size_t fragStart = fileLen < hrefLen ? fileLen + 1 : hrefLen;
    c.book->headings.push_back(
        {static_cast<uint16_t>(smilIndex),
         std::string(href + fragStart, hrefLen - fragStart)});
  }
}

}  // namespace

bool nccParse(XmlReadFn read, void* ctx, size_t sizeHint, NccBook& out) {
  out.smils.clear();
  out.headings.clear();
  (void)sizeHint;
  Context c{&out};
  xmlScanTags(read, ctx, onTag, &c);
  return !out.smils.empty();
}
