# BL 版 UNO R4 WiFi USB bridge

本仓库对应工作区中的“BL 版 UNO bridge”，目录名为 `uno-r4-wifi-usb-bridge-bl`。

目标是以 BL616CL 替代 UNO R4 WiFi 的 ESP32-S3，承接无线协处理器及 USB 桥接功能；RA4M1 继续运行用户程序。

当前阶段：仓库初始化；没有迁入上游应用、兼容层或旧固件。用户已确认首轮演示为USB串口透传+RA4M1发AT联网/TCP，尚未实现这些功能。

通用平台达到桥接应用所需的最小基线后，优先交付 bridge。应用管理 AT、RA4M1 引脚和 CMSIS-DAP，通过通用平台接口使用底层能力。

本地共享入口：[首轮演示](../docs/project/bridge-demo.md)、[UNO 架构](../docs/reference/uno-r4-wifi.md)、[路线与切换点](../docs/project/roadmap.md)、[开发规范](../AGENTS.md)。
