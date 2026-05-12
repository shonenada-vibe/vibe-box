# Firmware

这是 `vibe-box` 的设备端固件工程，目标板是 `ESP32-S3-Touch-ePaper-1.54` V2。

当前阶段：

- 建立最小 `ESP-IDF` 工程骨架
- 已打通 `Wi-Fi STA + NVS + SoftAP 配网 + /health | /v1/query` 的可编译底座
- 后续逐步接入：
  - 墨水屏
  - 音频采集
  - 传感器
  - `/v1/query` 请求

## 预期结构

```text
firmware/
  main/
    main.c
    CMakeLists.txt
```

后续会扩展到 `components/` 目录。

## 构建前提

- `ESP-IDF v5.5.0+`
- 与板子 `V2` 示例兼容的资源与驱动

## 当前行为

- 通过 `menuconfig` 配置：
  - `Wi-Fi SSID`
  - `Wi-Fi password`
  - `Provisioning AP SSID`
  - `Provisioning AP password`
  - `Server base URL`
  - `API token`
  - `Device ID`
  - `Firmware version`
  - `Language`
  - `Recording duration`
  - `Enable demo /v1/query requests`
  - `Enable demo audio upload`
  - `Enable I2S RX audio capture`
  - `I2S port / MCLK / BCLK / WS / DIN`
  - `I2S sample rate / channels`
  - `Demo query text`
  - `Demo audio duration`
  - `Demo audio tone frequency`
  - `Demo temperature`
  - `Demo humidity`
  - `Health poll interval`
  - `Demo query poll interval`
- 启动时会优先从 `NVS` 读取运行时配置：
  - `wifi_ssid`
  - `wifi_password`
  - `server_base_url`
  - `api_token`
  - `device_id`
  - `firmware_version`
  - `language`
  - `recording_duration_ms`
- 若 `NVS` 里没有完整配置，启动后会进入 `provisioning` 状态：
  - 启动 `SoftAP`
  - 打开 `http://192.168.4.1/`
  - 提交表单后写入 `NVS` 并自动切回 `STA`
- 若已有完整配置，启动后会：
  - 连接 Wi-Fi
  - 在 demo 模式下周期性请求 `POST {server_base_url}/v1/query`
  - 若启用 `Enable I2S RX audio capture`，会先录制一段真实 `I2S` 音频，再上传 `WAV`
  - 若启用 `Enable demo audio upload`，会生成一段本地 `WAV` 测试音频并用 `multipart/form-data` 上传
  - 非 demo 模式下周期性请求 `GET {server_base_url}/health`
  - 用日志打印状态、请求内容、响应内容和解析结果

## 真实音频采集

- 当前已经有一个不依赖外部 codec 组件的 `I2S RX` 采集入口
- 默认关闭，避免在板卡引脚未确认前误用
- 你确认好 `BCLK / WS / DIN / 可选 MCLK` 后，只需要在 `menuconfig` 里打开：
  - `Enable I2S RX audio capture`
  - 填好 `I2S` 引脚和采样参数
- 这条链路目前只负责 `I2S -> WAV -> 上传`
- `ES8311` 的寄存器级初始化还没接，因为还缺这块 Waveshare V2 的准确硬件资源

## 配网流程

1. 首次启动或清空 `NVS` 后，设备进入 `provisioning`
2. 手机或电脑连接 `Provisioning AP SSID`
3. 打开 `http://192.168.4.1/`
4. 填写：
   - 家里 Wi‑Fi 的 `SSID / password`
   - `Server Base URL`
   - 可选 `API Token / Device ID / Firmware Version`
   - `Language / Recording Duration`
5. 提交后设备保存配置并自动尝试连回目标 Wi‑Fi

## 常用命令

```sh
pyenv shell 3.13
source ~/w/esp/esp-idf/export.sh
export IDF_COMPONENT_MANAGER=0

idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.usbmodem421201 flash
idf.py -p /dev/cu.usbmodem421201 monitor
```

说明：

- 在当前环境里，`flash` 可能被串口权限限制拦住。
- 本工程默认 `sdkconfig.defaults` 已按 `8MB Flash` 做了基础设置。
- 串口日志建议直接看 `idf.py monitor`。
