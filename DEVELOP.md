# Developing the UNO R4 WiFi USB bridge

This guide is for contributors working on the BL616CL bridge application, its AT/HCI contract, and RA4M1 interoperability. Start with the [README](README.md) for an overview and the [protocol reference](docs/PROTOCOL.md) for wire behavior.

## Architecture and build targets

The board contains two independently programmed MCUs:

| Target | Role | Arduino FQBN |
| --- | --- | --- |
| BL616CL | This repository's resident bridge firmware: USB, AT server, BLE Controller transport, and RA4M1 control | `bouffalolab:bl:unor4wifi_bl616cl` |
| RA4M1 | User sketches using WiFiS3, ArduinoBLE, or the LED matrix | `arduino:renesas_uno:unor4wifi` |

The BL bridge is the BL616CL firmware; it is built and uploaded once for that MCU. RA4M1 applications are separate programs. Building a user sketch for one target does not update the other target.

General Arduino UART/USB/network/storage functionality belongs to the [platform](https://github.com/bouffalolab/arduino-bouffalo/tree/master). SDK configuration and packages belong to the [library builder](https://github.com/bouffalolab/arduino-bouffalo-lib-builder). Keep protocol compatibility and board control here.

## Prerequisites

- Linux x86_64 and an UNO R4 WiFi with the validated BL616CL module 1V0 for hardware testing.
- Git, Bash, Python 3, and a native C/C++ compiler for host tests. Source CI uses Python 3.11.
- Arduino CLI 1.5.1 and matching BL platform/dependency packages.
- Renesas UNO core 1.6.0, including WiFiS3; ArduinoBLE 2.1.0 for BLE scenarios.
- USB serial/HID access and the relevant physical network or BLE peer.
- Optional OpenOCD or pyOCD and the appropriate Renesas device support for SWD work.

The current BL dependency baseline is libs 0.10.0, flash tools 0.2.0, and GCC RISC-V 14.3.0. A dependency rebuild uses the builder's separate pinned environment.

## Local environment

Check out the bridge and the matching platform. These local names allow the host harness to discover the sibling platform automatically:

```sh
git clone --branch master https://github.com/bouffalolab/arduino-bouffalo.git arduino-bl
git clone --branch master https://github.com/bouffalolab/arduino-bouffalo-uno-r4-wifi-usb-bridge.git uno-r4-wifi-usb-bridge-bl
cd uno-r4-wifi-usb-bridge-bl
```

Follow the platform's [development setup](https://github.com/bouffalolab/arduino-bouffalo/blob/master/DEVELOP.md) to use a source checkout with matching installed tools. Select the approved package index or a local candidate index; the public index is unavailable until the first approved release.

For a separate Arduino CLI configuration, include `--config-file /absolute/path/to/arduino-cli.yaml` in the commands below. Install the RA dependencies into the same selected environment:

```sh
arduino-cli core update-index
arduino-cli core install arduino:renesas_uno@1.6.0
arduino-cli lib install "ArduinoBLE@2.1.0"
```

## Build and upload

### BL616CL bridge firmware

Run from the bridge repository root after preparing the platform:

```sh
arduino-cli compile -b bouffalolab:bl:unor4wifi_bl616cl \
  --build-path /absolute/path/to/build/bridge UNOR4USBBridgeBL
```

Keep the default partition scheme and debug logging off for the validated baseline. For a clean build of dependencies, platform examples, and the bridge, use the builder's [integrated firmware workflow](https://github.com/bouffalolab/arduino-bouffalo-lib-builder/blob/master/DEVELOP.md).

Identify the correct board and resolve its stable serial path to the current tty. If the existing firmware is the bridge, normal touch behavior is owned by the bridge; follow the platform's [recovery instructions](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/RECOVERY.md) to enter BL ROM download mode first.

```sh
BL_PORT="$(readlink -f /dev/serial/by-path/YOUR_BL_ROM_DEVICE)"
arduino-cli upload -b bouffalolab:bl:unor4wifi_bl616cl -p "$BL_PORT" \
  --input-dir /absolute/path/to/build/bridge UNOR4USBBridgeBL
```

### RA4M1 application

With the BL bridge running, compile the phone matrix example for the official Renesas target:

```sh
arduino-cli compile -b arduino:renesas_uno:unor4wifi examples/PhoneMatrix
RA_PORT="$(readlink -f /dev/serial/by-path/YOUR_RA_APPLICATION_DEVICE)"
arduino-cli upload -b arduino:renesas_uno:unor4wifi -p "$RA_PORT" examples/PhoneMatrix
```

The [example guide](examples/README.md) describes the phone commands and tested combination. BLE pairing/reconnection has its own compatibility scope. Apply the [ArduinoBLE compatibility patch](compatibility/ArduinoBLE-2.1.0/README.md) explicitly only for the documented library version and scenario, then rebuild the RA application.

## Directory structure

| Location | Responsibility |
| --- | --- |
| `UNOR4USBBridgeBL/` | BL bridge sketch, AT server, USB application behavior, and DAP implementation |
| `compatibility/` | Version-specific RA Host library patches and provenance |
| `examples/` | RA4M1 application examples and their usage/acceptance scope |
| `docs/` | AT/HCI protocol documentation |
| `tests/at_host/` | WiFiS3/bridge host harness and software substitutes |
| `tests/dap_hid_check.py` | Checks against a real running bridge's HID interface |
| `tools/` | Host debugging configuration |
| `.github/workflows/` | Source checks and integrated firmware builds |

## Testing

### Source checks

```sh
python3 -I -m compileall -q tests
python3 -I -m compileall -q examples
git diff --check
```

These check Python syntax and patch whitespace. They do not compile Arduino examples or verify the board.

### AT host integration

Select the installed Renesas core directory containing `cores/` and `libraries/WiFiS3/`. The native compiler builds host versions of both sides:

```sh
python3 -I tests/at_host/run.py \
  --renesas /absolute/path/to/renesas_uno/1.6.0 \
  --platform ../arduino-bl --build /absolute/path/to/build/at-host
python3 -I tests/at_host/tcp_tls_check.py /absolute/path/to/build/at-host/bl_host
python3 -I tests/at_host/file_check.py /absolute/path/to/build/at-host/bl_host
python3 -I tests/at_host/pref_check.py /absolute/path/to/build/at-host/bl_host
```

The harness uses a virtual AP, local sockets, plaintext TLS substitutes, and host storage. It checks protocol behavior and error handling; it does not test real radio, TLS encryption/certificate validation, BLE, or board Flash/littlefs.

### Hardware acceptance

Test the changed behavior through the real RA4M1 application and BL bridge with a physical peer. USB loopback or direct AT commands provide partial diagnostic evidence; they do not establish the complete WiFiS3 or ArduinoBLE path.

For USB/HID changes, identify the matching HID node before running:

```sh
python3 tests/dap_hid_check.py /dev/hidrawN
```

This accesses a real device and requires USB/hidraw permissions. The HID check does not substitute for actual SWD programming/debugging or network/BLE tests.

Record the board/module and wiring, both MCU firmware identities, dependency versions, configuration, host/peer devices, expected behavior, and raw logs. Separate software/fake checks, compilation, image generation, flashing, serial runtime, and end-to-end hardware results. Report the result and count for each category separately.

Before hardware work, confirm device ownership and preserve user-owned serial sessions. Save firmware and persistent data where required; filesystem formatting, layout migration, and SWD writes require a defined target and recovery path.

## Development and contribution workflow

1. Identify the affected AT request/response, defaults, failure behavior, and caller in WiFiS3 or ArduinoBLE.
2. Reproduce the issue with a targeted host or hardware scenario; inspect the upstream contract before changing compatibility behavior.
3. Keep the fix in the correct repository. Implement protocol handling here and general chip APIs in the platform.
4. Run relevant host checks, compile both affected MCU applications, and verify changed behavior on hardware.
5. Update the protocol, examples, compatibility notes, and support scope as applicable. Submit a PR with the trigger, solution, affected version combination, and separate software/hardware results.

Use English code comments and Doxygen-style public C/C++ function documentation. Preserve upstream notices. Keep credentials and private keys out of examples, logs, and release packages.

## CI and release coordination

The source workflow runs syntax checks. Firmware CI builds this bridge with pinned companion revisions and exports logs and image identities; it neither flashes hardware nor publishes a release.

Coordinate bridge revisions with the platform and dependency packages. Preserve tested firmware identities when later documentation or source revisions differ. Use the builder's [controlled release process](https://github.com/bouffalolab/arduino-bouffalo-lib-builder/blob/master/docs/releasing.md) for candidate review, versioned assets, and publication.
