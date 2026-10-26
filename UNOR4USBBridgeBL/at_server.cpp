/* WiFiS3 AT 服务端：行解析、命令表与命令处理，接口说明见 at_server.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WiFi.h"
#include "at_server.h"

#define FW_VERSION         "0.1.0" /* 本固件版本，AT+FWVERSION 返回 */
#define LINE_MAX           256     /* WiFiS3 的请求最长 128 字节 */
#define MAX_SOCKETS        8       /* 同上游：BEGINCLIENT 分配 0–7 */
#define MAX_UDP            4       /* 同上游：UDPBEGIN 分配 0–3，与 TCP 的编号各自独立 */
#define MAX_SERVERS        4       /* 同上游：BEGINSERVER 分配 0–3 */
#define SERVER_SOCK_BASE   1000    /* 同上游：服务端接入的连接编号 1000–1007，走 CLIENT* 命令 */
#define MAX_SSL            8       /* 同上游：SSLBEGINCLIENT 分配 2000–2007 */
#define SSL_SOCK_BASE      2000
#define CA_MAX             16384   /* SETCAROOT 的 PEM 上限 */
#define CONNECT_TIMEOUT_MS 8000    /* WiFiS3 每条命令等 10 s，连接要在此之前应答 */
#define PAYLOAD_TIMEOUT_MS 1000    /* CLIENTSEND 载荷两段之间的最长间隔 */
#define RECV_MAX           1023    /* WiFiS3 每次 CLIENTRECEIVE 最多要 1023 字节 */

typedef void (*handler_t)(Stream &io, const char *name, char op, char *args);

static char line[LINE_MAX + 1];
static size_t line_len;
static bool line_overflow;
/* CLIENTSEND、UDPWRITE 的分块，CLIENTRECEIVE、UDPREAD、WIFISCAN 的应答共用；UDPREAD 一次最多一个数据报 */
static uint8_t data[WIFI_UDP_BUFFER_SIZE];

static int wifi_mode;     /* AT+WIFIMODE：0 未设置，1 STA，2 AP */
static bool wifi_started; /* 发起过 BEGINSTA 或 BEGINSOFTAP：Wi-Fi 协议栈已初始化，断开、停止 AP 才有意义 */
static WiFiClient clients[MAX_SOCKETS];
static bool allocated[MAX_SOCKETS];
static WiFiUDP udps[MAX_UDP];
static bool udp_allocated[MAX_UDP];
static WiFiServer servers[MAX_SERVERS];
static bool server_allocated[MAX_SERVERS];
static WiFiSSLClient ssls[MAX_SSL];
static bool ssl_allocated[MAX_SSL];
static char *ssl_ca[MAX_SSL]; /* SETCAROOT 给的 PEM（'\0' 结尾）；为空时用根证书束 */
/* 服务端接入的连接，编号 SERVER_SOCK_BASE + i；server 为 -1 表示空位。accepted 为假的由 SERVERAVAILABLE 收下，
   SERVERWRITE 发给它们（同上游）；为真的由 SERVERACCEPT 交给主机 */
static struct {
    WiFiClient client;
    int server = -1;
    bool accepted;
} conns[MAX_SOCKETS];

static void reply(Stream &io, const char *name, const char *payload)
{
    io.print(name);
    io.print(": ");
    io.print(payload);
    io.print("\r\nOK\r\n");
}

static void reply_int(Stream &io, const char *name, long value)
{
    char s[12];
    snprintf(s, sizeof(s), "%ld", value);
    reply(io, name, s);
}

static void reply_mac(Stream &io, const char *name, const uint8_t *mac)
{
    char s[18];
    snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    reply(io, name, s);
}

/*
 * 失败一律回 "+<命令>:ERROR"：WiFiS3 把 RESULT_ERROR 误定义为 "OK"，收到裸 "ERROR" 要等满超时。
 * 冒号后不带空格：WIFISCAN、CLIENTRECEIVE 等命令之前主机调用 avoid_trim_results，不去空格，" ERROR" 认不出
 */
static void reply_error(Stream &io, const char *name)
{
    io.print(name);
    io.print(":ERROR\r\n");
}

/* "+<命令>: <n>|" 加 data 的前 n 个原始字节再加 "OK"，中间没有换行；主机按长度读，载荷可含任意字节 */
static void reply_data(Stream &io, const char *name, size_t n)
{
    io.print(name);
    io.print(": ");
    io.print(n);
    io.print('|');
    io.write(data, n);
    io.print("OK\r\n");
}

/* 按逗号把 args 原地切成最多 max 段，最后一段保留其余逗号；返回段数 */
static int split(char *args, char **argv, int max)
{
    int n = 0;
    char *p = args;
    while (n < max) {
        argv[n++] = p;
        if (n == max || (p = strchr(p, ',')) == NULL) {
            break;
        }
        *p++ = '\0';
    }
    return n;
}

static bool parse_long(const char *s, long *value)
{
    char *end;
    *value = strtol(s, &end, 10);
    return end != s && *end == '\0';
}

/* SSLBEGINCLIENT 分配的连接号 2000–2007 对应的位置，否则 -1 */
static int ssl_slot(const char *s)
{
    long v;
    long i = parse_long(s, &v) ? v - SSL_SOCK_BASE : -1;
    return i >= 0 && i < MAX_SSL && ssl_allocated[i] ? (int)i : -1;
}

/*
 * 命令参数中的已分配连接号，否则 -1。SSL 命令（名称以 "+SSL" 开头）与 CLIENT 类命令共用处理函数，
 * 前者只接受 SSLBEGINCLIENT 的 2000–2007，后者只接受 BEGINCLIENT 的 0–7 与服务端接入的 1000–1007
 */
static int socket_arg(const char *name, const char *s)
{
    long v;
    if (strncmp(name, "+SSL", 4) == 0) {
        int i = ssl_slot(s);
        return i >= 0 ? SSL_SOCK_BASE + i : -1;
    }
    if (!parse_long(s, &v)) {
        return -1;
    }
    if (v >= 0 && v < MAX_SOCKETS) {
        return allocated[v] ? (int)v : -1;
    }
    long i = v - SERVER_SOCK_BASE;
    return i >= 0 && i < MAX_SOCKETS && conns[i].server >= 0 ? (int)v : -1;
}

static WiFiClient &client_of(int s)
{
    if (s >= SSL_SOCK_BASE) {
        return ssls[s - SSL_SOCK_BASE];
    }
    return s < SERVER_SOCK_BASE ? clients[s] : conns[s - SERVER_SOCK_BASE].client;
}

static void close_socket(int s)
{
    client_of(s).stop();
    if (s < SERVER_SOCK_BASE) {
        allocated[s] = false;
    } else if (s < SSL_SOCK_BASE) {
        conns[s - SERVER_SOCK_BASE].server = -1;
    } else {
        ssl_allocated[s - SSL_SOCK_BASE] = false;
        free(ssl_ca[s - SSL_SOCK_BASE]);
        ssl_ca[s - SSL_SOCK_BASE] = nullptr;
    }
}

/* 已由 UDPBEGIN 分配的 UDP 编号，否则 -1 */
static int udp_arg(const char *s)
{
    long v;
    return parse_long(s, &v) && v >= 0 && v < MAX_UDP && udp_allocated[v] ? (int)v : -1;
}

static void close_udp(int u)
{
    udps[u].stop();
    udp_allocated[u] = false;
}

/*
 * 读命令行之后紧跟的 left 个原始字节，边收边写给 sink（为空时只读不写）。出错也读完，保持帧同步；
 * 两段之间超过 PAYLOAD_TIMEOUT_MS 时放弃。全部收到且写出返回 true
 */
static bool read_payload(Stream &io, long left, Print *sink)
{
    bool ok = sink != nullptr;
    unsigned long last = millis();
    while (left > 0) {
        long n = io.available();
        if (n <= 0) {
            if (millis() - last > PAYLOAD_TIMEOUT_MS) {
                return false;
            }
            delay(1);
            continue;
        }
        n = io.readBytes(data, min(n, min(left, (long)sizeof(data))));
        last = millis();
        left -= n;
        if (ok && sink->write(data, n) != (size_t)n) {
            ok = false;
        }
    }
    return ok;
}

/* 主机每次启动后第一个 Wi-Fi 调用前发送，只等 500 ms：先应答，再关 socket、断开 */
static void cmd_softreset(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
    for (int s = 0; s < MAX_SOCKETS; s++) {
        close_socket(s);
    }
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (conns[i].server >= 0) {
            close_socket(SERVER_SOCK_BASE + i);
        }
    }
    for (int v = 0; v < MAX_SERVERS; v++) {
        servers[v].end();
        server_allocated[v] = false;
    }
    for (int i = 0; i < MAX_SSL; i++) {
        close_socket(SSL_SOCK_BASE + i);
    }
    for (int u = 0; u < MAX_UDP; u++) {
        close_udp(u);
    }
    if (wifi_started) {
        WiFi.end(); /* 断开 STA、停止 AP，同上游 */
    }
    WiFi.config(IPAddress(0, 0, 0, 0)); /* 恢复 DHCP，同上游 */
    wifi_mode = 0;
}

static void cmd_fwversion(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply(io, name, FW_VERSION);
}

/* AT+FWVERSION_U32?：FW_VERSION 三段各一个原始字节再加 0（同上游 cmds_esp_generic.h），WiFiS3 取前 3 字节 */
static void cmd_fwversion_u32(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    unsigned v[3];
    if (op != '?' || sscanf(FW_VERSION, "%u.%u.%u", &v[0], &v[1], &v[2]) != 3) {
        reply_error(io, name);
        return;
    }
    const uint8_t b[4] = { (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], 0 };
    io.print(name);
    io.print(": ");
    io.write(b, sizeof(b));
    io.print("\r\nOK\r\n");
}

static bool ap_running()
{
    uint8_t st = WiFi.status();
    return st == WL_AP_LISTENING || st == WL_AP_CONNECTED;
}

/*
 * 1 STA、2 AP，同上游：切到 STA 时停止 AP（STA 不断开，WiFiS3 每次 begin 都先发 WIFIMODE=1），切到 AP 时断开 STA。
 * 0（关闭）与 3（STA 与 AP 同时运行）未实现
 */
static void cmd_wifimode(Stream &io, const char *name, char op, char *args)
{
    if (op == '?') {
        reply_int(io, name, wifi_mode);
    } else if (op == '=' && (strcmp(args, "1") == 0 || strcmp(args, "2") == 0)) {
        wifi_mode = args[0] - '0';
        if (wifi_mode == 1 && ap_running()) {
            WiFi.end(); /* AP 运行时 STA 未连接，这里只停止 AP */
        } else if (wifi_mode == 2 && wifi_started) {
            WiFi.disconnect();
        }
        reply(io, name, "");
    } else {
        reply_error(io, name);
    }
}

/* AT+BEGINSTA=<ssid>[,<密码>]：发起连接即应答，主机随后轮询 GETSTATUS。第一个逗号之后都算密码 */
static void cmd_beginsta(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    if (op != '=') {
        reply_error(io, name);
        return;
    }
    int n = split(args, argv, 2);
    WiFi.setTimeout(0);
    int status = WiFi.begin(argv[0], n > 1 ? argv[1] : nullptr);
    if (status == WL_NO_MODULE || status == WL_CONNECT_FAILED) {
        reply_error(io, name);
        return;
    }
    wifi_started = true;
    reply(io, name, "");
}

/* WL_CONNECTED（3）只在取得地址后出现 */
static void cmd_getstatus(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, WiFi.status());
}

/* AT+IPSTA=<n>：0 地址、1 网关、2 掩码、3/4 DNS */
static void cmd_ipsta(Stream &io, const char *name, char op, char *args)
{
    long n;
    IPAddress ip;
    if (op != '=' || !parse_long(args, &n) || n < 0 || n > 4) {
        reply_error(io, name);
        return;
    }
    switch (n) {
        case 0: ip = WiFi.localIP(); break;
        case 1: ip = WiFi.gatewayIP(); break;
        case 2: ip = WiFi.subnetMask(); break;
        default: ip = WiFi.dnsIP(n - 3); break;
    }
    reply(io, name, ip.toString().c_str());
}

/*
 * AT+SETIP=<地址>,<网关>,<掩码>[,<DNS1>[,<DNS2>]]（同上游，WiFiS3 的 config、setDNS 总是给 5 个）：静态地址，
 * 地址为 0.0.0.0 时恢复 DHCP；为 0.0.0.0 的 DNS 不改
 */
static void cmd_setip(Stream &io, const char *name, char op, char *args)
{
    char *argv[5];
    IPAddress ip[5];
    int n = op == '=' ? split(args, argv, 5) : 0;
    bool ok = n >= 3;
    for (int i = 0; i < n && ok; i++) {
        ok = ip[i].fromString(argv[i]);
    }
    if (!ok) {
        reply_error(io, name);
        return;
    }
    WiFi.config(ip[0], ip[3], ip[1], ip[2]);
    if (n == 5 && ip[0] != IPAddress(0, 0, 0, 0)) {
        WiFi.setDNS(ip[3], ip[4]);
    }
    reply(io, name, "");
}

/*
 * AT+BEGINSOFTAP=<ssid>[,<密码>[,<信道>]]：应答 1（WL_AP_LISTENING）或 0，同上游；底层启动失败后 GETSTATUS 为 9，
 * 平台拒绝的参数（密码不足 8 个字符、信道不在 1–13）只回 0。空密码为开放网络。
 * 上游的第 4、5 个参数（隐藏 SSID、最大连接数，WiFiS3 不发）未实现，回 ERROR
 */
static void cmd_beginsoftap(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    long ch = 1;
    int n = op == '=' ? split(args, argv, 3) : 0;
    if (n == 0 || argv[0][0] == '\0' || (n == 3 && !parse_long(argv[2], &ch))) {
        reply_error(io, name);
        return;
    }
    wifi_started = true;
    uint8_t st = WiFi.beginAP(argv[0], n > 1 ? argv[1] : "", ch >= 0 && ch <= 255 ? (uint8_t)ch : 0);
    reply(io, name, st == WL_AP_LISTENING ? "1" : "0");
}

/*
 * AT+SOFTAPCONFIG=<地址>,<网关>,<掩码>：AP 的地址与掩码。平台的 STA 与 AP 共用一套地址配置（同 WiFiNINA），这里按
 * config(地址, 网关, 网关, 掩码) 设置，也改 STA 的静态地址；WiFiS3 的 config 随后总会发 SETIP 设好 STA，结果与上游相同
 */
static void cmd_softapconfig(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    IPAddress ip[3];
    int n = op == '=' ? split(args, argv, 3) : 0;
    bool ok = n == 3;
    for (int i = 0; i < n && ok; i++) {
        ok = ip[i].fromString(argv[i]);
    }
    if (!ok) {
        reply_error(io, name);
        return;
    }
    WiFi.config(ip[0], ip[1], ip[1], ip[2]);
    reply(io, name, "");
}

/* AT+IPSOFTAP（不带 '?'，同上游）：AP 未运行时为 0.0.0.0 */
static void cmd_ipsoftap(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    reply(io, name, WiFi.softAPIP().toString().c_str());
}

static void cmd_getsoftapssid(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply(io, name, WiFi.softAPSSID());
}

/* SDK 的 AP 与 STA 接口用同一个 MAC 地址 */
static void cmd_macsoftap(Stream &io, const char *name, char op, char *args)
{
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_mac(io, name, WiFi.macAddress(mac));
}

static void cmd_getssid(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply(io, name, WiFi.SSID());
}

static void cmd_getrssi(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, WiFi.RSSI());
}

static void cmd_getbssid(Stream &io, const char *name, char op, char *args)
{
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_mac(io, name, WiFi.BSSID(mac));
}

static void cmd_macsta(Stream &io, const char *name, char op, char *args)
{
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_mac(io, name, WiFi.macAddress(mac));
}

/* 加密方式用上游的名称，WiFiS3 按名称换回 wl_enc_type */
static const char *enc_name(uint8_t enc)
{
    switch (enc) {
        case ENC_TYPE_NONE: return "open";
        case ENC_TYPE_WEP: return "WEP";
        case ENC_TYPE_WPA: return "WPA";
        case ENC_TYPE_WPA2: return "WPA2";
        case ENC_TYPE_WPA2_ENTERPRISE: return "WPA2-EAP";
        case ENC_TYPE_WPA3: return "WPA3";
        default: return "unknown";
    }
}

/*
 * AT+WIFISCAN：应答 "+WIFISCAN: <n>|" 加 n 字节的列表再加 "OK"，每个网络一行
 * "<ssid> | <bssid> | <rssi> | <信道> | <加密>\r\n"。没有网络时 n 为 0（上游只回 "OK"）
 */
static void cmd_wifiscan(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    int n = op == '\0' ? WiFi.scanNetworks() : -1;
    if (n < 0) {
        reply_error(io, name);
        return;
    }
    /* 10 行，每行最多 32 字节 SSID 加约 45 字节，放得进 data */
    char *list = (char *)data;
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b[WL_MAC_ADDR_LENGTH];
        WiFi.BSSID(i, b);
        len += snprintf(list + len, sizeof(data) - len, "%s | %02X:%02X:%02X:%02X:%02X:%02X | %ld | %u | %s\r\n",
                        WiFi.SSID(i), b[0], b[1], b[2], b[3], b[4], b[5], (long)WiFi.RSSI(i), WiFi.channel(i),
                        enc_name(WiFi.encryptionType(i)));
    }
    reply_data(io, name, len);
}

static void cmd_disconnect(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    if (wifi_started) {
        WiFi.disconnect();
    }
    reply(io, name, "");
}

static void cmd_hostbyname(Stream &io, const char *name, char op, char *args)
{
    IPAddress ip;
    if (op != '=' || !WiFi.hostByName(args, ip)) {
        reply_error(io, name);
        return;
    }
    reply(io, name, ip.toString().c_str());
}

/* WiFiS3 发 "AT+PING=,<主机>,<ttl>,<次数>"（等号后多一个逗号）；返回平均往返毫秒数或 WL_PING_* */
static void cmd_ping(Stream &io, const char *name, char op, char *args)
{
    char *argv[4];
    long ttl, count;
    if (op != '=') {
        reply_error(io, name);
        return;
    }
    int n = split(args, argv, 4);
    char **a = n == 4 && argv[0][0] == '\0' ? argv + 1 : argv;
    if (a + 3 != argv + n || !parse_long(a[1], &ttl) || ttl < 1 || ttl > 255 || !parse_long(a[2], &count) ||
        count < 1 || count > 255) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, WiFi.ping(a[0], (uint8_t)ttl, (uint8_t)count));
}

static void cmd_beginclient(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op == '\0') {
        for (int s = 0; s < MAX_SOCKETS; s++) {
            if (!allocated[s]) {
                allocated[s] = true;
                reply_int(io, name, s);
                return;
            }
        }
    }
    reply_error(io, name);
}

/* SSL 连接先设好 SETCAROOT 给的证书。服务端接入的连接不能再连 */
static void connect_socket(Stream &io, const char *name, char **argv, long timeout_ms)
{
    long port;
    IPAddress ip;
    int s = socket_arg(name, argv[0]);
    if (s < 0 || (s >= SERVER_SOCK_BASE && s < SSL_SOCK_BASE) || !parse_long(argv[2], &port) || port < 1 ||
        port > 65535) {
        reply_error(io, name);
        return;
    }
    if (s >= SSL_SOCK_BASE) {
        ssls[s - SSL_SOCK_BASE].setCACert(ssl_ca[s - SSL_SOCK_BASE]);
    }
    client_of(s).setConnectionTimeout(timeout_ms);
    /* 点分地址的 TCP 连接用 connect(IPAddress)，AP 运行时也可用；域名与 TLS 要 STA 已连接。是否联网由平台判断 */
    bool by_ip = s < SSL_SOCK_BASE && ip.fromString(argv[1]);
    if (!(by_ip ? client_of(s).connect(ip, (uint16_t)port) : client_of(s).connect(argv[1], (uint16_t)port))) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

/* AT+CLIENTCONNECTNAME=<sock>,<主机>,<端口> */
static void cmd_clientconnectname(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    if (op != '=' || split(args, argv, 3) != 3) {
        reply_error(io, name);
        return;
    }
    connect_socket(io, name, argv, CONNECT_TIMEOUT_MS);
}

/* AT+CLIENTCONNECT=<sock>,<主机>,<端口>,<超时毫秒>（WiFiClient::setConnectionTimeout 之后使用）；超时不超过 8 s */
static void cmd_clientconnect(Stream &io, const char *name, char op, char *args)
{
    char *argv[4];
    long timeout_ms;
    if (op != '=' || split(args, argv, 4) != 4 || !parse_long(argv[3], &timeout_ms)) {
        reply_error(io, name);
        return;
    }
    connect_socket(io, name, argv, timeout_ms > 0 && timeout_ms < CONNECT_TIMEOUT_MS ? timeout_ms : CONNECT_TIMEOUT_MS);
}

/*
 * AT+CLIENTSEND=<sock>,<n> 后紧跟 n 个原始字节。边收边发往 socket；出错也读完 n 字节，保持帧同步。
 * 成功同上游只回裸 "OK"（主机此时不核对前缀）。
 */
static void cmd_clientsend(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long left;
    if (op != '=' || split(args, argv, 2) != 2 || !parse_long(argv[1], &left) || left < 0) {
        reply_error(io, name);
        return;
    }
    int s = socket_arg(name, argv[0]);
    if (read_payload(io, left, s >= 0 ? &client_of(s) : nullptr)) {
        io.print("OK\r\n");
    } else {
        reply_error(io, name);
    }
}

static void cmd_available(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, client_of(s).available());
}

/* AT+CLIENTRECEIVE=<sock>,<max>：应答见 reply_data */
static void cmd_clientreceive(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long max;
    int s = -1;
    if (op == '=' && split(args, argv, 2) == 2 && parse_long(argv[1], &max) && max >= 0) {
        s = socket_arg(name, argv[0]);
    }
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    size_t want = min(max, (long)RECV_MAX);
    size_t got = 0;
    while (got < want) {
        int n = client_of(s).read(data + got, want - got);
        if (n <= 0) {
            break;
        }
        got += n;
    }
    reply_data(io, name, got);
}

static void cmd_clientconnected(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, client_of(s).connected() ? 1 : 0);
}

static void cmd_clientclose(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    close_socket(s);
    reply(io, name, "");
}

/* 平台 WiFiClient 的发送不经缓冲，flush 无事可做 */
static void cmd_flush(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    client_of(s).flush();
    reply(io, name, "");
}

static void cmd_remoteip(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply(io, name, client_of(s).remoteIP().toString().c_str());
}

static void cmd_remoteport(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, client_of(s).remotePort());
}

/* 只有 SSL 连接有 PEEK 命令（WiFiS3 的 WiFiClient::peek 只看本地缓冲） */
static void cmd_peek(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, client_of(s).peek());
}

static void cmd_sslbeginclient(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op == '\0') {
        for (int i = 0; i < MAX_SSL; i++) {
            if (!ssl_allocated[i]) {
                ssl_allocated[i] = true;
                reply_int(io, name, SSL_SOCK_BASE + i);
                return;
            }
        }
    }
    reply_error(io, name);
}

/* 把载荷写进定长缓冲，写满即失败 */
class BufferSink : public Print {
public:
    BufferSink(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap) {}
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t *p, size_t n) override
    {
        if (n > cap_ - len_) {
            return 0;
        }
        memcpy(buf_ + len_, p, n);
        len_ += n;
        return n;
    }

private:
    uint8_t *buf_;
    size_t cap_;
    size_t len_ = 0;
};

/*
 * AT+SETCAROOT=<sock>：之后的连接用根证书束，回 "+SETCAROOT: " 加 OK。
 * AT+SETCAROOT=<sock>,<n> 后紧跟 n 字节 PEM：之后的连接只信任它；同上游只回裸 "OK"，出错也读完 n 字节
 */
static void cmd_setcaroot(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long n;
    int argc = op == '=' ? split(args, argv, 2) : 0;
    int i = argc >= 1 ? ssl_slot(argv[0]) : -1;
    if (argc == 1 && i >= 0) {
        free(ssl_ca[i]);
        ssl_ca[i] = nullptr;
        reply(io, name, "");
        return;
    }
    if (argc != 2 || !parse_long(argv[1], &n) || n <= 0) {
        reply_error(io, name);
        return;
    }
    char *pem = i >= 0 && n <= CA_MAX ? static_cast<char *>(malloc(n + 1)) : nullptr;
    BufferSink sink(reinterpret_cast<uint8_t *>(pem), n);
    if (!read_payload(io, n, pem != nullptr ? &sink : nullptr)) {
        free(pem);
        reply_error(io, name);
        return;
    }
    pem[n] = '\0';
    free(ssl_ca[i]);
    ssl_ca[i] = pem;
    io.print("OK\r\n");
}

/* AT+SETECCSLOT=<sock>,<槽>,<n> 加 n 字节证书：本板没有安全芯片，读完载荷后回 ERROR */
static void cmd_seteccslot(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    long n;
    if (op == '=' && split(args, argv, 3) == 3 && parse_long(argv[2], &n) && n > 0) {
        read_payload(io, n, nullptr);
    }
    reply_error(io, name);
}

/* 已由 BEGINSERVER 分配的服务端号，否则 -1 */
static int server_arg(const char *s)
{
    long v;
    return parse_long(s, &v) && v >= 0 && v < MAX_SERVERS && server_allocated[v] ? (int)v : -1;
}

/* 接入连接的空位；没有时取连接已断且数据读完的位置（先关掉，同上游按 connected() 回收）；都没有时 -1 */
static int free_conn(void)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (conns[i].server < 0) {
            return i;
        }
    }
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!conns[i].client.connected()) {
            close_socket(SERVER_SOCK_BASE + i);
            return i;
        }
    }
    return -1;
}

/* AT+BEGINSERVER=<端口>：应答服务端号 */
static void cmd_beginserver(Stream &io, const char *name, char op, char *args)
{
    long port;
    if (op == '=' && parse_long(args, &port) && port >= 1 && port <= 65535) {
        for (int v = 0; v < MAX_SERVERS; v++) {
            if (!server_allocated[v]) {
                servers[v].begin((uint16_t)port);
                if (!servers[v]) {
                    break;
                }
                server_allocated[v] = true;
                reply_int(io, name, v);
                return;
            }
        }
    }
    reply_error(io, name);
}

/*
 * AT+SERVERAVAILABLE=<srv>（WiFiServer::available）：把新连接收进空位，再从上次返回的位置起找一个有数据可读、
 * 未被 SERVERACCEPT 取走的连接，应答其编号；没有时应答 -1（同上游）
 */
static void cmd_serveravailable(Stream &io, const char *name, char op, char *args)
{
    static int last;
    int v = op == '=' ? server_arg(args) : -1;
    if (v < 0) {
        reply_error(io, name);
        return;
    }
    for (int i = free_conn(); i >= 0; i = free_conn()) {
        WiFiClient c = servers[v].accept();
        if (!c) {
            break;
        }
        conns[i].client = c;
        conns[i].server = v;
        conns[i].accepted = false;
    }
    for (int k = 0; k < MAX_SOCKETS; k++) {
        int i = (last + k) % MAX_SOCKETS;
        if (conns[i].server == v && !conns[i].accepted && conns[i].client.available() > 0) {
            last = i;
            reply_int(io, name, SERVER_SOCK_BASE + i);
            return;
        }
    }
    reply_int(io, name, -1);
}

/* AT+SERVERACCEPT=<srv>（WiFiServer::accept）：取一个新连接交给主机，应答其编号；没有时 -1 */
static void cmd_serveraccept(Stream &io, const char *name, char op, char *args)
{
    int v = op == '=' ? server_arg(args) : -1;
    if (v < 0) {
        reply_error(io, name);
        return;
    }
    int i = free_conn();
    WiFiClient c = i >= 0 ? servers[v].accept() : WiFiClient();
    if (!c) {
        reply_int(io, name, -1);
        return;
    }
    conns[i].client = c;
    conns[i].server = v;
    conns[i].accepted = true;
    reply_int(io, name, SERVER_SOCK_BASE + i);
}

/* SERVERWRITE 的去处：服务端 srv 由 SERVERAVAILABLE 收下、仍连着的连接；返回各连接都写出的字节数 */
class ServerSink : public Print {
public:
    explicit ServerSink(int srv) : srv_(srv) {}
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t *buf, size_t n) override
    {
        size_t least = n;
        for (auto &c : conns) {
            if (c.server == srv_ && !c.accepted && c.client.connected()) {
                least = min(least, c.client.write(buf, n));
            }
        }
        return least;
    }
    using Print::write;

private:
    int srv_;
};

/* AT+SERVERWRITE=<srv>,<n> 后紧跟 n 个原始字节；同 CLIENTSEND，成功只回裸 "OK" */
static void cmd_serverwrite(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long left;
    if (op != '=' || split(args, argv, 2) != 2 || !parse_long(argv[1], &left) || left < 0) {
        reply_error(io, name);
        return;
    }
    int v = server_arg(argv[0]);
    ServerSink sink(v);
    if (read_payload(io, left, v >= 0 ? &sink : nullptr)) {
        io.print("OK\r\n");
    } else {
        reply_error(io, name);
    }
}

/* 停止监听；已接入的连接保留，主机照常用 CLIENT* 命令（同上游） */
static void cmd_serverend(Stream &io, const char *name, char op, char *args)
{
    int v = op == '=' ? server_arg(args) : -1;
    if (v < 0) {
        reply_error(io, name);
        return;
    }
    servers[v].end();
    server_allocated[v] = false;
    reply(io, name, "");
}

/* AT+UDPBEGIN=<本地端口>[,<本地地址>]：应答 UDP 编号 */
static void cmd_udpbegin(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long port;
    IPAddress local;
    int n = op == '=' ? split(args, argv, 2) : 0;
    if (n == 0 || !parse_long(argv[0], &port) || port < 0 || port > 65535 || (n == 2 && !local.fromString(argv[1]))) {
        reply_error(io, name);
        return;
    }
    for (int u = 0; u < MAX_UDP; u++) {
        if (!udp_allocated[u]) {
            if (!(n == 2 ? udps[u].begin(local, (uint16_t)port) : udps[u].begin((uint16_t)port))) {
                break;
            }
            udp_allocated[u] = true;
            reply_int(io, name, u);
            return;
        }
    }
    reply_error(io, name);
}

/* AT+UDPBEGINPACKETIP=<udp>,<端口>,<地址>、AT+UDPBEGINPACKETADD=<udp>,<端口>,<主机名>（beginPacket(host)） */
static void begin_packet(Stream &io, const char *name, char op, char *args, bool by_name)
{
    char *argv[3];
    long port;
    IPAddress ip;
    int u = -1;
    if (op == '=' && split(args, argv, 3) == 3 && parse_long(argv[1], &port) && port > 0 && port <= 65535 &&
        (by_name || ip.fromString(argv[2]))) {
        u = udp_arg(argv[0]);
    }
    if (u < 0 || !(by_name ? udps[u].beginPacket(argv[2], (uint16_t)port) : udps[u].beginPacket(ip, (uint16_t)port))) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

static void cmd_udpbeginpacketip(Stream &io, const char *name, char op, char *args)
{
    begin_packet(io, name, op, args, false);
}

static void cmd_udpbeginpacketadd(Stream &io, const char *name, char op, char *args)
{
    begin_packet(io, name, op, args, true);
}

/* AT+UDPWRITE=<udp>,<n> 后紧跟 n 个原始字节，追加到当前数据报；同 CLIENTSEND，成功只回裸 "OK" */
static void cmd_udpwrite(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long left;
    if (op != '=' || split(args, argv, 2) != 2 || !parse_long(argv[1], &left) || left < 0) {
        reply_error(io, name);
        return;
    }
    int u = udp_arg(argv[0]);
    if (read_payload(io, left, u >= 0 ? &udps[u] : nullptr)) {
        io.print("OK\r\n");
    } else {
        reply_error(io, name);
    }
}

static void cmd_udpendpacket(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0 || !udps[u].endPacket()) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

static void cmd_udpparse(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, udps[u].parsePacket());
}

static void cmd_udpavailable(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, udps[u].available());
}

/* AT+UDPREAD=<udp>,<max>：读当前数据报，应答见 reply_data */
static void cmd_udpread(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long max;
    int u = -1;
    if (op == '=' && split(args, argv, 2) == 2 && parse_long(argv[1], &max) && max >= 0) {
        u = udp_arg(argv[0]);
    }
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_data(io, name, udps[u].read(data, min(max, (long)sizeof(data))));
}

/* 下一个字节的值，没有时 -1 */
static void cmd_udppeek(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, udps[u].peek());
}

static void cmd_udpflush(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    udps[u].flush();
    reply(io, name, "");
}

static void cmd_udpremoteip(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply(io, name, udps[u].remoteIP().toString().c_str());
}

static void cmd_udpremoteport(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, udps[u].remotePort());
}

static void cmd_udpstop(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    close_udp(u);
    reply(io, name, "");
}

/* AT+RESTARTBOOTLOADER：BL616CL 复位进 ROM 下载模式，供更新本固件。上游直接复位、不应答，主机要等满超时；
   这里先回 OK 并等它发完 */
static void cmd_restartbootloader(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
    io.flush();
    resetToBootloader();
}

static const struct {
    const char *name;
    handler_t handler;
} commands[] = {
    { "+SOFTRESETWIFI", cmd_softreset },
    { "+FWVERSION", cmd_fwversion },
    { "+FWVERSION_U32", cmd_fwversion_u32 },
    { "+WIFIMODE", cmd_wifimode },
    { "+BEGINSTA", cmd_beginsta },
    { "+GETSTATUS", cmd_getstatus },
    { "+IPSTA", cmd_ipsta },
    { "+SETIP", cmd_setip },
    { "+GETSSID", cmd_getssid },
    { "+GETRSSI", cmd_getrssi },
    { "+GETBSSID", cmd_getbssid },
    { "+MACSTA", cmd_macsta },
    /* AP：DISCONNECTSOFTAP（WiFiS3 不发）未实现，按未知命令回 ERROR */
    { "+BEGINSOFTAP", cmd_beginsoftap },
    { "+SOFTAPCONFIG", cmd_softapconfig },
    { "+IPSOFTAP", cmd_ipsoftap },
    { "+GETSOFTAPSSID", cmd_getsoftapssid },
    { "+MACSOFTAP", cmd_macsoftap },
    { "+WIFISCAN", cmd_wifiscan },
    { "+DISCONNECT", cmd_disconnect },
    { "+HOSTBYNAME", cmd_hostbyname },
    { "+PING", cmd_ping },
    { "+BEGINCLIENT", cmd_beginclient },
    { "+CLIENTCONNECTNAME", cmd_clientconnectname },
    { "+CLIENTCONNECT", cmd_clientconnect },
    { "+CLIENTSEND", cmd_clientsend },
    { "+AVAILABLE", cmd_available },
    { "+CLIENTRECEIVE", cmd_clientreceive },
    { "+CLIENTCONNECTED", cmd_clientconnected },
    { "+CLIENTCLOSE", cmd_clientclose },
    { "+FLUSH", cmd_flush },
    { "+REMOTEIP", cmd_remoteip },
    { "+REMOTEPORT", cmd_remoteport },
    /* TLS：除 SSLBEGINCLIENT、SETCAROOT、SETECCSLOT、SSLPEEK 外与 TCP 客户端共用处理函数 */
    { "+SSLBEGINCLIENT", cmd_sslbeginclient },
    { "+SETCAROOT", cmd_setcaroot },
    { "+SETECCSLOT", cmd_seteccslot },
    { "+SSLCLIENTCONNECTNAME", cmd_clientconnectname },
    { "+SSLCLIENTCONNECT", cmd_clientconnect },
    { "+SSLCLIENTSEND", cmd_clientsend },
    { "+SSLAVAILABLE", cmd_available },
    { "+SSLCLIENTRECEIVE", cmd_clientreceive },
    { "+SSLCLIENTCONNECTED", cmd_clientconnected },
    { "+SSLCLIENTCLOSE", cmd_clientclose },
    { "+SSLCLIENTFLUSH", cmd_flush },
    { "+SSLREMOTEIP", cmd_remoteip },
    { "+SSLREMOTEPORT", cmd_remoteport },
    { "+SSLPEEK", cmd_peek },
    { "+BEGINSERVER", cmd_beginserver },
    { "+SERVERAVAILABLE", cmd_serveravailable },
    { "+SERVERACCEPT", cmd_serveraccept },
    { "+SERVERWRITE", cmd_serverwrite },
    { "+SERVEREND", cmd_serverend },
    /* 组播（UDPBEGINMULTI、BEGINPACKETMULTI）未实现，按未知命令回 ERROR */
    { "+UDPBEGIN", cmd_udpbegin },
    { "+UDPBEGINPACKETIP", cmd_udpbeginpacketip },
    { "+UDPBEGINPACKETADD", cmd_udpbeginpacketadd },
    { "+UDPWRITE", cmd_udpwrite },
    { "+UDPENDPACKET", cmd_udpendpacket },
    { "+UDPPARSE", cmd_udpparse },
    { "+UDPAVAILABLE", cmd_udpavailable },
    { "+UDPREAD", cmd_udpread },
    { "+UDPPEEK", cmd_udppeek },
    { "+UDPFLUSH", cmd_udpflush },
    { "+UDPREMOTEIP", cmd_udpremoteip },
    { "+UDPREMOTEPORT", cmd_udpremoteport },
    { "+UDPSTOP", cmd_udpstop },
    { "+RESTARTBOOTLOADER", cmd_restartbootloader },
};

/* 一行请求："AT"、"AT+NAME"、"AT+NAME?"、"AT+NAME=args"；不以 "AT" 开头的行忽略 */
static void dispatch(Stream &io, char *s)
{
    if (strncmp(s, "AT", 2) != 0) {
        return;
    }
    s += 2;
    if (*s == '\0') {
        io.print("OK\r\n");
        return;
    }
    if (*s != '+') {
        return;
    }
    char *end = s + 1 + strcspn(s + 1, "=?");
    char op = *end;
    *end = '\0';
    char *args = op == '\0' ? end : end + 1;
    for (const auto &c : commands) {
        if (strcmp(c.name, s) == 0) {
            c.handler(io, s, op, args);
            return;
        }
    }
    reply_error(io, s);
}

void at_serve(Stream &io)
{
    while (io.available() > 0) {
        int c = io.read();
        if (c == '\n') {
            if (!line_overflow) {
                if (line_len > 0 && line[line_len - 1] == '\r') {
                    line_len--;
                }
                line[line_len] = '\0';
                dispatch(io, line);
            }
            line_len = 0;
            line_overflow = false;
        } else if (line_len < LINE_MAX) {
            line[line_len++] = (char)c;
        } else {
            line_overflow = true; /* 过长的行整行丢弃 */
        }
    }
}
