/*
 * Host-side BL endpoint: at_server plus platform WiFi with host wifi/net ports. Usage: bl_host
 * <serial-fd>. Nonempty AT_HOST_TRACE logs raw RX '<' and TX '>' to stderr. --early-client <port>
 * probes address/hostname connects before Wi-Fi initialization and prints EARLYCLIENT results;
 * invalid early net_port calls abort.
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
    /**
     * @brief Host-only fixture: consume available input bytes; return the byte/count or the
     * interface-specific empty/error value.
     */
    int read() override
    {
        int c = FdSerial::read();
        if (c >= 0) {
            trace('<', (uint8_t)c);
        }
        return c;
    }
    /**
     * @brief Host-only fixture: write the supplied byte or buffer and return the accepted byte
     * count.
     */
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
    /**
     * @brief Print direction changes and raw/escaped host-test bytes to stderr.
     */
    void trace(char dir, uint8_t c)
    {
        if (dir != dir_) {
            fprintf(stderr, "\n%c ", dir);
            dir_ = dir;
        }
        fprintf(stderr, c >= 0x20 && c < 0x7F ? "%c" : "\\x%02X", c);
    }
};

/*
 * Host resetToBootloader substitute: print a marker on stderr and exit so run.py can verify the
 * command.
 */
/**
 * @brief Print the host-test reset marker and exit; no board reset or ROM downloader is
 * exercised.
 */
extern "C" void resetToBootloader(void)
{
    fprintf(stderr, "RESETTOBOOTLOADER\n");
    exit(0);
}

/**
 * @brief Run the host AT bridge fixture on the supplied file descriptor.
 */
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
