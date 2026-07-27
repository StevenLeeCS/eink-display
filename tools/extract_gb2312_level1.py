#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path


GLYPH_BYTES = 32
ZONE_WIDTH = 94
LEVEL1_FIRST_HIGH = 0xB0
LEVEL1_LAST_HIGH = 0xD7
LEVEL1_LAST_LOW = 0xF9
HZK_FIRST_BYTE = 0xA1
LEVEL1_GLYPH_COUNT = 3755


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract the 16x16 GB2312 level-1 font and Unicode map."
    )
    parser.add_argument("hzk16", type=Path, help="Path to the source HZK16 file")
    parser.add_argument(
        "--output-dir", type=Path, default=Path("assets"), help="Output directory"
    )
    return parser.parse_args()


def glyph_offset(high: int, low: int) -> int:
    slot = (high - HZK_FIRST_BYTE) * ZONE_WIDTH + (low - HZK_FIRST_BYTE)
    return slot * GLYPH_BYTES


def main() -> None:
    args = parse_args()
    source = args.hzk16.read_bytes()
    first_offset = glyph_offset(LEVEL1_FIRST_HIGH, HZK_FIRST_BYTE)
    end_offset = glyph_offset(LEVEL1_LAST_HIGH, LEVEL1_LAST_LOW) + GLYPH_BYTES
    if len(source) < end_offset:
        raise ValueError(
            f"HZK16 is too short: {len(source)} bytes, need at least {end_offset}"
        )

    font_data = source[first_offset:end_offset]
    if len(font_data) != LEVEL1_GLYPH_COUNT * GLYPH_BYTES:
        raise AssertionError("Unexpected GB2312 level-1 font size")

    unicode_map = []
    glyph_index = 0
    for high in range(LEVEL1_FIRST_HIGH, LEVEL1_LAST_HIGH + 1):
        final_low = LEVEL1_LAST_LOW if high == LEVEL1_LAST_HIGH else 0xFE
        for low in range(HZK_FIRST_BYTE, final_low + 1):
            character = bytes((high, low)).decode("gb2312")
            unicode_map.append((ord(character), glyph_index))
            glyph_index += 1

    if glyph_index != LEVEL1_GLYPH_COUNT:
        raise AssertionError("Unexpected GB2312 level-1 glyph count")

    unicode_map.sort(key=lambda item: item[0])
    map_data = b"".join(struct.pack("<HH", *item) for item in unicode_map)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    font_path = args.output_dir / "gb2312_level1_16.bin"
    map_path = args.output_dir / "gb2312_level1_unicode_map.bin"
    font_path.write_bytes(font_data)
    map_path.write_bytes(map_data)

    print(f"Wrote {font_path}: {len(font_data)} bytes")
    print(f"Wrote {map_path}: {len(map_data)} bytes")
    print(f"Glyphs: {glyph_index}")


if __name__ == "__main__":
    main()
