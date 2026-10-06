#pragma once

#include <cstdint>

// 本ごとのしおり(再生位置)の不揮発保存(Preferences/NVS)を担当する。
// 直近config::kBookmarkMax冊分を、MRU順の単一blobとして持つ。
// 書き込みはbookmarkSave()/bookmarkRemove()の呼び出し時に即座に行う
// (呼び出し側が一時停止中など、書き込み頻度が低いタイミングで呼ぶこと)。

// idの本のしおりがあればsmil/clipへ入れてtrueを返す。
bool bookmarkLoad(uint32_t id, uint16_t* smil, uint16_t* clip);

// idの本のしおりを保存する。MRUの先頭へ移し、上限を超えた最古のものは捨てる。
void bookmarkSave(uint32_t id, uint16_t smil, uint16_t clip);

// idの本のしおりを削除する(無ければ何もしない)。
void bookmarkRemove(uint32_t id);
