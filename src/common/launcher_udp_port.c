#include "launcher_udp_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET lng_sock;
#define LNG_SOCK_INVALID INVALID_SOCKET
static void lng_net_startup(void) {
    static int started;
    WSADATA wsa;
    if (!started) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
            started = 1;
    }
}
static void lng_sock_close(lng_sock *s) {
    if (s && *s != LNG_SOCK_INVALID) {
        closesocket(*s);
        *s = LNG_SOCK_INVALID;
    }
}
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int lng_sock;
#define LNG_SOCK_INVALID (-1)
static void lng_net_startup(void) {}
static void lng_sock_close(lng_sock *s) {
    if (s && *s != LNG_SOCK_INVALID) {
        close(*s);
        *s = LNG_SOCK_INVALID;
    }
}
#endif

int launcher_udp_port_available(int port) {
    lng_sock sock;
    struct sockaddr_in addr;
    int ok;

    if (port <= 0 || port > 65535)
        return 0;
    lng_net_startup();
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == LNG_SOCK_INVALID)
        return 0;
    /* No SO_REUSEADDR — busy ports must fail the probe. */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    ok = (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) ? 1 : 0;
    lng_sock_close(&sock);
    return ok;
}

int launcher_udp_find_free_port(int preferred, int span) {
    int i;

    if (preferred <= 0 || preferred > 65535)
        preferred = 7777;
    if (span <= 0)
        span = 32;
    for (i = 0; i < span; ++i) {
        const int port = preferred + i;
        if (port > 65535)
            break;
        if (launcher_udp_port_available(port))
            return port;
    }
    return -1;
}

int launcher_udp_preferred_local_ipv4(char *out, size_t cap) {
    lng_sock sock;
    struct sockaddr_in dest;
    struct sockaddr_in local;
#ifdef _WIN32
    int local_len;
#else
    socklen_t local_len;
    struct ifaddrs *ifap = NULL;
    struct ifaddrs *ifa;
#endif
    char buf[INET_ADDRSTRLEN];

    if (!out || cap < 8U)
        return 0;
    out[0] = '\0';

#ifndef _WIN32
    /* Prefer a real interface address when getifaddrs is available. */
    if (getifaddrs(&ifap) == 0) {
        for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
            struct sockaddr_in *sa;
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (!ifa->ifa_name || strncmp(ifa->ifa_name, "lo", 2) == 0)
                continue;
            sa = (struct sockaddr_in *)ifa->ifa_addr;
            if (sa->sin_addr.s_addr == htonl(INADDR_LOOPBACK) ||
                sa->sin_addr.s_addr == 0)
                continue;
            if (!inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)))
                continue;
            snprintf(out, cap, "%s", buf);
            break;
        }
        freeifaddrs(ifap);
        if (out[0])
            return 1;
    }
#endif

    /* UDP connect trick: kernel picks the outbound interface source IP. */
    lng_net_startup();
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == LNG_SOCK_INVALID)
        return 0;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(9); /* discard; no packets need to be sent */
    dest.sin_addr.s_addr = htonl(0x08080808u); /* 8.8.8.8 */
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        lng_sock_close(&sock);
        return 0;
    }
    memset(&local, 0, sizeof(local));
#ifdef _WIN32
    local_len = (int)sizeof(local);
#else
    local_len = (socklen_t)sizeof(local);
#endif
    if (getsockname(sock, (struct sockaddr *)&local, &local_len) != 0) {
        lng_sock_close(&sock);
        return 0;
    }
    lng_sock_close(&sock);
    if (local.sin_addr.s_addr == 0 ||
        local.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
        return 0;
    if (!inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)))
        return 0;
    snprintf(out, cap, "%s", buf);
    return out[0] != '\0';
}

static int prepare_bind(char *out, size_t cap, int preferred_port) {
    char host[64];
    const int port = launcher_udp_find_free_port(preferred_port, 32);
    int n;

    if (!out || cap < 16U || port < 0)
        return -1;
    if (!launcher_udp_preferred_local_ipv4(host, sizeof(host)))
        snprintf(host, sizeof(host), "0.0.0.0");
    n = snprintf(out, cap, "%s:%d", host, port);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int launcher_udp_prepare_guest_bind(char *out, size_t cap) {
    return prepare_bind(out, cap, /*preferred=*/7778);
}

int launcher_udp_prepare_host_bind(char *out, size_t cap) {
    return prepare_bind(out, cap, /*preferred=*/7777);
}

int launcher_endpoint_port(const char *endpoint) {
    const char *colon;
    int port;

    if (!endpoint || !endpoint[0])
        return 7777;
    colon = strrchr(endpoint, ':');
    if (!colon || !colon[1])
        return 7777;
    port = atoi(colon + 1);
    return (port > 0 && port <= 65535) ? port : 7777;
}

int launcher_endpoint_set_port(char *endpoint, size_t cap, int port) {
    char host[64];
    const char *colon;
    size_t host_len;
    int n;

    if (!endpoint || cap < 4U || port <= 0 || port > 65535)
        return -1;
    colon = strrchr(endpoint, ':');
    if (colon) {
        host_len = (size_t)(colon - endpoint);
        if (host_len >= sizeof(host))
            host_len = sizeof(host) - 1U;
        memcpy(host, endpoint, host_len);
        host[host_len] = '\0';
    } else if (endpoint[0]) {
        snprintf(host, sizeof(host), "%s", endpoint);
    } else {
        host[0] = '\0';
    }
    if (!host[0]) {
        if (!launcher_udp_preferred_local_ipv4(host, sizeof(host)))
            snprintf(host, sizeof(host), "0.0.0.0");
    }
    n = snprintf(endpoint, cap, "%s:%d", host, port);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}
