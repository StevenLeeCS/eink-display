# Project Guide

## Purpose

This project runs a voice-to-text task display on a Seeed Studio XIAO ESP32-C3,
a ZJY420S08W0G01 4.2-inch 400x300 monochrome e-paper module, and an INMP441
microphone. The panel is used clockwise in a logical 300x400 portrait layout,
split into top and bottom task regions controlled by separate buttons. The old
arbitrary-image display pipeline has been removed.

## Hardware

- E-paper: MOSI D10/GPIO10, SCK D8/GPIO8, CS D1/GPIO3, DC D2/GPIO4,
  RESET D3/GPIO5, BUSY D4/GPIO6.
- INMP441: SCK D5/GPIO7, WS D6/GPIO21, SD D7/GPIO20, L/R to GND.
- Top button A1: D0/GPIO2 to GND.
- Bottom button A2: D9/GPIO9 to GND.
- Buttons use `INPUT_PULLUP` and are active low.

## Current Behavior

- Startup shows marker-free `A1` and `A2` voice prompts.
- Rendering uses logical 300x400 portrait coordinates mapped onto the panel's
  native 400x300 framebuffer with a 90-degree clockwise rotation. Each region
  is 300x200 logical pixels.
- Presses shorter than 500 ms toggle an existing result between incomplete
  (hollow square) and complete (filled square). Initial and prompt states have
  no marker.
- Holding at least 500 ms records 16 kHz mono PCM16 and streams it with HTTP
  chunked encoding. Releasing the button submits recognition. The hard maximum
  is 60 seconds.
- On-device VAD ignores the first 300 ms, uses RMS threshold 300, and requires
  200 ms of sustained activity. A silent three-second hold immediately resets
  that region without waiting for release.
- Successful recognition replaces the selected region and starts incomplete.
- No-speech and generic retry prompts have no marker. Technical errors remain
  in the serial log.
- Display updates write both SSD1683 RAM planes. Four fast refreshes are
  followed by one full refresh to limit artifacts.

## Code Map

- `src/main.cpp`: SSD1683 driver, framebuffer, fonts, region rendering, and UI
  event handling.
- `src/voice_upload.cpp`: buttons, pre-roll, VAD, Wi-Fi, chunked upload, and
  response-to-region events.
- `src/inmp441_audio.cpp`: INMP441 I2S initialization and PCM capture.
- `src/ascii_font_16.cpp`, `src/chinese_font_16.cpp`: access embedded font data.
- `assets/`: required ASCII and GB2312 level-1 font binaries. These are not
  general display images and must remain embedded by `platformio.ini`.
- `tools/audio_receiver.py`: temporary WAV receiver and faster-whisper service.
- `tools/test_audio_receiver.py`: receiver and chunked-transfer tests.
- `DEVELOPMENT_LOG.md`: concise chronological implementation record.

## Local Configuration

Copy `include/network_config.example.h` to `include/network_config.h` and set the
Wi-Fi credentials plus receiver address. The real config is ignored by Git and
must not be committed.

## Verification

```powershell
.\.venv\Scripts\python.exe -m py_compile tools\audio_receiver.py tools\test_audio_receiver.py
.\.venv\Scripts\python.exe tools\test_audio_receiver.py
pio run
git diff --check
```

Firmware upload and physical button/display tests remain manual unless the user
explicitly requests them.

## Repository Boundaries

- Do not modify or remove `docs/`; it is vendor reference material.
- Do not modify or remove `media/`; the user manages it in parallel and it is
  unrelated to firmware development.
- Update `README.md` only when the user explicitly requests it.
- Preserve unrelated working-tree changes and keep daily log entries concise.
