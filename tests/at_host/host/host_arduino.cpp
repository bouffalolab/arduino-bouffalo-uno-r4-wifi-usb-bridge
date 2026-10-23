/* 主机测试用的时间函数、FdSerial 与 api/itoa.h 要求的转换函数 */
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "Arduino.h"

FdSerial Serial2;

static uint64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static const uint64_t start_us = now_us();

extern "C" {
unsigned long millis(void)
{
    return (unsigned long)((now_us() - start_us) / 1000u);
}

unsigned long micros(void)
{
    return (unsigned long)(now_us() - start_us);
}

void delay(unsigned long ms)
{
    usleep(ms * 1000u);
}

void delayMicroseconds(unsigned int us)
{
    usleep(us);
}

void yield(void)
{
}

static char *to_radix(unsigned long value, bool negative, char *string, int radix)
{
    char tmp[sizeof(unsigned long) * 8 + 1];
    int n = 0;
    do {
        int d = (int)(value % (unsigned long)radix);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= (unsigned long)radix;
    } while (value != 0);
    char *p = string;
    if (negative) {
        *p++ = '-';
    }
    while (n > 0) {
        *p++ = tmp[--n];
    }
    *p = '\0';
    return string;
}

char *ltoa(long value, char *string, int radix)
{
    bool negative = radix == 10 && value < 0;
    return to_radix(negative ? 0ul - (unsigned long)value : (unsigned long)value, negative, string, radix);
}

char *ultoa(unsigned long value, char *string, int radix)
{
    return to_radix(value, false, string, radix);
}

char *itoa(int value, char *string, int radix)
{
    return radix == 10 ? ltoa(value, string, radix) : to_radix((unsigned int)value, false, string, radix);
}

char *utoa(unsigned int value, char *string, int radix)
{
    return to_radix(value, false, string, radix);
}
}

int FdSerial::available()
{
    int n = 0;
    if (ioctl(fd_, FIONREAD, &n) != 0) {
        n = 0;
    }
    return n + (peeked_ >= 0 ? 1 : 0);
}

int FdSerial::read()
{
    if (peeked_ >= 0) {
        int c = peeked_;
        peeked_ = -1;
        return c;
    }
    uint8_t c;
    return recv(fd_, &c, 1, MSG_DONTWAIT) == 1 ? c : -1;
}

int FdSerial::peek()
{
    if (peeked_ < 0) {
        peeked_ = read();
    }
    return peeked_;
}

size_t FdSerial::write(const uint8_t *buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = send(fd_, buf + done, size - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        done += (size_t)n;
    }
    return done;
}
