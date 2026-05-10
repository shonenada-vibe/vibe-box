from __future__ import annotations

from uuid import uuid4

from fastapi import FastAPI, File, Form, UploadFile

from provider_adapter import fake_transcribe
from response_shaper import build_display_lines, build_reply_text
from schemas import QueryResponse
from tts_proxy import build_speak_text

app = FastAPI(title="vibe-box-server", version="0.1.0")


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
    audio_format: str = Form(default="wav"),
    audio: UploadFile | None = File(default=None),
) -> QueryResponse:
    _ = (device_id, firmware_version, session_id, battery_level, language, audio_format)
    transcript = fake_transcribe(audio.filename if audio else None)
    reply_text = build_reply_text(transcript)
    display_lines = build_display_lines(transcript)

    if temperature or humidity:
        display_lines[-1] = f"T:{temperature or '-'} H:{humidity or '-'}"

    if audio is not None:
        await audio.read()

    return QueryResponse(
        request_id=str(uuid4()),
        transcript=transcript,
        reply_text=reply_text,
        display_lines=display_lines,
        speak_text=build_speak_text(reply_text),
        refresh_mode="partial",
        cache_ttl_s=60,
    )
