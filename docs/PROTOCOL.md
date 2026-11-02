# BL bridge AT contract reference

This reference describes the reviewed bridge 0.1.0. The implementation is
`UNOR4USBBridgeBL/at_server.cpp`; the README lists network command coverage and
official differences. The RA Host reference is Renesas core 1.6.0 / WiFiS3,
with ArduinoBLE 2.1.0 for BLE. The BL chip runs only the BLE Controller.

RA AT uses Serial2/SCI1 at 115200 8N1, without flow control. Requests end in CRLF:
`AT+CMD=a,b\r\n`, `AT+CMD?\r\n`, or `AT+CMD\r\n`. Normal success uses a command
prefix and an OK terminator; raw send commands can return bare `OK\r\n`.
Length-framed binary payloads are raw bytes, not escaped text. In particular,
CLIENTRECEIVE has no CRLF between its payload and OK, while FS/PREFGET retain
their documented CRLF. Do not use a single line parser for all response types.

## HCI over AT

| Request | Contract |
| --- | --- |
| `AT+HCIBEGIN` | Start a new Host session, restart the Controller, discard old events/incomplete H4 state; Host subsequently sends HCI Reset |
| `AT+HCIEND` | Stop the Controller and release transport resources; repeated close succeeds |
| `AT+HCIAVAILABLE?` | Return the current visible byte window; return zero once after each complete H4 packet so the Host can finish its poll without overwriting shared command-result storage |
| `AT+HCIREAD` | Return `+HCIREAD: n\|` followed by exactly n raw H4 bytes and the response terminator; at most 257 bytes, without crossing a complete H4 packet boundary |
| `AT+HCIWRITE=n` then n raw bytes | Return bare OK only after full Controller submission; payload can contain NUL and CRLF; a payload gap over one second errors and closes the Controller; begin a fresh session before reuse |
| `AT+HCIWAIT=ms` | Wait for data or timeout; timeout without data is a successful wait, not an error |

Illegal forms and read/wait without an active session return a command error.
HCIWRITE consumes its declared payload before rejecting an inactive session,
preserving command framing. HCIBUFFERALLOC is not an implemented command.

ArduinoBLE 2.1.0 transport `end()` does not send HCIEND; explicitly release the
Controller through the modem. The original timed wait command is malformed;
ordinary polling is the accepted path. The public-address BlueZ bonded-reconnect
fix is an explicit [RA library patch](../compatibility/ArduinoBLE-2.1.0/README.md),
not a BL firmware or Core patch. RPA and power-persistent keys remain unaccepted.

## Files and Preferences

Files and typed Preferences share the default 416 KiB LittleFS region. Mount failure
does not automatically format. See the README for the exact file-command list.
File writes are limited to 255 bytes per frame and reads to 1023 bytes. Payloads
must be transferred as raw bytes with the declared length, not text-escaped data.

| Preferences request | Contract |
| --- | --- |
| `AT+PREFBEGIN=name,readOnly,partition` | End the old session, open the namespace, return 1/0; preserve the trailing comma for an empty partition |
| `AT+PREFEND` | Close and return 0 |
| `AT+PREFCLEAR`, `AT+PREFREMOVE=key` | Affect only the open namespace; return 1/0 |
| `AT+PREFPUT=key,type,value` | Return committed bytes; types 0–7 use decimal integers, types 8/9 carry a following raw string/BLOB length |
| `AT+PREFGET=key,type,default` | Return a decimal integer or length-framed string; missing/wrong type uses the supplied default |
| `AT+PREFGET=key,9` | Return a length-framed BLOB; missing key has length zero |
| `AT+PREFTYPE=key` | Return type 0–9, or 10 if absent |
| `AT+PREFLEN=key` | Return BLOB length, or zero for missing/non-BLOB |
| `AT+PREFSTAT` | Return logical free slots in this namespace: 64 minus key count, not physical free flash |

String/BLOB responses use `+PREFGET: n|<n raw bytes>\r\nOK\r\n`.
Command errors use `+PREFx:ERROR\r\n`; real read/corruption errors are not hidden
as missing-key defaults. Namespace/key names are 1–15 bytes, values at most 1023 bytes.
Atomic replacement needs temporary space; a full filesystem can fail before the
logical slot count reaches zero. The format is private to the platform Preferences
library; no ESP32 NVS image compatibility is claimed.

## Application roles

PhoneMatrix is an RA4M1 ArduinoBLE GATT application. Its service and binary command
format are documented in [the example guide](../examples/README.md). They are not
AT commands or features coupled into the Core. For platform installation, support,
security limits and recovery, see the platform repository's versioned `docs/`.
