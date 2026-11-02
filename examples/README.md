# UNO R4 WiFi examples

## PhoneMatrix

`PhoneMatrix/PhoneMatrix.ino` is an RA4M1 application example for ARDUINO-57. It uses the official ArduinoBLE and Arduino_LED_Matrix libraries. The BL616CL bridge only transports HCI over AT.

Build it for the official Renesas board package:

```sh
arduino-cli compile -b arduino:renesas_uno:unor4wifi PhoneMatrix
arduino-cli upload -b arduino:renesas_uno:unor4wifi -p <ra4m1-port> PhoneMatrix
```

Status: supported for the combination tested on November 2, 2026: UNO R4 WiFi 1V0 with a BL616CL bridge, Renesas core 1.6.0, ArduinoBLE 2.1.0, and a Xiaomi 14 Pro running HyperOS 3.0.31 with nRF Connect 4.29.1. The Android major version was not independently confirmed. Use a BLE utility such as nRF Connect or LightBlue:

1. Scan for `BL616 Matrix`.
2. Connect and open service `19B10000-E8F2-537E-4F6C-D104768A1214`.
3. Write `01`, `02`, `03`, and `00` to command characteristic `19B10001-E8F2-537E-4F6C-D104768A1214`.
4. Write `10` followed by 12 bytes for a raw bitmap: 8 rows of 12 pixels, packed row by row, most significant bit first. The first payload bit is the top-left pixel; the last bit is the bottom-right pixel. Twelve `FF` bytes turn all pixels on.
5. Observe status notifications on `19B10002-E8F2-537E-4F6C-D104768A1214`.
6. Repeat writes, disconnect/reconnect, and send invalid lengths/opcodes.

Real phone acceptance covered discovery and connection, all presets and clear, status notifications, raw all-on and opposite-corner frames, invalid input preserving the frame, macro loops, and three reconnections. The board log contains 249 preset/clear acknowledgements, including a contiguous run of 40 heart/smile/checker/clear cycles. The phone macro itself was not collected. Only the final reconnected session has an independent heart acknowledgement in the serial log; all three reconnections were confirmed by the user.

For raw pixel ordering, write `10 80 00 00 00 00 00 00 00 00 00 00 01`: only the top-left and bottom-right pixels should light up. A short `10` frame or unsupported `7F` command returns `ERR:COMMAND` without changing the display.

The phone test is the acceptance test. A source build or host syntax check does not count as BLE hardware acceptance. Other phones, pairing/encrypted reconnection, and Arduino Cloud are outside this example's tested scope.
