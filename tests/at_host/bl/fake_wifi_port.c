/*
 * 主机测试用的 wifi_port（接口见平台 libraries/WiFi/src/wifi_port.h）：一个虚拟 AP，
 * 网络名与密码取自环境变量 AT_HOST_SSID、AT_HOST_PASS；连接 300 ms 后出结果，地址都在本机回环上。
 * 失败时的状态码与实板一致：密码错误 2，找不到 SSID 12。
 * 扫描：第一次给出 scan_table 中按 RSSI 排好的前 max 个，第二次 0 个，之后都失败；run.py 按同一张表核对。
 * 地址：DHCP 时报 FAKE_*；静态地址与 setDNS 的规则同平台 wifi_port.c，只改报出的地址（socket 仍在回环上）。
 * AP：地址规则同平台（未配置时 192.168.4.1/24）；没有设备会连上，状态只有 AP_LISTENING；SSID 为 "ap-fail" 时模拟底层启动失败。
 */
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
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
static uint32_t cfg_ip, cfg_mask, cfg_gw, cfg_dns[2]; /* 网络字节序；cfg_ip 为 0 时用 DHCP */
static uint32_t dns_now[2];
static bool ap_up, ap_failed;
static uint32_t ap_ip, ap_mask, ap_gw, ap_cfg_ip, ap_cfg_mask, ap_cfg_gw;
static bool auto_reconnect = true, ipv6;

static void use_dhcp_dns(void)
{
    dns_now[0] = htonl(FAKE_DNS);
    dns_now[1] = 0;
}

static void apply_static_dns(void)
{
    for (int i = 0; i < 2; i++) {
        if (cfg_dns[i] != 0) {
            dns_now[i] = cfg_dns[i];
        }
    }
}

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
        if (state == WIFI_PORT_CONNECTED) {
            if (cfg_ip != 0) {
                apply_static_dns();
            } else {
                use_dhcp_dns();
            }
        }
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
    if (ap_up) {
        return WIFI_PORT_AP_LISTENING;
    }
    return ap_failed ? WIFI_PORT_AP_FAILED : state;
}

/* Only interface/configuration plumbing is simulated here. No radio, ND,
   reconnection or association-limit claim can be established by this fake. */
enum wifi_port_state wifi_port_sta_state(void) { settle(); return state; }
bool wifi_port_ap_running(void) { return ap_up; }
int wifi_port_set_auto_reconnect(bool enable) { auto_reconnect = enable; return 0; }
bool wifi_port_get_auto_reconnect(void) { return auto_reconnect; }
int wifi_port_enable_ipv6(void) { ipv6 = true; return 0; }
void wifi_port_ipv6(char *address, uint32_t size) { snprintf(address, size, "%s", ipv6 && wifi_port_sta_state() == WIFI_PORT_CONNECTED ? "fe80::1" : "::"); }
void wifi_port_ap_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw) {
    *ip = ap_up ? ap_ip : 0; *mask = ap_up ? ap_mask : 0; *gw = ap_up ? ap_gw : 0;
}
int wifi_port_ap_config(uint32_t ip, uint32_t mask, uint32_t gw) {
    if (ip && (mask != htonl(0xffffff00u) || (ntohl(ip) & 255) == 0 || (ntohl(ip) & 255) == 255)) return -1;
    ap_cfg_ip = ip; ap_cfg_mask = mask; ap_cfg_gw = gw;
    if (ap_up) { ap_ip = ip; ap_mask = mask; ap_gw = gw; }
    return 0;
}
void wifi_port_sta_ip(uint32_t *ip, uint32_t *mask, uint32_t *gw) {
    bool up = wifi_port_sta_state() == WIFI_PORT_CONNECTED;
    *ip = !up ? 0 : cfg_ip ? cfg_ip : htonl(FAKE_IP);
    *mask = !up ? 0 : cfg_ip ? cfg_mask : htonl(FAKE_MASK);
    *gw = !up ? 0 : cfg_ip ? cfg_gw : htonl(FAKE_GW);
}

bool wifi_port_net_up(void)
{
    enum wifi_port_state s = wifi_port_state();
    return s == WIFI_PORT_CONNECTED || s == WIFI_PORT_AP_LISTENING;
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
    if (ap_up) {
        *ip = *gw = ap_ip;
        *mask = ap_mask;
        return;
    }
    bool up = wifi_port_sta_state() == WIFI_PORT_CONNECTED;
    *ip = !up ? 0 : cfg_ip != 0 ? cfg_ip : htonl(FAKE_IP);
    *mask = !up ? 0 : cfg_ip != 0 ? cfg_mask : htonl(FAKE_MASK);
    *gw = !up ? 0 : cfg_ip != 0 ? cfg_gw : htonl(FAKE_GW);
}

void wifi_port_ip_config(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2)
{
    cfg_ip = ip;
    cfg_mask = mask;
    cfg_gw = gw;
    cfg_dns[0] = ip != 0 ? dns1 : 0;
    cfg_dns[1] = ip != 0 ? dns2 : 0;
    if (wifi_port_sta_state() == WIFI_PORT_CONNECTED) {
        if (ip != 0) {
            apply_static_dns();
        } else {
            use_dhcp_dns();
        }
    }
}

void wifi_port_set_dns(uint32_t dns1, uint32_t dns2)
{
    bool up = wifi_port_sta_state() == WIFI_PORT_CONNECTED;
    if (cfg_ip == 0) {
        if (up) {
            uint32_t ip, mask, gw;
            wifi_port_ip(&ip, &mask, &gw);
            wifi_port_ip_config(ip, mask, gw, dns1, dns2);
        }
        return;
    }
    if (dns1 != 0) {
        cfg_dns[0] = dns1;
    }
    if (dns2 != 0) {
        cfg_dns[1] = dns2;
    }
    if (up) {
        apply_static_dns();
    }
}

uint32_t wifi_port_dns(int index)
{
    return wifi_port_sta_state() == WIFI_PORT_CONNECTED && index >= 0 && index < 2 ? dns_now[index] : 0;
}

int32_t wifi_port_rssi(void)
{
    return wifi_port_sta_state() == WIFI_PORT_CONNECTED ? FAKE_RSSI : 0;
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
    if (wifi_port_sta_state() != WIFI_PORT_CONNECTED) {
        memset(bssid, 0, 6);
        return -1;
    }
    memcpy(bssid, b, 6);
    return 0;
}

static const struct wifi_port_ap scan_table[] = {
    { "testnet", FAKE_BSSID, FAKE_RSSI, 6, WIFI_PORT_ENC_WPA2 },
    { "open net", { 0x02, 0, 0, 0, 1, 2 }, -50, 1, WIFI_PORT_ENC_NONE },
    { "abcdefghijklmnopqrstuvwxyz012345", { 0x02, 0, 0, 0, 1, 3 }, -55, 11, WIFI_PORT_ENC_WPA3 },
    { "wep", { 0x02, 0, 0, 0, 1, 4 }, -60, 3, WIFI_PORT_ENC_WEP },
    { "wpa", { 0x02, 0, 0, 0, 1, 5 }, -61, 4, WIFI_PORT_ENC_WPA },
    { "eap", { 0x02, 0, 0, 0, 1, 6 }, -62, 5, WIFI_PORT_ENC_WPA2_ENTERPRISE },
    { "odd", { 0x02, 0, 0, 0, 1, 7 }, -63, 13, WIFI_PORT_ENC_UNKNOWN },
    { "n8", { 0x02, 0, 0, 0, 1, 8 }, -70, 2, WIFI_PORT_ENC_WPA2 },
    { "n9", { 0x02, 0, 0, 0, 1, 9 }, -75, 7, WIFI_PORT_ENC_WPA2 },
    { "n10", { 0x02, 0, 0, 0, 1, 10 }, -80, 9, WIFI_PORT_ENC_WPA2 },
    { "n11", { 0x02, 0, 0, 0, 1, 11 }, -90, 10, WIFI_PORT_ENC_WPA2 },
};

int wifi_port_scan(struct wifi_port_ap *aps, int max, uint32_t timeout_ms)
{
    static int calls;
    (void)timeout_ms;
    switch (calls++) {
        case 0: {
            int n = 0;
            for (; n < max && n < (int)(sizeof(scan_table) / sizeof(scan_table[0])); n++) {
                aps[n] = scan_table[n];
            }
            return n;
        }
        case 1: return 0;
        default: return -1;
    }
}

int wifi_port_ap_start(const char *ssid, const char *pass, uint8_t channel, bool hidden, uint8_t max_connections, uint32_t timeout_ms)
{
    (void)hidden;
    (void)max_connections;
    (void)pass;
    (void)channel;
    (void)timeout_ms;
    ap_up = false;
    ap_failed = false;
    if (strcmp(ssid, "ap-fail") == 0) {
        ap_failed = true;
        return -1;
    }
    ap_ip = ap_cfg_ip != 0 ? ap_cfg_ip : htonl(0xC0A80401u);
    ap_mask = ap_cfg_ip != 0 ? ap_cfg_mask : htonl(0xFFFFFF00u);
    ap_gw = ap_cfg_ip != 0 ? ap_cfg_gw : ap_ip;
    ap_up = true;
    return 0;
}

void wifi_port_ap_stop(uint32_t timeout_ms)
{
    (void)timeout_ms;
    ap_up = false;
    ap_failed = false;
}
