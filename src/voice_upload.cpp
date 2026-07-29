#include "voice_upload.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <cstring>

#include "inmp441_audio.h"
#include "network_config.h"
#include "task_processing.h"
#include "wifi_provisioning.h"

namespace voice_upload {
namespace {

constexpr uint32_t kMaxRecordingSeconds = 60;
constexpr uint32_t kWriteTimeoutMs = 5000;
constexpr uint32_t kButtonDebounceMs = 40;
constexpr uint32_t kShortPressMs = 500;
constexpr uint32_t kProvisioningGestureMs = 2000;
constexpr uint32_t kResetRecordingSeconds = 3;
constexpr int32_t kSpeechRmsThreshold = 300;
constexpr uint32_t kVadIgnoreMs = 300;
constexpr uint32_t kSpeechConfirmationMs = 200;
constexpr uint8_t kRegionCount = 2;
constexpr int kButtonPins[kRegionCount] = {2, 9};  // D0, D9
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
  bool rawPressed;
  bool stablePressed;
  uint32_t rawChangedAt;
};

int16_t pcmBuffer[inmp441_audio::kMaxSamplesPerRead];
int16_t preRollBuffer[kPreRollSamples];
ButtonState buttonStates[kRegionCount] = {};
uint32_t bothButtonsPressedAt = 0;
bool initialized = false;
EventCallback eventCallback = nullptr;

void emitRegionEvent(uint8_t region, RegionEvent event,
                     const char* text = nullptr) {
  if (eventCallback != nullptr) eventCallback(region, event, text);
}

bool configurationIsReady() {
  return voice_config::kServerHost[0] != '\0';
}

bool connectWifi() {
  return wifi_provisioning::reconnect();
}

void showProvisioningStatus() {
  emitRegionEvent(0, RegionEvent::Status, wifi_provisioning::kSetupApSsid);
  char passwordPrompt[32];
  snprintf(passwordPrompt, sizeof(passwordPrompt), "Pass: %s",
           wifi_provisioning::kSetupApPassword);
  emitRegionEvent(1, RegionEvent::Status, passwordPrompt);
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

bool buttonWasReleased(int buttonPin, uint32_t& releaseStartedAt) {
  if (digitalRead(buttonPin) == LOW) {
    releaseStartedAt = 0;
    return false;
  }
  if (releaseStartedAt == 0) releaseStartedAt = millis();
  return millis() - releaseStartedAt >= kButtonDebounceMs;
}

void handleButtonPress(uint8_t region, int buttonPin) {
  if (!inmp441_audio::restartCapture()) {
    emitRegionEvent(region, RegionEvent::Error);
    return;
  }

  size_t preRollCount = 0;
  size_t voicedSamples = 0;
  bool localSpeechDetected = false;
  uint32_t releaseStartedAt = 0;
  const uint32_t pressedAt = millis();
  while (millis() - pressedAt < kShortPressMs) {
    if (digitalRead(kButtonPins[1U - region]) == LOW) {
      bothButtonsPressedAt = millis();
      Serial.println("A1+A2 chord detected; keep holding to open Wi-Fi setup.");
      return;
    }
    if (buttonWasReleased(buttonPin, releaseStartedAt)) {
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

  WiFiClient client;
  Serial.printf("Connecting to audio receiver at %s:%u...\n",
                voice_config::kServerHost, voice_config::kServerPort);
  if (!client.connect(voice_config::kServerHost, voice_config::kServerPort)) {
    Serial.println("ERROR: Cannot connect to the audio receiver.");
    emitRegionEvent(region, RegionEvent::Error);
    return;
  }

  client.printf("POST %s HTTP/1.1\r\n", voice_config::kServerPath);
  client.printf("Host: %s:%u\r\n", voice_config::kServerHost,
                voice_config::kServerPort);
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
    if (buttonWasReleased(buttonPin, releaseStartedAt)) break;

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
  for (uint8_t region = 0; region < kRegionCount; ++region) {
    pinMode(kButtonPins[region], INPUT_PULLUP);
  }

  if (!wifi_provisioning::begin(false, showProvisioningStatus)) {
    return false;
  }
  if (!configurationIsReady()) {
    Serial.println(
        "ERROR: Configure the receiver in include/network_config.h before uploading.");
    return false;
  }
  if (!inmp441_audio::begin()) return false;

  for (uint8_t region = 0; region < kRegionCount; ++region) {
    const bool pressed = digitalRead(kButtonPins[region]) == LOW;
    buttonStates[region] = {pressed, pressed, millis()};
  }

  initialized = true;
  Serial.println("Voice recorder ready: hold D0 for the top region or D9 for the bottom region.");
  return true;
}

void poll() {
  if (!initialized) {
    delay(1000);
    return;
  }

  const bool bothButtonsPressed =
      digitalRead(kButtonPins[0]) == LOW &&
      digitalRead(kButtonPins[1]) == LOW;
  if (bothButtonsPressed) {
    if (bothButtonsPressedAt == 0) bothButtonsPressedAt = millis();
    if (millis() - bothButtonsPressedAt >= kProvisioningGestureMs) {
      Serial.println("A1+A2 held for two seconds; opening Wi-Fi setup.");
      wifi_provisioning::begin(true, showProvisioningStatus);
    }
    delay(5);
    return;
  }
  if (bothButtonsPressedAt != 0) {
    bothButtonsPressedAt = 0;
    for (uint8_t region = 0; region < kRegionCount; ++region) {
      const bool pressed = digitalRead(kButtonPins[region]) == LOW;
      buttonStates[region] = {pressed, pressed, millis()};
    }
  }

  for (uint8_t region = 0; region < kRegionCount; ++region) {
    ButtonState& state = buttonStates[region];
    const bool pressed = digitalRead(kButtonPins[region]) == LOW;
    if (pressed != state.rawPressed) {
      state.rawPressed = pressed;
      state.rawChangedAt = millis();
    }

    if (state.stablePressed != state.rawPressed &&
        millis() - state.rawChangedAt >= kButtonDebounceMs) {
      state.stablePressed = state.rawPressed;
      if (state.stablePressed) {
        handleButtonPress(region, kButtonPins[region]);
        state.rawPressed = digitalRead(kButtonPins[region]) == LOW;
        state.stablePressed = state.rawPressed;
        state.rawChangedAt = millis();
      }
    }
  }
  delay(5);
}

}  // namespace voice_upload
