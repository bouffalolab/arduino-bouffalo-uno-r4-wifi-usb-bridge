# BL 版 UNO R4 WiFi USB bridge

本仓库对应工作区中的“BL 版 UNO bridge”，目录名为 `uno-r4-wifi-usb-bridge-bl`。

目标是以 BL616CL 替代 UNO R4 WiFi 的 ESP32-S3，承接无线协处理器及 USB 桥接功能；RA4M1 继续运行用户程序。

当前阶段：USB 串口透传、RA4M1 AT 网络与 CMSIS-DAP/SWD 已在首块板上实测；初版的其他必备功能与发布门槛见路线。

通用平台达到桥接应用所需的最小基线后，优先交付 bridge。应用管理 AT、RA4M1 引脚和 CMSIS-DAP，通过通用平台接口使用底层能力。

本地共享入口：[首轮演示](../docs/project/bridge-demo.md)、[UNO 架构](../docs/reference/uno-r4-wifi.md)、[路线与切换点](../docs/project/roadmap.md)、[开发规范](../AGENTS.md)。

## 构建与上传

固件是 arduino-bl 平台上的 sketch `UNOR4USBBridgeBL/`，FQBN `bouffalolab:bl:unor4wifi_bl616cl`，需要含 HID、external-host Controller 与 littlefs 的 `bl616cl-libs` 0.6.0、`bl-flash-tools` 0.2.0 及配套平台。默认按 1V0 模组（MD GPIO14、SWCLK GPIO12、SWDIO GPIO13）；1V1 映射为 MD GPIO10、SWCLK GPIO8、SWDIO GPIO9，需同时设置 `--build-property compiler.cpp.extra_flags=-DBRIDGE_MODULE_1V1` 与 `--build-property compiler.c.extra_flags=-DBRIDGE_MODULE_1V1`，目前未做 1V1 实板验证。

```sh
arduino-cli compile -b bouffalolab:bl:unor4wifi_bl616cl --build-path <编译目录> UNOR4USBBridgeBL
arduino-cli upload -b bouffalolab:bl:unor4wifi_bl616cl -p <串口> --input-dir <编译目录> UNOR4USBBridgeBL
```

bridge 运行时自己处理 1200 波特率（给 RA4M1 用），不响应 BL616CL 的 1200 bps touch。更新 bridge 本身要先让 BL616CL 进 ROM 下载模式，再对 ROM 的下载串口（`ffff:ffff`）执行上传。进入下载模式有三种办法：

- RA4M1 发 `AT+RESTARTBOOTLOADER`（官方入口）：bridge 回 `OK` 后复位进 ROM 下载模式。WiFiS3 没有封装这条命令，RA4M1 sketch 这样发：

  ```cpp
  #include <WiFiS3.h>

  modem.begin();
  std::string res;
  modem.write(std::string(PROMPT(_RESTART_BOOTLOADER)), res, "%s", CMD(_RESTART_BOOTLOADER));
  ```

- BL616CL 复位时 GPIO36（J6.4）为高电平。
- 对 vendor HID 的 report ID 0 发送 Feature，载荷首字节为 `0xAA`（Linux hidraw 调用时前置 report ID 字节 0）。GET Feature 返回本 bridge 版本三个字节，当前 `00 01 00`。这是 BL ROM 更新入口，之后使用 BL 上传工具；不兼容 ESP32 固件镜像或其烧录协议。

经 ROM 下载口上传偶尔失败（2026-10-23 首块板 77 次中 3 次：握手失败 2 次，写入中途 ROM 停止应答 1 次；原因未查明，RA4M1 发或不发串口数据的对照各 15 次都没有失败）。失败后 BL616CL 停在 ROM 下载口，可直接再上传；中途失败后断电重启能否启动未测，不能启动时用 GPIO36 进入下载模式。

## 功能

| 功能 | 状态 | 证据或说明 |
| --- | --- | --- |
| CDC + vendor HID / CMSIS-DAP v1 传输；SWD 访问真实 RA4M1，64 字节、单个在途请求；无 JTAG/SWO | experimental | Linux x86_64、BL616CL 1V0、libs 0.4.1：OpenOCD 0.11.0-arduino2 / pyOCD 0.45.0（Renesas.RA_DFP 6.6.0），100 kHz 请求频率；DPIDR `0x5ba02477`、CPUID `0x410fc241`，SRAM/寄存器读写恢复、sketch 编程与完整读回、硬件断点、单步、复位、三次重连通过。CDC 与持续 SWD 读取并发时 50 B、2 KB、8 KB、64 KB 回显一致；串口上传前主动断开 DAP。来源与修改见 [第三方声明](THIRD_PARTY_NOTICES.md)。 |
| USB 串口（`Serial`）与 RA4M1 用户串口（`Serial1`，uart0 GPIO34/35 经 U4 接 SCI9）双向透传；其他波特率设为 `Serial1` 的波特率（8N1），每次改动后清空未转发的接收数据 | experimental | 首块板（1V0），Linux：以 FT232 代替 RA4M1 接在 GPIO34/35，115200、9600、230400、921600 下双向各 1–8 KB 逐字节一致；921600 下双向同时各 64 KB 连续 3 轮一致；重新打开端口后跟随波特率。接 RA4M1（R47/R48 补焊后）：115200 下回显 50 字节、2 KB 一致 |
| 1200：MD 高、RESET 双击，RA4M1 进 bootloader；2400：MD 低、RESET 一次，RA4M1 进 ROM 下载模式，MD 保持低到下一次 1200（同官方 bridge） | experimental | 首块板（1V0），Linux：arduino-cli（renesas 1.6.0，unor4wifi）经 bridge 给 RA4M1 上传并运行，bossac 以 921600 连上 bootloader；2400 后 RA4M1 ROM 应答同步（0x00→0x00、0x55→0xC3），再经 1200 上传恢复；1200 时 BL616CL 不进下载模式 |
| AT 命令（`Serial2`，uart1 GPIO6/7 经 U4 接 RA4M1 SCI1，115200 8N1，独立任务处理）：WiFiS3 的 STA、AP 与 TCP 客户端，即 SOFTRESETWIFI、FWVERSION、WIFIMODE（0 OFF、1 STA、2 AP、3 STA+AP）、BEGINSTA、BEGINSOFTAP、SOFTAPCONFIG、IPSOFTAP、GETSOFTAPSSID、MACSOFTAP、GETSTATUS、IPSTA、GETSSID、GETRSSI、GETBSSID、MACSTA、DISCONNECT、HOSTBYNAME、PING、BEGINCLIENT、CLIENTCONNECTNAME、CLIENTCONNECT、CLIENTCONNECTIP、CLIENTSTATE、CLIENTSTATUS、IPCLIENT、PEEK、CLIENTSEND、AVAILABLE、CLIENTRECEIVE、CLIENTCONNECTED、CLIENTCLOSE、FLUSH、REMOTEIP、REMOTEPORT；失败回 `+<命令>:ERROR`（冒号后不带空格，否则 WiFiS3 在 CLIENTRECEIVE、WIFISCAN 等命令上认不出、要等满 10 s） | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0）：连 WPA3 网络 5.4 s 取得地址，DNS、ping 网关、按地址与域名连 80 端口 HTTP 收发、3000 字节 POST 正文写出、断开均正常；与局域网电脑上的 TCP 回显服务收发 1 B–4 KB 二进制负载（含 0x00、CRLF），两端逐字节一致；同一固件下透传与经 bridge 上传回归通过 |
| `AT+RESTARTBOOTLOADER`：回 `OK` 后复位进 BL616CL 的 ROM 下载模式，供更新本固件（上游直接复位、不应答） | experimental | 首块板（1V0），Linux：RA4M1 用未修改的 WiFiS3 发命令，0.6–0.9 s 后出现 ROM 下载口，经 USB 上传 bridge 后重新枚举，RA4M1 收到 `OK` 且未被复位，更新后 RA4M1 串口数据经 bridge 正常到达电脑；连续 6 轮通过（另一次运行第 4 轮上传中途 ROM 停止应答，见上文）。主机对接测试覆盖应答与复位调用 |
| `AT+WIFISCAN`（WiFiS3 `scanNetworks()`）：调用平台 `WiFi.scanNetworks()`，按上游格式列出最强的 10 个网络；没有网络时回 `+WIFISCAN: 0\|`（上游只回 `OK`） | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0）：未连接与已连接时扫描各 1.9–2.0 s 返回，字段有效、RSSI 有序，列出测试 AP 与当前 BSSID，扫描后连接保持、ping 网关成功；未修改的 ScanNetworks 例程连续 3 轮列出 8 个网络（10 个中 2 个隐藏网络被 WiFiS3 跳过）。主机对接测试覆盖 SSID 含空格与 32 字节、各加密方式、0 个网络与扫描失败 |
| UDP（WiFiS3 `WiFiUDP`）：UDPBEGIN、UDPBEGINPACKETIP、UDPBEGINPACKETADD（按域名）、UDPWRITE、UDPENDPACKET、UDPPARSE、UDPAVAILABLE、UDPREAD、UDPPEEK、UDPFLUSH、UDPREMOTEIP、UDPREMOTEPORT、UDPSTOP；同上游最多 4 个（0–3，与 TCP 编号独立），一个数据报最多 1460 字节；与 TCP 共用平台的 8 个 socket | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3：与局域网电脑往返 1–1460 字节二进制数据报 3 轮 18 个一致、无重发，往返 41–336 ms，另一来源端口的数据报回到该端口；未修改的 WiFiUdpSendReceiveString（电脑发 3 次都收到 acknowledged）、WiFiUdpNtpClient（3 次 NTP 时间与电脑相差 1.4–1.6 s）正常。主机对接测试覆盖分段写入、按域名发送、超过 1460 字节的写入失败后帧同步、4 个上限与释放后复用 |
| TCP 服务端（WiFiS3 `WiFiServer`）：BEGINSERVER、SERVERAVAILABLE、SERVERACCEPT、SERVERWRITE、SERVEREND；同上游最多 4 个服务端（0–3），接入的连接编号 1000–1007，之后走 AVAILABLE、CLIENTRECEIVE、CLIENTSEND、CLIENTCLOSE 等命令；语义同上游：SERVERAVAILABLE 只报模块侧有数据的连接，SERVERWRITE 发给未 accept 的连接，SERVEREND 只停止监听。WiFiS3 的 `read()` 把数据整段取进 RA4M1 本地缓冲，`server.available()` 换到别的编号时这些字节被丢弃，所以未修改的 WiFiChatServer 每段数据只回显首字符，上游 ESP32 固件也是这样（代码阅读，见协议文档）；用 `accept()` 并自己保存连接的写法（如 WiFiAdvancedChatServer）没有这个问题 | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0），电脑作客户端：WiFiWebServer 3 次 GET 都是 200、282 字节，发完后关闭连接；WiFiChatServer 两个客户端都收到逐字符发送的一行，整段发送只回显首字符；WiFiAdvancedChatServer 两个客户端得到编号 0、1，一个客户端的行只转给另一个，断开后例程检测到。主机对接测试覆盖同一连接多次返回、accept、广播、10 个连接轮换回收、end 后拒绝连接、4 个服务端上限与释放后复用、联网前 begin 失败 |
| TLS 客户端（WiFiS3 `WiFiSSLClient`）：SSLBEGINCLIENT、SETCAROOT（证书束，或最长 16384 字节的 PEM）、SSLCLIENTCONNECTNAME、SSLCLIENTCONNECT、SSLCLIENTCONNECTIP、SSLCLIENTSTATE、SSLCLIENTSTATUS、SSLIPCLIENT、SSLPEEK、SSLCLIENTSEND、SSLAVAILABLE、SSLCLIENTRECEIVE、SSLCLIENTCONNECTED、SSLCLIENTFLUSH、SSLREMOTEIP、SSLREMOTEPORT、SSLCLIENTCLOSE；编号 2000–2007（同上游 8 个）；调用平台 `WiFiSSLClient`，TLS 1.2、默认用包内根证书束、不查证书有效期，限制见 arduino-bl README；SETECCSLOT=<socket>,<slot>,<der-len> 写入软件证书/密钥槽，供后续 TLS 连接使用；同时能建立的连接受 BL616CL 内存限制，本固件实测 2 个，超出时 connect 失败 | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0）：证书束下 www.baidu.com、valid-isrgrootx2.letsencrypt.org 成功，self-signed、untrusted-root.badssl.com 失败；`setCACert` 测试 CA 后与电脑 TLS 服务往返 HTTP 100062 字节、字节和一致（约 11 s），回显一致，名称不符失败，过期证书成功；同时开 8 个连接 2 个成功，连做两次相同；只把 www.google.com 换成 www.baidu.com 的 WiFiWebClientSSL 打印 HTTP/1.1 200 后正常结束，未修改的例程因本测试网络连不上 google 而连接失败、正常结束。主机对接测试覆盖证书束与自定义 CA 的信任、SETCAROOT 载荷与帧同步、SETECCSLOT 软件槽、连接超时参数、对端关闭、TCP 与 TLS 编号混用、8 个编号上限 |
| 静态地址（WiFiS3 `config`、`setDNS`）：SETIP（地址、网关、掩码，可再带 1–2 个 DNS），调用平台 `WiFi.config`、`setDNS`；地址为 0.0.0.0 时恢复 DHCP，SOFTRESETWIFI 也恢复 DHCP（同上游）。WiFiS3 的 `config` 先发 SOFTAPCONFIG 设置 AP 地址，再发 SETIP 设置 STA 地址 | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3（renesas 1.6.0）：与平台相同的一组检查全部通过（连接前与已连接时设静态地址、断开重连、`setDNS`、恢复 DHCP、DHCP 下 `setDNS` 转为静态地址；ping 网关、DNS 解析、与电脑 TCP 回显、电脑 ping 板子）。主机对接测试覆盖已连接时生效、两个 DNS、重连保持、`config(地址)` 的默认值、参数不足与不合法地址、恢复 DHCP、SOFTRESETWIFI |
| AP（WiFiS3 `beginAP`、`softAPIP`、`softAPSSID`）：BEGINSOFTAP（SSID、密码、可选信道）、SOFTAPCONFIG、IPSOFTAP、GETSOFTAPSSID、MACSOFTAP；WIFIMODE=2 切换到 AP，WIFIMODE=1 / `begin` 回 STA；默认切换 STA/AP，显式 WIFIMODE=3 可并行 | experimental | 主机对接测试：78/78；红测 78 项中 8 项 AP 相关失败，合入后全绿。首块板（1V0）：run-ap-2 全套 34/34；电脑连入取得 DHCP 地址、状态 8、ping 应答，TCP/UDP 回显正常，SOFTRESETWIFI 停止 AP；AP→STA 的 `begin` 路径在平台实板与 bridge 主机测试中通过，bridge 实板尚未覆盖。证据见工作区 [AP 验证记录](../.tasks/2026-10-21-arduino-bl-platform/subtasks/ST008-bridge-features/work/2026-10-26-ap.txt) |
| `AT+FWVERSION_U32`（WiFiS3 `firmwareVersionU32()`）：版本三段各一个原始字节再加 0（同上游），WiFiS3 取前 3 字节；某段为 10、13、32 或 124 时 WiFiS3 的解析会出错（上游相同，本固件为 0.1.0） | experimental | 首块板（1V0），RA4M1 跑未修改的 WiFiS3：读到 0x000100，连读两次相同。主机对接测试覆盖 |
| BLE HCI over AT（HCIBEGIN、HCIEND、HCIWAIT、HCIAVAILABLE、HCIREAD、HCIWRITE） | experimental | Linux + BL616CL 1V0、libs 0.5.0、真实 RA4M1 使用未修改 ArduinoBLE 2.1.0：7 个官方示例通过；central/peripheral 各 3 次连接、60 轮 GATT 读写/通知；STA/TCP 与 BLE/USB 60 轮并发及 10 轮 BLE 启停通过。BL 仅运行 Controller；公共地址、RAM 密钥的数字比较/拒绝/超时/加密读写通知与密钥重连已实测；RPA、密钥持久化与长稳未验收，见下文及 ARDUINO-44 记录 |
| 文件 AT（MOUNTFS、FS 的删除/覆盖/读取/追加），调用平台 LittleFS | experimental | BL616CL 1V0 + 真实 RA4M1、官方 WiFiS3 1.6.0：10009 字节二进制、1023 字节读取、分段/非法帧/超时、满盘报错及删除复用通过；限制见下文及 ARDUINO-47 记录 |
| Preferences AT（九条 PREF 命令），调用平台 Preferences/LittleFS | experimental | 原版RA Modem类型往返、二进制、真实满盘/恢复及普通文件共存通过；见下文与ARDUINO-48 |
| SNTP 时间查询与 DHCP hostname（GETTIME、HOSTNAME） | experimental | BL616CL 1V0，时间与 DHCP option 12 实机证据见 ARDUINO-45 task；RAM 主机名、UTC 时钟与 TLS 策略限制见下文 |
| OTA | unimplemented | 对应 AT 命令回 `+<命令>:ERROR` |

## 文件 AT 使用边界

先发送 `AT+MOUNTFS=0`（或 `WiFiFileSystem.mount(false)`）。`AT+FS=0,1,/path,n` 后紧跟 n 个原始字节，覆盖/创建；op=3 追加，op=0 删除，op=2 的参数为 `/path,offset,length`。成功写入/删除回裸 `OK\r\n`；读取回 `+FS: <实际长度>|<原始字节>\r\nOK\r\n`。EOF、缺失文件及非法操作明确回 `+FS:ERROR\r\n`，没有独立的 size/exists AT 命令。

写/追加单帧 0–255 字节，读单帧最多 1023 字节；文件总大小受 FS 空闲容量限制。沿用公共缓冲，完整接收单帧后才操作 Flash，避免擦写期间 UART 载荷丢失；不在栈上缓存整文件。路径必须以 `/` 开头，拒绝 `..` 跳转；路径不能包含逗号、CRLF 或 NUL，整条 AT 行最多 256 字节。AT task 串行执行，不提供并发文件事务。有效写长度已解析时，失败也消费声明载荷；载荷间隔超过 1 秒则返回错误，主机须放弃旧载荷并开始新命令。失败覆盖/追加不保证回滚。

`MOUNTFS=1` 仍只尝试挂载，失败不自动格式化；这与上游 format-on-fault 不同，用于保护已有数据。默认使用平台 416 KiB 独立 FS；普通同布局 Arduino 上传保留 FS，改变布局不承诺自动迁移。WiFiFileSystem 的 `readfile()` 把数据当 C 字符串打印，精确二进制读取需使用原 Modem 的长度模式；到达 EOF 返回错误以结束其读取循环。Preferences 的私有路径 `/.preferences` 及其子路径禁止文件 AT 操作；PREF 合同与限制见下文。

## Preferences AT

九条 `PREFBEGIN/END/CLEAR/REMOVE/PUT/GET/TYPE/LEN/STAT` 遵循固定上游线合同，
通用存储实现在平台 `Preferences` library，bridge 只解析参数和收发原始载荷。
RA4M1 的官方 WiFiS3 1.6.0 只有命令名，没有 Preferences 封装；本项目以其未修改的
Modem 实测这些帧。命名空间/键各1–15字节，类型0–7为定长整数，8为字符串，9为BLOB，
单值最多1023字节。完整接收后才写 Flash；错误回 `+PREFx:ERROR\r\n`。
`PREFSTAT` 是当前 namespace 剩余逻辑键槽（64-count），与上游NVS全分区统计不同，
不能据此保证物理剩余空间。值提交失败返回0；空字符串成功也返回0，需按类型读取确认。
默认/空/lfs partition可用，其他partition拒绝；挂载/格式错误不自动format。
文件与Preferences共享416KiB容量，原子替换需临时空间，没有预留配额。
功能状态 experimental；真实RA原Modem全类型/1023字节往返和满盘错误/恢复通过。
满盘后PREF新增在未达64槽时返回0，旧键/其他namespace/普通文件保持。
挂载故障的6条真实RA帧均明确失败且完整416KiB FS读回未变，未自动格式化。
验收及实机/辅助证据分别记录于ARDUINO-48任务。

## BLE 使用边界

依赖固定 ArduinoBLE 2.1.0、Renesas core 1.6.0 和 libs 0.5.0。HCIREAD 每次最多 257 字节；HCIAVAILABLE 提供有限窗口，在完整 H4 包之后返回一次 0，让 poll 返回并避免 ArduinoBLE 的共用接收缓冲覆盖命令结果。HCIBEGIN 开始新 Host 会话；截断 HCIWRITE 失败会关闭 Controller，须重新 HCIBEGIN。请求和应答详见 [HCI AT 合同](../docs/reference/wifis3-at-protocol.md#ble-hci-over-at2026-10-27)。

ArduinoBLE 此版本的 AT transport `end()` 为空，释放 Controller 需通过 WiFiS3 modem 发 HCIEND；其 `wait()` 存在命令格式错误，带超时的 `BLE.poll(timeout)` 未验收。普通轮询路径已实测。Jira 提到的 HCIBUFFERALLOC 在固定官方 bridge/ArduinoBLE/WiFiS3 中不存在，本固件明确回 ERROR。

真实对端为 Intel AX200：官方 central 三例用 Bumble Host 直接驱动实体射频，peripheral 四例用 BlueZ/Bleak；均非模拟 HCI。BlueZ/Linux 对端曾先收到 ACL、后收到连接事件并丢首包；自定义 central 的 BlueZ 用例连接后等 250 ms，通过 3 连接/60 轮。官方例程和 ArduinoBLE 库均未修改。完整记录见 [ARDUINO-44](../.tasks/2026-10-27-arduino-44-ble-hci-arduino-ble/STATUS.md)。

配对实测使用自定义 RA4M1 安全探针和实体 AX200/Bumble，ArduinoBLE 库保持未修改：未加密访问返回 ATT 0x0F；数字比较接受/拒绝、8 秒确认超时、30 秒配对窗口到期、10 轮加密读写/通知、关闭新配对后的已有密钥重连通过。密钥仅留 RAM，不记录密钥或确认码。此测试采用公共地址；未验证 RPA 或掉电后的密钥恢复。负例与正例之间复位 RA4M1：该库可能跨连接保留受保护读取的延迟响应；不宣称覆盖这条连续恢复路径。

当前硬件未验收其余五个官方示例：LedControl、ButtonLED 缺按钮接线，BatteryMonitor 缺确定 A0 输入，EncryptedBatteryMonitor 还需板型/安全适配，SensorTagButton 缺 TI SensorTag 实物。12 例编译通过仅为软件辅助检查。

本轮 0.5.0 包额外通过 AP 回归：信道 1/6 在 BLE 开启前、开启中、关闭后均可被真实扫描发现；信道 6 下电脑连入、DHCP、3 次 ping、60 轮 TCP/UDP/BLE/USB 并发及 10 次 BLE 启停后 TCP 保持通过。此前扫描脚本未等待 NetworkManager 的 LastScan 更新，得到“未看到 AP”；加入扫描完成检查后通过，旧失败及修改前 bridge 对照日志保留，不能据此宣称 bridge 曾出现 AP 回归。

## SWD 调试与编程

USB HID 使用 vendor page `0xFF00`、无 report ID、64 字节 Input/Output/Feature。DAP 只走中断 OUT/IN，不能用 HID SET_REPORT Output 替代；Feature 走控制端点。DAP 与线路切换都在主循环执行，USB 中断只搬运数据。1200/2400 会断开 DAP、释放 SWD 并接管 RESET；调试工具随后需要重新连接。普通 CDC 透传可与 SWD 访问并行。

Linux 需要 USB 与 hidraw 访问权限，安装平台的 [udev 规则](../arduino-bl/tools/99-arduino-bl.rules) 后重插板子；未配置时 OpenOCD 可能报告找不到设备。已验证的调试入口：

```sh
openocd -f tools/ra4m1_swd.cfg
# 然后用 Cortex-M GDB 连接 localhost:3333。
```

该 OpenOCD 配置提供调试和内存访问，未配置 RA4M1 Flash 驱动。编程使用 pyOCD 0.45.0 与 `Renesas.RA_DFP` 6.6.0；先 `pyocd pack install r7fa4m1ab`，用 `pyocd list` 查板子的 USB 序列号。仅将 `arduino:renesas_uno:unor4wifi` 生成的 sketch `.bin` 写到 `0x4000`，长度不得超过 `0x3C000`。`0x0000–0x3FFF` 是 bootloader，DataFlash/配置区不在此上传范围；先保存需要保留的原始固件。

```sh
pyocd commander -u <USB序列号> -t r7fa4m1ab -f 100000 \
  -O auto_unlock=false -O chip_erase=sector \
  -O smart_flash=false -O keep_unwritten=true \
  -c "reset halt" -c "load sketch.bin 0x4000" \
  -c "compare 0x4000 sketch.bin" -c reset
```

编程前的 `reset halt` 必须保留：本轮从运行中的 Arduino 程序直接加载 Flash 算法时初始化失败，明确复位并停止后编程通过。SWCLK 是软件产生的，请求 100 kHz 已验证；GPIO 与调度开销会降低实际频率。1V1、其他主机、JTAG/SWO 和更高频率没有本轮证据。

## 测试

运行中的 bridge 可执行 `python3 tests/dap_hid_check.py /dev/hidrawN`，核对设备身份、能力/包长、错误命令、非法时钟、连续请求和 Feature 版本，共 110 项；不改写目标内存。SWD 编程、调试和 USB/串口组合实板记录在工作区 [ARDUINO-42 任务](../.tasks/2026-10-26-arduino-42-usb-dap/STATUS.md)。

`tests/at_host/run.py` 在电脑上做 AT 对接测试：RA4M1 侧是未修改的 WiFiS3（直接编译已安装的 `arduino:renesas_uno` 包里的源码），BL616CL 侧是本仓库的 AT 服务端加 arduino-bl 的 WiFi 库，其中 Wi-Fi 换成虚拟 AP、socket 换成本机 socket；两个程序经 socketpair 相连，逐项核对扫描、联网、地址查询、DNS、ping、TCP 收发、UDP 收发、TCP 服务端、TLS 客户端、静态地址、AP 与 STA/AP 切换、固件版本、socket 上限、错误应答与 RESTARTBOOTLOADER，共 78 项；UDP 对端是本脚本在 127.0.0.1 起的 UDP 回显服务，TCP 服务端的对端是本脚本中的客户端线程；BLEController 替身固定不可用，不检验 BLE；TLS 换成不加密的替身（`tests/at_host/bl/fake_tls_port.c`），只核对命令把主机名、证书参数与数据交给平台，不检验加密与证书校验。需要 `g++`、`python3`，arduino-bl 默认取本仓库旁边的目录（`--platform` 可改）：

```sh
python3 tests/at_host/run.py --renesas <arduino 数据目录>/packages/arduino/hardware/renesas_uno/1.6.0
```

设环境变量 `AT_HOST_TRACE=1` 时，BL 侧把串口上的原始收发写到 `bl_host.stderr`。主机测试不代替实板结果。

文件命令另有 `python3 tests/at_host/file_check.py <上述编译目录>/bl_host`：32 项软件/fake 检查，存储由 POSIX 文件替代，注入写入、提交及挂载失败；不运行真实 littlefs 或 Flash，不计入实机通过项。真实 RA4M1、固件身份、原始失败/修正和恢复证据见工作区 [ARDUINO-47](../.tasks/2026-10-28-arduino-47-file-at/STATUS.md)。

AT 协议与命令子集见 [WiFiS3 AT 协议](../docs/reference/wifis3-at-protocol.md)。`AT+FWVERSION` 返回本固件版本 0.1.0，WiFiS3 示例按字符串与 0.6.0 比较后会打印升级提示，不影响使用。`AT+BEGINSTA` 把第一个逗号之后的内容都当作密码（含逗号的密码经主机测试验证）。

Preferences 命令辅助检查：`python3 tests/at_host/pref_check.py <编译目录>/bl_host`。
172 项软件/fake，使用 POSIX 存储并注入写入/close/rename/挂载失败，不运行真实 littlefs/Flash；
实机溢出失败与修复后回归分别留证于 [ARDUINO-48](../.tasks/2026-10-28-arduino-48-preferences/STATUS.md)。

## WiFi/net AT 补全（ARDUINO-49）

`WIFIMODE=0..3[,autoconnect]` / `WIFIMODE?` 由平台管理接口生命周期。`DISCONNECT[=wifi_off]` 只断 STA，`DISCONNECTSOFTAP[=wifi_off]` 只停 AP，默认参数0保留模式位，1移除对应位；另一接口继续运行。`SETIP` 调用平台独立 `configSTA`，`SOFTAPCONFIG` 调用 `softAPConfig`，在线/离线配置不再共用。

`RECONNECT?` / `RECONNECT=0|1` 控制意外掉线重连，默认1；显式断开抑制重试，下一次 begin 取得 IP 后恢复策略。`AUTOCONNECT?` 始终0、setter及 WIFIMODE 第二参数不生效：这是固定官方 ESP32 子模块已废弃的占位合同，不是开机自动联网功能，也不保存凭据。`IPV6` 启用 STA link-local，`IPV6?` 返回有效地址或 `::`。

`BEGINSOFTAP=ssid[,pass[,channel[,hidden[,max_connection]]]]` 默认 open / 1 / 0 / 5。隐藏必须影响 beacon 与 wildcard probes；关联上限接受1..5，SDK最多4槽，默认5有效上限4。非法帧回 `+CMD:ERROR`；可解析但无效的 AP 参数回0，不能停掉当前正常连接。AP地址当前支持/24，变更地址会重启DHCP。mode2的 `IPSTA=1/2` 按既有 WiFiS3 行为返回 AP 网关/掩码，mode3返回独立STA配置；`IPSTA=0` 始终为STA地址。

需要底层 `bl616cl-libs` 0.7.0 与配套平台源码。首块板验证与缺项以本轮 task 记录为准；主机/fake 检查不能证明无线行为，单客户端不能验收多客户端关联上限。

## 时间与主机名（ARDUINO-45）

`AT+GETTIME=` 立即读取平台 `WiFi.getTime()`，未同步回 0，不等待 SNTP。`AT+HOSTNAME?` 查询，`AT+HOSTNAME=name` 调用平台 setter；名称校验、RAM 生命周期及 DHCP 生效时机属于平台。默认 SNTP 为 `pool.ntp.org`；新增命令不改 TLS 策略。需要完整 `bl616cl-libs` 0.8.0 和配套平台，协议与明确的上游差异见工作区 `docs/reference/wifis3-at-protocol.md`，实机验收见 ARDUINO-45 task。

## UDP 组播与目标复用（ARDUINO-50）

依赖平台及`bl616cl-libs` 0.9.0。`AT+UDPBEGINMULTI=port,groupIP`分配0..3的UDP编号，加入IPv4组；`AT+BEGINPACKETMULTI=sock`选入组目标，`AT+UDPBEGINPACKET=sock`复用最近显式/组播目标或收到的数据报来源。后两条成功应答空结果及OK，仍通过UDPWRITE/UDPENDPACKET发送。无当前目标、非组播socket调用组播begin、非法编号/组/端口和缺参返回命令ERROR，失败不占编号。UDPSTOP关闭socket，由平台/lwIP离组。

组播port必须1..65535、地址224..239开头且符合底层组限制；0.0.0.0不退化为普通UDP，数字严格解析，这是与固定官方ESP32的宽松入参差异。4个UDP对象与TCP/TLS共用平台8个socket；耗尽明确失败，关闭后可复用。1460字节上限与错误载荷消费/帧同步合同保持不变。WiFiS3会发送前两种命令；无参数UDPBEGINPACKET也按固定官方真实实现覆盖。

当前experimental：真实RA4M1使用未修改WiFiS3经vela双向1/255/1460字节组播、来源IP/端口、离组后无接收、四组同端口、20轮循环、编号复用、三种目标复用、UART错误帧同步及UDP/TCP/TLS资源边界已实机验证，最终固件与证据见独立ARDUINO-50任务。软件/fake IGMP仅验证协议，不能替代此实机记录。无线组播可能丢报，不能推导为可靠传输或长稳验收；STA/AP并行沿用同信道限制。
