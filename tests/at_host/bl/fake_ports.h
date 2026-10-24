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
/* TLS 替身：根证书束只认 FAKE_HOST；setCACert 给的 PEM 必须与 FAKE_CA_PEM 逐字节相同（本机生成的测试 CA） */
#define FAKE_CA_PEM \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIBbjCCARSgAwIBAgIUPWXV8T0fBmdE0eKUp621QrZsl1AwCgYIKoZIzj0EAwIw\n" \
    "HTEbMBkGA1UEAwwSYXJkdWluby1ibCB0ZXN0IENBMB4XDTI2MTAyNDA5MTcyM1oX\n" \
    "DTM2MTAyMTA5MTcyM1owHTEbMBkGA1UEAwwSYXJkdWluby1ibCB0ZXN0IENBMFkw\n" \
    "EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYUH5p3Y4wObRzSWgxo7puKc5lFSQ+ATt\n" \
    "VuhNDColXMxc3bMbcIOqWzjuuqmU/BsM2Hkxzg8NbRXWag030HQJ/qMyMDAwHQYD\n" \
    "VR0OBBYEFPz7hy3GviF2JKYO5HoaH+Dwf49tMA8GA1UdEwEB/wQFMAMBAf8wCgYI\n" \
    "KoZIzj0EAwIDSAAwRQIgLjU9e7qNScnRl93FNewDLeNt6l7i9NV0pjQfrfojXtEC\n" \
    "IQCsjHa00XSN0CA39iEokf62PtKPcgb3ok+poS9dD9Pm+g==\n" \
    "-----END CERTIFICATE-----\n"

#ifdef __cplusplus
extern "C" {
#endif
/* wifi_port_init 调用过；之前调用 net_port 即违反平台约定（实板上 lwIP 的 tcpip 线程尚未创建） */
extern bool fake_wifi_up;
#ifdef __cplusplus
}
#endif
