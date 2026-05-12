from __future__ import annotations

import logging
import os
from pathlib import Path
from uuid import uuid4

from fastapi import FastAPI, File, Form, Header, HTTPException, UploadFile

from display_bitmap import render_display_bitmap_hex
from schemas import QueryResponse
from provider_adapter import transcribe_audio
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
    language: str = Form(default="zh"),
    recording_duration_ms: int = Form(default=0),
    audio_format: str = Form(default="wav"),
    audio: UploadFile = File(...),
    authorization: str | None = Header(default=None),
) -> QueryResponse:
    _check_auth(authorization)

    audio_bytes = await audio.read()
    if not audio_bytes:
        raise HTTPException(status_code=400, detail="audio file is empty")

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

    AUDIO_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cache_path = AUDIO_CACHE_DIR / f"audio-{request_id}.wav"
    cache_path.write_bytes(audio_bytes)
    logger.info("query request_id=%s saved audio to %s", request_id, cache_path)

    try:
        transcript = transcribe_audio(
            filename=audio.filename or "recording.wav",
            audio_bytes=audio_bytes,
            language=language,
        )
    except RuntimeError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    reply_text = build_reply_text(transcript)
    display_lines = build_display_lines(transcript, reply_text)
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
