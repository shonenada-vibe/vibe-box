from __future__ import annotations

import logging
import os
from pathlib import Path
from uuid import uuid4

from fastapi import FastAPI, File, Form, Header, HTTPException, UploadFile

from display_bitmap import render_display_bitmap_hex
from schemas import QueryResponse
from provider_adapter import fake_transcribe
from response_shaper import build_display_lines, build_reply_text
from tts_proxy import build_speak_text

logging.basicConfig(
    level=os.environ.get("VIBE_BOX_LOG_LEVEL", "INFO").upper(),
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)

app = FastAPI(title="vibe-box-server", version="0.1.0")
logger = logging.getLogger("vibe_box.server")
logger.setLevel(logging.INFO)

AUDIO_CACHE_DIR = Path(__file__).resolve().parent / ".cache"


def _check_auth(authorization: str | None) -> None:
    expected_token = os.environ.get("VIBE_BOX_DEVICE_TOKEN", "").strip()
    if not expected_token:
        return

    expected_header = f"Bearer {expected_token}"
    if authorization != expected_header:
        raise HTTPException(status_code=401, detail="invalid device token")


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.post("/v1/query", response_model=QueryResponse)
async def query(
    device_id: str = Form(default="unknown-device"),
    firmware_version: str = Form(default="dev"),
    session_id: str = Form(default=""),
    temperature: str = Form(default=""),
    humidity: str = Form(default=""),
    battery_level: str = Form(default=""),
    language: str = Form(default="zh"),
    recording_duration_ms: int = Form(default=0),
    query_text: str = Form(default=""),
    audio_format: str = Form(default="wav"),
    audio: UploadFile | None = File(default=None),
    authorization: str | None = Header(default=None),
) -> QueryResponse:
    _check_auth(authorization)

    audio_bytes = b""
    if audio is not None:
        audio_bytes = await audio.read()

    request_id = str(uuid4())
    logger.info(
        "query request_id=%s device_id=%s firmware=%s session_id=%s language=%s recording_duration_ms=%d audio_format=%s audio_bytes=%d auth=%s",
        request_id,
        device_id,
        firmware_version,
        session_id,
        language,
        recording_duration_ms,
        audio_format,
        len(audio_bytes),
        "present" if authorization else "missing",
    )

    if audio_bytes:
        AUDIO_CACHE_DIR.mkdir(parents=True, exist_ok=True)
        cache_path = AUDIO_CACHE_DIR / f"audio-{request_id}.wav"
        cache_path.write_bytes(audio_bytes)
        logger.info("query request_id=%s saved audio to %s", request_id, cache_path)

    transcript = fake_transcribe(
        query_text=query_text,
        filename=audio.filename if audio else None,
        audio_bytes=audio_bytes if audio else None,
        audio_size=len(audio_bytes) if audio else None,
        language=language,
    )
    reply_text = build_reply_text(
        transcript,
        device_id=device_id,
        temperature=temperature,
        humidity=humidity,
    )
    display_lines = build_display_lines(
        transcript,
        reply_text,
        temperature=temperature,
        humidity=humidity,
    )
    display_bitmap_hex = render_display_bitmap_hex(display_lines or [reply_text])

    logger.info(
        "query response request_id=%s transcript=%r display_lines=%s bitmap=%s",
        request_id,
        transcript,
        display_lines,
        "yes" if display_bitmap_hex else "no",
    )

    return QueryResponse(
        request_id=request_id,
        transcript=transcript,
        reply_text=reply_text,
        display_lines=display_lines,
        display_bitmap_hex=display_bitmap_hex,
        speak_text=build_speak_text(reply_text),
        refresh_mode="partial",
        cache_ttl_s=60,
    )
