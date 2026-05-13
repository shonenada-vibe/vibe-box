# Firmware

这是 `vibe-box` 的设备端固件工程，目标板是 `ESP32-S3-Touch-ePaper-1.54` V2。

当前阶段：

- 建立最小 `ESP-IDF` 工程骨架
- 已打通 `Wi-Fi STA + NVS + SoftAP 配网 + Whisper-compatible STT` 的可编译底座
- 已增加 BLE HID keyboard，设备名为 `VibeBox`
- 后续逐步接入：
  - 墨水屏
  - 音频采集
  - 传感器
  - Whisper-compatible `/audio/transcriptions` 请求
  - BLE 文本输入辅助脚本

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
  - `Whisper-compatible API URL`
  - `Whisper-compatible API key`
  - `STT model`
  - `Device ID`
  - `Firmware version`
  - `Language`
  - `Recording duration`
  - `Enable I2S RX audio capture`
  - `I2S port / MCLK / BCLK / WS / DIN`
  - `I2S sample rate / channels`
  - `Health poll interval`
- 启动时会优先从 `NVS` 读取运行时配置：
  - `wifi_ssid`
  - `wifi_password`
  - `whisper_api_url`
  - `whisper_api_key`
  - `stt_model`
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
  - 启动 BLE 并以 `VibeBox` 广播为键盘
  - 周期性检查 Wi-Fi 连接状态
  - 用户按住 `BOOT` 键录音，松手后把真实 `WAV` 直接发到 Whisper-compatible STT
  - 若 Mac 端 `host/vibebox_text_input.py` 已连接并订阅文本通知，会把转写文本粘贴到当前激活应用
  - 用日志打印状态、请求内容、响应内容和解析结果

## 蓝牙文字输入

- 固件启动后会开启 BLE，广播名称为 `VibeBox`
- 系统蓝牙里连接后会显示为键盘类 HID 设备
- 任意 Unicode 文本输入通过自定义 BLE notify characteristic 交给 Mac 端脚本执行粘贴：
- 快速双击板载 `PWR` 键会重置 BLE 会话状态并重新广播，等待主机重新连接

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r host/requirements.txt
python host/vibebox_text_input.py
```

可选参数：

- `--press-return`：粘贴后自动按回车
- `--once`：连接断开后退出
- `--verbose`：输出 BLE 调试日志

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
   - `Whisper API URL`
   - 可选 `Whisper API Key / STT Model / Device ID / Firmware Version`
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
