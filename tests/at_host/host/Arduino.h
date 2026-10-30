#pragma once

/*
 * Host Arduino environment: ArduinoCore-API plus file-descriptor serial. Compile RA WiFiS3 and BL
 * at_server/platform WiFi into separate programs connected through socketpair-backed FdSerial
 * instances.
 */
#include <stdarg.h> /* Standard headers normally included by the Renesas Arduino.h and used directly by WiFiS3. */
#include <stdio.h>

#include "api/ArduinoAPI.h"
#include "api/HardwareSerial.h"

/*
 * Renesas FifoBuffer uses this interrupt guard; the host is single-threaded, so an ordinary block
 * suffices.
 */
#define synchronized

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Declare the host-test reset substitute, which prints a marker and exits.
 */
void resetToBootloader(void) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
using namespace arduino;

/*
 * Nonblocking reads; writes block until complete because socketpair buffering and the active peer
 * permit progress.
 */
class FdSerial : public HardwareSerial {
public:
    /**
     * @brief Construct a host serial adapter without claiming a hardware UART.
     */
    FdSerial() = default;
    /**
     * @brief Construct a host serial adapter without claiming a hardware UART.
     */
    FdSerial(int tx, int rx) { (void)tx; (void)rx; } /* Constructor used by non-UNO-R4 WiFiS3 branches, not exercised here. */
    /**
     * @brief Destroy the host serial adapter without taking ownership of its descriptor.
     */
    virtual ~FdSerial() = default;
    /**
     * @brief Attach the host serial fixture to a caller-owned file descriptor.
     */
    void setFd(int fd) { fd_ = fd; }
    /**
     * @brief Accept Arduino serial arguments without configuring a hardware UART.
     */
    void begin(unsigned long) override {}
    /**
     * @brief Accept Arduino serial arguments without configuring a hardware UART.
     */
    void begin(unsigned long, uint16_t) override {}
    /**
     * @brief Leave the caller-owned host descriptor open; this fixture does not own a hardware
     * UART.
     */
    void end() override {}
    /**
     * @brief Host-only fixture: return the currently readable byte count without consuming
     * input.
     */
    int available() override;
    /**
     * @brief Host-only fixture: return the next input byte without consuming it, or -1 when
     * none is available.
     */
    int peek() override;
    /**
     * @brief Host-only fixture: consume available input bytes; return the byte/count or the
     * interface-specific empty/error value.
     */
    int read() override;
    /**
     * @brief Provide a no-op host serial hook because writes already send directly to the
     * descriptor.
     */
    void flush() override {}
    /**
     * @brief Host-only fixture: write the supplied byte or buffer and return the accepted byte
     * count.
     */
    size_t write(uint8_t c) override { return write(&c, 1); }
    /**
     * @brief Host-only fixture: write the supplied byte or buffer and return the accepted byte
     * count.
     */
    size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;
    /**
     * @brief Report whether this object currently refers to a usable resource.
     */
    operator bool() override { return fd_ >= 0; }

private:
    int fd_ = -1;
    int peeked_ = -1;
};

typedef FdSerial UART; /* Type name expected by the WiFiS3 Modem. */
extern FdSerial Serial2;
#endif
