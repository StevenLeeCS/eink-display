#!/usr/bin/env python3
import argparse
from pathlib import Path


GLYPH_BYTES = 16
FIRST_CODEPOINT = 0x20
LAST_CODEPOINT = 0x7E
SOURCE_GLYPH_COUNT = 256


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract printable 8x16 ASCII glyphs from ASC16."
    )
    parser.add_argument("asc16", type=Path, help="Path to the source ASC16 file")
    parser.add_argument(
        "--output-dir", type=Path, default=Path("assets"), help="Output directory"
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    source = args.asc16.read_bytes()
    expected_size = SOURCE_GLYPH_COUNT * GLYPH_BYTES
    if len(source) != expected_size:
        raise ValueError(
            f"Unexpected ASC16 size: {len(source)} bytes, expected {expected_size}"
        )

    first_offset = FIRST_CODEPOINT * GLYPH_BYTES
    end_offset = (LAST_CODEPOINT + 1) * GLYPH_BYTES
    printable_data = source[first_offset:end_offset]

    args.output_dir.mkdir(parents=True, exist_ok=True)
    output_path = args.output_dir / "asc16_printable.bin"
    output_path.write_bytes(printable_data)

    glyph_count = LAST_CODEPOINT - FIRST_CODEPOINT + 1
    print(f"Wrote {output_path}: {len(printable_data)} bytes")
    print(f"Glyphs: {glyph_count} (0x{FIRST_CODEPOINT:02X}-0x{LAST_CODEPOINT:02X})")


if __name__ == "__main__":
    main()
