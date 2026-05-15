#!/usr/bin/env python3
"""Read or write VibeBox device configuration over BLE from the command line."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

DEFAULT_DEVICE_NAME = "VibeBox"
DEFAULT_HOST_CONFIG_PATH = Path(
    os.environ.get("VIBEBOX_TUI_CONFIG", "~/.config/vibebox/tui.json")
).expanduser()
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

DEVICE_DEFAULTS: dict[str, str | int | bool | list] = {
    "wifi_list": [],
    "whisper_api_url": "",
    "whisper_api_key": "",
    "stt_model": "whisper-large-v3-turbo",
    "openai_api_base": "",
    "openai_api_key": "",
    "translation_model": "gpt-4o-mini",
    "translation_target_language": "English",
    "translation_prompt": DEFAULT_TRANSLATION_PROMPT,
    "translation_enabled": False,
    "refine_prompt": DEFAULT_REFINE_PROMPT,
    "refine_enabled": False,
    "volc_tts_appid": "",
    "volc_tts_api_key": "",
    "volc_tts_voice_type": "",
    "volc_tts_cluster": "volcano_tts",
    "tts_enabled": False,
    "tts_volume_percent": 100,
    "tts_muted": False,
    "device_id": "vibe-box-dev",
    "firmware_version": "dev",
    "language": "zh",
    "recording_duration_ms": 3000,
}

DEVICE_FIELD_LIMITS = {
    "whisper_api_url": 255,
    "whisper_api_key": 255,
    "stt_model": 95,
    "openai_api_base": 255,
    "openai_api_key": 255,
    "translation_model": 95,
    "translation_target_language": 31,
    "translation_prompt": 511,
    "refine_prompt": 511,
    "volc_tts_appid": 95,
    "volc_tts_api_key": 255,
    "volc_tts_voice_type": 95,
    "volc_tts_cluster": 63,
    "device_id": 63,
    "firmware_version": 63,
    "language": 15,
}


@dataclass
class HostConfig:
    device_name: str = DEFAULT_DEVICE_NAME
    config_char_uuid: str = DEFAULT_CONFIG_CHAR_UUID
    scan_timeout: float = 8.0
    response_timeout: float = 12.0
    write_delay: float = 0.01

    @classmethod
    def load(cls, path: Path = DEFAULT_HOST_CONFIG_PATH) -> "HostConfig":
        cfg = cls()
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return cfg
        except (OSError, json.JSONDecodeError) as exc:
            logging.warning("failed to load host config %s: %s", path, exc)
            return cfg

        if isinstance(raw.get("device_name"), str) and raw["device_name"].strip():
            cfg.device_name = raw["device_name"].strip()
        if isinstance(raw.get("config_char_uuid"), str) and raw["config_char_uuid"].strip():
            cfg.config_char_uuid = raw["config_char_uuid"].strip()
        if isinstance(raw.get("scan_timeout"), (int, float)) and raw["scan_timeout"] > 0:
            cfg.scan_timeout = float(raw["scan_timeout"])
        return cfg


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


def load_ble_helpers():
    try:
        from bleak import BleakClient
        from bleak.exc import BleakError
    except ImportError as exc:
        raise RuntimeError("missing Python dependency: install host/requirements.txt first") from exc

    try:
        from vibebox_text_input import find_device
    except ImportError:  # pragma: no cover - supports running from the repo root.
        from host.vibebox_text_input import find_device

    return BleakClient, BleakError, find_device


def extract_device_payload(raw: Any) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise ValueError("config file must contain a JSON object")
    for key in ("device_config", "device"):
        nested = raw.get(key)
        if isinstance(nested, dict):
            return dict(nested)
    return dict(raw)


def load_device_config_file(path: Path) -> dict[str, Any]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"config file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid JSON in {path}: {exc}") from exc
    except OSError as exc:
        raise ValueError(f"failed to read {path}: {exc}") from exc
    return extract_device_payload(raw)


def normalize_device_config(values: dict[str, Any], *, require_complete: bool = True) -> dict[str, str | int | bool]:
    unknown = sorted(set(values) - set(DEVICE_DEFAULTS))
    if unknown:
        raise ValueError(f"unknown device config field(s): {', '.join(unknown)}")

    normalized = dict(DEVICE_DEFAULTS)
    normalized.update(values)

    for key, default in DEVICE_DEFAULTS.items():
        value = normalized[key]
        if isinstance(default, bool):
            if isinstance(value, bool):
                normalized[key] = value
            elif isinstance(value, str) and value.lower() in {"true", "1", "yes", "on"}:
                normalized[key] = True
            elif isinstance(value, str) and value.lower() in {"false", "0", "no", "off"}:
                normalized[key] = False
            else:
                raise ValueError(f"{key} must be a boolean")
        elif isinstance(default, int):
            try:
                normalized[key] = int(value)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"{key} must be an integer") from exc
        else:
            normalized[key] = "" if value is None else str(value)

    validate_device_config(normalized, require_complete=require_complete)
    return normalized


def validate_device_config(cfg: dict[str, str | int | bool | list], *, require_complete: bool = True) -> None:
    if require_complete:
        wifi_list = cfg.get("wifi_list", [])
        if not wifi_list or not any(isinstance(w, dict) and w.get("ssid", "").strip() for w in wifi_list):
            raise ValueError("at least one wifi network is required")
        if not str(cfg["whisper_api_url"]).strip():
            raise ValueError("whisper_api_url is required")

    recording_ms = int(cfg["recording_duration_ms"])
    if recording_ms < 1000 or recording_ms > 15000:
        raise ValueError("recording_duration_ms must be between 1000 and 15000")
    tts_volume = int(cfg["tts_volume_percent"])
    if tts_volume < 0 or tts_volume > 100:
        raise ValueError("tts_volume_percent must be between 0 and 100")

    for key, limit in DEVICE_FIELD_LIMITS.items():
        value = str(cfg[key])
        if len(value.encode("utf-8")) > limit:
            raise ValueError(f"{key} is too long; max {limit} bytes")


def config_packets_for_save(cfg: dict[str, str | int | bool]) -> list[bytes]:
    validate_device_config(cfg)
    payload = json.dumps(cfg, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    packets = [bytes([CONFIG_OPCODE_SAVE_BEGIN])]
    for idx in range(0, len(payload), BLE_PACKET_PAYLOAD_MAX):
        packets.append(bytes([CONFIG_OPCODE_SAVE_CHUNK]) + payload[idx : idx + BLE_PACKET_PAYLOAD_MAX])
    packets.append(bytes([CONFIG_OPCODE_SAVE_END]))
    return packets


async def config_transaction(host: HostConfig, packets: list[bytes]) -> tuple[bool, str]:
    BleakClient, BleakError, find_device = load_ble_helpers()
    loop = asyncio.get_running_loop()
    response_future = loop.create_future()
    disconnected = asyncio.Event()

    def on_response(ok: bool, payload: str) -> None:
        if not response_future.done():
            response_future.set_result((ok, payload))

    def on_disconnect(_client) -> None:
        disconnected.set()
        if not response_future.done():
            response_future.set_exception(RuntimeError("BLE device disconnected"))

    assembler = ConfigResponseAssembler(on_response)
    logging.info("scanning for %s", host.device_name)
    device = await find_device(host.device_name, host.scan_timeout)
    if device is None:
        raise RuntimeError(f"device not found: {host.device_name}")

    logging.info("connecting to %s (%s)", host.device_name, device.address)
    async with BleakClient(device, disconnected_callback=on_disconnect) as client:
        await client.start_notify(host.config_char_uuid, lambda _sender, data: assembler.handle_packet(data))
        try:
            for packet in packets:
                if disconnected.is_set():
                    raise RuntimeError("BLE device disconnected")
                await client.write_gatt_char(host.config_char_uuid, packet, response=True)
                await asyncio.sleep(host.write_delay)
            return await asyncio.wait_for(response_future, host.response_timeout)
        finally:
            try:
                await client.stop_notify(host.config_char_uuid)
            except BleakError:
                pass


async def fetch_device_config(host: HostConfig) -> dict[str, str | int | bool]:
    ok, payload = await config_transaction(host, [bytes([CONFIG_OPCODE_GET])])
    if not ok:
        raise RuntimeError(payload or "device rejected config fetch")
    try:
        values = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"device returned invalid JSON: {exc}") from exc
    return normalize_device_config(extract_device_payload(values), require_complete=False)


async def save_device_config(host: HostConfig, cfg: dict[str, str | int | bool]) -> str:
    ok, payload = await config_transaction(host, config_packets_for_save(cfg))
    if not ok:
        raise RuntimeError(payload or "device rejected config save")
    return payload


def print_json(payload: dict[str, Any], compact: bool = False) -> None:
    if compact:
        print(json.dumps(payload, separators=(",", ":"), ensure_ascii=False))
    else:
        print(json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False))


def apply_host_overrides(args: argparse.Namespace) -> HostConfig:
    host = HostConfig.load(args.host_config)
    if args.device_name:
        host.device_name = args.device_name
    if args.config_char_uuid:
        host.config_char_uuid = args.config_char_uuid
    if args.scan_timeout is not None:
        host.scan_timeout = args.scan_timeout
    if args.response_timeout is not None:
        host.response_timeout = args.response_timeout
    return host


async def cmd_get(args: argparse.Namespace) -> int:
    host = apply_host_overrides(args)
    cfg = await fetch_device_config(host)
    print_json(cfg, compact=args.compact)
    return 0


async def cmd_write(args: argparse.Namespace) -> int:
    host = apply_host_overrides(args)
    file_values = load_device_config_file(args.config_file)

    if args.replace:
        cfg = normalize_device_config(file_values)
    else:
        logging.info("fetching current device config for merge")
        current = await fetch_device_config(host)
        merged = dict(current)
        merged.update(file_values)
        cfg = normalize_device_config(merged)

    if args.dry_run:
        print_json(cfg, compact=args.compact)
        return 0

    response = await save_device_config(host, cfg)
    if args.print_payload:
        print_json(cfg, compact=args.compact)
    print(response or "{\"ok\":true}")
    return 0


def cmd_template(args: argparse.Namespace) -> int:
    payload = {
        "wifi_list": [
            {"ssid": "Your Wi-Fi SSID", "password": "Your Wi-Fi password"},
        ],
    }
    if args.full:
        payload = dict(DEVICE_DEFAULTS)
        payload["wifi_list"] = [
            {"ssid": "Your Wi-Fi SSID", "password": "Your Wi-Fi password"},
        ]
        payload["whisper_api_url"] = "https://api.openai.com/v1/audio/transcriptions"
    print_json(payload, compact=args.compact)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-config", type=Path, default=DEFAULT_HOST_CONFIG_PATH)
    parser.add_argument("--device-name", help=f"BLE device name, default from host config or {DEFAULT_DEVICE_NAME}")
    parser.add_argument("--config-char-uuid", help=f"Config characteristic UUID, default {DEFAULT_CONFIG_CHAR_UUID}")
    parser.add_argument("--scan-timeout", type=float, help="BLE scan timeout in seconds")
    parser.add_argument("--response-timeout", type=float, help="Config response timeout in seconds")
    parser.add_argument("--verbose", action="store_true")

    subparsers = parser.add_subparsers(dest="command", required=True)

    get_parser = subparsers.add_parser("get", help="Read current config from the device")
    get_parser.add_argument("--compact", action="store_true")
    get_parser.set_defaults(func=cmd_get)

    write_parser = subparsers.add_parser("write", help="Merge a JSON file into device config and save over BLE")
    write_parser.add_argument("config_file", type=Path)
    write_parser.add_argument(
        "--replace",
        action="store_true",
        help="Do not fetch current config first; the file must contain a complete valid config",
    )
    write_parser.add_argument("--dry-run", action="store_true", help="Print the final payload without BLE write")
    write_parser.add_argument("--print-payload", action="store_true", help="Print final payload after a successful write")
    write_parser.add_argument("--compact", action="store_true")
    write_parser.set_defaults(func=cmd_write)

    template_parser = subparsers.add_parser("template", help="Print an example JSON config file")
    template_parser.add_argument("--full", action="store_true", help="Print every supported device field")
    template_parser.add_argument("--compact", action="store_true")
    template_parser.set_defaults(func=cmd_template)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    try:
        result = args.func(args)
        if asyncio.iscoroutine(result):
            return asyncio.run(result)
        return int(result)
    except KeyboardInterrupt:
        return 130
    except (RuntimeError, ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
