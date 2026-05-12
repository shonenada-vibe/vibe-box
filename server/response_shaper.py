from __future__ import annotations


def _fit_line(text: str, width: int = 18) -> str:
    stripped = " ".join(text.split())
    if not stripped:
        return ""
    return stripped[:width]


def build_reply_text(transcript: str) -> str:
    if transcript:
        return transcript
    return ""

def build_display_lines(transcript: str, reply_text: str) -> list[str]:
    lines = [
        _fit_line(transcript) or "no transcript",
        _fit_line(reply_text) or "no reply",
    ]

    return [line for line in lines if line][:4]
