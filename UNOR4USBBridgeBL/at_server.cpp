/* WiFiS3 AT 服务端：行解析、命令表与命令处理，接口说明见 at_server.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WiFi.h"
#include "at_server.h"

#define FW_VERSION         "0.1.0" /* 本固件版本，AT+FWVERSION 返回 */
#define LINE_MAX           256     /* WiFiS3 的请求最长 128 字节 */
#define MAX_SOCKETS        8       /* 同上游：BEGINCLIENT 分配 0–7 */
#define CONNECT_TIMEOUT_MS 8000    /* WiFiS3 每条命令等 10 s，连接要在此之前应答 */
#define PAYLOAD_TIMEOUT_MS 1000    /* CLIENTSEND 载荷两段之间的最长间隔 */
#define RECV_MAX           1023    /* WiFiS3 每次 CLIENTRECEIVE 最多要 1023 字节 */

typedef void (*handler_t)(Stream &io, const char *name, char op, char *args);

static char line[LINE_MAX + 1];
static size_t line_len;
static bool line_overflow;
static uint8_t data[RECV_MAX + 1];

static int wifi_mode;    /* AT+WIFIMODE：0 未设置，1 STA */
static bool sta_started; /* 发起过 BEGINSTA：Wi-Fi 协议栈已初始化，断开才有意义 */
static WiFiClient clients[MAX_SOCKETS];
static bool allocated[MAX_SOCKETS];

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

/* 失败一律回 "+<命令>: ERROR"：WiFiS3 把 RESULT_ERROR 误定义为 "OK"，收到裸 "ERROR" 要等满超时 */
static void reply_error(Stream &io, const char *name)
{
    io.print(name);
    io.print(": ERROR\r\n");
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

/* 已由 BEGINCLIENT 分配的 socket 号，否则 -1 */
static int socket_arg(const char *s)
{
    long v;
    return parse_long(s, &v) && v >= 0 && v < MAX_SOCKETS && allocated[v] ? (int)v : -1;
}

static void close_socket(int s)
{
    clients[s].stop();
    allocated[s] = false;
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
    if (sta_started) {
        WiFi.disconnect();
    }
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

/* 只支持 STA（1）；AP 未实现 */
static void cmd_wifimode(Stream &io, const char *name, char op, char *args)
{
    if (op == '?') {
        reply_int(io, name, wifi_mode);
    } else if (op == '=' && strcmp(args, "1") == 0) {
        wifi_mode = 1;
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
    sta_started = true;
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

static void cmd_disconnect(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    if (sta_started) {
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

/* 主机名或点分地址都经 WiFiClient::connect(host) 解析 */
static void connect_socket(Stream &io, const char *name, char **argv, long timeout_ms)
{
    long port;
    int s = socket_arg(argv[0]);
    if (s < 0 || !parse_long(argv[2], &port) || port < 1 || port > 65535 || WiFi.status() != WL_CONNECTED) {
        reply_error(io, name);
        return;
    }
    clients[s].setConnectionTimeout(timeout_ms);
    if (!clients[s].connect(argv[1], (uint16_t)port)) {
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
    int s = socket_arg(argv[0]);
    bool ok = s >= 0;
    unsigned long last = millis();
    while (left > 0) {
        long n = io.available();
        if (n <= 0) {
            if (millis() - last > PAYLOAD_TIMEOUT_MS) {
                ok = false;
                break;
            }
            delay(1);
            continue;
        }
        n = io.readBytes(data, min(n, min(left, (long)sizeof(data))));
        last = millis();
        left -= n;
        if (ok && clients[s].write(data, n) != (size_t)n) {
            ok = false;
        }
    }
    if (ok) {
        io.print("OK\r\n");
    } else {
        reply_error(io, name);
    }
}

static void cmd_available(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, clients[s].available());
}

/* AT+CLIENTRECEIVE=<sock>,<max>：应答 "+CLIENTRECEIVE: <n>|" 加 n 个原始字节再加 "OK"，中间没有换行 */
static void cmd_clientreceive(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long max;
    int s = -1;
    if (op == '=' && split(args, argv, 2) == 2 && parse_long(argv[1], &max) && max >= 0) {
        s = socket_arg(argv[0]);
    }
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    size_t want = min(max, (long)RECV_MAX);
    size_t got = 0;
    while (got < want) {
        int n = clients[s].read(data + got, want - got);
        if (n <= 0) {
            break;
        }
        got += n;
    }
    io.print(name);
    io.print(": ");
    io.print(got);
    io.print('|');
    io.write(data, got);
    io.print("OK\r\n");
}

static void cmd_clientconnected(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, clients[s].connected() ? 1 : 0);
}

static void cmd_clientclose(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(args) : -1;
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
    int s = op == '=' ? socket_arg(args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    clients[s].flush();
    reply(io, name, "");
}

static void cmd_remoteip(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply(io, name, clients[s].remoteIP().toString().c_str());
}

static void cmd_remoteport(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, clients[s].remotePort());
}

static const struct {
    const char *name;
    handler_t handler;
} commands[] = {
    { "+SOFTRESETWIFI", cmd_softreset },
    { "+FWVERSION", cmd_fwversion },
    { "+WIFIMODE", cmd_wifimode },
    { "+BEGINSTA", cmd_beginsta },
    { "+GETSTATUS", cmd_getstatus },
    { "+IPSTA", cmd_ipsta },
    { "+GETSSID", cmd_getssid },
    { "+GETRSSI", cmd_getrssi },
    { "+GETBSSID", cmd_getbssid },
    { "+MACSTA", cmd_macsta },
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
