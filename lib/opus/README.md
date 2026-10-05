# lib/opus — libopus 1.5.2(デコーダのみ・固定小数点)

JCBA サイマル(Ogg/Opus over WebSocket)再生用に vendor した libopus。
出典: https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz(BSD-3-Clause、COPYING を同梱)。

## 取り込んだもの / 外したもの
- `include/` … 公開ヘッダ。プロジェクトからは `#include <opus.h>` だけを使う。
- `celt/` … `celt_sources.mk` の一覧から `celt_encoder.c` を除いたもの。x86/arm/tests は無し。
- `silk/` … トップレベルの `*.c` からエンコーダ専用(enc_API, NSQ, VAD, control_*, encode_* 等)を除いたもの。
  `silk/fixed/` `silk/float/` はエンコーダ専用なので丸ごと無し。
- `src/` … `opus.c` `opus_decoder.c` `extensions.c` `repacketizer.c` のみ(multistream/projection/encoder/analysis は無し)。
- `dnn/`(DRED / Deep PLC、18MB)は無し。`ENABLE_DRED` `ENABLE_DEEP_PLC` を定義しなければ参照されない。

## ビルド設定(library.json)
- `FIXED_POINT` `DISABLE_FLOAT_API` … 固定小数点デコーダ。ESP32-S3 に FPU はあるが、固定小数点の方が
  速く、opus_decode() の int16 出力だけを使うので float API は不要。
- `VAR_ARRAYS` … CELT/SILK の作業領域を C99 VLA(スタック)に置く。デコードを呼ぶタスクのスタックは
  その分(数KB〜十数KB、実測は probe/opus_probe を参照)大きく取ること。ヒープの断片化は起きない。
- `OPUS_BUILD` … 内部ヘッダの必須定義。
- 内部の include パスは `pio_build.py` で足している(ソースが "celt.h" のように bare name で include し合うため)。

## 更新のしかた
新しいリリースの tarball を展開し、上記の方針で同じファイルを上書きコピーする。
エンコーダ専用ファイルが増減していたらリンカが教えてくれる(未定義参照なら足す、
`main_FIX.h` が無いと言われたらそのファイルはエンコーダ専用なので消す)。
