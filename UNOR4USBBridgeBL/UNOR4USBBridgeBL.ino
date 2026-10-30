// UNO R4 WiFi USB bridge with BL616CL replacing ESP32-S3.
// Implemented functions:
// - Bidirectional USB Serial to RA4M1 Serial1 passthrough; uart0 GPIO34/35 connects through U4 to SCI9.
// - WiFiS3 AT over Serial2 (uart1 GPIO6/7 through U4 to SCI1); see at_server.h for network and other commands.
// Handle host baud changes using the official bridge semantics:
// - 1200: MD high and a double RESET pulse enter the RA4M1 Arduino bootloader for bossac upload.
// - 2400: MD low and one RESET pulse enter RA4M1 ROM; keep MD low until a later 1200 request.
// - Other rates configure Serial1 with fixed 8N1 framing.
// After each switch, discard RA4M1 bytes not yet forwarded. Disable Core touch reset because this bridge handles 1200 bps itself.
// Build with bouffalolab:bl:unor4wifi_bl616cl; -DBRIDGE_MODULE_1V1 selects the alternate module wiring, which remains unvalidated.
#include "FreeRTOS.h"
#include "task.h"

#include "at_server.h"
#include "dap_bridge.h"

#define PIN_RA4M1_RESET 3 // ESP_IO4, connected through U4 to RA4M1 RESET.
#if defined(BRIDGE_MODULE_1V1)
#define PIN_RA4M1_MD 10
#else
#define PIN_RA4M1_MD 14 // 1V0 module; ESP_IO9 through U4 to RA4M1 MD, without a board pull-up.
#endif

#define RESET_PULSE_MS 100
#define BAUD_BOOTLOADER 1200
#define BAUD_ROM 2400
#define USER_RX_BUFFER 8192 // USB writes can stall for 100 ms, about 9 KB at 921600 baud.
#define AT_BAUD 115200      // WiFiS3 Modem: Serial2 at 115200 8N1.
#define AT_RX_BUFFER 8192   // Buffer CLIENTSEND payloads while forwarding to the socket, matching upstream.
#define AT_TASK_STACK_SIZE 8192 // Bytes.
#define AT_TASK_PRIORITY 1      // Same priority as the Core loop task, allowing both tasks to run.

static uint8_t buf[1024];

// The callback only notifies; loop serializes line switching and DAP access to RESET/SWD.
static uint32_t user_baud;
static volatile uint32_t line_events;
// Used only by loop.
static uint32_t handled_baud;
// Used only by loop.
static uint32_t seen_events;
static uint32_t applied_baud;

/**
 * @brief Handle AT in a separate task because connection/DNS calls can wait seconds and must
 * not stop passthrough. All Wi-Fi calls run in that task.
 */
static void atTask(void *arg)
{
    (void)arg;
    for (;;) {
        at_serve(Serial2);
        delay(1);
    }
}

/**
 * @brief Pulse the RA4M1 reset line for the bridge's serial-upload sequence.
 */
static void resetPulse()
{
    digitalWrite(PIN_RA4M1_RESET, LOW);
    delay(RESET_PULSE_MS);
    digitalWrite(PIN_RA4M1_RESET, HIGH);
}

/**
 * @brief Core invokes this callback after each line/control request. Act only on baud changes.
 * Ignore control-only changes and repeated rates; Linux emits SET_LINE_CODING only when its
 * value changes.
 */
static void onLineChange()
{
    line_events = line_events + 1;
}

/**
 * @brief Apply host CDC line settings to the RA UART and bridge reset policy.
 */
static void applyLineChange()
{
    uint32_t baud = Serial.baud();
    if (baud == handled_baud) {
        return;
    }
    handled_baud = baud;
    if (baud == BAUD_BOOTLOADER) {
        dap_bridge_disconnect();
        digitalWrite(PIN_RA4M1_MD, HIGH);
        resetPulse();
        delay(RESET_PULSE_MS);
        resetPulse();
    } else if (baud == BAUD_ROM) {
        dap_bridge_disconnect();
        digitalWrite(PIN_RA4M1_MD, LOW);
        delay(RESET_PULSE_MS);
        resetPulse();
    } else {
        user_baud = baud;
    }
}

/**
 * @brief Initialize the bridge peripherals and protocol services.
 */
void setup()
{
    digitalWrite(PIN_RA4M1_MD, HIGH);
    digitalWrite(PIN_RA4M1_RESET, HIGH);
    pinMode(PIN_RA4M1_MD, OUTPUT);
    pinMode(PIN_RA4M1_RESET, OUTPUT);
    dap_bridge_begin();

    // Core starts USB just before setup, so host line requests arrive after callback registration; earlier values are Core defaults.
    handled_baud = Serial.baud();
    user_baud = applied_baud = handled_baud;
    Serial1.setRxBufferSize(USER_RX_BUFFER);
    Serial1.begin(applied_baud);
    if (Serial.onLineChange(onLineChange)) {
        Serial.setTouchReset(false); // Keep touch enabled when callback registration fails, preserving USB firmware recovery.
    }

    Serial2.setRxBufferSize(AT_RX_BUFFER);
    Serial2.begin(AT_BAUD);
    xTaskCreate(atTask, "at", AT_TASK_STACK_SIZE / sizeof(StackType_t), NULL, AT_TASK_PRIORITY, NULL);
}

/**
 * @brief Service USB, RA UART and AT traffic in the bridge task.
 */
void loop()
{
    // Reconfigure Serial1 in loop, which also owns its reads/writes, avoiding concurrent begin().
    if (line_events != seen_events) {
        seen_events = line_events;
        applyLineChange();
        uint32_t baud = user_baud;
        if (baud != applied_baud) {
            Serial1.begin(baud);
            applied_baud = baud;
        }
        while (Serial1.read() >= 0) {
        }
    }

    dap_bridge_poll();

    // Read only enough USB bytes to fit the Serial1 TX FIFO. Blocking a 1 KiB write at 921600 baud takes about 11 ms.
    // During that wait RA4M1 RX can overflow; simultaneous 64 KiB traffic lost about 1.4 KiB in the original measurement.
    int n = min(Serial.available(), Serial1.availableForWrite());
    if (n > 0) {
        n = Serial.readBytes(buf, n);
        Serial1.write(buf, n);
    }
    n = Serial1.available();
    if (n > 0) {
        n = Serial1.readBytes(buf, min((size_t)n, sizeof(buf)));
        Serial.write(buf, n);
    }
}
