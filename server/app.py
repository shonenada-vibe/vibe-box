from __future__ import annotations

import logging
from uuid import uuid4

from fastapi import FastAPI, File, Form, UploadFile

from schemas import QueryResponse
from provider_adapter import fake_transcribe
from response_shaper import build_display_lines, build_reply_text
from tts_proxy import build_speak_text

app = FastAPI(title="vibe-box-server", version="0.1.0")
logger = logging.getLogger("vibe_box.server")


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
    query_text: str = Form(default=""),
    audio_format: str = Form(default="wav"),
    audio: UploadFile | None = File(default=None),
) -> QueryResponse:
    audio_bytes = b""
    if audio is not None:
        audio_bytes = await audio.read()

    request_id = str(uuid4())
    logger.info(
        "query request_id=%s device_id=%s firmware=%s session_id=%s language=%s audio_format=%s audio_bytes=%d",
        request_id,
        device_id,
        firmware_version,
        session_id,
        language,
        audio_format,
        len(audio_bytes),
    )

    transcript = fake_transcribe(
        query_text=query_text,
        filename=audio.filename if audio else None,
        audio_size=len(audio_bytes) if audio else None,
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

    logger.info(
        "query response request_id=%s transcript=%r display_lines=%s",
        request_id,
        transcript,
        display_lines,
    )

    return QueryResponse(
        request_id=request_id,
        transcript=transcript,
        reply_text=reply_text,
        display_lines=display_lines,
        speak_text=build_speak_text(reply_text),
        refresh_mode="partial",
        cache_ttl_s=60,
    )
