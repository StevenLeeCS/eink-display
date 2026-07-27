#include "ascii_font_16.h"

namespace {

extern const uint8_t kAsciiFontDataStart[]
    asm("_binary_assets_asc16_printable_bin_start");

}  // namespace

namespace ascii16 {

const uint8_t* findGlyph(uint32_t codepoint) {
  if (codepoint < kFirstCodepoint || codepoint > kLastCodepoint) {
    return nullptr;
  }

  const size_t glyphIndex = codepoint - kFirstCodepoint;
  return kAsciiFontDataStart + glyphIndex * kGlyphBytes;
}

}  // namespace ascii16
