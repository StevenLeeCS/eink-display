#include <Arduino.h>
#include <SPI.h>
#include <cstring>

#include "ascii_font_16.h"
#include "chinese_font_16.h"
#include "task_store.h"
#include "voice_upload.h"

namespace {

constexpr uint16_t kPanelWidth = 400;
constexpr uint16_t kPanelHeight = 300;
constexpr uint16_t kDisplayWidth = kPanelHeight;
constexpr uint16_t kDisplayHeight = kPanelWidth;
constexpr size_t kPanelStride = kPanelWidth / 8;
constexpr size_t kFrameBytes = kPanelWidth * kPanelHeight / 8;

static_assert(kDisplayWidth * kDisplayHeight == kPanelWidth * kPanelHeight,
              "Portrait and panel frame areas must match");

// XIAO ESP32-C3 pin labels and GPIO numbers.
constexpr int kPinMosi = 10;  // D10
constexpr int kPinSck = 8;    // D8
constexpr int kPinCs = 3;     // D1
constexpr int kPinDc = 4;     // D2
constexpr int kPinReset = 5;  // D3
constexpr int kPinBusy = 6;   // D4, HIGH while SSD1683 is busy

constexpr uint32_t kSpiFrequency = 4000000;
constexpr uint32_t kBusyTimeoutMs = 30000;
constexpr uint8_t kVoiceRegionCount = 2;
constexpr uint16_t kVoiceRegionHeight = kDisplayHeight / kVoiceRegionCount;
constexpr uint16_t kVoiceDividerY = kVoiceRegionHeight - 1;
constexpr uint16_t kVoiceMarkerX = 12;
constexpr uint16_t kVoiceMarkerSize = 12;
constexpr uint16_t kVoiceMarkerGap = 8;
constexpr uint16_t kVoiceTextX =
    kVoiceMarkerX + kVoiceMarkerSize + kVoiceMarkerGap;
constexpr uint16_t kVoiceTextTopPadding = 10;
constexpr uint16_t kVoiceTextWidth = kDisplayWidth - kVoiceTextX - 16;
constexpr uint16_t kVoiceTextHeight =
    kVoiceRegionHeight - 2 * kVoiceTextTopPadding;
constexpr uint16_t kVoiceLineHeight = 20;
constexpr uint8_t kPartialRefreshesBeforeFull = 4;

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
    setAddressWindow(0, 0, kPanelWidth - 1, kPanelHeight - 1);
    setCursor(0, 0);
    return waitUntilIdle("initialization");
  }

  bool display(const uint8_t* image) {
    if (image == nullptr) return false;

    const uint8_t updateControl[] = {0x40, 0x00};
    command(0x21);
    data(updateControl, sizeof(updateControl));
    commandWithData(0x3C, 0x05);

    writeFrame(0x24, image);  // Black/white RAM.
    writeFrame(0x26, image);  // Baseline for the next black/white update.

    commandWithData(0x22, 0xF7);  // Load OTP LUT and perform full update.
    command(0x20);                // MASTER_ACTIVATION
    return waitUntilIdle("display update");
  }

  bool displayPartial(const uint8_t* image) {
    if (image == nullptr) return false;

    const uint8_t updateControl[] = {0x00, 0x00};
    command(0x21);
    data(updateControl, sizeof(updateControl));
    commandWithData(0x3C, 0x80);  // Vendor partial-update border setting.

    // The vendor demo only validates the black/white partial waveform with a
    // full RAM window. Unchanged pixels have identical old and new RAM bits.
    writeFrame(0x24, image);

    commandWithData(0x22, 0xFF);  // Load and run the black/white LUT.
    command(0x20);
    if (!waitUntilIdle("partial display update")) return false;

    // The black/white LUT uses RAM 0x26 as the previous pixel state.
    writeFrame(0x26, image);
    return true;
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

  void writeFrame(uint8_t ramCommand, const uint8_t* image) {
    setAddressWindow(0, 0, kPanelWidth - 1, kPanelHeight - 1);
    setCursor(0, 0);
    command(ramCommand);
    data(image, kFrameBytes);
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
uint8_t framebuffer[kFrameBytes];
bool voiceDisplayReady = false;
uint8_t voicePartialRefreshes = 0;
bool regionCompleted[kVoiceRegionCount] = {false, false};
bool regionHasResult[kVoiceRegionCount] = {false, false};
uint16_t regionMarkerX[kVoiceRegionCount] = {};
uint16_t regionMarkerY[kVoiceRegionCount] = {};

void setPixel(uint16_t x, uint16_t y, bool black) {
  if (x >= kDisplayWidth || y >= kDisplayHeight) return;

  // Logical portrait coordinates map to a panel rotated clockwise.
  const uint16_t panelX = y;
  const uint16_t panelY = kPanelHeight - 1 - x;
  const size_t index = panelY * kPanelStride + panelX / 8;
  const uint8_t mask = 0x80U >> (panelX & 0x07U);
  if (black) {
    framebuffer[index] &= static_cast<uint8_t>(~mask);
  } else {
    framebuffer[index] |= mask;
  }
}

bool drawAsciiGlyph16(uint16_t x, uint16_t y, uint32_t codepoint) {
  if (x + ascii16::kGlyphWidth > kDisplayWidth ||
      y + ascii16::kGlyphHeight > kDisplayHeight) {
    return false;
  }

  const uint8_t* glyph = ascii16::findGlyph(codepoint);
  if (glyph == nullptr) return false;

  for (uint16_t row = 0; row < ascii16::kGlyphHeight; ++row) {
    for (uint16_t column = 0; column < ascii16::kGlyphWidth; ++column) {
      const bool black = (glyph[row] & (0x80U >> column)) != 0;
      setPixel(x + column, y + row, black);
    }
  }
  return true;
}

bool drawChineseGlyph16(uint16_t x, uint16_t y, uint32_t codepoint) {
  if (x + font16::kGlyphWidth > kDisplayWidth ||
      y + font16::kGlyphHeight > kDisplayHeight) {
    return false;
  }

  const uint8_t* glyph = font16::findGlyph(codepoint);
  if (glyph == nullptr) return false;

  for (uint16_t row = 0; row < font16::kGlyphHeight; ++row) {
    for (uint16_t column = 0; column < font16::kGlyphWidth; ++column) {
      const uint8_t value = glyph[row * 2 + column / 8];
      const bool black = (value & (0x80U >> (column & 0x07U))) != 0;
      setPixel(x + column, y + row, black);
    }
  }
  return true;
}

uint16_t glyphWidth16(uint32_t codepoint) {
  if (codepoint >= ascii16::kFirstCodepoint &&
      codepoint <= ascii16::kLastCodepoint) {
    return ascii16::kGlyphWidth;
  }
  return font16::findGlyph(codepoint) != nullptr ? font16::kGlyphWidth
                                                  : ascii16::kGlyphWidth;
}

bool drawCodepoint16(uint16_t x, uint16_t y, uint32_t codepoint) {
  if (codepoint >= ascii16::kFirstCodepoint &&
      codepoint <= ascii16::kLastCodepoint) {
    return drawAsciiGlyph16(x, y, codepoint);
  }
  if (drawChineseGlyph16(x, y, codepoint)) return true;
  return drawAsciiGlyph16(x, y, '?');
}

bool readUtf8Codepoint(const char*& text, uint32_t& codepoint) {
  const uint8_t first = static_cast<uint8_t>(*text++);
  if (first < 0x80U) {
    codepoint = first;
    return true;
  }

  if (first >= 0xC2U && first <= 0xDFU) {
    const uint8_t second = static_cast<uint8_t>(*text++);
    if ((second & 0xC0U) != 0x80U) return false;
    codepoint = ((first & 0x1FU) << 6) | (second & 0x3FU);
    return true;
  }

  if (first >= 0xE0U && first <= 0xEFU) {
    const uint8_t second = static_cast<uint8_t>(*text++);
    const uint8_t third = static_cast<uint8_t>(*text++);
    if ((second & 0xC0U) != 0x80U || (third & 0xC0U) != 0x80U) return false;
    if ((first == 0xE0U && second < 0xA0U) ||
        (first == 0xEDU && second >= 0xA0U)) {
      return false;
    }
    codepoint = ((first & 0x0FU) << 12) | ((second & 0x3FU) << 6) |
                (third & 0x3FU);
    return true;
  }

  return false;
}

bool measureText16(const char* text, uint16_t& width) {
  width = 0;
  while (*text != '\0') {
    uint32_t codepoint = 0;
    if (!readUtf8Codepoint(text, codepoint)) return false;
    const uint16_t glyphWidth = glyphWidth16(codepoint);
    if (width > kDisplayWidth - glyphWidth) return false;
    width += glyphWidth;
  }
  return width != 0;
}

bool drawText16(uint16_t x, uint16_t y, const char* text) {
  while (*text != '\0') {
    uint32_t codepoint = 0;
    if (!readUtf8Codepoint(text, codepoint) ||
        !drawCodepoint16(x, y, codepoint)) {
      return false;
    }
    x += glyphWidth16(codepoint);
  }
  return true;
}

bool drawWrappedText16(uint16_t x, uint16_t y, uint16_t width,
                       uint16_t height, const char* text) {
  if (text == nullptr || *text == '\0') return false;

  const uint16_t right = x + width;
  const uint16_t bottom = y + height;
  uint16_t cursorX = x;
  uint16_t cursorY = y;
  bool drewGlyph = false;

  while (*text != '\0') {
    uint32_t codepoint = 0;
    if (!readUtf8Codepoint(text, codepoint)) codepoint = '?';
    if (codepoint == '\r') continue;
    if (codepoint == '\n') {
      cursorX = x;
      cursorY += kVoiceLineHeight;
      continue;
    }

    const uint16_t glyphWidth = glyphWidth16(codepoint);
    if (cursorX + glyphWidth > right) {
      cursorX = x;
      cursorY += kVoiceLineHeight;
    }
    if (cursorY + font16::kGlyphHeight > bottom) break;

    if (drawCodepoint16(cursorX, cursorY, codepoint)) drewGlyph = true;
    cursorX += glyphWidth;
  }
  return drewGlyph;
}

void drawVoiceDivider() {
  for (uint16_t x = 0; x < kDisplayWidth; ++x) {
    setPixel(x, kVoiceDividerY, true);
  }
}

void clearVoiceRegion(uint8_t region) {
  const uint16_t yStart = region * kVoiceRegionHeight;
  const uint16_t yEnd = yStart + kVoiceRegionHeight;
  for (uint16_t y = yStart; y < yEnd; ++y) {
    for (uint16_t x = 0; x < kDisplayWidth; ++x) {
      setPixel(x, y, false);
    }
  }
  drawVoiceDivider();
}

void drawCompletionMarker(uint16_t x, uint16_t y, bool completed) {
  for (uint16_t row = 0; row < kVoiceMarkerSize; ++row) {
    for (uint16_t column = 0; column < kVoiceMarkerSize; ++column) {
      const bool border = row < 2 || row >= kVoiceMarkerSize - 2 ||
                          column < 2 || column >= kVoiceMarkerSize - 2;
      setPixel(x + column, y + row, completed || border);
    }
  }
}

bool drawMarkerFreeRegion(uint8_t region, const char* text) {
  if (region >= kVoiceRegionCount || text == nullptr || *text == '\0') {
    return false;
  }

  clearVoiceRegion(region);
  uint16_t textWidth = 0;
  if (!measureText16(text, textWidth)) return false;
  const uint16_t textX = (kDisplayWidth - textWidth) / 2;
  const uint16_t textY = region * kVoiceRegionHeight +
                         (kVoiceRegionHeight - font16::kGlyphHeight) / 2;

  regionCompleted[region] = false;
  regionHasResult[region] = false;
  return drawText16(textX, textY, text);
}

bool drawInitialRegion(uint8_t region) {
  const char* prompt = region == 0 ? u8"\u6309\u4F4F A1 \u8F93\u5165"
                                   : u8"\u6309\u4F4F A2 \u8F93\u5165";
  return drawMarkerFreeRegion(region, prompt);
}

bool drawInitialVoicePrompt() {
  drawVoiceDivider();
  return drawInitialRegion(0) && drawInitialRegion(1);
}

bool refreshVoiceRegion(uint8_t region) {
  if (region >= kVoiceRegionCount) return false;

  if (voicePartialRefreshes >= kPartialRefreshesBeforeFull) {
    Serial.println("Performing periodic full refresh.");
    const bool refreshed = display.display(framebuffer);
    if (refreshed) voicePartialRefreshes = 0;
    return refreshed;
  }

  Serial.printf("Differential refresh for logical region %u.\n", region + 1);
  const bool refreshed = display.displayPartial(framebuffer);
  if (refreshed) ++voicePartialRefreshes;
  return refreshed;
}

bool drawRecognitionRegion(uint8_t region, const char* text) {
  clearVoiceRegion(region);
  regionCompleted[region] = false;
  regionHasResult[region] = true;
  regionMarkerX[region] = kVoiceMarkerX;
  const uint16_t textY =
      region * kVoiceRegionHeight + kVoiceTextTopPadding;
  regionMarkerY[region] =
      textY + (font16::kGlyphHeight - kVoiceMarkerSize) / 2;
  drawCompletionMarker(regionMarkerX[region], regionMarkerY[region], false);

  const char* visibleText =
      text != nullptr && *text != '\0'
          ? text
          : u8"\u672A\u8BC6\u522B\u5230\u8BED\u97F3";
  return drawWrappedText16(kVoiceTextX, textY, kVoiceTextWidth,
                           kVoiceTextHeight, visibleText);
}

void restoreCachedTasks() {
  if (!task_store::ready()) return;
  for (uint8_t region = 0; region < kVoiceRegionCount; ++region) {
    task_store::CurrentTask task = {};
    if (!task_store::getCurrent(region, task) || !task.present) continue;
    if (!drawRecognitionRegion(region, task.text)) {
      Serial.printf("ERROR: Cached task for region %u could not be rendered.\n",
                    static_cast<unsigned>(region + 1));
      continue;
    }
    if (task.completed) {
      regionCompleted[region] = true;
      drawCompletionMarker(regionMarkerX[region], regionMarkerY[region], true);
    }
  }
}

void clearCachedTask(uint8_t region) {
  task_store::CurrentTask task = {};
  if (task_store::getCurrent(region, task) && task.present &&
      !task_store::clearCurrent(region)) {
    Serial.println("ERROR: Current task cache could not be cleared.");
  }
}

void displayRegionEvent(uint8_t region, voice_upload::RegionEvent event,
                        const char* text) {
  if (!voiceDisplayReady) {
    Serial.println("ERROR: Display is not ready for region updates.");
    return;
  }
  if (region >= kVoiceRegionCount) {
    Serial.println("ERROR: Invalid display region.");
    return;
  }

  switch (event) {
    case voice_upload::RegionEvent::Recognition:
      if (!drawRecognitionRegion(region, text)) {
        Serial.println("ERROR: Recognition text could not be rendered.");
        return;
      }
      if (!task_store::setCurrent(region, text)) {
        Serial.println("ERROR: Recognition task could not be cached.");
      }
      break;
    case voice_upload::RegionEvent::NoSpeech:
      if (!drawMarkerFreeRegion(
              region, u8"\u672A\u8BC6\u522B\u5230\u8BED\u97F3")) {
        Serial.println("ERROR: No-speech prompt could not be rendered.");
        return;
      }
      clearCachedTask(region);
      break;
    case voice_upload::RegionEvent::NotTask:
      if (regionHasResult[region]) {
        Serial.println("Non-task speech ignored; existing region preserved.");
        return;
      }
      if (!drawMarkerFreeRegion(
              region, u8"\u672A\u8BC6\u522B\u5230\u5F85\u529E\u4E8B\u9879")) {
        Serial.println("ERROR: Non-task prompt could not be rendered.");
        return;
      }
      break;
    case voice_upload::RegionEvent::ToggleCompletion:
      if (!regionHasResult[region]) {
        Serial.println("Region has no recognition result to toggle.");
        return;
      }
      regionCompleted[region] = !regionCompleted[region];
      drawCompletionMarker(regionMarkerX[region], regionMarkerY[region],
                           regionCompleted[region]);
      if (!task_store::setCompleted(region, regionCompleted[region])) {
        Serial.println("ERROR: Task completion state could not be saved.");
      }
      break;
    case voice_upload::RegionEvent::Reset:
      if (!drawInitialRegion(region)) {
        Serial.println("ERROR: Initial region prompt could not be rendered.");
        return;
      }
      clearCachedTask(region);
      break;
    case voice_upload::RegionEvent::Status:
      if (!drawMarkerFreeRegion(region, text)) {
        Serial.println("ERROR: Status text could not be rendered.");
        return;
      }
      break;
    case voice_upload::RegionEvent::Error:
      if (!drawMarkerFreeRegion(
              region, u8"\u64CD\u4F5C\u5931\u8D25\uFF0C\u8BF7\u91CD\u8BD5")) {
        Serial.println("ERROR: Generic error prompt could not be rendered.");
        return;
      }
      clearCachedTask(region);
      break;
  }

  const bool refreshed = refreshVoiceRegion(region);
  Serial.println(refreshed ? "Region update displayed."
                           : "ERROR: Display refresh failed.");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  voice_upload::setEventCallback(displayRegionEvent);
  memset(framebuffer, 0xFF, sizeof(framebuffer));
  if (!task_store::begin()) {
    Serial.println("ERROR: Task cache is unavailable.");
  }
  if (!drawInitialVoicePrompt()) {
    Serial.println("ERROR: Initial voice prompt could not be rendered.");
  }
  restoreCachedTasks();
  const bool displayInitialized = display.begin() && display.display(framebuffer);
  if (displayInitialized) {
    voiceDisplayReady = true;
    Serial.println("Voice display ready with synchronized partial-refresh RAM.");
  } else {
    Serial.println("ERROR: Voice display initialization failed.");
  }
  if (!voice_upload::begin()) {
    Serial.println("Voice upload initialization failed.");
  }
}

void loop() { voice_upload::poll(); }
