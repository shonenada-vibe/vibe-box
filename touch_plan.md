# Touch "Press-and-Hold to Record" Feature Design

## Goal

Mirror the existing BOOT-button behavior on the touch panel:

- Touch down on the screen → start recording (same path as `handle_press_and_hold_recording()` in `firmware/main/main.c`)
- Touch lift → stop and upload (same path as `handle_press_release_and_upload()` in `firmware/main/main.c`)
- Re-use BOOT's safety net: ignore down-events while a recording is already active, debounce noise, enforce `VIBE_BOX_RECORDING_MIN_MS`.

## Hardware (Waveshare ESP32-S3-Touch-ePaper-1.54 V2)

From the schematic (`ESP32-S3-Touch-ePaper-1.54-Schematic.pdf`) and the verified e-paper pinmap in `firmware/components/ui_epaper/ui_epaper.c`:

| Signal       | GPIO    | Notes                                                       |
| ------------ | ------- | ----------------------------------------------------------- |
| `EPD_TP_SDA` | GPIO 47 | Shared I²C bus (already used by ES8311 / SHTC3 / PCF85063)  |
| `EPD_TP_SCL` | GPIO 48 | Shared I²C bus                                              |
| `EPD_TP_RST` | GPIO 7  | Touch reset, active-low                                     |
| `EPD_TP_INT` | GPIO 21 | Touch IRQ, active-low pulse on touch event                  |

Touch IC: **CST816T** (Waveshare's standard for the 1.54" Touch-ePaper, 7-bit I²C address `0x15`).

> Confirm GPIO7 vs GPIO21 mapping (TP_RST / TP_INT) against the official V2 demo before flashing — the schematic text extract is ambiguous on which of the two leftover pins is which.

## Architectural Decisions

### 1. Detection strategy: poll + IRQ-assisted (recommended) or pure poll

CST816T's INT pin pulses low only when a finger first touches; it does NOT stay asserted while held. To know when the finger is **lifted** we must poll register `0x02` (FingerNum) over I²C. Two viable shapes:

- **Pure-poll** (simplest, mirrors BOOT button task)
  - New FreeRTOS task `touch_button_task` polling `FingerNum` every ~20 ms.
  - Same debounce/edge structure as `audio_button_task` in `firmware/main/main.c`.
  - 20 ms latency is fine for press-and-hold.

- **IRQ-armed poll** (slightly lower idle CPU)
  - Use INT to wake from a long block; once pressed, fall back to polling FingerNum until lift.

For consistency with BOOT, **pure-poll** is recommended.

### 2. I²C bus sharing

The codec/sensor I²C bus (GPIO47/48) is initialized today inside `audio_input` for ES8311. Touch needs to read at idle without contention.

- Extract the I²C master init into a small shared `vbox_i2c` helper (or simply guard with a mutex) so `touch` and `audio_input` can both call `i2c_master_bus_add_device()` against the same bus handle.
- Address conflicts: ES8311 `0x18`, SHTC3 `0x70`, PCF85063 `0x51`, CST816T `0x15` — no collisions.

### 3. Mutual exclusion with BOOT

`audio_input_recording_is_active()` already guards re-entry inside `handle_press_and_hold_recording()`. The new touch task will call the same `handle_press_and_hold_recording()` / `handle_press_release_and_upload()`, so the existing guard prevents BOOT-and-touch double-trigger automatically.

Add a small lock around start/stop pair so a "BOOT down → touch down → BOOT up → touch up" sequence cannot stop a recording owned by the other source mid-stream. Simplest: track an `s_record_owner` enum (`NONE | BOOT | TOUCH`); only the owner is allowed to stop.

### 4. Configuration surface (Kconfig)

Add to `firmware/main/Kconfig.projbuild`:

- `VIBE_BOX_TOUCH_ENABLE` (default y)
- `VIBE_BOX_TOUCH_RST_GPIO` (default 7)
- `VIBE_BOX_TOUCH_INT_GPIO` (default 21)
- `VIBE_BOX_TOUCH_I2C_ADDR` (default 0x15)
- `VIBE_BOX_TOUCH_POLL_MS` (default 20)

I²C SDA/SCL/port reuse the existing `VIBE_BOX_I2C_*` settings.

### 5. New component layout

```
firmware/components/
├─ audio_input/
├─ ble_keyboard/
├─ ui_epaper/
└─ touch_input/                  ← new
   ├─ CMakeLists.txt
   ├─ touch_input.h              ← init + is_pressed() + on_event callback
   └─ touch_input.c              ← CST816T driver + polling task
```

Public API:

```c
typedef enum { TOUCH_EVENT_DOWN, TOUCH_EVENT_UP } touch_event_t;
typedef void (*touch_event_cb_t)(touch_event_t ev, void *user);

esp_err_t touch_input_init(const touch_input_config_t *cfg,
                           touch_event_cb_t cb, void *user);
```

### 6. Wiring into `main.c`

```c
static void on_touch_event(touch_event_t ev, void *user) {
    if (ev == TOUCH_EVENT_DOWN)  handle_press_and_hold_recording();
    else                          handle_press_release_and_upload();
}
```

Register inside `app_main()` next to `audio_button_task` / `pwr_button_task` creation.

### 7. UI feedback

Reuse the existing `render_ui_status(APP_STATE_RECORDING, "Recording", "release to send")` calls already inside the BOOT handlers — no new UI work.

## State machine (touch only)

```
                 finger detected (FingerNum>0, debounced)
   ╭─────────╮  ─────────────────────────────────▶  ╭─────────────╮
   │  IDLE   │                                       │  PRESSED    │
   │ (poll)  │  ◀─────────────────────────────────  │ (poll)      │
   ╰─────────╯  finger gone (FingerNum==0, debounced) ╰─────────────╯
        │                                                  │
        │ DOWN edge → handle_press_and_hold_recording()    │
        │ UP edge   → handle_press_release_and_upload() ◀──╯
```

## Risks / Open Questions

1. **Pin mapping uncertainty (GPIO7 vs GPIO21)** — verify against Waveshare V2 demo before flashing.
2. **I²C bus init ordering** — ES8311 init currently owns the bus inside `audio_input`. We must either extract it or make sure both modules can call `i2c_new_master_bus()` idempotently.
3. **Sleep-mode wake-up** — if you later add light/deep sleep, INT-line wake (GPIO21 RTC GPIO?) becomes important; pure polling won't survive sleep.
4. **No multi-touch / gesture support** in v1 — purely down/up, exactly like BOOT.

## Open questions for confirmation

- Pin assignments (GPIO7 RST, GPIO21 INT) — OK to proceed or verify in the Waveshare V2 demo first?
- Component name `touch_input` and Kconfig defaults above?
- Pure-poll task vs IRQ-armed poll?
