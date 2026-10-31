# UNO R4 WiFi examples

## PhoneMatrix

`PhoneMatrix/PhoneMatrix.ino` is an RA4M1 application example for ARDUINO-57. It uses the official ArduinoBLE and Arduino_LED_Matrix libraries. The BL616CL bridge only transports HCI over AT.

Build it for the official Renesas board package:

```sh
arduino-cli compile -b arduino:renesas_uno:unor4wifi PhoneMatrix
arduino-cli upload -b arduino:renesas_uno:unor4wifi -p <ra4m1-port> PhoneMatrix
```

The current task environment has no phone, so the commands above and the BLE interaction are pending real-device execution. When a phone is available, use a BLE utility such as nRF Connect or LightBlue:

1. Scan for `BL616 Matrix`.
2. Connect and open service `19B10000-E8F2-537E-4F6C-D104768A1214`.
3. Write `01`, `02`, `03`, and `00` to command characteristic `19B10001-E8F2-537E-4F6C-D104768A1214`.
4. Write `10` followed by 12 bytes for a raw 12-row bitmap.
5. Observe status notifications on `19B10002-E8F2-537E-4F6C-D104768A1214`.
6. Repeat writes, disconnect/reconnect, and send invalid lengths/opcodes.

The phone test is the acceptance test. A source build or host syntax check does not count as BLE hardware acceptance.
