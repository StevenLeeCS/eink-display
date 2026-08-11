#pragma once

#include <cstdint>

#include "task_store.h"

namespace voice_upload {

enum class RegionEvent : uint8_t {
  Recognition,
  RecognitionNoHistory,
  NoSpeech,
  ToggleCompletion,
  Reset,
  Status,
  Error,
};

using EventCallback =
    void (*)(uint8_t region, RegionEvent event, const char* text,
             const task_store::TaskSchedule* schedule);

void setEventCallback(EventCallback callback);
bool begin();
void poll();

}  // namespace voice_upload
