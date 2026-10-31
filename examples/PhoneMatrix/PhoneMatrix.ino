/*
  PhoneMatrix

  Expose the UNO R4 WiFi 12x8 LED matrix as a small BLE peripheral.
  A phone writes commands to the command characteristic. The RA4M1
  applies the frame with the official Arduino_LED_Matrix library.

  The BL616CL bridge supplies HCI over AT. It does not own the matrix.
*/

#include <ArduinoBLE.h>
#include <string.h>
#include "Arduino_LED_Matrix.h"

namespace {

constexpr char kServiceUuid[] = "19B10000-E8F2-537E-4F6C-D104768A1214";
constexpr char kCommandUuid[] = "19B10001-E8F2-537E-4F6C-D104768A1214";
constexpr char kStatusUuid[] = "19B10002-E8F2-537E-4F6C-D104768A1214";
constexpr size_t kBitmapBytes = 12;
constexpr size_t kMaxCommandBytes = 1 + kBitmapBytes;

BLEService matrixService(kServiceUuid);
BLECharacteristic commandCharacteristic(kCommandUuid, BLEWrite, kMaxCommandBytes);
BLEStringCharacteristic statusCharacteristic(kStatusUuid, BLERead | BLENotify, 16);
ArduinoLEDMatrix matrix;
uint8_t bitmap[kBitmapBytes];

const char* const kHeart[12] = {
    "01100110", "11111111", "11111111", "11111111",
    "01111110", "00111100", "00011000", "00000000",
    "00000000", "00000000", "00000000", "00000000"};

const char* const kSmile[12] = {
    "00000000", "01100110", "01100110", "00000000",
    "00000000", "10000001", "01000010", "00111100",
    "00000000", "00000000", "00000000", "00000000"};

const char* const kChecker[12] = {
    "10101010", "01010101", "10101010", "01010101",
    "10101010", "01010101", "10101010", "01010101",
    "10101010", "01010101", "10101010", "01010101"};

void setStatus(const char* value) {
  statusCharacteristic.writeValue(value);
  Serial.println(value);
}

void loadRows(const char* const rows[12]) {
  for (size_t row = 0; row < 12; ++row) {
    uint8_t value = 0;
    for (size_t column = 0; column < 8; ++column) {
      value <<= 1;
      if (rows[row][column] == '1') {
        value |= 1;
      }
    }
    bitmap[row] = value;
  }
  matrix.loadPixels(bitmap, sizeof(bitmap));
}

void clearMatrix() {
  memset(bitmap, 0, sizeof(bitmap));
  matrix.clear();
}

bool applyCommand(const uint8_t* command, size_t length, const char*& result) {
  if (length == 1 && command[0] == 0x00) {
    clearMatrix();
    result = "OK:CLEAR";
    return true;
  }
  if (length == 1 && command[0] == 0x01) {
    loadRows(kHeart);
    result = "OK:HEART";
    return true;
  }
  if (length == 1 && command[0] == 0x02) {
    loadRows(kSmile);
    result = "OK:SMILE";
    return true;
  }
  if (length == 1 && command[0] == 0x03) {
    loadRows(kChecker);
    result = "OK:CHECKER";
    return true;
  }
  if (length == kMaxCommandBytes && command[0] == 0x10) {
    memcpy(bitmap, command + 1, kBitmapBytes);
    matrix.loadPixels(bitmap, sizeof(bitmap));
    result = "OK:RAW";
    return true;
  }
  result = length == 0 ? "ERR:LENGTH" : "ERR:COMMAND";
  return false;
}

void handleCommand() {
  uint8_t command[kMaxCommandBytes] = {};
  const int length = commandCharacteristic.readValue(command, sizeof(command));
  const char* result = nullptr;
  applyCommand(command, length > 0 ? static_cast<size_t>(length) : 0, result);
  setStatus(result);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  matrix.begin();
  clearMatrix();

  if (!BLE.begin()) {
    setStatus("ERR:BLE");
    while (true) {
      delay(1000);
    }
  }

  BLE.setLocalName("BL616 Matrix");
  BLE.setAdvertisedService(matrixService);
  matrixService.addCharacteristic(commandCharacteristic);
  matrixService.addCharacteristic(statusCharacteristic);
  BLE.addService(matrixService);
  statusCharacteristic.writeValue("READY");
  BLE.advertise();
  Serial.println("PhoneMatrix ready");
}

void loop() {
  BLEDevice central = BLE.central();
  if (!central) {
    return;
  }

  setStatus("CONNECTED");
  while (central.connected()) {
    BLE.poll();
    if (commandCharacteristic.written()) {
      handleCommand();
    }
  }
  setStatus("DISCONNECTED");
}
