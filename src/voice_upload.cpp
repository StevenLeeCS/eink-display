#include "voice_upload.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <cstdlib>

#include "board_pins.h"
#include "device_settings.h"
#include "inmp441_audio.h"
#include "pomodoro.h"
#include "task_processing.h"
#include "task_store.h"
#include "ui_layout.h"
#include "wifi_provisioning.h"

namespace voice_upload {
namespace {

constexpr uint32_t kMaxRecordingSeconds = 60;
constexpr uint32_t kWriteTimeoutMs = 5000;
constexpr uint32_t kButtonDebounceMs = 40;
constexpr uint32_t kShortPressMs = 500;
constexpr uint32_t kDoublePressMs = 320;
constexpr uint32_t kResetRecordingSeconds = 3;
constexpr int32_t kSpeechRmsThreshold = 300;
constexpr uint32_t kVadIgnoreMs = 300;
constexpr uint32_t kSpeechConfirmationMs = 200;
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
uint32_t pendingPomodoroPressAt = 0;

void emitRegionEvent(uint8_t region, RegionEvent event,
                     const char* text = nullptr,
                     const task_store::TaskSchedule* schedule = nullptr) {
  if (eventCallback != nullptr) eventCallback(region, event, text, schedule);
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

void appendRollingPreRoll(const int16_t* samples, size_t count,
                          size_t& storedSamples) {
  if (samples == nullptr || count == 0 || count > kPreRollSamples) return;
  const size_t retained =
      storedSamples + count > kPreRollSamples ? kPreRollSamples - count
                                               : storedSamples;
  if (retained > 0 && retained < storedSamples) {
    memmove(preRollBuffer, preRollBuffer + storedSamples - retained,
            retained * sizeof(preRollBuffer[0]));
  }
  memcpy(preRollBuffer + retained, samples,
         count * sizeof(preRollBuffer[0]));
  storedSamples = retained + count;
}

bool readHttpResponse(WiFiClient& client, String& responseBody,
                      bool& speechDetected, bool& taskDetected,
                      task_store::TaskSchedule& schedule,
                      int* focusMinutes = nullptr,
                      int* breakMinutes = nullptr) {
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
    } else if (header.startsWith("X-Task-Time-Kind:")) {
      String kind = header.substring(17);
      kind.trim();
      schedule.kind =
          kind == "exact" ? task_store::ScheduleKind::Exact
          : kind == "window" ? task_store::ScheduleKind::Window
                             : task_store::ScheduleKind::None;
    } else if (header.startsWith("X-Task-Start-At:")) {
      schedule.startAt = strtoull(header.substring(16).c_str(), nullptr, 10);
    } else if (header.startsWith("X-Task-End-At:")) {
      schedule.endAt = strtoull(header.substring(14).c_str(), nullptr, 10);
    } else if (focusMinutes != nullptr &&
               header.startsWith("X-Pomodoro-Focus-Minutes:")) {
      *focusMinutes = header.substring(25).toInt();
    } else if (breakMinutes != nullptr &&
               header.startsWith("X-Pomodoro-Break-Minutes:")) {
      *breakMinutes = header.substring(25).toInt();
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
  const bool exactValid = schedule.kind == task_store::ScheduleKind::Exact &&
                          schedule.startAt != 0 &&
                          schedule.startAt == schedule.endAt;
  const bool windowValid = schedule.kind == task_store::ScheduleKind::Window &&
                           schedule.startAt != 0 &&
                           schedule.startAt < schedule.endAt;
  if (!exactValid && !windowValid) schedule = {};
  return success;
}

uint8_t readButtonMask() {
  uint8_t mask = 0;
  for (size_t button = 0; button < board_pins::kButtonCount; ++button) {
    if (digitalRead(board_pins::kButtons[button].pin) == LOW) {
      mask |= static_cast<uint8_t>(1U << button);
    }
  }
  return mask;
}

int8_t singleButtonIndex(uint8_t mask) {
  if (mask == 0 || (mask & static_cast<uint8_t>(mask - 1U)) != 0) return -1;
  for (size_t button = 0; button < board_pins::kButtonCount; ++button) {
    if (mask == static_cast<uint8_t>(1U << button)) {
      return static_cast<int8_t>(button);
    }
  }
  return -1;
}

void registerPomodoroShortPress() {
  const uint32_t now = millis();
  if (pendingPomodoroPressAt != 0 &&
      now - pendingPomodoroPressAt <= kDoublePressMs) {
    pendingPomodoroPressAt = 0;
    if (!pomodoro::switchPhase()) {
      Serial.println("Pomodoro double press ignored outside pomodoro mode.");
    }
    return;
  }
  pendingPomodoroPressAt = now;
}

bool buttonWasReleased(uint8_t requiredMask, uint32_t& releaseStartedAt) {
  if ((readButtonMask() & requiredMask) == requiredMask) {
    releaseStartedAt = 0;
    return false;
  }
  if (releaseStartedAt == 0) releaseStartedAt = millis();
  return millis() - releaseStartedAt >= kButtonDebounceMs;
}

void reportInputError(uint8_t region, bool pomodoroButton) {
  if (pomodoroButton) {
    pomodoro::showSettingsError();
  } else {
    emitRegionEvent(region, RegionEvent::Error);
  }
}

void handleButtonPress(uint8_t region, uint8_t requiredMask,
                       uint32_t pressedAt, bool pomodoroButton = false) {
  if (!inmp441_audio::restartCapture()) {
    reportInputError(region, pomodoroButton);
    return;
  }

  size_t preRollCount = 0;
  size_t voicedSamples = 0;
  bool localSpeechDetected = false;
  uint32_t releaseStartedAt = 0;
  while (millis() - pressedAt < kShortPressMs) {
    if (buttonWasReleased(requiredMask, releaseStartedAt)) {
      if (pomodoroButton) {
        registerPomodoroShortPress();
      } else {
        Serial.printf("Region %u short press: toggle completion.\n",
                      static_cast<unsigned>(region + 1));
        emitRegionEvent(region, RegionEvent::ToggleCompletion);
      }
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
      reportInputError(region, pomodoroButton);
      return;
    }
    preRollCount += samplesRead;
    updateSpeechDetection(preRollBuffer + preRollCount - samplesRead,
                          samplesRead, preRollCount, voicedSamples,
                          localSpeechDetected);
  }
  if (pomodoroButton) pendingPomodoroPressAt = 0;

  // A silent three-second hold is an offline mode toggle. Only connect after
  // local VAD confirms that this is a spoken settings command.
  if (pomodoroButton && !localSpeechDetected) {
    size_t capturedSamples = preRollCount;
    while (capturedSamples <
           inmp441_audio::kSampleRate * kResetRecordingSeconds) {
      if (buttonWasReleased(requiredMask, releaseStartedAt)) {
        Serial.println("Pomodoro hold released before speech or 3 seconds.");
        return;
      }
      const size_t samplesRead = inmp441_audio::readPcm16(
          pcmBuffer, inmp441_audio::kMaxSamplesPerRead, 1000);
      if (samplesRead == 0) return;
      capturedSamples += samplesRead;
      updateSpeechDetection(pcmBuffer, samplesRead, capturedSamples,
                            voicedSamples, localSpeechDetected);
      appendRollingPreRoll(pcmBuffer, samplesRead, preRollCount);
      if (localSpeechDetected) break;
    }
    if (!localSpeechDetected) {
      pendingPomodoroPressAt = 0;
      const bool changed = pomodoro::state().active ? pomodoro::exit()
                                                    : pomodoro::enter();
      Serial.println(changed ? "Pomodoro mode toggled by silent hold."
                             : "ERROR: Pomodoro mode toggle failed.");
      while ((readButtonMask() & requiredMask) == requiredMask) delay(5);
      return;
    }
  }

  if (!connectWifi()) {
    reportInputError(region, pomodoroButton);
    return;
  }

  const device_settings::ReceiverSettings& receiver =
      device_settings::receiver();
  WiFiClient client;
  Serial.printf("Connecting to audio receiver at %s:%u...\n",
                receiver.host, receiver.port);
  if (!client.connect(receiver.host, receiver.port)) {
    Serial.println("ERROR: Cannot connect to the audio receiver.");
    reportInputError(region, pomodoroButton);
    return;
  }

  client.printf("POST %s HTTP/1.1\r\n", receiver.path);
  client.printf("Host: %s:%u\r\n", receiver.host, receiver.port);
  client.println("Content-Type: application/octet-stream");
  client.println("Transfer-Encoding: chunked");
  client.printf("X-Sample-Rate: %u\r\n", inmp441_audio::kSampleRate);
  client.println("X-Channels: 1");
  client.println("X-Sample-Width: 2");
  if (pomodoroButton) client.println("X-Recognition-Mode: pomodoro");
  client.println("Connection: close");
  client.println();

  if (!writeChunk(client, reinterpret_cast<const uint8_t*>(preRollBuffer),
                  preRollCount * sizeof(preRollBuffer[0]))) {
    Serial.println("ERROR: Could not send button pre-roll audio.");
    client.stop();
    reportInputError(region, pomodoroButton);
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
      reportInputError(region, pomodoroButton);
      return;
    }
    updateSpeechDetection(pcmBuffer, samplesRead, samplesSent + samplesRead,
                          voicedSamples, localSpeechDetected);
    if (!writeChunk(client, reinterpret_cast<const uint8_t*>(pcmBuffer),
                    samplesRead * sizeof(pcmBuffer[0]))) {
      Serial.println("ERROR: Audio stream interrupted.");
      client.stop();
      reportInputError(region, pomodoroButton);
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
    if (!pomodoroButton && !localSpeechDetected &&
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
    reportInputError(region, pomodoroButton);
    return;
  }

  Serial.printf("Region %u recording complete; waiting for recognition...\n",
                static_cast<unsigned>(region + 1));
  String recognizedText;
  // A missing header from an older receiver must never clear display content.
  bool speechDetected = true;
  bool taskDetected = true;
  task_store::TaskSchedule schedule = {};
  int focusMinutes = -1;
  int breakMinutes = -1;
  const bool success =
      readHttpResponse(client, recognizedText, speechDetected, taskDetected,
                       schedule, pomodoroButton ? &focusMinutes : nullptr,
                       pomodoroButton ? &breakMinutes : nullptr);
  client.stop();
  if (success) {
    Serial.println("Recognition result:");
    Serial.println(recognizedText);
    if (pomodoroButton) {
      if (speechDetected &&
          pomodoro::applyVoiceMinutes(focusMinutes, breakMinutes)) {
        Serial.printf("Pomodoro settings: focus=%d, break=%d.\n",
                      focusMinutes, breakMinutes);
      } else {
        Serial.println("Pomodoro voice settings were not recognized.");
        pomodoro::showSettingsError();
      }
      return;
    }
    const bool resetRegion =
        samplesSent >= inmp441_audio::kSampleRate * kResetRecordingSeconds &&
        !speechDetected;
    if (resetRegion) {
      Serial.printf("Region %u reset after silent recording.\n",
                    static_cast<unsigned>(region + 1));
      emitRegionEvent(region, RegionEvent::Reset);
    } else if (!speechDetected) {
      emitRegionEvent(region, RegionEvent::NoSpeech);
    } else {
      const task_processing::TaskRecord task =
          task_processing::fromRecognition(recognizedText);
      const String displayText = task_processing::textForDisplay(task);
      const RegionEvent resultEvent =
          taskDetected ? RegionEvent::Recognition
                       : RegionEvent::RecognitionNoHistory;
      emitRegionEvent(region, resultEvent,
                      displayText.c_str(), taskDetected ? &schedule : nullptr);
    }
    Serial.println("Press and hold a region button to record again.");
  } else {
    Serial.println("ERROR: The receiver rejected the recording.");
    if (!recognizedText.isEmpty()) Serial.println(recognizedText);
    reportInputError(region, pomodoroButton);
  }
}

}  // namespace

void setEventCallback(EventCallback callback) { eventCallback = callback; }

bool begin() {
  for (size_t button = 0; button < board_pins::kButtonCount; ++button) {
    pinMode(board_pins::kButtons[button].pin, INPUT_PULLUP);
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
  Serial.println("Voice controls ready:");
  for (size_t button = 0; button < board_pins::kButtonCount; ++button) {
    const board_pins::ButtonBinding& binding = board_pins::kButtons[button];
    if (binding.functionArea) {
      Serial.printf("  %s (GPIO%d) -> function area\n", binding.label,
                    binding.pin);
    } else {
      Serial.printf("  %s (GPIO%d) -> task A%u\n", binding.label,
                    binding.pin,
                    static_cast<unsigned>(binding.taskRegion + 1));
    }
  }
  return true;
}

void poll() {
  wifi_provisioning::poll();
  if (pendingPomodoroPressAt != 0 &&
      millis() - pendingPomodoroPressAt > kDoublePressMs) {
    pendingPomodoroPressAt = 0;
    if (!pomodoro::toggleRunning()) {
      Serial.println("Pomodoro short press ignored outside pomodoro mode.");
    }
  }
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
      const uint8_t actionMask = buttonState.stableMask;
      const int8_t buttonIndex = singleButtonIndex(actionMask);
      if (buttonIndex >= 0) {
        const board_pins::ButtonBinding& binding =
            board_pins::kButtons[buttonIndex];
        if (binding.functionArea) {
          handleButtonPress(ui_layout::kFunctionSlot, actionMask, pressedAt,
                            true);
        } else {
          pendingPomodoroPressAt = 0;
          handleButtonPress(binding.taskRegion, actionMask, pressedAt);
        }
      } else {
        pendingPomodoroPressAt = 0;
        Serial.println("Multiple button press ignored.");
        while (readButtonMask() != 0) delay(5);
      }
      const uint8_t currentMask = readButtonMask();
      buttonState = {currentMask, currentMask, millis()};
    }
  }
  delay(5);
}

}  // namespace voice_upload
