# Project Guide

## Purpose

This project runs a voice-to-text task display on a Seeed Studio XIAO ESP32-C3,
a ZJY420S08W0G01 4.2-inch 400x300 monochrome e-paper module, and an INMP441
microphone. The panel is used clockwise in a logical 300x400 portrait layout,
split into top and bottom task regions controlled by separate buttons. The old
arbitrary-image display pipeline has been removed.

The current remote revision is the completed Stage 0 baseline. Continue product
development on a new branch; keep the baseline usable while replacing the
temporary PC recognition path incrementally.

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

## Target Product Flow

The target flow is:

1. Provision Wi-Fi from an ESP-hosted setup page opened on a phone on first
   use, then reconnect automatically with saved credentials.
2. Stream PCM while the button is held, finalize recognition on release, and
   pass the cloud transcript to DeepSeek.
3. Normalize the result into a task record ordered by time, place, person and
   event, then render it in the selected e-paper region.
4. After the management backend exists, add completed-task history and local
   persistence so the backend can retrieve it.

LVGL code from the reference project may inform the setup flow and visual
states, but LVGL itself is not a phone web UI. On the current display-only
hardware, provisioning should be an ESP-hosted captive portal or equivalent
HTTP page opened on the phone, with e-paper used only for status prompts.

## Reuse Strategy

Keep and extend these proven components:

- SSD1683 full-window differential refresh, portrait coordinate mapping,
  framebuffer, font lookup and region rendering in `src/main.cpp`.
- Button gestures, region ownership, VAD concepts and UI event semantics in
  `src/voice_upload.cpp`.
- INMP441 16 kHz mono PCM capture in `src/inmp441_audio.cpp`.
- GB2312 level-1 and ASCII font assets and their embedding configuration.
- The Python receiver and its tests as development fixtures for audio capture
  and protocol regression tests, not as a production dependency.

Refactor rather than preserve the current transport coupling: split capture,
cloud STT, DeepSeek processing, task persistence and UI updates into explicit
modules. The current chunked upload to `tools/audio_receiver.py` is a useful
prototype, but the production cloud API adapter will replace it.

## Development Roadmap

### Stage 0 - Baseline (complete)

- The working two-region voice task display and stable visual-region refresh
  are committed and uploaded to the remote repository.
- Start subsequent work on a new GitHub branch.

### Major Stage 1 - Core cloud workflow (development stages 1-3)

1. Wi-Fi provisioning: add first-use phone provisioning, store credentials in
   NVS/Preferences, reconnect automatically and provide a recoverable way to
   re-enter provisioning.
2. DeepSeek and display: validate a bounded structured response containing
   time, place, person and event, then render the result in the selected region.
   Preserve readable transcript/task text when fields are absent or processing
   fails.
3. Cloud STT integration: replace the PC/faster-whisper receiver with a cloud
   adapter, initially targeting the selected Baidu speech API. Preserve the
   current press-to-stream and release-to-finalize flow, and keep provider
   details outside the capture and UI modules.

Implement cloud transport last within this major stage. Wi-Fi provisioning and
DeepSeek/task rendering can first be developed against the existing streaming
receiver, so changing the STT endpoint does not block the core product path.
The stream may retain a server-policy or safety timeout, but there is no
memory-driven short recording limit and no requirement to buffer the complete
recording in ESP32-C3 RAM.

Acceptance: a fresh device can be provisioned by phone; after reboot it can
record, obtain cloud text, structure and display a task without a PC service.
Completed-task history is not required in this major stage.

### Major Stage 2 - Management backend and secondary features

- Add an explicit AP/admin mode and a phone-accessible web application.
- Manage Wi-Fi switching/logout and cloud STT/DeepSeek settings from the admin
  surface, with authentication and secret handling reviewed before release.
- After the backend shell and its data interfaces are stable, implement the
  deferred development Stage 4: versioned local persistence and management of
  completed-task history. LittleFS is the leading storage choice for records;
  NVS remains appropriate for small configuration values.
- Add other secondary convenience features only after that foundation works.

Acceptance: admin mode cannot be entered accidentally during normal recording;
the phone can maintain tasks and configuration; exiting admin mode restores
normal station-mode operation without losing records.

### Major Stage 3 - Hardware and capacity expansion

- Evaluate a PSRAM-equipped controller, with ESP32-S3 as the leading option.
- Expand the number of display regions and redesign input pin allocation,
  button scanning and layout configuration together.
- Revalidate e-paper refresh windows, memory budgets, power behavior and
  end-to-end recovery on the new board.
- Leave the final audio transport decision until the end of this stage because
  it does not block the core workflow. Prototype and measure both alternatives:
  PSRAM-backed record-then-submit to a conventional API, and direct streaming
  to a cloud WebSocket API. Compare supported duration, latency, memory use,
  reconnect behavior, provider constraints and implementation complexity, then
  select one. Until that decision, retain the current press-to-stream input
  path and its safety timeout.

Do not treat the reference `docs/ESP32S3EINCanalize.ino` as drop-in production
code. Reuse its API flow and PSRAM/LVGL ideas selectively behind this project's
interfaces.

## Future Backlog

- True hardware sub-window refresh if a stable panel-specific waveform and RAM
  window sequence can be validated.
- Additional display polish and convenience features that do not block the
  three core requirements.
- Migration from prototype compile-time API keys to protected runtime
  configuration before any public or production deployment.

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
- Update `README.md` only when the user explicitly requests it.
- Preserve unrelated working-tree changes and keep daily log entries concise.
