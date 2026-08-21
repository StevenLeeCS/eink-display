#include "inmp441_audio.h"

#include <Arduino.h>
#include <driver/i2s.h>

#include "board_pins.h"

namespace inmp441_audio {
namespace {

constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr int kPinBclk = board_pins::kMicrophoneBclk;
constexpr int kPinLrclk = board_pins::kMicrophoneLrclk;
constexpr int kPinData = board_pins::kMicrophoneData;

int32_t rawSamples[kMaxSamplesPerRead];
bool initialized = false;

int16_t toPcm16(int32_t rawSample) {
  // INMP441 sends signed 24-bit data left-aligned in the 32-bit I2S slot.
  return static_cast<int16_t>(rawSample >> 16);
}

}  // namespace

bool begin() {
  if (initialized) return true;

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 16;
  config.dma_buf_len = kMaxSamplesPerRead;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = kPinBclk;
  pins.ws_io_num = kPinLrclk;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = kPinData;

  esp_err_t result = i2s_driver_install(kI2sPort, &config, 0, nullptr);
  if (result != ESP_OK) {
    Serial.printf("ERROR: i2s_driver_install failed: %s\n",
                  esp_err_to_name(result));
    return false;
  }

  result = i2s_set_pin(kI2sPort, &pins);
  if (result != ESP_OK) {
    Serial.printf("ERROR: i2s_set_pin failed: %s\n", esp_err_to_name(result));
    i2s_driver_uninstall(kI2sPort);
    return false;
  }

  i2s_zero_dma_buffer(kI2sPort);
  initialized = true;
  Serial.println("INMP441 ready: 16000 Hz, mono, PCM16");
  Serial.printf("SCK=GPIO%d, WS=GPIO%d, SD=GPIO%d, L/R=GND\n",
                kPinBclk, kPinLrclk, kPinData);
  return true;
}

bool restartCapture() {
  if (!initialized) return false;

  esp_err_t result = i2s_stop(kI2sPort);
  if (result == ESP_OK) result = i2s_zero_dma_buffer(kI2sPort);
  if (result == ESP_OK) result = i2s_start(kI2sPort);
  if (result != ESP_OK) {
    Serial.printf("ERROR: restarting I2S failed: %s\n", esp_err_to_name(result));
    return false;
  }
  return true;
}

size_t readPcm16(int16_t* output, size_t maxSamples, uint32_t timeoutMs) {
  if (!initialized || output == nullptr || maxSamples == 0) return 0;
  if (maxSamples > kMaxSamplesPerRead) maxSamples = kMaxSamplesPerRead;

  size_t bytesRead = 0;
  const esp_err_t result =
      i2s_read(kI2sPort, rawSamples, maxSamples * sizeof(rawSamples[0]),
               &bytesRead, pdMS_TO_TICKS(timeoutMs));
  if (result != ESP_OK) {
    Serial.printf("ERROR: i2s_read failed: %s\n", esp_err_to_name(result));
    return 0;
  }

  const size_t samplesRead = bytesRead / sizeof(rawSamples[0]);
  for (size_t i = 0; i < samplesRead; ++i) output[i] = toPcm16(rawSamples[i]);
  return samplesRead;
}

}  // namespace inmp441_audio
