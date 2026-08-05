#include "voice_upload.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <cstring>

#include "device_settings.h"
#include "inmp441_audio.h"
#include "task_processing.h"
#include "ui_layout.h"
#include "wifi_provisioning.h"

namespace voice_upload {
namespace {

constexpr uint32_t kMaxRecordingSeconds = 60;
constexpr uint32_t kWriteTimeoutMs = 5000;
constexpr uint32_t kButtonDebounceMs = 40;
constexpr uint32_t kChordDetectionMs = 120;
constexpr uint32_t kShortPressMs = 500;
constexpr uint32_t kResetRecordingSeconds = 3;
constexpr int32_t kSpeechRmsThreshold = 300;
constexpr uint32_t kVadIgnoreMs = 300;
constexpr uint32_t kSpeechConfirmationMs = 200;
constexpr uint8_t kPhysicalButtonCount = 2;
constexpr int kButtonPins[kPhysicalButtonCount] = {2, 9};  // D0, D9
constexpr uint8_t kButtonA1Mask = 0x01;
constexpr uint8_t kButtonA2Mask = 0x02;
constexpr uint8_t kButtonA3Mask = kButtonA1Mask | kButtonA2Mask;
constexpr size_t kMaxRecordingSamples =
    inmp441_audio::kSampleRate * kMaxRecordingSeconds;
constexpr size_t kPreRollSamples =
    inmp441_audio::kSampleRate * kShortPressMs / 1000;
constexpr size_t kSpeechConfirmationSamples =
    inmp441_audio::kSampleRate * kSpeechConfirmationMs / 1000;
constexpr size_t kVadIgnoreSamples =
    inmp441_audio::kSampleRate * kVadIgnoreMs / 1000;

static_assert(kVadIgnoreMs < kShortPressMs,
              "VAD must start before long-press recording begins");
static_assert(kSpeechConfirmationMs > 80,
              "Button transients must not confirm speech");

struct ButtonState {
  uint8_t rawMask;
  uint8_t stableMask;
  uint32_t rawChangedAt;
};

int16_t pcmBuffer[inmp441_audio::kMaxSamplesPerRead];
int16_t preRollBuffer[kPreRollSamples];
ButtonState buttonState = {};
bool initialized = false;
EventCallback eventCallback = nullptr;

void emitRegionEvent(uint8_t region, RegionEvent event,
                     const char* text = nullptr) {
  if (eventCallback != nullptr) eventCallback(region, event, text);
}

bool configurationIsReady() {
  return device_settings::receiver().host[0] != '\0';
}

bool connectWifi() {
  return wifi_provisioning::reconnect();
}

void showProvisioningStatus() {
  char prompt[80];
  snprintf(prompt, sizeof(prompt), "WiFi: %s\nPass: %s\n\xE8\xBF\x9E\xE6\x8E\xA5\xE5\x90\x8E\xE9\x85\x8D\xE7\xBD\xAE",
           wifi_provisioning::kSetupApSsid,
           wifi_provisioning::kSetupApPassword);
  emitRegionEvent(ui_layout::kFunctionSlot, RegionEvent::Status, prompt);
}

bool writeAll(WiFiClient& client, const uint8_t* data, size_t size) {
  size_t sent = 0;
  uint32_t lastProgress = millis();
  while (sent < size) {
    if (!client.connected()) return false;
    const size_t written = client.write(data + sent, size - sent);
    if (written > 0) {
      sent += written;
      lastProgress = millis();
    } else {
      if (millis() - lastProgress >= kWriteTimeoutMs) return false;
      delay(1);
    }
  }
  return true;
}

bool writeChunk(WiFiClient& client, const uint8_t* data, size_t size) {
  char header[16];
  const int headerLength =
      snprintf(header, sizeof(header), "%X\r\n", static_cast<unsigned>(size));
  if (headerLength <= 0 ||
      !writeAll(client, reinterpret_cast<const uint8_t*>(header),
                static_cast<size_t>(headerLength)) ||
      !writeAll(client, data, size)) {
    return false;
  }

  constexpr uint8_t kTerminator[] = {'\r', '\n'};
  return writeAll(client, kTerminator, sizeof(kTerminator));
}

bool finishChunkedBody(WiFiClient& client, bool discardAudio = false) {
  constexpr uint8_t kFinalChunk[] = {'0', '\r', '\n', '\r', '\n'};
  constexpr uint8_t kDiscardedFinalChunk[] = {
      '0', '\r', '\n', 'X', '-', 'D', 'i', 's', 'c', 'a', 'r', 'd', '-',
      'A', 'u', 'd', 'i', 'o', ':', ' ', '1', '\r', '\n', '\r', '\n'};
  return discardAudio
             ? writeAll(client, kDiscardedFinalChunk,
                        sizeof(kDiscardedFinalChunk))
             : writeAll(client, kFinalChunk, sizeof(kFinalChunk));
}

bool chunkContainsSpeech(const int16_t* samples, size_t count) {
  if (samples == nullptr || count == 0) return false;

  int64_t sum = 0;
  uint64_t squareSum = 0;
  for (size_t i = 0; i < count; ++i) {
    const int32_t sample = samples[i];
    sum += sample;
    squareSum += static_cast<uint64_t>(static_cast<int64_t>(sample) * sample);
  }

  const uint64_t centeredEnergy =
      squareSum * count - static_cast<uint64_t>(sum * sum);
  const uint64_t thresholdEnergy =
      static_cast<uint64_t>(kSpeechRmsThreshold) * kSpeechRmsThreshold * count *
      count;
  return centeredEnergy >= thresholdEnergy;
}

void updateSpeechDetection(const int16_t* samples, size_t count,
                           size_t capturedSamples,
                           size_t& voicedSamples, bool& speechDetected) {
  const bool chunkHasSpeech = chunkContainsSpeech(samples, count);
  if (speechDetected) return;
  if (capturedSamples <= kVadIgnoreSamples) {
    voicedSamples = 0;
    return;
  }
  if (chunkHasSpeech) {
    voicedSamples += count;
  } else {
    voicedSamples = voicedSamples > count ? voicedSamples - count : 0;
  }
  speechDetected = voicedSamples >= kSpeechConfirmationSamples;
}

bool readHttpResponse(WiFiClient& client, String& responseBody,
                      bool& speechDetected, bool& taskDetected) {
  client.setTimeout(60000);
  const String statusLine = client.readStringUntil('\n');
  Serial.print("Server response: ");
  Serial.println(statusLine);
  const bool success = statusLine.startsWith("HTTP/1.1 200") ||
                       statusLine.startsWith("HTTP/1.0 200");

  int contentLength = -1;
  while (client.connected() || client.available() > 0) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (header.isEmpty()) break;
    if (header.startsWith("Content-Length:")) {
      contentLength = header.substring(15).toInt();
    } else if (header.startsWith("X-Speech-Detected:")) {
      speechDetected = header.substring(18).toInt() == 1;
    } else if (header.startsWith("X-Task-Detected:")) {
      taskDetected = header.substring(16).toInt() == 1;
    }
  }

  if (contentLength > 0) responseBody.reserve(contentLength + 1);
  const uint32_t started = millis();
  while ((contentLength < 0 ||
          responseBody.length() < static_cast<size_t>(contentLength)) &&
         millis() - started < 60000) {
    while (client.available() > 0) {
      responseBody += static_cast<char>(client.read());
    }
    if (!client.connected() && client.available() == 0) break;
    delay(1);
  }
  responseBody.trim();
  return success;
}

uint8_t readButtonMask() {
  uint8_t mask = 0;
  if (digitalRead(kButtonPins[0]) == LOW) mask |= kButtonA1Mask;
  if (digitalRead(kButtonPins[1]) == LOW) mask |= kButtonA2Mask;
  return mask;
}

uint8_t regionForButtonMask(uint8_t mask) {
  if (mask == kButtonA1Mask) return 0;
  if (mask == kButtonA2Mask) return 1;
  return 2;
}

bool buttonWasReleased(uint8_t requiredMask, uint32_t& releaseStartedAt) {
  if ((readButtonMask() & requiredMask) == requiredMask) {
    releaseStartedAt = 0;
    return false;
  }
  if (releaseStartedAt == 0) releaseStartedAt = millis();
  return millis() - releaseStartedAt >= kButtonDebounceMs;
}

void handleButtonPress(uint8_t region, uint8_t requiredMask,
                       uint32_t pressedAt) {
  if (!inmp441_audio::restartCapture()) {
    emitRegionEvent(region, RegionEvent::Error);
    return;
  }

  size_t preRollCount = 0;
  size_t voicedSamples = 0;
  bool localSpeechDetected = false;
  uint32_t releaseStartedAt = 0;
  while (millis() - pressedAt < kShortPressMs) {
    if (buttonWasReleased(requiredMask, releaseStartedAt)) {
      Serial.printf("Region %u short press: toggle completion.\n",
                    static_cast<unsigned>(region + 1));
      emitRegionEvent(region, RegionEvent::ToggleCompletion);
      return;
    }

    const size_t remaining = kPreRollSamples - preRollCount;
    if (remaining == 0) break;
    const size_t requested =
        remaining < inmp441_audio::kMaxSamplesPerRead
            ? remaining
            : inmp441_audio::kMaxSamplesPerRead;
    const size_t samplesRead = inmp441_audio::readPcm16(
        preRollBuffer + preRollCount, requested, 1000);
    if (samplesRead == 0) {
      Serial.println("ERROR: Could not capture button pre-roll audio.");
      emitRegionEvent(region, RegionEvent::Error);
      return;
    }
    preRollCount += samplesRead;
    updateSpeechDetection(preRollBuffer + preRollCount - samplesRead,
                          samplesRead, preRollCount, voicedSamples,
                          localSpeechDetected);
  }

  if (!connectWifi()) {
    emitRegionEvent(region, RegionEvent::Error);
    return;
  }

  const device_settings::ReceiverSettings& receiver =
      device_settings::receiver();
  WiFiClient client;
  Serial.printf("Connecting to audio receiver at %s:%u...\n",
                receiver.host, receiver.port);
  if (!client.connect(receiver.host, receiver.port)) {
    Serial.println("ERROR: Cannot connect to the audio receiver.");
    emitRegionEvent(region, RegionEvent::Error);
    return;
  }

  client.printf("POST %s HTTP/1.1\r\n", receiver.path);
  client.printf("Host: %s:%u\r\n", receiver.host, receiver.port);
  client.println("Content-Type: application/octet-stream");
  client.println("Transfer-Encoding: chunked");
  client.printf("X-Sample-Rate: %u\r\n", inmp441_audio::kSampleRate);
  client.println("X-Channels: 1");
  client.println("X-Sample-Width: 2");
  client.println("Connection: close");
  client.println();

  if (!writeChunk(client, reinterpret_cast<const uint8_t*>(preRollBuffer),
                  preRollCount * sizeof(preRollBuffer[0]))) {
    Serial.println("ERROR: Could not send button pre-roll audio.");
    client.stop();
    emitRegionEvent(region, RegionEvent::Error);
    return;
  }
  Serial.printf(
      "Region %u recording started; release the button to submit (max 60 seconds).\n",
      static_cast<unsigned>(region + 1));
  size_t samplesSent = preRollCount;
  uint32_t nextProgressSecond = 1;
  releaseStartedAt = 0;
  while (samplesSent < kMaxRecordingSamples) {
    if (buttonWasReleased(requiredMask, releaseStartedAt)) break;

    const size_t remaining = kMaxRecordingSamples - samplesSent;
    const size_t requested =
        remaining < inmp441_audio::kMaxSamplesPerRead
            ? remaining
            : inmp441_audio::kMaxSamplesPerRead;
    const size_t samplesRead =
        inmp441_audio::readPcm16(pcmBuffer, requested, 1000);
    if (samplesRead == 0) {
      Serial.println("ERROR: Audio stream interrupted.");
      client.stop();
      emitRegionEvent(region, RegionEvent::Error);
      return;
    }
    updateSpeechDetection(pcmBuffer, samplesRead, samplesSent + samplesRead,
                          voicedSamples, localSpeechDetected);
    if (!writeChunk(client, reinterpret_cast<const uint8_t*>(pcmBuffer),
                    samplesRead * sizeof(pcmBuffer[0]))) {
      Serial.println("ERROR: Audio stream interrupted.");
      client.stop();
      emitRegionEvent(region, RegionEvent::Error);
      return;
    }

    samplesSent += samplesRead;
    const uint32_t elapsedSeconds = samplesSent / inmp441_audio::kSampleRate;
    if (elapsedSeconds >= nextProgressSecond) {
      Serial.printf("Region %u recorded %lu seconds\n",
                    static_cast<unsigned>(region + 1),
                    static_cast<unsigned long>(elapsedSeconds));
      nextProgressSecond = elapsedSeconds + 1;
    }
    if (!localSpeechDetected &&
        samplesSent >=
            inmp441_audio::kSampleRate * kResetRecordingSeconds) {
      Serial.printf(
          "Region %u reset after three seconds without local speech activity.\n",
          static_cast<unsigned>(region + 1));
      if (!finishChunkedBody(client, true)) {
        Serial.println("ERROR: Could not cancel the silent audio stream.");
      }
      client.stop();
      emitRegionEvent(region, RegionEvent::Reset);
      return;
    }
  }

  if (samplesSent >= kMaxRecordingSamples) {
    Serial.println("Maximum recording duration reached (60 seconds).");
  }

  if (!finishChunkedBody(client)) {
    Serial.println("ERROR: Could not finish the audio stream.");
    client.stop();
    emitRegionEvent(region, RegionEvent::Error);
    return;
  }

  Serial.printf("Region %u recording complete; waiting for recognition...\n",
                static_cast<unsigned>(region + 1));
  String recognizedText;
  // A missing header from an older receiver must never clear display content.
  bool speechDetected = true;
  bool taskDetected = true;
  const bool success =
      readHttpResponse(client, recognizedText, speechDetected, taskDetected);
  client.stop();
  if (success) {
    Serial.println("Recognition result:");
    Serial.println(recognizedText);
    const bool resetRegion =
        samplesSent >= inmp441_audio::kSampleRate * kResetRecordingSeconds &&
        !speechDetected;
    if (resetRegion) {
      Serial.printf("Region %u reset after silent recording.\n",
                    static_cast<unsigned>(region + 1));
      emitRegionEvent(region, RegionEvent::Reset);
    } else if (!speechDetected) {
      emitRegionEvent(region, RegionEvent::NoSpeech);
    } else if (!taskDetected) {
      emitRegionEvent(region, RegionEvent::NotTask);
    } else {
      const task_processing::TaskRecord task =
          task_processing::fromRecognition(recognizedText);
      const String displayText = task_processing::textForDisplay(task);
      emitRegionEvent(region, RegionEvent::Recognition,
                      displayText.c_str());
    }
    Serial.println("Press and hold a region button to record again.");
  } else {
    Serial.println("ERROR: The receiver rejected the recording.");
    if (!recognizedText.isEmpty()) Serial.println(recognizedText);
    emitRegionEvent(region, RegionEvent::Error);
  }
}

}  // namespace

void setEventCallback(EventCallback callback) { eventCallback = callback; }

bool begin() {
  for (uint8_t button = 0; button < kPhysicalButtonCount; ++button) {
    pinMode(kButtonPins[button], INPUT_PULLUP);
  }

  if (!wifi_provisioning::begin(false, showProvisioningStatus)) {
    return false;
  }
  if (!configurationIsReady()) {
    Serial.println(
        "ERROR: Configure the recognition receiver in management mode.");
    return false;
  }
  if (!inmp441_audio::begin()) return false;

  const uint8_t pressedMask = readButtonMask();
  buttonState = {pressedMask, pressedMask, millis()};

  initialized = true;
  Serial.println(
      "Voice recorder ready: D0 controls A1, D9 controls A2, and D0+D9 controls A3.");
  return true;
}

uint8_t detectChord(uint8_t initialMask) {
  if (initialMask == kButtonA3Mask) return initialMask;

  const uint32_t startedAt = millis();
  uint32_t bothPressedAt = 0;
  while (millis() - startedAt < kChordDetectionMs) {
    const uint8_t currentMask = readButtonMask();
    if ((currentMask & initialMask) == 0) return initialMask;
    if (currentMask == kButtonA3Mask) {
      if (bothPressedAt == 0) bothPressedAt = millis();
      if (millis() - bothPressedAt >= kButtonDebounceMs) {
        return kButtonA3Mask;
      }
    } else {
      bothPressedAt = 0;
    }
    delay(5);
  }
  return initialMask;
}

void poll() {
  wifi_provisioning::poll();
  if (!initialized) {
    delay(1000);
    return;
  }

  const uint8_t pressedMask = readButtonMask();
  if (pressedMask != buttonState.rawMask) {
    buttonState.rawMask = pressedMask;
    buttonState.rawChangedAt = millis();
  }

  if (buttonState.stableMask != buttonState.rawMask &&
      millis() - buttonState.rawChangedAt >= kButtonDebounceMs) {
    buttonState.stableMask = buttonState.rawMask;
    if (buttonState.stableMask != 0) {
      const uint32_t pressedAt = buttonState.rawChangedAt;
      const uint8_t actionMask = detectChord(buttonState.stableMask);
      const uint8_t region = regionForButtonMask(actionMask);
      handleButtonPress(region, actionMask, pressedAt);
      const uint8_t currentMask = readButtonMask();
      buttonState = {currentMask, currentMask, millis()};
    }
  }
  delay(5);
}

}  // namespace voice_upload
