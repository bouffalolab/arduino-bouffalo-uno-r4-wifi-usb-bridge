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
constexpr size_t kRows = 8;
constexpr size_t kColumns = 12;
constexpr size_t kPixelCount = kRows * kColumns;
constexpr size_t kBitmapBytes = kPixelCount / 8;
constexpr size_t kMaxCommandBytes = 1 + kBitmapBytes;

BLEService matrixService(kServiceUuid);
BLECharacteristic commandCharacteristic(kCommandUuid, BLEWrite, kMaxCommandBytes);
BLEStringCharacteristic statusCharacteristic(kStatusUuid, BLERead | BLENotify, 16);
ArduinoLEDMatrix matrix;
uint8_t bitmap[kPixelCount];

const char* const kHeart[kRows] = {
    "000110011000", "001111111100", "001111111100", "001111111100",
    "000111111000", "000011110000", "000001100000", "000000000000"};

const char* const kSmile[kRows] = {
    "000000000000", "000110011000", "000110011000", "000000000000",
    "000000000000", "001000000100", "000100001000", "000011110000"};

const char* const kChecker[kRows] = {
    "101010101010", "010101010101", "101010101010", "010101010101",
    "101010101010", "010101010101", "101010101010", "010101010101"};

void setStatus(const char* value) {
  statusCharacteristic.writeValue(value);
  Serial.println(value);
}

/** @brief Render eight rows of twelve pixels in the official matrix order. */
void loadRows(const char* const rows[kRows]) {
  for (size_t row = 0; row < kRows; ++row) {
    for (size_t column = 0; column < kColumns; ++column) {
      bitmap[row * kColumns + column] = rows[row][column] == '1' ? 1 : 0;
    }
  }
  matrix.loadPixels(bitmap, kPixelCount);
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
    // Pack all 96 pixels row by row, most significant bit first.
    for (size_t pixel = 0; pixel < kPixelCount; ++pixel) {
      bitmap[pixel] = (command[1 + pixel / 8] >> (7 - pixel % 8)) & 1;
    }
    matrix.loadPixels(bitmap, kPixelCount);
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
