# 智慧农场

ESP32 固件、Android APP 与 Altium Designer 24 工程。

- `main/`：ESP-IDF 固件源码。
- `app-android/`：Android APP 源码。
- `tools/`：开发环境与调试脚本。
- `hardware/altium/test/test.PrjPcb`：AD 主工程。
- `hardware/altium/test/Sheet1.SchDoc`：原理图。
- `hardware/altium/test/PCB1.PcbDoc`：PCB。
- `hardware/altium/test/library/`：配套器件库。

固件本地配置：复制 `main/secrets.h.example` 为 `main/secrets.h` 并填写。
APP 本地配置：复制 `app-android/mqtt.properties.example` 为 `app-android/mqtt.properties` 并填写。
真实凭据和构建产物不提交。

硬件文件保留当前设计进度，不表示已完成生产验证。
