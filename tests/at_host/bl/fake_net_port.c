/*
 * 主机测试用的 net_port（接口见平台 libraries/WiFi/src/net_port.h），建在 POSIX socket 上。
 * 域名只认 FAKE_HOST；ping 只有 FAKE_IP 应答。在 wifi_port_init 之前调用任何函数都中止进程，
 * 让测试暴露违反平台约定的调用。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fake_ports.h"
#include "net_port.h"

static uint32_t close_counts[1024]; /* 按 fd 计 */

static void require_wifi(const char *fn)
{
    if (!fake_wifi_up) {
        fprintf(stderr, "FATAL %s called before wifi_port_init\n", fn);
        abort();
    }
}

int net_port_resolve(const char *host, uint32_t *addr)
{
    struct in_addr a;

    require_wifi(__func__);
    if (host != NULL && inet_pton(AF_INET, host, &a) == 1) {
        *addr = a.s_addr;
        return 0;
    }
    if (host != NULL && strcmp(host, FAKE_HOST) == 0) {
        *addr = htonl(FAKE_IP);
        return 0;
    }
    return -1;
}

int net_port_tcp_connect(uint32_t addr, uint16_t port, uint32_t timeout_ms)
{
    require_wifi(__func__);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = addr;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        struct pollfd p = { .fd = sock, .events = POLLOUT };
        int err = 0;
        socklen_t len = sizeof(err);
        if (errno != EINPROGRESS || poll(&p, 1, (int)timeout_ms) <= 0 ||
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            close(sock);
            return -1;
        }
    }
    fcntl(sock, F_SETFL, flags);
    return sock;
}

int net_port_send(int sock, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t done = 0;

    require_wifi(__func__);
    while (done < len) {
        struct pollfd p = { .fd = sock, .events = POLLOUT };
        if (poll(&p, 1, (int)timeout_ms) <= 0) {
            break;
        }
        ssize_t n = send(sock, buf + done, len - done, MSG_NOSIGNAL);
        if (n <= 0) {
            break;
        }
        done += (size_t)n;
    }
    return done > 0 || len == 0 ? (int)done : -1;
}

int net_port_available(int sock)
{
    int n = 0;
    uint8_t c;

    require_wifi(__func__);
    if (ioctl(sock, FIONREAD, &n) == 0 && n > 0) {
        return n;
    }
    ssize_t r = recv(sock, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    return r > 0 ? 1 : -1;
}

int net_port_recv(int sock, uint8_t *buf, size_t len, bool peek)
{
    require_wifi(__func__);
    ssize_t n = recv(sock, buf, peek ? 1 : len, MSG_DONTWAIT | (peek ? MSG_PEEK : 0));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    return n > 0 ? (int)n : -1;
}

bool net_port_connected(int sock)
{
    return net_port_available(sock) >= 0;
}

int net_port_peer(int sock, uint32_t *addr, uint16_t *port)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);

    require_wifi(__func__);
    if (getpeername(sock, (struct sockaddr *)&sa, &len) != 0) {
        return -1;
    }
    *addr = sa.sin_addr.s_addr;
    *port = ntohs(sa.sin_port);
    return 0;
}

int net_port_local(int sock, uint32_t *addr, uint16_t *port)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);

    require_wifi(__func__);
    if (getsockname(sock, (struct sockaddr *)&sa, &len) != 0) {
        return -1;
    }
    *addr = sa.sin_addr.s_addr;
    *port = ntohs(sa.sin_port);
    return 0;
}

void net_port_close(int sock)
{
    require_wifi(__func__);
    if (sock >= 0 && sock < 1024) {
        close_counts[sock]++;
    }
    close(sock);
}

uint32_t net_port_close_count(int sock)
{
    return sock >= 0 && sock < 1024 ? close_counts[sock] : 0;
}

int net_port_tcp_listen(uint16_t port)
{
    require_wifi(__func__);
    int one = 1;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(sock, 4) != 0) {
        close(sock);
        return -1;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    return sock;
}

int net_port_tcp_accept(int listen_sock)
{
    require_wifi(__func__);
    int sock = accept(listen_sock, NULL, NULL);
    if (sock >= 0) {
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) & ~O_NONBLOCK);
    }
    return sock;
}

int net_port_udp_open(uint32_t addr, uint16_t port)
{
    require_wifi(__func__);
    int one = 1;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = addr;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        bind(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

/* IGMP is a fake here. This harness does not validate radio group membership. */
int net_port_udp_join(int sock, uint32_t group)
{
    require_wifi(__func__);
    (void)sock;
    /* The fixed lwIP rejects explicit membership in its implicit all-hosts group. */
    return ntohl(group) == 0xe0000001u ? -1 : 0;
}

int net_port_udp_send(int sock, uint32_t addr, uint16_t port, const uint8_t *buf, size_t len)
{
    require_wifi(__func__);
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = addr;
    return sendto(sock, buf, len, 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)len ? 0 : -1;
}

int net_port_udp_recv(int sock, uint8_t *buf, size_t len, uint32_t *addr, uint16_t *port)
{
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    require_wifi(__func__);
    ssize_t n = recvfrom(sock, buf, len, MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);
    if (n < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
    *addr = from.sin_addr.s_addr;
    *port = ntohs(from.sin_port);
    return (int)n;
}

int net_port_ping(uint32_t addr, uint8_t ttl, uint16_t seq, uint32_t timeout_ms)
{
    (void)ttl;
    (void)seq;
    require_wifi(__func__);
    if (addr == htonl(FAKE_IP)) {
        return 1;
    }
    usleep(timeout_ms * 1000u);
    return -2;
}
