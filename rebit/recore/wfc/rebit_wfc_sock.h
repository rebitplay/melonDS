/*
 * Rebit WFC socket layer.
 *
 * libslirp runs inside the WebAssembly core and talks to the internet through
 * the Signal WFC proxy. Every socket call it makes is answered from local state
 * immediately: data goes out as fire-and-forget WebSocket messages and replies
 * arrive as pushes between frames. Nothing here ever waits on the network, so
 * emulation speed does not depend on the player's round trip to the proxy.
 *
 * This header is force-included into the libslirp sources so their socket
 * calls resolve to the rebit_wfc_* functions below.
 */
#ifndef REBIT_WFC_SOCK_H
#define REBIT_WFC_SOCK_H

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

int rebit_wfc_socket(int domain, int type, int protocol);
int rebit_wfc_connect(int fd, const struct sockaddr *address, socklen_t address_len);
int rebit_wfc_bind(int fd, const struct sockaddr *address, socklen_t address_len);
int rebit_wfc_listen(int fd, int backlog);
int rebit_wfc_accept(int fd, struct sockaddr *address, socklen_t *address_len);
ssize_t rebit_wfc_send(int fd, const void *buffer, size_t length, int flags);
ssize_t rebit_wfc_recv(int fd, void *buffer, size_t length, int flags);
ssize_t rebit_wfc_sendto(int fd, const void *buffer, size_t length, int flags,
                         const struct sockaddr *address, socklen_t address_len);
ssize_t rebit_wfc_recvfrom(int fd, void *buffer, size_t length, int flags,
                           struct sockaddr *address, socklen_t *address_len);
int rebit_wfc_getsockopt(int fd, int level, int name, void *value, socklen_t *length);
int rebit_wfc_setsockopt(int fd, int level, int name, const void *value, socklen_t length);
int rebit_wfc_getsockname(int fd, struct sockaddr *address, socklen_t *address_len);
int rebit_wfc_getpeername(int fd, struct sockaddr *address, socklen_t *address_len);
int rebit_wfc_shutdown(int fd, int how);
int rebit_wfc_close(int fd);
int rebit_wfc_fcntl(int fd, int command, ...);
int rebit_wfc_ioctl(int fd, unsigned long request, ...);
int rebit_wfc_poll(struct pollfd *fds, nfds_t count, int timeout);
int rebit_wfc_select(int count, void *read_fds, void *write_fds, void *error_fds, void *timeout);

/* Opens the proxy WebSocket. Returns 0 when no proxy URL is configured. */
int rebit_wfc_start(void);
void rebit_wfc_stop(void);

/*
 * The proxy gives each session its own /24 in 10.64.0.0/10 so players behind
 * the same proxy reach each other by their local addresses, the way GameSpy
 * pairs consoles behind one NAT. Returns -1 while the answer is pending, 0 when
 * the proxy has no LAN (use 10.64.0.0/24), otherwise the subnet index.
 */
int rebit_wfc_lan_subnet(void);
/* Tells the proxy which guest UDP port a slirp socket carries. */
void rebit_wfc_lan_port(int fd, int port);

#ifdef __cplusplus
}
#endif

#ifdef REBIT_WFC_REDIRECT
#define REBIT_WFC_UDP_ATTACHED(so) rebit_wfc_lan_port((so)->s, ntohs((so)->so_lport))
#define socket(...) rebit_wfc_socket(__VA_ARGS__)
#define connect(...) rebit_wfc_connect(__VA_ARGS__)
#define bind(...) rebit_wfc_bind(__VA_ARGS__)
#define listen(...) rebit_wfc_listen(__VA_ARGS__)
#define accept(...) rebit_wfc_accept(__VA_ARGS__)
#define send(...) rebit_wfc_send(__VA_ARGS__)
#define recv(...) rebit_wfc_recv(__VA_ARGS__)
#define sendto(...) rebit_wfc_sendto(__VA_ARGS__)
#define recvfrom(...) rebit_wfc_recvfrom(__VA_ARGS__)
#define getsockopt(...) rebit_wfc_getsockopt(__VA_ARGS__)
#define setsockopt(...) rebit_wfc_setsockopt(__VA_ARGS__)
#define getsockname(...) rebit_wfc_getsockname(__VA_ARGS__)
#define getpeername(...) rebit_wfc_getpeername(__VA_ARGS__)
#define shutdown(...) rebit_wfc_shutdown(__VA_ARGS__)
#define close(...) rebit_wfc_close(__VA_ARGS__)
#define fcntl(...) rebit_wfc_fcntl(__VA_ARGS__)
#define ioctl(...) rebit_wfc_ioctl(__VA_ARGS__)
#define select(...) rebit_wfc_select(__VA_ARGS__)
#endif

#endif
