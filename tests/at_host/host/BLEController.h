#pragma once

/* Software-only WiFiS3 regression: no Controller or radio exists here.
 * BLE is deliberately unavailable; hardware tests provide BLE acceptance.
 */
#include "Arduino.h"

class BLEControllerClass {
public:
    bool begin() { return false; }
    void end() {}
    int available() { return 0; }
    int read() { return -1; }
    size_t read(uint8_t *, size_t) { return 0; }
    size_t write(const uint8_t *, size_t) { return 0; }
};

inline BLEControllerClass BLEController;
