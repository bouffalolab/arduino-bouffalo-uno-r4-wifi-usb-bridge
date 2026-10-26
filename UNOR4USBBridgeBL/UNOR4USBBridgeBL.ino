// BL 版 UNO R4 WiFi USB bridge（BL616CL 替代 ESP32-S3）。
// 当前功能：
// - 电脑看到的 USB 串口（Serial）与 RA4M1 用户串口（Serial1，uart0 GPIO34/35 经 U4 接 SCI9）双向透传；
// - RA4M1 WiFiS3 的 AT 命令（Serial2，uart1 GPIO6/7 经 U4 接 SCI1）：联网与 TCP 客户端，见 at_server.h。
// 电脑设置的波特率按官方 bridge 语义处理：
// - 1200：MD 高，RESET 双击，RA4M1 进 Arduino bootloader（IDE/arduino-cli 经 bossac 上传）；
// - 2400：MD 低，RESET 一次，RA4M1 进 ROM 下载模式；MD 保持低，直到下一次 1200；
// - 其他：设为 Serial1 的波特率（帧格式固定 8N1）。
// 每次处理后清空 RA4M1 发来、尚未转发的数据。1200 由本固件处理，所以关闭 Core 的 1200 bps touch。
// 构建：arduino-bl 平台，FQBN bouffalolab:bl:unor4wifi_bl616cl；1V1 模组加 -DBRIDGE_MODULE_1V1。
#include "FreeRTOS.h"
#include "task.h"

#include "at_server.h"
#include "dap_bridge.h"

#define PIN_RA4M1_RESET 3 // ESP_IO4，经 U4 接 RA4M1 RESET
#if defined(BRIDGE_MODULE_1V1)
#define PIN_RA4M1_MD 10
#else
#define PIN_RA4M1_MD 14 // 1V0 模组；ESP_IO9，经 U4 接 RA4M1 MD（板上无上拉）
#endif

#define RESET_PULSE_MS 100
#define BAUD_BOOTLOADER 1200
#define BAUD_ROM 2400
#define USER_RX_BUFFER 8192 // USB 写最多阻塞 100 ms，921600 下约 9 KB
#define AT_BAUD 115200      // WiFiS3 Modem：Serial2 115200 8N1
#define AT_RX_BUFFER 8192   // CLIENTSEND 载荷在发往 socket 期间缓存在这里（同上游）
#define AT_TASK_STACK_SIZE 8192 // 字节
#define AT_TASK_PRIORITY 1      // 同 Core 的 loop 任务，两者轮流运行

static uint8_t buf[1024];

// 回调只通知，线路切换与 DAP 都在 loop 执行，串行访问 RESET/SWD。
static uint32_t user_baud;
static volatile uint32_t line_events;
// 只在 loop 中使用
static uint32_t handled_baud;
// 只在 loop 中使用
static uint32_t seen_events;
static uint32_t applied_baud;

// AT 命令在单独的任务中处理：连接、DNS 等可能等待数秒，不能停住 loop 中的透传。Wi-Fi 调用只在这个任务中
static void atTask(void *arg)
{
    (void)arg;
    for (;;) {
        at_serve(Serial2);
        delay(1);
    }
}

static void resetPulse()
{
    digitalWrite(PIN_RA4M1_RESET, LOW);
    delay(RESET_PULSE_MS);
    digitalWrite(PIN_RA4M1_RESET, HIGH);
}

// Core 在每个 SET_LINE_CODING、SET_CONTROL_LINE_STATE 之后调用；只按波特率变化动作，
// 控制线变化和重复设置同一波特率不处理（Linux 只在值变化时发 SET_LINE_CODING）。
static void onLineChange()
{
    line_events = line_events + 1;
}

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

void setup()
{
    digitalWrite(PIN_RA4M1_MD, HIGH);
    digitalWrite(PIN_RA4M1_RESET, HIGH);
    pinMode(PIN_RA4M1_MD, OUTPUT);
    pinMode(PIN_RA4M1_RESET, OUTPUT);
    dap_bridge_begin();

    // Core 在 setup() 之前才启动 USB，主机的线路请求在注册回调之后到达；此前是 Core 的默认值
    handled_baud = Serial.baud();
    user_baud = applied_baud = handled_baud;
    Serial1.setRxBufferSize(USER_RX_BUFFER);
    Serial1.begin(applied_baud);
    if (Serial.onLineChange(onLineChange)) {
        Serial.setTouchReset(false); // 回调不可用时保留 touch，仍能经 USB 更新本固件
    }

    Serial2.setRxBufferSize(AT_RX_BUFFER);
    Serial2.begin(AT_BAUD);
    xTaskCreate(atTask, "at", AT_TASK_STACK_SIZE / sizeof(StackType_t), NULL, AT_TASK_PRIORITY, NULL);
}

void loop()
{
    // 改波特率在 loop 中做：Serial1 的收发也只在这里，不与 begin() 并发
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

    // 只取 Serial1 发送 FIFO 放得下的量：write() 在 FIFO 满时等待，921600 下写 1 KB 约 11 ms，
    // 期间 RA4M1 发来的数据会积压到超出接收缓冲（实测双向同时 64 KB 丢约 1.4 KB）
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
