/* WiFiS3 AT 服务端：行解析、命令表与命令处理，接口说明见 at_server.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "WiFi.h"
#include "at_server.h"
#include <BLEController.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "bridge_version.h"
#define FW_VERSION         BRIDGE_VERSION_STRING /* 本固件版本，AT+FWVERSION 返回 */
#define LINE_MAX           256     /* WiFiS3 的请求最长 128 字节 */
#define MAX_SOCKETS        8       /* 同上游：BEGINCLIENT 分配 0–7 */
#define MAX_UDP            4       /* 同上游：UDPBEGIN 分配 0–3，与 TCP 的编号各自独立 */
#define MAX_SERVERS        4       /* 同上游：BEGINSERVER 分配 0–3 */
#define SERVER_SOCK_BASE   1000    /* 同上游：服务端接入的连接编号 1000–1007，走 CLIENT* 命令 */
#define MAX_SSL            8       /* 同上游：SSLBEGINCLIENT 分配 2000–2007 */
#define SSL_SOCK_BASE      2000
#define CA_MAX             16384   /* SETCAROOT 的 PEM 上限 */
#define CONNECT_TIMEOUT_MS 3000    /* WiFiS3 官方 CLIENTCONNECT 默认值 */
#define PAYLOAD_TIMEOUT_MS 1000    /* CLIENTSEND 载荷两段之间的最长间隔 */
#define RECV_MAX           1023    /* WiFiS3 每次 CLIENTRECEIVE 最多要 1023 字节 */
#define TLS_SLOT_NAMESPACE  "tls-sse"

typedef void (*handler_t)(Stream &io, const char *name, char op, char *args);

static char line[LINE_MAX + 1];
static size_t line_len;
static bool line_overflow;
/* CLIENTSEND、UDPWRITE 的分块，CLIENTRECEIVE、UDPREAD、WIFISCAN 的应答共用；UDPREAD 一次最多一个数据报 */
static uint8_t data[WIFI_UDP_BUFFER_SIZE];

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
static char *ssl_cert[MAX_SSL];
static char *ssl_key[MAX_SSL];
static bool hci_started;
static Preferences preferences;
static size_t hci_visible;
static bool hci_batch_done;
static uint8_t hci_header[5];
static size_t hci_header_used, hci_header_size, hci_payload_left;
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
        free(ssl_cert[s - SSL_SOCK_BASE]);
        ssl_cert[s - SSL_SOCK_BASE] = nullptr;
        free(ssl_key[s - SSL_SOCK_BASE]);
        ssl_key[s - SSL_SOCK_BASE] = nullptr;
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
}

static void cmd_hcibegin(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    // A new RA4M1 sketch may start while the previous Host was still scanning.
    // Start a new H4 session instead of inheriting queued events and partial frames.
    BLEController.end();
    hci_started = false;
    if (!BLEController.begin()) {
        reply_error(io, name);
        return;
    }
    hci_started = true;
    hci_visible = 0;
    hci_batch_done = false;
    hci_header_used = hci_header_size = hci_payload_left = 0;
    reply(io, name, "");
}

static void cmd_hciwait(Stream &io, const char *name, char op, char *args)
{
    long timeout;
    if (!hci_started || op != '=' || !parse_long(args, &timeout) || timeout < 0) {
        reply_error(io, name);
        return;
    }
    unsigned long start = millis();
    while (BLEController.available() == 0 && millis() - start < (unsigned long)timeout) {
        delay(1);
    }
    reply(io, name, "");
}

static void cmd_hciend(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    BLEController.end();
    hci_started = false;
    hci_visible = 0;
    hci_batch_done = false;
    hci_header_used = hci_header_size = hci_payload_left = 0;
    reply(io, name, "");
}

static void cmd_hciavailable(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (!hci_started || op != '?') {
        reply_error(io, name);
        return;
    }
    // ArduinoBLE keeps Command Complete results in its shared receive buffer.
    // Return zero after each H4 packet so the next packet cannot overwrite
    // a result before sendCommand() returns to its caller.
    if (hci_batch_done) {
        hci_batch_done = false;
        reply_int(io, name, 0);
        return;
    }
    if (hci_visible == 0) hci_visible = BLEController.available();
    reply_int(io, name, hci_visible);
}

static void cmd_hciread(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (!hci_started || op != '\0') {
        reply_error(io, name);
        return;
    }
    // ArduinoBLE 2.1.0 queues each reply in RingBufferN<258>, usable capacity 257.
    if (hci_visible == 0) hci_visible = BLEController.available();
    size_t n = 0, limit = min(hci_visible, (size_t)257);
    bool packet_done = false;
    while (n < limit) {
        if (hci_header_size == 0 || hci_header_used < hci_header_size) {
            int byte = BLEController.read();
            if (byte < 0) break;
            data[n++] = byte;
            if (hci_header_used == 0) {
                // Controller output used by ArduinoBLE is Event or ACL.
                hci_header_size = byte == 4 ? 3 : byte == 2 ? 5 : 1;
            }
            hci_header[hci_header_used++] = byte;
            if (hci_header_used < hci_header_size) continue;
            hci_payload_left = hci_header[0] == 4 ? hci_header[2] :
                               hci_header[0] == 2 ? hci_header[3] | (hci_header[4] << 8) : 0;
        } else {
            size_t got = BLEController.read(data + n, min(limit - n, hci_payload_left));
            if (got == 0) break;
            n += got;
            hci_payload_left -= got;
        }
        if (hci_payload_left == 0) {
            hci_header_used = hci_header_size = 0;
            packet_done = true;
            break;
        }
    }
    hci_visible = packet_done ? 0 : hci_visible - n;
    hci_batch_done = n > 0 && (packet_done || hci_visible == 0);
    reply_data(io, name, n);
}

class HciSink : public Print {
public:
    size_t write(uint8_t byte) override
    {
        return write(&byte, 1);
    }

    size_t write(const uint8_t *buffer, size_t size) override
    {
        size_t sent = 0;
        unsigned long start = millis();
        while (sent < size && millis() - start < 5000) {
            sent += BLEController.write(buffer + sent, size - sent);
            if (sent < size) delay(1);
        }
        return sent;
    }
};

/* AT+HCIWRITE=<n> 后紧跟 n 个 H4 字节；ArduinoBLE 的 passthrough 直接发送载荷。 */
static void cmd_hciwrite(Stream &io, const char *name, char op, char *args)
{
    long left;
    if (op != '=' || !parse_long(args, &left) || left <= 0) {
        reply_error(io, name);
        return;
    }
    HciSink sink;
    if (read_payload(io, left, hci_started ? &sink : nullptr)) {
        io.print("OK\r\n");
    } else {
        // A timed-out transaction may leave a partial H4 header in the Controller.
        // Require HCIBEGIN again so the next Host cannot inherit that frame.
        BLEController.end();
        hci_started = false;
        hci_visible = 0;
        hci_batch_done = false;
        hci_header_used = hci_header_size = hci_payload_left = 0;
        reply_error(io, name);
    }
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

/* Mode and radio lifecycle belong to the platform WiFi library. */
static void cmd_gettime(Stream &io, const char *name, char op, char *args)
{
    if (op != '=' || args[0] != '\0') {
        reply_error(io, name);
        return;
    }
    char value[12];
    snprintf(value, sizeof(value), "%lu", WiFi.getTime());
    reply(io, name, value);
}

static void cmd_hostname(Stream &io, const char *name, char op, char *args)
{
    if (op == '?') {
        const char *value = WiFi.getHostname();
        if (value != nullptr) {
            reply(io, name, value);
            return;
        }
    } else if (op == '=' && WiFi.setHostname(args)) {
        reply(io, name, "");
        return;
    }
    reply_error(io, name);
}

static void cmd_wifimode(Stream &io, const char *name, char op, char *args)
{
    if (op == '?') {
        reply_int(io, name, WiFi.getMode());
        return;
    }
    char *argv[2];
    long mode, auto_connect;
    int n = op == '=' ? split(args, argv, 2) : 0;
    if (n < 1 || !parse_long(argv[0], &mode) || mode < 0 || mode > 3 ||
        (n == 2 && argv[1][0] != '\0' && (!parse_long(argv[1], &auto_connect) || auto_connect < 0 || auto_connect > 1))) {
        reply_error(io, name);
        return;
    }
    /* Official ESP32 setAutoConnect is deprecated and returns false.
       The optional second parameter has no effect, exactly as upstream. */
    if (!WiFi.mode((WiFiMode)mode)) {
        reply_error(io, name);
        return;
    }
    wifi_started = true;
    reply(io, name, "");
}

static void cmd_reconnect(Stream &io, const char *name, char op, char *args)
{
    long enable;
    if (op == '?') {
        reply_int(io, name, WiFi.getAutoReconnect());
    } else if (op == '=' && parse_long(args, &enable) && enable >= 0 && enable <= 1 &&
               WiFi.setAutoReconnect(enable != 0)) {
        reply(io, name, "");
    } else {
        reply_error(io, name);
    }
}

/* Explicit compatibility placeholder: the pinned official implementation also
   ignores the setter and returns false from getAutoConnect. No Flash storage. */
static void cmd_autoconnect(Stream &io, const char *name, char op, char *args)
{
    long enable;
    if (op == '?') {
        reply_int(io, name, 0);
    } else if (op == '=' && parse_long(args, &enable) && enable >= 0 && enable <= 1) {
        reply(io, name, "");
    } else {
        reply_error(io, name);
    }
}

static void cmd_ipv6(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op == '?') {
        reply(io, name, WiFi.localIPv6().c_str());
    } else if (op == '\0' && WiFi.enableIpV6()) {
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
        case 0: ip = WiFi.stationIP(); break;
        /* WiFiS3 also asks IPSTA for AP gateway/mask in mode 2. */
        case 1: ip = WiFi.getMode() == WIFI_AP ? WiFi.gatewayIP() : WiFi.stationGatewayIP(); break;
        case 2: ip = WiFi.getMode() == WIFI_AP ? WiFi.subnetMask() : WiFi.stationSubnetMask(); break;
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
    WiFi.configSTA(ip[0], ip[3], ip[1], ip[2]);
    if (n == 5 && ip[0] != IPAddress(0, 0, 0, 0)) {
        WiFi.setDNS(ip[3], ip[4]);
    }
    reply(io, name, "");
}

/* Official defaults: open, channel 1, visible, max 5. The SDK has four
   association slots; max 5 is a ceiling, not a promise of five-client capacity. */
static void cmd_beginsoftap(Stream &io, const char *name, char op, char *args)
{
    char *argv[5];
    long ch = 1, hidden = 0, maximum = 5;
    int n = op == '=' ? split(args, argv, 5) : 0;
    if (n == 0 || argv[0][0] == '\0' ||
        (n >= 3 && !parse_long(argv[2], &ch)) ||
        (n >= 4 && (!parse_long(argv[3], &hidden) || hidden < 0 || hidden > 1)) ||
        (n == 5 && !parse_long(argv[4], &maximum))) {
        reply_error(io, name);
        return;
    }
    wifi_started = true;
    uint8_t st = WiFi.beginAP(argv[0], n > 1 ? argv[1] : "", ch >= 1 && ch <= 13 ? (uint8_t)ch : 0,
                              hidden != 0, maximum >= 1 && maximum <= 5 ? (uint8_t)maximum : 0);
    reply(io, name, st == WL_AP_LISTENING ? "1" : "0");
}

static void cmd_softapconfig(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    IPAddress ip[3];
    int n = op == '=' ? split(args, argv, 3) : 0;
    bool ok = n == 3;
    for (int i = 0; i < n && ok; i++) {
        ok = ip[i].fromString(argv[i]);
    }
    if (!ok || !WiFi.softAPConfig(ip[0], ip[1], ip[2])) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

static void cmd_disconnectsoftap(Stream &io, const char *name, char op, char *args)
{
    long wifi_off = 0;
    if ((op != '\0' && (op != '=' || !parse_long(args, &wifi_off) || wifi_off < 0 || wifi_off > 1)) ||
        !WiFi.softAPdisconnect(wifi_off != 0)) {
        reply_error(io, name);
        return;
    }
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
    long wifi_off = 0;
    if (op != '\0' && (op != '=' || !parse_long(args, &wifi_off) || wifi_off < 0 || wifi_off > 1)) {
        reply_error(io, name);
        return;
    }
    if (wifi_started) {
        WiFi.disconnect(wifi_off != 0);
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
        int i = s - SSL_SOCK_BASE;
        ssls[i].setCACert(ssl_ca[i]);
        ssls[i].setCertificate(ssl_cert[i]);
        ssls[i].setPrivateKey(ssl_key[i]);
    }
    if (timeout_ms > 0) {
        client_of(s).setConnectionTimeout(timeout_ms);
    }
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
    connect_socket(io, name, argv, 0);
}

static void cmd_clientconnectip(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    IPAddress ip;
    if (op != '=' || split(args, argv, 3) != 3 || !ip.fromString(argv[1])) {
        reply_error(io, name);
        return;
    }
    connect_socket(io, name, argv, 0);
}

/* AT+CLIENTCONNECT=<sock>,<主机>,<端口>[,<超时毫秒>]；省略时使用官方 3000 ms 默认值 */
static void cmd_clientconnect(Stream &io, const char *name, char op, char *args)
{
    char *argv[4];
    long timeout_ms;
    int n = op == '=' ? split(args, argv, 4) : 0;
    if (n < 3 || n > 4) {
        reply_error(io, name);
        return;
    }
    if (n == 4 && (!parse_long(argv[3], &timeout_ms) || timeout_ms <= 0)) {
        timeout_ms = CONNECT_TIMEOUT_MS;
    } else if (n == 3) {
        timeout_ms = CONNECT_TIMEOUT_MS;
    }
    connect_socket(io, name, argv, timeout_ms);
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

static void cmd_clientstate(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    if (client_of(s).connected()) {
        char result[64];
        snprintf(result, sizeof(result), "%s,%u,%u", client_of(s).remoteIP().toString().c_str(),
                 (unsigned)client_of(s).remotePort(), (unsigned)client_of(s).localPort());
        reply(io, name, result);
    } else {
        reply(io, name, "");
    }
}

static void cmd_ipclient(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    if (client_of(s).connected()) {
        reply(io, name, client_of(s).localIP().toString().c_str());
    } else {
        reply(io, name, "");
    }
}

static void cmd_clientstatus(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
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

static bool store_pem_payload(Stream &io, long size, char **slot)
{
    if (size <= 0 || size > CA_MAX) {
        read_payload(io, size, nullptr);
        return false;
    }
    char *pem = static_cast<char *>(malloc((size_t)size + 1));
    if (pem == nullptr) {
        read_payload(io, size, nullptr);
        return false;
    }
    BufferSink sink(reinterpret_cast<uint8_t *>(pem), size);
    bool ok = read_payload(io, size, &sink);
    if (!ok) {
        free(pem);
        return false;
    }
    pem[size] = '\0';
    free(*slot);
    *slot = pem;
    return true;
}

static void cmd_sslclientsetcert(Stream &io, const char *name, char op, char *args)
{
    char *argv[2]; long size;
    int n = op == '=' ? split(args, argv, 2) : 0;
    int s = n == 2 ? ssl_slot(argv[0]) : -1;
    bool parsed = n == 2 && parse_long(argv[1], &size);
    bool stored = parsed && s >= 0 && store_pem_payload(io, size, &ssl_cert[s]);
    if (parsed && s < 0) {
        read_payload(io, size, nullptr);
    }
    if (!stored) {
        reply_error(io, name);
        return;
    }
    ssls[s].setCertificate(ssl_cert[s]);
    reply(io, name, "");
}

static void cmd_sslclientsetpkey(Stream &io, const char *name, char op, char *args)
{
    char *argv[2]; long size;
    int n = op == '=' ? split(args, argv, 2) : 0;
    int s = n == 2 ? ssl_slot(argv[0]) : -1;
    bool parsed = n == 2 && parse_long(argv[1], &size);
    bool stored = parsed && s >= 0 && store_pem_payload(io, size, &ssl_key[s]);
    if (parsed && s < 0) {
        read_payload(io, size, nullptr);
    }
    if (!stored) {
        reply_error(io, name);
        return;
    }
    ssls[s].setPrivateKey(ssl_key[s]);
    reply(io, name, "");
}

static char *der_to_pem(const uint8_t *der, size_t size, const char *header, const char *footer)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded = ((size + 2) / 3) * 4;
    size_t cap = strlen(header) + encoded + (encoded + 63) / 64 + strlen(footer) + 1;
    char *pem = static_cast<char *>(malloc(cap));
    if (pem == nullptr) {
        return nullptr;
    }
    size_t out = 0, line = 0;
    memcpy(pem + out, header, strlen(header));
    out += strlen(header);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = uint32_t(der[i]) << 16;
        if (i + 1 < size) v |= uint32_t(der[i + 1]) << 8;
        if (i + 2 < size) v |= der[i + 2];
        pem[out++] = alphabet[(v >> 18) & 63];
        pem[out++] = alphabet[(v >> 12) & 63];
        pem[out++] = i + 1 < size ? alphabet[(v >> 6) & 63] : '=';
        pem[out++] = i + 2 < size ? alphabet[v & 63] : '=';
        line += 4;
        if (line == 64 && i + 3 < size) {
            pem[out++] = '\n';
            line = 0;
        }
    }
    memcpy(pem + out, footer, strlen(footer));
    out += strlen(footer);
    pem[out] = '\0';
    if (out >= cap) {
        free(pem);
        return nullptr;
    }
    return pem;
}

/* AT+SETECCSLOT=<sock>,<槽>,<n>：证书用本次载荷，私钥从 littlefs Preferences 的 tls-sse/slotN 读取。 */
static void cmd_seteccslot(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    long n = 0;
    long slot = -1;
    int s = -1;
    if (op == '=' && split(args, argv, 3) == 3 && parse_long(argv[1], &slot) && slot >= 0 && slot <= 15 &&
        parse_long(argv[2], &n) && n > 0 && n <= 4096) {
        s = ssl_slot(argv[0]);
    }
    if (s < 0) {
        if (n > 0) read_payload(io, n, nullptr);
        reply_error(io, name);
        return;
    }
    uint8_t *der = static_cast<uint8_t *>(malloc((size_t)n));
    BufferSink der_sink(der, n);
    bool got = der != nullptr && read_payload(io, n, &der_sink);
    if (!got) {
        free(der);
        reply_error(io, name);
        return;
    }
    char key_name[16];
    snprintf(key_name, sizeof(key_name), "slot%ld", slot);
    Preferences sse;
    size_t key_size = 0;
    if (!sse.begin(TLS_SLOT_NAMESPACE, true, "lfs")) {
        free(der);
        reply_error(io, name);
        return;
    }
    key_size = sse.getBytesLength(key_name);
    char *cert = der_to_pem(der, (size_t)n, "-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n");
    char *key_der = key_size > 0 && key_size <= Preferences::MAX_VALUE_SIZE ? static_cast<char *>(malloc(key_size)) : nullptr;
    char *key = nullptr;
    bool key_ok = key_der != nullptr && sse.getBytes(key_name, key_der, key_size) == key_size;
    sse.end();
    if (key_ok) {
        key = der_to_pem(reinterpret_cast<const uint8_t *>(key_der), key_size,
                         "-----BEGIN EC PRIVATE KEY-----\n", "-----END EC PRIVATE KEY-----\n");
    }
    free(key_der);
    free(der);
    if (cert == nullptr || key == nullptr) {
        free(cert); free(key);
        reply_error(io, name);
        return;
    }
    free(ssl_cert[s]); ssl_cert[s] = cert;
    free(ssl_key[s]); ssl_key[s] = key;
    ssls[s].setCertificate(cert);
    ssls[s].setPrivateKey(key);
    reply(io, name, "");
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
static void udp_begin(Stream &io, const char *name, char op, char *args, bool multicast)
{
    char *argv[2];
    long port;
    IPAddress local;
    int n = op == '=' ? split(args, argv, 2) : 0;
    if (n == 0 || !parse_long(argv[0], &port) || port < 0 || port > 65535 ||
        (n == 2 && !local.fromString(argv[1])) || (multicast && (n != 2 || port == 0))) {
        reply_error(io, name);
        return;
    }
    for (int u = 0; u < MAX_UDP; u++) {
        if (!udp_allocated[u]) {
            if (!(multicast ? udps[u].beginMulticast(local, (uint16_t)port) :
                  n == 2 ? udps[u].begin(local, (uint16_t)port) : udps[u].begin((uint16_t)port))) {
                break;
            }
            udp_allocated[u] = true;
            reply_int(io, name, u);
            return;
        }
    }
    reply_error(io, name);
}

static void cmd_udpbegin(Stream &io, const char *name, char op, char *args)
{
    udp_begin(io, name, op, args, false);
}

static void cmd_udpbeginmulti(Stream &io, const char *name, char op, char *args)
{
    udp_begin(io, name, op, args, true);
}

static void begin_existing_packet(Stream &io, const char *name, char op, char *args, bool multicast)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0 || !(multicast ? udps[u].beginMulticastPacket() : udps[u].beginPacket())) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

static void cmd_udpbeginpacket(Stream &io, const char *name, char op, char *args)
{
    begin_existing_packet(io, name, op, args, false);
}

static void cmd_beginpacketmulti(Stream &io, const char *name, char op, char *args)
{
    begin_existing_packet(io, name, op, args, true);
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

/* 保留官方 MOUNTFS 形式；本平台挂载失败不自动格式化，包括 format_on_fault=1。 */
static void cmd_mountfs(Stream &io, const char *name, char op, char *args)
{
    long flag;
    if (op != '=' || !parse_long(args, &flag) || (flag != 0 && flag != 1) || !LittleFS.begin()) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "FS Mounted\r\n");
}

/* PREF 数字结果按固定上游保留额外空行；数值0是begin/写/删/clear的失败结果。 */
static void pref_number(Stream &io, const char *name, uint64_t value)
{
    char result[24];
    snprintf(result, sizeof(result), "%" PRIu64 "\r\n", value);
    reply(io, name, result);
}

static bool pref_name(const char *key) { return key && *key && strlen(key) <= 15; }
static bool pref_default_error() {
    int e = preferences.lastError();
    return e == 0 || e == LFS_ERR_NOENT || e == LFS_ERR_INVAL;
}

static void cmd_prefbegin(Stream &io, const char *name, char op, char *args)
{
    char *argv[4]; long readOnly;
    int argc = split(args, argv, 4);
    if (op != '=' || argc != 3 || !parse_long(argv[1], &readOnly) || (readOnly != 0 && readOnly != 1)) {
        reply_error(io, name); return;
    }
    preferences.end();
    pref_number(io, name, preferences.begin(argv[0], readOnly, argv[2]));
}

static void cmd_prefcontrol(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != 0) { reply_error(io, name); return; }
    if (!strcmp(name, "+PREFEND")) { preferences.end(); reply(io, name, "0"); return; }
    if (!strcmp(name, "+PREFCLEAR")) { pref_number(io, name, preferences.clear()); return; }
    size_t n = preferences.freeEntries();
    if (preferences.lastError()) reply_error(io, name);
    else pref_number(io, name, n);
}

static void cmd_prefkey(Stream &io, const char *name, char op, char *args)
{
    if (op != '=' || !pref_name(args) || strchr(args, ',')) { reply_error(io, name); return; }
    if (!strcmp(name, "+PREFREMOVE")) { pref_number(io, name, preferences.remove(args)); return; }
    size_t value = !strcmp(name, "+PREFTYPE") ? preferences.getType(args) : preferences.getBytesLength(args);
    if (!pref_default_error()) reply_error(io, name);
    else pref_number(io, name, value);
}

static bool pref_integer(const char *text, long type, int64_t &signedValue, uint64_t &unsignedValue)
{
    bool negative = *text == '-';
    if (*text == '+' || negative) ++text;
    if (!*text || ((type & 1) && negative)) return false;
    unsigned bits = (1u << (type / 2)) * 8;
    uint64_t limit = (type & 1) ? UINT64_MAX >> (64 - bits) :
        (uint64_t(1) << (bits - 1)) - !negative;
    uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = *text - '0';
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    unsignedValue = value;
    if (!(type & 1)) signedValue = negative ?
        (value == (uint64_t(1) << 63) ? INT64_MIN : -int64_t(value)) : int64_t(value);
    return true;
}

static void cmd_prefput(Stream &io, const char *name, char op, char *args)
{
    char *argv[4]; long type;
    int argc = split(args, argv, 4);
    if (op != '=' || argc < 3 || !parse_long(argv[1], &type)) { reply_error(io, name); return; }
    if (type == PT_STR || type == PT_BLOB) {
        long length;
        if (!parse_long(argv[2], &length) || length < 0) { reply_error(io, name); return; }
        if (argc != 3 || !pref_name(argv[0]) || length > (long)Preferences::MAX_VALUE_SIZE) {
            read_payload(io, length, nullptr); reply_error(io, name); return;
        }
        if (io.readBytes(data, length) != (size_t)length ||
            (type == PT_STR && memchr(data, 0, length)) || (type == PT_BLOB && !length)) {
            reply_error(io, name); return;
        }
        data[length] = 0;
        size_t n = type == PT_STR ? preferences.putString(argv[0], reinterpret_cast<const char *>(data)) :
                                   preferences.putBytes(argv[0], data, length);
        pref_number(io, name, n); return;
    }
    int64_t s = 0; uint64_t u = 0;
    if (argc != 3 || !pref_name(argv[0]) || type < 0 || type >= PT_STR || !pref_integer(argv[2], type, s, u)) {
        reply_error(io, name); return;
    }
    size_t n = 0;
    switch (type) {
        case PT_I8: n = preferences.putChar(argv[0], s); break;
        case PT_U8: n = preferences.putUChar(argv[0], u); break;
        case PT_I16: n = preferences.putShort(argv[0], s); break;
        case PT_U16: n = preferences.putUShort(argv[0], u); break;
        case PT_I32: n = preferences.putInt(argv[0], s); break;
        case PT_U32: n = preferences.putUInt(argv[0], u); break;
        case PT_I64: n = preferences.putLong64(argv[0], s); break;
        case PT_U64: n = preferences.putULong64(argv[0], u); break;
    }
    pref_number(io, name, n);
}

static void cmd_prefget(Stream &io, const char *name, char op, char *args)
{
    char *argv[4]; long type;
    int argc = split(args, argv, 4);
    if (op != '=' || argc < 2 || !pref_name(argv[0]) || !parse_long(argv[1], &type) ||
        type < 0 || type >= PT_INVALID || argc != (type == PT_BLOB ? 2 : 3)) {
        reply_error(io, name); return;
    }
    if (type == PT_STR || type == PT_BLOB) {
        size_t n;
        if (type == PT_STR) {
            String result = preferences.getString(argv[0], argv[2]);
            n = result.length(); memcpy(data, result.c_str(), n);
        } else {
            n = preferences.getBytesLength(argv[0]);
            if (n && preferences.getBytes(argv[0], data, sizeof(data)) != n) { reply_error(io, name); return; }
        }
        if (!pref_default_error()) { reply_error(io, name); return; }
        io.print(name); io.print(": "); io.print(n); io.print('|'); io.write(data, n); io.print("\r\nOK\r\n");
        return;
    }
    int64_t s = 0; uint64_t u = 0;
    if (!pref_integer(argv[2], type, s, u)) { reply_error(io, name); return; }
    switch (type) {
        case PT_I8: s = preferences.getChar(argv[0], s); break;
        case PT_U8: u = preferences.getUChar(argv[0], u); break;
        case PT_I16: s = preferences.getShort(argv[0], s); break;
        case PT_U16: u = preferences.getUShort(argv[0], u); break;
        case PT_I32: s = preferences.getInt(argv[0], s); break;
        case PT_U32: u = preferences.getUInt(argv[0], u); break;
        case PT_I64: s = preferences.getLong64(argv[0], s); break;
        case PT_U64: u = preferences.getULong64(argv[0], u); break;
    }
    if (!pref_default_error()) { reply_error(io, name); return; }
    if (type & 1) pref_number(io, name, u);
    else { char result[24]; snprintf(result, sizeof(result), "%" PRId64 "\r\n", s); reply(io, name, result); }
}

/* 官方 FS=0,op,path[,size|,offset,length]。写帧即使失败也消费已声明的载荷。 */
static void cmd_fs(Stream &io, const char *name, char op, char *args)
{
    if (op != '=') {
        reply_error(io, name);
        return;
    }
    char *argv[6];
    int argc = split(args, argv, 6);
    long type, action, length;
    if (argc < 2 || !parse_long(argv[1], &action)) {
        reply_error(io, name);
        return;
    }
    bool local = argc >= 3 && parse_long(argv[0], &type) && type == 0 && LittleFS.mounted() &&
                 !Preferences::isStoragePath(argv[2]);
    if (action == 1 || action == 3) {
        if (argc < 4 || !parse_long(argv[3], &length) || length < 0) {
            reply_error(io, name);
            return;
        }
        if (!local || argc != 4 || length > 255) {
            read_payload(io, length, nullptr);
            reply_error(io, name);
            return;
        }
        /* Flash 操作会暂时屏蔽中断；先收完整帧，避免写入时丢失仍在传输的 UART 载荷。 */
        if (io.readBytes(data, length) != (size_t)length) {
            reply_error(io, name);
            return;
        }
        File file = LittleFS.open(argv[2], action == 1 ? "w" : "a");
        bool written = file && file.write(data, length) == (size_t)length;
        bool committed = file && file.close();
        if (written && committed) io.print("OK\r\n");
        else reply_error(io, name);
        return;
    }
    if (local && action == 0 && argc == 3) {
        if (LittleFS.remove(argv[2])) io.print("OK\r\n");
        else reply_error(io, name);
        return;
    }
    long offset;
    if (!local || action != 2 || argc != 5 || !parse_long(argv[3], &offset) ||
        offset < 0 || offset > 0x7fffffffL || !parse_long(argv[4], &length) ||
        length < 0 || length > RECV_MAX) {
        reply_error(io, name);
        return;
    }
    File file = LittleFS.open(argv[2]);
    if (!file || (size_t)offset >= file.size() || !file.seek(offset)) {
        reply_error(io, name);
        return;
    }
    int n = file.read(data, min((size_t)length, file.size() - offset));
    bool closed = file.close();
    if (n < 0 || !closed) {
        reply_error(io, name);
        return;
    }
    io.print(name);
    io.print(": ");
    io.print(n);
    io.print('|');
    io.write(data, n);
    io.print("\r\nOK\r\n");
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
    { "+PREFBEGIN", cmd_prefbegin },
    { "+PREFEND", cmd_prefcontrol },
    { "+PREFCLEAR", cmd_prefcontrol },
    { "+PREFSTAT", cmd_prefcontrol },
    { "+PREFREMOVE", cmd_prefkey },
    { "+PREFTYPE", cmd_prefkey },
    { "+PREFLEN", cmd_prefkey },
    { "+PREFPUT", cmd_prefput },
    { "+PREFGET", cmd_prefget },
    { "+MOUNTFS", cmd_mountfs },
    { "+FS", cmd_fs },
    { "+SOFTRESETWIFI", cmd_softreset },
    { "+HCIBEGIN", cmd_hcibegin },
    { "+HCIEND", cmd_hciend },
    { "+HCIWAIT", cmd_hciwait },
    { "+HCIAVAILABLE", cmd_hciavailable },
    { "+HCIREAD", cmd_hciread },
    { "+HCIWRITE", cmd_hciwrite },
    { "+FWVERSION", cmd_fwversion },
    { "+FWVERSION_U32", cmd_fwversion_u32 },
    { "+WIFIMODE", cmd_wifimode },
    { "+GETTIME", cmd_gettime },
    { "+HOSTNAME", cmd_hostname },
    { "+BEGINSTA", cmd_beginsta },
    { "+GETSTATUS", cmd_getstatus },
    { "+IPSTA", cmd_ipsta },
    { "+SETIP", cmd_setip },
    { "+GETSSID", cmd_getssid },
    { "+GETRSSI", cmd_getrssi },
    { "+GETBSSID", cmd_getbssid },
    { "+MACSTA", cmd_macsta },
    { "+RECONNECT", cmd_reconnect },
    { "+AUTOCONNECT", cmd_autoconnect },
    { "+IPV6", cmd_ipv6 },
    { "+DISCONNECTSOFTAP", cmd_disconnectsoftap },
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
    { "+CLIENTSTATE", cmd_clientstate },
    { "+CLIENTCONNECTIP", cmd_clientconnectip },
    { "+CLIENTCONNECTNAME", cmd_clientconnectname },
    { "+CLIENTCONNECT", cmd_clientconnect },
    { "+CLIENTSEND", cmd_clientsend },
    { "+AVAILABLE", cmd_available },
    { "+CLIENTRECEIVE", cmd_clientreceive },
    { "+CLIENTCONNECTED", cmd_clientconnected },
    { "+CLIENTSTATUS", cmd_clientstatus },
    { "+CLIENTCLOSE", cmd_clientclose },
    { "+FLUSH", cmd_flush },
    { "+REMOTEIP", cmd_remoteip },
    { "+REMOTEPORT", cmd_remoteport },
    { "+IPCLIENT", cmd_ipclient },
    { "+PEEK", cmd_peek },
    /* TLS：除 SSLBEGINCLIENT、SETCAROOT、SETECCSLOT、SSLPEEK 外与 TCP 客户端共用处理函数 */
    { "+SSLBEGINCLIENT", cmd_sslbeginclient },
    { "+SETCAROOT", cmd_setcaroot },
    { "+SETECCSLOT", cmd_seteccslot },
    { "+SSLCLIENTSTATE", cmd_clientstate },
    { "+SSLCLIENTCONNECTIP", cmd_clientconnectip },
    { "+SSLCLIENTCONNECTNAME", cmd_clientconnectname },
    { "+SSLCLIENTCONNECT", cmd_clientconnect },
    { "+SSLCLIENTSEND", cmd_clientsend },
    { "+SSLAVAILABLE", cmd_available },
    { "+SSLCLIENTRECEIVE", cmd_clientreceive },
    { "+SSLCLIENTCONNECTED", cmd_clientconnected },
    { "+SSLCLIENTSTATUS", cmd_clientstatus },
    { "+SSLCLIENTCLOSE", cmd_clientclose },
    { "+SSLCLIENTFLUSH", cmd_flush },
    { "+SSLREMOTEIP", cmd_remoteip },
    { "+SSLREMOTEPORT", cmd_remoteport },
    { "+SSLIPCLIENT", cmd_ipclient },
    { "+SSLCLIENTSETCERT", cmd_sslclientsetcert },
    { "+SSLCLIENTSETPKEY", cmd_sslclientsetpkey },
    { "+SSLPEEK", cmd_peek },
    { "+BEGINSERVER", cmd_beginserver },
    { "+SERVERAVAILABLE", cmd_serveravailable },
    { "+SERVERACCEPT", cmd_serveraccept },
    { "+SERVERWRITE", cmd_serverwrite },
    { "+SERVEREND", cmd_serverend },
    { "+UDPBEGIN", cmd_udpbegin },
    { "+UDPBEGINMULTI", cmd_udpbeginmulti },
    { "+UDPBEGINPACKET", cmd_udpbeginpacket },
    { "+BEGINPACKETMULTI", cmd_beginpacketmulti },
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
