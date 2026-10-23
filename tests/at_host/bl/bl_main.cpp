/*
 * BL616CL 侧：at_server 加平台 WiFi 库（wifi_port、net_port 换成主机实现）。用法：bl_host <串口 fd>
 * 环境变量 AT_HOST_TRACE 非空时，把串口上收发的字节写到 stderr（"<" 收、">" 发）。
 * bl_host --early-client <端口>：Wi-Fi 初始化之前直接调用平台 WiFiClient::connect（按地址、按域名），
 * 输出 "EARLYCLIENT <结果> <结果>"；违反平台约定调用 net_port 时进程中止。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Arduino.h"
#include "WiFi.h"
#include "at_server.h"
#include "fake_ports.h"

class TraceSerial : public FdSerial {
public:
    int read() override
    {
        int c = FdSerial::read();
        if (c >= 0) {
            trace('<', (uint8_t)c);
        }
        return c;
    }
    size_t write(const uint8_t *buf, size_t size) override
    {
        for (size_t i = 0; i < size; i++) {
            trace('>', buf[i]);
        }
        return FdSerial::write(buf, size);
    }
    using FdSerial::write;

private:
    char dir_ = 0;
    void trace(char dir, uint8_t c)
    {
        if (dir != dir_) {
            fprintf(stderr, "\n%c ", dir);
            dir_ = dir;
        }
        fprintf(stderr, c >= 0x20 && c < 0x7F ? "%c" : "\\x%02X", c);
    }
};

/* 平台 resetToBootloader 的替身：在 stderr 打标记后退出，run.py 据此判定 AT+RESTARTBOOTLOADER */
extern "C" void resetToBootloader(void)
{
    fprintf(stderr, "RESETTOBOOTLOADER\n");
    exit(0);
}

extern "C" int host_main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--early-client") == 0) {
        WiFiClient c;
        int by_ip = c.connect(IPAddress(127, 0, 0, 1), (uint16_t)atoi(argv[2]));
        int by_name = c.connect(FAKE_HOST, (uint16_t)atoi(argv[2]));
        c.stop();
        printf("EARLYCLIENT %d %d\n", by_ip, by_name);
        return 0;
    }
    if (argc != 2) {
        return 2;
    }
    static TraceSerial traced;
    FdSerial &io = getenv("AT_HOST_TRACE") != NULL ? traced : Serial2;
    io.setFd(atoi(argv[1]));
    for (;;) {
        at_serve(io);
        usleep(200);
    }
}
