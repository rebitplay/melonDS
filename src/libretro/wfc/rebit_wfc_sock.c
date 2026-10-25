#include "rebit_wfc_sock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/*
 * Local socket table. The proxy assigns its own ids asynchronously; the JS
 * side maps our fds to them and holds sends until the id arrives, so the core
 * never waits for a reply.
 */
enum {
    WFC_FD_BASE = 1000,
    WFC_FD_COUNT = 256,
};

typedef struct {
    int used;
    int type; /* SOCK_STREAM or SOCK_DGRAM */
    int flags; /* O_NONBLOCK etc. as set through fcntl */
    uint32_t local_port;
} WfcSocket;

static WfcSocket wfc_sockets[WFC_FD_COUNT];

static WfcSocket *wfc_get(int fd)
{
    if (fd < WFC_FD_BASE || fd >= WFC_FD_BASE + WFC_FD_COUNT)
        return NULL;
    WfcSocket *socket = &wfc_sockets[fd - WFC_FD_BASE];
    return socket->used ? socket : NULL;
}

/* The proxy reports Linux errno values; the WebAssembly libc uses its own. */
static int wfc_errno_from_linux(int code)
{
    switch (code)
    {
    case 1: return EPERM;
    case 5: return EIO;
    case 9: return EBADF;
    case 11: return EAGAIN;
    case 12: return ENOMEM;
    case 13: return EACCES;
    case 22: return EINVAL;
    case 32: return EPIPE;
    case 38: return ENOSYS;
    case 93: return EPROTONOSUPPORT;
    case 95: return EOPNOTSUPP;
    case 97: return EAFNOSUPPORT;
    case 101: return ENETUNREACH;
    case 103: return ECONNABORTED;
    case 104: return ECONNRESET;
    case 107: return ENOTCONN;
    case 110: return ETIMEDOUT;
    case 111: return ECONNREFUSED;
    case 113: return EHOSTUNREACH;
    case 115: return EINPROGRESS;
    default: return EIO;
    }
}

static int wfc_fail(int linux_errno)
{
    errno = wfc_errno_from_linux(linux_errno);
    return -1;
}

#ifdef __EMSCRIPTEN__

/*
 * Transport state lives in JavaScript (Module.rebitWfc). Every function here
 * returns at once: results >= 0 are values, negative results are -Linux errno.
 */
EM_JS(int, wfc_js_start, (void), {
    const url = Module["rebitWfcSocketProxy"];
    if (typeof url !== "string" || url.length === 0) return 0;
    if (Module["rebitWfc"]) return 1;

    const EAGAIN = 11, ENOTCONN = 107, ECONNRESET = 104, EINPROGRESS = 115, ENETUNREACH = 101;
    const MSG_SOCKET = 1, MSG_SHUTDOWN = 3, MSG_CONNECT = 5, MSG_ENABLE_PUSH = 22, MSG_SENDTO_FAST = 23, MSG_SEND_FAST = 24, MSG_LAN_JOIN = 25, MSG_LAN_PORT = 26;
    const LAN_ANSWER_MS = 5000;
    const PUSH_DATA = 1, PUSH_CLOSED = 2, PUSH_ERROR = 3;
    const MAX_BUFFERED = 4 * 1024 * 1024;

    const wfc = {
        ws: null,
        open: false,
        down: false,
        waiting: [],
        nextCallId: 1,
        calls: new Map(),
        sockets: new Map(),
        bySid: new Map(),
        lanSubnet: -1,
        lanAskedAt: 0,
        stats: { framesOut: 0, framesIn: 0, bytesOut: 0, bytesIn: 0, sockets: 0, connects: 0, connectFailures: 0, maxConnectMs: 0 },
    };

    const writeSockaddr = (view, offset, ip, port) => {
        view.setUint16(offset, 2, true);
        view.setUint16(offset + 2, port, false);
        view.setUint32(offset + 4, ip, false);
    };

    wfc.transmit = (fn, size, fill, reply) => {
        const frame = new ArrayBuffer(8 + size);
        const view = new DataView(frame);
        const callId = wfc.nextCallId++;
        if (wfc.nextCallId > 0x7fffffff) wfc.nextCallId = 1;
        view.setInt32(0, callId, true);
        view.setInt32(4, fn, true);
        fill(view, new Uint8Array(frame));
        if (reply) wfc.calls.set(callId, reply);
        wfc.stats.framesOut++;
        wfc.stats.bytesOut += frame.byteLength;
        if (wfc.open) wfc.ws.send(frame);
        else wfc.waiting.push(frame);
    };

    wfc.withSid = (socket, action) => {
        if (socket.sid !== null) action(socket.sid);
        else socket.outbox.push(action);
    };

    wfc.fail = (socket, error) => {
        if (socket.state === "connecting") wfc.stats.connectFailures++;
        socket.state = "error";
        socket.error = error;
    };

    wfc.open_socket = (fd, type) => {
        const socket = { fd, type, sid: null, state: type === 2 ? "ready" : "new", error: 0, queue: [], queued: 0, closed: false, outbox: [], released: false, connectStarted: 0 };
        wfc.sockets.set(fd, socket);
        wfc.stats.sockets++;
        if (wfc.down) {
            wfc.fail(socket, ENETUNREACH);
            return;
        }
        wfc.transmit(MSG_SOCKET, 12, (view) => {
            view.setInt32(8, 2, true);
            view.setInt32(12, type, true);
            view.setInt32(16, 0, true);
        }, (ret, error) => {
            if (socket.released) {
                if (ret >= 0) wfc.transmit(MSG_SHUTDOWN, 8, (view) => { view.setInt32(8, ret, true); view.setInt32(12, 2, true); });
                return;
            }
            if (ret < 0) {
                wfc.fail(socket, error || ENETUNREACH);
                socket.outbox = [];
                return;
            }
            socket.sid = ret;
            wfc.bySid.set(ret, socket);
            const pending = socket.outbox;
            socket.outbox = [];
            for (const action of pending) action(ret);
        });
    };

    wfc.connect = (fd, ip, port) => {
        const socket = wfc.sockets.get(fd);
        if (!socket) return -9;
        if (socket.state === "error") return -socket.error;
        if (socket.type === 2) {
            wfc.withSid(socket, (sid) => wfc.transmit(MSG_CONNECT, 24, (view) => {
                view.setInt32(8, sid, true);
                view.setInt32(12, 16, true);
                writeSockaddr(view, 16, ip, port);
            }, (ret, error) => { if (ret < 0) wfc.fail(socket, error); }));
            return 0;
        }
        if (socket.state === "connected") return 0;
        if (socket.state === "connecting") return -EINPROGRESS;
        socket.state = "connecting";
        socket.connectStarted = performance.now();
        wfc.stats.connects++;
        wfc.withSid(socket, (sid) => wfc.transmit(MSG_CONNECT, 24, (view) => {
            view.setInt32(8, sid, true);
            view.setInt32(12, 16, true);
            writeSockaddr(view, 16, ip, port);
        }, (ret, error) => {
            if (socket.state !== "connecting") return;
            if (ret < 0) {
                wfc.fail(socket, error || ECONNRESET);
                return;
            }
            socket.state = "connected";
            wfc.stats.maxConnectMs = Math.max(wfc.stats.maxConnectMs, performance.now() - socket.connectStarted);
        }));
        return -EINPROGRESS;
    };

    wfc.send = (fd, pointer, length, ip, port, hasAddress) => {
        const socket = wfc.sockets.get(fd);
        if (!socket) return -9;
        if (socket.state === "error") return -socket.error;
        if (socket.type === 1) {
            if (socket.state === "connecting" || socket.state === "new") return -(socket.state === "new" ? ENOTCONN : EAGAIN);
            if (socket.closed) return -32;
        }
        if (length === 0) return 0;
        if (wfc.ws && wfc.ws.bufferedAmount > MAX_BUFFERED) return -EAGAIN;
        const data = HEAPU8.slice(pointer, pointer + length);
        if (hasAddress) {
            wfc.withSid(socket, (sid) => wfc.transmit(MSG_SENDTO_FAST, 272 + length, (view, bytes) => {
                view.setInt32(8, sid, true);
                view.setUint32(12, length, true);
                view.setInt32(16, 0, true);
                view.setUint32(20, 16, true);
                writeSockaddr(view, 24, ip, port);
                bytes.set(data, 280);
            }));
        } else {
            wfc.withSid(socket, (sid) => wfc.transmit(MSG_SEND_FAST, 12 + length, (view, bytes) => {
                view.setInt32(8, sid, true);
                view.setUint32(12, length, true);
                view.setInt32(16, 0, true);
                bytes.set(data, 20);
            }));
        }
        return length;
    };

    wfc.recv = (fd, pointer, capacity, peek, addressPointer) => {
        const socket = wfc.sockets.get(fd);
        if (!socket) return -9;
        if (socket.queue.length === 0) {
            if (socket.state === "error") return -socket.error;
            if (socket.closed) return 0;
            return -EAGAIN;
        }
        const head = socket.queue[0];
        if (socket.type === 2) {
            const size = Math.min(capacity, head.data.length);
            HEAPU8.set(head.data.subarray(0, size), pointer);
            if (addressPointer) {
                HEAPU32[addressPointer >> 2] = head.ip;
                HEAPU32[(addressPointer >> 2) + 1] = head.port;
            }
            if (!peek) {
                socket.queue.shift();
                socket.queued -= head.data.length;
            }
            return size;
        }
        let copied = 0;
        let index = 0;
        while (copied < capacity && index < socket.queue.length) {
            const chunk = socket.queue[index];
            const size = Math.min(capacity - copied, chunk.data.length);
            HEAPU8.set(chunk.data.subarray(0, size), pointer + copied);
            copied += size;
            if (peek) {
                index++;
                continue;
            }
            if (size === chunk.data.length) socket.queue.shift();
            else chunk.data = chunk.data.subarray(size);
        }
        if (!peek) socket.queued -= copied;
        return copied;
    };

    wfc.lanPort = (fd, port) => {
        const socket = wfc.sockets.get(fd);
        if (!socket || socket.lanPort === port) return;
        socket.lanPort = port;
        wfc.withSid(socket, (sid) => wfc.transmit(MSG_LAN_PORT, 8, (view) => {
            view.setInt32(8, sid, true);
            view.setInt32(12, port, true);
        }));
    };

    // Ask once per connection, for the subnet already in use when reconnecting.
    wfc.joinLan = () => {
        wfc.lanAskedAt = performance.now();
        const wanted = wfc.lanSubnet > 0 ? wfc.lanSubnet : 0;
        wfc.transmit(MSG_LAN_JOIN, 4, (view) => view.setInt32(8, wanted, true), (ret) => {
            if (wfc.lanSubnet < 0) wfc.lanSubnet = ret > 0 ? ret : 0;
        });
    };

    wfc.lan = () => {
        if (wfc.lanSubnet >= 0) return wfc.lanSubnet;
        if (wfc.down || performance.now() - wfc.lanAskedAt > LAN_ANSWER_MS) wfc.lanSubnet = 0;
        return wfc.lanSubnet;
    };

    wfc.pending = (fd) => {
        const socket = wfc.sockets.get(fd);
        if (!socket) return -9;
        if (socket.type === 2) return socket.queue.length ? socket.queue[0].data.length : 0;
        return socket.queued;
    };

    // revents bits: 1 POLLIN, 4 POLLOUT, 8 POLLERR, 16 POLLHUP
    wfc.poll = (fd) => {
        const socket = wfc.sockets.get(fd);
        if (!socket) return 32;
        let events = 0;
        if (socket.queue.length > 0 || socket.closed) events |= 1;
        if (socket.state === "error") events |= 8 | 1;
        if (socket.type === 2 ? socket.state !== "error" : socket.state === "connected" && !socket.closed) events |= 4;
        if (socket.closed && socket.queue.length === 0) events |= 16;
        return events;
    };

    wfc.takeError = (fd) => {
        const socket = wfc.sockets.get(fd);
        if (!socket) return 9;
        if (socket.state !== "error") return 0;
        const error = socket.error;
        return error;
    };

    wfc.release = (fd, how) => {
        const socket = wfc.sockets.get(fd);
        if (!socket) return;
        if (how === 1) {
            if (socket.sid !== null) wfc.transmit(MSG_SHUTDOWN, 8, (view) => { view.setInt32(8, socket.sid, true); view.setInt32(12, 1, true); });
            return;
        }
        wfc.sockets.delete(fd);
        socket.released = true;
        if (socket.sid !== null) {
            wfc.bySid.delete(socket.sid);
            wfc.transmit(MSG_SHUTDOWN, 8, (view) => { view.setInt32(8, socket.sid, true); view.setInt32(12, 2, true); });
        }
    };

    wfc.receive = (frame) => {
        const view = new DataView(frame);
        wfc.stats.framesIn++;
        wfc.stats.bytesIn += frame.byteLength;
        if (frame.byteLength < 12) return;
        const callId = view.getInt32(0, true);
        if (callId !== 0) {
            const reply = wfc.calls.get(callId);
            if (!reply) return;
            wfc.calls.delete(callId);
            reply(view.getInt32(4, true), view.getInt32(8, true));
            return;
        }
        const kind = view.getInt32(4, true);
        const socket = wfc.bySid.get(view.getInt32(8, true));
        if (!socket) return;
        if (kind === PUSH_DATA) {
            const size = view.getInt32(12, true);
            const addressSize = view.getInt32(16, true);
            const data = new Uint8Array(frame.slice(20, 20 + size));
            const entry = { data, ip: 0, port: 0 };
            if (addressSize >= 8) {
                const address = 20 + size;
                entry.port = view.getUint16(address + 2, false);
                entry.ip = view.getUint32(address + 4, false);
            }
            socket.queue.push(entry);
            socket.queued += size;
        } else if (kind === PUSH_CLOSED) {
            socket.closed = true;
        } else if (kind === PUSH_ERROR) {
            if (socket.state !== "connected" || socket.queue.length === 0) wfc.fail(socket, view.getInt32(12, true) || ECONNRESET);
            socket.closed = true;
        }
    };

    wfc.shutdownAll = (error) => {
        wfc.down = true;
        wfc.open = false;
        for (const reply of wfc.calls.values()) reply(-1, error);
        wfc.calls.clear();
        for (const socket of wfc.sockets.values()) {
            if (socket.state !== "error") wfc.fail(socket, error);
            socket.closed = true;
        }
    };

    wfc.connectWs = () => {
        wfc.down = false;
        wfc.open = false;
        wfc.waiting = [];
        wfc.nextCallId = 2;
        wfc.joinLan();
        try {
            wfc.ws = new WebSocket(url);
        } catch (error) {
            console.warn("[rebit-wfc] could not open proxy", error);
            wfc.down = true;
            wfc.retryAt = performance.now() + 5000;
            return;
        }
        const ws = wfc.ws;
        ws.binaryType = "arraybuffer";
        ws.onopen = () => {
            if (ws !== wfc.ws) return;
            wfc.open = true;
            ws.send(new Uint8Array([1, 0, 0, 0, MSG_ENABLE_PUSH, 0, 0, 0]).buffer);
            const waiting = wfc.waiting;
            wfc.waiting = [];
            for (const frame of waiting) ws.send(frame);
        };
        ws.onmessage = (event) => { if (ws === wfc.ws && event.data instanceof ArrayBuffer) wfc.receive(event.data); };
        ws.onclose = () => {
            if (ws !== wfc.ws) return;
            wfc.shutdownAll(ECONNRESET);
            wfc.retryAt = performance.now() + 2000;
        };
        ws.onerror = () => console.warn("[rebit-wfc] proxy connection error");
    };

    // A dropped proxy fails the open sockets (the DS sees a lost connection) and
    // reconnects in the background for the next attempt.
    wfc.available = () => {
        if (!wfc.down) return 1;
        if (performance.now() < (wfc.retryAt || 0)) return 0;
        wfc.sockets.clear();
        wfc.bySid.clear();
        wfc.connectWs();
        return wfc.down ? 0 : 1;
    };

    wfc.connectWs();
    Module["rebitWfc"] = wfc;
    Module["rebitWfcStats"] = () => ({ ...wfc.stats, open: wfc.open, down: wfc.down, sockets: wfc.sockets.size, bufferedAmount: wfc.ws ? wfc.ws.bufferedAmount : 0 });
    return 1;
});

EM_JS(void, wfc_js_stop, (void), {
    const wfc = Module["rebitWfc"];
    if (!wfc) return;
    delete Module["rebitWfc"];
    const ws = wfc.ws;
    wfc.ws = null;
    try { ws && ws.close(); } catch (error) {}
});

EM_JS(int, wfc_js_available, (void), {
    const wfc = Module["rebitWfc"];
    return wfc ? wfc.available() : 0;
});

EM_JS(int, wfc_js_lan_subnet, (void), {
    const wfc = Module["rebitWfc"];
    return wfc ? wfc.lan() : 0;
});
EM_JS(void, wfc_js_lan_port, (int fd, int port), { Module["rebitWfc"].lanPort(fd, port); });
EM_JS(void, wfc_js_open_socket, (int fd, int type), { Module["rebitWfc"].open_socket(fd, type); });
EM_JS(int, wfc_js_connect, (int fd, uint32_t ip, int port), { return Module["rebitWfc"].connect(fd, ip >>> 0, port); });
EM_JS(int, wfc_js_send, (int fd, const void *data, int length, uint32_t ip, int port, int has_address), {
    return Module["rebitWfc"].send(fd, data, length, ip >>> 0, port, has_address);
});
EM_JS(int, wfc_js_recv, (int fd, void *data, int capacity, int peek, uint32_t *address), {
    return Module["rebitWfc"].recv(fd, data, capacity, peek, address);
});
EM_JS(int, wfc_js_pending, (int fd), { return Module["rebitWfc"].pending(fd); });
EM_JS(int, wfc_js_poll, (int fd), { return Module["rebitWfc"].poll(fd); });
EM_JS(int, wfc_js_take_error, (int fd), { return Module["rebitWfc"].takeError(fd); });
EM_JS(void, wfc_js_release, (int fd, int how), { Module["rebitWfc"].release(fd, how); });

#else

static int wfc_js_start(void) { return 0; }
static void wfc_js_stop(void) {}
static int wfc_js_available(void) { return 0; }
static int wfc_js_lan_subnet(void) { return 0; }
static void wfc_js_lan_port(int fd, int port) { (void)fd; (void)port; }
static void wfc_js_open_socket(int fd, int type) { (void)fd; (void)type; }
static int wfc_js_connect(int fd, uint32_t ip, int port) { (void)fd; (void)ip; (void)port; return -101; }
static int wfc_js_send(int fd, const void *data, int length, uint32_t ip, int port, int has_address)
{
    (void)fd; (void)data; (void)length; (void)ip; (void)port; (void)has_address;
    return -101;
}
static int wfc_js_recv(int fd, void *data, int capacity, int peek, uint32_t *address)
{
    (void)fd; (void)data; (void)capacity; (void)peek; (void)address;
    return -11;
}
static int wfc_js_pending(int fd) { (void)fd; return 0; }
static int wfc_js_poll(int fd) { (void)fd; return 8; }
static int wfc_js_take_error(int fd) { (void)fd; return 101; }
static void wfc_js_release(int fd, int how) { (void)fd; (void)how; }

#endif

int rebit_wfc_start(void)
{
    memset(wfc_sockets, 0, sizeof(wfc_sockets));
    return wfc_js_start();
}

void rebit_wfc_stop(void)
{
    for (int i = 0; i < WFC_FD_COUNT; i++)
    {
        if (wfc_sockets[i].used)
            wfc_js_release(WFC_FD_BASE + i, 2);
        wfc_sockets[i].used = 0;
    }
    wfc_js_stop();
}

int rebit_wfc_lan_subnet(void)
{
    return wfc_js_lan_subnet();
}

void rebit_wfc_lan_port(int fd, int port)
{
    if (wfc_get(fd))
        wfc_js_lan_port(fd, port);
}

int rebit_wfc_socket(int domain, int type, int protocol)
{
    (void)protocol;
    int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (domain != AF_INET)
    {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM)
    {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    if (!wfc_js_available())
    {
        errno = ENETUNREACH;
        return -1;
    }
    for (int i = 0; i < WFC_FD_COUNT; i++)
    {
        if (wfc_sockets[i].used)
            continue;
        WfcSocket *socket = &wfc_sockets[i];
        memset(socket, 0, sizeof(*socket));
        socket->used = 1;
        socket->type = base_type;
        socket->flags = (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0;
        socket->local_port = 40000 + i;
        wfc_js_open_socket(WFC_FD_BASE + i, base_type);
        return WFC_FD_BASE + i;
    }
    errno = EMFILE;
    return -1;
}

static int wfc_read_inet(const struct sockaddr *address, socklen_t length, uint32_t *ip, int *port)
{
    if (!address || length < (socklen_t)sizeof(struct sockaddr_in) || address->sa_family != AF_INET)
        return 0;
    const struct sockaddr_in *inet = (const struct sockaddr_in *)address;
    *ip = ntohl(inet->sin_addr.s_addr);
    *port = ntohs(inet->sin_port);
    return 1;
}

static void wfc_write_inet(struct sockaddr *address, socklen_t *length, uint32_t ip, int port)
{
    if (!address || !length)
        return;
    struct sockaddr_in inet;
    memset(&inet, 0, sizeof(inet));
    inet.sin_family = AF_INET;
    inet.sin_port = htons((uint16_t)port);
    inet.sin_addr.s_addr = htonl(ip);
    socklen_t size = *length < (socklen_t)sizeof(inet) ? *length : (socklen_t)sizeof(inet);
    memcpy(address, &inet, size);
    *length = sizeof(inet);
}

int rebit_wfc_connect(int fd, const struct sockaddr *address, socklen_t address_len)
{
    uint32_t ip;
    int port;
    if (!wfc_get(fd))
        return wfc_fail(9);
    if (!wfc_read_inet(address, address_len, &ip, &port))
        return wfc_fail(97);
    int result = wfc_js_connect(fd, ip, port);
    return result < 0 ? wfc_fail(-result) : 0;
}

int rebit_wfc_bind(int fd, const struct sockaddr *address, socklen_t address_len)
{
    (void)address;
    (void)address_len;
    /* The proxy binds UDP sockets on first use; the local address is virtual. */
    return wfc_get(fd) ? 0 : wfc_fail(9);
}

int rebit_wfc_listen(int fd, int backlog)
{
    (void)fd;
    (void)backlog;
    return wfc_fail(95);
}

int rebit_wfc_accept(int fd, struct sockaddr *address, socklen_t *address_len)
{
    (void)fd;
    (void)address;
    (void)address_len;
    return wfc_fail(95);
}

ssize_t rebit_wfc_send(int fd, const void *buffer, size_t length, int flags)
{
    (void)flags;
    if (!wfc_get(fd))
        return wfc_fail(9);
    int result = wfc_js_send(fd, buffer, (int)length, 0, 0, 0);
    return result < 0 ? wfc_fail(-result) : result;
}

ssize_t rebit_wfc_sendto(int fd, const void *buffer, size_t length, int flags,
                         const struct sockaddr *address, socklen_t address_len)
{
    uint32_t ip = 0;
    int port = 0;
    (void)flags;
    if (!wfc_get(fd))
        return wfc_fail(9);
    int has_address = wfc_read_inet(address, address_len, &ip, &port);
    int result = wfc_js_send(fd, buffer, (int)length, ip, port, has_address);
    return result < 0 ? wfc_fail(-result) : result;
}

ssize_t rebit_wfc_recv(int fd, void *buffer, size_t length, int flags)
{
    if (!wfc_get(fd))
        return wfc_fail(9);
    int result = wfc_js_recv(fd, buffer, (int)length, (flags & MSG_PEEK) != 0, NULL);
    return result < 0 ? wfc_fail(-result) : result;
}

ssize_t rebit_wfc_recvfrom(int fd, void *buffer, size_t length, int flags,
                           struct sockaddr *address, socklen_t *address_len)
{
    uint32_t source[2] = {0, 0};
    if (!wfc_get(fd))
        return wfc_fail(9);
    int result = wfc_js_recv(fd, buffer, (int)length, (flags & MSG_PEEK) != 0, source);
    if (result < 0)
        return wfc_fail(-result);
    wfc_write_inet(address, address_len, source[0], (int)source[1]);
    return result;
}

int rebit_wfc_getsockopt(int fd, int level, int name, void *value, socklen_t *length)
{
    WfcSocket *socket = wfc_get(fd);
    if (!socket)
        return wfc_fail(9);
    if (!value || !length || *length < (socklen_t)sizeof(int))
        return wfc_fail(22);
    int result = 0;
    if (level == SOL_SOCKET && name == SO_ERROR)
    {
        int error = wfc_js_take_error(fd);
        result = error ? wfc_errno_from_linux(error) : 0;
    }
    else if (level == SOL_SOCKET && name == SO_TYPE)
        result = socket->type;
    else if (level == SOL_SOCKET && (name == SO_RCVBUF || name == SO_SNDBUF))
        result = 256 * 1024;
    memcpy(value, &result, sizeof(int));
    *length = sizeof(int);
    return 0;
}

int rebit_wfc_setsockopt(int fd, int level, int name, const void *value, socklen_t length)
{
    (void)level;
    (void)name;
    (void)value;
    (void)length;
    /* Reuse, Nagle and buffer sizes are the proxy's business. */
    return wfc_get(fd) ? 0 : wfc_fail(9);
}

int rebit_wfc_getsockname(int fd, struct sockaddr *address, socklen_t *address_len)
{
    WfcSocket *socket = wfc_get(fd);
    if (!socket)
        return wfc_fail(9);
    wfc_write_inet(address, address_len, 0, (int)socket->local_port);
    return 0;
}

int rebit_wfc_getpeername(int fd, struct sockaddr *address, socklen_t *address_len)
{
    (void)address;
    (void)address_len;
    return wfc_get(fd) ? wfc_fail(107) : wfc_fail(9);
}

int rebit_wfc_shutdown(int fd, int how)
{
    if (!wfc_get(fd))
        return wfc_fail(9);
    if (how == SHUT_WR || how == SHUT_RDWR)
        wfc_js_release(fd, 1);
    return 0;
}

int rebit_wfc_close(int fd)
{
    WfcSocket *socket = wfc_get(fd);
    if (!socket)
        return wfc_fail(9);
    wfc_js_release(fd, 2);
    socket->used = 0;
    return 0;
}

int rebit_wfc_fcntl(int fd, int command, ...)
{
    WfcSocket *socket = wfc_get(fd);
    if (!socket)
        return wfc_fail(9);
    va_list args;
    va_start(args, command);
    int value = (command == F_SETFL || command == F_SETFD) ? va_arg(args, int) : 0;
    va_end(args);
    switch (command)
    {
    case F_GETFL:
        return socket->flags | O_RDWR;
    case F_SETFL:
        socket->flags = value;
        return 0;
    case F_GETFD:
        return FD_CLOEXEC;
    case F_SETFD:
        return 0;
    default:
        return wfc_fail(22);
    }
}

int rebit_wfc_ioctl(int fd, unsigned long request, ...)
{
    WfcSocket *socket = wfc_get(fd);
    if (!socket)
        return wfc_fail(9);
    va_list args;
    va_start(args, request);
    int *value = va_arg(args, int *);
    va_end(args);
    if (request == FIONREAD)
    {
        int pending = wfc_js_pending(fd);
        if (pending < 0)
            return wfc_fail(-pending);
        if (value)
            *value = pending;
        return 0;
    }
    if (request == FIONBIO)
    {
        if (value && *value)
            socket->flags |= O_NONBLOCK;
        else
            socket->flags &= ~O_NONBLOCK;
        return 0;
    }
    return wfc_fail(22);
}

int rebit_wfc_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)timeout; /* Never wait: the next frame polls again. */
    int ready = 0;
    for (nfds_t i = 0; i < count; i++)
    {
        fds[i].revents = 0;
        if (!wfc_get(fds[i].fd))
        {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        int state = wfc_js_poll(fds[i].fd);
        short revents = 0;
        if ((state & 1) && (fds[i].events & (POLLIN | POLLRDNORM)))
            revents |= fds[i].events & (POLLIN | POLLRDNORM);
        if ((state & 4) && (fds[i].events & (POLLOUT | POLLWRNORM)))
            revents |= fds[i].events & (POLLOUT | POLLWRNORM);
        if (state & 8)
            revents |= POLLERR;
        if (state & 16)
            revents |= POLLHUP;
        fds[i].revents = revents;
        if (revents)
            ready++;
    }
    return ready;
}

int rebit_wfc_select(int count, void *read_fds, void *write_fds, void *error_fds, void *timeout)
{
    (void)count;
    (void)read_fds;
    (void)write_fds;
    (void)error_fds;
    (void)timeout;
    return wfc_fail(38);
}
