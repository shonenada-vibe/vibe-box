# Server

这是 `vibe-box` 的最小薄服务端骨架。

当前目标不是完整生产服务，而是尽快提供一个设备可请求的薄适配层，用于联调整个链路：

- 接收设备请求
- 转发到第三方 STT / LLM / TTS
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

`POST /v1/query` 当前接收设备上传的 `audio` 文件。

还支持这些设备字段：

- `language`
- `recording_duration_ms`
- `Authorization: Bearer <token>` 请求头

当前行为：

- 要求请求里带真实音频
- 如果配置了 `WHISPER_API_URL`，会转发到 Whisper-compatible STT
- 如果 STT 配置缺失或调用失败，会直接返回错误
- 再生成适合 200x200 墨水屏显示的 `display_lines`
- 在 macOS 本地开发环境下，会额外尝试把结果渲染成 `200x200` 的 1-bit 点阵并返回 `display_bitmap_hex`

## 可选环境变量

- `WHISPER_API_URL`
- `WHISPER_API_KEY`
- `VIBE_BOX_STT_MODEL`
- `VIBE_BOX_STRICT_STT`
- `VIBE_BOX_DEVICE_TOKEN`

说明：

- `WHISPER_API_URL` 指向一个 Whisper-compatible `/audio/transcriptions` 接口
- `VIBE_BOX_DEVICE_TOKEN` 如果配置了，设备请求必须带匹配的 Bearer token

## 后续拆分

- `provider_adapter.py`
- `response_shaper.py`
- `tts_proxy.py`
- `schemas.py`

## 中文显示说明

当前设备端自带的是最小 `ASCII` 字库，所以中文不适合直接在固件里逐字渲染。

现在的联调方案是：

- server 继续返回 `display_lines`
- server 同时尝试把这些行渲染成整屏 `1bpp` 点阵
- 设备端如果收到 `display_bitmap_hex`，就直接把这张位图刷到 e-paper
- 如果服务端本机没有可用的 `Swift/AppKit` 环境，则会自动退回纯文本字段，不影响接口可用性
