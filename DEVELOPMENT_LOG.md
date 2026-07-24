# Development Log

Daily handoff notes for developers and AI agents. Add new dates at the top.
Update `README.md` only when explicitly requested.

## 2026-07-23

### Completed

- Confirmed the ZJY420S08W0G01 is a 400x300 monochrome SSD1683 module using
  3.3 V, 4-wire SPI; checked its wiring to the XIAO ESP32-C3.
- Implemented the PlatformIO/Arduino SSD1683 driver with full refresh, BUSY
  timeout handling and deep sleep.
- Successfully tested all-white, all-black and asymmetric diagnostic frames.
- Established the framebuffer format: 15,000 bytes, row-major, MSB first,
  `1 = white`, `0 = black`.
- Imported and validated PCtoLCD data from `jk.TXT`.
- Added `tools/image_to_header.py` for image resizing, threshold/dithering,
  rotation, inversion, preview generation and C header output.
- Converted `13.png` with threshold 190 and contrast 1.25; the generated
  `include/image_data.h` is the image currently displayed by the firmware.

### Current state

- Firmware uploads successfully on COM10, refreshes the generated image once,
  then puts the display into deep sleep.
- Building and uploading are performed manually by the developer.

### Note

- Serial monitor output was not visible consistently, but this did not block
  firmware upload or display operation.
