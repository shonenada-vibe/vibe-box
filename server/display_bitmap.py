from __future__ import annotations

import json
import logging
import subprocess
from pathlib import Path

logger = logging.getLogger("vibe_box.server.display_bitmap")

FRAME_WIDTH = 200
FRAME_HEIGHT = 200
FRAME_BYTES = (FRAME_WIDTH // 8) * FRAME_HEIGHT

SERVER_DIR = Path(__file__).resolve().parent
CACHE_DIR = SERVER_DIR / ".cache"
SWIFT_SOURCE = SERVER_DIR / "render_epaper.swift"
SWIFT_HELPER = CACHE_DIR / "render_epaper_helper"
SWIFT_MODULE_CACHE = Path("/tmp/vibe-box-swift-module-cache")


def _ensure_swift_helper() -> Path:
    if not SWIFT_SOURCE.exists():
        raise FileNotFoundError(f"missing swift renderer: {SWIFT_SOURCE}")

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    SWIFT_MODULE_CACHE.mkdir(parents=True, exist_ok=True)

    if SWIFT_HELPER.exists() and SWIFT_HELPER.stat().st_mtime >= SWIFT_SOURCE.stat().st_mtime:
        return SWIFT_HELPER

    subprocess.run(
        [
            "swiftc",
            "-O",
            "-module-cache-path",
            str(SWIFT_MODULE_CACHE),
            str(SWIFT_SOURCE),
            "-o",
            str(SWIFT_HELPER),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return SWIFT_HELPER


def render_display_bitmap_hex(lines: list[str]) -> str:
    normalized_lines = [line.strip() for line in lines if line and line.strip()]
    if not normalized_lines:
        return ""

    try:
        helper = _ensure_swift_helper()
        payload = json.dumps(
            {
                "width": FRAME_WIDTH,
                "height": FRAME_HEIGHT,
                "lines": normalized_lines[:4],
            },
            ensure_ascii=False,
        ).encode("utf-8")
        proc = subprocess.run(
            [str(helper)],
            input=payload,
            capture_output=True,
            check=True,
        )
    except Exception as exc:
        logger.warning("display bitmap render unavailable: %s", exc)
        return ""

    bitmap = proc.stdout
    if len(bitmap) != FRAME_BYTES:
        logger.warning(
            "display bitmap size mismatch: got=%d expected=%d stderr=%r",
            len(bitmap),
            FRAME_BYTES,
            proc.stderr.decode("utf-8", errors="replace"),
        )
        return ""

    return bitmap.hex()
