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

#include <algorithm>
#include <cstring>
#include <array>
#include <limits>
#include <vector>

#include "LocalMP.h"

using namespace melonDS;
using namespace melonDS::Platform;

using Platform::Log;
using Platform::LogLevel;

namespace melonDS
{

namespace
{

constexpr u32 LocalMPStateMagic = 0x504D4252; // "RBMP" in little endian.
constexpr u32 LocalMPStateVersion = 1;
constexpr u32 MaximumSerializedSignals = 8192;

void QueueLock(Platform::Mutex* mutex) noexcept
{
#if !defined(REBIT_MELONDS_DUAL_SCHEDULED)
    Platform::Mutex_Lock(mutex);
#else
    (void)mutex;
#endif
}

void QueueUnlock(Platform::Mutex* mutex) noexcept
{
#if !defined(REBIT_MELONDS_DUAL_SCHEDULED)
    Platform::Mutex_Unlock(mutex);
#else
    (void)mutex;
#endif
}

void ResetSignal(Platform::Semaphore* semaphore) noexcept
{
#if !defined(REBIT_MELONDS_DUAL_SCHEDULED)
    Platform::Semaphore_Reset(semaphore);
#else
    (void)semaphore;
#endif
}

void PostSignal(Platform::Semaphore* semaphore) noexcept
{
#if !defined(REBIT_MELONDS_DUAL_SCHEDULED)
    Platform::Semaphore_Post(semaphore);
#else
    (void)semaphore;
#endif
}

bool TryWaitSignal(Platform::Semaphore* semaphore, u32 pending, int timeout) noexcept
{
#if !defined(REBIT_MELONDS_DUAL_SCHEDULED)
    (void)pending;
    return Platform::Semaphore_TryWait(semaphore, timeout);
#else
    (void)semaphore;
    (void)timeout;
    return pending > 0;
#endif
}

void Append16(std::vector<u8>& output, u16 value)
{
    output.push_back(static_cast<u8>(value));
    output.push_back(static_cast<u8>(value >> 8));
}

void Append32(std::vector<u8>& output, u32 value)
{
    output.push_back(static_cast<u8>(value));
    output.push_back(static_cast<u8>(value >> 8));
    output.push_back(static_cast<u8>(value >> 16));
    output.push_back(static_cast<u8>(value >> 24));
}

bool Read16(const u8*& cursor, const u8* end, u16& value)
{
    if (end - cursor < 2)
        return false;
    value = static_cast<u16>(cursor[0]) | (static_cast<u16>(cursor[1]) << 8);
    cursor += 2;
    return true;
}

bool Read32(const u8*& cursor, const u8* end, u32& value)
{
    if (end - cursor < 4)
        return false;
    value = static_cast<u32>(cursor[0])
        | (static_cast<u32>(cursor[1]) << 8)
        | (static_cast<u32>(cursor[2]) << 16)
        | (static_cast<u32>(cursor[3]) << 24);
    cursor += 4;
    return true;
}

bool ReadBytes(const u8*& cursor, const u8* end, void* destination, std::size_t length)
{
    if (length > static_cast<std::size_t>(end - cursor))
        return false;
    std::memcpy(destination, cursor, length);
    cursor += length;
    return true;
}

}

LocalMP::LocalMP() noexcept :
    MPQueueLock(Mutex_Create())
{
    memset(MPPacketQueue, 0, kPacketQueueSize);
    memset(MPReplyQueue, 0, kReplyQueueSize);
    memset(&MPStatus, 0, sizeof(MPStatus));
    memset(PacketReadOffset, 0, sizeof(PacketReadOffset));
    memset(ReplyReadOffset, 0, sizeof(ReplyReadOffset));

    // prepare semaphores
    // semaphores 0-15: regular frames; semaphore I is posted when instance I needs to process a new frame
    // semaphores 16-31: MP replies; semaphore I is posted when instance I needs to process a new MP reply

    for (int i = 0; i < 32; i++)
    {
        SemPool[i] = Semaphore_Create();
    }

    Log(LogLevel::Info, "MP comm init OK\n");
}

LocalMP::~LocalMP() noexcept
{
    for (int i = 0; i < 32; i++)
    {
        Semaphore_Free(SemPool[i]);
        SemPool[i] = nullptr;
    }

    Mutex_Free(MPQueueLock);
}

void LocalMP::Begin(int inst)
{
    QueueLock(MPQueueLock);
    PacketReadOffset[inst] = MPStatus.PacketWriteOffset;
    ReplyReadOffset[inst] = MPStatus.ReplyWriteOffset;
    ResetSignal(SemPool[inst]);
    ResetSignal(SemPool[16 + inst]);
    PacketSignalCount[inst] = 0;
    ReplySignalCount[inst] = 0;
    MPStatus.ConnectedBitmask |= (1 << inst);
    QueueUnlock(MPQueueLock);
}

void LocalMP::End(int inst)
{
    QueueLock(MPQueueLock);
    MPStatus.ConnectedBitmask &= ~(1 << inst);
    QueueUnlock(MPQueueLock);
}

void LocalMP::FIFORead(int inst, int fifo, void* buf, int len) noexcept
{
    u8* data;

    u32 offset, datalen;
    if (fifo == 0)
    {
        offset = PacketReadOffset[inst];
        data = MPPacketQueue;
        datalen = kPacketQueueSize;
    }
    else
    {
        offset = ReplyReadOffset[inst];
        data = MPReplyQueue;
        datalen = kReplyQueueSize;
    }

    if ((offset + len) >= datalen)
    {
        u32 part1 = datalen - offset;
        memcpy(buf, &data[offset], part1);
        memcpy(&((u8*)buf)[part1], data, len - part1);
        offset = len - part1;
    }
    else
    {
        memcpy(buf, &data[offset], len);
        offset += len;
    }

    if (fifo == 0) PacketReadOffset[inst] = offset;
    else           ReplyReadOffset[inst] = offset;
}

void LocalMP::FIFOWrite(int inst, int fifo, void* buf, int len) noexcept
{
    u8* data;

    u32 offset, datalen;
    if (fifo == 0)
    {
        offset = MPStatus.PacketWriteOffset;
        data = MPPacketQueue;
        datalen = kPacketQueueSize;
    }
    else
    {
        offset = MPStatus.ReplyWriteOffset;
        data = MPReplyQueue;
        datalen = kReplyQueueSize;
    }

    if ((offset + len) >= datalen)
    {
        u32 part1 = datalen - offset;
        memcpy(&data[offset], buf, part1);
        memcpy(data, &((u8*)buf)[part1], len - part1);
        offset = len - part1;
    }
    else
    {
        memcpy(&data[offset], buf, len);
        offset += len;
    }

    if (fifo == 0) MPStatus.PacketWriteOffset = offset;
    else           MPStatus.ReplyWriteOffset = offset;
}

int LocalMP::SendPacketGeneric(int inst, u32 type, u8* packet, int len, u64 timestamp) noexcept
{
    if (len > kMaxFrameSize)
    {
        Log(LogLevel::Warn, "wifi: attempting to send frame too big (len=%d max=%d)\n", len, kMaxFrameSize);
        return 0;
    }

    QueueLock(MPQueueLock);

    u16 mask = MPStatus.ConnectedBitmask;

    // TODO: check if the FIFO is full!

    MPPacketHeader pktheader;
    pktheader.Magic = 0x4946494E;
    pktheader.SenderID = inst;
    pktheader.Type = type;
    pktheader.Length = len;
    pktheader.Timestamp = timestamp;

    type &= 0xFFFF;
    int nfifo = (type == 2) ? 1 : 0;
    FIFOWrite(inst, nfifo, &pktheader, sizeof(pktheader));
    if (len)
        FIFOWrite(inst, nfifo, packet, len);

    if (type == 1)
    {
        // NOTE: this is not guarded against, say, multiple multiplay games happening on the same machine
        // we would need to pass the packet's SenderID through the wifi module for that
        MPStatus.MPHostinst = inst;
        MPStatus.MPReplyBitmask = 0;
        ReplyReadOffset[inst] = MPStatus.ReplyWriteOffset;
        ResetSignal(SemPool[16 + inst]);
        ReplySignalCount[inst] = 0;
    }
    else if (type == 2)
    {
        MPStatus.MPReplyBitmask |= (1 << inst);
    }

    const int replyHost = MPStatus.MPHostinst;
    if (type == 2)
    {
        if (replyHost >= 0 && replyHost < 16)
            ++ReplySignalCount[replyHost];
    }
    else
    {
        for (int i = 0; i < 16; ++i)
            if (mask & (1 << i))
                ++PacketSignalCount[i];
    }

    QueueUnlock(MPQueueLock);

    if (type == 2)
    {
        if (replyHost >= 0 && replyHost < 16)
            PostSignal(SemPool[16 + replyHost]);
    }
    else
    {
        for (int i = 0; i < 16; i++)
        {
            if (mask & (1<<i))
                PostSignal(SemPool[i]);
        }
    }

    return len;
}

int LocalMP::RecvPacketGeneric(int inst, u8* packet, bool block, u64* timestamp) noexcept
{
    for (;;)
    {
        if (!TryWaitSignal(SemPool[inst], PacketSignalCount[inst], block ? RecvTimeout : 0))
        {
            return 0;
        }

        QueueLock(MPQueueLock);

        if (PacketSignalCount[inst] > 0)
            --PacketSignalCount[inst];

        MPPacketHeader pktheader = {};
        FIFORead(inst, 0, &pktheader, sizeof(pktheader));

        if (pktheader.Magic != 0x4946494E)
        {
            Log(LogLevel::Warn, "PACKET FIFO OVERFLOW\n");
            PacketReadOffset[inst] = MPStatus.PacketWriteOffset;
            ResetSignal(SemPool[inst]);
            PacketSignalCount[inst] = 0;
            QueueUnlock(MPQueueLock);
            return 0;
        }

        if (pktheader.SenderID == inst)
        {
            // skip this packet
            PacketReadOffset[inst] += pktheader.Length;
            if (PacketReadOffset[inst] >= kPacketQueueSize)
                PacketReadOffset[inst] -= kPacketQueueSize;

            QueueUnlock(MPQueueLock);
            continue;
        }

        if (pktheader.Length)
        {
            FIFORead(inst, 0, packet, pktheader.Length);

            if (pktheader.Type == 1)
                LastHostID = pktheader.SenderID;
        }

        if (timestamp) *timestamp = pktheader.Timestamp;
        QueueUnlock(MPQueueLock);
        return pktheader.Length;
    }
}

int LocalMP::SendPacket(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 0, packet, len, timestamp);
}

int LocalMP::RecvPacket(int inst, u8* packet, u64* timestamp)
{
    return RecvPacketGeneric(inst, packet, false, timestamp);
}

int LocalMP::SendCmd(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 1, packet, len, timestamp);
}

int LocalMP::SendReply(int inst, u8* packet, int len, u64 timestamp, u16 aid)
{
    return SendPacketGeneric(inst, 2 | (aid<<16), packet, len, timestamp);
}

int LocalMP::SendAck(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 3, packet, len, timestamp);
}

int LocalMP::RecvHostPacket(int inst, u8* packet, u64* timestamp)
{
    if (LastHostID != -1)
    {
        // check if the host is still connected

        u16 curinstmask = MPStatus.ConnectedBitmask;

        if (!(curinstmask & (1 << LastHostID)))
            return -1;
    }

    return RecvPacketGeneric(inst, packet, true, timestamp);
}

#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
bool LocalMP::PacketsReady(int inst) noexcept
{
    if (inst < 0 || inst >= 16)
        return false;
    QueueLock(MPQueueLock);
    const bool ready = PacketSignalCount[inst] > 0;
    QueueUnlock(MPQueueLock);
    return ready;
}

bool LocalMP::RepliesReady(int inst) noexcept
{
    if (inst < 0 || inst >= 16)
        return false;
    QueueLock(MPQueueLock);
    const u16 connected = MPStatus.ConnectedBitmask;
    const u16 others = connected & ~(1 << inst);
    const bool ready = others == 0 || ReplySignalCount[inst] > 0;
    QueueUnlock(MPQueueLock);
    return ready;
}
#endif

u16 LocalMP::RecvReplies(int inst, u8* packets, u64 timestamp, u16 aidmask)
{
    u16 ret = 0;
    u16 myinstmask = (1 << inst);
    u16 curinstmask;

    curinstmask = MPStatus.ConnectedBitmask;

    // if all clients have left: return early
    if ((myinstmask & curinstmask) == curinstmask)
        return 0;

    for (;;)
    {
        if (!TryWaitSignal(SemPool[16 + inst], ReplySignalCount[inst], RecvTimeout))
        {
            // no more replies available
            return ret;
        }

        QueueLock(MPQueueLock);

        if (ReplySignalCount[inst] > 0)
            --ReplySignalCount[inst];

        MPPacketHeader pktheader = {};
        FIFORead(inst, 1, &pktheader, sizeof(pktheader));

        if (pktheader.Magic != 0x4946494E)
        {
            Log(LogLevel::Warn, "REPLY FIFO OVERFLOW\n");
            ReplyReadOffset[inst] = MPStatus.ReplyWriteOffset;
            ResetSignal(SemPool[16 + inst]);
            ReplySignalCount[inst] = 0;
            QueueUnlock(MPQueueLock);
            return 0;
        }

        if ((pktheader.SenderID == inst) || // packet we sent out (shouldn't happen, but hey)
            (pktheader.Timestamp < (timestamp - 32))) // stale packet
        {
            // skip this packet
            ReplyReadOffset[inst] += pktheader.Length;
            if (ReplyReadOffset[inst] >= kReplyQueueSize)
                ReplyReadOffset[inst] -= kReplyQueueSize;

            QueueUnlock(MPQueueLock);
            continue;
        }

        if (pktheader.Length)
        {
            u32 aid = (pktheader.Type >> 16);
            FIFORead(inst, 1, &packets[(aid-1)*1024], pktheader.Length);
            ret |= (1 << aid);
        }

        myinstmask |= (1 << pktheader.SenderID);
        if (((myinstmask & curinstmask) == curinstmask) ||
            ((ret & aidmask) == aidmask))
        {
            // all the clients have sent their reply

            QueueUnlock(MPQueueLock);
            return ret;
        }

        QueueUnlock(MPQueueLock);
    }
}

std::vector<u8> LocalMP::SerializeState()
{
    std::vector<u8> output;
    output.reserve(8 + 18 + (16 * 4 * 4) + kPacketQueueSize + kReplyQueueSize);

    QueueLock(MPQueueLock);
    Append32(output, LocalMPStateMagic);
    Append32(output, LocalMPStateVersion);
    Append16(output, MPStatus.ConnectedBitmask);
    Append32(output, MPStatus.PacketWriteOffset);
    Append32(output, MPStatus.ReplyWriteOffset);
    Append16(output, MPStatus.MPHostinst);
    Append16(output, MPStatus.MPReplyBitmask);
    Append32(output, static_cast<u32>(LastHostID));
    for (u32 value : PacketReadOffset) Append32(output, value);
    for (u32 value : ReplyReadOffset) Append32(output, value);
    for (u32 value : PacketSignalCount) Append32(output, value);
    for (u32 value : ReplySignalCount) Append32(output, value);
    output.insert(output.end(), MPPacketQueue, MPPacketQueue + kPacketQueueSize);
    output.insert(output.end(), MPReplyQueue, MPReplyQueue + kReplyQueueSize);
    QueueUnlock(MPQueueLock);

    return output;
}

bool LocalMP::DeserializeState(const u8* data, std::size_t length)
{
    if (!data || length > static_cast<std::size_t>(std::numeric_limits<u32>::max()))
        return false;

    const u8* cursor = data;
    const u8* end = data + length;
    u32 magic = 0;
    u32 version = 0;
    MPStatusData status {};
    u32 lastHost = 0;
    std::array<u32, 16> packetRead {};
    std::array<u32, 16> replyRead {};
    std::array<u32, 16> packetSignals {};
    std::array<u32, 16> replySignals {};
    std::array<u8, kPacketQueueSize> packetQueue {};
    std::array<u8, kReplyQueueSize> replyQueue {};

    if (!Read32(cursor, end, magic)
        || !Read32(cursor, end, version)
        || !Read16(cursor, end, status.ConnectedBitmask)
        || !Read32(cursor, end, status.PacketWriteOffset)
        || !Read32(cursor, end, status.ReplyWriteOffset)
        || !Read16(cursor, end, status.MPHostinst)
        || !Read16(cursor, end, status.MPReplyBitmask)
        || !Read32(cursor, end, lastHost))
        return false;
    for (u32& value : packetRead) if (!Read32(cursor, end, value)) return false;
    for (u32& value : replyRead) if (!Read32(cursor, end, value)) return false;
    for (u32& value : packetSignals) if (!Read32(cursor, end, value)) return false;
    for (u32& value : replySignals) if (!Read32(cursor, end, value)) return false;
    if (!ReadBytes(cursor, end, packetQueue.data(), packetQueue.size())
        || !ReadBytes(cursor, end, replyQueue.data(), replyQueue.size())
        || cursor != end
        || magic != LocalMPStateMagic
        || version != LocalMPStateVersion
        || status.PacketWriteOffset >= kPacketQueueSize
        || status.ReplyWriteOffset >= kReplyQueueSize
        || status.MPHostinst >= 16
        || (lastHost != std::numeric_limits<u32>::max() && lastHost >= 16))
        return false;
    for (int index = 0; index < 16; ++index)
    {
        if (packetRead[index] >= kPacketQueueSize
            || replyRead[index] >= kReplyQueueSize
            || packetSignals[index] > MaximumSerializedSignals
            || replySignals[index] > MaximumSerializedSignals)
            return false;
    }

    QueueLock(MPQueueLock);
    MPStatus = status;
    LastHostID = lastHost == std::numeric_limits<u32>::max() ? -1 : static_cast<int>(lastHost);
    std::copy(packetRead.begin(), packetRead.end(), PacketReadOffset);
    std::copy(replyRead.begin(), replyRead.end(), ReplyReadOffset);
    std::copy(packetSignals.begin(), packetSignals.end(), PacketSignalCount);
    std::copy(replySignals.begin(), replySignals.end(), ReplySignalCount);
    std::copy(packetQueue.begin(), packetQueue.end(), MPPacketQueue);
    std::copy(replyQueue.begin(), replyQueue.end(), MPReplyQueue);
    for (int index = 0; index < 16; ++index)
    {
        ResetSignal(SemPool[index]);
        ResetSignal(SemPool[16 + index]);
        for (u32 signal = 0; signal < PacketSignalCount[index]; ++signal)
            PostSignal(SemPool[index]);
        for (u32 signal = 0; signal < ReplySignalCount[index]; ++signal)
            PostSignal(SemPool[16 + index]);
    }
    QueueUnlock(MPQueueLock);
    return true;
}

}
