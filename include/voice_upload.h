#pragma once

#include <cstdint>

namespace voice_upload {

enum class RegionEvent : uint8_t {
  Recognition,
  NoSpeech,
  ToggleCompletion,
  Reset,
  Status,
  Error,
};

using EventCallback =
    void (*)(uint8_t region, RegionEvent event, const char* text);

void setEventCallback(EventCallback callback);
bool begin();
void poll();

}  // namespace voice_upload
