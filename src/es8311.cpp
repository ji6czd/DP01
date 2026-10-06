// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_HW
#define LOG_MODULE_TAG "HW"
#include "es8311.h"

#include <Arduino.h>

#include "i2c_manager.h"
#include "log_config.h"

namespace {

constexpr uint8_t kEs8311Addr = 0x18;
constexpr uint8_t kRegDacVolume = 0x32;

bool writeReg(uint8_t reg, uint8_t value) { return i2cWriteReg8(kEs8311Addr, reg, value); }

// DACボリュームレジスタは書き込み専用の扱いなので、直近に書き込んだ値をここで
// 保持しておく(volumeUp/volumeDown等が現在値を知るために使う)。
uint8_t g_currentVolume = 0;

}  // namespace

bool es8311Begin(uint8_t volume) {
  i2cManagerBegin();

  bool ok = true;
  ok = ok && writeReg(0x00, 0x80);  // RESET/ CSM POWER ON
  ok = ok && writeReg(0x01, 0xB5);  // CLOCK_MANAGER/ MCLK=BCLK
  ok = ok && writeReg(0x02, 0x18);  // CLOCK_MANAGER/ MULT_PRE=3
  ok = ok && writeReg(0x0D, 0x01);  // SYSTEM/ analog回路の電源投入
  ok = ok && writeReg(0x12, 0x00);  // SYSTEM/ DAC電源投入
  ok = ok && writeReg(0x13, 0x10);  // SYSTEM/ HPドライブへの出力を有効化
  ok = ok && writeReg(0x37, 0x08);  // DAC/ DACイコライザをバイパス
  ok = ok && writeReg(kRegDacVolume, volume);
  if (ok) g_currentVolume = volume;

  if (!ok) {
    LOGE("ES8311 register bring-up failed (I2C NACK?)");
  }
  return ok;
}

void es8311SetVolume(uint8_t volume) {
  if (writeReg(kRegDacVolume, volume)) {
    g_currentVolume = volume;
  }
}

uint8_t es8311GetVolume() { return g_currentVolume; }

i2s_chan_handle_t es8311CreateI2sTxChannel() {
  constexpr i2s_port_t kI2sPort = I2S_NUM_1;
  constexpr gpio_num_t kBckPin = GPIO_NUM_41;
  constexpr gpio_num_t kWsPin = GPIO_NUM_43;
  constexpr gpio_num_t kDoutPin = GPIO_NUM_42;
  // reconfig前の仮値。i2s_speaker.cppが最初の実データで実際のレートへreconfigするため、
  // ここでの値自体は音質に影響しない。
  constexpr uint32_t kInitialSampleRateHz = 48000;

  i2s_chan_handle_t txHandle = nullptr;
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(kI2sPort, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chanCfg, &txHandle, nullptr) != ESP_OK) {
    LOGE("I2S new channel failed");
    return nullptr;
  }

  i2s_std_config_t stdCfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kInitialSampleRateHz),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,  // Cardputer ADVはMCLK未配線(ES8311がBCLKから内部生成)
              .bclk = kBckPin,
              .ws = kWsPin,
              .dout = kDoutPin,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };

  if (i2s_channel_init_std_mode(txHandle, &stdCfg) != ESP_OK) {
    LOGE("I2S channel init std mode failed");
    return nullptr;
  }
  return txHandle;
}
