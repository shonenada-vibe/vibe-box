from __future__ import annotations


def fake_transcribe(filename: str | None) -> str:
    if filename:
        return f"received audio file: {filename}"
    return "received request without audio file"
