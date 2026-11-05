# arduino-bouffalo-uno-r4-wifi-usb-bridge

[![Source CI](https://github.com/bouffalolab/arduino-bouffalo-uno-r4-wifi-usb-bridge/actions/workflows/source-ci.yml/badge.svg?branch=master)](https://github.com/bouffalolab/arduino-bouffalo-uno-r4-wifi-usb-bridge/actions/workflows/source-ci.yml)
[![Firmware CI](https://github.com/bouffalolab/arduino-bouffalo-uno-r4-wifi-usb-bridge/actions/workflows/firmware-ci.yml/badge.svg?branch=master)](https://github.com/bouffalolab/arduino-bouffalo-uno-r4-wifi-usb-bridge/actions/workflows/firmware-ci.yml)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)

An application bridge for the Arduino UNO R4 WiFi. It connects the RA4M1 application MCU with a BouffaloLab BL616CL wireless MCU and exposes a WiFiS3-compatible interface over the board's existing connections.

## Explore the bridge

[**Open Bridge Explorer →**](https://bouffalolab.github.io/arduino-bouffalo-uno-r4-wifi-usb-bridge/)

An interactive Chinese/English guide to the board, USB and AT paths, Wi-Fi, BLE,
CMSIS-DAP and the PhoneMatrix example. Rotate the illustrated board, follow
protocol replays and draw a 12×8 matrix frame in your browser. Replays are
illustrations, not hardware logs; the site neither connects to a board nor
changes the firmware release status. Site maintenance: [site/README.md](site/README.md).

## Features

- USB serial communication between the host, RA4M1, and BL616CL.
- Wi-Fi station and access-point operation with TCP, UDP, TLS, DNS, IPv6, and multicast support.
- SNTP time queries, DHCP hostname handling, LittleFS file access, and Preferences storage.
- BLE HCI transport over AT for use with ArduinoBLE on the RA4M1 side.
- Vendor USB HID and CMSIS-DAP support for RA4M1 programming and debugging.
- Recovery and coexistence paths for USB, Wi-Fi, BLE, and serial workloads.

The current master line is an initial candidate. The published hardware scope is Linux x86_64 with an Arduino UNO R4 WiFi using the validated BL616CL module revision; other hosts, board revisions, and long-duration scenarios require separate validation. Review the support and release information before deploying a feature.

## Getting started

### Prerequisites

- Arduino UNO R4 WiFi with a compatible BL616CL module.
- Arduino IDE or Arduino CLI with the Renesas UNO R4 platform.
- Matching versions of the `arduino-bl` platform, dependency packages, bridge application, and ArduinoBLE where BLE is used.
- USB access to the board. Linux is required for the currently validated CMSIS-DAP and upload workflow.

### Installation

1. Install the matching platform and dependency packages from the selected release.
2. Build the bridge application for the BL616CL coprocessor and upload it through the platform's download workflow.
3. Select the official Renesas UNO R4 WiFi board to build and upload RA4M1 user applications.
4. Reconnect the board and verify USB serial communication before testing Wi-Fi, BLE, or debug features.

Keep the bridge, platform, and dependency packages on the same release. Their protocol and binary interfaces are versioned together.

## Usage

The following sketch runs on RA4M1, with the bridge firmware already running on BL616CL.

```cpp
#include <WiFiS3.h>

char ssid[] = "your-network";
char password[] = "your-password";

void setup() {
  Serial.begin(115200);
  while (WiFi.begin(ssid, password) != WL_CONNECTED) {
    delay(1000);
  }
  Serial.println("Wi-Fi connected");
}

void loop() {
  delay(1000);
}
```

The same bridge transport can be used by ArduinoBLE applications and by host tools that need USB serial or CMSIS-DAP access. Network, storage, BLE, and debug capabilities have separate limits and validation scopes.

## Directory structure

- **Bridge application** — AT command handling, RA4M1 control, serial transport, and USB application behavior.
- **Protocol and compatibility** — WiFiS3, BLE HCI, storage, and host interoperability contracts.
- **Examples** — representative networking, BLE, storage, and board workflows.
- **Validation and documentation** — host checks, support scope, recovery guidance, and release notes.

## Contributing

See the [developer guide](DEVELOP.md) for prerequisites, local setup, the two MCU build targets, tests, and the contribution workflow.

Use GitHub Issues for protocol compatibility problems, board-specific failures, and documentation gaps. Pull requests should state the affected MCU, host interface, protocol behavior, and validation scope. Changes that alter the bridge contract should be reviewed with the matching platform and dependency package revisions.

## License and acknowledgement

This repository is released under the [Apache License 2.0](LICENSE). It works with the Arduino UNO R4 WiFi software stack, ArduinoCore-API, ArduinoBLE, and BouffaloLab platform packages; keep the corresponding third-party notices with redistributed builds.

The bridge is designed to work with [arduino-bl](https://github.com/bouffalolab/arduino-bouffalo) and [arduino-bl-lib-builder](https://github.com/bouffalolab/arduino-bouffalo-lib-builder).
