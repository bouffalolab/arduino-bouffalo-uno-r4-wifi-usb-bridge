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

bridge 运行时自己处理 1200 波特率（给 RA4M1 用），不响应 BL616CL 的 1200 bps touch。更新 bridge 本身要先让 BL616CL 进 ROM 下载模式，再对 ROM 的下载串口（`ffff:ffff`）执行上传。进入下载模式有两种办法：

- RA4M1 发 `AT+RESTARTBOOTLOADER`（官方入口）：bridge 回 `OK` 后复位进 ROM 下载模式。WiFiS3 没有封装这条命令，RA4M1 sketch 这样发：

  ```cpp
  #include <WiFiS3.h>

  modem.begin();
  std::string res;
  modem.write(std::string(PROMPT(_RESTART_BOOTLOADER)), res, "%s", CMD(_RESTART_BOOTLOADER));
  ```

- BL616CL 复位时 GPIO36（J6.4）为高电平。

经 ROM 下载口上传偶尔失败（2026-10-23 首块板 77 次中 3 次：握手失败 2 次，写入中途 ROM 停止应答 1 次；原因未查明，RA4M1 发或不发串口数据的对照各 15 次都没有失败）。失败后 BL616CL 停在 ROM 下载口，可直接再上传；中途失败后断电重启能否启动未测，不能启动时用 GPIO36 进入下载模式。

## 功能

| 功能 | 状态 | 证据或说明 |
| --- | --- | --- |
| USB 串口（`Serial`）与 RA4M1 用户串口（`Serial1`，uart0 GPIO34/35 经 U4 接 SCI9）双向透传；其他波特率设为 `Serial1` 的波特率（8N1），每次改动后清空未转发的接收数据 | experimental | 首块板（1V0），Linux：以 FT232 代替 RA4M1 接在 GPIO34/35，115200、9600、230400、921600 下双向各 1–8 KB 逐字节一致；921600 下双向同时各 64 KB 连续 3 轮一致；重新打开端口后跟随波特率。接 RA4M1（R47/R48 补焊后）：115200 下回显 50 字节、2 KB 一致 |
| 1200：MD 高、RESET 双击，RA4M1 进 bootloader；2400：MD 低、RESET 一次，RA4M1 进 ROM 下载模式，MD 保持低到下一次 1200（同官方 bridge） | experimental | 首块板（1V0），Linux：arduino-cli（renesas 1.6.0，unor4wifi）经 bridge 给 RA4M1 上传并运行，bossac 以 921600 连上 bootloader；2400 后 RA4M1 ROM 应答同步（0x00→0x00、0x55→0xC3），再经 1200 上传恢复；1200 时 BL616CL 不进下载模式 |
| AT 命令（`Serial2`，uart1 GPIO6/7 经 U4 接 RA4M1 SCI1，115200 8N1，独立任务处理）：WiFiS3 的联网与 TCP 客户端，即 SOFTRESETWIFI、FWVERSION、WIFIMODE（只支持 1）、BEGINSTA、GETSTATUS、IPSTA、GETSSID、GETRSSI、GETBSSID、MACSTA、DISCONNECT、HOSTBYNAME、PING、BEGINCLIENT、CLIENTCONNECTNAME、CLIENTCONNECT、CLIENTSEND、AVAILABLE、CLIENTRECEIVE、CLIENTCONNECTED、CLIENTCLOSE、FLUSH、REMOTEIP、REMOTEPORT；失败回 `+<命令>:ERROR`（冒号后不带空格，否则 WiFiS3 在 CLIENTRECEIVE、WIFISCAN 等命令上认不出、要等满 10 s） | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0）：连 WPA3 网络 5.4 s 取得地址，DNS、ping 网关、按地址与域名连 80 端口 HTTP 收发、3000 字节 POST 正文写出、断开均正常；与局域网电脑上的 TCP 回显服务收发 1 B–4 KB 二进制负载（含 0x00、CRLF），两端逐字节一致；同一固件下透传与经 bridge 上传回归通过 |
| `AT+RESTARTBOOTLOADER`：回 `OK` 后复位进 BL616CL 的 ROM 下载模式，供更新本固件（上游直接复位、不应答） | experimental | 首块板（1V0），Linux：RA4M1 用未修改的 WiFiS3 发命令，0.6–0.9 s 后出现 ROM 下载口，经 USB 上传 bridge 后重新枚举，RA4M1 收到 `OK` 且未被复位，更新后 RA4M1 串口数据经 bridge 正常到达电脑；连续 6 轮通过（另一次运行第 4 轮上传中途 ROM 停止应答，见上文）。主机对接测试覆盖应答与复位调用 |
| `AT+WIFISCAN`（WiFiS3 `scanNetworks()`）：调用平台 `WiFi.scanNetworks()`，按上游格式列出最强的 10 个网络；没有网络时回 `+WIFISCAN: 0\|`（上游只回 `OK`） | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0）：未连接与已连接时扫描各 1.9–2.0 s 返回，字段有效、RSSI 有序，列出测试 AP 与当前 BSSID，扫描后连接保持、ping 网关成功；未修改的 ScanNetworks 例程连续 3 轮列出 8 个网络（10 个中 2 个隐藏网络被 WiFiS3 跳过）。主机对接测试覆盖 SSID 含空格与 32 字节、各加密方式、0 个网络与扫描失败 |
| UDP（WiFiS3 `WiFiUDP`）：UDPBEGIN、UDPBEGINPACKETIP、UDPBEGINPACKETADD（按域名）、UDPWRITE、UDPENDPACKET、UDPPARSE、UDPAVAILABLE、UDPREAD、UDPPEEK、UDPFLUSH、UDPREMOTEIP、UDPREMOTEPORT、UDPSTOP；同上游最多 4 个（0–3，与 TCP 编号独立），一个数据报最多 1460 字节；与 TCP 共用平台的 8 个 socket | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3：与局域网电脑往返 1–1460 字节二进制数据报 3 轮 18 个一致、无重发，往返 41–336 ms，另一来源端口的数据报回到该端口；未修改的 WiFiUdpSendReceiveString（电脑发 3 次都收到 acknowledged）、WiFiUdpNtpClient（3 次 NTP 时间与电脑相差 1.4–1.6 s）正常。主机对接测试覆盖分段写入、按域名发送、超过 1460 字节的写入失败后帧同步、4 个上限与释放后复用 |
| TCP 服务端（WiFiS3 `WiFiServer`）：BEGINSERVER、SERVERAVAILABLE、SERVERACCEPT、SERVERWRITE、SERVEREND；同上游最多 4 个服务端（0–3），接入的连接编号 1000–1007，之后走 AVAILABLE、CLIENTRECEIVE、CLIENTSEND、CLIENTCLOSE 等命令；语义同上游：SERVERAVAILABLE 只报模块侧有数据的连接，SERVERWRITE 发给未 accept 的连接，SERVEREND 只停止监听。WiFiS3 的 `read()` 把数据整段取进 RA4M1 本地缓冲，`server.available()` 换到别的编号时这些字节被丢弃，所以未修改的 WiFiChatServer 每段数据只回显首字符，上游 ESP32 固件也是这样（代码阅读，见协议文档）；用 `accept()` 并自己保存连接的写法（如 WiFiAdvancedChatServer）没有这个问题 | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0），电脑作客户端：WiFiWebServer 3 次 GET 都是 200、282 字节，发完后关闭连接；WiFiChatServer 两个客户端都收到逐字符发送的一行，整段发送只回显首字符；WiFiAdvancedChatServer 两个客户端得到编号 0、1，一个客户端的行只转给另一个，断开后例程检测到。主机对接测试覆盖同一连接多次返回、accept、广播、10 个连接轮换回收、end 后拒绝连接、4 个服务端上限与释放后复用、联网前 begin 失败 |
| TLS 客户端（WiFiS3 `WiFiSSLClient`）：SSLBEGINCLIENT、SETCAROOT（证书束，或最长 16384 字节的 PEM）、SSLCLIENTCONNECTNAME、SSLCLIENTCONNECT（超时含 TCP 与握手）、SSLCLIENTSEND、SSLAVAILABLE、SSLCLIENTRECEIVE、SSLPEEK、SSLCLIENTCONNECTED、SSLCLIENTFLUSH、SSLREMOTEIP、SSLREMOTEPORT、SSLCLIENTCLOSE；编号 2000–2007（同上游 8 个）；调用平台 `WiFiSSLClient`，TLS 1.2、默认用包内根证书束、不查证书有效期，限制见 arduino-bl README；SETECCSLOT 读掉证书后回 ERROR（无安全芯片）；同时能建立的连接受 BL616CL 内存限制，本固件实测 2 个，超出时 connect 失败 | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0）：证书束下 www.baidu.com、valid-isrgrootx2.letsencrypt.org 成功，self-signed、untrusted-root.badssl.com 失败；`setCACert` 测试 CA 后与电脑 TLS 服务往返 HTTP 100062 字节、字节和一致（约 11 s），回显一致，名称不符失败，过期证书成功；同时开 8 个连接 2 个成功，连做两次相同；只把 www.google.com 换成 www.baidu.com 的 WiFiWebClientSSL 打印 HTTP/1.1 200 后正常结束，未修改的例程因本测试网络连不上 google 而连接失败、正常结束。主机对接测试覆盖证书束与自定义 CA 的信任、SETCAROOT 载荷与帧同步、SETECCSLOT 拒绝、连接超时参数、对端关闭、TCP 与 TLS 编号混用、8 个编号上限 |
| 静态地址（WiFiS3 `config`、`setDNS`）：SETIP（地址、网关、掩码，可再带 1–2 个 DNS），调用平台 `WiFi.config`、`setDNS`；地址为 0.0.0.0 时恢复 DHCP，SOFTRESETWIFI 也恢复 DHCP（同上游）。WiFiS3 的 `config` 先发 SOFTAPCONFIG，AP 未实现时它回 ERROR，WiFiS3 不看结果 | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0）：与平台相同的一组检查全部通过（连接前与已连接时设静态地址、断开重连、`setDNS`、恢复 DHCP、DHCP 下 `setDNS` 转为静态地址；ping 网关、DNS 解析、与电脑 TCP 回显、电脑 ping 板子）。主机对接测试覆盖已连接时生效、两个 DNS、重连保持、`config(地址)` 的默认值、参数不足与不合法地址、恢复 DHCP、SOFTRESETWIFI |
| AP、UDP 组播（UDPBEGINMULTI、BEGINPACKETMULTI）、HOSTNAME、时间、文件系统、OTA、BLE、CMSIS-DAP | unimplemented | 对应 AT 命令回 `+<命令>:ERROR` |

## 测试

`tests/at_host/run.py` 在电脑上做 AT 对接测试：RA4M1 侧是未修改的 WiFiS3（直接编译已安装的 `arduino:renesas_uno` 包里的源码），BL616CL 侧是本仓库的 AT 服务端加 arduino-bl 的 WiFi 库，其中 Wi-Fi 换成虚拟 AP、socket 换成本机 socket；两个程序经 socketpair 相连，逐项核对扫描、联网、地址查询、DNS、ping、TCP 收发、UDP 收发、TCP 服务端、TLS 客户端、静态地址、socket 上限、错误应答与 RESTARTBOOTLOADER，共 65 项；UDP 对端是本脚本在 127.0.0.1 起的 UDP 回显服务，TCP 服务端的对端是本脚本中的客户端线程；TLS 换成不加密的替身（`tests/at_host/bl/fake_tls_port.c`），只核对命令把主机名、证书参数与数据交给平台，不检验加密与证书校验。需要 `g++`、`python3`，arduino-bl 默认取本仓库旁边的目录（`--platform` 可改）：

```sh
python3 tests/at_host/run.py --renesas <arduino 数据目录>/packages/arduino/hardware/renesas_uno/1.6.0
```

设环境变量 `AT_HOST_TRACE=1` 时，BL 侧把串口上的原始收发写到 `bl_host.stderr`。主机测试不代替实板结果。

AT 协议与命令子集见 [WiFiS3 AT 协议](../docs/reference/wifis3-at-protocol.md)。`AT+FWVERSION` 返回本固件版本 0.1.0，WiFiS3 示例按字符串与 0.6.0 比较后会打印升级提示，不影响使用。`AT+BEGINSTA` 把第一个逗号之后的内容都当作密码（含逗号的密码经主机测试验证）。
