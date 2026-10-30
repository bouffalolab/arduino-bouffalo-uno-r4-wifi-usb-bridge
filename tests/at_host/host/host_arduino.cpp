/* Host timing functions, FdSerial and integer conversions required by api/itoa.h. */
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "Arduino.h"

FdSerial Serial2;

/**
 * @brief Return monotonic host microseconds for software-test Arduino timing.
 */
static uint64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static const uint64_t start_us = now_us();

extern "C" {
/**
 * @brief Return elapsed program milliseconds with unsigned Arduino wraparound.
 */
unsigned long millis(void)
{
    return (unsigned long)((now_us() - start_us) / 1000u);
}

/**
 * @brief Return elapsed program microseconds with unsigned Arduino wraparound.
 */
unsigned long micros(void)
{
    return (unsigned long)(now_us() - start_us);
}

/**
 * @brief Sleep on the host for the requested milliseconds; this fixture does not run FreeRTOS.
 */
void delay(unsigned long ms)
{
    usleep(ms * 1000u);
}

/**
 * @brief Sleep on the host for the requested microseconds using usleep, without busy-waiting.
 */
void delayMicroseconds(unsigned int us)
{
    usleep(us);
}

/**
 * @brief Provide a no-op host fixture hook; this process does not run the SDK scheduler.
 */
void yield(void)
{
}

/**
 * @brief Format a host-test integer with the requested sign and radix.
 */
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

/**
 * @brief Format a host signed long using the local radix converter, adding a sign for negative
 * decimal values.
 */
char *ltoa(long value, char *string, int radix)
{
    bool negative = radix == 10 && value < 0;
    return to_radix(negative ? 0ul - (unsigned long)value : (unsigned long)value, negative, string, radix);
}

/**
 * @brief Format a host unsigned long using the local radix converter.
 */
char *ultoa(unsigned long value, char *string, int radix)
{
    return to_radix(value, false, string, radix);
}

/**
 * @brief Convert a signed integer into caller-provided text storage.
 */
char *itoa(int value, char *string, int radix)
{
    return radix == 10 ? ltoa(value, string, radix) : to_radix((unsigned int)value, false, string, radix);
}

/**
 * @brief Convert an unsigned integer into caller-provided text storage.
 */
char *utoa(unsigned int value, char *string, int radix)
{
    return to_radix(value, false, string, radix);
}
}

/**
 * @brief Host-only fixture: return the currently readable byte count without consuming input.
 */
int FdSerial::available()
{
    int n = 0;
    if (ioctl(fd_, FIONREAD, &n) != 0) {
        n = 0;
    }
    return n + (peeked_ >= 0 ? 1 : 0);
}

/**
 * @brief Host-only fixture: consume available input bytes; return the byte/count or the
 * interface-specific empty/error value.
 */
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

/**
 * @brief Host-only fixture: return the next input byte without consuming it, or -1 when none
 * is available.
 */
int FdSerial::peek()
{
    if (peeked_ < 0) {
        peeked_ = read();
    }
    return peeked_;
}

/**
 * @brief Host-only fixture: write the supplied byte or buffer and return the accepted byte
 * count.
 */
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
