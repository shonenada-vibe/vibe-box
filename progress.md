# Vibe Box 当前进展

## 概览

当前项目已经完成到：

- 设备端 `ESP-IDF` 工程已建立并可稳定编译
- 板子已可独立连接 `Wi-Fi`
- 设备已可通过 `HTTP POST /v1/query` 向薄服务端发送请求
- 设备已可接收并解析服务端 JSON 响应
- 已完成 `SoftAP + Web` 首次配网与 `NVS` 持久化
- 已完成 demo 文本请求与 demo 音频上传链路
- 已建立真实 `I2S RX -> WAV -> 上传` 的代码入口，但尚未接通板级 codec 初始化

当前还没有完成的核心能力：

- 墨水屏真实渲染
- ES8311 真正可用的录音初始化
- 触摸输入接入
- SHTC3 温湿度与 RTC 接入
- 扬声器播放
- 低功耗与恢复策略

## 对照 plan.md 的完成情况

### Phase 0: 板级验证

已完成：

- 已确认项目基于 `ESP-IDF v5.5+`
- 已建立可编译运行的最小 `ESP-IDF` 工程
- 已补充板级 bring-up 说明到 `docs/bringup.md`
- 已从官方资料确认这是 `ESP32-S3-Touch-ePaper-1.54` 的 `V2` 板型
- 已从官方原理图确认部分音频相关引脚信息，用于后续 `I2S` 接线配置

未完成：

- 官方 `V2` 的墨水屏、音频、触摸、SHTC3、RTC 示例逐项跑通并记录
- 从官方 demo 中提取最终可用的 `e-paper / ES8311 / touch / sensor` 配置

### Phase 1: 工程骨架

已完成：

- 初始化 `firmware/` 独立 `ESP-IDF` 工程
- 建立 `firmware/main/main.c` 主流程
- 建立统一日志输出和状态机
- 状态机已覆盖：
  - `boot`
  - `provisioning`
  - `idle`
  - `recording`
  - `uploading`
  - `displaying`
  - `error`
- 接入 `NVS` 用于保存设备配置
- 建立 `audio_input` 组件：
  - `firmware/components/audio_input/audio_input.c`
  - `firmware/components/audio_input/audio_input.h`

当前实现细节：

- 主流程集中在 `firmware/main/main.c`
- `NVS` 命名空间为 `vibe_box`
- 启动时优先读取 `NVS`，不存在时退回 `sdkconfig` 默认值
- UI 目前是“日志化 UI”，还没有真实 e-paper driver

### Phase 2: 配网与设备配置

已完成：

- 实现 `SoftAP + 本地网页` 首次配网
- 设备进入配网模式时启动本地 AP
- 可通过 `http://192.168.4.1/` 提交配置
- 配置写入 `NVS`
- 重启后自动恢复配置
- 设备可自动切回 `STA` 模式并联网

已持久化的配置项：

- `Wi-Fi SSID`
- `Wi-Fi password`
- `Server base URL`
- `API token`
- `Device ID`
- `Firmware version`
- `Language`
- `Recording duration ms`

当前实现细节：

- 配网页由 `esp_http_server` 提供
- `main.c` 中已实现：
  - 配网页 HTML 渲染
  - 表单解析
  - URL decode
  - 配置保存
- 配网状态和 IP 当前通过串口日志显示，尚未显示到墨水屏

### Phase 3: 音频链路

已完成：

- 建立统一音频目标格式：
  - `16kHz`
  - `mono`
  - `16-bit PCM`
- 已支持生成标准 `WAV`
- 已支持 demo 音频生成并上传
- 已支持 `I2S RX` 捕获代码路径并封装为 `WAV`

当前实现细节：

- `audio_input_generate_demo_wav()` 可生成测试音频
- `audio_input_capture_i2s_wav()` 已实现 `I2S -> PCM -> WAV`
- `menuconfig` 已可配置：
  - `I2S port`
  - `MCLK`
  - `BCLK`
  - `WS`
  - `DIN`
  - `sample rate`
  - `channels`

未完成：

- ES8311 初始化
- 与板级真实麦克风路径联通
- 真实录音结果验证
- 按键或触摸触发录音
- VAD / 静音截断

说明：

- 当前“真实 `I2S` 采音”代码只完成通用入口，尚未完成 Waveshare V2 板卡级 codec bring-up，因此默认仍使用 demo 路径测试服务端闭环

### Phase 4: 薄服务端最小实现

已完成：

- 建立 `server/` 最小服务端骨架
- 已实现：
  - `GET /health`
  - `POST /v1/query`
- 服务端已完成模块拆分：
  - `server/app.py`
  - `server/provider_adapter.py`
  - `server/response_shaper.py`
  - `server/tts_proxy.py`
  - `server/schemas.py`

当前实现细节：

- `POST /v1/query` 同时支持：
  - `query_text`
  - `multipart/form-data` 音频上传
- 已支持可选设备鉴权：
  - 设备端发 `Authorization: Bearer ...`
  - 服务端通过 `VIBE_BOX_DEVICE_TOKEN` 校验
- `response_shaper` 已负责压缩成适合墨水屏使用的 `display_lines`
- 当前仍以 mock / 最小适配为主，适合联调，不是最终生产逻辑

已验证：

- `python3 -m py_compile server/*.py` 通过

未完成：

- 接真实第三方 `STT / LLM / TTS` 的稳定配置
- 更完整的 provider 选择与错误分类

### Phase 5: 设备联网请求

已完成：

- 已集成 `esp_http_client`
- 已实现：
  - `GET /health`
  - `POST /v1/query`
  - 超时与重试基础逻辑
  - Bearer token header
  - `application/x-www-form-urlencoded` 文本请求
  - `multipart/form-data` 音频上传
  - JSON 响应解析
  - 将结果映射回设备状态机

关键实现细节：

- `perform_http_request()` 统一封装请求逻辑
- `query_server_once()` 根据配置选择：
  - 文本 demo 请求
  - demo 音频上传
  - I2S 音频上传
- 响应解析由 `parse_query_response()` 完成
- 已解析字段：
  - `request_id`
  - `transcript`
  - `reply_text`
  - `display_lines`

本阶段联调中已修复的问题：

- 修复空字符串表单字段导致 URL 编码被误判失败
- 修复 `main` 任务栈过小导致上传时 `stack overflow`
- 修复 `HTTP_EVENT_ON_DATA` 已收到数据但响应体未写入 buffer 的问题

当前状态：

- 板子已成功连上局域网
- 已成功向服务端发起 `/v1/query`
- 已收到 `200` 响应并完成 body 累计逻辑修复
- 最新固件已具备完整请求闭环能力，正在继续做最终实机验证

未完成：

- TLS 证书校验
- 更细粒度的断网恢复与请求恢复
- 墨水屏真实显示结果

### Phase 6: 墨水屏体验优化

已完成：

- 建立 UI 页面状态模型
- 已有这些逻辑页面：
  - `boot`
  - `provisioning`
  - `idle`
  - `recording`
  - `uploading`
  - `displaying`
  - `error`
- 已能把结果映射到：
  - `headline`
  - `detail`
  - `display_lines`

当前实现细节：

- 现在仍是 `render_ui_status()` / `render_ui_query_result()` 的日志化渲染
- 已经具备后续替换成 `ui_epaper` 驱动层的接口形态

未完成：

- 真正的 e-paper 驱动接入
- 文本布局与字体策略
- 局刷 / 全刷策略
- 页面稳定显示

### Phase 7: 传感器与扩展信息

已完成：

- 设备协议已预留：
  - `temperature`
  - `humidity`
  - `battery_level`
- 服务端 `POST /v1/query` 已接收温湿度字段

当前状态：

- 固件当前可发送 demo 温湿度字段
- 板载 `SHTC3` 尚未接入真实读取
- `RTC` 尚未接入

### Phase 8: 语音回放

已完成：

- 服务端结构中已预留 `tts_proxy.py`
- 协议设计中已保留 `speak_text / audio_url` 路径

未完成：

- ES8311 播放初始化
- 扬声器回放链路
- 服务端 TTS 返回与设备拉流播放

### Phase 9: 功耗与可靠性

已完成：

- 状态机已能覆盖失败与恢复路径的基础状态
- 设备配置已持久化到 `NVS`

未完成：

- `light sleep / deep sleep`
- 墨水屏掉电恢复
- 最后一屏恢复
- 离线缓存策略
- 更强的弱网恢复

## 当前代码结构

### 固件

- `firmware/main/main.c`
  - 主状态机
  - Wi-Fi
  - 配网 Web
  - NVS 配置
  - HTTP 请求
  - query 响应解析
  - 日志化 UI
- `firmware/main/Kconfig.projbuild`
  - 暴露设备配置和 demo / I2S 选项
- `firmware/components/audio_input/audio_input.c`
  - demo WAV
  - I2S WAV capture
- `firmware/README.md`
  - 当前开发和刷机说明

### 服务端

- `server/app.py`
  - FastAPI 入口
  - `/health`
  - `/v1/query`
- `server/provider_adapter.py`
  - provider 适配层
- `server/response_shaper.py`
  - 屏幕友好输出压缩
- `server/tts_proxy.py`
  - TTS 预留
- `server/README.md`
  - 启动说明和鉴权说明

## 已完成验证

- `idf.py build` 可通过
- 当前目标板为 `esp32s3`
- `flash_size` 已配置为 `8MB`
- 设备已成功连接本地 `Wi-Fi`
- 设备已成功向服务端发起 `POST /v1/query`
- 服务端已返回 `200`
- 多轮实机联调中已修复：
  - Wi-Fi 配置覆盖问题
  - 表单编码问题
  - 上传路径栈溢出
  - 响应体读取为空

## 当前最重要的未完成项

1. 将当前“日志化 UI”替换成真实 `ui_epaper`
2. 接通 Waveshare V2 板卡的 ES8311 / I2S 真录音链路
3. 接入触摸或按键触发录音
4. 接入 SHTC3 与 RTC
5. 完成一次“真实录音 -> 服务端 -> 墨水屏显示”的端到端闭环

## 备注

- 当前项目已经摆脱“电脑代为处理主逻辑”的旧架构，设备已具备独立联网和请求服务端的基础能力
- 当前仍处于“网络与协议层先打通，再接板级真外设”的阶段
- `progress.md` 记录的是已落地代码状态，不代表 `plan.md` 中所有目标已经完成
