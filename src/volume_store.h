#pragma once

#include <cstdint>

// ボリューム設定の不揮発保存(Preferences/NVS)を担当する。
// フラッシュの書き込み回数を抑えるため、変更の都度は書き込まず、一定時間
// キー操作(=ボリューム変更)が無かった場合にのみ実際に書き込む(デバウンス)。

// 起動時に呼ぶ。保存済みの値があればそれを、無ければdefaultVolumeを返す。
uint8_t volumeStoreLoad(uint8_t defaultVolume);

// ボリュームが変更されるたび(volumeUp()/volumeDown()から)呼ぶ。
// この時点では書き込まない。実際の書き込みはvolumeStoreTick()側で行う。
void volumeStoreNotifyChanged(uint8_t volume);

// loop()から毎回呼ぶ。デバウンス期間(config::kVolumeSaveDebounceMs)経過後、
// 変更が溜まっていれば実際にPreferencesへ書き込む。
void volumeStoreTick();
