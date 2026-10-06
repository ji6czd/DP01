#pragma once

#include <cstddef>
#include <cstdint>

// SMIL/ncc.htmlから必要な属性だけを拾うための、XML/HTMLの最小限のタグ走査。
// Arduinoには依存しない(読み出しは関数ポインタ経由)。
// Shift_JIS/UTF-8どちらでも、必要なのはASCIIのタグ名・属性値だけなのでバイト単位で走査する
// (Shift_JISの2バイト目は0x40以上で、'<' '>' '"' と衝突しない)。

// 読み出す側。lenまで読んで実際に読めたバイト数を返す。0でEOF/エラー。
using XmlReadFn = size_t (*)(void* ctx, uint8_t* buf, size_t len);

// タグ(先頭の'<'と末尾の'>'を除いた文字列)ごとに呼ばれる。tagはNUL終端されない。
// 閉じタグは"/h1"のように'/'で始まる。コメントや宣言(<!...>,
// <?...?>)も渡される。
using XmlTagFn = void (*)(void* ctx, const char* tag, size_t len);

// readから全体を読み、タグごとにonTagを呼ぶ。255バイトを超えるタグは捨てる。
void xmlScanTags(XmlReadFn read, void* readCtx, XmlTagFn onTag, void* tagCtx);

// タグ名の長さ。閉じタグの'/'も名前に含む("/h1"なら3)。
size_t xmlTagNameLen(const char* tag, size_t len);

// s[0..len)が、小文字で書いたnameと(大文字小文字を無視して)一致するか。
bool xmlNameEquals(const char* s, size_t len, const char* name);

// タグ名が(小文字で書いた)nameと一致するか。閉じタグは"/a"のように指定する。
bool xmlTagIs(const char* tag, size_t len, const char* name);

// 属性nameの値(引用符の中身)を取り出す。nameは小文字で指定する。値は引用符(" か
// ')で 囲まれている前提。無ければfalse。
bool xmlGetAttr(const char* tag, size_t len, const char* name,
                const char** value, size_t* valueLen);
