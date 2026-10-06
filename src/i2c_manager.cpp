#include "i2c_manager.h"

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr int kSdaPin = 8;
constexpr int kSclPin = 9;
constexpr uint32_t kI2cFreqHz = 100000;

SemaphoreHandle_t g_lock = nullptr;

}  // namespace

void i2cManagerBegin() {
  Wire.begin(kSdaPin, kSclPin, kI2cFreqHz);
  if (g_lock == nullptr) {
    g_lock = xSemaphoreCreateMutex();
  }
}

bool i2cWriteReg8(uint8_t devAddr, uint8_t reg, uint8_t value) {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  Wire.beginTransmission(devAddr);
  Wire.write(reg);
  Wire.write(value);
  bool ok = Wire.endTransmission() == 0;
  xSemaphoreGive(g_lock);
  return ok;
}

bool i2cReadReg8(uint8_t devAddr, uint8_t reg, uint8_t* value) {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  Wire.beginTransmission(devAddr);
  Wire.write(reg);
  bool ok = Wire.endTransmission(false) == 0;  // リピートスタート(ストップコンディションを出さない)
  if (ok) {
    ok = Wire.requestFrom(static_cast<int>(devAddr), 1) == 1;
    if (ok) {
      *value = Wire.read();
    }
  }
  xSemaphoreGive(g_lock);
  return ok;
}
