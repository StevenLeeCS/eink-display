#pragma once

#include <cstddef>
#include <cstdint>

#include "ui_layout.h"

namespace task_store {

constexpr uint8_t kRegionCount = ui_layout::kTaskRegionCount;
constexpr size_t kHistoryCapacity = 30;
constexpr size_t kTextCapacity = 384;

enum class ScheduleKind : uint8_t {
  None = 0,
  Exact = 1,
  Window = 2,
};

enum class ReminderKind : uint8_t {
  DueSoon,
  DueCheck,
};

struct TaskSchedule {
  ScheduleKind kind;
  uint64_t startAt;
  uint64_t endAt;
};

struct CurrentTask {
  bool present;
  bool completed;
  bool historyEligible;
  bool dueSoonSent;
  bool dueCheckSent;
  uint32_t completionHistorySequence;
  uint32_t revision;
  TaskSchedule schedule;
  char text[kTextCapacity];
};

struct CompletedTask {
  uint32_t sequence;
  uint8_t region;
  uint64_t completedAt;
  char text[kTextCapacity];
};

struct FunctionState {
  uint8_t scene;
  uint64_t shownAt;
  uint32_t lastWarmSlot;
};

bool begin();
bool ready();
bool getCurrent(uint8_t region, CurrentTask& task);
bool setCurrent(uint8_t region, const char* text,
                bool historyEligible = true,
                TaskSchedule schedule = {});
bool setCompleted(uint8_t region, bool completed, uint64_t completedAt = 0);
bool markReminderSent(uint8_t region, ReminderKind reminder);
bool clearCurrent(uint8_t region);
size_t completedCount();
bool getCompleted(size_t newestFirstIndex, CompletedTask& task);
bool clearCompleted();
bool getFunctionState(FunctionState& state);
bool setFunctionState(uint8_t scene, uint64_t shownAt);
bool setLastWarmSlot(uint32_t slot);

}  // namespace task_store
