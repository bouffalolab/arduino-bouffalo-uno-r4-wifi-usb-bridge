#pragma once

/*
 * 主机测试用的 Arduino 环境：ArduinoCore-API 的通用类，加一个建在文件描述符上的串口。
 * RA4M1 侧（WiFiS3）与 BL616CL 侧（at_server 加平台 WiFi 库）各编成一个程序，两个 FdSerial 经 socketpair 相连。
 */
#include <stdarg.h> /* renesas core 的 Arduino.h 带入的标准头，WiFiS3 直接使用 */
#include <stdio.h>

#include "api/ArduinoAPI.h"
#include "api/HardwareSerial.h"

/* renesas core 的 FifoBuffer.h 用它关中断；主机程序单线程，按普通代码块处理 */
#define synchronized

#ifdef __cplusplus
using namespace arduino;

/* 读不等待；写阻塞到全部发出（socketpair 缓冲足够大，对端总在读） */
class FdSerial : public HardwareSerial {
public:
    FdSerial() = default;
    FdSerial(int tx, int rx) { (void)tx; (void)rx; } /* WiFiS3 非 UNO R4 WiFi 分支的构造方式，本测试不用 */
    virtual ~FdSerial() = default;
    void setFd(int fd) { fd_ = fd; }
    void begin(unsigned long) override {}
    void begin(unsigned long, uint16_t) override {}
    void end() override {}
    int available() override;
    int peek() override;
    int read() override;
    void flush() override {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;
    operator bool() override { return fd_ >= 0; }

private:
    int fd_ = -1;
    int peeked_ = -1;
};

typedef FdSerial UART; /* WiFiS3 的 Modem 用这个类型名 */
extern FdSerial Serial2;
#endif
