/*
 * 主机测试用的 wifi_port（接口见平台 libraries/WiFi/src/wifi_port.h）：一个虚拟 AP，
 * 网络名与密码取自环境变量 AT_HOST_SSID、AT_HOST_PASS；连接 300 ms 后出结果，地址都在本机回环上。
 * 失败时的状态码与实板一致：密码错误 2，找不到 SSID 12。
 */
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fake_ports.h"
#include "wifi_port.h"

#define CONNECT_DELAY_MS 300

bool fake_wifi_up;

static enum wifi_port_state state = WIFI_PORT_IDLE;
static enum wifi_port_state outcome;
static bool connecting;
static uint64_t ready_at;
static uint16_t last_status;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void settle(void)
{
    if (connecting && now_ms() >= ready_at) {
        connecting = false;
        state = outcome;
    }
}

static const char *env(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v != NULL ? v : fallback;
}

int wifi_port_init(void)
{
    fake_wifi_up = true;
    return 0;
}

int wifi_port_connect(const char *ssid, const char *pass)
{
    if (strcmp(ssid, env("AT_HOST_SSID", "testnet")) != 0) {
        outcome = WIFI_PORT_NO_SSID;
        last_status = 12;
    } else if (strcmp(pass != NULL ? pass : "", env("AT_HOST_PASS", "")) != 0) {
        outcome = WIFI_PORT_FAILED;
        last_status = 2;
    } else {
        outcome = WIFI_PORT_CONNECTED;
        last_status = 0;
    }
    state = WIFI_PORT_IDLE;
    connecting = true;
    ready_at = now_ms() + CONNECT_DELAY_MS;
    return 0;
}

void wifi_port_disconnect(uint32_t timeout_ms)
{
    (void)timeout_ms;
    connecting = false;
    state = WIFI_PORT_DISCONNECTED;
}

enum wifi_port_state wifi_port_state(void)
{
    settle();
    return state;
}

bool wifi_port_connecting(void)
{
    settle();
    return connecting;
}

void wifi_port_last_error(uint16_t *status, uint16_t *reason)
{
    *status = last_status;
    *reason = 0;
}

void wifi_port_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
    bool up = wifi_port_state() == WIFI_PORT_CONNECTED;
    *ip = up ? htonl(FAKE_IP) : 0;
    *mask = up ? htonl(FAKE_MASK) : 0;
    *gw = up ? htonl(FAKE_GW) : 0;
}

uint32_t wifi_port_dns(int index)
{
    return wifi_port_state() == WIFI_PORT_CONNECTED && index == 0 ? htonl(FAKE_DNS) : 0;
}

int32_t wifi_port_rssi(void)
{
    return wifi_port_state() == WIFI_PORT_CONNECTED ? FAKE_RSSI : 0;
}

int wifi_port_mac(uint8_t mac[6])
{
    static const uint8_t m[6] = FAKE_MAC;
    memcpy(mac, m, 6);
    return 0;
}

int wifi_port_bssid(uint8_t bssid[6])
{
    static const uint8_t b[6] = FAKE_BSSID;
    if (wifi_port_state() != WIFI_PORT_CONNECTED) {
        memset(bssid, 0, 6);
        return -1;
    }
    memcpy(bssid, b, 6);
    return 0;
}
