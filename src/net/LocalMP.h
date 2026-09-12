/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef LOCALMP_H
#define LOCALMP_H

#include "types.h"
#include "Platform.h"
#include "MPInterface.h"

#include <cstddef>
#include <vector>

namespace melonDS
{
class Savestate;
struct MPStatusData
{
    u16 ConnectedBitmask; // bitmask of which instances are ready to send/receive packets
    u32 PacketWriteOffset;
    u32 ReplyWriteOffset;
    u16 MPHostinst; // instance ID from which the last CMD frame was sent
    u16 MPReplyBitmask;   // bitmask of which clients replied in time
};

constexpr u32 kPacketQueueSize = 0x10000;
constexpr u32 kReplyQueueSize = 0x10000;
constexpr u32 kMaxFrameSize = 0x948;

class LocalMP : public MPInterface
{
public:
    LocalMP() noexcept;
    LocalMP(const LocalMP&) = delete;
    LocalMP& operator=(const LocalMP&) = delete;
    LocalMP(LocalMP&& other) = delete;
    LocalMP& operator=(LocalMP&& other) = delete;
    ~LocalMP() noexcept;

    void Process() {}

    void Begin(int inst);
    void End(int inst);

    int SendPacket(int inst, u8* data, int len, u64 timestamp);
    int RecvPacket(int inst, u8* data, u64* timestamp);
    int SendCmd(int inst, u8* data, int len, u64 timestamp);
    int SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid);
    int SendAck(int inst, u8* data, int len, u64 timestamp);
    int RecvHostPacket(int inst, u8* data, u64* timestamp);
    u16 RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask);
    // Download Play uses one emulated console per process and bridges its
    // LocalMP frames over the browser data channel. These methods inject the
    // remote radio stream without changing the normal in-process queues.
    void SetExternalConnected(int inst, bool connected, bool waitForReplies = true);
    bool InjectExternalPacket(int inst, u32 type, u16 aid, const u8* packet, int len, u64 timestamp);
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    bool PacketsReady(int inst) noexcept;
    bool RepliesReady(int inst) noexcept;
#endif

    std::vector<u8> SerializeState();
    bool DeserializeState(const u8* data, std::size_t length);
#ifdef REBIT_MELONDS_ROLLBACK
    void DoRollbackState(Savestate* file);
#endif

private:
    void FIFORead(int inst, int fifo, void* buf, int len) noexcept;
    void FIFOWrite(int inst, int fifo, void* buf, int len) noexcept;
    int SendPacketGeneric(int inst, u32 type, u8* packet, int len, u64 timestamp) noexcept;
    int RecvPacketGeneric(int inst, u8* packet, bool block, u64* timestamp) noexcept;

    Platform::Mutex* MPQueueLock;
    MPStatusData MPStatus {};
    u8 MPPacketQueue[kPacketQueueSize] {};
    u8 MPReplyQueue[kReplyQueueSize] {};
    u32 PacketReadOffset[16] {};
    u32 ReplyReadOffset[16] {};
    u32 PacketSignalCount[16] {};
    u32 ReplySignalCount[16] {};
    bool ExternalConnected[16] {};
    bool ExternalWaitReplies[16] {};
    bool ExternalWaitPackets[16] {};

    int LastHostID = -1;
    Platform::Semaphore* SemPool[32] {};
};
}

#endif // LOCALMP_H
