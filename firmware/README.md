# Firmware

这是 `vibe-box` 的设备端固件工程，目标板是 `ESP32-S3-Touch-ePaper-1.54` V2。

当前阶段：

- 建立最小 `ESP-IDF` 工程骨架
- 先保留状态机与日志主循环
- 后续逐步接入：
  - NVS
  - Wi-Fi 配网
  - 墨水屏
  - 音频采集
  - 传感器
  - 服务端请求

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
