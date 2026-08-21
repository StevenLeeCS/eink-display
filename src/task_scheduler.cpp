#include "task_scheduler.h"

#include <Arduino.h>
#include <time.h>

#include "device_settings.h"
#include "pomodoro.h"
#include "task_store.h"

namespace task_scheduler {
namespace {

constexpr uint64_t kValidClockEpoch = 1700000000ULL;
constexpr uint64_t kDueSoonLeadSeconds = 30ULL * 60ULL;
constexpr uint64_t kDueCheckLeaseSeconds = 3ULL * 60ULL * 60ULL;
constexpr uint64_t kInteractionLeaseSeconds = 3ULL * 60ULL * 60ULL;
constexpr uint32_t kPollIntervalMs = 15000;
constexpr uint8_t kWarmHours[] = {8, 11, 14, 17, 20};
constexpr uint8_t kWarmWindowMinutes = 5;

SceneCallback sceneCallback = nullptr;
function_area::Scene currentScene = function_area::Scene::Welcome;
uint64_t currentSceneShownAt = 0;
uint32_t lastWarmSlot = 0;
uint32_t lastPollAt = 0;
bool clockReadyLogged = false;
bool clockWaitingLogged = false;

bool automaticEnabled() {
  const device_settings::ProfileSettings& profile =
      device_settings::profile();
  return profile.functionTestEnabled && profile.aiSceneTestEnabled;
}

uint64_t nowEpoch() {
  const time_t now = time(nullptr);
  return now >= static_cast<time_t>(kValidClockEpoch)
             ? static_cast<uint64_t>(now)
             : 0;
}

bool activeTask(const task_store::CurrentTask& task) {
  return task.present && !task.completed && task.historyEligible &&
         task.schedule.kind != task_store::ScheduleKind::None;
}

uint64_t dueAt(const task_store::CurrentTask& task) {
  return task.schedule.endAt;
}

uint64_t dueSoonAt(const task_store::CurrentTask& task) {
  const uint64_t start = task.schedule.startAt;
  return start > kDueSoonLeadSeconds ? start - kDueSoonLeadSeconds : 1;
}

void showScene(function_area::Scene scene, uint64_t now, bool force = false) {
  if (!automaticEnabled()) return;
  if (!force && currentScene == scene) return;
  currentScene = scene;
  currentSceneShownAt = now;
  if (!task_store::setFunctionState(static_cast<uint8_t>(scene), now)) {
    Serial.println("WARNING: Function scene state could not be saved.");
  }
  if (sceneCallback != nullptr) sceneCallback(scene);
}

function_area::Scene urgentScene(uint64_t now) {
  bool dueSoon = false;
  for (uint8_t region = 0; region < task_store::kRegionCount; ++region) {
    task_store::CurrentTask task = {};
    if (!task_store::getCurrent(region, task) || !activeTask(task)) continue;
    if (now >= dueAt(task)) return function_area::Scene::DueCheck;
    if (now >= dueSoonAt(task)) dueSoon = true;
  }
  return dueSoon ? function_area::Scene::DueSoon
                 : function_area::Scene::Welcome;
}

bool emitNewDueReminder(uint64_t now) {
  bool hasNewDueCheck = false;
  bool hasNewDueSoon = false;
  for (uint8_t region = 0; region < task_store::kRegionCount; ++region) {
    task_store::CurrentTask task = {};
    if (!task_store::getCurrent(region, task) || !activeTask(task)) continue;
    if (now >= dueAt(task) && !task.dueCheckSent) hasNewDueCheck = true;
    if (now >= dueSoonAt(task) && now < dueAt(task) &&
        !task.dueSoonSent) {
      hasNewDueSoon = true;
    }
  }

  if (hasNewDueCheck) {
    for (uint8_t region = 0; region < task_store::kRegionCount; ++region) {
      task_store::CurrentTask task = {};
      if (!task_store::getCurrent(region, task) || !activeTask(task) ||
          now < dueAt(task) || task.dueCheckSent) {
        continue;
      }
      if (!task_store::markReminderSent(
              region, task_store::ReminderKind::DueCheck)) {
        Serial.println("WARNING: Due reminder state could not be saved.");
      }
    }
    showScene(function_area::Scene::DueCheck, now, true);
    return true;
  }
  if (hasNewDueSoon) {
    for (uint8_t region = 0; region < task_store::kRegionCount; ++region) {
      task_store::CurrentTask task = {};
      if (!task_store::getCurrent(region, task) || !activeTask(task) ||
          now < dueSoonAt(task) || now >= dueAt(task) || task.dueSoonSent) {
        continue;
      }
      if (!task_store::markReminderSent(
              region, task_store::ReminderKind::DueSoon)) {
        Serial.println("WARNING: Due-soon reminder state could not be saved.");
      }
    }
    showScene(function_area::Scene::DueSoon, now, true);
    return true;
  }
  return false;
}

uint32_t warmSlotKey(uint64_t now) {
  time_t raw = static_cast<time_t>(now);
  struct tm local = {};
  if (localtime_r(&raw, &local) == nullptr ||
      local.tm_min >= kWarmWindowMinutes) {
    return 0;
  }
  uint8_t slot = 0;
  for (uint8_t index = 0; index < sizeof(kWarmHours); ++index) {
    if (local.tm_hour == kWarmHours[index]) {
      slot = static_cast<uint8_t>(index + 1);
      break;
    }
  }
  if (slot == 0) return 0;
  return static_cast<uint32_t>(local.tm_year + 1900) * 100000U +
         static_cast<uint32_t>(local.tm_yday + 1) * 10U + slot;
}

bool sceneBlocksWarm(uint64_t now) {
  if (currentScene == function_area::Scene::DueSoon) {
    return urgentScene(now) == function_area::Scene::DueSoon;
  }
  if (currentScene == function_area::Scene::DueCheck) {
    return currentSceneShownAt != 0 &&
           now < currentSceneShownAt + kDueCheckLeaseSeconds;
  }
  return (currentScene == function_area::Scene::NextAction ||
          currentScene == function_area::Scene::Summary) &&
         currentSceneShownAt != 0 &&
         now < currentSceneShownAt + kInteractionLeaseSeconds;
}

void evaluateAfterTaskChange(function_area::Scene fallback) {
  if (!automaticEnabled()) return;
  const uint64_t now = nowEpoch();
  if (now != 0) {
    if (emitNewDueReminder(now)) return;
    const function_area::Scene urgent = urgentScene(now);
    if (urgent != function_area::Scene::Welcome && urgent == currentScene)
      return;
  }
  showScene(fallback, now);
}

}  // namespace

void begin(SceneCallback callback) {
  sceneCallback = callback;
  configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org");

  task_store::FunctionState state = {};
  if (task_store::getFunctionState(state) &&
      function_area::isValid(static_cast<function_area::Scene>(state.scene))) {
    currentScene = static_cast<function_area::Scene>(state.scene);
    currentSceneShownAt = state.shownAt;
    lastWarmSlot = state.lastWarmSlot;
  }
  if (automaticEnabled() && sceneCallback != nullptr) {
    sceneCallback(currentScene);
  }
  lastPollAt = millis() - kPollIntervalMs;
}

void poll() {
  if (pomodoro::state().active) {
    lastPollAt = millis();
    return;
  }
  if (!automaticEnabled() || millis() - lastPollAt < kPollIntervalMs) return;
  lastPollAt = millis();
  const uint64_t now = nowEpoch();
  if (now == 0) {
    if (!clockWaitingLogged) {
      Serial.println("Waiting for network time before scheduled reminders.");
      clockWaitingLogged = true;
    }
    return;
  }
  if (!clockReadyLogged) {
    Serial.println("Task reminder clock synchronized.");
    clockReadyLogged = true;
    if (currentSceneShownAt == 0 &&
        (currentScene == function_area::Scene::NextAction ||
         currentScene == function_area::Scene::Summary)) {
      currentSceneShownAt = now;
      task_store::setFunctionState(static_cast<uint8_t>(currentScene), now);
    }
  }

  if (emitNewDueReminder(now)) return;

  const uint32_t warmSlot = warmSlotKey(now);
  if (warmSlot == 0 || warmSlot == lastWarmSlot) return;
  lastWarmSlot = warmSlot;
  if (!task_store::setLastWarmSlot(warmSlot)) {
    Serial.println("WARNING: Warm reminder slot could not be saved.");
  }
  if (!sceneBlocksWarm(now)) {
    showScene(function_area::Scene::Warm, now, true);
  }
}

void taskStored() {
  if (pomodoro::state().active) return;
  evaluateAfterTaskChange(function_area::Scene::NextAction);
}

void taskRemoved() {
  if (pomodoro::state().active) return;
  evaluateAfterTaskChange(function_area::Scene::Welcome);
}

void completionChanged(bool completed) {
  if (pomodoro::state().active) return;
  evaluateAfterTaskChange(completed ? function_area::Scene::Summary
                                    : function_area::Scene::NextAction);
}

void settingsChanged() {
  if (pomodoro::state().active) return;
  if (!automaticEnabled()) return;
  evaluateAfterTaskChange(currentScene);
  if (sceneCallback != nullptr) sceneCallback(currentScene);
}

void resetToWelcome() {
  if (pomodoro::state().active) return;
  currentScene = function_area::Scene::Welcome;
  currentSceneShownAt = nowEpoch();
  lastPollAt = millis();
  if (!task_store::setFunctionState(
          static_cast<uint8_t>(currentScene), currentSceneShownAt)) {
    Serial.println("WARNING: Welcome scene state could not be saved.");
  }
}

}  // namespace task_scheduler
