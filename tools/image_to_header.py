#!/usr/bin/env python3
"""Convert an image to the 1-bit, 400x300 SSD1683 framebuffer format."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageEnhance, ImageOps

WIDTH = 400
HEIGHT = 300


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert PNG/JPG/BMP to include/image_data.h"
    )
    parser.add_argument("input", type=Path, help="source image")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("include/image_data.h"),
        help="generated header (default: include/image_data.h)",
    )
    parser.add_argument(
        "--fit",
        choices=("contain", "cover", "stretch"),
        default="contain",
        help="resize policy (default: contain)",
    )
    parser.add_argument(
        "--threshold",
        type=int,
        default=None,
        metavar="0..255",
        help="fixed threshold; omit to use Floyd-Steinberg dithering",
    )
    parser.add_argument(
        "--rotate", type=int, choices=(0, 90, 180, 270), default=0
    )
    parser.add_argument("--invert", action="store_true")
    parser.add_argument(
        "--contrast", type=float, default=1.15, help="contrast multiplier"
    )
    return parser.parse_args()


def prepare_image(args: argparse.Namespace) -> Image.Image:
    image = Image.open(args.input)
    image = ImageOps.exif_transpose(image).convert("L")
    if args.rotate:
        image = image.rotate(-args.rotate, expand=True, fillcolor=255)

    if args.fit == "stretch":
        image = image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    elif args.fit == "cover":
        image = ImageOps.fit(
            image, (WIDTH, HEIGHT), method=Image.Resampling.LANCZOS
        )
    else:
        image.thumbnail((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        canvas = Image.new("L", (WIDTH, HEIGHT), 255)
        canvas.paste(image, ((WIDTH - image.width) // 2, (HEIGHT - image.height) // 2))
        image = canvas

    image = ImageOps.autocontrast(image)
    image = ImageEnhance.Contrast(image).enhance(args.contrast)
    if args.invert:
        image = ImageOps.invert(image)

    if args.threshold is None:
        return image.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    if not 0 <= args.threshold <= 255:
        raise SystemExit("--threshold must be between 0 and 255")
    return image.point(lambda value: 255 if value >= args.threshold else 0, "1")


def pack_pixels(image: Image.Image) -> bytes:
    pixels = image.load()
    packed = bytearray()
    for y in range(HEIGHT):
        for x0 in range(0, WIDTH, 8):
            value = 0
            for bit in range(8):
                if pixels[x0 + bit, y]:  # SSD1683: 1=white, 0=black.
                    value |= 0x80 >> bit
            packed.append(value)
    assert len(packed) == WIDTH * HEIGHT // 8
    return bytes(packed)


def write_header(output: Path, data: bytes) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "#define EPD_HAS_IMAGE 1",
        f"constexpr size_t kImageDataSize = {len(data)};",
        "const uint8_t kImageData[kImageDataSize] PROGMEM = {",
    ]
    for offset in range(0, len(data), 16):
        chunk = ", ".join(f"0x{value:02X}" for value in data[offset : offset + 16])
        lines.append(f"    {chunk},")
    lines.extend(("};", ""))
    output.write_text("\n".join(lines), encoding="ascii")


def main() -> None:
    args = parse_args()
    image = prepare_image(args)
    data = pack_pixels(image)
    write_header(args.output, data)
    preview = args.output.with_suffix(".preview.png")
    image.convert("L").save(preview)
    print(f"Wrote {args.output} ({len(data)} bytes)")
    print(f"Preview: {preview}")


if __name__ == "__main__":
    main()
