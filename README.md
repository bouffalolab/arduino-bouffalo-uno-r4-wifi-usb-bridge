# BL 版 UNO R4 WiFi USB bridge

本仓库对应工作区中的“BL 版 UNO bridge”，目录名为 `uno-r4-wifi-usb-bridge-bl`。

目标是以 BL616CL 替代 UNO R4 WiFi 的 ESP32-S3，承接无线协处理器及 USB 桥接功能；RA4M1 继续运行用户程序。

当前阶段：首轮演示的两项（USB 串口透传、RA4M1 发 AT 联网与 TCP 客户端）已在首块板上实测通过；CMSIS-DAP 尚未实现。

通用平台达到桥接应用所需的最小基线后，优先交付 bridge。应用管理 AT、RA4M1 引脚和 CMSIS-DAP，通过通用平台接口使用底层能力。

本地共享入口：[首轮演示](../docs/project/bridge-demo.md)、[UNO 架构](../docs/reference/uno-r4-wifi.md)、[路线与切换点](../docs/project/roadmap.md)、[开发规范](../AGENTS.md)。

## 构建与上传

固件是 arduino-bl 平台上的 sketch `UNOR4USBBridgeBL/`，FQBN `bouffalolab:bl:unor4wifi_bl616cl`。默认按 1V0 模组（RA4M1 MD 接 GPIO14）；1V1 模组（MD 接 GPIO10）编译时加 `--build-property compiler.cpp.extra_flags=-DBRIDGE_MODULE_1V1`。

```sh
arduino-cli compile -b bouffalolab:bl:unor4wifi_bl616cl --build-path <编译目录> UNOR4USBBridgeBL
arduino-cli upload -b bouffalolab:bl:unor4wifi_bl616cl -p <串口> --input-dir <编译目录> UNOR4USBBridgeBL
```

bridge 运行时自己处理 1200 波特率（给 RA4M1 用），不响应 BL616CL 的 1200 bps touch。更新 bridge 本身要先让 BL616CL 进 ROM 下载模式（复位时 GPIO36 为高电平），再对 ROM 的下载串口（`ffff:ffff`）执行上传；官方的 `AT+RESTARTBOOTLOADER` 入口尚未实现。

## 功能

| 功能 | 状态 | 证据或说明 |
| --- | --- | --- |
| USB 串口（`Serial`）与 RA4M1 用户串口（`Serial1`，uart0 GPIO34/35 经 U4 接 SCI9）双向透传；其他波特率设为 `Serial1` 的波特率（8N1），每次改动后清空未转发的接收数据 | experimental | 首块板（1V0），Linux：以 FT232 代替 RA4M1 接在 GPIO34/35，115200、9600、230400、921600 下双向各 1–8 KB 逐字节一致；921600 下双向同时各 64 KB 连续 3 轮一致；重新打开端口后跟随波特率。接 RA4M1（R47/R48 补焊后）：115200 下回显 50 字节、2 KB 一致 |
| 1200：MD 高、RESET 双击，RA4M1 进 bootloader；2400：MD 低、RESET 一次，RA4M1 进 ROM 下载模式，MD 保持低到下一次 1200（同官方 bridge） | experimental | 首块板（1V0），Linux：arduino-cli（renesas 1.6.0，unor4wifi）经 bridge 给 RA4M1 上传并运行，bossac 以 921600 连上 bootloader；2400 后 RA4M1 ROM 应答同步（0x00→0x00、0x55→0xC3），再经 1200 上传恢复；1200 时 BL616CL 不进下载模式 |
| AT 命令（`Serial2`，uart1 GPIO6/7 经 U4 接 RA4M1 SCI1，115200 8N1，独立任务处理）：WiFiS3 的联网与 TCP 客户端，即 SOFTRESETWIFI、FWVERSION、WIFIMODE（只支持 1）、BEGINSTA、GETSTATUS、IPSTA、GETSSID、GETRSSI、GETBSSID、MACSTA、DISCONNECT、HOSTBYNAME、PING、BEGINCLIENT、CLIENTCONNECTNAME、CLIENTCONNECT、CLIENTSEND、AVAILABLE、CLIENTRECEIVE、CLIENTCONNECTED、CLIENTCLOSE、FLUSH、REMOTEIP、REMOTEPORT；失败回 `+<命令>: ERROR` | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0）：连 WPA3 网络 5.4 s 取得地址，DNS、ping 网关、按地址与域名连 80 端口 HTTP 收发、3000 字节 POST 正文写出、断开均正常；与局域网电脑上的 TCP 回显服务收发 1 B–4 KB 二进制负载（含 0x00、CRLF），两端逐字节一致；同一固件下透传与经 bridge 上传回归通过 |
| AP、扫描、静态地址、WiFiServer、UDP、TLS（WiFiSSLClient）、时间、文件系统、OTA、BLE、CMSIS-DAP、`AT+RESTARTBOOTLOADER` | unimplemented | 对应 AT 命令回 `+<命令>: ERROR` |

AT 协议与命令子集见 [WiFiS3 AT 协议](../docs/reference/wifis3-at-protocol.md)。`AT+FWVERSION` 返回本固件版本 0.1.0，WiFiS3 示例按字符串与 0.6.0 比较后会打印升级提示，不影响使用。`AT+BEGINSTA` 把第一个逗号之后的内容都当作密码。
