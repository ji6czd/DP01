// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_STORE
#define LOG_MODULE_TAG "BOOKMARK"
#include "bookmark_store.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstring>

#include "config.h"
#include "log_config.h"

namespace {

constexpr char kNamespace[] = "m5rajiru";
constexpr char kKeyBookmarks[] = "bookmarks";
constexpr uint8_t kVersion = 1;

struct Entry {
  uint32_t id;
  uint16_t smil;
  uint16_t clip;
};

// blobの中身: Header + Entry[count]。Entryは最近使った順。
struct Header {
  uint8_t version;
  uint8_t count;
  uint16_t reserved;
};

constexpr size_t kMaxBlobBytes = sizeof(Header) + sizeof(Entry) * config::kBookmarkMax;
static_assert(kMaxBlobBytes < 1984, "must fit in a single NVS page");

struct Table {
  uint8_t count = 0;
  Entry entries[config::kBookmarkMax];
};

Preferences s_prefs;

// 保存済みのblobをtへ読む。無い/壊れていれば空にする。
void readTable(Table& t) {
  t.count = 0;
  s_prefs.begin(kNamespace, /*readOnly=*/true);
  static uint8_t buf[kMaxBlobBytes];  // タスクのスタックを節約(呼び出しは再生タスクのみ)
  const size_t len = s_prefs.getBytes(kKeyBookmarks, buf, sizeof(buf));
  s_prefs.end();
  if (len < sizeof(Header)) {
    return;
  }
  Header h;
  memcpy(&h, buf, sizeof(h));
  if (h.version != kVersion || h.count > config::kBookmarkMax || len != sizeof(Header) + sizeof(Entry) * h.count) {
    return;
  }
  memcpy(t.entries, buf + sizeof(Header), sizeof(Entry) * h.count);
  t.count = h.count;
}

void writeTable(const Table& t) {
  static uint8_t buf[kMaxBlobBytes];  // タスクのスタックを節約(呼び出しは再生タスクのみ)
  const Header h{kVersion, t.count, 0};
  memcpy(buf, &h, sizeof(h));
  memcpy(buf + sizeof(Header), t.entries, sizeof(Entry) * t.count);
  s_prefs.begin(kNamespace, /*readOnly=*/false);
  s_prefs.putBytes(kKeyBookmarks, buf, sizeof(Header) + sizeof(Entry) * t.count);
  s_prefs.end();
}

// idの添字。無ければ-1。
int find(const Table& t, uint32_t id) {
  for (int i = 0; i < t.count; i++) {
    if (t.entries[i].id == id) {
      return i;
    }
  }
  return -1;
}

// 添字indexのエントリを削除して詰める。
void erase(Table& t, int index) {
  memmove(&t.entries[index], &t.entries[index + 1], sizeof(Entry) * (t.count - index - 1));
  t.count--;
}

}  // namespace

bool bookmarkLoad(uint32_t id, uint16_t* smil, uint16_t* clip) {
  Table t;
  readTable(t);
  const int i = find(t, id);
  if (i < 0) {
    return false;
  }
  *smil = t.entries[i].smil;
  *clip = t.entries[i].clip;
  return true;
}

void bookmarkSave(uint32_t id, uint16_t smil, uint16_t clip) {
  Table t;
  readTable(t);
  const int i = find(t, id);
  if (i >= 0) {
    erase(t, i);
  } else if (t.count >= config::kBookmarkMax) {
    t.count--;  // 最古を捨てる
  }
  memmove(&t.entries[1], &t.entries[0], sizeof(Entry) * t.count);
  t.entries[0] = {id, smil, clip};
  t.count++;
  writeTable(t);
  LOGI("saved bookmark id=%08X smil=%u clip=%u (%u books)", static_cast<unsigned>(id), static_cast<unsigned>(smil),
       static_cast<unsigned>(clip), static_cast<unsigned>(t.count));
}

void bookmarkRemove(uint32_t id) {
  Table t;
  readTable(t);
  const int i = find(t, id);
  if (i < 0) {
    return;
  }
  erase(t, i);
  writeTable(t);
  LOGI("removed bookmark id=%08X (%u books)", static_cast<unsigned>(id), static_cast<unsigned>(t.count));
}
