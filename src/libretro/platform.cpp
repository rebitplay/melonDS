#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#if defined(_WIN32) && !defined(_XBOX)
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#define socket_t    SOCKET
#define sockaddr_t  SOCKADDR
#define pcap_dev_name description
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define socket_t    int
#define sockaddr_t  struct sockaddr
#define closesocket close
#define pcap_dev_name name
#endif

#if defined(__HAIKU__)
#include <sys/select.h>
#endif

#ifdef HAVE_PCAP
#include "libui_sdl/LAN_PCap.h"
#include "libui_sdl/LAN_Socket.h"
#endif

#ifdef HAVE_LIBNX
#include <switch/services/bsd.h>
#endif

#ifndef HAVE_WIFI
#define SO_REUSEADDR 0
#define SO_BROADCAST 0
#define socket(domain, type, protocol) NULL
#define bind(sockfd, addr, addrlen) -1
#define setsockopt(sockfd, level, optname, optval, optlen) -1
#define sendto(sockfd, buf, len, flags, dest_addr, addrlen) 0
#define recvfrom(sockfd, buf, len, flags, src_addr, addrlen) 0
#endif

#ifdef HAVE_THREADS
#include <stdlib.h>

#include <rthreads/rthreads.h>
#include <rthreads/rsemaphore.h>
#endif

#include <streams/file_stream.h>
#include <streams/file_stream_transforms.h>
#include <retro_timers.h>

#include "types.h"
#include "utils.h"
#include "Platform.h"

extern char retro_base_directory[4096];

#ifndef INVALID_SOCKET
#define INVALID_SOCKET  (socket_t)-1
#endif

#define NIFI_VER 1

socket_t MPSocket;
sockaddr_t MPSendAddr;
sockaddr_t MPHubAddr;
socklen_t MPHubAddrLen;
bool MPUseHub;
bool MPUseDirect;
u64 MPNetplayRoom;
u16 MPNetplayClientID;
u16 MPDirectBasePort;
u16 MPDirectPeerCount;
u8 PacketBuffer[2048];
u8 NetplayPacketBuffer[4096];

#define NETPLAY_BROADCAST_ID 0xFFFF
#define NETPLAY_FLAG_PRESENCE (1u << 31)
#define NETPLAY_HEADER_LEN 24
#define DIRECT_LAN_DEFAULT_BASE_PORT 7064
#define DIRECT_LAN_DEFAULT_PEER_COUNT 4

static const char* FirstEnv(const char* first, const char* second)
{
   const char* value = getenv(first);
   if (value && value[0])
      return value;
   value = getenv(second);
   if (value && value[0])
      return value;
   return NULL;
}

static u16 ReadLE16(const u8* data)
{
   return (u16)data[0] | ((u16)data[1] << 8);
}

static u32 ReadLE32(const u8* data)
{
   return (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static u64 ReadLE64(const u8* data)
{
   return (u64)ReadLE32(data) | ((u64)ReadLE32(data + 4) << 32);
}

static void WriteLE16(u8* data, u16 value)
{
   data[0] = value & 0xFF;
   data[1] = value >> 8;
}

static void WriteLE32(u8* data, u32 value)
{
   data[0] = value & 0xFF;
   data[1] = (value >> 8) & 0xFF;
   data[2] = (value >> 16) & 0xFF;
   data[3] = value >> 24;
}

static void WriteLE64(u8* data, u64 value)
{
   WriteLE32(data, (u32)value);
   WriteLE32(data + 4, (u32)(value >> 32));
}

static u64 HashNetplayRoom(const char* room)
{
   u64 hash = 14695981039346656037ULL;
   if (!room || !room[0])
      room = "default";

   for (const unsigned char* p = (const unsigned char*)room; *p; p++)
   {
      hash ^= *p;
      hash *= 1099511628211ULL;
   }

   return hash;
}

static bool ParseNetplayClientID(const char* text, u16* id)
{
   if (!text || !text[0])
      return false;

   char* end = NULL;
   unsigned long value = strtoul(text, &end, 10);
   if (*end != '\0' || value >= NETPLAY_BROADCAST_ID)
      return false;

   *id = (u16)value;
   return true;
}

static u16 ParseEnvU16(const char* text, u16 fallback, u16 minValue, u16 maxValue)
{
   if (!text || !text[0])
      return fallback;

   char* end = NULL;
   unsigned long value = strtoul(text, &end, 10);
   if (*end != '\0' || value < minValue || value > maxValue)
      return fallback;

   return (u16)value;
}

static bool ResolveNetplayHub(const char* hub, sockaddr_t* addr, socklen_t* addrLen)
{
   if (!hub || !hub[0])
      return false;

   std::string host;
   std::string port;
   const char* colon = strrchr(hub, ':');
   if (!colon || colon == hub || !colon[1])
      return false;

   host.assign(hub, colon - hub);
   port.assign(colon + 1);

   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_DGRAM;

   struct addrinfo* result = NULL;
   int res = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
   if (res != 0 || !result)
      return false;

   memcpy(addr, result->ai_addr, result->ai_addrlen);
   *addrLen = result->ai_addrlen;
   freeaddrinfo(result);
   return true;
}

static void NetplayEncodePacket(u8* out, u64 room, u16 src, u16 dst, u32 flags, const u8* payload, u32 payloadLen)
{
   out[0] = 'R';
   out[1] = 'N';
   out[2] = 'P';
   out[3] = '1';
   WriteLE64(out + 4, room);
   WriteLE16(out + 12, src);
   WriteLE16(out + 14, dst);
   WriteLE32(out + 16, flags);
   WriteLE32(out + 20, payloadLen);
   if (payloadLen > 0)
      memcpy(out + NETPLAY_HEADER_LEN, payload, payloadLen);
}

static bool NetplayDecodePacket(const u8* packet, int packetLen, u64* room, u16* src, u16* dst, u32* flags, const u8** payload, u32* payloadLen)
{
   if (packetLen < NETPLAY_HEADER_LEN)
      return false;
   if (packet[0] != 'R' || packet[1] != 'N' || packet[2] != 'P' || packet[3] != '1')
      return false;

   *room = ReadLE64(packet + 4);
   *src = ReadLE16(packet + 12);
   *dst = ReadLE16(packet + 14);
   *flags = ReadLE32(packet + 16);
   *payloadLen = ReadLE32(packet + 20);
   if (*payloadLen > (u32)(packetLen - NETPLAY_HEADER_LEN))
      return false;

   *payload = packet + NETPLAY_HEADER_LEN;
   return true;
}

static int MP_SendNetplayPacket(const u8* payload, int payloadLen, u32 flags, u16 dst)
{
   if (payloadLen < 0 || payloadLen > (int)(sizeof(NetplayPacketBuffer) - NETPLAY_HEADER_LEN))
      return 0;

   NetplayEncodePacket(NetplayPacketBuffer, MPNetplayRoom, MPNetplayClientID, dst, flags, payload, (u32)payloadLen);
   int slen = sendto(MPSocket, (const char*)NetplayPacketBuffer, payloadLen + NETPLAY_HEADER_LEN, 0, &MPHubAddr, MPHubAddrLen);
   if (slen < NETPLAY_HEADER_LEN)
      return 0;
   return payloadLen;
}

static void MP_SendNetplayPresence()
{
   u16 dst = MPNetplayClientID == 0 ? NETPLAY_BROADCAST_ID : 0;
   MP_SendNetplayPacket(NULL, 0, NETPLAY_FLAG_PRESENCE, dst);
}

static int MP_SendDirectPacket(const u8* payload, int payloadLen)
{
   int sent = 0;
   for (u16 peer = 0; peer < MPDirectPeerCount; peer++)
   {
      sockaddr_t peerAddr;
      memset(&peerAddr, 0, sizeof(peerAddr));
      peerAddr.sa_family = AF_INET;
      *(u32*)&peerAddr.sa_data[2] = htonl(INADDR_LOOPBACK);
      *(u16*)&peerAddr.sa_data[0] = htons(MPDirectBasePort + peer);

      int slen = sendto(MPSocket, (const char*)payload, payloadLen, 0, &peerAddr, sizeof(sockaddr_t));
      if (slen == payloadLen)
         sent = slen;
   }

   return sent;
}

namespace Platform
{
    FILE* OpenFile(const char* path, const char* mode, bool mustexist)
    {
    FILE* ret;

    if (mustexist)
    {
        ret = fopen(path, "rb");
        if (ret) fclose(ret);
        ret = fopen(path, mode);
    }
    else
        ret = fopen(path, mode);

    return ret;
}

   FILE* OpenLocalFile(const char* path, const char* mode)
   {
      std::string fullpath = std::string(retro_base_directory) + std::string(1, PLATFORM_DIR_SEPERATOR) + std::string(path);
      FILE* f = OpenFile(fullpath.c_str(), mode, true);
      return f;
   }

   FILE* OpenDataFile(const char* path)
   {
      return OpenLocalFile(path, "rb");
   }

   void StopEmu()
   {
       return;
   }

   void Semaphore_Reset(Semaphore *sema)
   {
   #ifdef HAVE_THREADS
      while (ssem_get((ssem_t*)sema) > 0) {
        ssem_trywait((ssem_t*)sema);
      }
   #endif
   }

   void Semaphore_Post(Semaphore *sema, int count)
   {
   #ifdef HAVE_THREADS
       for (int i = 0; i < count; i++)
      {
         ssem_signal((ssem_t*)sema);
      }
   #endif
   }

   void Semaphore_Wait(Semaphore *sema)
   {
   #ifdef HAVE_THREADS
      ssem_wait((ssem_t*)sema);
   #endif
   }

   void Semaphore_Free(Semaphore *sema)
   {
   #ifdef HAVE_THREADS
      ssem_t *sem = (ssem_t*)sema;
      if (sem)
         ssem_free(sem);
   #endif
   }

   Semaphore *Semaphore_Create()
   {
   #ifdef HAVE_THREADS
      ssem_t *sem = ssem_new(0);
      if (sem)
         return (Semaphore*)sem;
   #endif
      return NULL;
   }

   Mutex* Mutex_Create()
   {
   #ifdef HAVE_THREADS
      return (Mutex*)slock_new();
   #endif
      return NULL;
   }

   void Mutex_Free(Mutex* mutex)
   {
   #ifdef HAVE_THREADS
      slock_free((slock_t*)mutex);
   #endif
   }

   void Mutex_Lock(Mutex* mutex)
   {
   #ifdef HAVE_THREADS
      slock_lock((slock_t*)mutex);
   #endif
   }

   void Mutex_Unlock(Mutex* mutex)
   {
   #ifdef HAVE_THREADS
      slock_unlock((slock_t*)mutex);
   #endif
   }

   bool Mutex_TryLock(Mutex* mutex)
   {
   #ifdef HAVE_THREADS
      slock_try_lock((slock_t*)mutex);
   #endif
      return true;
   }

   void Thread_Free(Thread *thread)
   {
   #if HAVE_THREADS
      sthread_detach((sthread_t*)thread);
   #endif
   }

   struct ThreadData
   {
      std::function<void()> fn;
   };

   void function_trampoline(void* param) {
      ThreadData* data = (ThreadData*)param;
      data->fn();
      delete data;
   }

   Thread *Thread_Create(std::function<void()> func)
   {
   #if HAVE_THREADS
      return (Thread*) sthread_create(function_trampoline, new ThreadData{func});
   #endif
   }

   void Thread_Wait(Thread *thread)
   {
   #if HAVE_THREADS
      sthread_join((sthread_t*)thread);
   #endif
   }


   bool MP_Init()
   {
      int opt_true = 1;
      int res;
      const char* hub = FirstEnv("MELONDS_NETPLAY_HUB", "REBIT_MELONDS_NETPLAY_HUB");
      const char* room = FirstEnv("MELONDS_NETPLAY_ROOM", "REBIT_MELONDS_NETPLAY_ROOM");
      const char* clientIDText = FirstEnv("MELONDS_NETPLAY_CLIENT_ID", "REBIT_MELONDS_NETPLAY_CLIENT_ID");
      MPUseHub = false;
      MPUseDirect = false;

#ifdef _WIN32
      WSADATA wsadata;
      if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
      {
         return false;
      }
#endif // __WXMSW__

      MPSocket = socket(AF_INET, SOCK_DGRAM, 0);
      if (MPSocket < 0)
      {
         return false;
      }

      if (ParseNetplayClientID(clientIDText, &MPNetplayClientID))
      {
         MPUseDirect = true;
         MPDirectBasePort = ParseEnvU16(FirstEnv("MELONDS_LAN_BASE_PORT", "REBIT_MELONDS_LAN_BASE_PORT"), DIRECT_LAN_DEFAULT_BASE_PORT, 1024, 65000);
         MPDirectPeerCount = ParseEnvU16(FirstEnv("MELONDS_LAN_PEER_COUNT", "REBIT_MELONDS_LAN_PEER_COUNT"), DIRECT_LAN_DEFAULT_PEER_COUNT, 1, 16);
      }
      else if (hub && ResolveNetplayHub(hub, &MPHubAddr, &MPHubAddrLen))
      {
         MPUseHub = true;
         MPNetplayClientID = 0;
         MPNetplayRoom = HashNetplayRoom(room);
      }
      else if (hub)
      {
         printf("melonDS LAN configuration is invalid; falling back to UDP broadcast\n");
      }

      res = setsockopt(MPSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt_true, sizeof(int));
      if (res < 0)
      {
         closesocket(MPSocket);
         MPSocket = INVALID_SOCKET;
         return false;
      }

      sockaddr_t saddr;
      saddr.sa_family = AF_INET;
      *(u32*)&saddr.sa_data[2] = htonl(INADDR_ANY);
      *(u16*)&saddr.sa_data[0] = htons(MPUseDirect ? MPDirectBasePort + MPNetplayClientID : (MPUseHub ? 0 : 7064));
      res = bind(MPSocket, &saddr, sizeof(sockaddr_t));
      if (res < 0)
      {
         closesocket(MPSocket);
         MPSocket = INVALID_SOCKET;
         return false;
      }

      if (MPUseDirect)
      {
         printf("melonDS LAN direct loopback enabled: client=%u port=%u peers=%u\n", MPNetplayClientID, MPDirectBasePort + MPNetplayClientID, MPDirectPeerCount);
         return true;
      }

      if (MPUseHub)
      {
         printf("melonDS LAN hub enabled: room=%s client=%u hub=%s\n", room && room[0] ? room : "default", MPNetplayClientID, hub);
         MP_SendNetplayPresence();
         return true;
      }

      res = setsockopt(MPSocket, SOL_SOCKET, SO_BROADCAST, (const char*)&opt_true, sizeof(int));
      if (res < 0)
      {
         closesocket(MPSocket);
         MPSocket = INVALID_SOCKET;
         return false;
      }

      MPSendAddr.sa_family = AF_INET;
      *(u32*)&MPSendAddr.sa_data[2] = htonl(INADDR_BROADCAST);
      *(u16*)&MPSendAddr.sa_data[0] = htons(7064);

      return true;
   }

   void MP_DeInit()
   {
      if (MPSocket >= 0)
         closesocket(MPSocket);

#ifdef _WIN32
      WSACleanup();
#endif // __WXMSW__
   }

   int MP_SendPacket(u8* data, int len)
   {
      if (MPSocket < 0)
      {
         printf("MP_SendPacket: early return (%d)\n", len);
         return 0;
      }

      if (len > 2048-8)
      {
         printf("MP_SendPacket: error: packet too long (%d)\n", len);
         return 0;
      }

      *(u32*)&PacketBuffer[0] = htonl(0x4946494E); // NIFI
      PacketBuffer[4] = NIFI_VER;
      PacketBuffer[5] = 0;
      *(u16*)&PacketBuffer[6] = htons(len);
      memcpy(&PacketBuffer[8], data, len);

      int slen;
      if (MPUseDirect)
         slen = MP_SendDirectPacket(PacketBuffer, len + 8);
      else if (MPUseHub)
         slen = MP_SendNetplayPacket(PacketBuffer, len + 8, 0, NETPLAY_BROADCAST_ID);
      else
         slen = sendto(MPSocket, (const char*)PacketBuffer, len+8, 0, &MPSendAddr, sizeof(sockaddr_t));
      if (slen < 8) return 0;
      return slen - 8;

   }

   int MP_RecvPacket(u8* data, bool block)
   {
      if (MPSocket < 0)
      {
         printf("MP_RecvPacket: early return\n");
         return 0;
      }

      fd_set fd;
      struct timeval tv;

      FD_ZERO(&fd);
      FD_SET(MPSocket, &fd);
      tv.tv_sec = 0;
      tv.tv_usec = block ? 5000 : 0;

      if (!select(MPSocket+1, &fd, 0, 0, &tv))
      {
         return 0;
      }

      sockaddr_t fromAddr;
      socklen_t fromLen = sizeof(sockaddr_t);
      int rlen;
      if (MPUseHub)
      {
         rlen = recvfrom(MPSocket, (char*)NetplayPacketBuffer, sizeof(NetplayPacketBuffer), 0, &fromAddr, &fromLen);
         if (rlen < NETPLAY_HEADER_LEN)
         {
            return 0;
         }

         u64 room;
         u16 src;
         u16 dst;
         u32 flags;
         const u8* payload;
         u32 payloadLen;
         if (!NetplayDecodePacket(NetplayPacketBuffer, rlen, &room, &src, &dst, &flags, &payload, &payloadLen))
         {
            return 0;
         }
         if (room != MPNetplayRoom || src == MPNetplayClientID || (dst != MPNetplayClientID && dst != NETPLAY_BROADCAST_ID))
         {
            return 0;
         }
         if (payloadLen == 0 || (flags & NETPLAY_FLAG_PRESENCE))
         {
            return 0;
         }
         if (payloadLen > sizeof(PacketBuffer))
         {
            return 0;
         }

         memcpy(PacketBuffer, payload, payloadLen);
         rlen = payloadLen;
      }
      else
      {
         rlen = recvfrom(MPSocket, (char*)PacketBuffer, 2048, 0, &fromAddr, &fromLen);
      }
      if (rlen < 8+24)
      {
         return 0;
      }
      rlen -= 8;

      if (ntohl(*(u32*)&PacketBuffer[0]) != 0x4946494E)
      {
         return 0;
      }

      if (PacketBuffer[4] != NIFI_VER)
      {
         return 0;
      }

      if (ntohs(*(u16*)&PacketBuffer[6]) != rlen)
      {
        return 0;
      }

      memcpy(data, &PacketBuffer[8], rlen);
      return rlen;
   }

   bool LAN_Init()
   {
#ifdef HAVE_PCAP
    if (Config::DirectLAN)
    {
        if (!LAN_PCap::Init(true))
            return false;
    }
    else
    {
        if (!LAN_Socket::Init())
            return false;
    }

    return true;
#else
   return false;
#endif
   }

   void LAN_DeInit()
   {
      // checkme. blarg
      //if (Config::DirectLAN)
      //    LAN_PCap::DeInit();
      //else
      //    LAN_Socket::DeInit();
#ifdef HAVE_PCAP
      LAN_PCap::DeInit();
      LAN_Socket::DeInit();
#endif
   }

   int LAN_SendPacket(u8* data, int len)
   {
#ifdef HAVE_PCAP
      if (Config::DirectLAN)
         return LAN_PCap::SendPacket(data, len);
      else
         return LAN_Socket::SendPacket(data, len);
#else
      return 0;
#endif
   }

   int LAN_RecvPacket(u8* data)
   {
#ifdef HAVE_PCAP
      if (Config::DirectLAN)
         return LAN_PCap::RecvPacket(data);
      else
         return LAN_Socket::RecvPacket(data);
#else
      return 0;
#endif
   }

#ifdef HAVE_OPENGL
   void* GL_GetProcAddress(const char* proc)
   {
      return (void*)nullptr;
   }
#endif

   void Sleep(u64 usecs)
   {
      retro_sleep(usecs / 1000);
   }
};
