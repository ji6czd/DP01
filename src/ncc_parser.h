#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xml_scan.h"

// DAISY 2.02のncc.htmlから、再生順のSMIL一覧と見出し一覧を取り出す。
// 見出しの文字列は持たない(表示もTTSも想定しない)。持つのは「どのSMILのどのアンカーか」だけ。

struct NccHeading {
  uint16_t smilIndex;  // NccBook::smilsの添字
  std::string
      fragment;  // href="xxx.smil#fragment"のfragment。SMIL内の<text id>と対応
};

struct NccBook {
  // 本の再生順に並べた、重複を除いたSMILファイル名。ncc.html中の<a
  // href>の出現順 (見出し・ページ・グループのどれから参照されたものも含む)。
  std::vector<std::string> smils;
  // <h1>〜<h6>の中の<a>。ncc.html中の出現順。
  std::vector<NccHeading> headings;
};

// readから全体を読んでoutへ詰める(outは先に空にする)。sizeHintはファイルサイズ(0でも可)。
// SMILが1つ以上取れたらtrue。
bool nccParse(XmlReadFn read, void* ctx, size_t sizeHint, NccBook& out);
