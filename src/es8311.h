#pragma once

#include <driver/i2s_std.h>

#include <cstdint>

// ES8311コーデック(Cardputer ADV搭載)のI2Cブリングアップと音量制御を担当する。
// レジスタ列はM5Unifiedの`_speaker_enabled_cb_cardputer_adv`と同一(内部I2C, addr 0x18)。

// I2Cバスを初期化し、コーデックを起動する。i2sSpeakerBegin()より先に呼ぶこと
// (M5Unifiedの初期化順序と同じく、I2Sのクロックが動く前にコーデック側のクロック
//  ソース設定[0x01 MCLK=BCLK]を書き込む)。
bool es8311Begin(uint8_t volume);

// DACボリュームレジスタ(0x32)を直接書き込む。0x00=ミュート、0xBFで±0dB、
// 0.5dB/stepで0xFFまで(+32dB相当)。M5Unified側のsetVolume()とはスケールが
// 異なる(あちらはPCMへのソフトウェア乗算)ため、実機で聴きながら調整すること。
void es8311SetVolume(uint8_t volume);

// 直近にes8311Begin()/es8311SetVolume()で書き込んだボリューム値を返す
// (レジスタ自体は書き込み専用として扱っているため、この関数はキャッシュを返す)。
uint8_t es8311GetVolume();

// Cardputer ADVの配線(GPIO41=BCK, GPIO43=WS, GPIO42=DOUT, MCLK未配線)でI2S TXチャンネルを
// 生成・std_mode初期化する(enable前の状態で返す。enable以降のライフサイクル管理は
// 呼び出し元のi2sSpeakerBegin()が行う)。失敗時はnullptrを返す。
i2s_chan_handle_t es8311CreateI2sTxChannel();
