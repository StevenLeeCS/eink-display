#include "pomodoro.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>
#include <time.h>

namespace pomodoro {
namespace {

constexpr char kNamespace[] = "eink-pomo";
constexpr char kStateKey[] = "state";
constexpr char kHistoryKey[] = "history";
constexpr uint32_t kMagic = 0x504F4D4FU;  // "POMO"
constexpr uint8_t kVersion = 1;
constexpr uint16_t kDefaultFocusMinutes = 25;
constexpr uint16_t kDefaultBreakMinutes = 5;
constexpr uint16_t kMaxFocusMinutes = 180;
constexpr uint16_t kMaxBreakMinutes = 60;
constexpr size_t kHistoryCapacity = 10;
constexpr uint32_t kDisplayStepSeconds = 5U * 60U;
constexpr uint64_t kValidEpoch = 1700000000ULL;

struct StoredState {
  uint32_t magic;
  uint8_t version;
  uint8_t active;
  uint8_t running;
  uint8_t phase;
  uint16_t round;
  uint16_t focusMinutes;
  uint16_t breakMinutes;
  uint16_t completedFocusRounds;
  uint32_t phaseElapsedSeconds;
  uint32_t totalFocusSeconds;
  uint32_t totalBreakSeconds;
  uint64_t phaseStartedAt;
  uint32_t displayedStep;
  uint8_t targetDisplayed;
};

struct StoredHistory {
  uint32_t magic;
  uint8_t version;
  uint8_t count;
  uint8_t next;
  HistoryEntry entries[kHistoryCapacity];
};

State current = {};
StoredHistory storedHistory = {};
Presentation currentPresentation = {};
RefreshCallback refreshCallback = nullptr;
uint64_t phaseStartedAt = 0;
uint32_t displayedStep = 0;
bool targetDisplayed = false;
bool initialized = false;

uint64_t nowEpoch() {
  const time_t now = time(nullptr);
  return now >= static_cast<time_t>(kValidEpoch)
             ? static_cast<uint64_t>(now)
             : 0;
}

void setDefaults() {
  current = {};
  current.phase = Phase::Focus;
  current.round = 1;
  current.focusMinutes = kDefaultFocusMinutes;
  current.breakMinutes = kDefaultBreakMinutes;
  phaseStartedAt = 0;
  displayedStep = 0;
  targetDisplayed = false;
}

bool validStored(const StoredState& record) {
  return record.magic == kMagic && record.version == kVersion &&
         record.active <= 1 && record.running <= 1 && record.phase <= 1 &&
         record.round > 0 && record.focusMinutes >= 1 &&
         record.focusMinutes <= kMaxFocusMinutes && record.breakMinutes >= 1 &&
         record.breakMinutes <= kMaxBreakMinutes;
}

uint32_t calculatedElapsedSeconds() {
  if (!current.active || !current.running || phaseStartedAt == 0) {
    return current.phaseElapsedSeconds;
  }
  const uint64_t now = nowEpoch();
  if (now == 0 || now < phaseStartedAt) return current.phaseElapsedSeconds;
  const uint64_t elapsed = current.phaseElapsedSeconds + now - phaseStartedAt;
  return elapsed > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(elapsed);
}

uint32_t targetSeconds() {
  return static_cast<uint32_t>(current.phase == Phase::Focus
                                   ? current.focusMinutes
                                   : current.breakMinutes) *
         60U;
}

uint32_t wholeMinutes(uint32_t seconds) { return seconds / 60U; }

const char* phaseText() {
  return current.phase == Phase::Focus ? u8"专注" : u8"休息";
}

void setPresentation(const char* line1, const char* line2, const char* line3,
                     const char* emoticon) {
  snprintf(currentPresentation.line1, sizeof(currentPresentation.line1),
           "%s", line1 != nullptr ? line1 : "");
  snprintf(currentPresentation.line2, sizeof(currentPresentation.line2),
           "%s", line2 != nullptr ? line2 : "");
  snprintf(currentPresentation.line3, sizeof(currentPresentation.line3),
           "%s", line3 != nullptr ? line3 : "");
  snprintf(currentPresentation.emoticon,
           sizeof(currentPresentation.emoticon), "%s",
           emoticon != nullptr ? emoticon : "^_^");
}

const char* nextPhaseText() {
  return current.phase == Phase::Focus ? u8"休息" : u8"专注";
}

void formatTimerLine(char* output, size_t outputSize) {
  snprintf(output, outputSize, u8"第%u轮 %s%lu/%u分", current.round,
           phaseText(),
           static_cast<unsigned long>(
               wholeMinutes(calculatedElapsedSeconds())),
           current.phase == Phase::Focus ? current.focusMinutes
                                         : current.breakMinutes);
}

void formatSettingsLine(char* output, size_t outputSize) {
  snprintf(output, outputSize, u8"专注%u分/休息%u分", current.focusMinutes,
           current.breakMinutes);
}

void setReadyPresentation(bool voiceGuide) {
  char line2[48];
  char line3[64];
  snprintf(line2, sizeof(line2),
           current.running ? u8"短按暂停,双击进入%s"
                           : u8"短按开始,双击进入%s",
           nextPhaseText());
  formatSettingsLine(line3, sizeof(line3));
  setPresentation(voiceGuide ? u8"长按语音输入时长" : u8"时长设置完成",
                  line2, line3, "^_^");
}

void setCurrentPresentation() {
  if (!current.active) {
    setPresentation(u8"欢迎使用电纸便利贴!", "", "", "^_^");
    return;
  }
  char line2[48];
  char line3[64];
  const uint32_t elapsed = calculatedElapsedSeconds();
  snprintf(line2, sizeof(line2), u8"双击进入%s", nextPhaseText());
  formatTimerLine(line3, sizeof(line3));
  if (!current.running) {
    setPresentation(u8"短按开始", line2, line3, "._.");
  } else if (elapsed >= targetSeconds()) {
    snprintf(line2, sizeof(line2), u8"短按暂停,双击进入%s",
             nextPhaseText());
    setPresentation(u8"已达目标,仍在计时", line2, line3, "^-^");
  } else {
    setPresentation(u8"短按暂停", line2, line3,
                    current.phase == Phase::Focus ? ">_<" : "^_^");
  }
}

bool persistState() {
  StoredState record = {};
  record.magic = kMagic;
  record.version = kVersion;
  record.active = current.active ? 1 : 0;
  record.running = current.running ? 1 : 0;
  record.phase = static_cast<uint8_t>(current.phase);
  record.round = current.round;
  record.focusMinutes = current.focusMinutes;
  record.breakMinutes = current.breakMinutes;
  record.completedFocusRounds = current.completedFocusRounds;
  record.phaseElapsedSeconds = current.phaseElapsedSeconds;
  record.totalFocusSeconds = current.totalFocusSeconds;
  record.totalBreakSeconds = current.totalBreakSeconds;
  record.phaseStartedAt = phaseStartedAt;
  record.displayedStep = displayedStep;
  record.targetDisplayed = targetDisplayed ? 1 : 0;
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  const size_t written = preferences.putBytes(kStateKey, &record, sizeof(record));
  preferences.end();
  return written == sizeof(record);
}

bool persistHistory() {
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  const size_t written =
      preferences.putBytes(kHistoryKey, &storedHistory, sizeof(storedHistory));
  preferences.end();
  return written == sizeof(storedHistory);
}

void notify() {
  if (refreshCallback != nullptr) refreshCallback();
}

void commitRun() {
  current.phaseElapsedSeconds = calculatedElapsedSeconds();
  phaseStartedAt = 0;
}

void commitPhase(bool completed) {
  commitRun();
  if (current.phase == Phase::Focus) {
    current.totalFocusSeconds += current.phaseElapsedSeconds;
    if (completed && current.phaseElapsedSeconds > 0) {
      ++current.completedFocusRounds;
    }
  } else {
    current.totalBreakSeconds += current.phaseElapsedSeconds;
  }
}

void appendHistory() {
  if (current.totalFocusSeconds == 0 && current.totalBreakSeconds == 0) return;
  HistoryEntry& entry = storedHistory.entries[storedHistory.next];
  entry = {nowEpoch(), current.totalFocusSeconds, current.totalBreakSeconds,
           current.completedFocusRounds};
  storedHistory.next =
      static_cast<uint8_t>((storedHistory.next + 1) % kHistoryCapacity);
  if (storedHistory.count < kHistoryCapacity) ++storedHistory.count;
  persistHistory();
}

bool activate(bool voiceGuide) {
  if (current.active) return false;
  const uint16_t focus = current.focusMinutes;
  const uint16_t rest = current.breakMinutes;
  setDefaults();
  current.active = true;
  current.focusMinutes = focus;
  current.breakMinutes = rest;
  setReadyPresentation(voiceGuide);
  const bool saved = persistState();
  notify();
  return saved;
}

}  // namespace

void begin(RefreshCallback callback) {
  if (initialized) {
    refreshCallback = callback;
    return;
  }
  initialized = true;
  refreshCallback = callback;
  setDefaults();
  storedHistory = {};
  storedHistory.magic = kMagic;
  storedHistory.version = kVersion;

  Preferences preferences;
  if (preferences.begin(kNamespace, true)) {
    StoredState record = {};
    if (preferences.isKey(kStateKey) &&
        preferences.getBytes(kStateKey, &record, sizeof(record)) ==
            sizeof(record) &&
        validStored(record)) {
      current.active = record.active != 0;
      current.running = record.running != 0;
      current.phase = static_cast<Phase>(record.phase);
      current.round = record.round;
      current.focusMinutes = record.focusMinutes;
      current.breakMinutes = record.breakMinutes;
      current.completedFocusRounds = record.completedFocusRounds;
      current.phaseElapsedSeconds = record.phaseElapsedSeconds;
      current.totalFocusSeconds = record.totalFocusSeconds;
      current.totalBreakSeconds = record.totalBreakSeconds;
      phaseStartedAt = record.phaseStartedAt;
      displayedStep = record.displayedStep;
      targetDisplayed = record.targetDisplayed != 0;
      if (current.running && phaseStartedAt == 0) current.running = false;
    }
    StoredHistory historyRecord = {};
    if (preferences.isKey(kHistoryKey) &&
        preferences.getBytes(kHistoryKey, &historyRecord,
                             sizeof(historyRecord)) == sizeof(historyRecord) &&
        historyRecord.magic == kMagic &&
        historyRecord.version == kVersion &&
        historyRecord.count <= kHistoryCapacity &&
        historyRecord.next < kHistoryCapacity) {
      storedHistory = historyRecord;
    }
    preferences.end();
  }
  setCurrentPresentation();
}

void poll() {
  if (!current.active || !current.running) return;
  const uint32_t elapsed = calculatedElapsedSeconds();
  const uint32_t step = elapsed / kDisplayStepSeconds;
  const bool reached = elapsed >= targetSeconds();
  if (reached && !targetDisplayed) {
    targetDisplayed = true;
    displayedStep = step;
  } else if (step > displayedStep) {
    displayedStep = step;
  } else {
    return;
  }
  setCurrentPresentation();
  persistState();
  notify();
}

const State& state() {
  return current;
}

uint32_t elapsedSeconds() { return calculatedElapsedSeconds(); }

const Presentation& presentation() { return currentPresentation; }

bool enter() {
  return activate(true);
}

bool exit() {
  if (!current.active) return false;
  commitPhase(false);
  appendHistory();
  const uint16_t focus = current.focusMinutes;
  const uint16_t rest = current.breakMinutes;
  setDefaults();
  current.focusMinutes = focus;
  current.breakMinutes = rest;
  setCurrentPresentation();
  const bool saved = persistState();
  notify();
  return saved;
}

bool toggleRunning() {
  if (!current.active) return false;
  if (current.running) {
    commitRun();
    current.running = false;
  } else {
    const uint64_t now = nowEpoch();
    if (now == 0) return false;
    phaseStartedAt = now;
    current.running = true;
  }
  setCurrentPresentation();
  const bool saved = persistState();
  notify();
  return saved;
}

bool switchPhase() {
  if (!current.active) return false;
  const Phase previous = current.phase;
  const uint32_t actual = calculatedElapsedSeconds();
  commitPhase(true);
  if (previous == Phase::Focus) {
    current.phase = Phase::Break;
  } else {
    current.phase = Phase::Focus;
    ++current.round;
  }
  current.running = false;
  current.phaseElapsedSeconds = 0;
  phaseStartedAt = 0;
  displayedStep = 0;
  targetDisplayed = false;
  char line1[48];
  char line2[48];
  char line3[64];
  snprintf(line1, sizeof(line1), u8"%s%lu分,已切换",
           previous == Phase::Focus ? u8"专注" : u8"休息",
           static_cast<unsigned long>(wholeMinutes(actual)));
  snprintf(line2, sizeof(line2), u8"短按开始,双击进入%s", nextPhaseText());
  formatTimerLine(line3, sizeof(line3));
  setPresentation(line1, line2, line3, "^-^");
  const bool saved = persistState();
  notify();
  return saved;
}

bool configure(uint16_t focusMinutes, uint16_t breakMinutes) {
  if (focusMinutes < 1 || focusMinutes > kMaxFocusMinutes ||
      breakMinutes < 1 || breakMinutes > kMaxBreakMinutes) {
    return false;
  }
  current.focusMinutes = focusMinutes;
  current.breakMinutes = breakMinutes;
  const uint32_t elapsed = calculatedElapsedSeconds();
  displayedStep = elapsed / kDisplayStepSeconds;
  targetDisplayed = elapsed >= targetSeconds();
  if (current.active) {
    if (current.running) {
      setCurrentPresentation();
    } else {
      setReadyPresentation(false);
    }
  }
  const bool saved = persistState();
  if (current.active) notify();
  return saved;
}

bool applyVoiceMinutes(int focusMinutes, int breakMinutes) {
  if (focusMinutes < 0 && breakMinutes < 0) return false;
  const uint16_t focus = focusMinutes >= 0
                             ? static_cast<uint16_t>(focusMinutes)
                             : current.focusMinutes;
  const uint16_t rest = breakMinutes >= 0
                            ? static_cast<uint16_t>(breakMinutes)
                            : current.breakMinutes;
  if (focus < 1 || focus > kMaxFocusMinutes || rest < 1 ||
      rest > kMaxBreakMinutes) {
    return false;
  }
  if (!current.active) {
    current.focusMinutes = focus;
    current.breakMinutes = rest;
    return activate(false);
  }
  return configure(focus, rest);
}

void showSettingsError() {
  char line3[64];
  formatSettingsLine(line3, sizeof(line3));
  setPresentation(u8"时间设置失败,请重试", u8"长按语音输入时长",
                  line3, ">_<");
  notify();
}

size_t historyCount() { return storedHistory.count; }

bool history(size_t newestFirstIndex, HistoryEntry& entry) {
  if (newestFirstIndex >= storedHistory.count) return false;
  const size_t index =
      (storedHistory.next + kHistoryCapacity - 1 - newestFirstIndex) %
      kHistoryCapacity;
  entry = storedHistory.entries[index];
  return true;
}

bool clearHistory() {
  storedHistory = {};
  storedHistory.magic = kMagic;
  storedHistory.version = kVersion;
  return persistHistory();
}

}  // namespace pomodoro
