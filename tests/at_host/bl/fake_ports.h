#pragma once

/* Fixed virtual-network parameters in host byte order, checked by run.py. */
#include <stdbool.h>

#define FAKE_IP    0x7F000001u /* 127.0.0.1, also used by echo services. */
#define FAKE_GW    0x7F000001u
#define FAKE_MASK  0xFF000000u
#define FAKE_DNS   0x7F000035u /* 127.0.0.53 */
#define FAKE_RSSI  (-42)
#define FAKE_MAC   { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }
#define FAKE_BSSID { 0x02, 0x00, 0x00, 0x00, 0x00, 0xAA }
#define FAKE_HOST  "echo.test" /* Only resolvable hostname; maps to FAKE_IP. */
/*
 * Plaintext TLS substitute: bundled trust accepts only FAKE_HOST; custom PEM must exactly match
 * the locally generated FAKE_CA_PEM fixture.
 */
#define FAKE_CA_PEM \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIBbjCCARSgAwIBAgIUPWXV8T0fBmdE0eKUp621QrZsl1AwCgYIKoZIzj0EAwIw\n" \
    "HTEbMBkGA1UEAwwSYXJkdWluby1ibCB0ZXN0IENBMB4XDTI2MTAyNDA5MTcyM1oX\n" \
    "DTM2MTAyMTA5MTcyM1owHTEbMBkGA1UEAwwSYXJkdWluby1ibCB0ZXN0IENBMFkw\n" \
    "EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYUH5p3Y4wObRzSWgxo7puKc5lFSQ+ATt\n" \
    "VuhNDColXMxc3bMbcIOqWzjuuqmU/BsM2Hkxzg8NbRXWag030HQJ/qMyMDAwHQYD\n" \
    "VR0OBBYEFPz7hy3GviF2JKYO5HoaH+Dwf49tMA8GA1UdEwEB/wQFMAMBAf8wCgYI\n" \
    "KoZIzj0EAwIDSAAwRQIgLjU9e7qNScnRl93FNewDLeNt6l7i9NV0pjQfrfojXtEC\n" \
    "IQCsjHa00XSN0CA39iEokf62PtKPcgb3ok+poS9dD9Pm+g==\n" \
    "-----END CERTIFICATE-----\n"

#ifdef __cplusplus
extern "C" {
#endif
/*
 * True after wifi_port_init. Earlier net_port access violates the hardware requirement that lwIP
 * tcpip exists first.
 */
extern bool fake_wifi_up;
#ifdef __cplusplus
}
#endif
