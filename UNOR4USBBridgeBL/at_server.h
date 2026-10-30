#pragma once

#include "Arduino.h"

/*
 * WiFiS3 AT server: RA4M1 sends commands over Serial2 and handlers call platform libraries.
 * Implement network/scan, TCP client/server, UDP including multicast, TLS, static/AP
 * configuration, SNTP/hostname, BLE HCI, LittleFS/File/Preferences and ROM restart. Unsupported
 * commands reply "+<command>:ERROR". Protocol differences are documented in workspace
 * docs/reference/wifis3-at-protocol.md. Run Wi-Fi/socket calls in the single at_serve task.
 */

/**
 * @brief Consume available bytes from io and execute complete command lines. CLIENTSEND
 * consumes its payload internally. Call repeatedly from one task only.
 */
void at_serve(Stream &io);
