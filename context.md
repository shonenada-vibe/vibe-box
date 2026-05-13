# Vibe Box Agent Context

This file is for future coding agents taking over the project. Prefer it as the first orientation document, then read `progress.md`, `firmware/README.md`, and `docs/bringup.md` only as needed.

## Project Goal

Vibe Box is an ESP32-S3 device project for recording short audio, sending it directly to a Whisper-compatible transcription endpoint, and using the transcript as text input on macOS through BLE.

Target hardware:

- Waveshare `ESP32-S3-Touch-ePaper-1.54` V2
- ESP-IDF 5.5+
- 8 MB flash, octal PSRAM

## Current State

Working or implemented:

- ESP-IDF firmware builds for `esp32s3`
- Wi-Fi STA plus SoftAP web provisioning
- Runtime config persisted in NVS namespace `vibe_box`
- Press-and-hold `BOOT` button records audio, release uploads WAV to a Whisper-compatible `/v1/audio/transcriptions` endpoint
- BLE starts as `VibeBox`
- BLE HID keyboard service is present for OS pairing/classification
- Custom BLE text notify service sends transcripts to the host script
- macOS helper script receives BLE text notifications and pastes into the active app
- Fast double click on `PWR` resets BLE session state and waits for reconnect

Still incomplete or risky:

- ES8311 and real board audio path need more hardware validation
- Touch, sensors, RTC, speaker, and low-power flows are not complete
- BLE has been sensitive to macOS/CoreBluetooth GATT caching; see BLE notes below
- `server/` is a thin/mock service path and is no longer the primary firmware path

## Key Files

- `firmware/main/main.c`
  Main app state, provisioning, Wi-Fi, upload, button tasks, UI status.
- `firmware/components/audio_input/`
  I2S/WAV capture and ES8311-related audio code.
- `firmware/components/ble_keyboard/`
  BLE HID keyboard plus custom transcript notify service.
- `host/vibebox_text_input.py`
  macOS-only BLE client that subscribes to transcript notifications and pastes text via AppleScript.
- `firmware/main/Kconfig.projbuild`
  User-facing firmware config, including Wi-Fi, Whisper endpoint, I2S pins, PWR double click config.
- `firmware/sdkconfig.defaults`
  Defaults for flash, partitions, PSRAM, BLE.
- `firmware/partitions.csv`
  Custom 8 MB flash partition table. App partition is 3 MB because BLE + Wi-Fi + HTTP grew the binary.

## Build And Test Commands

Use this environment pattern in this workspace:

```sh
eval "$(pyenv init -)"
pyenv shell 3.13.8
source ~/w/esp/esp-idf/export.sh >/dev/null
cd firmware
idf.py build
```

Useful checks:

```sh
git diff --check
python3 -m py_compile host/vibebox_text_input.py host/vibebox_tui.py
```

Flash/monitor, adjust port as needed:

```sh
cd firmware
idf.py -p /dev/cu.usbmodem421201 flash monitor
```

Host helper:

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r host/requirements.txt
python host/vibebox_text_input.py
```

macOS must grant Accessibility permission to the terminal app running the helper, otherwise paste will fail.

Interactive host TUI:

```sh
python host/vibebox_tui.py
```

The TUI runs the same BLE text notification bridge, stores host-side settings in `~/.config/vibebox/tui.json`, and can fetch/edit/save runtime config over BLE without joining the provisioning Wi-Fi AP.

## BLE Notes

Device name: `VibeBox`.

Current custom text service:

- Service UUID: `48f2d101-7a15-4b3f-8d67-60587f5d1001`
- Characteristic UUID: `48f2d101-7a15-4b3f-8d67-60587f5d1002`
- Config characteristic UUID: `48f2d101-7a15-4b3f-8d67-60587f5d1003`
- Notification packet format:
  - byte 0 opcode
  - `0x01`: begin, clears host buffer
  - `0x02`: UTF-8 chunk payload
  - `0x03`: end, host decodes and pastes
- Config packet format:
  - host writes `0x10` to fetch JSON config
  - host writes `0x21` begin, `0x22` UTF-8 JSON chunks, `0x23` end to save config
  - device notifies `0x81` begin, `0x82` UTF-8 JSON/text chunks, `0x83` success end, or `0x84` error end

Important history:

- Earlier UUID version `48f2d100...` caused CoreBluetooth cache trouble and `Writing is not permitted` during `start_notify`.
- Firmware now advertises with a random static BLE address and regenerates it during BLE reinitialize to force macOS to rediscover GATT services.
- The helper defaults to only the `48f2d101...` characteristic. Do not re-enable old UUID fallback unless you have a specific migration reason.
- Fast double click `PWR` runs `ble_keyboard_reinitialize()`, disconnects current BLE peer if known, clears text notification state, changes random address, and restarts advertising.

## Button And GPIO Notes

- `BOOT` button is GPIO0, active-low.
  - Press starts recording.
  - Release stops and uploads.
- `PWR` button is GPIO18, active-low, from Waveshare V2 examples.
  - Two releases within `CONFIG_VIBE_BOX_PWR_DOUBLE_CLICK_MS` trigger BLE reinitialize.
- e-paper power is GPIO6 active-low; VBAT enable is GPIO17 active-high in `ui_epaper`.
- Audio PA defaults are GPIO42 and GPIO46.
- I2S defaults currently align with Waveshare V2 exploration:
  - MCLK GPIO14
  - BCLK GPIO15
  - WS GPIO38
  - DIN GPIO16
  - I2C SDA GPIO47
  - I2C SCL GPIO48
  - ES8311 address `0x18`

## Runtime Config

Provisioning page is at `http://192.168.4.1/` when the device lacks complete runtime config.

Persisted runtime fields include:

- `wifi_ssid`
- `wifi_password`
- `whisper_api_url`
- `whisper_api_key`
- `stt_model`
- `device_id`
- `firmware_version`
- `language`
- `recording_duration_ms`

`runtime_config_is_complete()` currently requires Wi-Fi SSID and Whisper API URL.

## Git Hygiene

At the time this file was created, `server/.envsss` is an untracked local environment file and should not be committed unless the user explicitly asks.

The user may have local edits unrelated to firmware. Do not revert unrelated dirty files.
