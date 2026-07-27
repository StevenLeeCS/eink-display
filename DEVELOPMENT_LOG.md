# Development Log

Daily handoff notes for developers and AI agents. Add new dates at the top.
Update `README.md` only when explicitly requested.

## 2026-07-27

### Completed

- Added press-and-hold streaming input on D0/D9 with release-to-submit.
- Split recognition output into persistent top and bottom display regions.
- Added chunked HTTP audio upload support while retaining fixed-length uploads.
- Extended press-and-hold recording to a 60-second client/server safety limit.
- Verified two-button streaming input and added centered A1/A2 startup prompts.
- Added short-press completion toggles with hollow/filled region markers.
- Added 500 ms audio pre-roll and silent three-second hold-to-reset behavior.
- Kept initial A1/A2 prompts marker-free and localized no-speech output to
  Chinese.
- Made no-speech prompts marker-free and added one generic on-screen retry
  message for all upload and recognition failures.
- Added on-device speech activity detection so a silent three-second hold
  resets immediately without waiting for button release or server recognition.
- Prevented button-noise VAD latching by ignoring the first 300 ms and requiring
  200 ms of sustained speech activity.
- Removed the obsolete image pipeline and standalone display/microphone demos;
  added `AGENTS.md` as the AI project handoff entry point.
- Rotated the UI to a 300x400 portrait coordinate system with A1 above A2.
- Finalized the current release documentation and removed the obsolete Pillow
  dependency before publishing the voice-task version.

### Current state

- The portrait two-button voice workflow builds successfully and receiver tests
  pass; firmware upload and physical verification remain manual.

## 2026-07-24

### Completed

- Added the 3755-character GB2312 level-1 HZK16 font, a Unicode lookup map and
  UTF-8 text rendering for partial refresh.
- Added printable ASC16 English, number and punctuation glyphs with mixed-width
  Chinese/ASCII rendering.
- Added and compiled an INMP441 16 kHz I2S microphone level test on D5-D7.
- Added 10-second PCM streaming over Wi-Fi and a local WAV receiver.
- Added automatic faster-whisper recognition and UTF-8 text responses.
- Added wrapped recognition text rendering with automatic partial refresh.
- Switched repeated updates to the vendor dual-RAM fast-refresh sequence and
  made uploaded WAV recordings temporary.
- Verified repeated voice recognition, old-text clearing and e-paper display;
  updated the README for the working end-to-end version.

### Current state

- The end-to-end voice-to-e-paper workflow is working; recording is triggered
  by sending `r` in the serial monitor.

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
- Converted `13.png` with threshold 190 and contrast 1.25 and generated
  `include/image_data.h`.
- Added a full-white plus black/white rectangle test for SSD1683 partial refresh.
- Added a Unicode-indexed 16x16 Chinese subset font and partial-refresh text test.

### Current state

- Source is prepared to display, clear and redisplay five Chinese glyphs with
  partial refresh before putting the display into deep sleep.
- Building and uploading are performed manually by the developer.

### Note

- Serial monitor output was not visible consistently, but this did not block
  firmware upload or display operation.
