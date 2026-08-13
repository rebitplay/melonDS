#ifndef REBIT_MELONDS_DUAL_RUNTIME_INTERNAL_H
#define REBIT_MELONDS_DUAL_RUNTIME_INTERNAL_H

#include <atomic>
#include <cstdint>
#include <vector>

namespace melonDS { class LocalMP; }

namespace rebit
{

struct SlotContext
{
    int id = 0;
    std::atomic<bool> stopped {false};
    std::atomic<std::uint64_t> packetsSent {0};
    std::atomic<std::uint64_t> packetsReceived {0};
    std::atomic<std::uint64_t> commands {0};
    std::atomic<std::uint64_t> replies {0};
    std::atomic<std::uint64_t> multiplayerCalls {0};
    std::atomic<std::uint64_t> replyBaseline {0};
    std::atomic<bool> awaitingReplies {false};
    std::vector<std::uint8_t> latestSave;
};

enum class MultiplayerOperation : std::uint8_t
{
    Begin,
    End,
    SendPacket,
    RecvPacket,
    SendCommand,
    SendReply,
    SendAck,
    RecvHostPacket,
    RecvReplies,
};

int InstanceId(void* userdata) noexcept;
SlotContext* Context(void* userdata) noexcept;
melonDS::LocalMP* LocalMultiplayer() noexcept;
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
bool MultiplayerPacketsReady(void* userdata) noexcept;
bool MultiplayerRepliesReady(void* userdata) noexcept;
void RequestMultiplayerYield(void* userdata) noexcept;
#endif
bool EnterMultiplayerTurn(void* userdata, MultiplayerOperation operation) noexcept;
void LeaveMultiplayerTurn(void* userdata, bool scheduled) noexcept;
void NoteMultiplayerCommand(void* userdata) noexcept;
void NoteMultiplayerReply() noexcept;
void SignalStopped(void* userdata) noexcept;
void StoreSave(const std::uint8_t* data, std::uint32_t length, void* userdata);

}

#endif
