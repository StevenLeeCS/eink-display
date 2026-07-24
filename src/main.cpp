#include <Arduino.h>
#include <SPI.h>

#include "image_data.h"

namespace {

constexpr uint16_t kDisplayWidth = 400;
constexpr uint16_t kDisplayHeight = 300;
constexpr size_t kFrameBytes = kDisplayWidth * kDisplayHeight / 8;

// XIAO ESP32-C3 pin labels and GPIO numbers.
constexpr int kPinMosi = 10;  // D10
constexpr int kPinSck = 8;    // D8
constexpr int kPinCs = 3;     // D1
constexpr int kPinDc = 4;     // D2
constexpr int kPinReset = 5;  // D3
constexpr int kPinBusy = 6;   // D4, HIGH while SSD1683 is busy

constexpr uint32_t kSpiFrequency = 4000000;
constexpr uint32_t kBusyTimeoutMs = 30000;

static_assert(kImageDataSize == kFrameBytes,
              "Image data must match the 400x300 display");

class Epd42 {
 public:
  bool begin() {
    pinMode(kPinCs, OUTPUT);
    pinMode(kPinDc, OUTPUT);
    pinMode(kPinReset, OUTPUT);
    pinMode(kPinBusy, INPUT_PULLUP);
    digitalWrite(kPinCs, HIGH);
    digitalWrite(kPinDc, HIGH);
    digitalWrite(kPinReset, HIGH);

    SPI.begin(kPinSck, -1, kPinMosi, kPinCs);
    reset();
    if (!waitUntilIdle("hardware reset")) return false;

    command(0x12);  // SW_RESET
    if (!waitUntilIdle("software reset")) return false;

    const uint8_t updateControl[] = {0x40, 0x00};
    command(0x21);                 // Display update control.
    data(updateControl, sizeof(updateControl));
    commandWithData(0x3C, 0x05);  // Border waveform.
    commandWithData(0x11, 0x03);  // Increment X, then Y.
    setAddressWindow(0, 0, kDisplayWidth - 1, kDisplayHeight - 1);
    setCursor(0, 0);
    return waitUntilIdle("initialization");
  }

  bool display(const uint8_t* image) {
    setCursor(0, 0);
    command(0x24);  // Black/white RAM.
    data(image, kFrameBytes);

    commandWithData(0x22, 0xF7);  // Load OTP LUT and perform full update.
    command(0x20);                // MASTER_ACTIVATION
    return waitUntilIdle("display update");
  }

  void sleep() {
    commandWithData(0x10, 0x01);  // Deep sleep; reset is required to wake.
    delay(200);
  }

 private:
  SPISettings settings_{kSpiFrequency, MSBFIRST, SPI_MODE0};

  void reset() {
    digitalWrite(kPinReset, HIGH);
    delay(100);
    digitalWrite(kPinReset, LOW);
    delay(10);
    digitalWrite(kPinReset, HIGH);
    delay(100);
  }

  bool waitUntilIdle(const char* operation) {
    const uint32_t started = millis();
    while (digitalRead(kPinBusy) == HIGH) {
      if (millis() - started >= kBusyTimeoutMs) {
        Serial.printf("ERROR: BUSY timeout during %s. Check VCC, GND, BUSY and RST.\n",
                      operation);
        return false;
      }
      delay(10);
    }
    return true;
  }

  void command(uint8_t value) {
    SPI.beginTransaction(settings_);
    digitalWrite(kPinDc, LOW);
    digitalWrite(kPinCs, LOW);
    SPI.transfer(value);
    digitalWrite(kPinCs, HIGH);
    digitalWrite(kPinDc, HIGH);
    SPI.endTransaction();
  }

  void commandWithData(uint8_t cmd, uint8_t value) {
    command(cmd);
    data(&value, 1);
  }

  void data(const uint8_t* bytes, size_t count) {
    SPI.beginTransaction(settings_);
    digitalWrite(kPinDc, HIGH);
    digitalWrite(kPinCs, LOW);
    SPI.writeBytes(bytes, count);
    digitalWrite(kPinCs, HIGH);
    SPI.endTransaction();
  }

  void setAddressWindow(uint16_t xStart, uint16_t yStart, uint16_t xEnd,
                        uint16_t yEnd) {
    const uint8_t xData[] = {static_cast<uint8_t>(xStart >> 3),
                             static_cast<uint8_t>(xEnd >> 3)};
    command(0x44);
    data(xData, sizeof(xData));

    const uint8_t yData[] = {
        static_cast<uint8_t>(yStart), static_cast<uint8_t>(yStart >> 8),
        static_cast<uint8_t>(yEnd), static_cast<uint8_t>(yEnd >> 8)};
    command(0x45);
    data(yData, sizeof(yData));
  }

  void setCursor(uint16_t x, uint16_t y) {
    commandWithData(0x4E, static_cast<uint8_t>(x >> 3));
    const uint8_t yData[] = {static_cast<uint8_t>(y),
                             static_cast<uint8_t>(y >> 8)};
    command(0x4F);
    data(yData, sizeof(yData));
  }
};

Epd42 display;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ZJY420S08W0G01 / SSD1683 startup");

  if (!display.begin()) return;
  Serial.println("Displaying imported image...");
  if (!display.display(kImageData)) return;
  display.sleep();
  Serial.println("Done. Display is asleep.");
}

void loop() { delay(1000); }
