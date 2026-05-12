# Server

这是 `vibe-box` 的最小薄服务端骨架。

当前目标不是完整生产服务，而是尽快提供一个设备可请求的薄适配层，用于联调整个链路：

- 接收设备请求
- 暂时返回假数据
- 后续转发到第三方 STT / LLM / TTS
- 把返回结果压缩成适合 200x200 墨水屏显示的短文本

## 设计原则

- 设备是主系统，server 不是主系统
- server 尽量无状态
- server 不承担 UI、录音、设备状态机
- API key 保存在 server，不保存在设备
- 第一版不引入数据库，除非调试和鉴权明确需要

## 当前接口

- `GET /health`
- `POST /v1/query`

`POST /v1/query` 当前支持两种输入：

- `query_text`
- `audio` 文件

还支持这些设备字段：

- `language`
- `recording_duration_ms`
- `Authorization: Bearer <token>` 请求头

目前仍然是 mock 逻辑：

- 如果传了 `query_text`，直接把它当作转写结果
- 如果传了 `audio` 且配置了 `WHISPER_API_URL`，会转发到 Whisper-compatible STT
- 如果传了 `audio` 但没配置 STT 环境变量，会返回占位 transcript
- 再生成适合 200x200 墨水屏显示的 `display_lines`

## 可选环境变量

- `WHISPER_API_URL`
- `WHISPER_API_KEY`
- `VIBE_BOX_STT_MODEL`
- `VIBE_BOX_STRICT_STT`
- `VIBE_BOX_DEVICE_TOKEN`

说明：

- `WHISPER_API_URL` 指向一个 Whisper-compatible `/audio/transcriptions` 接口
- `VIBE_BOX_STRICT_STT=true` 时，STT 失败会直接报错，不走 fallback transcript
- `VIBE_BOX_DEVICE_TOKEN` 如果配置了，设备请求必须带匹配的 Bearer token

## 后续拆分

- `provider_adapter.py`
- `response_shaper.py`
- `tts_proxy.py`
- `schemas.py`
