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
    parser.add_argument(
        "--extra-chars-file",
        type=Path,
        default=Path(__file__).with_name("font_extra_chars.txt"),
        help="UTF-8 file containing supplemental GB2312 characters",
    )
    return parser.parse_args()


def glyph_offset(high: int, low: int) -> int:
    slot = (high - HZK_FIRST_BYTE) * ZONE_WIDTH + (low - HZK_FIRST_BYTE)
    return slot * GLYPH_BYTES


def read_extra_characters(path: Path) -> list[str]:
    if not path.exists():
        return []
    characters: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        content = line.split("#", 1)[0]
        for character in content:
            if not character.isspace() and character not in characters:
                characters.append(character)
    return characters


def main() -> None:
    args = parse_args()
    source = args.hzk16.read_bytes()
    first_offset = glyph_offset(LEVEL1_FIRST_HIGH, HZK_FIRST_BYTE)
    end_offset = glyph_offset(LEVEL1_LAST_HIGH, LEVEL1_LAST_LOW) + GLYPH_BYTES
    if len(source) < end_offset:
        raise ValueError(
            f"HZK16 is too short: {len(source)} bytes, need at least {end_offset}"
        )

    font_data = bytearray(source[first_offset:end_offset])
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

    mapped_codepoints = {codepoint for codepoint, _ in unicode_map}
    for character in read_extra_characters(args.extra_chars_file):
        codepoint = ord(character)
        if codepoint in mapped_codepoints:
            continue
        encoded = character.encode("gb2312")
        if len(encoded) != 2:
            raise ValueError(f"Supplemental character is not a GB2312 Hanzi: {character}")
        offset = glyph_offset(encoded[0], encoded[1])
        glyph = source[offset : offset + GLYPH_BYTES]
        if len(glyph) != GLYPH_BYTES:
            raise ValueError(f"HZK16 has no complete glyph for: {character}")
        font_data.extend(glyph)
        unicode_map.append((codepoint, glyph_index))
        mapped_codepoints.add(codepoint)
        glyph_index += 1

    unicode_map.sort(key=lambda item: item[0])
    map_data = b"".join(struct.pack("<HH", *item) for item in unicode_map)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    font_path = args.output_dir / "gb2312_level1_16.bin"
    map_path = args.output_dir / "gb2312_level1_unicode_map.bin"
    font_path.write_bytes(bytes(font_data))
    map_path.write_bytes(map_data)

    print(f"Wrote {font_path}: {len(font_data)} bytes")
    print(f"Wrote {map_path}: {len(map_data)} bytes")
    print(f"Glyphs: {glyph_index} ({glyph_index - LEVEL1_GLYPH_COUNT} supplemental)")


if __name__ == "__main__":
    main()
