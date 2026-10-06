// モジュールのログ設定はlog_config.hのincludeより前に定義する。
#define LOG_MODULE_LEVEL LOG_LEVEL_HW
#define LOG_MODULE_TAG "HW"
#include "tca8418.h"

#include <Arduino.h>

#include "i2c_manager.h"
#include "log_config.h"

namespace {
// TCA8418のI2Cアドレスと割り込みＧＰＩＯ
constexpr uint8_t kTca8418Addr = 0x34;
constexpr int kTCA8418INT = 11;
// TCA8418RTWRのレジスタアドレス
constexpr uint8_t REG_CFG = 0x01;
constexpr uint8_t REG_INT_STAT = 0x02;
constexpr uint8_t REG_KEY_EVENT_A = 0x04;
constexpr uint8_t REG_KP_GPIO1 = 0x1D;
constexpr uint8_t REG_KP_GPIO2 = 0x1E;
constexpr uint8_t REG_KP_GPIO3 = 0x1F;

bool writeReg(uint8_t reg, uint8_t value) { return i2cWriteReg8(kTca8418Addr, reg, value); }
bool readReg(uint8_t reg, uint8_t* value) { return i2cReadReg8(kTca8418Addr, reg, value); }
}  // namespace

bool tca8418Begin() {
  pinMode(kTCA8418INT, INPUT_PULLUP);
  i2cManagerBegin();
  // TCA8418の初期化
  // M5Cardputer ADV回路図とデータシート（レジスタ0x1D-0x1F）によると、
  // キーパッド機能のためにROW0-6(7行)とCOL0-7(8列)を有効にする必要があります
  // (キーIDは row*10+col+1 で、commandForKeyId() の最大 68 = row6/col7 と整合)。
  // これらのビットに'1'を設定すると、ピンがキースキャンモードになります。
  // 1. キーパッドマトリックス用のROW0-6を有効化 (KP_GPIO1)
  bool ret = writeReg(REG_KP_GPIO1, 0x7F);
  // 2. キーパッドマトリックス用のCOL0-7を有効化 (KP_GPIO2)
  if (ret) ret = writeReg(REG_KP_GPIO2, 0xFF);
  // 3. 未使用のキーパッドピン (COL8, COL9) を無効化 (KP_GPIO3)
  if (ret) ret = writeReg(REG_KP_GPIO3, 0x00);
  // 4. 割り込みとキーパッドスキャン動作の設定
  // KE_IEN = 1: キーイベントのFIFOへの報告を有効化
  if (ret) ret = writeReg(REG_CFG, 0x01);  // キーイベント割り込みを有効化（FIFOレポート機能）

  // 5. クリーンなスタートを確保するため、FIFOバッファ内の古いイベントをフラッシュ。
  // REG_KEY_EVENT_A は読むたびにFIFOを1件ポップし、空になると0x00を返す(bit7=押下/
  // リリース, bit6:0=キー番号なので有効イベントが0x00になることはない)。イベント数
  // レジスタ(REG_KEY_LCK_EC)には依存せず、0x00が返るまで読み捨てる。FIFOは10段なので
  // 上限10回で頭打ちにする(読み取り失敗時もそこで抜ける)。
  LOGD("TCA8418: flushing FIFO");
  for (int i = 0; ret && i < 10; ++i) {
    uint8_t event_data = 0;
    if (!readReg(REG_KEY_EVENT_A, &event_data)) {
      ret = false;
      break;
    }
    if (event_data == 0) {
      break;  // FIFO 空
    }
  }
  // 割り込みステータスをクリア。INT_STATはwrite-1-to-clearなので、起動前に溜まって
  // いた全ビット(K_INT/GPI_INT/K_LCK_INT/OVR_FLOW_INT/CAD_INT)を落とす。ここで
  // 落とさないとINTピンがLOWのまま残り、tca8418ReadKeyEvent()のゲートが閉じない。
  if (ret) ret = writeReg(REG_INT_STAT, 0x1F);
  if (!ret) {
    LOGE("TCA8418 init failed");
  }
  return ret;
}

KeyEvent tca8418ReadKeyEvent() {
  // INTピン(オープンドレイン, アクティブLOW)はK_INTが立っている間だけLOW。
  // HIGHならFIFOは空なのでI2Cに触らずに戻る。loop()が無停止で呼ぶため、この
  // ゲートが無いとアイドル中もI2Cトランザクションを回し続けてしまう。
  if (digitalRead(kTCA8418INT) != LOW) return KeyEvent{.raw = 0};

  // REG_KEY_EVENT_Aは読むたびにFIFOを1件ポップし、空なら0x00を返す(呼び出し側の
  // 「0=イベントなし」の番兵と一致)。イベント数レジスタを先に読む必要はない。
  KeyEvent event{.raw = 0};
  if (!readReg(REG_KEY_EVENT_A, &event.raw)) return event;

  if (event.raw == 0) {
    // FIFOを出し切った。K_INTを落としてINTピンをHIGHへ戻す(write-1-to-clear)。
    // FIFOが空でないときはK_INTが立ったままになる仕様なので、この読み出しと
    // クリアの間に新しいイベントが入ってもINTはLOWのまま=取りこぼさない。
    writeReg(REG_INT_STAT, 0x01);
  }
  return event;
}
