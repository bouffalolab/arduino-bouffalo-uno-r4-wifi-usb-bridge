/*
 * RA4M1 侧：未修改的 WiFiS3 经串口向 BL616CL 侧发 AT，逐项输出 "<项> <值>..."，由 run.py 判定。
 * 用法：ra_host <串口 fd> <网络名> <密码> <回显端口> <关闭端口> <UDP 回显端口> <UDP 本地端口> <服务端口,另 5 个空闲端口>
 * 回显端口原样发回收到的字节；关闭端口发 "bye\n" 后关闭连接。UDP 回显服务把数据报原样发回来源，
 * 收到 "push" 时改从另一个 socket 发 "pushed"。服务端各项由 run.py 的客户端线程按顺序配合（见 run.py 的 server_peer）。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "WiFiS3.h"
#include "fake_ports.h"

static const IPAddress loopback(127, 0, 0, 1);

static void mac_line(const char *item, const uint8_t *m)
{
    printf("%s %02X:%02X:%02X:%02X:%02X:%02X\n", item, m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* 读到 n 字节或超时 */
static size_t read_n(WiFiClient &c, uint8_t *buf, size_t n, unsigned long timeout_ms)
{
    size_t got = 0;
    unsigned long start = millis();
    while (got < n && millis() - start < timeout_ms) {
        if (c.available() > 0) {
            int r = c.read(buf + got, n - got);
            got += r > 0 ? (size_t)r : 0;
        } else {
            delay(1);
        }
    }
    return got;
}

/* 发 n 字节（含 0x00、\r\n、'|'）读回，输出 "<项> <连接> <写出> <读回> <一致>" */
static void echo(const char *item, WiFiClient &c, int connected, size_t n)
{
    static uint8_t out[4096], in[4096];
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)(i * 31 + n);
    }
    size_t wrote = connected ? c.write(out, n) : 0;
    size_t got = connected ? read_n(c, in, n, 5000) : 0;
    printf("%s %d %zu %zu %d\n", item, connected, wrote, got, got == n && memcmp(in, out, n) == 0);
}

/* 等下一个数据报最多 timeout_ms；返回其长度，超时返回 0 */
static int wait_packet(WiFiUDP &u, unsigned long timeout_ms)
{
    unsigned long start = millis();
    while (millis() - start < timeout_ms) {
        int n = u.parsePacket();
        if (n > 0) {
            return n;
        }
        delay(10);
    }
    return 0;
}

/* 读完当前数据报（每次最多 chunk 字节），返回字节数 */
static size_t read_packet(WiFiUDP &u, uint8_t *buf, size_t max, size_t chunk)
{
    size_t got = 0;
    while (got < max) {
        int r = u.read(buf + got, min(chunk, max - got));
        if (r <= 0) {
            break;
        }
        got += r;
    }
    return got;
}

/* TLS：BL 侧的 tls_port 是明文替身（见 bl/fake_tls_port.c），这里检验命令、证书参数传递与编号管理 */
static void ssl_tests(uint16_t echo_port, uint16_t close_port)
{
    {
        WiFiSSLClient c;
        int r = c.connect(FAKE_HOST, echo_port);
        echo("SSLECHO", c, r, 3000);
        printf("SSLREMOTE %s %u %u\n", c.remoteIP().toString().c_str(), (unsigned)c.remotePort(), (unsigned)echo_port);
        c.write((const uint8_t *)"pk", 2);
        unsigned long start = millis();
        while (c.available() == 0 && millis() - start < 3000) {
            delay(1);
        }
        int peek = c.peek(); /* WiFiS3 的 available() 只问 SSLAVAILABLE，SSLPEEK 看模块侧，应为 'p' */
        uint8_t two[2] = { 0, 0 };
        size_t got = read_n(c, two, 2, 3000);
        printf("SSLPEEK %d %zu %d\n", peek, got, got == 2 && two[0] == 'p' && two[1] == 'k');
        c.stop();
        printf("SSLSTOP %d\n", (int)c.connected());
    }
    {
        WiFiSSLClient c;
        int r = c.connect(loopback, echo_port); /* 按地址字符串校验：根证书束只认 FAKE_HOST */
        printf("SSLUNTRUSTED %d\n", r);
        c.stop();
    }
    {
        WiFiSSLClient c;
        c.setCACert(FAKE_CA_PEM);
        int r = c.connect(loopback, echo_port);
        echo("SSLCA", c, r, 100);
        c.stop();
        c.setCACert("-----BEGIN CERTIFICATE-----\nnot the test ca\n-----END CERTIFICATE-----\n");
        int bad = c.connect(loopback, echo_port);
        c.stop();
        c.setCACert(nullptr);
        int bundle = c.connect(FAKE_HOST, echo_port); /* 不带载荷的 SETCAROOT 恢复用根证书束 */
        c.stop();
        printf("SSLCAFLOW %d %d\n", bad, bundle);
    }
    {
        static const uint8_t cert[300] = { 0x30, 0x82 };
        WiFiSSLClient e;
        e.setEccSlot(0, cert, sizeof(cert));
        int ecc = e.connect(FAKE_HOST, echo_port);
        e.stop();
        WiFiSSLClient c;
        c.setConnectionTimeout(1500); /* SSLCLIENTCONNECT 带超时 */
        int r = c.connect(FAKE_HOST, echo_port);
        echo("SSLTIMEOUT", c, r, 1);
        printf("SSLECC %d\n", ecc);
        c.stop();
    }
    {
        WiFiSSLClient c;
        int r = c.connect(FAKE_HOST, close_port);
        uint8_t bye[4];
        size_t got = r ? read_n(c, bye, 4, 3000) : 0;
        unsigned long start = millis();
        while (c.connected() && millis() - start < 3000) {
            delay(10);
        }
        printf("SSLPEERCLOSE %d %zu %d\n", r, got, (int)c.connected());
        c.stop();
    }
    {
        /* SSL 编号不能用于 CLIENT 命令，反之亦然 */
        WiFiSSLClient sc;
        WiFiClient tc;
        int r = sc.connect(FAKE_HOST, echo_port) && tc.connect(loopback, echo_port);
        std::string res;
        bool tcp_on_ssl = modem.write(std::string(PROMPT(_AVAILABLE)), res, "%s%d\r\n", CMD_WRITE(_AVAILABLE), 2000);
        bool ssl_on_tcp = modem.write(std::string(PROMPT(_SSLAVAILABLE)), res, "%s%d\r\n", CMD_WRITE(_SSLAVAILABLE), 0);
        printf("SSLMIX %d %d %d\n", r, (int)tcp_on_ssl, (int)ssl_on_tcp);
        sc.stop();
        tc.stop();
    }
    {
        WiFiSSLClient cs[9];
        int ok = 0;
        for (int i = 0; i < 8; i++) {
            ok += cs[i].connect(FAKE_HOST, echo_port);
        }
        int ninth = cs[8].connect(FAKE_HOST, echo_port);
        for (auto &c : cs) {
            c.stop();
        }
        WiFiSSLClient again;
        int reuse = again.connect(FAKE_HOST, echo_port);
        again.stop();
        printf("SSLSOCKETS %d %d %d\n", ok, ninth, reuse);
    }
}

static void udp_tests(uint16_t echo_port, uint16_t local_port)
{
    static uint8_t out[1500], in[1500];
    for (size_t i = 0; i < sizeof(out); i++) {
        out[i] = (uint8_t)(i * 7 + 3);
    }
    out[0] = 0x00;
    memcpy(out + 1, "\r\n|", 3);
    {
        WiFiUDP u;
        int b = u.begin(local_port);
        int p = u.beginPacket(loopback, echo_port);
        size_t w = u.write(out, 1) + u.write(out + 1, 1399);
        int e = u.endPacket();
        int n = wait_packet(u, 3000);
        int pk = u.peek();
        int av = u.available();
        size_t got = read_packet(u, in, sizeof(in), 500);
        printf("UDPECHO %d %d %zu %d %d %d %d %zu %d\n", b, p, w, e, n, pk, av, got, got == 1400 && memcmp(in, out, 1400) == 0);
        printf("UDPREMOTE %s %u %u\n", u.remoteIP().toString().c_str(), (unsigned)u.remotePort(), (unsigned)echo_port);
        unsigned long t = millis();
        n = u.parsePacket();
        printf("UDPNONE %d %lu\n", n, millis() - t);

        p = u.beginPacket(FAKE_HOST, echo_port);
        u.write((const uint8_t *)"hello", 5);
        e = u.endPacket();
        n = wait_packet(u, 3000);
        u.flush();
        printf("UDPNAME %d %d %d %d\n", p, e, n, u.available());

        u.beginPacket(loopback, echo_port);
        u.write((const uint8_t *)"push", 4);
        u.endPacket();
        n = wait_packet(u, 3000);
        got = read_packet(u, in, sizeof(in), 64);
        printf("UDPPUSH %d %d %u\n", n, got == 6 && memcmp(in, "pushed", 6) == 0, (unsigned)u.remotePort());

        /* 超过 1460 字节：write 失败，帧保持同步，之后的命令照常 */
        p = u.beginPacket(loopback, echo_port);
        w = u.write(out, 1500);
        e = u.endPacket();
        n = wait_packet(u, 3000);
        got = read_packet(u, in, sizeof(in), 1459);
        printf("UDPBIG %d %zu %d %d %d\n", p, w, e, n, got == 1460 && memcmp(in, out, 1460) == 0);
        u.stop();
    }
    {
        WiFiUDP us[5];
        int ok = 0;
        for (int i = 0; i < 4; i++) {
            ok += us[i].begin(0);
        }
        int fifth = us[4].begin(0);
        us[0].stop();
        int reuse = us[4].begin(0);
        for (auto &u : us) {
            u.stop();
        }
        printf("UDPSOCKETS %d %d %d\n", ok, fifth, reuse);
    }
    {
        WiFiUDP u;
        unsigned long t = millis();
        int m = u.beginMulticast(IPAddress(239, 1, 2, 3), local_port);
        printf("UDPMULTI %d %lu\n", m, millis() - t);
        u.stop();
    }
}

/* 从 c 读一行（不含 '\n'）到 line，最多等 timeout_ms；读到返回 true */
static bool read_line(WiFiClient &c, std::string &line, unsigned long timeout_ms)
{
    unsigned long start = millis();
    line.clear();
    while (millis() - start < timeout_ms) {
        int b = c.available() > 0 ? c.read() : -1;
        if (b < 0) {
            delay(1);
        } else if (b == '\n') {
            return true;
        } else {
            line += (char)b;
        }
    }
    return false;
}

/* 一个服务端轮询 available()，对每行回一行，直到处理 n 行或超时；返回处理的行数。
   upper 为真时回大写的原文，否则回 "k" */
static int serve_lines(WiFiServer &srv, int n, bool upper, unsigned long timeout_ms, int *same)
{
    WiFiClient first;
    int done = 0;
    unsigned long start = millis();
    *same = 1;
    while (done < n && millis() - start < timeout_ms) {
        WiFiClient c = srv.available();
        std::string line;
        if (!c || !read_line(c, line, 1000)) {
            delay(5);
            continue;
        }
        if (done == 0) {
            first = c;
        } else if (!(c == first)) {
            *same = 0;
        }
        for (auto &ch : line) {
            ch = upper ? (char)toupper((unsigned char)ch) : ch;
        }
        c.print(upper ? line.c_str() : "k");
        c.print("\n");
        done++;
    }
    return done;
}

/* ports[0] 为主服务端口，ports[1..5] 用于服务端个数上限 */
static void server_tests(const uint16_t *ports)
{
    WiFiServer srv(ports[0]);
    srv.begin();
    printf("SRVBEGIN %d\n", (int)(bool)srv);
    int same;
    int n = serve_lines(srv, 2, true, 10000, &same);
    printf("SRVCHAT %d %d\n", n, same);
    {
        WiFiClient c;
        unsigned long start = millis();
        while (!(c = srv.accept()) && millis() - start < 5000) {
            delay(5);
        }
        int got = (bool)c;
        if (got) {
            c.print("welcome\n");
        }
        std::string line;
        int ok = got && read_line(c, line, 3000) && line == "ok";
        c.stop();
        printf("SRVACCEPT %d %d\n", got, ok);
    }
    {
        WiFiClient seen[2];
        int distinct = 0;
        unsigned long start = millis();
        while (distinct < 2 && millis() - start < 5000) {
            WiFiClient c = srv.available();
            std::string line;
            if (c && read_line(c, line, 1000) && line == "x" && (distinct == 0 || !(c == seen[0]))) {
                seen[distinct++] = c;
            }
        }
        size_t w = srv.write((const uint8_t *)"bc\n", 3);
        printf("SRVBCAST %d %zu\n", distinct, w);
    }
    n = serve_lines(srv, 10, false, 20000, &same);
    printf("SRVCHURN %d\n", n);
    srv.end();
    printf("SRVEND %d\n", (int)(bool)srv);
    {
        /* 不用 ports[0]：run.py 此时在确认它已不再接受连接 */
        WiFiServer more[5] = { ports[1], ports[2], ports[3], ports[4], ports[5] };
        int ok = 0;
        for (int i = 0; i < 4; i++) {
            more[i].begin();
            ok += (bool)more[i];
        }
        more[4].begin();
        int fifth = (bool)more[4];
        more[0].end();
        more[4].begin();
        int reuse = (bool)more[4];
        for (auto &m : more) {
            m.end();
        }
        printf("SRVLIMIT %d %d %d\n", ok, fifth, reuse);
    }
}

/* 输出 "<项> <个数> <毫秒>"，再每个网络一行 "<项>AP <序号>|<ssid>|<bssid>|<rssi>|<信道>|<加密>" */
static void scan(const char *item)
{
    unsigned long t = millis();
    int found = WiFi.scanNetworks();
    printf("%s %d %lu\n", item, found, millis() - t);
    for (int i = 0; i < found; i++) {
        uint8_t b[6];
        WiFi.BSSID(i, b);
        printf("%sAP %d|%s|%02X:%02X:%02X:%02X:%02X:%02X|%d|%u|%u\n", item, i, WiFi.SSID(i), b[0], b[1], b[2], b[3],
               b[4], b[5], (int)WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i));
    }
}

extern "C" int host_main(int argc, char **argv)
{
    if (argc != 9) {
        return 2;
    }
    Serial2.setFd(atoi(argv[1]));
    const char *ssid = argv[2];
    const char *pass = argv[3];
    uint16_t echo_port = (uint16_t)atoi(argv[4]);
    uint16_t close_port = (uint16_t)atoi(argv[5]);
    uint16_t udp_echo_port = (uint16_t)atoi(argv[6]);
    uint16_t udp_local_port = (uint16_t)atoi(argv[7]);
    uint16_t server_ports[6];
    {
        char *p = argv[8];
        for (auto &sp : server_ports) {
            sp = (uint16_t)strtoul(p, &p, 10);
            p += *p == ',';
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    unsigned long t = millis();
    int st = WiFi.status();
    printf("STATUS0 %d %lu\n", st, millis() - t);
    printf("FW %s\n", WiFi.firmwareVersion());

    /* 联网之前：连接、解析都应失败，BL 侧不能因此出错 */
    {
        WiFiClient c;
        printf("EARLYCONNECT %d\n", c.connect(loopback, echo_port));
        c.stop();
        IPAddress ip;
        printf("EARLYDNS %d\n", WiFi.hostByName(FAKE_HOST, ip));
        printf("EARLYPING %d\n", WiFi.ping(loopback));
        WiFiUDP u;
        printf("EARLYUDP %d\n", u.begin(udp_local_port));
        WiFiServer s(server_ports[0]);
        s.begin();
        printf("EARLYSERVER %d\n", (int)(bool)s);
    }
    /* 未连接时扫描；BL 侧替身依次给出一张表、空表、失败 */
    scan("SCAN");
    scan("SCANEMPTY");
    scan("SCANFAIL");

    WiFi.setTimeout(2000);
    /* 先调用再输出：printf 参数的求值顺序不确定 */
    st = WiFi.begin(ssid, "wrong-pass");
    printf("BADPASS %d %d\n", st, WiFi.status());
    st = WiFi.begin("no-such-net", pass);
    printf("NOSSID %d %d\n", st, WiFi.status());
    t = millis();
    st = WiFi.begin(ssid, pass);
    unsigned long took = millis() - t;
    printf("BEGIN %d %lu\n", st, took);

    printf("IP %s\n", WiFi.localIP().toString().c_str());
    printf("GW %s\n", WiFi.gatewayIP().toString().c_str());
    printf("MASK %s\n", WiFi.subnetMask().toString().c_str());
    printf("DNS %s\n", WiFi.dnsIP(0).toString().c_str());
    printf("SSID %s\n", WiFi.SSID());
    printf("RSSI %d\n", (int)WiFi.RSSI());
    uint8_t mac[6];
    mac_line("BSSID", WiFi.BSSID(mac));
    mac_line("MAC", WiFi.macAddress(mac));

    IPAddress ip;
    int found = WiFi.hostByName(FAKE_HOST, ip);
    printf("HOSTBYNAME %d %s\n", found, ip.toString().c_str());
    printf("HOSTBYNAMEBAD %d\n", WiFi.hostByName("no-such-host.invalid", ip));
    int p0 = WiFi.ping(loopback);
    int p1 = WiFi.ping(FAKE_HOST);
    int p2 = WiFi.ping("no-such-host.invalid");
    printf("PING %d %d %d\n", p0, p1, p2);

    {
        WiFiClient c;
        int r = c.connect(loopback, echo_port);
        echo("ECHOIP", c, r, 3000);
        printf("REMOTE %s %u %u\n", c.remoteIP().toString().c_str(), (unsigned)c.remotePort(), (unsigned)echo_port);
        c.stop();
    }
    {
        WiFiClient c;
        int r = c.connect(FAKE_HOST, echo_port);
        echo("ECHONAME", c, r, 100);
        c.stop();
    }
    {
        WiFiClient c;
        c.setConnectionTimeout(1500);
        int r = c.connect(loopback, echo_port);
        echo("ECHOTIMEOUT", c, r, 1);
        c.stop();
    }
    {
        WiFiClient c;
        int r = c.connect(loopback, close_port);
        uint8_t bye[4];
        size_t got = r ? read_n(c, bye, 4, 3000) : 0;
        unsigned long start = millis();
        while (c.connected() && millis() - start < 3000) {
            delay(10);
        }
        printf("PEERCLOSE %d %zu %d\n", r, got, (int)c.connected());
        c.stop();
    }
    {
        WiFiClient cs[9];
        int ok = 0;
        for (int i = 0; i < 8; i++) {
            ok += cs[i].connect(loopback, echo_port);
        }
        int ninth = cs[8].connect(loopback, echo_port);
        for (auto &c : cs) {
            c.stop();
        }
        WiFiClient again;
        int reuse = again.connect(loopback, echo_port);
        again.stop();
        printf("SOCKETS %d %d %d\n", ok, ninth, reuse);
    }
    ssl_tests(echo_port, close_port);
    udp_tests(udp_echo_port, udp_local_port);
    server_tests(server_ports);
    {
        std::string res;
        t = millis();
        bool ok = modem.write(std::string("+FOOBAR:"), res, "%s", "AT+FOOBAR\r\n");
        printf("UNKNOWN %d %lu\n", ok, millis() - t);
    }

    WiFi.disconnect();
    printf("DISC %d\n", WiFi.status());
    {
        // 最后一条：BL 侧应答后复位进下载模式（主机替身退出进程）
        std::string res;
        t = millis();
        bool ok = modem.write(std::string(PROMPT(_RESTART_BOOTLOADER)), res, "%s", CMD(_RESTART_BOOTLOADER));
        printf("RESTARTBOOT %d %lu\n", ok, millis() - t);
    }
    printf("DONE\n");
    return 0;
}
