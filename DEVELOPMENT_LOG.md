# Development Log

Daily handoff notes for developers and AI agents. Add new dates at the top.
Update `README.md` only when explicitly requested.

## 2026-07-29

### Completed

- Added standard-library Baidu STT and DeepSeek adapters behind the existing
  streaming audio receiver.
- Added bounded `time/place/person/event` JSON parsing and ordered e-paper text
  formatting with transcript fallback.
- Added `tools/cloud_config.example.env`; real API keys remain local and ignored.
- Added offline request-contract tests for Baidu OAuth/STT and DeepSeek; real
  provider calls remain pending user API configuration.
- Fixed the receiver/DeepSeek client method contract and added an integration
  regression test for structured display output.
- Added explicit non-task classification: empty regions show a marker-free
  prompt, while existing task content and completion state are preserved.
- Split provider orchestration from the HTTP receiver, retained transcript text
  when structured fields are incomplete, and added Baidu token expiry refresh.
- Added a mobile admin page for Wi-Fi, receiver and reserved cloud API settings;
  it is reachable through both the setup AP and the device's normal LAN IP.
- Added LittleFS persistence for both active regions and a 30-entry completed
  task history, with restart recovery and backend display/clear controls.

### Current state

- Set `STT_PROVIDER=baidu` and fill Baidu keys to replace faster-whisper; set
  `ENABLE_DEEPSEEK=true` and fill the DeepSeek key to enable task structuring.
- ESP32 audio streaming requests remain unchanged; the response protocol and
  display events now include an explicit non-task state.
- Firmware, Python tests and the 390 px mobile layout pass; flashing and
  physical persistence verification remain manual.

## 2026-07-28

### Completed

- Replaced unstable native half-window updates with the vendor-validated
  full-window black/white differential refresh and synchronized `0x24/0x26`.
- Verified stable A1/A2 visual region updates on the physical display without
  cross-region resets, old-content migration or half-screen inversion.
- Defined the three-major-stage product roadmap and code reuse boundaries.
- Added an ESP-hosted phone Wi-Fi portal with network scanning, credential
  verification, project-specific NVS storage and automatic reconnect.
- Added an A1+A2 two-second recovery gesture for changing Wi-Fi and verified
  the setup access point and phone provisioning flow on hardware.
- Added a disabled task-processing boundary; recognition text still passes
  through unchanged, with Baidu STT and DeepSeek integration deferred.

### Current state

- Region rendering is software-local, while each differential update transfers
  the full 400x300 frame; native SSD1683 sub-windows remain unsupported.
- Wi-Fi no longer comes from `network_config.h`; that file currently retains
  only the temporary PC audio-receiver address.

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
- Switched to the vendor SSD1683 black/white differential refresh; UI events
  only alter the selected half while the controller uses a full RAM window.
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
