#ifndef LAUNCHER_UDP_PORT_H
#define LAUNCHER_UDP_PORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Universal UDP port policy used by recomp-ui before create()/join():
 *   Host create LAN/Direct IP — exact UI port required; fail when busy
 *   Host create online        — prefer concrete local IPv4 + 7777..(+span)
 *   Guest join (any path)     — prefer concrete local IPv4 + 7778..(+span)
 *
 * Advertising a concrete LAN IPv4 (not 0.0.0.0) lets MotK keep that host in
 * rewrite_endpoint instead of substituting the WebSocket TCP peer IP (often
 * wrong on hairpinned LAN paths). UDP listen may still bind INADDR_ANY.
 *
 * Self-contained (no recomp-net link). Exclusive bind probes omit SO_REUSEADDR
 * so a second bind on the same port is detected. */

/* 1 = free on INADDR_ANY, 0 = busy / invalid. */
int launcher_udp_port_available(int port);

/* Returns a free port, or -1. span <= 0 selects 32 (MotK contract). */
int launcher_udp_find_free_port(int preferred, int span);

/* First non-loopback IPv4, or empty on failure. Returns 1 if out filled. */
int launcher_udp_preferred_local_ipv4(char *out, size_t cap);

/* Fill guest_bind with "<local-ipv4|0.0.0.0>:<free>" (prefer 7778). Returns 0. */
int launcher_udp_prepare_guest_bind(char *out, size_t cap);

/* Fill host online bind similarly (prefer 7777). Returns 0. */
int launcher_udp_prepare_host_bind(char *out, size_t cap);

/* Parse "host:port"; returns 7777 when missing/invalid. */
int launcher_endpoint_port(const char *endpoint);

/* Rewrite port in place (capacity includes NUL). Returns 0 on success. */
int launcher_endpoint_set_port(char *endpoint, size_t cap, int port);

#ifdef __cplusplus
}
#endif

#endif /* LAUNCHER_UDP_PORT_H */
