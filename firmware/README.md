# Firmware

这是 `vibe-box` 的设备端固件工程，目标板是 `ESP32-S3-Touch-ePaper-1.54` V2。

当前阶段：

- 建立最小 `ESP-IDF` 工程骨架
- 已打通 `Wi-Fi STA + /health` 探测的可编译底座
- 后续逐步接入：
  - NVS
  - 墨水屏
  - 音频采集
  - 传感器
  - 配网
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
  - `Server base URL`
  - `Device ID`
  - `Firmware version`
  - `Enable demo /v1/query requests`
  - `Demo query text`
  - `Health poll interval`
  - `Demo query poll interval`
- 若未配置 `SSID`，启动后会停在 `provisioning` 状态
- 若已配置 `SSID`，启动后会：
  - 连接 Wi-Fi
  - 在 demo 模式下周期性请求 `POST {server_base_url}/v1/query`
  - 非 demo 模式下周期性请求 `GET {server_base_url}/health`
  - 用日志打印状态、请求内容、响应内容和解析结果

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
