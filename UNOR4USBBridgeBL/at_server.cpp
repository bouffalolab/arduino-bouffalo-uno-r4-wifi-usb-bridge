/* WiFiS3 AT server: line parsing, dispatch and command handlers; see at_server.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "WiFi.h"
#include "at_server.h"
#include <BLEController.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <BLUpdate.h>
extern "C" {
#include "bl616cl_sys.h"
}

#include "bridge_version.h"
#define FW_VERSION         BRIDGE_VERSION_STRING /* Firmware version returned by AT+FWVERSION. */
#define LINE_MAX           256     /* WiFiS3 requests are at most 128 bytes. */
#define MAX_SOCKETS        8       /* BEGINCLIENT allocates identifiers 0-7, matching upstream. */
#define MAX_UDP            4       /* UDPBEGIN allocates 0-3 independently of TCP, matching upstream. */
#define MAX_SERVERS        4       /* BEGINSERVER allocates 0-3, matching upstream. */
#define SERVER_SOCK_BASE   1000    /* Accepted clients use 1000-1007 and CLIENT* commands, matching upstream. */
#define MAX_SSL            8       /* SSLBEGINCLIENT allocates 2000-2007, matching upstream. */
#define SSL_SOCK_BASE      2000
#define CA_MAX             16384   /* Maximum SETCAROOT PEM length. */
#define CONNECT_TIMEOUT_MS 3000    /* Official WiFiS3 CLIENTCONNECT default. */
#define PAYLOAD_TIMEOUT_MS 1000    /* Maximum gap between CLIENTSEND payload chunks. */
#define RECV_MAX           1023    /* WiFiS3 requests at most 1023 bytes per CLIENTRECEIVE. */
#define TLS_SLOT_NAMESPACE  "tls-sse"

typedef void (*handler_t)(Stream &io, const char *name, char op, char *args);

static char line[LINE_MAX + 1];
static size_t line_len;
static bool line_overflow;
/*
 * Shared chunks/replies for CLIENTSEND, UDPWRITE, CLIENTRECEIVE, UDPREAD and WIFISCAN; UDPREAD
 * returns at most one datagram.
 */
static uint8_t data[WIFI_UDP_BUFFER_SIZE];

static bool wifi_started; /*
 * BEGINSTA or BEGINSOFTAP has initialized networking, making disconnect/AP-stop operations
 * meaningful.
 */
static WiFiClient clients[MAX_SOCKETS];
static bool allocated[MAX_SOCKETS];
static WiFiUDP udps[MAX_UDP];
static bool udp_allocated[MAX_UDP];
static WiFiServer servers[MAX_SERVERS];
static bool server_allocated[MAX_SERVERS];
static WiFiSSLClient ssls[MAX_SSL];
static bool ssl_allocated[MAX_SSL];
static char *ssl_ca[MAX_SSL]; /* NUL-terminated SETCAROOT PEM; an empty value selects the root bundle. */
static char *ssl_cert[MAX_SSL];
static char *ssl_key[MAX_SSL];
static bool hci_started;
static Preferences preferences;
static size_t hci_visible;
static bool hci_batch_done;
static uint8_t hci_header[5];
static size_t hci_header_used, hci_header_size, hci_payload_left;
/*
 * Accepted clients use SERVER_SOCK_BASE+i; server=-1 marks a free slot. SERVERAVAILABLE retains
 * unaccepted clients for SERVERWRITE, while SERVERACCEPT transfers accepted ones to the host.
 */
static struct {
    WiFiClient client;
    int server = -1;
    bool accepted;
} conns[MAX_SOCKETS];

/**
 * @brief Write a successful named AT response followed by the OK terminator.
 */
static void reply(Stream &io, const char *name, const char *payload)
{
    io.print(name);
    io.print(": ");
    io.print(payload);
    io.print("\r\nOK\r\n");
}

/**
 * @brief Format a signed integer as a successful named AT response.
 */
static void reply_int(Stream &io, const char *name, long value)
{
    char s[12];
    snprintf(s, sizeof(s), "%ld", value);
    reply(io, name, s);
}

/**
 * @brief Format a six-byte MAC address as a successful named AT response.
 */
static void reply_mac(Stream &io, const char *name, const uint8_t *mac)
{
    char s[18];
    snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    reply(io, name, s);
}

/**
 * @brief Always reply "+<command>:ERROR" on failure. WiFiS3 incorrectly defines RESULT_ERROR
 * as "OK", so bare "ERROR" waits for timeout. Omit the space after ':' because
 * avoid_trim_results for WIFISCAN/CLIENTRECEIVE would not recognize " ERROR".
 */
static void reply_error(Stream &io, const char *name)
{
    io.print(name);
    io.print(":ERROR\r\n");
}

/**
 * @brief Reply "+<command>: <n>|", exactly n raw bytes, then "OK" without an intervening
 * newline. Length- delimited payloads may contain arbitrary bytes.
 */
static void reply_data(Stream &io, const char *name, size_t n)
{
    io.print(name);
    io.print(": ");
    io.print(n);
    io.print('|');
    io.write(data, n);
    io.print("OK\r\n");
}

/**
 * @brief Split args in place into at most max comma-separated fields; retain remaining commas
 * in the last field. Return the field count.
 */
static int split(char *args, char **argv, int max)
{
    int n = 0;
    char *p = args;
    while (n < max) {
        argv[n++] = p;
        if (n == max || (p = strchr(p, ',')) == NULL) {
            break;
        }
        *p++ = '\0';
    }
    return n;
}

/**
 * @brief Parse a complete signed decimal argument without accepting trailing characters.
 */
static bool parse_long(const char *s, long *value)
{
    char *end;
    *value = strtol(s, &end, 10);
    return end != s && *end == '\0';
}

/**
 * @brief Return the slot for SSLBEGINCLIENT identifiers 2000-2007, or -1.
 */
static int ssl_slot(const char *s)
{
    long v;
    long i = parse_long(s, &v) ? v - SSL_SOCK_BASE : -1;
    return i >= 0 && i < MAX_SSL && ssl_allocated[i] ? (int)i : -1;
}

/**
 * @brief Resolve an allocated client identifier or return -1. Shared handlers accept
 * SSLBEGINCLIENT 2000-2007 only for names beginning +SSL; ordinary CLIENT commands accept 0-7
 * and accepted-server clients 1000-1007.
 */
static int socket_arg(const char *name, const char *s)
{
    long v;
    if (strncmp(name, "+SSL", 4) == 0) {
        int i = ssl_slot(s);
        return i >= 0 ? SSL_SOCK_BASE + i : -1;
    }
    if (!parse_long(s, &v)) {
        return -1;
    }
    if (v >= 0 && v < MAX_SOCKETS) {
        return allocated[v] ? (int)v : -1;
    }
    long i = v - SERVER_SOCK_BASE;
    return i >= 0 && i < MAX_SOCKETS && conns[i].server >= 0 ? (int)v : -1;
}

/**
 * @brief Resolve a validated TCP, accepted-server or TLS identifier to its client object.
 */
static WiFiClient &client_of(int s)
{
    if (s >= SSL_SOCK_BASE) {
        return ssls[s - SSL_SOCK_BASE];
    }
    return s < SERVER_SOCK_BASE ? clients[s] : conns[s - SERVER_SOCK_BASE].client;
}

/**
 * @brief Stop a validated client and release its allocation and owned TLS credential buffers.
 */
static void close_socket(int s)
{
    client_of(s).stop();
    if (s < SERVER_SOCK_BASE) {
        allocated[s] = false;
    } else if (s < SSL_SOCK_BASE) {
        conns[s - SERVER_SOCK_BASE].server = -1;
    } else {
        ssl_allocated[s - SSL_SOCK_BASE] = false;
        free(ssl_ca[s - SSL_SOCK_BASE]);
        ssl_ca[s - SSL_SOCK_BASE] = nullptr;
        free(ssl_cert[s - SSL_SOCK_BASE]);
        ssl_cert[s - SSL_SOCK_BASE] = nullptr;
        free(ssl_key[s - SSL_SOCK_BASE]);
        ssl_key[s - SSL_SOCK_BASE] = nullptr;
    }
}

/**
 * @brief Return an allocated UDP slot or -1.
 */
static int udp_arg(const char *s)
{
    long v;
    return parse_long(s, &v) && v >= 0 && v < MAX_UDP && udp_allocated[v] ? (int)v : -1;
}

/**
 * @brief Close a UDP socket and release its bridge identifier.
 */
static void close_udp(int u)
{
    udps[u].stop();
    udp_allocated[u] = false;
}

/**
 * @brief Consume left raw payload bytes after the command line, streaming to sink when
 * nonnull. Drain even after sink failure to retain frame synchronization. Abandon after a
 * PAYLOAD_TIMEOUT_MS gap; return true only if all bytes were received and written.
 */
static bool read_payload(Stream &io, long left, Print *sink)
{
    bool ok = sink != nullptr;
    unsigned long last = millis();
    while (left > 0) {
        long n = io.available();
        if (n <= 0) {
            if (millis() - last > PAYLOAD_TIMEOUT_MS) {
                return false;
            }
            delay(1);
            continue;
        }
        n = io.readBytes(data, min(n, min(left, (long)sizeof(data))));
        last = millis();
        left -= n;
        if (ok && sink->write(data, n) != (size_t)n) {
            ok = false;
        }
    }
    return ok;
}

/**
 * @brief WiFiS3 sends this before its first Wi-Fi call and waits only 500 ms. Acknowledge
 * before closing sockets/disconnecting.
 */
static void cmd_softreset(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
    for (int s = 0; s < MAX_SOCKETS; s++) {
        close_socket(s);
    }
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (conns[i].server >= 0) {
            close_socket(SERVER_SOCK_BASE + i);
        }
    }
    for (int v = 0; v < MAX_SERVERS; v++) {
        servers[v].end();
        server_allocated[v] = false;
    }
    for (int i = 0; i < MAX_SSL; i++) {
        close_socket(SSL_SOCK_BASE + i);
    }
    for (int u = 0; u < MAX_UDP; u++) {
        close_udp(u);
    }
    if (wifi_started) {
        WiFi.end(); /* Disconnect STA and stop AP, matching upstream. */
    }
    WiFi.config(IPAddress(0, 0, 0, 0)); /* Restore DHCP, matching upstream. */
}

/**
 * @brief Start the external-Host BLE Controller transport and acknowledge its readiness.
 */
static void cmd_hcibegin(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    // A new RA4M1 sketch may start while the previous Host was still scanning.
    // Start a new H4 session instead of inheriting queued events and partial frames.
    BLEController.end();
    hci_started = false;
    if (!BLEController.begin()) {
        reply_error(io, name);
        return;
    }
    hci_started = true;
    hci_visible = 0;
    hci_batch_done = false;
    hci_header_used = hci_header_size = hci_payload_left = 0;
    reply(io, name, "");
}

/**
 * @brief Set the HCI read wait policy used by subsequent AT transfers.
 */
static void cmd_hciwait(Stream &io, const char *name, char op, char *args)
{
    long timeout;
    if (!hci_started || op != '=' || !parse_long(args, &timeout) || timeout < 0) {
        reply_error(io, name);
        return;
    }
    unsigned long start = millis();
    while (BLEController.available() == 0 && millis() - start < (unsigned long)timeout) {
        delay(1);
    }
    reply(io, name, "");
}

/**
 * @brief Stop the Controller transport and clear bridge HCI framing state.
 */
static void cmd_hciend(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    BLEController.end();
    hci_started = false;
    hci_visible = 0;
    hci_batch_done = false;
    hci_header_used = hci_header_size = hci_payload_left = 0;
    reply(io, name, "");
}

/**
 * @brief Report the currently visible complete HCI receive batch.
 */
static void cmd_hciavailable(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (!hci_started || op != '?') {
        reply_error(io, name);
        return;
    }
    // ArduinoBLE keeps Command Complete results in its shared receive buffer.
    // Return zero after each H4 packet so the next packet cannot overwrite
    // a result before sendCommand() returns to its caller.
    if (hci_batch_done) {
        hci_batch_done = false;
        reply_int(io, name, 0);
        return;
    }
    if (hci_visible == 0) hci_visible = BLEController.available();
    reply_int(io, name, hci_visible);
}

/**
 * @brief Return the requested bounded HCI bytes in WiFiS3 binary-response framing.
 */
static void cmd_hciread(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (!hci_started || op != '\0') {
        reply_error(io, name);
        return;
    }
    // ArduinoBLE 2.1.0 queues each reply in RingBufferN<258>, usable capacity 257.
    if (hci_visible == 0) hci_visible = BLEController.available();
    size_t n = 0, limit = min(hci_visible, (size_t)257);
    bool packet_done = false;
    while (n < limit) {
        if (hci_header_size == 0 || hci_header_used < hci_header_size) {
            int byte = BLEController.read();
            if (byte < 0) break;
            data[n++] = byte;
            if (hci_header_used == 0) {
                // Controller output used by ArduinoBLE is Event or ACL.
                hci_header_size = byte == 4 ? 3 : byte == 2 ? 5 : 1;
            }
            hci_header[hci_header_used++] = byte;
            if (hci_header_used < hci_header_size) continue;
            hci_payload_left = hci_header[0] == 4 ? hci_header[2] :
                               hci_header[0] == 2 ? hci_header[3] | (hci_header[4] << 8) : 0;
        } else {
            size_t got = BLEController.read(data + n, min(limit - n, hci_payload_left));
            if (got == 0) break;
            n += got;
            hci_payload_left -= got;
        }
        if (hci_payload_left == 0) {
            hci_header_used = hci_header_size = 0;
            packet_done = true;
            break;
        }
    }
    hci_visible = packet_done ? 0 : hci_visible - n;
    hci_batch_done = n > 0 && (packet_done || hci_visible == 0);
    reply_data(io, name, n);
}

class HciSink : public Print {
public:
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(uint8_t byte) override
    {
        return write(&byte, 1);
    }

    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(const uint8_t *buffer, size_t size) override
    {
        size_t sent = 0;
        unsigned long start = millis();
        while (sent < size && millis() - start < 5000) {
            sent += BLEController.write(buffer + sent, size - sent);
            if (sent < size) delay(1);
        }
        return sent;
    }
};

/**
 * @brief AT+HCIWRITE=<n> is followed by n raw H4 bytes; ArduinoBLE passthrough sends the
 * payload directly.
 */
static void cmd_hciwrite(Stream &io, const char *name, char op, char *args)
{
    long left;
    if (op != '=' || !parse_long(args, &left) || left <= 0) {
        reply_error(io, name);
        return;
    }
    HciSink sink;
    if (read_payload(io, left, hci_started ? &sink : nullptr)) {
        io.print("OK\r\n");
    } else {
        // A timed-out transaction may leave a partial H4 header in the Controller.
        // Require HCIBEGIN again so the next Host cannot inherit that frame.
        BLEController.end();
        hci_started = false;
        hci_visible = 0;
        hci_batch_done = false;
        hci_header_used = hci_header_size = hci_payload_left = 0;
        reply_error(io, name);
    }
}

/**
 * @brief Return the bridge firmware version string.
 */
static void cmd_fwversion(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply(io, name, FW_VERSION);
}

/**
 * @brief AT+FWVERSION_U32? encodes the three FW_VERSION components as raw bytes plus zero,
 * matching cmds_esp_generic.h; WiFiS3 consumes the first three bytes.
 */
static void cmd_fwversion_u32(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    unsigned v[3];
    if (op != '?' || sscanf(FW_VERSION, "%u.%u.%u", &v[0], &v[1], &v[2]) != 3) {
        reply_error(io, name);
        return;
    }
    const uint8_t b[4] = { (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], 0 };
    io.print(name);
    io.print(": ");
    io.write(b, sizeof(b));
    io.print("\r\nOK\r\n");
}

/**
 * @brief Mode and radio lifecycle belong to the platform WiFi library.
 */
static void cmd_gettime(Stream &io, const char *name, char op, char *args)
{
    if (op != '=' || args[0] != '\0') {
        reply_error(io, name);
        return;
    }
    char value[12];
    snprintf(value, sizeof(value), "%lu", WiFi.getTime());
    reply(io, name, value);
}

/**
 * @brief Query or update the platform hostname through AT arguments.
 */
static void cmd_hostname(Stream &io, const char *name, char op, char *args)
{
    if (op == '?') {
        const char *value = WiFi.getHostname();
        if (value != nullptr) {
            reply(io, name, value);
            return;
        }
    } else if (op == '=' && WiFi.setHostname(args)) {
        reply(io, name, "");
        return;
    }
    reply_error(io, name);
}

/**
 * @brief Query or select the STA/AP interface mode bits.
 */
static void cmd_wifimode(Stream &io, const char *name, char op, char *args)
{
    if (op == '?') {
        reply_int(io, name, WiFi.getMode());
        return;
    }
    char *argv[2];
    long mode, auto_connect;
    int n = op == '=' ? split(args, argv, 2) : 0;
    if (n < 1 || !parse_long(argv[0], &mode) || mode < 0 || mode > 3 ||
        (n == 2 && argv[1][0] != '\0' && (!parse_long(argv[1], &auto_connect) || auto_connect < 0 || auto_connect > 1))) {
        reply_error(io, name);
        return;
    }
    /* Official ESP32 setAutoConnect is deprecated and returns false.
       The optional second parameter has no effect, exactly as upstream. */
    if (!WiFi.mode((WiFiMode)mode)) {
        reply_error(io, name);
        return;
    }
    wifi_started = true;
    reply(io, name, "");
}

/**
 * @brief Query or change the unexpected-disconnection reconnect policy.
 */
static void cmd_reconnect(Stream &io, const char *name, char op, char *args)
{
    long enable;
    if (op == '?') {
        reply_int(io, name, WiFi.getAutoReconnect());
    } else if (op == '=' && parse_long(args, &enable) && enable >= 0 && enable <= 1 &&
               WiFi.setAutoReconnect(enable != 0)) {
        reply(io, name, "");
    } else {
        reply_error(io, name);
    }
}

/**
 * @brief Explicit compatibility placeholder: the pinned official implementation also ignores
 * the setter and returns false from getAutoConnect. No Flash storage.
 */
static void cmd_autoconnect(Stream &io, const char *name, char op, char *args)
{
    long enable;
    if (op == '?') {
        reply_int(io, name, 0);
    } else if (op == '=' && parse_long(args, &enable) && enable >= 0 && enable <= 1) {
        reply(io, name, "");
    } else {
        reply_error(io, name);
    }
}

/**
 * @brief Enable or query supported STA link-local IPv6 addressing.
 */
static void cmd_ipv6(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op == '?') {
        reply(io, name, WiFi.localIPv6().c_str());
    } else if (op == '\0' && WiFi.enableIpV6()) {
        reply(io, name, "");
    } else {
        reply_error(io, name);
    }
}

/**
 * @brief AT+BEGINSTA=<ssid>[,<pass>]: reply after initiating association; the host polls
 * GETSTATUS. Everything after the first comma belongs to the passphrase.
 */
static void cmd_beginsta(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    if (op != '=') {
        reply_error(io, name);
        return;
    }
    int n = split(args, argv, 2);
    WiFi.setTimeout(0);
    int status = WiFi.begin(argv[0], n > 1 ? argv[1] : nullptr);
    if (status == WL_NO_MODULE || status == WL_CONNECT_FAILED) {
        reply_error(io, name);
        return;
    }
    wifi_started = true;
    reply(io, name, "");
}

/**
 * @brief Report WL_CONNECTED (3) only after acquiring an address.
 */
static void cmd_getstatus(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, WiFi.status());
}

/**
 * @brief AT+IPSTA=<n>: 0 address, 1 gateway, 2 netmask, 3/4 DNS.
 */
static void cmd_ipsta(Stream &io, const char *name, char op, char *args)
{
    long n;
    IPAddress ip;
    if (op != '=' || !parse_long(args, &n) || n < 0 || n > 4) {
        reply_error(io, name);
        return;
    }
    switch (n) {
        case 0: ip = WiFi.stationIP(); break;
        /* WiFiS3 also asks IPSTA for AP gateway/mask in mode 2. */
        case 1: ip = WiFi.getMode() == WIFI_AP ? WiFi.gatewayIP() : WiFi.stationGatewayIP(); break;
        case 2: ip = WiFi.getMode() == WIFI_AP ? WiFi.subnetMask() : WiFi.stationSubnetMask(); break;
        default: ip = WiFi.dnsIP(n - 3); break;
    }
    reply(io, name, ip.toString().c_str());
}

/**
 * @brief AT+SETIP=<address>,<gateway>,<mask>[,<DNS1>[,<DNS2>]] selects static addressing;
 * WiFiS3 config/setDNS always send five fields. Address 0.0.0.0 restores DHCP; zero DNS leaves
 * that server unchanged.
 */
static void cmd_setip(Stream &io, const char *name, char op, char *args)
{
    char *argv[5];
    IPAddress ip[5];
    int n = op == '=' ? split(args, argv, 5) : 0;
    bool ok = n >= 3;
    for (int i = 0; i < n && ok; i++) {
        ok = ip[i].fromString(argv[i]);
    }
    if (!ok) {
        reply_error(io, name);
        return;
    }
    WiFi.configSTA(ip[0], ip[3], ip[1], ip[2]);
    if (n == 5 && ip[0] != IPAddress(0, 0, 0, 0)) {
        WiFi.setDNS(ip[3], ip[4]);
    }
    reply(io, name, "");
}

/**
 * @brief Official defaults: open, channel 1, visible, max 5. The SDK has four association
 * slots; max 5 is a ceiling, not a promise of five-client capacity.
 */
static void cmd_beginsoftap(Stream &io, const char *name, char op, char *args)
{
    char *argv[5];
    long ch = 1, hidden = 0, maximum = 5;
    int n = op == '=' ? split(args, argv, 5) : 0;
    if (n == 0 || argv[0][0] == '\0' ||
        (n >= 3 && !parse_long(argv[2], &ch)) ||
        (n >= 4 && (!parse_long(argv[3], &hidden) || hidden < 0 || hidden > 1)) ||
        (n == 5 && !parse_long(argv[4], &maximum))) {
        reply_error(io, name);
        return;
    }
    wifi_started = true;
    uint8_t st = WiFi.beginAP(argv[0], n > 1 ? argv[1] : "", ch >= 1 && ch <= 13 ? (uint8_t)ch : 0,
                              hidden != 0, maximum >= 1 && maximum <= 5 ? (uint8_t)maximum : 0);
    reply(io, name, st == WL_AP_LISTENING ? "1" : "0");
}

/**
 * @brief Apply AP IPv4 addressing from validated AT arguments.
 */
static void cmd_softapconfig(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    IPAddress ip[3];
    int n = op == '=' ? split(args, argv, 3) : 0;
    bool ok = n == 3;
    for (int i = 0; i < n && ok; i++) {
        ok = ip[i].fromString(argv[i]);
    }
    if (!ok || !WiFi.softAPConfig(ip[0], ip[1], ip[2])) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

/**
 * @brief Stop AP while preserving the requested STA mode policy.
 */
static void cmd_disconnectsoftap(Stream &io, const char *name, char op, char *args)
{
    long wifi_off = 0;
    if ((op != '\0' && (op != '=' || !parse_long(args, &wifi_off) || wifi_off < 0 || wifi_off > 1)) ||
        !WiFi.softAPdisconnect(wifi_off != 0)) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

/**
 * @brief AT+IPSOFTAP has no '?' suffix, matching upstream; return 0.0.0.0 when AP is stopped.
 */
static void cmd_ipsoftap(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    reply(io, name, WiFi.softAPIP().toString().c_str());
}

/**
 * @brief Return the active AP SSID.
 */
static void cmd_getsoftapssid(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply(io, name, WiFi.softAPSSID());
}

/**
 * @brief The SDK AP and STA interfaces share one MAC address; use the common platform MAC
 * query.
 */
static void cmd_macsoftap(Stream &io, const char *name, char op, char *args)
{
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_mac(io, name, WiFi.macAddress(mac));
}

/**
 * @brief Return the mode-selected current SSID.
 */
static void cmd_getssid(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply(io, name, WiFi.SSID());
}

/**
 * @brief Return the current Wi-Fi signal strength in dBm.
 */
static void cmd_getrssi(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, WiFi.RSSI());
}

/**
 * @brief Return the connected access point BSSID.
 */
static void cmd_getbssid(Stream &io, const char *name, char op, char *args)
{
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_mac(io, name, WiFi.BSSID(mac));
}

/**
 * @brief Return the common platform MAC query in WiFiS3 response format.
 */
static void cmd_macsta(Stream &io, const char *name, char op, char *args)
{
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    (void)args;
    if (op != '?') {
        reply_error(io, name);
        return;
    }
    reply_mac(io, name, WiFi.macAddress(mac));
}

/**
 * @brief Use upstream encryption names so WiFiS3 can map them back to wl_enc_type.
 */
static const char *enc_name(uint8_t enc)
{
    switch (enc) {
        case ENC_TYPE_NONE: return "open";
        case ENC_TYPE_WEP: return "WEP";
        case ENC_TYPE_WPA: return "WPA";
        case ENC_TYPE_WPA2: return "WPA2";
        case ENC_TYPE_WPA2_ENTERPRISE: return "WPA2-EAP";
        case ENC_TYPE_WPA3: return "WPA3";
        default: return "unknown";
    }
}

/**
 * @brief AT+WIFISCAN replies "+WIFISCAN: <n>|", n list bytes and "OK". Each line is "<ssid> |
 * <bssid> | <rssi> | <channel> | <encryption>\\r\\n". No networks produces n=0 rather than
 * upstream's bare OK.
 */
static void cmd_wifiscan(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    int n = op == '\0' ? WiFi.scanNetworks() : -1;
    if (n < 0) {
        reply_error(io, name);
        return;
    }
    /* Ten rows, each with a 32-byte SSID and about 45 other bytes, fit in data. */
    char *list = (char *)data;
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b[WL_MAC_ADDR_LENGTH];
        WiFi.BSSID(i, b);
        len += snprintf(list + len, sizeof(data) - len, "%s | %02X:%02X:%02X:%02X:%02X:%02X | %ld | %u | %s\r\n",
                        WiFi.SSID(i), b[0], b[1], b[2], b[3], b[4], b[5], (long)WiFi.RSSI(i), WiFi.channel(i),
                        enc_name(WiFi.encryptionType(i)));
    }
    reply_data(io, name, len);
}

/**
 * @brief Disconnect STA while preserving the requested AP mode policy.
 */
static void cmd_disconnect(Stream &io, const char *name, char op, char *args)
{
    long wifi_off = 0;
    if (op != '\0' && (op != '=' || !parse_long(args, &wifi_off) || wifi_off < 0 || wifi_off > 1)) {
        reply_error(io, name);
        return;
    }
    if (wifi_started) {
        WiFi.disconnect(wifi_off != 0);
    }
    reply(io, name, "");
}

/**
 * @brief Resolve an AT hostname argument to IPv4.
 */
static void cmd_hostbyname(Stream &io, const char *name, char op, char *args)
{
    IPAddress ip;
    if (op != '=' || !WiFi.hostByName(args, ip)) {
        reply_error(io, name);
        return;
    }
    reply(io, name, ip.toString().c_str());
}

/**
 * @brief WiFiS3 sends "AT+PING=,<host>,<ttl>,<count>" with an extra comma after '='. Return
 * mean successful RTT milliseconds or WL_PING_*.
 */
static void cmd_ping(Stream &io, const char *name, char op, char *args)
{
    char *argv[4];
    long ttl, count;
    if (op != '=') {
        reply_error(io, name);
        return;
    }
    int n = split(args, argv, 4);
    char **a = n == 4 && argv[0][0] == '\0' ? argv + 1 : argv;
    if (a + 3 != argv + n || !parse_long(a[1], &ttl) || ttl < 1 || ttl > 255 || !parse_long(a[2], &count) ||
        count < 1 || count > 255) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, WiFi.ping(a[0], (uint8_t)ttl, (uint8_t)count));
}

/**
 * @brief Allocate a free plain TCP client identifier without connecting it.
 */
static void cmd_beginclient(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op == '\0') {
        for (int s = 0; s < MAX_SOCKETS; s++) {
            if (!allocated[s]) {
                allocated[s] = true;
                reply_int(io, name, s);
                return;
            }
        }
    }
    reply_error(io, name);
}

/**
 * @brief Configure SETCAROOT trust before SSL connect. Accepted server connections cannot
 * reconnect.
 */
static void connect_socket(Stream &io, const char *name, char **argv, long timeout_ms)
{
    long port;
    IPAddress ip;
    int s = socket_arg(name, argv[0]);
    if (s < 0 || (s >= SERVER_SOCK_BASE && s < SSL_SOCK_BASE) || !parse_long(argv[2], &port) || port < 1 ||
        port > 65535) {
        reply_error(io, name);
        return;
    }
    if (s >= SSL_SOCK_BASE) {
        int i = s - SSL_SOCK_BASE;
        ssls[i].setCACert(ssl_ca[i]);
        ssls[i].setCertificate(ssl_cert[i]);
        ssls[i].setPrivateKey(ssl_key[i]);
    }
    if (timeout_ms > 0) {
        client_of(s).setConnectionTimeout(timeout_ms);
    }
    /*
 * Dotted TCP addresses use connect(IPAddress), also usable in AP mode. Hostname/TLS paths require
 * STA; the platform determines network availability.
 */
    bool by_ip = s < SSL_SOCK_BASE && ip.fromString(argv[1]);
    if (!(by_ip ? client_of(s).connect(ip, (uint16_t)port) : client_of(s).connect(argv[1], (uint16_t)port))) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

/**
 * @brief AT+CLIENTCONNECTNAME=<sock>,<host>,<port>.
 */
static void cmd_clientconnectname(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    if (op != '=' || split(args, argv, 3) != 3) {
        reply_error(io, name);
        return;
    }
    connect_socket(io, name, argv, 0);
}

/**
 * @brief Connect a validated TCP or TLS client to an IPv4 address and port.
 */
static void cmd_clientconnectip(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    IPAddress ip;
    if (op != '=' || split(args, argv, 3) != 3 || !ip.fromString(argv[1])) {
        reply_error(io, name);
        return;
    }
    connect_socket(io, name, argv, 0);
}

/**
 * @brief AT+CLIENTCONNECT=<sock>,<host>,<port>[,<timeout-ms>]; omitted timeout uses the
 * official 3000 ms default.
 */
static void cmd_clientconnect(Stream &io, const char *name, char op, char *args)
{
    char *argv[4];
    long timeout_ms;
    int n = op == '=' ? split(args, argv, 4) : 0;
    if (n < 3 || n > 4 || (n == 4 && argv[3][0] == '\0')) {
        reply_error(io, name);
        return;
    }
    if (n == 4 && (!parse_long(argv[3], &timeout_ms) || timeout_ms <= 0)) {
        timeout_ms = CONNECT_TIMEOUT_MS;
    } else if (n == 3) {
        timeout_ms = CONNECT_TIMEOUT_MS;
    }
    connect_socket(io, name, argv, timeout_ms);
}

/**
 * @brief AT+CLIENTSEND=<sock>,<n> is followed by n raw bytes streamed to the socket. Drain all
 * n even on failure to retain framing. On success reply bare OK, matching the host's
 * unchecked- prefix path.
 */
static void cmd_clientsend(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long left;
    if (op != '=' || split(args, argv, 2) != 2 || !parse_long(argv[1], &left) || left < 0) {
        reply_error(io, name);
        return;
    }
    int s = socket_arg(name, argv[0]);
    if (read_payload(io, left, s >= 0 ? &client_of(s) : nullptr)) {
        io.print("OK\r\n");
    } else {
        reply_error(io, name);
    }
}

/**
 * @brief Return readable bytes for a validated TCP or TLS client.
 */
static void cmd_available(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, client_of(s).available());
}

/**
 * @brief AT+CLIENTRECEIVE=<sock>,<max>: see reply_data for response framing.
 */
static void cmd_clientreceive(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long max;
    int s = -1;
    if (op == '=' && split(args, argv, 2) == 2 && parse_long(argv[1], &max) && max >= 0) {
        s = socket_arg(name, argv[0]);
    }
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    size_t want = min(max, (long)RECV_MAX);
    size_t got = 0;
    while (got < want) {
        int n = client_of(s).read(data + got, want - got);
        if (n <= 0) {
            break;
        }
        got += n;
    }
    reply_data(io, name, got);
}

/**
 * @brief Report connection state for a validated TCP or TLS client.
 */
static void cmd_clientconnected(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, client_of(s).connected() ? 1 : 0);
}

/**
 * @brief Return the WiFiS3-compatible socket state for a validated client.
 */
static void cmd_clientstate(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    if (client_of(s).connected()) {
        char result[64];
        snprintf(result, sizeof(result), "%s,%u,%u", client_of(s).remoteIP().toString().c_str(),
                 (unsigned)client_of(s).remotePort(), (unsigned)client_of(s).localPort());
        reply(io, name, result);
    } else {
        reply(io, name, "");
    }
}

/**
 * @brief Return the local IPv4 address of a validated client socket.
 */
static void cmd_ipclient(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    if (client_of(s).connected()) {
        reply(io, name, client_of(s).localIP().toString().c_str());
    } else {
        reply(io, name, "");
    }
}

/**
 * @brief Report whether the requested client identifier remains connected.
 */
static void cmd_clientstatus(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

/**
 * @brief Close a validated client and release its bridge-owned resources.
 */
static void cmd_clientclose(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    close_socket(s);
    reply(io, name, "");
}

/**
 * @brief Platform WiFiClient sends without buffering, so flush has nothing to drain.
 */
static void cmd_flush(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    client_of(s).flush();
    reply(io, name, "");
}

/**
 * @brief Return the remote IPv4 address of a validated client socket.
 */
static void cmd_remoteip(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply(io, name, client_of(s).remoteIP().toString().c_str());
}

/**
 * @brief Return the remote port of a validated client socket.
 */
static void cmd_remoteport(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, client_of(s).remotePort());
}

/**
 * @brief Only SSL exposes PEEK commands; WiFiS3 WiFiClient::peek inspects its local buffer.
 */
static void cmd_peek(Stream &io, const char *name, char op, char *args)
{
    int s = op == '=' ? socket_arg(name, args) : -1;
    if (s < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, client_of(s).peek());
}

/**
 * @brief Allocate a free TLS client identifier with independent credential ownership.
 */
static void cmd_sslbeginclient(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op == '\0') {
        for (int i = 0; i < MAX_SSL; i++) {
            if (!ssl_allocated[i]) {
                ssl_allocated[i] = true;
                reply_int(io, name, SSL_SOCK_BASE + i);
                return;
            }
        }
    }
    reply_error(io, name);
}

/* Write payload into a fixed buffer; fail on overflow. */
class BufferSink : public Print {
public:
    /**
     * @brief Wrap a caller-owned bounded buffer as a Print payload sink.
     */
    BufferSink(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap) {}
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(uint8_t b) override { return write(&b, 1); }
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(const uint8_t *p, size_t n) override
    {
        if (n > cap_ - len_) {
            return 0;
        }
        memcpy(buf_ + len_, p, n);
        len_ += n;
        return n;
    }

private:
    uint8_t *buf_;
    size_t cap_;
    size_t len_ = 0;
};

/**
 * @brief AT+SETCAROOT=<sock> restores bundled roots for later connections and replies
 * "+SETCAROOT: " plus OK. With <n>, consume n PEM bytes and trust only them; success is bare
 * OK. Drain payload even on error.
 */
static void cmd_setcaroot(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long n;
    int argc = op == '=' ? split(args, argv, 2) : 0;
    int i = argc >= 1 ? ssl_slot(argv[0]) : -1;
    if (argc == 1 && i >= 0) {
        free(ssl_ca[i]);
        ssl_ca[i] = nullptr;
        reply(io, name, "");
        return;
    }
    if (argc != 2 || !parse_long(argv[1], &n) || n <= 0) {
        reply_error(io, name);
        return;
    }
    char *pem = i >= 0 && n <= CA_MAX ? static_cast<char *>(malloc(n + 1)) : nullptr;
    BufferSink sink(reinterpret_cast<uint8_t *>(pem), n);
    if (!read_payload(io, n, pem != nullptr ? &sink : nullptr)) {
        free(pem);
        reply_error(io, name);
        return;
    }
    pem[n] = '\0';
    free(ssl_ca[i]);
    ssl_ca[i] = pem;
    io.print("OK\r\n");
}

/**
 * @brief Receive a bounded PEM payload and replace the selected credential only on success.
 */
static bool store_pem_payload(Stream &io, long size, char **slot)
{
    if (size <= 0 || size > CA_MAX) {
        read_payload(io, size, nullptr);
        return false;
    }
    char *pem = static_cast<char *>(malloc((size_t)size + 1));
    if (pem == nullptr) {
        read_payload(io, size, nullptr);
        return false;
    }
    BufferSink sink(reinterpret_cast<uint8_t *>(pem), size);
    bool ok = read_payload(io, size, &sink);
    if (!ok) {
        free(pem);
        return false;
    }
    pem[size] = '\0';
    free(*slot);
    *slot = pem;
    return true;
}

/**
 * @brief Receive and replace the selected TLS client's PEM certificate.
 */
static void cmd_sslclientsetcert(Stream &io, const char *name, char op, char *args)
{
    char *argv[2]; long size;
    int n = op == '=' ? split(args, argv, 2) : 0;
    int s = n == 2 ? ssl_slot(argv[0]) : -1;
    bool parsed = n == 2 && parse_long(argv[1], &size);
    bool stored = parsed && s >= 0 && store_pem_payload(io, size, &ssl_cert[s]);
    if (parsed && s < 0) {
        read_payload(io, size, nullptr);
    }
    if (!stored) {
        reply_error(io, name);
        return;
    }
    ssls[s].setCertificate(ssl_cert[s]);
    reply(io, name, "");
}

/**
 * @brief Receive and replace the selected TLS client's PEM private key.
 */
static void cmd_sslclientsetpkey(Stream &io, const char *name, char op, char *args)
{
    char *argv[2]; long size;
    int n = op == '=' ? split(args, argv, 2) : 0;
    int s = n == 2 ? ssl_slot(argv[0]) : -1;
    bool parsed = n == 2 && parse_long(argv[1], &size);
    bool stored = parsed && s >= 0 && store_pem_payload(io, size, &ssl_key[s]);
    if (parsed && s < 0) {
        read_payload(io, size, nullptr);
    }
    if (!stored) {
        reply_error(io, name);
        return;
    }
    ssls[s].setPrivateKey(ssl_key[s]);
    reply(io, name, "");
}

/**
 * @brief Allocate a PEM encoding of DER data with the supplied header and footer.
 */
static char *der_to_pem(const uint8_t *der, size_t size, const char *header, const char *footer)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded = ((size + 2) / 3) * 4;
    size_t cap = strlen(header) + encoded + (encoded + 63) / 64 + strlen(footer) + 1;
    char *pem = static_cast<char *>(malloc(cap));
    if (pem == nullptr) {
        return nullptr;
    }
    size_t out = 0, line = 0;
    memcpy(pem + out, header, strlen(header));
    out += strlen(header);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = uint32_t(der[i]) << 16;
        if (i + 1 < size) v |= uint32_t(der[i + 1]) << 8;
        if (i + 2 < size) v |= der[i + 2];
        pem[out++] = alphabet[(v >> 18) & 63];
        pem[out++] = alphabet[(v >> 12) & 63];
        pem[out++] = i + 1 < size ? alphabet[(v >> 6) & 63] : '=';
        pem[out++] = i + 2 < size ? alphabet[v & 63] : '=';
        line += 4;
        if (line == 64 && i + 3 < size) {
            pem[out++] = '\n';
            line = 0;
        }
    }
    memcpy(pem + out, footer, strlen(footer));
    out += strlen(footer);
    pem[out] = '\0';
    if (out >= cap) {
        free(pem);
        return nullptr;
    }
    return pem;
}

/**
 * @brief AT+SETECCSLOT=<sock>,<slot>,<n>: use the payload certificate and read its private key
 * from LittleFS Preferences tls-sse/slotN.
 */
static void cmd_seteccslot(Stream &io, const char *name, char op, char *args)
{
    char *argv[3];
    long n = 0;
    long slot = -1;
    int s = -1;
    if (op == '=' && split(args, argv, 3) == 3 && parse_long(argv[1], &slot) && slot >= 0 && slot <= 15 &&
        parse_long(argv[2], &n) && n > 0 && n <= 4096) {
        s = ssl_slot(argv[0]);
    }
    if (s < 0) {
        if (n > 0) read_payload(io, n, nullptr);
        reply_error(io, name);
        return;
    }
    uint8_t *der = static_cast<uint8_t *>(malloc((size_t)n));
    BufferSink der_sink(der, n);
    bool got = der != nullptr && read_payload(io, n, &der_sink);
    if (!got) {
        free(der);
        reply_error(io, name);
        return;
    }
    char key_name[16];
    snprintf(key_name, sizeof(key_name), "slot%ld", slot);
    Preferences sse;
    size_t key_size = 0;
    if (!sse.begin(TLS_SLOT_NAMESPACE, true, "lfs")) {
        free(der);
        reply_error(io, name);
        return;
    }
    key_size = sse.getBytesLength(key_name);
    char *cert = der_to_pem(der, (size_t)n, "-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n");
    char *key_der = key_size > 0 && key_size <= Preferences::MAX_VALUE_SIZE ? static_cast<char *>(malloc(key_size)) : nullptr;
    char *key = nullptr;
    bool key_ok = key_der != nullptr && sse.getBytes(key_name, key_der, key_size) == key_size;
    sse.end();
    if (key_ok) {
        key = der_to_pem(reinterpret_cast<const uint8_t *>(key_der), key_size,
                         "-----BEGIN EC PRIVATE KEY-----\n", "-----END EC PRIVATE KEY-----\n");
    }
    free(key_der);
    free(der);
    if (cert == nullptr || key == nullptr) {
        free(cert); free(key);
        reply_error(io, name);
        return;
    }
    free(ssl_cert[s]); ssl_cert[s] = cert;
    free(ssl_key[s]); ssl_key[s] = key;
    ssls[s].setCertificate(cert);
    ssls[s].setPrivateKey(key);
    reply(io, name, "");
}

/**
 * @brief Return an allocated server slot, or -1.
 */
static int server_arg(const char *s)
{
    long v;
    return parse_long(s, &v) && v >= 0 && v < MAX_SERVERS && server_allocated[v] ? (int)v : -1;
}

/**
 * @brief Find a free accepted-client slot, otherwise close/reuse one whose connection ended
 * and data was drained, matching upstream connected() reclamation. Return -1 if full.
 */
static int free_conn(void)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (conns[i].server < 0) {
            return i;
        }
    }
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!conns[i].client.connected()) {
            close_socket(SERVER_SOCK_BASE + i);
            return i;
        }
    }
    return -1;
}

/**
 * @brief AT+BEGINSERVER=<port>: return a server identifier.
 */
static void cmd_beginserver(Stream &io, const char *name, char op, char *args)
{
    long port;
    if (op == '=' && parse_long(args, &port) && port >= 1 && port <= 65535) {
        for (int v = 0; v < MAX_SERVERS; v++) {
            if (!server_allocated[v]) {
                servers[v].begin((uint16_t)port);
                if (!servers[v]) {
                    break;
                }
                server_allocated[v] = true;
                reply_int(io, name, v);
                return;
            }
        }
    }
    reply_error(io, name);
}

/**
 * @brief AT+SERVERAVAILABLE=<srv> accepts new clients into free slots, then rotates from the
 * last result to find a readable client not transferred by SERVERACCEPT. Return its
 * identifier, or -1 if none, matching upstream.
 */
static void cmd_serveravailable(Stream &io, const char *name, char op, char *args)
{
    static int last;
    int v = op == '=' ? server_arg(args) : -1;
    if (v < 0) {
        reply_error(io, name);
        return;
    }
    for (int i = free_conn(); i >= 0; i = free_conn()) {
        WiFiClient c = servers[v].accept();
        if (!c) {
            break;
        }
        conns[i].client = c;
        conns[i].server = v;
        conns[i].accepted = false;
    }
    for (int k = 0; k < MAX_SOCKETS; k++) {
        int i = (last + k) % MAX_SOCKETS;
        if (conns[i].server == v && !conns[i].accepted && conns[i].client.available() > 0) {
            last = i;
            reply_int(io, name, SERVER_SOCK_BASE + i);
            return;
        }
    }
    reply_int(io, name, -1);
}

/**
 * @brief AT+SERVERACCEPT=<srv> transfers one new client to the host; return its identifier or
 * -1.
 */
static void cmd_serveraccept(Stream &io, const char *name, char op, char *args)
{
    int v = op == '=' ? server_arg(args) : -1;
    if (v < 0) {
        reply_error(io, name);
        return;
    }
    int i = free_conn();
    WiFiClient c = i >= 0 ? servers[v].accept() : WiFiClient();
    if (!c) {
        reply_int(io, name, -1);
        return;
    }
    conns[i].client = c;
    conns[i].server = v;
    conns[i].accepted = true;
    reply_int(io, name, SERVER_SOCK_BASE + i);
}

/*
 * SERVERWRITE targets connected clients retained by SERVERAVAILABLE for srv. Return the minimum
 * byte count sent to every client.
 */
class ServerSink : public Print {
public:
    /**
     * @brief Wrap an allocated server as a Print sink for streamed AT payloads.
     */
    explicit ServerSink(int srv) : srv_(srv) {}
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(uint8_t b) override { return write(&b, 1); }
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(const uint8_t *buf, size_t n) override
    {
        size_t least = n;
        for (auto &c : conns) {
            if (c.server == srv_ && !c.accepted && c.client.connected()) {
                least = min(least, c.client.write(buf, n));
            }
        }
        return least;
    }
    using Print::write;

private:
    int srv_;
};

/**
 * @brief AT+SERVERWRITE=<srv>,<n> is followed by n raw bytes; like CLIENTSEND, success replies
 * bare OK.
 */
static void cmd_serverwrite(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long left;
    if (op != '=' || split(args, argv, 2) != 2 || !parse_long(argv[1], &left) || left < 0) {
        reply_error(io, name);
        return;
    }
    int v = server_arg(argv[0]);
    ServerSink sink(v);
    if (read_payload(io, left, v >= 0 ? &sink : nullptr)) {
        io.print("OK\r\n");
    } else {
        reply_error(io, name);
    }
}

/**
 * @brief Stop listening but retain accepted clients for normal CLIENT* commands, matching
 * upstream.
 */
static void cmd_serverend(Stream &io, const char *name, char op, char *args)
{
    int v = op == '=' ? server_arg(args) : -1;
    if (v < 0) {
        reply_error(io, name);
        return;
    }
    servers[v].end();
    server_allocated[v] = false;
    reply(io, name, "");
}

/**
 * @brief AT+UDPBEGIN=<local-port>[,<local-address>]: return a UDP identifier.
 */
static void udp_begin(Stream &io, const char *name, char op, char *args, bool multicast)
{
    char *argv[2];
    long port;
    IPAddress local;
    int n = op == '=' ? split(args, argv, 2) : 0;
    if (n == 0 || !parse_long(argv[0], &port) || port < 0 || port > 65535 ||
        (n == 2 && !local.fromString(argv[1])) || (multicast && (n != 2 || port == 0))) {
        reply_error(io, name);
        return;
    }
    for (int u = 0; u < MAX_UDP; u++) {
        if (!udp_allocated[u]) {
            if (!(multicast ? udps[u].beginMulticast(local, (uint16_t)port) :
                  n == 2 ? udps[u].begin(local, (uint16_t)port) : udps[u].begin((uint16_t)port))) {
                break;
            }
            udp_allocated[u] = true;
            reply_int(io, name, u);
            return;
        }
    }
    reply_error(io, name);
}

/**
 * @brief Allocate and bind a plain UDP socket from AT arguments.
 */
static void cmd_udpbegin(Stream &io, const char *name, char op, char *args)
{
    udp_begin(io, name, op, args, false);
}

/**
 * @brief Allocate a UDP socket and join the requested multicast group.
 */
static void cmd_udpbeginmulti(Stream &io, const char *name, char op, char *args)
{
    udp_begin(io, name, op, args, true);
}

/**
 * @brief Select a retained UDP destination or multicast group for the next packet.
 */
static void begin_existing_packet(Stream &io, const char *name, char op, char *args, bool multicast)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0 || !(multicast ? udps[u].beginMulticastPacket() : udps[u].beginPacket())) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

/**
 * @brief Select the UDP socket's retained destination for the next packet.
 */
static void cmd_udpbeginpacket(Stream &io, const char *name, char op, char *args)
{
    begin_existing_packet(io, name, op, args, false);
}

/**
 * @brief Select the UDP socket's joined multicast group for the next packet.
 */
static void cmd_beginpacketmulti(Stream &io, const char *name, char op, char *args)
{
    begin_existing_packet(io, name, op, args, true);
}

/**
 * @brief AT+UDPBEGINPACKETIP=<udp>,<port>,<address> and
 * UDPBEGINPACKETADD=<udp>,<port>,<hostname> select destinations.
 */
static void begin_packet(Stream &io, const char *name, char op, char *args, bool by_name)
{
    char *argv[3];
    long port;
    IPAddress ip;
    int u = -1;
    if (op == '=' && split(args, argv, 3) == 3 && parse_long(argv[1], &port) && port > 0 && port <= 65535 &&
        (by_name || ip.fromString(argv[2]))) {
        u = udp_arg(argv[0]);
    }
    if (u < 0 || !(by_name ? udps[u].beginPacket(argv[2], (uint16_t)port) : udps[u].beginPacket(ip, (uint16_t)port))) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

/**
 * @brief Select an explicit IPv4 UDP destination and port.
 */
static void cmd_udpbeginpacketip(Stream &io, const char *name, char op, char *args)
{
    begin_packet(io, name, op, args, false);
}

/**
 * @brief Resolve and select a hostname UDP destination and port.
 */
static void cmd_udpbeginpacketadd(Stream &io, const char *name, char op, char *args)
{
    begin_packet(io, name, op, args, true);
}

/**
 * @brief AT+UDPWRITE=<udp>,<n> appends n following raw bytes to the current datagram; like
 * CLIENTSEND, success replies bare OK.
 */
static void cmd_udpwrite(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long left;
    if (op != '=' || split(args, argv, 2) != 2 || !parse_long(argv[1], &left) || left < 0) {
        reply_error(io, name);
        return;
    }
    int u = udp_arg(argv[0]);
    if (read_payload(io, left, u >= 0 ? &udps[u] : nullptr)) {
        io.print("OK\r\n");
    } else {
        reply_error(io, name);
    }
}

/**
 * @brief Send the buffered UDP datagram and report acceptance.
 */
static void cmd_udpendpacket(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0 || !udps[u].endPacket()) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
}

/**
 * @brief Fetch the next UDP datagram and report its size.
 */
static void cmd_udpparse(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, udps[u].parsePacket());
}

/**
 * @brief Report unread bytes in the selected UDP datagram.
 */
static void cmd_udpavailable(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, udps[u].available());
}

/**
 * @brief AT+UDPREAD=<udp>,<max> reads the current datagram; see reply_data for framing.
 */
static void cmd_udpread(Stream &io, const char *name, char op, char *args)
{
    char *argv[2];
    long max;
    int u = -1;
    if (op == '=' && split(args, argv, 2) == 2 && parse_long(argv[1], &max) && max >= 0) {
        u = udp_arg(argv[0]);
    }
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_data(io, name, udps[u].read(data, min(max, (long)sizeof(data))));
}

/**
 * @brief Return the next byte, or -1 when none is available.
 */
static void cmd_udppeek(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, udps[u].peek());
}

/**
 * @brief Discard unread bytes in the selected UDP datagram.
 */
static void cmd_udpflush(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    udps[u].flush();
    reply(io, name, "");
}

/**
 * @brief Return the most recently received UDP source IPv4 address.
 */
static void cmd_udpremoteip(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply(io, name, udps[u].remoteIP().toString().c_str());
}

/**
 * @brief Return the most recently received UDP source port.
 */
static void cmd_udpremoteport(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    reply_int(io, name, udps[u].remotePort());
}

/**
 * @brief Close a validated UDP socket and release its identifier.
 */
static void cmd_udpstop(Stream &io, const char *name, char op, char *args)
{
    int u = op == '=' ? udp_arg(args) : -1;
    if (u < 0) {
        reply_error(io, name);
        return;
    }
    close_udp(u);
    reply(io, name, "");
}

/**
 * @brief Retain official MOUNTFS syntax but never format automatically on mount failure,
 * including format_on_fault=1.
 */
static void cmd_mountfs(Stream &io, const char *name, char op, char *args)
{
    long flag;
    if (op != '=' || !parse_long(args, &flag) || (flag != 0 && flag != 1) || !LittleFS.begin()) {
        reply_error(io, name);
        return;
    }
    reply(io, name, "FS Mounted\r\n");
}

/**
 * @brief Preserve the upstream extra blank line for numeric PREF results. Zero indicates
 * begin/write/delete/clear failure.
 */
static void pref_number(Stream &io, const char *name, uint64_t value)
{
    char result[24];
    snprintf(result, sizeof(result), "%" PRIu64 "\r\n", value);
    reply(io, name, result);
}

/**
 * @brief Validate a Preferences key using the bridge's bounded name rules.
 */
static bool pref_name(const char *key) { return key && *key && strlen(key) <= 15; }
/**
 * @brief Report whether a Preferences read failed for a reason requiring an AT error.
 */
static bool pref_default_error() {
    int e = preferences.lastError();
    return e == 0 || e == LFS_ERR_NOENT || e == LFS_ERR_INVAL;
}

/**
 * @brief Open a validated Preferences namespace with the requested read-only policy.
 */
static void cmd_prefbegin(Stream &io, const char *name, char op, char *args)
{
    char *argv[4]; long readOnly;
    int argc = split(args, argv, 4);
    if (op != '=' || argc != 3 || !parse_long(argv[1], &readOnly) || (readOnly != 0 && readOnly != 1)) {
        reply_error(io, name); return;
    }
    preferences.end();
    pref_number(io, name, preferences.begin(argv[0], readOnly, argv[2]));
}

/**
 * @brief Handle Preferences end, clear and remaining-entry queries.
 */
static void cmd_prefcontrol(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != 0) { reply_error(io, name); return; }
    if (!strcmp(name, "+PREFEND")) { preferences.end(); reply(io, name, "0"); return; }
    if (!strcmp(name, "+PREFCLEAR")) { pref_number(io, name, preferences.clear()); return; }
    size_t n = preferences.freeEntries();
    if (preferences.lastError()) reply_error(io, name);
    else pref_number(io, name, n);
}

/**
 * @brief Handle Preferences remove, type and blob-length operations for one key.
 */
static void cmd_prefkey(Stream &io, const char *name, char op, char *args)
{
    if (op != '=' || !pref_name(args) || strchr(args, ',')) { reply_error(io, name); return; }
    if (!strcmp(name, "+PREFREMOVE")) { pref_number(io, name, preferences.remove(args)); return; }
    size_t value = !strcmp(name, "+PREFTYPE") ? preferences.getType(args) : preferences.getBytesLength(args);
    if (!pref_default_error()) reply_error(io, name);
    else pref_number(io, name, value);
}

/**
 * @brief Parse a Preferences integer while checking the selected type's signedness and range.
 */
static bool pref_integer(const char *text, long type, int64_t &signedValue, uint64_t &unsignedValue)
{
    bool negative = *text == '-';
    if (*text == '+' || negative) ++text;
    if (!*text || ((type & 1) && negative)) return false;
    unsigned bits = (1u << (type / 2)) * 8;
    uint64_t limit = (type & 1) ? UINT64_MAX >> (64 - bits) :
        (uint64_t(1) << (bits - 1)) - !negative;
    uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = *text - '0';
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    unsignedValue = value;
    if (!(type & 1)) signedValue = negative ?
        (value == (uint64_t(1) << 63) ? INT64_MIN : -int64_t(value)) : int64_t(value);
    return true;
}

/**
 * @brief Validate and store a typed Preferences value from AT arguments.
 */
static void cmd_prefput(Stream &io, const char *name, char op, char *args)
{
    char *argv[4]; long type;
    int argc = split(args, argv, 4);
    if (op != '=' || argc < 3 || !parse_long(argv[1], &type)) { reply_error(io, name); return; }
    if (type == PT_STR || type == PT_BLOB) {
        long length;
        if (!parse_long(argv[2], &length) || length < 0) { reply_error(io, name); return; }
        if (argc != 3 || !pref_name(argv[0]) || length > (long)Preferences::MAX_VALUE_SIZE) {
            read_payload(io, length, nullptr); reply_error(io, name); return;
        }
        if (io.readBytes(data, length) != (size_t)length ||
            (type == PT_STR && memchr(data, 0, length)) || (type == PT_BLOB && !length)) {
            reply_error(io, name); return;
        }
        data[length] = 0;
        size_t n = type == PT_STR ? preferences.putString(argv[0], reinterpret_cast<const char *>(data)) :
                                   preferences.putBytes(argv[0], data, length);
        pref_number(io, name, n); return;
    }
    int64_t s = 0; uint64_t u = 0;
    if (argc != 3 || !pref_name(argv[0]) || type < 0 || type >= PT_STR || !pref_integer(argv[2], type, s, u)) {
        reply_error(io, name); return;
    }
    size_t n = 0;
    switch (type) {
        case PT_I8: n = preferences.putChar(argv[0], s); break;
        case PT_U8: n = preferences.putUChar(argv[0], u); break;
        case PT_I16: n = preferences.putShort(argv[0], s); break;
        case PT_U16: n = preferences.putUShort(argv[0], u); break;
        case PT_I32: n = preferences.putInt(argv[0], s); break;
        case PT_U32: n = preferences.putUInt(argv[0], u); break;
        case PT_I64: n = preferences.putLong64(argv[0], s); break;
        case PT_U64: n = preferences.putULong64(argv[0], u); break;
    }
    pref_number(io, name, n);
}

/**
 * @brief Read a typed Preferences value or validated fallback into the AT response.
 */
static void cmd_prefget(Stream &io, const char *name, char op, char *args)
{
    char *argv[4]; long type;
    int argc = split(args, argv, 4);
    if (op != '=' || argc < 2 || !pref_name(argv[0]) || !parse_long(argv[1], &type) ||
        type < 0 || type >= PT_INVALID || argc != (type == PT_BLOB ? 2 : 3)) {
        reply_error(io, name); return;
    }
    if (type == PT_STR || type == PT_BLOB) {
        size_t n;
        if (type == PT_STR) {
            String result = preferences.getString(argv[0], argv[2]);
            n = result.length(); memcpy(data, result.c_str(), n);
        } else {
            n = preferences.getBytesLength(argv[0]);
            if (n && preferences.getBytes(argv[0], data, sizeof(data)) != n) { reply_error(io, name); return; }
        }
        if (!pref_default_error()) { reply_error(io, name); return; }
        io.print(name); io.print(": "); io.print(n); io.print('|'); io.write(data, n); io.print("\r\nOK\r\n");
        return;
    }
    int64_t s = 0; uint64_t u = 0;
    if (!pref_integer(argv[2], type, s, u)) { reply_error(io, name); return; }
    switch (type) {
        case PT_I8: s = preferences.getChar(argv[0], s); break;
        case PT_U8: u = preferences.getUChar(argv[0], u); break;
        case PT_I16: s = preferences.getShort(argv[0], s); break;
        case PT_U16: u = preferences.getUShort(argv[0], u); break;
        case PT_I32: s = preferences.getInt(argv[0], s); break;
        case PT_U32: u = preferences.getUInt(argv[0], u); break;
        case PT_I64: s = preferences.getLong64(argv[0], s); break;
        case PT_U64: u = preferences.getULong64(argv[0], u); break;
    }
    if (!pref_default_error()) { reply_error(io, name); return; }
    if (type & 1) pref_number(io, name, u);
    else { char result[24]; snprintf(result, sizeof(result), "%" PRId64 "\r\n", s); reply(io, name, result); }
}

/**
 * @brief Official FS=0,op,path[,size|,offset,length]. Consume the declared write payload even
 * if the operation fails.
 */
static void cmd_fs(Stream &io, const char *name, char op, char *args)
{
    if (op != '=') {
        reply_error(io, name);
        return;
    }
    char *argv[6];
    int argc = split(args, argv, 6);
    long type, action, length;
    if (argc < 2 || !parse_long(argv[1], &action)) {
        reply_error(io, name);
        return;
    }
    bool local = argc >= 3 && parse_long(argv[0], &type) && type == 0 && LittleFS.mounted() &&
                 !Preferences::isStoragePath(argv[2]);
    if (action == 1 || action == 3) {
        if (argc < 4 || !parse_long(argv[3], &length) || length < 0) {
            reply_error(io, name);
            return;
        }
        if (!local || argc != 4 || length > 255) {
            read_payload(io, length, nullptr);
            reply_error(io, name);
            return;
        }
        /*
 * Flash operations briefly mask interrupts. Receive the full frame first to avoid losing UART
 * payload still arriving during writes.
 */
        if (io.readBytes(data, length) != (size_t)length) {
            reply_error(io, name);
            return;
        }
        File file = LittleFS.open(argv[2], action == 1 ? "w" : "a");
        bool written = file && file.write(data, length) == (size_t)length;
        bool committed = file && file.close();
        if (written && committed) io.print("OK\r\n");
        else reply_error(io, name);
        return;
    }
    if (local && action == 0 && argc == 3) {
        if (LittleFS.remove(argv[2])) io.print("OK\r\n");
        else reply_error(io, name);
        return;
    }
    long offset;
    if (!local || action != 2 || argc != 5 || !parse_long(argv[3], &offset) ||
        offset < 0 || offset > 0x7fffffffL || !parse_long(argv[4], &length) ||
        length < 0 || length > RECV_MAX) {
        reply_error(io, name);
        return;
    }
    File file = LittleFS.open(argv[2]);
    if (!file || (size_t)offset >= file.size() || !file.seek(offset)) {
        reply_error(io, name);
        return;
    }
    int n = file.read(data, min((size_t)length, file.size() - offset));
    bool closed = file.close();
    if (n < 0 || !closed) {
        reply_error(io, name);
        return;
    }
    io.print(name);
    io.print(": ");
    io.print(n);
    io.print('|');
    io.write(data, n);
    io.print("\r\nOK\r\n");
}

/**
 * @brief AT+RESTARTBOOTLOADER resets BL616CL into ROM for firmware update. Upstream resets
 * without replying, forcing a host timeout; acknowledge and drain output first here.
 */
static void cmd_restartbootloader(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') {
        reply_error(io, name);
        return;
    }
    reply(io, name, "");
    io.flush();
    resetToBootloader();
}

/** @brief Receive the chip-labelled 512-byte BL firmware update header. */
static void cmd_blfotabegin(Stream &io, const char *name, char op, char *args)
{
    long length;
    if (op != '=' || !parse_long(args, &length) || length < 0) { reply_error(io, name); return; }
    if (length != 512) { read_payload(io, length, nullptr); reply_error(io, name); return; }
    if (io.readBytes(data, length) != (size_t)length || !BLUpdate.begin(data, length)) {
        BLUpdate.abort(); reply_error(io, name); return;
    }
    reply_int(io, name, 0);
}

/** @brief Receive one ordered raw update chunk before writing it to flash. */
static void cmd_blfotadata(Stream &io, const char *name, char op, char *args)
{
    char *argv[3]; long offset, length;
    int count = split(args, argv, 3);
    if (op != '=' || count != 2 || !parse_long(argv[0], &offset) ||
        !parse_long(argv[1], &length) || length < 0) { reply_error(io, name); return; }
    if (offset < 0 || length == 0 || length > 1024) {
        read_payload(io, length, nullptr); BLUpdate.abort(); reply_error(io, name); return;
    }
    if (io.readBytes(data, length) != (size_t)length || !BLUpdate.write(offset, data, length)) {
        BLUpdate.abort(); reply_error(io, name); return;
    }
    reply_int(io, name, 0);
}

/** @brief Verify, commit, or abort a platform firmware update session. */
static void cmd_blfotacontrol(Stream &io, const char *name, char op, char *args)
{
    (void)args;
    if (op != '\0') { reply_error(io, name); return; }
    if (!strcmp(name, "+BLFOTAABORT")) { BLUpdate.abort(); reply_int(io, name, 0); return; }
    bool commit = !strcmp(name, "+BLFOTACOMMIT");
    if (!(commit ? BLUpdate.commit() : BLUpdate.finish())) { reply_error(io, name); return; }
    reply_int(io, name, 0);
    if (commit) { io.flush(); bl_sys_reset_por(); }
}

static const struct {
    const char *name;
    handler_t handler;
} commands[] = {
    { "+BLFOTABEGIN", cmd_blfotabegin },
    { "+BLFOTADATA", cmd_blfotadata },
    { "+BLFOTAFINISH", cmd_blfotacontrol },
    { "+BLFOTACOMMIT", cmd_blfotacontrol },
    { "+BLFOTAABORT", cmd_blfotacontrol },
    { "+PREFBEGIN", cmd_prefbegin },
    { "+PREFEND", cmd_prefcontrol },
    { "+PREFCLEAR", cmd_prefcontrol },
    { "+PREFSTAT", cmd_prefcontrol },
    { "+PREFREMOVE", cmd_prefkey },
    { "+PREFTYPE", cmd_prefkey },
    { "+PREFLEN", cmd_prefkey },
    { "+PREFPUT", cmd_prefput },
    { "+PREFGET", cmd_prefget },
    { "+MOUNTFS", cmd_mountfs },
    { "+FS", cmd_fs },
    { "+SOFTRESETWIFI", cmd_softreset },
    { "+HCIBEGIN", cmd_hcibegin },
    { "+HCIEND", cmd_hciend },
    { "+HCIWAIT", cmd_hciwait },
    { "+HCIAVAILABLE", cmd_hciavailable },
    { "+HCIREAD", cmd_hciread },
    { "+HCIWRITE", cmd_hciwrite },
    { "+FWVERSION", cmd_fwversion },
    { "+FWVERSION_U32", cmd_fwversion_u32 },
    { "+WIFIMODE", cmd_wifimode },
    { "+GETTIME", cmd_gettime },
    { "+HOSTNAME", cmd_hostname },
    { "+BEGINSTA", cmd_beginsta },
    { "+GETSTATUS", cmd_getstatus },
    { "+IPSTA", cmd_ipsta },
    { "+SETIP", cmd_setip },
    { "+GETSSID", cmd_getssid },
    { "+GETRSSI", cmd_getrssi },
    { "+GETBSSID", cmd_getbssid },
    { "+MACSTA", cmd_macsta },
    { "+RECONNECT", cmd_reconnect },
    { "+AUTOCONNECT", cmd_autoconnect },
    { "+IPV6", cmd_ipv6 },
    { "+DISCONNECTSOFTAP", cmd_disconnectsoftap },
    { "+BEGINSOFTAP", cmd_beginsoftap },
    { "+SOFTAPCONFIG", cmd_softapconfig },
    { "+IPSOFTAP", cmd_ipsoftap },
    { "+GETSOFTAPSSID", cmd_getsoftapssid },
    { "+MACSOFTAP", cmd_macsoftap },
    { "+WIFISCAN", cmd_wifiscan },
    { "+DISCONNECT", cmd_disconnect },
    { "+HOSTBYNAME", cmd_hostbyname },
    { "+PING", cmd_ping },
    { "+BEGINCLIENT", cmd_beginclient },
    { "+CLIENTSTATE", cmd_clientstate },
    { "+CLIENTCONNECTIP", cmd_clientconnectip },
    { "+CLIENTCONNECTNAME", cmd_clientconnectname },
    { "+CLIENTCONNECT", cmd_clientconnect },
    { "+CLIENTSEND", cmd_clientsend },
    { "+AVAILABLE", cmd_available },
    { "+CLIENTRECEIVE", cmd_clientreceive },
    { "+CLIENTCONNECTED", cmd_clientconnected },
    { "+CLIENTSTATUS", cmd_clientstatus },
    { "+CLIENTCLOSE", cmd_clientclose },
    { "+FLUSH", cmd_flush },
    { "+REMOTEIP", cmd_remoteip },
    { "+REMOTEPORT", cmd_remoteport },
    { "+IPCLIENT", cmd_ipclient },
    { "+PEEK", cmd_peek },
    /* TLS shares TCP client handlers except SSLBEGINCLIENT, SETCAROOT, SETECCSLOT and SSLPEEK. */
    { "+SSLBEGINCLIENT", cmd_sslbeginclient },
    { "+SETCAROOT", cmd_setcaroot },
    { "+SETECCSLOT", cmd_seteccslot },
    { "+SSLCLIENTSTATE", cmd_clientstate },
    { "+SSLCLIENTCONNECTIP", cmd_clientconnectip },
    { "+SSLCLIENTCONNECTNAME", cmd_clientconnectname },
    { "+SSLCLIENTCONNECT", cmd_clientconnect },
    { "+SSLCLIENTSEND", cmd_clientsend },
    { "+SSLAVAILABLE", cmd_available },
    { "+SSLCLIENTRECEIVE", cmd_clientreceive },
    { "+SSLCLIENTCONNECTED", cmd_clientconnected },
    { "+SSLCLIENTSTATUS", cmd_clientstatus },
    { "+SSLCLIENTCLOSE", cmd_clientclose },
    { "+SSLCLIENTFLUSH", cmd_flush },
    { "+SSLREMOTEIP", cmd_remoteip },
    { "+SSLREMOTEPORT", cmd_remoteport },
    { "+SSLIPCLIENT", cmd_ipclient },
    { "+SSLCLIENTSETCERT", cmd_sslclientsetcert },
    { "+SSLCLIENTSETPKEY", cmd_sslclientsetpkey },
    { "+SSLPEEK", cmd_peek },
    { "+BEGINSERVER", cmd_beginserver },
    { "+SERVERAVAILABLE", cmd_serveravailable },
    { "+SERVERACCEPT", cmd_serveraccept },
    { "+SERVERWRITE", cmd_serverwrite },
    { "+SERVEREND", cmd_serverend },
    { "+UDPBEGIN", cmd_udpbegin },
    { "+UDPBEGINMULTI", cmd_udpbeginmulti },
    { "+UDPBEGINPACKET", cmd_udpbeginpacket },
    { "+BEGINPACKETMULTI", cmd_beginpacketmulti },
    { "+UDPBEGINPACKETIP", cmd_udpbeginpacketip },
    { "+UDPBEGINPACKETADD", cmd_udpbeginpacketadd },
    { "+UDPWRITE", cmd_udpwrite },
    { "+UDPENDPACKET", cmd_udpendpacket },
    { "+UDPPARSE", cmd_udpparse },
    { "+UDPAVAILABLE", cmd_udpavailable },
    { "+UDPREAD", cmd_udpread },
    { "+UDPPEEK", cmd_udppeek },
    { "+UDPFLUSH", cmd_udpflush },
    { "+UDPREMOTEIP", cmd_udpremoteip },
    { "+UDPREMOTEPORT", cmd_udpremoteport },
    { "+UDPSTOP", cmd_udpstop },
    { "+RESTARTBOOTLOADER", cmd_restartbootloader },
};

/**
 * @brief Accepted request forms: AT, AT+NAME, AT+NAME? and AT+NAME=args. Ignore lines not
 * starting with AT.
 */
static void dispatch(Stream &io, char *s)
{
    if (strncmp(s, "AT", 2) != 0) {
        return;
    }
    s += 2;
    if (*s == '\0') {
        io.print("OK\r\n");
        return;
    }
    if (*s != '+') {
        return;
    }
    char *end = s + 1 + strcspn(s + 1, "=?");
    char op = *end;
    *end = '\0';
    char *args = op == '\0' ? end : end + 1;
    for (const auto &c : commands) {
        if (strcmp(c.name, s) == 0) {
            c.handler(io, s, op, args);
            return;
        }
    }
    reply_error(io, s);
}

/**
 * @brief Consume available AT input and dispatch complete bounded lines without waiting for a
 * new line.
 */
void at_serve(Stream &io)
{
    while (io.available() > 0) {
        int c = io.read();
        if (c == '\n') {
            if (!line_overflow) {
                if (line_len > 0 && line[line_len - 1] == '\r') {
                    line_len--;
                }
                line[line_len] = '\0';
                dispatch(io, line);
            }
            line_len = 0;
            line_overflow = false;
        } else if (line_len < LINE_MAX) {
            line[line_len++] = (char)c;
        } else {
            line_overflow = true; /* Discard an overlong line completely. */
        }
    }
}
