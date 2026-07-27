#pragma once

#include <cstddef>
#include <cstdint>

namespace inmp441_audio {

constexpr uint32_t kSampleRate = 16000;
constexpr size_t kMaxSamplesPerRead = 256;

bool begin();
bool restartCapture();
size_t readPcm16(int16_t* output, size_t maxSamples, uint32_t timeoutMs);

}  // namespace inmp441_audio
