#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xml_scan.h"

// DAISY(SMIL)ファイルから<audio>クリップ列と、<text
// id>のアンカーを取り出す軽量パーサ。
// ArduinoのFile等には依存せず、読み出しは関数ポインタ経由で行う(ホストでもテストできる)。
// テキストの中身は扱わない(<text>はidとクリップ位置の対応だけ記録する)。

// clip-endが無い(=ファイル末尾まで)ことを表す値。
inline constexpr uint32_t kSmilClipToEnd = UINT32_MAX;

struct SmilClip {
  uint8_t srcIndex;  // SmilBook::sourcesの添字
  uint32_t beginMs;
  uint32_t endMs;  // kSmilClipToEndならファイル末尾まで
};

// <text
// id="...">が現れた位置。clipIndexはその直後(同じ<par>以降)の最初のクリップの添字。
// ncc.htmlの見出し(href="xxx.smil#id")からクリップへ飛ぶのに使う。見出しだけで音声の無い
// <par>も、次の音声クリップへ対応づく。末尾にあってクリップが続かない場合はclips.size()になる。
struct SmilAnchor {
  std::string id;
  uint32_t clipIndex;
};

struct SmilBook {
  std::vector<std::string>
      sources;  // 重複を除いた音声ファイル名(SMILのsrc属性そのまま)
  std::vector<SmilClip> clips;      // 文書順
  std::vector<SmilAnchor> anchors;  // 文書順
  size_t skipped = 0;               // 解釈できず捨てた<audio>の数
};

using SmilReadFn = XmlReadFn;

// readから全体を読んでoutへ詰める(outは先に空にする)。sizeHintはファイルサイズ
// (vectorのreserve用、0でも可)。クリップが1つ以上取れたらtrue。
// srcは255種類まで(超えた分は捨ててskippedに数える)。
bool smilParse(SmilReadFn read, void* ctx, size_t sizeHint, SmilBook& out);

// "npt=12.345s" / "12.345s" / "12.345" / "h:mm:ss.fff" / "mm:ss"
// をミリ秒にする。
// 浮動小数点は使わない。小数は3桁まで(以降は切り捨て)。解釈できなければfalse。
bool smilParseTime(const char* s, size_t len, uint32_t& ms);
