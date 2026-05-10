from __future__ import annotations


def build_display_lines(transcript: str) -> list[str]:
    return [
        "Vibe Box",
        transcript[:18] or "no transcript",
        "server mock ok",
    ]


def build_reply_text(transcript: str) -> str:
    if transcript:
        return f"Server mock processed: {transcript}"
    return "Server mock processed an empty transcript."
