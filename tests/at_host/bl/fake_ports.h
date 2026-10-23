#pragma once

/* 虚拟网络的固定参数（主机字节序），run.py 按同样的值核对 */
#include <stdbool.h>

#define FAKE_IP    0x7F000001u /* 127.0.0.1，回显服务也在这里 */
#define FAKE_GW    0x7F000001u
#define FAKE_MASK  0xFF000000u
#define FAKE_DNS   0x7F000035u /* 127.0.0.53 */
#define FAKE_RSSI  (-42)
#define FAKE_MAC   { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }
#define FAKE_BSSID { 0x02, 0x00, 0x00, 0x00, 0x00, 0xAA }
#define FAKE_HOST  "echo.test" /* 唯一能解析的域名，解析到 FAKE_IP */

#ifdef __cplusplus
extern "C" {
#endif
/* wifi_port_init 调用过；之前调用 net_port 即违反平台约定（实板上 lwIP 的 tcpip 线程尚未创建） */
extern bool fake_wifi_up;
#ifdef __cplusplus
}
#endif
