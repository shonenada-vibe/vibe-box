from __future__ import annotations


def fake_transcribe(
    query_text: str | None = None,
    filename: str | None = None,
    audio_size: int | None = None,
) -> str:
    cleaned_query = (query_text or "").strip()
    if cleaned_query:
        return cleaned_query

    if filename:
        size_suffix = f" ({audio_size} bytes)" if audio_size is not None else ""
        return f"received audio file: {filename}{size_suffix}"

    return "received request without audio file"
