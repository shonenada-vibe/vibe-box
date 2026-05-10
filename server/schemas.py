from __future__ import annotations

from pydantic import BaseModel, Field


class QueryResponse(BaseModel):
    request_id: str
    transcript: str
    reply_text: str
    display_lines: list[str] = Field(default_factory=list)
    speak_text: str = ""
    refresh_mode: str = "partial"
    cache_ttl_s: int = 60
