from __future__ import annotations

import json
import os
import uuid
from urllib import error, request


DEFAULT_WHISPER_MODEL = os.environ.get("VIBE_BOX_STT_MODEL", "whisper-large-v3-turbo")


def _extract_text(payload: object) -> str:
    if isinstance(payload, dict):
        text = payload.get("text") or payload.get("transcription")
        if text is not None:
            return str(text).strip()

    raise RuntimeError(f"STT response did not include transcription text: {payload!r}")


def _multipart_audio_body(
    *,
    audio_bytes: bytes,
    filename: str,
    language: str,
    model: str,
) -> tuple[bytes, str]:
    boundary = f"----vibe-box-{uuid.uuid4().hex}"
    fields = {
        "model": model,
        "language": language,
        "response_format": "json",
        "temperature": "0",
    }
    parts: list[bytes] = []

    for name, value in fields.items():
        parts.extend([
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode(),
            str(value).encode(),
            b"\r\n",
        ])

    parts.extend([
        f"--{boundary}\r\n".encode(),
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'.encode(),
        b"Content-Type: application/octet-stream\r\n\r\n",
        audio_bytes,
        b"\r\n",
        f"--{boundary}--\r\n".encode(),
    ])
    return b"".join(parts), f"multipart/form-data; boundary={boundary}"


def _whisper_compatible_transcribe(
    *,
    audio_bytes: bytes,
    filename: str,
    language: str,
) -> str:
    api_url = os.environ.get("WHISPER_API_URL", "").strip()
    api_key = os.environ.get("WHISPER_API_KEY", "").strip()
    model = os.environ.get("VIBE_BOX_STT_MODEL", DEFAULT_WHISPER_MODEL).strip() or DEFAULT_WHISPER_MODEL

    if not api_url:
        raise RuntimeError("WHISPER_API_URL is not set")

    body, content_type = _multipart_audio_body(
        audio_bytes=audio_bytes,
        filename=filename,
        language=language,
        model=model,
    )
    headers = {"Content-Type": content_type}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    req = request.Request(api_url, data=body, headers=headers, method="POST")
    try:
        with request.urlopen(req, timeout=120) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except error.HTTPError as exc:
        error_body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Whisper-compatible STT request failed with HTTP {exc.code}: {error_body}") from exc
    except error.URLError as exc:
        raise RuntimeError(f"Whisper-compatible STT request failed: {exc}") from exc

    return _extract_text(payload)


def transcribe_audio(
    *,
    filename: str,
    audio_bytes: bytes,
    language: str = "zh",
) -> str:
    if not filename:
        raise RuntimeError("audio filename is missing")
    if not audio_bytes:
        raise RuntimeError("audio payload is empty")

    return _whisper_compatible_transcribe(
        audio_bytes=audio_bytes,
        filename=filename,
        language=language,
    )
