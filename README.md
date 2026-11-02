# BL UNO R4 WiFi USB bridge

This repository is the workspace's BL UNO bridge, under `uno-r4-wifi-usb-bridge-bl`.

It replaces the UNO R4 WiFi ESP32-S3 with a BL616CL wireless coprocessor and USB bridge; the RA4M1 continues to run user programs.

Current stage: USB serial forwarding, RA4M1 AT networking, and CMSIS-DAP/SWD have been tested on the first board. See the roadmap for the remaining initial-release requirements and release gates.

Once the generic platform reaches the minimum baseline required by the application, bridge delivery takes priority. The application owns AT, RA4M1 pins, and CMSIS-DAP and uses underlying capabilities through generic platform interfaces.

Public references: [Platform installation](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/INSTALL.md), [support scope](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/SUPPORT.md), [recovery](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/RECOVERY.md), and [AT protocol notes](docs/PROTOCOL.md).

## Build and upload

The firmware is the `UNOR4USBBridgeBL/` sketch on arduino-bl, FQBN `bouffalolab:bl:unor4wifi_bl616cl`. The current local candidate requires `bl616cl-libs` 0.9.0 with HID, external-host Controller, littlefs, and the matching network ABI, `bl-flash-tools` 0.2.0, and the matching platform. The default module is 1V0 (MD GPIO14, SWCLK GPIO12, SWDIO GPIO13). For 1V1, the mapping is MD GPIO10, SWCLK GPIO8, SWDIO GPIO9; set both `--build-property compiler.cpp.extra_flags=-DBRIDGE_MODULE_1V1` and `--build-property compiler.c.extra_flags=-DBRIDGE_MODULE_1V1`. The 1V1 mapping has not been tested on hardware.

```sh
arduino-cli compile -b bouffalolab:bl:unor4wifi_bl616cl --build-path <build-directory> UNOR4USBBridgeBL
arduino-cli upload -b bouffalolab:bl:unor4wifi_bl616cl -p <serial-port> --input-dir <build-directory> UNOR4USBBridgeBL
```

The running bridge handles 1200 baud for the RA4M1 itself and disables BL616CL 1200 bps touch. To update the bridge, first enter BL616CL ROM download mode, then upload through its ROM download serial port (`ffff:ffff`). There are three entry methods:

- Send `AT+RESTARTBOOTLOADER` from RA4M1 (the official entry). After replying `OK`, the bridge resets into ROM download mode. WiFiS3 has no wrapper for this command; send it from an RA4M1 sketch as follows:

  ```cpp
  #include <WiFiS3.h>

  modem.begin();
  std::string res;
  modem.write(std::string(PROMPT(_RESTART_BOOTLOADER)), res, "%s", CMD(_RESTART_BOOTLOADER));
  ```

- Hold GPIO36 (J6.4) high when resetting BL616CL.
- Send a Feature report with report ID 0 to vendor HID, with payload byte 0 equal to `0xAA` (prefix the payload with report ID byte 0 for Linux hidraw). GET Feature returns three bridge version bytes, currently `00 01 00`. This enters BL ROM update mode for the BL upload tools; ESP32 firmware images and flashing protocols are incompatible.

ROM download uploads have occasionally failed (3 of 77 on the first board on 2026-10-23: 2 handshake failures and 1 ROM response loss during writing; cause unknown. Control groups with and without RA4M1 serial transmission each passed 15 uploads). In ARDUINO-52, after a real write interruption, complete power removal, and initial readback, the confirmed FTDI GPIO36/reset controls recovered the device through ROM. The current candidate passed 100 uploads. The historical 3/77 root cause remains unknown; the user accepted it as a Low, nonblocking issue. This does not establish a root-cause fix.

## Features

| Feature | Status | Evidence or notes |
| --- | --- | --- |
| CDC + vendor HID / CMSIS-DAP v1 transport; SWD to a real RA4M1, 64-byte packets and one request in flight; no JTAG/SWO | experimental | Linux x86_64, BL616CL 1V0, libs 0.4.1: OpenOCD 0.11.0-arduino2 / pyOCD 0.45.0 (Renesas.RA_DFP 6.6.0), requested 100 kHz. DPIDR `0x5ba02477`, CPUID `0x410fc241`; SRAM/register read/write and restoration, sketch programming and full readback, hardware breakpoints, stepping, reset, and three reconnections passed. Concurrent CDC and continuous SWD reads preserved 50 B, 2 KB, 8 KB, and 64 KB echoes. Disconnect DAP before serial upload. See [Third-party notices](THIRD_PARTY_NOTICES.md) for sources and modifications. |
| Bidirectional forwarding between USB serial (`Serial`) and RA4M1 user serial (`Serial1`, uart0 GPIO34/35 through U4 to SCI9). Other baud settings update `Serial1` (8N1); pending unforwarded RX data is cleared after each change | experimental | First board (1V0), Linux: an FT232 replacing RA4M1 on GPIO34/35 transferred 1–8 KB byte-for-byte in each direction at 115200, 9600, 230400, and 921600. At 921600, simultaneous 64 KB transfers in both directions matched for 3 rounds. Baud settings followed port reopening. With RA4M1 after R47/R48 rework, 50-byte and 2 KB echoes matched at 115200. |
| 1200: MD high and double RESET to enter RA4M1 bootloader. 2400: MD low and one RESET to enter RA4M1 ROM download mode; MD stays low until the next 1200 setting, matching the official bridge | experimental | First board (1V0), Linux: arduino-cli (renesas 1.6.0, unor4wifi) uploaded and ran RA4M1 firmware through the bridge; bossac connected to the bootloader at 921600. After 2400, RA4M1 ROM synchronization responded (0x00→0x00, 0x55→0xC3); a subsequent 1200 upload restored it. BL616CL did not enter download mode at 1200. |
| AT commands (`Serial2`, uart1 GPIO6/7 through U4 to RA4M1 SCI1, 115200 8N1, separate task): WiFiS3 STA, AP, and TCP client commands SOFTRESETWIFI, FWVERSION, WIFIMODE (0 OFF, 1 STA, 2 AP, 3 STA+AP), BEGINSTA, BEGINSOFTAP, SOFTAPCONFIG, IPSOFTAP, GETSOFTAPSSID, MACSOFTAP, GETSTATUS, IPSTA, GETSSID, GETRSSI, GETBSSID, MACSTA, DISCONNECT, HOSTBYNAME, PING, BEGINCLIENT, CLIENTCONNECTNAME, CLIENTCONNECT, CLIENTCONNECTIP, CLIENTSTATE, CLIENTSTATUS, IPCLIENT, PEEK, CLIENTSEND, AVAILABLE, CLIENTRECEIVE, CLIENTCONNECTED, CLIENTCLOSE, FLUSH, REMOTEIP, REMOTEPORT. Failures return `+<command>:ERROR`, with no space after the colon; otherwise WiFiS3 fails to recognize errors for commands such as CLIENTRECEIVE/WIFISCAN and waits the full 10 s | experimental | First board (1V0), RA4M1 with unmodified WiFiS3 (renesas 1.6.0): WPA3 connection acquired an address in 5.4 s; DNS, gateway ping, HTTP on port 80 by IP/name, a 3000-byte POST body, and disconnect worked. Binary TCP payloads of 1 B–4 KB, including 0x00 and CRLF, matched byte-for-byte against a LAN computer echo server. Forwarding and upload through the bridge passed regression with the same firmware. |
| `AT+RESTARTBOOTLOADER`: reply `OK`, then reset into BL616CL ROM download mode to update this firmware; upstream resets without replying | experimental | First board (1V0), Linux: RA4M1 sent the command using unmodified WiFiS3; the ROM port appeared after 0.6–0.9 s. The bridge re-enumerated after USB upload; RA4M1 received `OK` without being reset, and its serial data reached the computer afterward. 6 consecutive rounds passed. A separate run lost ROM responses during upload in round 4; see above. Host integration tests cover the response and reset call. |
| `AT+WIFISCAN` (WiFiS3 `scanNetworks()`): call platform `WiFi.scanNetworks()` and list the strongest 10 networks in upstream format. With no networks, return `+WIFISCAN: 0\|`; upstream returns only `OK` | experimental | First board (1V0), RA4M1 with unmodified WiFiS3 (renesas 1.6.0): disconnected and connected scans returned in 1.9–2.0 s with valid fields, ordered RSSI, the test AP, and current BSSID. The connection remained usable and gateway ping passed afterward. Unmodified ScanNetworks listed 8 networks for 3 rounds; WiFiS3 skipped 2 hidden networks among the 10. Host integration tests cover spaces and 32-byte SSIDs, encryption types, zero networks, and scan failure. |
| UDP (WiFiS3 `WiFiUDP`): UDPBEGIN, UDPBEGINPACKETIP, UDPBEGINPACKETADD (hostname), UDPWRITE, UDPENDPACKET, UDPPARSE, UDPAVAILABLE, UDPREAD, UDPPEEK, UDPFLUSH, UDPREMOTEIP, UDPREMOTEPORT, UDPSTOP. Up to 4 objects (0–3, independent of TCP IDs), matching upstream; 1460 bytes per datagram. Shares the platform's 8 sockets with TCP | experimental | First board (1V0), RA4M1 with unmodified WiFiS3: 18 binary datagrams of 1–1460 bytes matched over 3 LAN round-trip rounds without retransmission, taking 41–336 ms. A datagram from another source port was answered on that port. Unmodified WiFiUdpSendReceiveString returned acknowledged for all 3 computer requests; WiFiUdpNtpClient time differed from the computer by 1.4–1.6 s over 3 queries. Host integration tests cover segmented writes, hostname sending, frame synchronization after writes exceeding 1460 bytes fail, the 4-object limit, and reuse after release. |
| TCP server (WiFiS3 `WiFiServer`): BEGINSERVER, SERVERAVAILABLE, SERVERACCEPT, SERVERWRITE, SERVEREND. Up to 4 servers (0–3), matching upstream; accepted connection IDs are 1000–1007 and use AVAILABLE, CLIENTRECEIVE, CLIENTSEND, CLIENTCLOSE, etc. SERVERAVAILABLE reports only connections with module-side data; SERVERWRITE targets connections not transferred by accept; SERVEREND stops only listening. WiFiS3 `read()` fetches an entire segment into an RA4M1-local buffer; changing IDs through `server.available()` discards those bytes. Consequently, unmodified WiFiChatServer echoes only the first character of a segment. Upstream ESP32 firmware behaves the same, based on source inspection; see the protocol document. Retaining connections returned by `accept()`, as in WiFiAdvancedChatServer, avoids this issue | experimental | First board (1V0), RA4M1 with unmodified WiFiS3 (renesas 1.6.0), computer clients: 3 WiFiWebServer GETs returned 200 with 282 bytes and closed afterward. Both WiFiChatServer clients received a line sent character-by-character; a whole segment echoed only its first character. WiFiAdvancedChatServer assigned client IDs 0 and 1, forwarded each client's line only to the other, and detected disconnects. Host integration tests cover repeated returns of the same connection, accept, broadcast, recycling 10 connections, rejection after end, the 4-server limit, reuse after release, and begin failure before networking. |
| TLS client (WiFiS3 `WiFiSSLClient`): SSLBEGINCLIENT, SETCAROOT (bundle or PEM up to 16384 bytes), SSLCLIENTCONNECTNAME, SSLCLIENTCONNECT, SSLCLIENTCONNECTIP, SSLCLIENTSTATE, SSLCLIENTSTATUS, SSLIPCLIENT, SSLPEEK, SSLCLIENTSEND, SSLAVAILABLE, SSLCLIENTRECEIVE, SSLCLIENTCONNECTED, SSLCLIENTFLUSH, SSLREMOTEIP, SSLREMOTEPORT, SSLCLIENTCLOSE. IDs 2000–2007 (8, matching upstream). Uses platform `WiFiSSLClient`, TLS 1.2, bundled roots by default, and no certificate date validation; see arduino-bl README for limits. `SETECCSLOT=<socket>,<slot>,<der-len>` receives a DER certificate and reads a provisioned SEC1 DER private key from Preferences `tls-sse/slotN` (N=0–15) for subsequent TLS connections. `SSLCLIENTSETCERT` / `SSLCLIENTSETPKEY` directly set PEM data. BL616CL memory limits simultaneous connections: historical firmware achieved 2; the ARDUINO-51 stress case after sequential tests achieved only 1. Excess connect attempts explicitly fail and released resources can be reused | experimental | First board (1V0), RA4M1 with unmodified WiFiS3 (renesas 1.6.0): bundled roots accepted www.baidu.com and valid-isrgrootx2.letsencrypt.org and rejected self-signed/untrusted-root.badssl.com. After `setCACert` with a test CA, HTTP transfers of 100062 bytes with the computer TLS server matched byte sums (about 11 s), and echoes matched. Name mismatch failed; an expired certificate succeeded. Opening 8 connections produced 2 successes in each of two runs. WiFiWebClientSSL with only www.google.com replaced by www.baidu.com printed HTTP/1.1 200 and ended normally. The unmodified example could not reach google on this network and ended normally after connection failure. Host integration tests cover bundled/custom CA trust, SETCAROOT payload/frame synchronization, historical SETECCSLOT rejection, timeout parameters, peer close, mixed TCP/TLS IDs, and the 8-ID limit. |
| ARDUINO-51 additional TCP/TLS commands and client certificate slots | experimental | Real UNO R4 WiFi/BL616CL 1V0, original WiFiS3 1.6.0, vela: 139+119 assertions passed, including TCP/TLS states/endpoints, binary PEEK/reads, software key slots, original `setEccSlot()` mutual TLS, incorrect key/certificate rejection, and default/explicit timeouts. Host fake 78/78 and contract checks 35/35 are auxiliary only. Omitted or nonpositive timeouts for `CLIENTCONNECT` / `SSLCLIENTCONNECT` use 3000 ms. The full SOFTSE command set is outside scope. Software slots are not secure elements; the application provisions private keys through Preferences. |
| Static addressing (WiFiS3 `config`, `setDNS`): SETIP (address, gateway, mask, optionally 1–2 DNS servers) calls platform `WiFi.config` / `setDNS`. Address 0.0.0.0 restores DHCP; SOFTRESETWIFI also restores DHCP, matching upstream. WiFiS3 `config` first sends SOFTAPCONFIG for the AP address, then SETIP for the STA address | experimental | First board (1V0), RA4M1 with unmodified WiFiS3 (renesas 1.6.0): the same platform checks passed: static configuration before/during connection, disconnect/reconnect, `setDNS`, DHCP restoration, and DHCP-to-static conversion through `setDNS`; gateway ping, DNS, computer TCP echo, and computer-to-board ping passed. Host integration tests cover immediate application while connected, two DNS servers, reconnect retention, `config(address)` defaults, missing parameters, invalid addresses, DHCP restoration, and SOFTRESETWIFI. |
| AP (WiFiS3 `beginAP`, `softAPIP`, `softAPSSID`): BEGINSOFTAP (SSID, password, optional channel), SOFTAPCONFIG, IPSOFTAP, GETSOFTAPSSID, MACSOFTAP. WIFIMODE=2 selects AP; WIFIMODE=1 / `begin` returns to STA. STA/AP switch by default; explicit WIFIMODE=3 permits concurrency | experimental | Host integration: 78/78; 8 AP checks among 78 initially failed and all passed after integration. First board (1V0): complete run-ap-2 passed 34/34; the computer received a DHCP address, status 8, ping replies, and TCP/UDP echoes; SOFTRESETWIFI stopped the AP. The AP→STA `begin` path passed platform hardware and bridge host checks but was not yet covered on bridge hardware in this record. See the published [AP acceptance scope](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/SUPPORT.md). |
| `AT+FWVERSION_U32` (WiFiS3 `firmwareVersionU32()`): three raw version bytes followed by 0, matching upstream; WiFiS3 reads the first 3 bytes. Components equal to 10, 13, 32, or 124 break its parser, also upstream behavior; this firmware is 0.1.0 | experimental | First board (1V0), RA4M1 with unmodified WiFiS3: two reads both returned 0x000100. Covered by host integration tests. |
| BLE HCI over AT (HCIBEGIN, HCIEND, HCIWAIT, HCIAVAILABLE, HCIREAD, HCIWRITE) | experimental | Linux + BL616CL 1V0, libs 0.5.0, real RA4M1 with unmodified ArduinoBLE 2.1.0: 7 official examples passed; 3 connections each for central/peripheral and 60 GATT read/write/notification rounds; 60 concurrent STA/TCP/BLE/USB rounds and 10 BLE start/stop cycles passed. BL runs only the Controller. Public-address, RAM-key numeric comparison/rejection/timeout/encrypted reads/writes/notifications and key-based reconnection were tested. RPA, key persistence, and long-duration stability remain unaccepted; see below and ARDUINO-44. |
| File AT commands (MOUNTFS and FS delete/overwrite/read/append), using platform LittleFS | experimental | BL616CL 1V0 + real RA4M1, official WiFiS3 1.6.0: 10009-byte binary files, 1023-byte reads, segmented/invalid frames/timeouts, full-filesystem errors, and reuse after deletion passed. Limits are below and in ARDUINO-47. |
| Preferences AT (nine PREF commands), using platform Preferences/LittleFS | experimental | Original RA Modem type round trips, binary data, real full-filesystem/recovery behavior, and coexistence with ordinary files passed; see below and ARDUINO-48. |
| SNTP time queries and DHCP hostname (GETTIME, HOSTNAME) | experimental | BL616CL 1V0: hardware time and DHCP option 12 evidence is in ARDUINO-45. RAM hostname, UTC clock, and TLS policy limits are below. |
| OTA | unimplemented | Corresponding AT commands return `+<command>:ERROR`. |

## File AT boundaries

First send `AT+MOUNTFS=0` (or `WiFiFileSystem.mount(false)`). Follow `AT+FS=0,1,/path,n` immediately with n raw bytes to overwrite/create; op=3 appends, op=0 deletes, and op=2 takes `/path,offset,length`. Successful writes/deletes return bare `OK\r\n`; reads return `+FS: <actual-length>|<raw-bytes>\r\nOK\r\n`. EOF, missing files, and invalid operations explicitly return `+FS:ERROR\r\n`. There are no separate size/exists AT commands.

Each write/append frame accepts 0–255 bytes; each read returns at most 1023 bytes. Total file size is limited by free filesystem capacity. The shared buffer receives a complete frame before Flash access to avoid losing UART payload during erase/write; whole files are not buffered on the stack. Paths must start with `/`; `..` traversal is rejected. Paths cannot contain commas, CRLF, or NUL; the complete AT line is limited to 256 bytes. The AT task executes serially with no concurrent file transactions. Once a valid write length is parsed, failures still consume the declared payload. A payload gap exceeding 1 second returns an error; the host must abandon the old payload and start a new command. Failed overwrite/append operations do not guarantee rollback.

`MOUNTFS=1` still only attempts mounting and never auto-formats on failure, unlike upstream format-on-fault, to protect existing data. The default platform filesystem occupies an independent 416 KiB region. Ordinary same-layout Arduino uploads preserve it; layout changes do not promise automatic migration. WiFiFileSystem `readfile()` prints data as a C string; exact binary reads require the original Modem length mode. EOF returns an error to terminate that read loop. File AT access to the Preferences private path `/.preferences` and descendants is forbidden; see the PREF contract and limits below.

## Preferences AT

The nine `PREFBEGIN/END/CLEAR/REMOVE/PUT/GET/TYPE/LEN/STAT` commands follow the pinned upstream wire contract.
Generic storage is implemented by the platform `Preferences` library; the bridge only parses parameters and transfers raw payloads.
Official RA4M1 WiFiS3 1.6.0 supplies command names but no Preferences wrapper; hardware tests used its unmodified Modem.
Namespaces and keys are each 1–15 bytes. Types 0–7 are fixed-width integers, 8 is a string, and 9 is a BLOB; each value is at most 1023 bytes.
Flash is written only after complete reception; errors return `+PREFx:ERROR\r\n`.
`PREFSTAT` reports remaining logical key slots in the current namespace (64-count), unlike upstream NVS partition-wide statistics; it cannot guarantee physical free space.
A failed value commit returns 0. An empty string also returns 0 on success, requiring a typed read for confirmation.
Default/empty/lfs partitions are accepted; other partition names are rejected. Mount/format errors never trigger automatic formatting.
Files and Preferences share 416 KiB; atomic replacement requires temporary space, with no reserved quota.
Status: experimental. Real RA original-Modem round trips for every type/1023 bytes and full-filesystem error/recovery passed.
When full, adding a PREF key returns 0 even below 64 slots; existing keys, other namespaces, and ordinary files remain intact.
All 6 real RA frames under a mount fault explicitly failed, and the complete 416 KiB filesystem readback was unchanged without auto-formatting.
Acceptance and hardware/auxiliary evidence are recorded separately in ARDUINO-48.

## BLE boundaries

Requires pinned ArduinoBLE 2.1.0, Renesas core 1.6.0, and libs 0.5.0. HCIREAD returns at most 257 bytes per call. HCIAVAILABLE provides a bounded window and returns 0 once after each complete H4 packet so poll can return without ArduinoBLE's shared RX buffer overwriting command results. HCIBEGIN starts a new Host session. A truncated HCIWRITE failure closes the Controller and requires another HCIBEGIN. See the [HCI AT contract](docs/PROTOCOL.md#hci-over-at) for requests and responses.

In this ArduinoBLE version, AT transport `end()` is empty; release the Controller by sending HCIEND through the WiFiS3 modem. Its `wait()` has a malformed command, so timed `BLE.poll(timeout)` is unaccepted. Ordinary polling has been tested on hardware. HCIBUFFERALLOC, mentioned in Jira, does not exist in the pinned official bridge/ArduinoBLE/WiFiS3; this firmware explicitly returns ERROR.

The real peer is an Intel AX200. Three official central examples use Bumble Host driving the physical radio directly; four peripheral examples use BlueZ/Bleak. Neither uses simulated HCI. The BlueZ/Linux peer previously received ACL before the connection event and dropped the first packet. A custom central test waits 250 ms after BlueZ connection and passed 3 connections/60 rounds. Official examples and ArduinoBLE were unmodified. See the [BLE acceptance scope](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/SUPPORT.md) for the published scope; detailed raw records remain in the release review archive.

Pairing hardware tests used a custom RA4M1 security probe and physical AX200/Bumble with unmodified ArduinoBLE: unencrypted access returned ATT 0x0F; numeric comparison acceptance/rejection, 8-second confirmation timeout, 30-second pairing-window expiry, 10 encrypted read/write/notification rounds, and existing-key reconnection after disabling new pairing passed. Keys remained only in RAM; keys and confirmation codes were not logged. These tests used public addresses and did not verify RPA or key recovery after power loss. RA4M1 was reset between negative and positive cases because the library may retain a delayed protected-read response across connections; continuous recovery along that path is not claimed.

ARDUINO-53 supplement: with original ArduinoBLE 2.1.0 and a BlueZ public-address peer that does not distribute initiator IdKey, initial pairing/encryption passes but LTK is not stored and bonded reconnection fails. When this scenario is required, explicitly patch the user library using the [RA ArduinoBLE compatibility patch](compatibility/ArduinoBLE-2.1.0/README.md) and rebuild the RA program. The patch changes neither BL Core nor bridge firmware. Original-library success with Bumble does not establish original-library BlueZ bonded reconnection success.

Five other official examples remain unaccepted on current hardware: LedControl and ButtonLED lack button wiring; BatteryMonitor lacks a defined A0 input; EncryptedBatteryMonitor also requires board/security adaptation; SensorTagButton lacks a physical TI SensorTag. Compilation of 12 examples is software auxiliary evidence only.

The 0.5.0 package additionally passed AP regression: real scans discovered channels 1/6 before, during, and after BLE operation. On channel 6, the computer connected and received DHCP; 3 pings, 60 concurrent TCP/UDP/BLE/USB rounds, and continued TCP after 10 BLE start/stop cycles passed. The earlier scanner did not wait for NetworkManager LastScan to update and reported no AP. It passed after checking scan completion. Old failures and pre-change bridge control logs remain retained; they do not establish an actual bridge AP regression.

## SWD debugging and programming

USB HID uses vendor page `0xFF00`, no report ID, and 64-byte Input/Output/Feature reports. DAP uses interrupt OUT/IN only; HID SET_REPORT Output is not a substitute. Feature uses the control endpoint. DAP and line switching execute in the main loop; USB interrupts only transfer data. Baud settings 1200/2400 disconnect DAP, release SWD, and take over RESET; debug tools must reconnect afterward. Ordinary CDC forwarding can run alongside SWD access.

Linux requires USB and hidraw permissions. Install the platform [udev rules](https://github.com/bouffalolab/arduino-bouffalo/blob/master/tools/99-arduino-bl.rules) and reconnect the board; without them, OpenOCD may report no device found. Verified debug entry:

```sh
openocd -f tools/ra4m1_swd.cfg
# Then connect Cortex-M GDB to localhost:3333.
```

This OpenOCD configuration provides debugging and memory access without an RA4M1 Flash driver. Programming uses pyOCD 0.45.0 and `Renesas.RA_DFP` 6.6.0. Run `pyocd pack install r7fa4m1ab`, then find the board USB serial number with `pyocd list`. Write only a sketch `.bin` built for `arduino:renesas_uno:unor4wifi` at `0x4000`, with length at most `0x3C000`. `0x0000–0x3FFF` contains the bootloader; DataFlash/configuration regions are outside this upload scope. Back up original firmware that must be retained first.

```sh
pyocd commander -u <USB-serial-number> -t r7fa4m1ab -f 100000 \
  -O auto_unlock=false -O chip_erase=sector \
  -O smart_flash=false -O keep_unwritten=true \
  -c "reset halt" -c "load sketch.bin 0x4000" \
  -c "compare 0x4000 sketch.bin" -c reset
```

Keep `reset halt` before programming: loading the Flash algorithm directly from a running Arduino program failed initialization in this test, while explicit reset/halt allowed programming. SWCLK is generated in software; a requested 100 kHz has been verified. GPIO/scheduling overhead reduces the actual frequency. This round supplies no evidence for 1V1, other hosts, JTAG/SWO, or higher frequencies.

## Tests

On a running bridge, `python3 tests/dap_hid_check.py /dev/hidrawN` checks device identity, capabilities/packet size, erroneous commands, invalid clocks, consecutive requests, and Feature version: 110 checks without target-memory changes. The published scope for hardware SWD programming/debugging and combined USB/serial acceptance is in [the support summary](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/SUPPORT.md).

`tests/at_host/run.py` runs computer-side AT integration tests. The RA4M1 side is unmodified WiFiS3, compiled directly from installed `arduino:renesas_uno` sources. The BL616CL side uses this repository's AT server and arduino-bl WiFi library, with Wi-Fi replaced by a virtual AP and sockets by local computer sockets. The programs communicate through socketpair. Its 78 checks cover scans, connections, address queries, DNS, ping, TCP/UDP transfer, TCP servers, TLS clients, static addresses, AP and STA/AP switching, firmware version, socket limits, error responses, and RESTARTBOOTLOADER. The script runs the UDP echo peer on 127.0.0.1 and TCP peer in a client thread. Its BLEController substitute is always unavailable and does not test BLE. TLS uses an unencrypted substitute (`tests/at_host/bl/fake_tls_port.c`), verifying hostname/certificate/data forwarding to the platform without testing encryption or certificate validation. Requires `g++` and `python3`. The default arduino-bl path is the sibling directory, overridable with `--platform`:

```sh
python3 tests/at_host/run.py --renesas <arduino-data-directory>/packages/arduino/hardware/renesas_uno/1.6.0
```

Additional TCP/TLS contract checks: `python3 tests/at_host/tcp_tls_check.py <build-directory>/bl_host`, 35 software/fake checks covering empty timeouts, invalid/mixed IDs, states/local endpoints, and PEEK. First build with `run.py --build <directory>`. Real certificate, private-key, and wireless acceptance is recorded separately.

With `AT_HOST_TRACE=1`, the BL side logs raw serial traffic to `bl_host.stderr`. Host tests do not replace hardware results.

File commands additionally use `python3 tests/at_host/file_check.py <build-directory>/bl_host`: 32 software/fake checks with POSIX file storage and injected write/commit/mount failures. They run neither real littlefs nor Flash and do not count as hardware passes. Real hardware evidence is retained separately; see [the published file/storage scope](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/SUPPORT.md).

See the [WiFiS3 AT protocol](docs/PROTOCOL.md) for the protocol and command subset. `AT+FWVERSION` returns firmware version 0.1.0. WiFiS3 examples compare it as a string with 0.6.0 and print an upgrade prompt without affecting operation. `AT+BEGINSTA` treats everything after the first comma as the password; passwords containing commas were verified in host tests.

Preferences auxiliary checks: `python3 tests/at_host/pref_check.py <build-directory>/bl_host`.
The 172 software/fake checks use POSIX storage and inject write/close/rename/mount failures without running real littlefs/Flash.
Hardware overflow and recovery acceptance is summarized in [the published storage scope](https://github.com/bouffalolab/arduino-bouffalo/blob/master/docs/SUPPORT.md).

## WiFi/net AT additions (ARDUINO-49)

`WIFIMODE=0..3[,autoconnect]` / `WIFIMODE?` delegate interface lifecycle to the platform. `DISCONNECT[=wifi_off]` disconnects only STA; `DISCONNECTSOFTAP[=wifi_off]` stops only AP. Default parameter 0 retains the mode bit; 1 removes the corresponding bit while the other interface continues running. `SETIP` calls independent platform `configSTA`; `SOFTAPCONFIG` calls `softAPConfig`. Online/offline configurations are no longer shared.

`RECONNECT?` / `RECONNECT=0|1` control unexpected-disconnection retries, default 1. Explicit disconnect suppresses retries; the policy resumes after the next begin acquires an IP. `AUTOCONNECT?` always returns 0; its setter and the second WIFIMODE parameter have no effect. This is a deprecated placeholder contract in the pinned official ESP32 submodule, not boot-time auto-connect, and it stores no credentials. `IPV6` enables STA link-local; `IPV6?` returns a valid address or `::`.

`BEGINSOFTAP=ssid[,pass[,channel[,hidden[,max_connection]]]]` defaults to open / 1 / 0 / 5. Hidden mode must affect beacons and wildcard probes. Accepted association limits are 1..5, but the SDK has only 4 slots, so default 5 has an effective limit of 4. Invalid frames return `+CMD:ERROR`; parseable but invalid AP parameters return 0 without stopping a working connection. AP addressing currently supports /24; changing addresses restarts DHCP. In mode2, `IPSTA=1/2` returns AP gateway/mask following existing WiFiS3 behavior; mode3 returns independent STA configuration. `IPSTA=0` always returns the STA address.

Requires `bl616cl-libs` 0.7.0 and matching platform source. This task's records define first-board verification and missing coverage. Host/fake checks cannot establish wireless behavior; one client cannot validate the multi-client association limit.

## Time and hostname (ARDUINO-45)

`AT+GETTIME=` immediately reads platform `WiFi.getTime()`, returning 0 before synchronization without waiting for SNTP. `AT+HOSTNAME?` queries the name; `AT+HOSTNAME=name` calls the platform setter. Validation, RAM lifetime, and DHCP application timing belong to the platform. Default SNTP is `pool.ntp.org`; these commands do not change TLS policy. Requires complete `bl616cl-libs` 0.8.0 and the matching platform. See workspace `docs/reference/wifis3-at-protocol.md` for the protocol and explicit upstream differences, and ARDUINO-45 for hardware acceptance.

## UDP multicast and destination reuse (ARDUINO-50)

Requires the platform and `bl616cl-libs` 0.9.0. `AT+UDPBEGINMULTI=port,groupIP` allocates UDP ID 0..3 and joins an IPv4 group. `AT+BEGINPACKETMULTI=sock` selects the joined group destination; `AT+UDPBEGINPACKET=sock` reuses the latest explicit/multicast destination or received datagram source. The latter two commands return an empty result and OK on success; transmission still uses UDPWRITE/UDPENDPACKET. No current destination, multicast begin on a non-multicast socket, invalid ID/group/port, or missing parameters returns command ERROR without consuming an ID. UDPSTOP closes the socket; platform/lwIP leaves the group.

Multicast port must be 1..65535 and the address must start with 224..239 and satisfy underlying group restrictions. 0.0.0.0 does not fall back to ordinary UDP. Numeric parsing is strict, unlike permissive inputs in the pinned official ESP32 implementation. Four UDP objects share the platform's 8 sockets with TCP/TLS; exhaustion explicitly fails and closing allows reuse. The 1460-byte limit and error-payload consumption/frame synchronization contract remain unchanged. WiFiS3 sends the first two commands; argument-free UDPBEGINPACKET is also covered against the pinned official implementation.

Status: experimental. Real RA4M1 with unmodified WiFiS3 through vela verified bidirectional 1/255/1460-byte multicast, source IP/port, no reception after leaving, four groups on one port, 20 cycles, ID reuse, three destination-reuse paths, UART error-frame synchronization, and UDP/TCP/TLS resource boundaries. Final firmware and evidence are in the separate ARDUINO-50 task. Software/fake IGMP verifies only the protocol and cannot replace these hardware records. Wireless multicast may lose datagrams; this is not reliable-transfer or long-duration acceptance. Concurrent STA/AP retains the same-channel limitation.
