#pragma once

#include <cstddef>
#include <cstdint>

namespace board_pins {

struct ButtonBinding {
  int pin;
  uint8_t taskRegion;
  bool functionArea;
  const char* label;
};

#if defined(ARDUINO_AirM2M_CORE_ESP32C3)

constexpr int kEpaperMosi = 3;
constexpr int kEpaperSck = 2;
constexpr int kEpaperCs = 7;
constexpr int kEpaperDc = 18;
constexpr int kEpaperReset = 19;
constexpr int kEpaperBusy = 10;

constexpr int kMicrophoneBclk = 4;
constexpr int kMicrophoneLrclk = 5;
constexpr int kMicrophoneData = 6;

constexpr ButtonBinding kButtons[] = {
    {0, 0, false, "B4"},
    {1, 1, false, "B5"},
    {8, 2, false, "B6"},
    {12, 3, false, "B7"},
    {9, 0, true, "C1"},
};

#elif defined(ARDUINO_XIAO_ESP32C3)

constexpr int kEpaperMosi = 10;
constexpr int kEpaperSck = 8;
constexpr int kEpaperCs = 3;
constexpr int kEpaperDc = 4;
constexpr int kEpaperReset = 5;
constexpr int kEpaperBusy = 6;

constexpr int kMicrophoneBclk = 7;
constexpr int kMicrophoneLrclk = 21;
constexpr int kMicrophoneData = 20;

// Preserve the temporary two-button XIAO test mapping for hardware rollback.
constexpr ButtonBinding kButtons[] = {
    {2, 0, true, "D0"},
    {9, 3, false, "D9"},
};

#else
#error "Unsupported board: add its pin allocation to board_pins.h"
#endif

constexpr size_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);
static_assert(kButtonCount <= 8, "Button state is stored in one byte");

}  // namespace board_pins
