/*
 * RA4M1 侧：未修改的 WiFiS3 经串口向 BL616CL 侧发 AT，逐项输出 "<项> <值>..."，由 run.py 判定。
 * 用法：ra_host <串口 fd> <网络名> <密码> <回显端口> <关闭端口>
 * 回显端口原样发回收到的字节；关闭端口发 "bye\n" 后关闭连接。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

extern "C" int host_main(int argc, char **argv)
{
    if (argc != 6) {
        return 2;
    }
    Serial2.setFd(atoi(argv[1]));
    const char *ssid = argv[2];
    const char *pass = argv[3];
    uint16_t echo_port = (uint16_t)atoi(argv[4]);
    uint16_t close_port = (uint16_t)atoi(argv[5]);
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
    }

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
    {
        std::string res;
        t = millis();
        bool ok = modem.write(std::string("+FOOBAR:"), res, "%s", "AT+FOOBAR\r\n");
        printf("UNKNOWN %d %lu\n", ok, millis() - t);
    }

    WiFi.disconnect();
    printf("DISC %d\n", WiFi.status());
    printf("DONE\n");
    return 0;
}
