# sonic

話速変換ライブラリ Sonic(Bill Cox、Apache 2.0)の C 版を vendor したもの。
sonic.c / sonic.h / LICENSE は無改変。

- 取得元: https://github.com/waywardgeek/sonic
- コミット: b93885dcb70aae50c6f76b0fe4e0868f029a077e (2026-03-14)

src/mp3_player.cpp が、デコード済みPCMをi2s_speakerへ渡す手前で使う。
