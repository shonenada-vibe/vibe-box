from __future__ import annotations


def _fit_line(text: str, width: int = 18) -> str:
    stripped = " ".join(text.split())
    if not stripped:
        return ""
    return stripped[:width]


def build_reply_text(
    transcript: str,
    *,
    device_id: str = "",
    temperature: str = "",
    humidity: str = "",
) -> str:
    if transcript:
        status = []
        if temperature:
            status.append(f"T={temperature}")
        if humidity:
            status.append(f"H={humidity}")
        suffix = f" [{' '.join(status)}]" if status else ""
        prefix = f"{device_id}: " if device_id else ""
        return f"{prefix}{transcript}{suffix}"
    return "Server mock processed an empty transcript."


def build_display_lines(
    transcript: str,
    reply_text: str,
    *,
    temperature: str = "",
    humidity: str = "",
) -> list[str]:
    lines = [
        "Vibe Box",
        _fit_line(transcript) or "no transcript",
        _fit_line(reply_text) or "no reply",
    ]

    if temperature or humidity:
        lines.append(_fit_line(f"T:{temperature or '-'} H:{humidity or '-'}"))
    else:
        lines.append("server mock ok")

    return [line for line in lines if line][:4]
