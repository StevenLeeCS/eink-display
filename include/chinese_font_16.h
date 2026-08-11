#pragma once

#include <stddef.h>
#include <stdint.h>

namespace font16 {

constexpr uint16_t kGlyphWidth = 16;
constexpr uint16_t kGlyphHeight = 16;
constexpr size_t kGlyphBytes = 32;
constexpr size_t kLevel1GlyphCount = 3755;
constexpr size_t kSupplementGlyphCount = 1;
constexpr size_t kGlyphCount = kLevel1GlyphCount + kSupplementGlyphCount;

const uint8_t* findGlyph(uint32_t codepoint);

}  // namespace font16
