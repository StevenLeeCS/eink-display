#include "task_store.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <cstring>
#include <new>

namespace task_store {
namespace {

constexpr char kStorePath[] = "/tasks.bin";
constexpr char kTempPath[] = "/tasks.tmp";
constexpr char kBackupPath[] = "/tasks.bak";
constexpr uint32_t kStoreMagic = 0x5441534BU;  // "TASK"
constexpr uint8_t kStoreVersion = 3;

struct StoredCurrentTask {
  uint8_t present;
  uint8_t completed;
  uint8_t suppressHistory;
  uint8_t reserved;
  uint32_t completionHistorySequence;
  char text[kTextCapacity];
};

struct StoredCompletedTask {
  uint32_t sequence;
  uint8_t region;
  uint8_t reserved[3];
  char text[kTextCapacity];
};

struct StoreRecord {
  uint32_t magic;
  uint8_t version;
  uint8_t historyCount;
  uint8_t nextHistoryIndex;
  uint8_t reserved;
  uint32_t nextSequence;
  StoredCurrentTask current[kRegionCount];
  StoredCompletedTask history[kHistoryCapacity];
  uint32_t checksum;
};

StoreRecord store = {};
bool storeReady = false;

uint32_t checksum(const StoreRecord& record) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  constexpr size_t kChecksumOffset = offsetof(StoreRecord, checksum);
  uint32_t value = 2166136261U;
  for (size_t i = 0; i < kChecksumOffset; ++i) {
    value ^= bytes[i];
    value *= 16777619U;
  }
  return value;
}

void initializeEmptyStore() {
  store = {};
  store.magic = kStoreMagic;
  store.version = kStoreVersion;
  store.nextSequence = 1;
  store.checksum = checksum(store);
}

bool validText(const char* text, size_t capacity) {
  return text != nullptr && text[capacity - 1] == '\0';
}

bool historyContainsSequence(const StoreRecord& candidate, uint32_t sequence) {
  if (sequence == 0) return false;
  const size_t oldest =
      (candidate.nextHistoryIndex + kHistoryCapacity -
       candidate.historyCount) %
      kHistoryCapacity;
  for (size_t offset = 0; offset < candidate.historyCount; ++offset) {
    const size_t index = (oldest + offset) % kHistoryCapacity;
    if (candidate.history[index].sequence == sequence) return true;
  }
  return false;
}

bool validStore(const StoreRecord& candidate) {
  if (candidate.magic != kStoreMagic ||
      candidate.version != kStoreVersion ||
      candidate.historyCount > kHistoryCapacity ||
      candidate.nextHistoryIndex >= kHistoryCapacity ||
      candidate.nextSequence == 0 || candidate.checksum != checksum(candidate)) {
    return false;
  }
  for (uint8_t region = 0; region < kRegionCount; ++region) {
    const StoredCurrentTask& task = candidate.current[region];
    if (task.present > 1 || task.completed > 1 || task.suppressHistory > 1 ||
        !validText(task.text, sizeof(task.text))) {
      return false;
    }
    if (task.completionHistorySequence != 0 &&
        (task.present == 0 || task.completed == 0 ||
         task.suppressHistory != 0 ||
         !historyContainsSequence(candidate,
                                  task.completionHistorySequence))) {
      return false;
    }
  }
  const size_t oldest =
      (candidate.nextHistoryIndex + kHistoryCapacity -
       candidate.historyCount) %
      kHistoryCapacity;
  for (size_t offset = 0; offset < candidate.historyCount; ++offset) {
    const size_t index = (oldest + offset) % kHistoryCapacity;
    const StoredCompletedTask& task = candidate.history[index];
    if (task.sequence == 0 || task.region >= kRegionCount ||
        !validText(task.text, sizeof(task.text))) {
      return false;
    }
  }
  return true;
}

void copyUtf8Text(char* destination, size_t capacity, const char* source) {
  if (capacity == 0) return;
  destination[0] = '\0';
  if (source == nullptr) return;

  const uint8_t* input = reinterpret_cast<const uint8_t*>(source);
  size_t sourceOffset = 0;
  size_t outputLength = 0;
  while (input[sourceOffset] != 0 && outputLength < capacity - 1) {
    const uint8_t first = input[sourceOffset];
    size_t codepointBytes = 1;
    if ((first & 0xE0U) == 0xC0U) {
      codepointBytes = 2;
    } else if ((first & 0xF0U) == 0xE0U) {
      codepointBytes = 3;
    } else if ((first & 0xF8U) == 0xF0U) {
      codepointBytes = 4;
    }
    if (outputLength + codepointBytes >= capacity) break;
    for (size_t i = 0; i < codepointBytes; ++i) {
      if (input[sourceOffset + i] == 0) {
        destination[outputLength] = '\0';
        return;
      }
      destination[outputLength++] =
          static_cast<char>(input[sourceOffset + i]);
    }
    sourceOffset += codepointBytes;
  }
  destination[outputLength] = '\0';
}

bool loadFrom(const char* path, StoreRecord& candidate) {
  File file = LittleFS.open(path, FILE_READ);
  if (!file || file.size() != sizeof(candidate)) {
    if (file) file.close();
    return false;
  }
  const size_t bytesRead =
      file.read(reinterpret_cast<uint8_t*>(&candidate), sizeof(candidate));
  file.close();
  return bytesRead == sizeof(candidate) && validStore(candidate);
}

bool persist() {
  if (!storeReady) return false;
  store.checksum = checksum(store);

  LittleFS.remove(kTempPath);
  File file = LittleFS.open(kTempPath, FILE_WRITE);
  if (!file) return false;
  const size_t bytesWritten =
      file.write(reinterpret_cast<const uint8_t*>(&store), sizeof(store));
  file.flush();
  file.close();
  if (bytesWritten != sizeof(store)) {
    LittleFS.remove(kTempPath);
    return false;
  }

  LittleFS.remove(kBackupPath);
  if (LittleFS.exists(kStorePath) &&
      !LittleFS.rename(kStorePath, kBackupPath)) {
    LittleFS.remove(kTempPath);
    return false;
  }
  if (!LittleFS.rename(kTempPath, kStorePath)) {
    if (LittleFS.exists(kBackupPath)) {
      LittleFS.rename(kBackupPath, kStorePath);
    }
    return false;
  }
  LittleFS.remove(kBackupPath);
  return true;
}

void clearCurrentHistoryReference(uint32_t sequence) {
  if (sequence == 0) return;
  for (uint8_t region = 0; region < kRegionCount; ++region) {
    if (store.current[region].completionHistorySequence == sequence) {
      store.current[region].completionHistorySequence = 0;
    }
  }
}

uint32_t appendCompleted(uint8_t region, const char* text) {
  StoredCompletedTask& completed = store.history[store.nextHistoryIndex];
  if (store.historyCount == kHistoryCapacity) {
    clearCurrentHistoryReference(completed.sequence);
  }
  completed = {};
  completed.sequence = store.nextSequence++;
  if (store.nextSequence == 0) store.nextSequence = 1;
  completed.region = region;
  copyUtf8Text(completed.text, sizeof(completed.text), text);
  store.nextHistoryIndex =
      static_cast<uint8_t>((store.nextHistoryIndex + 1) % kHistoryCapacity);
  if (store.historyCount < kHistoryCapacity) ++store.historyCount;
  return completed.sequence;
}

bool removeCompleted(uint32_t sequence) {
  if (sequence == 0 || store.historyCount == 0) return false;
  const size_t oldest =
      (store.nextHistoryIndex + kHistoryCapacity - store.historyCount) %
      kHistoryCapacity;
  size_t targetOffset = store.historyCount;
  for (size_t offset = 0; offset < store.historyCount; ++offset) {
    const size_t index = (oldest + offset) % kHistoryCapacity;
    if (store.history[index].sequence == sequence) {
      targetOffset = offset;
      break;
    }
  }
  if (targetOffset == store.historyCount) return false;

  for (size_t offset = targetOffset; offset + 1 < store.historyCount;
       ++offset) {
    const size_t destination = (oldest + offset) % kHistoryCapacity;
    const size_t source = (oldest + offset + 1) % kHistoryCapacity;
    store.history[destination] = store.history[source];
  }
  const size_t last =
      (oldest + store.historyCount - 1) % kHistoryCapacity;
  store.history[last] = {};
  --store.historyCount;
  store.nextHistoryIndex = static_cast<uint8_t>(last);
  return true;
}

}  // namespace

bool begin() {
  if (storeReady) return true;
  if (!LittleFS.begin(true)) {
    Serial.println("ERROR: Could not mount task storage.");
    return false;
  }

  StoreRecord* candidate = new (std::nothrow) StoreRecord{};
  if (candidate == nullptr) {
    Serial.println("ERROR: Not enough memory to load task storage.");
    return false;
  }
  const bool loaded = loadFrom(kStorePath, *candidate) ||
                      loadFrom(kBackupPath, *candidate);
  if (loaded) {
    store = *candidate;
  } else {
    initializeEmptyStore();
  }
  delete candidate;
  storeReady = true;
  if (!loaded && !persist()) {
    Serial.println("ERROR: Could not initialize task storage file.");
    storeReady = false;
  }
  return storeReady;
}

bool ready() { return storeReady; }

bool getCurrent(uint8_t region, CurrentTask& task) {
  if (!storeReady || region >= kRegionCount) return false;
  const StoredCurrentTask& stored = store.current[region];
  task.present = stored.present != 0;
  task.completed = stored.completed != 0;
  task.historyEligible = task.present && stored.suppressHistory == 0;
  task.completionHistorySequence = stored.completionHistorySequence;
  copyUtf8Text(task.text, sizeof(task.text), stored.text);
  return true;
}

bool setCurrent(uint8_t region, const char* text, bool historyEligible) {
  if (!storeReady || region >= kRegionCount || text == nullptr ||
      text[0] == '\0') {
    return false;
  }
  StoredCurrentTask& task = store.current[region];
  task = {};
  task.present = 1;
  task.suppressHistory = historyEligible ? 0 : 1;
  copyUtf8Text(task.text, sizeof(task.text), text);
  return persist();
}

bool setCompleted(uint8_t region, bool completed) {
  if (!storeReady || region >= kRegionCount ||
      store.current[region].present == 0) {
    return false;
  }
  StoredCurrentTask& task = store.current[region];
  task.completed = completed ? 1 : 0;
  if (completed && task.completionHistorySequence == 0 &&
      task.suppressHistory == 0) {
    task.completionHistorySequence = appendCompleted(region, task.text);
  } else if (!completed && task.completionHistorySequence != 0) {
    removeCompleted(task.completionHistorySequence);
    task.completionHistorySequence = 0;
  }
  return persist();
}

bool clearCurrent(uint8_t region) {
  if (!storeReady || region >= kRegionCount) return false;
  store.current[region] = {};
  return persist();
}

size_t completedCount() { return storeReady ? store.historyCount : 0; }

bool getCompleted(size_t newestFirstIndex, CompletedTask& task) {
  if (!storeReady || newestFirstIndex >= store.historyCount) return false;
  const size_t index =
      (store.nextHistoryIndex + kHistoryCapacity - 1 - newestFirstIndex) %
      kHistoryCapacity;
  const StoredCompletedTask& stored = store.history[index];
  task.sequence = stored.sequence;
  task.region = stored.region;
  copyUtf8Text(task.text, sizeof(task.text), stored.text);
  return true;
}

bool clearCompleted() {
  if (!storeReady) return false;
  memset(store.history, 0, sizeof(store.history));
  store.historyCount = 0;
  store.nextHistoryIndex = 0;
  for (uint8_t region = 0; region < kRegionCount; ++region) {
    store.current[region].completionHistorySequence = 0;
  }
  return persist();
}

}  // namespace task_store
