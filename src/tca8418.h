#pragma once

#include <cstdint>

// TCA8418のキーイベントを表す構造体。stateは押下状態、keyはキー番号、rawは生の8ビット値。
struct KeyEvent {
  union {
    struct {
      uint8_t key : 7;
      bool state : 1;
    };
    uint8_t raw;
  };
};

bool tca8418Begin();
KeyEvent tca8418ReadKeyEvent();
