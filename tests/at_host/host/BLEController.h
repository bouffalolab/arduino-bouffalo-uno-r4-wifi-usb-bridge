#pragma once

/* Software-only WiFiS3 regression: no Controller or radio exists here.
 * BLE is deliberately unavailable; hardware tests provide BLE acceptance.
 */
#include "Arduino.h"

class BLEControllerClass {
public:
    /**
     * @brief Report BLE unavailable in this software-only fixture; always return false.
     */
    bool begin() { return false; }
    /**
     * @brief Leave the unavailable software-only BLE fixture unchanged.
     */
    void end() {}
    /**
     * @brief Return zero because the software-only BLE fixture has no receive data.
     */
    int available() { return 0; }
    /**
     * @brief Return -1 because the software-only BLE fixture has no receive data.
     */
    int read() { return -1; }
    /**
     * @brief Return zero without accessing the caller buffer; no Controller or radio is exercised.
     */
    size_t read(uint8_t *, size_t) { return 0; }
    /**
     * @brief Host-only BLE stub: return zero without exercising a Controller or radio.
     */
    size_t write(const uint8_t *, size_t) { return 0; }
};

inline BLEControllerClass BLEController;
