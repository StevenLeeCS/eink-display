#!/usr/bin/env python3

from pathlib import Path
import re
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
FONT_PATH = ROOT / "assets" / "gb2312_level1_16.bin"
MAP_PATH = ROOT / "assets" / "gb2312_level1_unicode_map.bin"
HEADER_PATH = ROOT / "include" / "chinese_font_16.h"
GLYPH_BYTES = 32
MAP_RECORD_BYTES = 4


class FontAssetsTest(unittest.TestCase):
    def test_font_and_map_have_matching_counts(self) -> None:
        font_count = FONT_PATH.stat().st_size // GLYPH_BYTES
        map_count = MAP_PATH.stat().st_size // MAP_RECORD_BYTES
        header = HEADER_PATH.read_text(encoding="utf-8")
        level1 = int(re.search(r"kLevel1GlyphCount = (\d+)", header).group(1))
        supplements = int(
            re.search(r"kSupplementGlyphCount = (\d+)", header).group(1)
        )

        self.assertEqual(font_count, map_count)
        self.assertEqual(font_count, level1 + supplements)

    def test_supplement_contains_xin(self) -> None:
        records = [
            struct.unpack_from("<HH", MAP_PATH.read_bytes(), offset)
            for offset in range(0, MAP_PATH.stat().st_size, MAP_RECORD_BYTES)
        ]
        mapping = dict(records)

        self.assertEqual(records, sorted(records))
        self.assertEqual(len(records), len(mapping))
        self.assertIn(ord("馨"), mapping)
        self.assertGreaterEqual(mapping[ord("馨")], 3755)


if __name__ == "__main__":
    unittest.main()
