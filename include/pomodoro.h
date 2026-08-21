#pragma once

#include <cstddef>
#include <cstdint>

namespace pomodoro {

enum class Phase : uint8_t { Focus = 0, Break = 1 };

struct State {
  bool active;
  bool running;
  Phase phase;
  uint16_t round;
  uint16_t focusMinutes;
  uint16_t breakMinutes;
  uint32_t phaseElapsedSeconds;
  uint32_t totalFocusSeconds;
  uint32_t totalBreakSeconds;
  uint16_t completedFocusRounds;
};

struct HistoryEntry {
  uint64_t endedAt;
  uint32_t focusSeconds;
  uint32_t breakSeconds;
  uint16_t focusRounds;
};

struct Presentation {
  char line1[48];
  char line2[48];
  char line3[64];
  char emoticon[8];
};

using RefreshCallback = void (*)();

void begin(RefreshCallback callback);
void poll();
const State& state();
uint32_t elapsedSeconds();
const Presentation& presentation();

bool enter();
bool exit();
bool toggleRunning();
bool switchPhase();
bool configure(uint16_t focusMinutes, uint16_t breakMinutes);
bool applyVoiceMinutes(int focusMinutes, int breakMinutes);
void showSettingsError();

size_t historyCount();
bool history(size_t newestFirstIndex, HistoryEntry& entry);
bool clearHistory();

}  // namespace pomodoro
