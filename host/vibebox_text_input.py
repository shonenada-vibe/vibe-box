#!/usr/bin/env python3
"""Receive VibeBox BLE text notifications and paste them into the active macOS app."""

from __future__ import annotations

import argparse
import asyncio
import logging
import subprocess
import sys
from dataclasses import dataclass, field

from bleak import BleakClient, BleakScanner


DEFAULT_DEVICE_NAME = "VibeBox"
DEFAULT_TEXT_CHAR_UUIDS = (
    "48f2d101-7a15-4b3f-8d67-60587f5d1002",
)

OPCODE_BEGIN = 0x01
OPCODE_CHUNK = 0x02
OPCODE_END = 0x03


def type_text_macos(text: str, press_return: bool) -> None:
    if not text:
        logging.info("text is empty; nothing to paste")
        return

    script = """
on run argv
	set typedText to item 1 of argv
	set submitFlag to item 2 of argv
	set the clipboard to typedText
	tell application "System Events"
		keystroke "v" using command down
		if submitFlag is "1" then
			key code 36
		end if
	end tell
end run
"""

    subprocess.run(
        ["osascript", "-e", script, "--", text, "1" if press_return else "0"],
        check=True,
    )
    logging.info("pasted %d characters", len(text))


@dataclass
class TextAssembler:
    press_return: bool
    chunks: bytearray = field(default_factory=bytearray)

    def handle_packet(self, data: bytearray) -> None:
        if not data:
            return

        opcode = data[0]
        payload = bytes(data[1:])

        if opcode == OPCODE_BEGIN:
            self.chunks.clear()
            return

        if opcode == OPCODE_CHUNK:
            self.chunks.extend(payload)
            return

        if opcode == OPCODE_END:
            text = self.chunks.decode("utf-8", errors="replace").strip()
            self.chunks.clear()
            logging.info("received text: %s", text)
            type_text_macos(text, self.press_return)
            return

        logging.warning("unknown text packet opcode: 0x%02x", opcode)


async def find_device(device_name: str, timeout: float):
    def matches(device, advertisement_data) -> bool:
        return device.name == device_name or advertisement_data.local_name == device_name

    return await BleakScanner.find_device_by_filter(matches, timeout=timeout)


async def run_once(args: argparse.Namespace) -> bool:
    logging.info("scanning for %s", args.device_name)
    device = await find_device(args.device_name, args.scan_timeout)
    if device is None:
        logging.warning("device not found")
        return False

    logging.info("connecting to %s (%s)", args.device_name, device.address)
    assembler = TextAssembler(press_return=args.press_return)
    disconnected = asyncio.Event()

    def on_disconnect(_client: BleakClient) -> None:
        logging.warning("device disconnected")
        disconnected.set()

    async with BleakClient(device, disconnected_callback=on_disconnect) as client:
        notify_started = False
        last_error: Exception | None = None
        for char_uuid in args.char_uuid:
            try:
                await client.start_notify(char_uuid, lambda _sender, data: assembler.handle_packet(data))
                logging.info("listening for text notifications on %s", char_uuid)
                notify_started = True
                break
            except Exception as exc:
                last_error = exc
                logging.warning("failed to subscribe %s: %s", char_uuid, exc)

        if not notify_started:
            assert last_error is not None
            raise last_error

        await disconnected.wait()
        return True


async def run(args: argparse.Namespace) -> int:
    while True:
        try:
            connected = await run_once(args)
            if args.once:
                return 0 if connected else 1
        except KeyboardInterrupt:
            return 130
        except Exception:
            logging.exception("BLE text input loop failed")
            if args.once:
                return 1

        await asyncio.sleep(args.retry_delay)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-name", default=DEFAULT_DEVICE_NAME)
    parser.add_argument(
        "--char-uuid",
        action="append",
        default=None,
        help="Text notification characteristic UUID. Can be passed multiple times.",
    )
    parser.add_argument("--scan-timeout", type=float, default=8.0)
    parser.add_argument("--retry-delay", type=float, default=2.0)
    parser.add_argument("--press-return", action="store_true")
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.char_uuid is None:
        args.char_uuid = list(DEFAULT_TEXT_CHAR_UUIDS)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    if sys.platform != "darwin":
        logging.error("this input helper currently supports macOS only")
        return 2

    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())
