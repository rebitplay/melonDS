/*
    Rebit WFC LAN backend, adapted from the melonDS indirect LAN interface
    (frontend/qt_sdl/LAN_Socket.cpp). libslirp turns the DS's Ethernet frames
    into socket calls, and those go through the non-blocking Rebit WFC socket
    layer to the Signal proxy.
*/

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "LAN_Wfc.h"
#include "../../Savestate.h"
#include "../../FIFO.h"
#include "rebit_wfc_sock.h"

#include <arpa/inet.h>
#include <libslirp.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace LAN_Wfc
{

// Each session gets its own /24 in 10.64.0.0/10 from the proxy (see
// rebit_wfc_lan_subnet); index 0 is the fallback when the proxy has no LAN.
const u32 kLanBase  = 0x0A400000;
const u32 kServerIP = 0x01;
const u32 kDNSIP    = 0x02;
const u32 kClientIP = 0x10;

// Kaeru WFC answers Nintendo WFC hostnames; libslirp forwards the DS's DNS
// queries to the resolver named in /etc/resolv.conf.
const char* const kKaeruDNS = "178.62.43.212";

FIFO<u32, (0x8000 >> 2)> RXBuffer;

Slirp* Ctx = nullptr;

void RXEnqueue(const void* buf, int len)
{
    int alignedlen = (len + 3) & ~3;
    int totallen = alignedlen + 4;

    if (!RXBuffer.CanFit(totallen >> 2))
        return;

    u32 header = (alignedlen & 0xFFFF) | (len << 16);
    RXBuffer.Write(header);
    for (int i = 0; i < alignedlen; i += 4)
        RXBuffer.Write(((u32*)buf)[i>>2]);
}

ssize_t SlirpCbSendPacket(const void* buf, size_t len, void* opaque)
{
    if (len > 2048)
        return 0;

    RXEnqueue(buf, len);
    return len;
}

void SlirpCbGuestError(const char* msg, void* opaque)
{
    printf("[rebit-wfc] slirp guest error: %s\n", msg);
}

int64_t SlirpCbClockGetNS(void* opaque)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void* SlirpCbTimerNew(SlirpTimerCb cb, void* cb_opaque, void* opaque)
{
    return nullptr;
}

void SlirpCbTimerFree(void* timer, void* opaque)
{
}

void SlirpCbTimerMod(void* timer, int64_t expire_time, void* opaque)
{
}

void SlirpCbRegisterPollFD(int fd, void* opaque)
{
}

void SlirpCbUnregisterPollFD(int fd, void* opaque)
{
}

void SlirpCbNotify(void* opaque)
{
}

SlirpCb cb =
{
    .send_packet = SlirpCbSendPacket,
    .guest_error = SlirpCbGuestError,
    .clock_get_ns = SlirpCbClockGetNS,
    .timer_new = SlirpCbTimerNew,
    .timer_free = SlirpCbTimerFree,
    .timer_mod = SlirpCbTimerMod,
    .register_poll_fd = SlirpCbRegisterPollFD,
    .unregister_poll_fd = SlirpCbUnregisterPollFD,
    .notify = SlirpCbNotify
};

static void WriteResolvConf()
{
    FILE* f = fopen("/etc/resolv.conf", "w");
    if (!f)
    {
#ifdef __EMSCRIPTEN__
        EM_ASM({ try { FS.mkdir('/etc'); } catch (e) {} });
#endif
        f = fopen("/etc/resolv.conf", "w");
    }
    if (!f) return;
    fprintf(f, "nameserver %s\n", kKaeruDNS);
    fclose(f);
}

bool Started = false;

bool Init()
{
    if (Started) return true;
    if (!rebit_wfc_start())
        return false;

    WriteResolvConf();
    Started = true;
    return true;
}

// slirp starts once the proxy has answered with this session's subnet. Until
// then the DS's first DHCP requests go unanswered and it simply retries.
bool StartSlirp()
{
    if (Ctx) return true;
    if (!Started) return false;

    int subnet = rebit_wfc_lan_subnet();
    if (subnet < 0) return false;
    u32 network = kLanBase + ((u32)subnet << 8);

    SlirpConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 1;

    cfg.in_enabled = true;
    *(u32*)&cfg.vnetwork = htonl(network);
    *(u32*)&cfg.vnetmask = htonl(0xFFFFFF00);
    *(u32*)&cfg.vhost = htonl(network | kServerIP);
    cfg.vhostname = "melonServer";
    *(u32*)&cfg.vdhcp_start = htonl(network | kClientIP);
    *(u32*)&cfg.vnameserver = htonl(network | kDNSIP);
    // Traffic to the virtual gateway must not reach the proxy host itself.
    cfg.disable_host_loopback = true;

    Ctx = slirp_new(&cfg, &cb, nullptr);
    return Ctx != nullptr;
}

void DeInit()
{
    if (Ctx)
    {
        slirp_cleanup(Ctx);
        Ctx = nullptr;
    }
    if (Started)
        rebit_wfc_stop();
    Started = false;
    RXBuffer.Clear();
}

int SendPacket(u8* data, int len)
{
    if (!StartSlirp()) return len;
    if (len > 2048) return 0;

    slirp_input(Ctx, data, len);
    return len;
}

const int PollListMax = 64;
struct pollfd PollList[PollListMax];
int PollListSize;

int SlirpCbAddPoll(int fd, int events, void* opaque)
{
    if (PollListSize >= PollListMax)
        return -1;

    int idx = PollListSize++;

    u16 evt = 0;
    if (events & SLIRP_POLL_IN) evt |= POLLIN;
    if (events & SLIRP_POLL_OUT) evt |= POLLOUT;
    if (events & SLIRP_POLL_PRI) evt |= POLLPRI;
    if (events & SLIRP_POLL_ERR) evt |= POLLERR;
    if (events & SLIRP_POLL_HUP) evt |= POLLHUP;

    PollList[idx].fd = fd;
    PollList[idx].events = evt;
    PollList[idx].revents = 0;

    return idx;
}

int SlirpCbGetREvents(int idx, void* opaque)
{
    if (idx < 0 || idx >= PollListSize)
        return 0;

    u16 evt = PollList[idx].revents;
    int ret = 0;

    if (evt & POLLIN) ret |= SLIRP_POLL_IN;
    if (evt & POLLOUT) ret |= SLIRP_POLL_OUT;
    if (evt & POLLPRI) ret |= SLIRP_POLL_PRI;
    if (evt & POLLERR) ret |= SLIRP_POLL_ERR;
    if (evt & POLLHUP) ret |= SLIRP_POLL_HUP;

    return ret;
}

int RecvPacket(u8* data)
{
    if (!StartSlirp()) return 0;

    // Only service slirp when the queue is empty, so the DS drains everything
    // already received before more socket work happens.
    if (RXBuffer.IsEmpty())
    {
        u32 timeout = 0;
        PollListSize = 0;
        slirp_pollfds_fill(Ctx, &timeout, SlirpCbAddPoll, nullptr);
        int res = rebit_wfc_poll(PollList, PollListSize, 0);
        slirp_pollfds_poll(Ctx, res < 0, SlirpCbGetREvents, nullptr);
    }

    if (RXBuffer.IsEmpty())
        return 0;

    u32 header = RXBuffer.Read();
    u32 len = header & 0xFFFF;

    for (u32 i = 0; i < len; i += 4)
        ((u32*)data)[i>>2] = RXBuffer.Read();

    return header >> 16;
}

}
