#pragma once

#include <cstddef>
#include <cstdint>

namespace task_store {

constexpr uint8_t kRegionCount = 2;
constexpr size_t kHistoryCapacity = 30;
constexpr size_t kTextCapacity = 384;

struct CurrentTask {
  bool present;
  bool completed;
  char text[kTextCapacity];
};

struct CompletedTask {
  uint32_t sequence;
  uint8_t region;
  char text[kTextCapacity];
};

bool begin();
bool ready();
bool getCurrent(uint8_t region, CurrentTask& task);
bool setCurrent(uint8_t region, const char* text);
bool setCompleted(uint8_t region, bool completed);
bool clearCurrent(uint8_t region);
size_t completedCount();
bool getCompleted(size_t newestFirstIndex, CompletedTask& task);
bool clearCompleted();

}  // namespace task_store
