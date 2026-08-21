#include <Arduino.h>
#include <SPI.h>
#include <cstring>
#include <time.h>

#include "ascii_font_16.h"
#include "board_pins.h"
#include "chinese_font_16.h"
#include "device_settings.h"
#include "function_area.h"
#include "pomodoro.h"
#include "task_scheduler.h"
#include "task_store.h"
#include "ui_layout.h"
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

constexpr int kPinMosi = board_pins::kEpaperMosi;
constexpr int kPinSck = board_pins::kEpaperSck;
constexpr int kPinCs = board_pins::kEpaperCs;
constexpr int kPinDc = board_pins::kEpaperDc;
constexpr int kPinReset = board_pins::kEpaperReset;
constexpr int kPinBusy = board_pins::kEpaperBusy;

constexpr uint32_t kSpiFrequency = 4000000;
constexpr uint32_t kBusyTimeoutMs = 30000;
constexpr uint16_t kSlotPitch = 83;
constexpr uint16_t kSlotVisibleHeight = 69;
constexpr uint16_t kTaskMarkerX = 12;
constexpr uint16_t kTaskMarkerSize = 12;
constexpr uint16_t kTaskMarkerGap = 8;
constexpr uint16_t kTaskTextX =
    kTaskMarkerX + kTaskMarkerSize + kTaskMarkerGap;
constexpr uint16_t kSlotTextTopPadding = 6;
constexpr uint16_t kTaskTextWidth = kDisplayWidth - kTaskTextX - 12;
constexpr uint16_t kSlotTextHeight = 56;
constexpr uint16_t kTextLineHeight = 20;
constexpr uint16_t kFunctionTextX = 8;
constexpr uint16_t kFunctionTextRight = kDisplayWidth - 8;
constexpr uint16_t kFunctionFirstLineOffset = 2;
constexpr uint16_t kFunctionLineHeight = 22;
constexpr int8_t kSlotTextOffsets[ui_layout::kDisplaySlotCount] = {
    -4, 0, 0, 4, 4};  // About 1 mm at the panel's pixel density.
constexpr uint32_t kFullWidthExclamation = 0xFF01;
constexpr uint8_t kPartialRefreshesBeforeFull = 4;

static_assert(kTaskTextWidth / font16::kGlyphWidth ==
                  ui_layout::kFullWidthCharactersPerLine,
              "Task width must match the server display contract");
static_assert(kSlotTextTopPadding + kSlotTextHeight <= kSlotVisibleHeight,
              "Three text lines must fit inside one case opening");

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
bool regionCompleted[ui_layout::kTaskRegionCount] = {};
bool regionHasResult[ui_layout::kTaskRegionCount] = {};
uint16_t regionMarkerX[ui_layout::kTaskRegionCount] = {};
uint16_t regionMarkerY[ui_layout::kTaskRegionCount] = {};
uint32_t renderedProfileRevision = 0;
function_area::Scene activeFunctionScene = function_area::Scene::Welcome;
bool deferFunctionRefresh = false;
bool functionAreaDirty = false;

void setPixel(uint16_t x, uint16_t y, bool black) {
  if (x >= kDisplayWidth || y >= kDisplayHeight) return;

  // Map portrait coordinates to the panel with the display turned 180 degrees
  // from the original clockwise orientation.
  const uint16_t panelX = kPanelWidth - 1 - y;
  const uint16_t panelY = x;
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
  if (codepoint == kFullWidthExclamation) return ascii16::kGlyphWidth;
  if (codepoint >= ascii16::kFirstCodepoint &&
      codepoint <= ascii16::kLastCodepoint) {
    return ascii16::kGlyphWidth;
  }
  return font16::findGlyph(codepoint) != nullptr ? font16::kGlyphWidth
                                                  : ascii16::kGlyphWidth;
}

bool drawCodepoint16(uint16_t x, uint16_t y, uint32_t codepoint) {
  if (codepoint == kFullWidthExclamation) {
    return drawAsciiGlyph16(x, y, '!');
  }
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
      cursorY += kTextLineHeight;
      continue;
    }

    const uint16_t glyphWidth = glyphWidth16(codepoint);
    if (cursorX + glyphWidth > right) {
      cursorX = x;
      cursorY += kTextLineHeight;
    }
    if (cursorY + font16::kGlyphHeight > bottom) break;

    if (drawCodepoint16(cursorX, cursorY, codepoint)) drewGlyph = true;
    cursorX += glyphWidth;
  }
  return drewGlyph;
}

uint16_t slotTop(uint8_t slot) {
  return static_cast<uint16_t>(slot) * kSlotPitch;
}

uint16_t slotTextY(uint8_t slot, uint16_t y) {
  return static_cast<uint16_t>(static_cast<int32_t>(y) +
                               kSlotTextOffsets[slot]);
}

void clearDisplaySlot(uint8_t slot) {
  const uint16_t yStart = slotTop(slot);
  const uint16_t yEnd =
      slot + 1 < ui_layout::kDisplaySlotCount ? slotTop(slot + 1)
                                              : kDisplayHeight;
  for (uint16_t y = yStart; y < yEnd; ++y) {
    for (uint16_t x = 0; x < kDisplayWidth; ++x) {
      setPixel(x, y, false);
    }
  }
}

void drawSlotDividers() {
  for (uint8_t slot = 0; slot + 1 < ui_layout::kDisplaySlotCount; ++slot) {
    const uint16_t dividerY = slotTop(slot) + kSlotVisibleHeight - 1;
    for (uint16_t x = 0; x < kDisplayWidth; ++x) {
      setPixel(x, dividerY, true);
    }
  }
}

void drawCompletionMarker(uint16_t x, uint16_t y, bool completed) {
  for (uint16_t row = 0; row < kTaskMarkerSize; ++row) {
    for (uint16_t column = 0; column < kTaskMarkerSize; ++column) {
      const bool border = row < 2 || row >= kTaskMarkerSize - 2 ||
                          column < 2 || column >= kTaskMarkerSize - 2;
      setPixel(x + column, y + row, completed || border);
    }
  }
}

bool drawCenteredSlotText(uint8_t slot, const char* text) {
  if (slot >= ui_layout::kDisplaySlotCount || text == nullptr ||
      *text == '\0') {
    return false;
  }

  String lines[ui_layout::kLinesPerSlot];
  uint8_t lineCount = 0;
  const char* lineStart = text;
  for (const char* cursor = text;; ++cursor) {
    if (*cursor != '\n' && *cursor != '\0') continue;
    if (lineCount >= ui_layout::kLinesPerSlot) return false;
    lines[lineCount++] = String(lineStart).substring(0, cursor - lineStart);
    if (*cursor == '\0') break;
    lineStart = cursor + 1;
  }

  clearDisplaySlot(slot);
  drawSlotDividers();
  const uint16_t blockHeight = font16::kGlyphHeight +
      static_cast<uint16_t>(lineCount - 1) * kTextLineHeight;
  const uint16_t firstY = slotTextY(
      slot, slotTop(slot) + (kSlotVisibleHeight - blockHeight) / 2);
  bool drewText = false;
  for (uint8_t line = 0; line < lineCount; ++line) {
    uint16_t textWidth = 0;
    if (!measureText16(lines[line].c_str(), textWidth) ||
        textWidth > kDisplayWidth) {
      return false;
    }
    const uint16_t textX = (kDisplayWidth - textWidth) / 2;
    if (!drawText16(textX, firstY + line * kTextLineHeight,
                    lines[line].c_str())) {
      return false;
    }
    drewText = true;
  }
  return drewText;
}

bool drawMarkerFreeTaskRegion(uint8_t region, const char* text) {
  if (region >= ui_layout::kTaskRegionCount) return false;
  regionCompleted[region] = false;
  regionHasResult[region] = false;
  return drawCenteredSlotText(region, text);
}

bool drawInitialRegion(uint8_t region) {
  if (region >= ui_layout::kTaskRegionCount) return false;
  constexpr const char* kPrompts[ui_layout::kTaskRegionCount] = {
      u8"\u6309\u4F4F B4 \u8F93\u5165",
      u8"\u6309\u4F4F B5 \u8F93\u5165",
      u8"\u6309\u4F4F B6 \u8F93\u5165",
      u8"\u6309\u4F4F B7 \u8F93\u5165",
  };
  regionCompleted[region] = false;
  regionHasResult[region] = false;
  return drawCenteredSlotText(region, kPrompts[region]);
}

bool drawFunctionPresentation(const char* message, const char* emoticon) {
  const device_settings::ProfileSettings& profile =
      device_settings::profile();
  const String salutation = String(profile.nickname) + ',';

  uint16_t salutationWidth = 0;
  uint16_t messageWidth = 0;
  uint16_t emoticonWidth = 0;
  if (!measureText16(salutation.c_str(), salutationWidth) ||
      !measureText16(message, messageWidth) ||
      !measureText16(emoticon, emoticonWidth) ||
      salutationWidth > kFunctionTextRight - kFunctionTextX ||
      messageWidth > kFunctionTextRight - kFunctionTextX ||
      emoticonWidth > kFunctionTextRight - kFunctionTextX) {
    return false;
  }

  clearDisplaySlot(ui_layout::kFunctionSlot);
  drawSlotDividers();
  const uint16_t firstY = slotTextY(
      ui_layout::kFunctionSlot,
      slotTop(ui_layout::kFunctionSlot) + kFunctionFirstLineOffset);
  return drawText16(kFunctionTextX, firstY, salutation.c_str()) &&
         drawText16(kFunctionTextX, firstY + kFunctionLineHeight,
                    message) &&
         drawText16(kFunctionTextRight - emoticonWidth,
                    firstY + 2 * kFunctionLineHeight,
                    emoticon);
}

bool drawPomodoroPresentation(const pomodoro::Presentation& presentation) {
  uint16_t line1Width = 0;
  uint16_t line2Width = 0;
  uint16_t line3Width = 0;
  uint16_t emoticonWidth = 0;
  if (!measureText16(presentation.line1, line1Width) ||
      !measureText16(presentation.line2, line2Width) ||
      !measureText16(presentation.line3, line3Width) ||
      !measureText16(presentation.emoticon, emoticonWidth)) {
    return false;
  }
  const uint16_t availableWidth = kFunctionTextRight - kFunctionTextX;
  if (line1Width > availableWidth || line2Width > availableWidth ||
      emoticonWidth + 8 > availableWidth ||
      line3Width > availableWidth - emoticonWidth - 8) {
    return false;
  }
  const uint16_t emoticonX = kFunctionTextRight - emoticonWidth;

  clearDisplaySlot(ui_layout::kFunctionSlot);
  drawSlotDividers();
  const uint16_t firstY = slotTextY(
      ui_layout::kFunctionSlot,
      slotTop(ui_layout::kFunctionSlot) + kFunctionFirstLineOffset);
  return drawText16(kFunctionTextX, firstY, presentation.line1) &&
         drawText16(kFunctionTextX, firstY + kFunctionLineHeight,
                    presentation.line2) &&
         drawText16(kFunctionTextX, firstY + 2 * kFunctionLineHeight,
                    presentation.line3) &&
         drawText16(emoticonX, firstY + 2 * kFunctionLineHeight,
                    presentation.emoticon);
}

bool drawFunctionArea() {
  if (pomodoro::state().active) {
    const pomodoro::Presentation& presentation = pomodoro::presentation();
    return drawPomodoroPresentation(presentation);
  }
  const device_settings::ProfileSettings& profile =
      device_settings::profile();
  const function_area::Scene scene =
      !profile.functionTestEnabled
          ? function_area::Scene::Welcome
          : (profile.aiSceneTestEnabled ? activeFunctionScene
                                        : profile.testScene);
  const function_area::Presentation& presentation =
      function_area::presentationFor(scene);
  return drawFunctionPresentation(presentation.message,
                                  presentation.emoticon);
}

bool drawInitialDisplay() {
  bool rendered = true;
  for (uint8_t region = 0; region < ui_layout::kTaskRegionCount; ++region) {
    rendered = drawInitialRegion(region) && rendered;
  }
  return drawFunctionArea() && rendered;
}

bool refreshDisplaySlot(uint8_t slot);

void applyScheduledFunctionScene(function_area::Scene scene) {
  if (!function_area::isValid(scene)) return;
  activeFunctionScene = scene;
  if (pomodoro::state().active) return;
  if (!drawFunctionArea()) {
    Serial.println("ERROR: Scheduled function scene could not be rendered.");
    return;
  }
  functionAreaDirty = true;
  if (deferFunctionRefresh || !voiceDisplayReady) return;
  const bool refreshed = refreshDisplaySlot(ui_layout::kFunctionSlot);
  if (refreshed) functionAreaDirty = false;
  Serial.printf("Scheduled function scene '%s' %s.\n",
                function_area::sceneId(scene),
                refreshed ? "displayed" : "refresh failed");
}

void refreshFunctionAreaIfChanged() {
  const uint32_t revision = device_settings::profileRevision();
  if (!voiceDisplayReady || revision == renderedProfileRevision) return;
  renderedProfileRevision = revision;
  if (pomodoro::state().active) return;
  if (device_settings::profile().functionTestEnabled &&
      device_settings::profile().aiSceneTestEnabled) {
    task_scheduler::settingsChanged();
    return;
  }
  activeFunctionScene = device_settings::profile().testScene;
  if (!drawFunctionArea()) {
    Serial.println("ERROR: Function area profile could not be rendered.");
    return;
  }
  const bool refreshed = refreshDisplaySlot(ui_layout::kFunctionSlot);
  Serial.println(refreshed ? "Function area profile displayed."
                           : "ERROR: Function area refresh failed.");
}

void refreshPomodoroArea() {
  if (!pomodoro::state().active) {
    task_scheduler::resetToWelcome();
    activeFunctionScene = function_area::Scene::Welcome;
    const function_area::Presentation& welcome =
        function_area::presentationFor(function_area::Scene::Welcome);
    if (!drawFunctionPresentation(welcome.message, welcome.emoticon)) {
      Serial.println("ERROR: Welcome function area could not be rendered.");
      return;
    }
    functionAreaDirty = true;
    if (!voiceDisplayReady) return;
    const bool refreshed = refreshDisplaySlot(ui_layout::kFunctionSlot);
    if (refreshed) functionAreaDirty = false;
    Serial.println(refreshed ? "Welcome function area displayed."
                             : "ERROR: Welcome display refresh failed.");
    return;
  }
  const pomodoro::Presentation& presentation = pomodoro::presentation();
  if (!drawPomodoroPresentation(presentation)) {
    Serial.println("ERROR: Pomodoro function area could not be rendered.");
    return;
  }
  functionAreaDirty = true;
  if (!voiceDisplayReady) return;
  const bool refreshed = refreshDisplaySlot(ui_layout::kFunctionSlot);
  if (refreshed) functionAreaDirty = false;
  Serial.println(refreshed ? "Pomodoro function area displayed."
                           : "ERROR: Pomodoro display refresh failed.");
}

bool refreshDisplaySlot(uint8_t slot) {
  if (slot >= ui_layout::kDisplaySlotCount) return false;

  if (voicePartialRefreshes >= kPartialRefreshesBeforeFull) {
    Serial.println("Performing periodic full refresh.");
    const bool refreshed = display.display(framebuffer);
    if (refreshed) voicePartialRefreshes = 0;
    return refreshed;
  }

  Serial.printf("Differential refresh for logical slot %u.\n", slot + 1);
  const bool refreshed = display.displayPartial(framebuffer);
  if (refreshed) ++voicePartialRefreshes;
  return refreshed;
}

bool drawRecognitionRegion(uint8_t region, const char* text) {
  if (region >= ui_layout::kTaskRegionCount) return false;
  clearDisplaySlot(region);
  drawSlotDividers();
  regionCompleted[region] = false;
  regionHasResult[region] = true;
  regionMarkerX[region] = kTaskMarkerX;
  const uint16_t textY =
      slotTextY(region, slotTop(region) + kSlotTextTopPadding);
  regionMarkerY[region] =
      textY + (font16::kGlyphHeight - kTaskMarkerSize) / 2;
  drawCompletionMarker(regionMarkerX[region], regionMarkerY[region], false);

  const char* visibleText =
      text != nullptr && *text != '\0'
          ? text
          : u8"\u672A\u8BC6\u522B\u5230\u8BED\u97F3";
  return drawWrappedText16(kTaskTextX, textY, kTaskTextWidth,
                           kSlotTextHeight, visibleText);
}

void restoreCachedTasks() {
  if (!task_store::ready()) return;
  for (uint8_t region = 0; region < ui_layout::kTaskRegionCount; ++region) {
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
                        const char* text,
                        const task_store::TaskSchedule* schedule) {
  if (!voiceDisplayReady) {
    Serial.println("ERROR: Display is not ready for region updates.");
    return;
  }
  if (event == voice_upload::RegionEvent::Status) {
    if (region != ui_layout::kFunctionSlot ||
        !drawCenteredSlotText(region, text)) {
      Serial.println("ERROR: Function status could not be rendered.");
      return;
    }
    const bool refreshed = refreshDisplaySlot(region);
    Serial.println(refreshed ? "Function status displayed."
                             : "ERROR: Display refresh failed.");
    return;
  }
  if (region >= ui_layout::kTaskRegionCount) {
    Serial.println("ERROR: Invalid display region.");
    return;
  }

  switch (event) {
    case voice_upload::RegionEvent::Recognition:
    case voice_upload::RegionEvent::RecognitionNoHistory:
      if (!drawRecognitionRegion(region, text)) {
        Serial.println("ERROR: Recognition text could not be rendered.");
        return;
      }
      deferFunctionRefresh = true;
      if (!task_store::setCurrent(
              region, text,
              event == voice_upload::RegionEvent::Recognition,
              schedule != nullptr ? *schedule : task_store::TaskSchedule{})) {
        Serial.println("ERROR: Recognition task could not be cached.");
      } else if (event == voice_upload::RegionEvent::Recognition) {
        task_scheduler::taskStored();
      }
      break;
    case voice_upload::RegionEvent::NoSpeech:
      if (!drawMarkerFreeTaskRegion(
              region, u8"\u672A\u8BC6\u522B\u5230\u8BED\u97F3")) {
        Serial.println("ERROR: No-speech prompt could not be rendered.");
        return;
      }
      deferFunctionRefresh = true;
      clearCachedTask(region);
      task_scheduler::taskRemoved();
      break;
    case voice_upload::RegionEvent::ToggleCompletion: {
      if (!regionHasResult[region]) {
        Serial.println("Region has no recognition result to toggle.");
        return;
      }
      deferFunctionRefresh = true;
      regionCompleted[region] = !regionCompleted[region];
      drawCompletionMarker(regionMarkerX[region], regionMarkerY[region],
                           regionCompleted[region]);
      const time_t completionTime = time(nullptr);
      const uint64_t completedAt =
          completionTime >= 1700000000
              ? static_cast<uint64_t>(completionTime)
              : 0;
      if (!task_store::setCompleted(region, regionCompleted[region],
                                    completedAt)) {
        Serial.println("ERROR: Task completion state could not be saved.");
      } else {
        task_scheduler::completionChanged(regionCompleted[region]);
      }
      break;
    }
    case voice_upload::RegionEvent::Reset:
      if (!drawInitialRegion(region)) {
        Serial.println("ERROR: Initial region prompt could not be rendered.");
        return;
      }
      deferFunctionRefresh = true;
      clearCachedTask(region);
      task_scheduler::taskRemoved();
      break;
    case voice_upload::RegionEvent::Status:
      // Function-slot status is handled before task-region validation.
      break;
    case voice_upload::RegionEvent::Error:
      if (!drawMarkerFreeTaskRegion(
              region, u8"\u64CD\u4F5C\u5931\u8D25\uFF0C\u8BF7\u91CD\u8BD5")) {
        Serial.println("ERROR: Generic error prompt could not be rendered.");
        return;
      }
      deferFunctionRefresh = true;
      clearCachedTask(region);
      task_scheduler::taskRemoved();
      break;
  }

  const bool refreshed = refreshDisplaySlot(region);
  deferFunctionRefresh = false;
  if (refreshed) functionAreaDirty = false;
  Serial.println(refreshed ? "Region update displayed."
                           : "ERROR: Display refresh failed.");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  voice_upload::setEventCallback(displayRegionEvent);
  memset(framebuffer, 0xFF, sizeof(framebuffer));
  device_settings::begin();
  activeFunctionScene = device_settings::profile().testScene;
  if (!task_store::begin()) {
    Serial.println("ERROR: Task cache is unavailable.");
  }
  pomodoro::begin(refreshPomodoroArea);
  task_scheduler::begin(applyScheduledFunctionScene);
  if (!drawInitialDisplay()) {
    Serial.println("ERROR: Initial five-slot display could not be rendered.");
  }
  restoreCachedTasks();
  renderedProfileRevision = device_settings::profileRevision();
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

void loop() {
  voice_upload::poll();
  refreshFunctionAreaIfChanged();
  task_scheduler::poll();
  pomodoro::poll();
}
