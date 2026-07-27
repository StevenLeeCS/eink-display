#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ascii16 {

constexpr uint16_t kGlyphWidth = 8;
constexpr uint16_t kGlyphHeight = 16;
constexpr size_t kGlyphBytes = 16;
constexpr uint8_t kFirstCodepoint = 0x20;
constexpr uint8_t kLastCodepoint = 0x7E;
constexpr size_t kGlyphCount = kLastCodepoint - kFirstCodepoint + 1;

const uint8_t* findGlyph(uint32_t codepoint);

}  // namespace ascii16
