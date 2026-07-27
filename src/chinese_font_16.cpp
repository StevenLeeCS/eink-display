#include "chinese_font_16.h"

namespace {

extern const uint8_t kFontDataStart[]
    asm("_binary_assets_gb2312_level1_16_bin_start");
extern const uint8_t kUnicodeMapStart[]
    asm("_binary_assets_gb2312_level1_unicode_map_bin_start");

constexpr size_t kMapRecordBytes = 4;

uint16_t readUint16Le(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

}  // namespace

namespace font16 {

const uint8_t* findGlyph(uint32_t codepoint) {
  if (codepoint > 0xFFFFU) return nullptr;

  size_t first = 0;
  size_t last = kGlyphCount;
  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    const uint8_t* record = kUnicodeMapStart + middle * kMapRecordBytes;
    const uint16_t mappedCodepoint = readUint16Le(record);

    if (mappedCodepoint < codepoint) {
      first = middle + 1;
    } else if (mappedCodepoint > codepoint) {
      last = middle;
    } else {
      const uint16_t glyphIndex = readUint16Le(record + 2);
      return kFontDataStart + glyphIndex * kGlyphBytes;
    }
  }

  return nullptr;
}

}  // namespace font16
