#!/usr/bin/env python3
"""Terminal UI for the VibeBox macOS BLE text bridge and provisioning config."""

from __future__ import annotations

import asyncio
import curses
import json
import logging
import os
import sys
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from bleak import BleakClient
from bleak.exc import BleakError

try:
    from vibebox_text_input import (
        DEFAULT_DEVICE_NAME,
        DEFAULT_TEXT_CHAR_UUIDS,
        OPCODE_BEGIN,
        OPCODE_CHUNK,
        OPCODE_END,
        find_device,
        type_text_macos,
    )
except ImportError:  # pragma: no cover - supports running from the repo root.
    from host.vibebox_text_input import (
        DEFAULT_DEVICE_NAME,
        DEFAULT_TEXT_CHAR_UUIDS,
        OPCODE_BEGIN,
        OPCODE_CHUNK,
        OPCODE_END,
        find_device,
        type_text_macos,
    )


CONFIG_PATH = Path(os.environ.get("VIBEBOX_TUI_CONFIG", "~/.config/vibebox/tui.json")).expanduser()
DEFAULT_CONFIG_CHAR_UUID = "48f2d101-7a15-4b3f-8d67-60587f5d1003"
CONFIG_OPCODE_GET = 0x10
CONFIG_OPCODE_SAVE_BEGIN = 0x21
CONFIG_OPCODE_SAVE_CHUNK = 0x22
CONFIG_OPCODE_SAVE_END = 0x23
CONFIG_OPCODE_RESP_BEGIN = 0x81
CONFIG_OPCODE_RESP_CHUNK = 0x82
CONFIG_OPCODE_RESP_END = 0x83
CONFIG_OPCODE_RESP_ERROR = 0x84
BLE_PACKET_PAYLOAD_MAX = 19
DEFAULT_TRANSLATION_PROMPT = (
    "You are a translation engine. Translate the user text to the target language. "
    "Return only the translated text."
)
DEFAULT_REFINE_PROMPT = (
    "You are a text refinement engine. Rewrite the text to be fluent and natural while preserving "
    "the user's final intent. Remove filler words, repeated phrases, false starts, and "
    "self-corrections. If the speaker corrects themselves, keep only the corrected final meaning. "
    "Return only the refined text."
)
BLE_CONFIG_TIMEOUT_SECONDS = 12.0


@dataclass
class HostConfig:
    device_name: str = DEFAULT_DEVICE_NAME
    char_uuids: list[str] = field(default_factory=lambda: list(DEFAULT_TEXT_CHAR_UUIDS))
    scan_timeout: float = 8.0
    retry_delay: float = 2.0
    press_return: bool = False
    config_char_uuid: str = DEFAULT_CONFIG_CHAR_UUID

    @classmethod
    def load(cls, path: Path = CONFIG_PATH) -> "HostConfig":
        cfg = cls()
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return cfg
        except (OSError, json.JSONDecodeError) as exc:
            logging.warning("failed to load %s: %s", path, exc)
            return cfg

        if isinstance(raw.get("device_name"), str):
            cfg.device_name = raw["device_name"]
        if isinstance(raw.get("char_uuids"), list) and raw["char_uuids"]:
            cfg.char_uuids = [str(item) for item in raw["char_uuids"] if str(item).strip()]
        if isinstance(raw.get("scan_timeout"), (int, float)):
            cfg.scan_timeout = float(raw["scan_timeout"])
        if isinstance(raw.get("retry_delay"), (int, float)):
            cfg.retry_delay = float(raw["retry_delay"])
        if isinstance(raw.get("press_return"), bool):
            cfg.press_return = raw["press_return"]
        if isinstance(raw.get("config_char_uuid"), str):
            cfg.config_char_uuid = raw["config_char_uuid"]
        return cfg

    def save(self, path: Path = CONFIG_PATH) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "device_name": self.device_name,
            "char_uuids": self.char_uuids,
            "scan_timeout": self.scan_timeout,
            "retry_delay": self.retry_delay,
            "press_return": self.press_return,
            "config_char_uuid": self.config_char_uuid,
        }
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


@dataclass
class DeviceConfig:
    wifi_ssid: str = ""
    wifi_password: str = ""
    whisper_api_url: str = ""
    whisper_api_key: str = ""
    stt_model: str = "whisper-large-v3-turbo"
    openai_api_base: str = ""
    openai_api_key: str = ""
    translation_model: str = "gpt-4o-mini"
    translation_target_language: str = "English"
    translation_prompt: str = DEFAULT_TRANSLATION_PROMPT
    translation_enabled: bool = False
    refine_prompt: str = DEFAULT_REFINE_PROMPT
    refine_enabled: bool = False
    device_id: str = "vibe-box-dev"
    firmware_version: str = "dev"
    language: str = "zh"
    recording_duration_ms: int = 3000

    def as_ble_payload(self) -> dict[str, str | int | bool]:
        return {
            "wifi_ssid": self.wifi_ssid,
            "wifi_password": self.wifi_password,
            "whisper_api_url": self.whisper_api_url,
            "whisper_api_key": self.whisper_api_key,
            "stt_model": self.stt_model,
            "openai_api_base": self.openai_api_base,
            "openai_api_key": self.openai_api_key,
            "translation_model": self.translation_model,
            "translation_target_language": self.translation_target_language,
            "translation_prompt": self.translation_prompt,
            "translation_enabled": self.translation_enabled,
            "refine_prompt": self.refine_prompt,
            "refine_enabled": self.refine_enabled,
            "device_id": self.device_id,
            "firmware_version": self.firmware_version,
            "language": self.language,
            "recording_duration_ms": self.recording_duration_ms,
        }


def device_config_from_json(payload: str) -> DeviceConfig:
    values = json.loads(payload)
    return DeviceConfig(
        wifi_ssid=values.get("wifi_ssid", ""),
        wifi_password=values.get("wifi_password", ""),
        whisper_api_url=values.get("whisper_api_url", ""),
        whisper_api_key=values.get("whisper_api_key", ""),
        stt_model=values.get("stt_model", "whisper-large-v3-turbo"),
        openai_api_base=values.get("openai_api_base", ""),
        openai_api_key=values.get("openai_api_key", ""),
        translation_model=values.get("translation_model", "gpt-4o-mini"),
        translation_target_language=values.get("translation_target_language", "English"),
        translation_prompt=values.get("translation_prompt", DEFAULT_TRANSLATION_PROMPT),
        translation_enabled=bool(values.get("translation_enabled", False)),
        refine_prompt=values.get("refine_prompt", DEFAULT_REFINE_PROMPT),
        refine_enabled=bool(values.get("refine_enabled", False)),
        device_id=values.get("device_id", "vibe-box-dev"),
        firmware_version=values.get("firmware_version", "dev"),
        language=values.get("language", "zh"),
        recording_duration_ms=int(values.get("recording_duration_ms") or 3000),
    )


def validate_device_config(cfg: DeviceConfig) -> None:
    if not cfg.wifi_ssid.strip() or not cfg.whisper_api_url.strip():
        raise ValueError("wifi_ssid and whisper_api_url are required")
    if cfg.recording_duration_ms < 1000 or cfg.recording_duration_ms > 15000:
        raise ValueError("recording_duration_ms must be between 1000 and 15000")


class TextAssembler:
    def __init__(self, on_text) -> None:
        self._chunks = bytearray()
        self._on_text = on_text

    def handle_packet(self, data: bytearray) -> None:
        if not data:
            return

        opcode = data[0]
        payload = bytes(data[1:])

        if opcode == OPCODE_BEGIN:
            self._chunks.clear()
            return
        if opcode == OPCODE_CHUNK:
            self._chunks.extend(payload)
            return
        if opcode == OPCODE_END:
            text = self._chunks.decode("utf-8", errors="replace").strip()
            self._chunks.clear()
            self._on_text(text)
            return

        self._on_text(f"unknown text packet opcode: 0x{opcode:02x}", is_error=True)


class ConfigResponseAssembler:
    def __init__(self, on_response) -> None:
        self._chunks = bytearray()
        self._on_response = on_response

    def handle_packet(self, data: bytearray) -> None:
        if not data:
            return

        opcode = data[0]
        payload = bytes(data[1:])

        if opcode == CONFIG_OPCODE_RESP_BEGIN:
            self._chunks.clear()
            return
        if opcode == CONFIG_OPCODE_RESP_CHUNK:
            self._chunks.extend(payload)
            return
        if opcode in {CONFIG_OPCODE_RESP_END, CONFIG_OPCODE_RESP_ERROR}:
            text = self._chunks.decode("utf-8", errors="replace").strip()
            self._chunks.clear()
            self._on_response(opcode == CONFIG_OPCODE_RESP_END, text)
            return

        self._on_response(False, f"unknown config packet opcode: 0x{opcode:02x}")


@dataclass
class TuiState:
    host_config: HostConfig
    device_config: DeviceConfig = field(default_factory=DeviceConfig)
    active_panel: int = 0
    selected: list[int] = field(default_factory=lambda: [0, 0])
    ble_task: asyncio.Task | None = None
    ble_stop_event: asyncio.Event | None = None
    ble_client: BleakClient | None = None
    ble_config_ready: bool = False
    config_response_future: asyncio.Future | None = None
    ble_status: str = "stopped"
    logs: deque[str] = field(default_factory=lambda: deque(maxlen=200))
    last_transcript: str = ""

    def log(self, message: str) -> None:
        self.logs.append(message)


HOST_FIELDS = [
    ("device_name", "Device name", False),
    ("char_uuids", "Text char UUIDs", False),
    ("scan_timeout", "Scan timeout", False),
    ("retry_delay", "Retry delay", False),
    ("press_return", "Press return", False),
    ("config_char_uuid", "Config char UUID", False),
]

DEVICE_FIELDS = [
    ("wifi_ssid", "Wi-Fi SSID", False),
    ("wifi_password", "Wi-Fi password", True),
    ("whisper_api_url", "Whisper API URL", False),
    ("whisper_api_key", "Whisper API key", True),
    ("stt_model", "STT model", False),
    ("openai_api_base", "OpenAI API base", False),
    ("openai_api_key", "OpenAI API key", True),
    ("translation_model", "Translation model", False),
    ("translation_target_language", "Translation target", False),
    ("translation_prompt", "Translation prompt", False),
    ("translation_enabled", "Translation enabled", False),
    ("refine_prompt", "Refine prompt", False),
    ("refine_enabled", "Refine enabled", False),
    ("device_id", "Device ID", False),
    ("firmware_version", "Firmware version", False),
    ("language", "Language", False),
    ("recording_duration_ms", "Recording duration ms", False),
]


def display_value(value: Any, secret: bool = False) -> str:
    if isinstance(value, list):
        value = ", ".join(value)
    if isinstance(value, bool):
        return "on" if value else "off"
    text = str(value)
    if secret:
        return "<empty>" if not text else "*" * min(len(text), 12)
    return text or "<empty>"


def fit(text: str, width: int) -> str:
    if width <= 0:
        return ""
    if len(text) <= width:
        return text
    if width <= 3:
        return text[:width]
    return text[: width - 3] + "..."


def safe_addstr(stdscr, y: int, x: int, text: str, attr: int = 0) -> None:
    height, width = stdscr.getmaxyx()
    if y < 0 or y >= height or x < 0 or x >= width:
        return
    try:
        stdscr.addstr(y, x, fit(text, width - x), attr)
    except curses.error:
        pass


def draw_panel(
    stdscr,
    y: int,
    x: int,
    width: int,
    title: str,
    fields: list[tuple[str, str, bool]],
    obj: Any,
    selected: int,
    active: bool,
) -> int:
    attr = curses.A_BOLD | (curses.color_pair(2) if active else curses.color_pair(1))
    safe_addstr(stdscr, y, x, fit(title, width), attr)
    y += 1
    for idx, (key, label, secret) in enumerate(fields):
        value = display_value(getattr(obj, key), secret=secret)
        line = f"{label}: {value}"
        row_attr = curses.A_REVERSE if active and idx == selected else curses.A_NORMAL
        safe_addstr(stdscr, y + idx, x, fit(line.ljust(width), width), row_attr)
    return y + len(fields)


def draw(stdscr, state: TuiState) -> None:
    stdscr.erase()
    height, width = stdscr.getmaxyx()
    header = (
        f"VibeBox TUI | BLE {state.ble_status} | "
        f"device {state.host_config.device_name} | press-return {display_value(state.host_config.press_return)}"
    )
    safe_addstr(stdscr, 0, 0, header.ljust(width), curses.A_REVERSE)

    left_width = min(max(38, width // 2), max(1, width))
    log_x = min(left_width + 2, width - 1)
    draw_panel(
        stdscr,
        2,
        0,
        left_width,
        "Host Settings",
        HOST_FIELDS,
        state.host_config,
        state.selected[0],
        state.active_panel == 0,
    )
    device_y = 2 + len(HOST_FIELDS) + 3
    draw_panel(
        stdscr,
        device_y,
        0,
        left_width,
        "Device Config",
        DEVICE_FIELDS,
        state.device_config,
        state.selected[1],
        state.active_panel == 1,
    )

    if log_x < width - 12:
        safe_addstr(stdscr, 2, log_x, "Logs", curses.A_BOLD | curses.color_pair(1))
        log_height = max(1, height - 7)
        recent_logs = list(state.logs)[-log_height:]
        for idx, line in enumerate(recent_logs):
            safe_addstr(stdscr, 3 + idx, log_x, line)

    transcript = state.last_transcript or "<none>"
    safe_addstr(stdscr, height - 3, 0, f"Last transcript: {transcript}", curses.color_pair(1))
    help_line = (
        "Tab switch | Up/Down select | Enter edit/toggle | b BLE start/stop | "
        "f fetch device | s save device | p press-return | q quit"
    )
    safe_addstr(stdscr, height - 2, 0, help_line, curses.A_REVERSE)
    stdscr.refresh()


def prompt(stdscr, label: str, initial: str = "", secret: bool = False) -> str | None:
    height, width = stdscr.getmaxyx()
    value = initial
    stdscr.nodelay(False)
    curses.curs_set(1)
    try:
        while True:
            shown = "*" * len(value) if secret else value
            line = fit(f"{label}: {shown}", width - 1)
            stdscr.move(height - 1, 0)
            stdscr.clrtoeol()
            safe_addstr(stdscr, height - 1, 0, line)
            stdscr.refresh()
            key = stdscr.get_wch()
            if key in ("\n", "\r"):
                return value
            if key == "\x1b":
                return None
            if key in ("\b", "\x7f", curses.KEY_BACKSPACE):
                value = value[:-1]
            elif isinstance(key, str) and key.isprintable():
                value += key
    finally:
        curses.curs_set(0)
        stdscr.nodelay(True)


def edit_host_field(stdscr, state: TuiState) -> None:
    key, label, secret = HOST_FIELDS[state.selected[0]]
    current = getattr(state.host_config, key)
    if isinstance(current, bool):
        setattr(state.host_config, key, not current)
        state.host_config.save()
        state.log(f"{label} set to {display_value(getattr(state.host_config, key))}")
        return

    initial = ", ".join(current) if isinstance(current, list) else str(current)
    next_value = prompt(stdscr, label, initial, secret=secret)
    if next_value is None:
        return

    try:
        if key == "char_uuids":
            parsed = [item.strip() for item in next_value.split(",") if item.strip()]
            if not parsed:
                raise ValueError("at least one characteristic UUID is required")
            setattr(state.host_config, key, parsed)
        elif key in {"scan_timeout", "retry_delay"}:
            parsed_float = float(next_value)
            if parsed_float <= 0:
                raise ValueError("value must be positive")
            setattr(state.host_config, key, parsed_float)
        else:
            setattr(state.host_config, key, next_value.strip())
        state.host_config.save()
        state.log(f"{label} updated")
    except ValueError as exc:
        state.log(f"Invalid {label}: {exc}")


def edit_device_field(stdscr, state: TuiState) -> None:
    key, label, secret = DEVICE_FIELDS[state.selected[1]]
    current = getattr(state.device_config, key)
    if isinstance(current, bool):
        setattr(state.device_config, key, not current)
        state.log(f"{label} staged: {display_value(getattr(state.device_config, key))}")
        return

    next_value = prompt(stdscr, label, str(current), secret=secret)
    if next_value is None:
        return

    try:
        if key == "recording_duration_ms":
            parsed = int(next_value)
            if parsed < 1000 or parsed > 15000:
                raise ValueError("must be between 1000 and 15000")
            setattr(state.device_config, key, parsed)
        else:
            setattr(state.device_config, key, next_value.strip())
        state.log(f"{label} staged")
    except ValueError as exc:
        state.log(f"Invalid {label}: {exc}")


async def paste_text(state: TuiState, text: str) -> None:
    if not text:
        state.log("received empty transcript")
        return
    try:
        await asyncio.to_thread(type_text_macos, text, state.host_config.press_return)
        state.log(f"pasted {len(text)} chars")
    except Exception as exc:  # noqa: BLE001 - paste failures should stay visible in the TUI.
        state.log(f"paste failed: {exc}")


def config_packets_for_save(cfg: DeviceConfig) -> list[bytes]:
    validate_device_config(cfg)
    payload = json.dumps(cfg.as_ble_payload(), separators=(",", ":")).encode("utf-8")
    packets = [bytes([CONFIG_OPCODE_SAVE_BEGIN])]
    for idx in range(0, len(payload), BLE_PACKET_PAYLOAD_MAX):
        packets.append(bytes([CONFIG_OPCODE_SAVE_CHUNK]) + payload[idx : idx + BLE_PACKET_PAYLOAD_MAX])
    packets.append(bytes([CONFIG_OPCODE_SAVE_END]))
    return packets


async def send_config_packets_with_active_client(state: TuiState, packets: list[bytes]) -> tuple[bool, str]:
    if state.ble_client is None or not state.ble_client.is_connected:
        raise RuntimeError("BLE bridge is not connected")
    if not state.ble_config_ready:
        raise RuntimeError("BLE config characteristic is not subscribed")
    if state.config_response_future is not None and not state.config_response_future.done():
        raise RuntimeError("config request already in progress")

    loop = asyncio.get_running_loop()
    state.config_response_future = loop.create_future()
    try:
        for packet in packets:
            await state.ble_client.write_gatt_char(state.host_config.config_char_uuid, packet, response=True)
            await asyncio.sleep(0.01)
        return await asyncio.wait_for(state.config_response_future, BLE_CONFIG_TIMEOUT_SECONDS)
    except BleakError as exc:
        state.ble_client = None
        state.ble_config_ready = False
        state.ble_status = "waiting"
        raise RuntimeError(f"BLE disconnected during config transaction: {exc}") from exc
    finally:
        state.config_response_future = None


async def send_config_packets_with_temp_client(
    host_config: HostConfig,
    packets: list[bytes],
    log,
) -> tuple[bool, str]:
    loop = asyncio.get_running_loop()
    response_future = loop.create_future()

    def on_response(ok: bool, payload: str) -> None:
        if not response_future.done():
            response_future.set_result((ok, payload))

    assembler = ConfigResponseAssembler(on_response)
    log(f"scanning for {host_config.device_name}")
    device = await find_device(host_config.device_name, host_config.scan_timeout)
    if device is None:
        raise RuntimeError("device not found")

    log(f"connecting to {device.address} for config")
    async with BleakClient(device) as client:
        await client.start_notify(host_config.config_char_uuid, lambda _sender, data: assembler.handle_packet(data))
        for packet in packets:
            await client.write_gatt_char(host_config.config_char_uuid, packet, response=True)
            await asyncio.sleep(0.01)
        return await asyncio.wait_for(response_future, BLE_CONFIG_TIMEOUT_SECONDS)


async def ble_config_transaction(state: TuiState, packets: list[bytes]) -> tuple[bool, str]:
    try:
        if state.ble_client is not None and state.ble_client.is_connected:
            return await send_config_packets_with_active_client(state, packets)
        return await send_config_packets_with_temp_client(state.host_config, packets, state.log)
    except BleakError as exc:
        state.ble_client = None
        state.ble_config_ready = False
        state.ble_status = "waiting"
        raise RuntimeError(f"BLE config transaction failed: {exc}") from exc


async def fetch_device_config_ble(state: TuiState) -> DeviceConfig:
    ok, payload = await ble_config_transaction(state, [bytes([CONFIG_OPCODE_GET])])
    if not ok:
        raise RuntimeError(payload or "device rejected config fetch")
    return device_config_from_json(payload)


async def save_device_config_ble(state: TuiState) -> str:
    ok, payload = await ble_config_transaction(state, config_packets_for_save(state.device_config))
    if not ok:
        raise RuntimeError(payload or "device rejected config save")
    return payload


async def ble_bridge(state: TuiState) -> None:
    assert state.ble_stop_event is not None
    loop = asyncio.get_running_loop()

    def on_text(text: str, is_error: bool = False) -> None:
        if is_error:
            state.log(text)
            return
        state.last_transcript = text
        state.log(f"received: {text}")
        loop.create_task(paste_text(state, text))

    def on_config_response(ok: bool, payload: str) -> None:
        if state.config_response_future is not None and not state.config_response_future.done():
            state.config_response_future.set_result((ok, payload))
        else:
            state.log(f"config response: {payload}")

    assembler = TextAssembler(on_text)
    config_assembler = ConfigResponseAssembler(on_config_response)

    while not state.ble_stop_event.is_set():
        state.ble_status = "scanning"
        state.log(f"scanning for {state.host_config.device_name}")
        device = await find_device(state.host_config.device_name, state.host_config.scan_timeout)
        if device is None:
            state.log("device not found")
            state.ble_status = "waiting"
            await asyncio.sleep(state.host_config.retry_delay)
            continue

        state.ble_status = f"connecting {device.address}"
        disconnected = asyncio.Event()

        def on_disconnect(_client: BleakClient) -> None:
            state.log("device disconnected")
            disconnected.set()

        try:
            async with BleakClient(device, disconnected_callback=on_disconnect) as client:
                state.ble_client = client
                state.ble_config_ready = False
                notify_started = False
                config_notify_started = False
                last_error: Exception | None = None
                for char_uuid in state.host_config.char_uuids:
                    try:
                        await client.start_notify(
                            char_uuid,
                            lambda _sender, data: assembler.handle_packet(data),
                        )
                        notify_started = True
                        state.ble_status = "connected"
                        state.log(f"listening on {char_uuid}")
                        break
                    except Exception as exc:  # noqa: BLE001 - show all subscription failures.
                        last_error = exc
                        state.log(f"subscribe failed {char_uuid}: {exc}")

                if not notify_started:
                    raise RuntimeError(last_error or "no characteristic subscribed")

                try:
                    await client.start_notify(
                        state.host_config.config_char_uuid,
                        lambda _sender, data: config_assembler.handle_packet(data),
                    )
                    config_notify_started = True
                    state.ble_config_ready = True
                    state.log(f"config BLE ready on {state.host_config.config_char_uuid}")
                except Exception as exc:  # noqa: BLE001 - config can be absent on older firmware.
                    state.log(f"config subscribe failed: {exc}")

                if notify_started or config_notify_started:
                    state.ble_status = "connected"

                while not state.ble_stop_event.is_set() and not disconnected.is_set():
                    await asyncio.sleep(0.1)
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001 - keep the bridge retrying.
            state.log(f"BLE loop failed: {exc}")
        finally:
            state.ble_client = None
            state.ble_config_ready = False

        if not state.ble_stop_event.is_set():
            state.ble_status = "waiting"
            await asyncio.sleep(state.host_config.retry_delay)

    state.ble_status = "stopped"


def move_selection(state: TuiState, delta: int) -> None:
    fields = HOST_FIELDS if state.active_panel == 0 else DEVICE_FIELDS
    idx = state.active_panel
    state.selected[idx] = (state.selected[idx] + delta) % len(fields)


async def toggle_ble(state: TuiState) -> None:
    if state.ble_task and not state.ble_task.done():
        assert state.ble_stop_event is not None
        state.ble_stop_event.set()
        state.ble_task.cancel()
        try:
            await state.ble_task
        except asyncio.CancelledError:
            pass
        state.ble_status = "stopped"
        state.log("BLE bridge stopped")
        return

    state.ble_stop_event = asyncio.Event()
    state.ble_task = asyncio.create_task(ble_bridge(state))
    state.log("BLE bridge started")


async def handle_key(stdscr, state: TuiState, key: int | str) -> bool:
    if key in (ord("q"), "q"):
        return False
    if key in (9, "\t"):
        state.active_panel = 1 - state.active_panel
    elif key in (curses.KEY_UP, "k"):
        move_selection(state, -1)
    elif key in (curses.KEY_DOWN, "j"):
        move_selection(state, 1)
    elif key in (ord("\n"), "\n", "\r", curses.KEY_ENTER, "e"):
        if state.active_panel == 0:
            edit_host_field(stdscr, state)
        else:
            edit_device_field(stdscr, state)
    elif key in (ord("b"), "b"):
        await toggle_ble(state)
    elif key in (ord("p"), "p"):
        state.host_config.press_return = not state.host_config.press_return
        state.host_config.save()
        state.log(f"Press return set to {display_value(state.host_config.press_return)}")
    elif key in (ord("f"), "f"):
        state.log("fetch device config over BLE...")
        try:
            state.device_config = await fetch_device_config_ble(state)
            state.log("fetch device config done")
        except (RuntimeError, ValueError, json.JSONDecodeError, TimeoutError, asyncio.TimeoutError) as exc:
            state.log(f"fetch device config failed: {exc}")
    elif key in (ord("s"), "s"):
        state.log("save device config over BLE...")
        try:
            response = await save_device_config_ble(state)
            state.log(f"save device config done: {response or '<empty response>'}")
        except (RuntimeError, ValueError, json.JSONDecodeError, TimeoutError, asyncio.TimeoutError) as exc:
            state.log(f"save device config failed: {exc}")
    elif key in (ord("l"), "l"):
        state.logs.clear()
    return True


async def main_curses(stdscr) -> int:
    curses.curs_set(0)
    if curses.has_colors():
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_CYAN, -1)
        curses.init_pair(2, curses.COLOR_GREEN, -1)
    stdscr.nodelay(True)
    stdscr.keypad(True)

    state = TuiState(host_config=HostConfig.load())
    state.log(f"config: {CONFIG_PATH}")
    state.log("press f to fetch device config over BLE before editing secrets")

    running = True
    while running:
        draw(stdscr, state)
        try:
            key = stdscr.get_wch()
        except curses.error:
            await asyncio.sleep(0.08)
            continue
        running = await handle_key(stdscr, state, key)

    if state.ble_task and not state.ble_task.done():
        assert state.ble_stop_event is not None
        state.ble_stop_event.set()
        state.ble_task.cancel()
        try:
            await state.ble_task
        except asyncio.CancelledError:
            pass
    return 0


def main() -> int:
    logging.basicConfig(level=logging.WARNING)
    if sys.platform != "darwin":
        print("vibebox_tui.py currently supports macOS only", file=sys.stderr)
        return 2
    return curses.wrapper(lambda stdscr: asyncio.run(main_curses(stdscr)))


if __name__ == "__main__":
    raise SystemExit(main())
