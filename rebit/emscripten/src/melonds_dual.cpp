#include "runtime_internal.h"
#include "rebit_melonds_dual.h"

#include "Args.h"
#include "GPU.h"
#include "LocalMP.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Savestate.h"
#include "SPI_Firmware.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define REBIT_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define REBIT_EXPORT
#endif

#ifndef REBIT_MELONDS_DUAL_BUILD
#define REBIT_MELONDS_DUAL_BUILD "melonds-dual-dev"
#endif
#ifndef REBIT_MELONDS_DUAL_RUNTIME_ABI
#define REBIT_MELONDS_DUAL_RUNTIME_ABI "rebit-melonds-dual-dev"
#endif

namespace rebit
{

namespace
{

constexpr int MinimumPlayers = 2;
constexpr int MaximumPlayers = 4;
constexpr int ScreenWidth = 256;
constexpr int ScreenHeight = 192;
constexpr int CombinedHeight = ScreenHeight * 2;
constexpr int AudioScratchFrames = 4096;
// LocalMP calls are cooperatively ordered below, so receive operations must
// never make correctness depend on a host scheduler or wall-clock timeout.
constexpr int LocalMultiplayerReceiveTimeoutMs = 0;
// The Wi-Fi device polls empty receive queues dozens of times per video frame.
// Switching pthreads after each poll is especially expensive in WebAssembly.
// A fixed batch keeps the schedule implementation-neutral and deterministic
// while still handing packet-producing operations to the next console at once.
constexpr int MultiplayerPacketReceivePollBatch = 4;
constexpr int MultiplayerHostReceivePollBatch = 4;
constexpr int MultiplayerProgressSpins = 8;
constexpr auto MultiplayerFrameWatchdog = std::chrono::seconds(2);
constexpr auto MultiplayerRecoveryWatchdog = std::chrono::seconds(5);
constexpr std::uint32_t CheckpointMagic = 0x53444E52; // "RNDS" in little endian.
constexpr std::uint32_t CheckpointVersion = 2;
constexpr std::uint32_t MaximumConsoleStateBytes = 64 * 1024 * 1024;
constexpr std::uint32_t MaximumMultiplayerStateBytes = 1024 * 1024;
constexpr std::uint32_t MaximumCheckpointBytes = MaximumPlayers * MaximumConsoleStateBytes + MaximumMultiplayerStateBytes;

struct InputState
{
    std::uint32_t keys = 0x0FFF;
    bool touching = false;
    std::uint16_t touchX = 0;
    std::uint16_t touchY = 0;
};

struct Slot
{
    SlotContext context;
    std::unique_ptr<melonDS::NDS> console;
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    bool cooperativeFrameComplete = true;
#else
    std::thread worker;
#endif
    InputState input;
    std::array<std::uint32_t, ScreenWidth * CombinedHeight> framebuffer {};
    std::uint64_t observedGeneration = 0;
    int multiplayerPacketReceivePolls = 0;
    int multiplayerHostReceivePolls = 0;
};

struct Runtime
{
    std::mutex frameMutex;
    std::condition_variable frameStart;
    std::condition_variable frameComplete;
    std::vector<std::unique_ptr<Slot>> slots;
    std::unique_ptr<melonDS::LocalMP> multiplayer;
    std::array<std::int16_t, AudioScratchFrames * 2> audio {};
    std::vector<std::uint8_t> checkpoint;
    std::string error;
    std::uint64_t generation = 0;
    int completed = 0;
    std::atomic<std::uint32_t> multiplayerDoneMask {0};
    std::atomic<std::uint64_t> multiplayerReplySequence {0};
    std::atomic<int> multiplayerTurn {-1};
    std::atomic<std::uint32_t> multiplayerProgressSequence {0};
    int visiblePlayer = 0;
    bool shuttingDown = false;
    std::atomic<bool> multiplayerFrameActive {false};
    std::atomic<std::uint32_t> schedulerJitterProfile {0};
    bool loaded = false;
    double lastFrameMs = 0.0;
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    std::uint64_t cooperativeSlices = 0;
#endif
};

Runtime State;

#ifdef REBIT_MELONDS_ROLLBACK
struct RollbackRecord
{
    std::array<std::vector<std::uint8_t>, MaximumPlayers + 1> parts;
    std::array<InputState, MaximumPlayers> inputs {};
    std::array<std::array<std::uint64_t, 7>, MaximumPlayers> context {};
    std::uint64_t replySequence = 0;
    std::uint32_t frame = UINT32_MAX;
};
std::vector<RollbackRecord> RollbackRing;
std::uint32_t RollbackBytes = 0;
bool RollbackFaulted = false;
#endif

int NextMultiplayerTurn(int current, std::uint32_t doneMask)
{
    const int players = static_cast<int>(State.slots.size());
    for (int offset = 1; offset <= players; ++offset)
    {
        const int candidate = (current + offset + players) % players;
        if ((doneMask & (1U << candidate)) == 0)
            return candidate;
    }
    return -1;
}

void NotifyMultiplayerProgress()
{
    State.multiplayerProgressSequence.fetch_add(1, std::memory_order_release);
#ifndef __EMSCRIPTEN__
    State.multiplayerProgressSequence.notify_all();
#endif
}

void WaitForMultiplayerProgress(std::uint32_t observedSequence)
{
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    (void)observedSequence;
#else
#ifdef __EMSCRIPTEN__
    while (State.multiplayerFrameActive.load(std::memory_order_acquire)
        && State.multiplayerProgressSequence.load(std::memory_order_acquire) == observedSequence)
    {
    }
    return;
#else
    // Most LocalMP turns are handed off within a few scheduler yields. Keep
    // that fast path lock-free, but bound it so a delayed browser worker
    // always falls back to the lossless sequence wait below.
    for (int attempt = 0; attempt < MultiplayerProgressSpins; ++attempt)
    {
        if (State.multiplayerProgressSequence.load(std::memory_order_acquire) != observedSequence)
            return;
        std::this_thread::yield();
    }
    // C++20 atomic wait closes the notify-before-wait race without a mutex or
    // a fixed wall-clock delay. It also maps directly to the browser futex.
    State.multiplayerProgressSequence.wait(observedSequence, std::memory_order_acquire);
#endif
#endif
}

void BeginMultiplayerFrame()
{
    for (const auto& slot : State.slots)
    {
        slot->multiplayerPacketReceivePolls = 0;
        slot->multiplayerHostReceivePolls = 0;
    }
    State.multiplayerDoneMask.store(0, std::memory_order_release);
    State.multiplayerTurn.store(State.slots.empty() ? -1 : 0, std::memory_order_release);
    State.multiplayerFrameActive.store(!State.slots.empty(), std::memory_order_release);
    NotifyMultiplayerProgress();
}

void CompleteMultiplayerFrame(int player)
{
    if (!State.multiplayerFrameActive.load(std::memory_order_acquire)
        || player < 0
        || player >= static_cast<int>(State.slots.size()))
        return;
#ifdef REBIT_MELONDS_ROLLBACK
    // Completion is a scheduled operation too. Otherwise it can change the
    // done mask between another console's mask read and turn handoff, leaving
    // the turn on a finished worker. Even without a deadlock, skipping that
    // worker according to host timing makes replay nondeterministic.
    if (!EnterMultiplayerTurn(&State.slots[player]->context, MultiplayerOperation::CompleteFrame))
        return;
#endif
    const std::uint32_t doneMask = State.multiplayerDoneMask.fetch_or(1U << player, std::memory_order_acq_rel)
        | (1U << player);
    const std::uint32_t allDoneMask = (1U << State.slots.size()) - 1U;
    if (doneMask == allDoneMask)
    {
        State.multiplayerFrameActive.store(false, std::memory_order_release);
        State.multiplayerTurn.store(-1, std::memory_order_release);
    }
    else if (State.multiplayerTurn.load(std::memory_order_acquire) == player)
    {
        int expected = player;
        State.multiplayerTurn.compare_exchange_strong(
            expected,
            NextMultiplayerTurn(player, doneMask),
            std::memory_order_acq_rel);
    }
    NotifyMultiplayerProgress();
}

void CancelMultiplayerFrame()
{
    State.multiplayerFrameActive.store(false, std::memory_order_release);
    State.multiplayerDoneMask.store(0, std::memory_order_release);
    State.multiplayerTurn.store(-1, std::memory_order_release);
    NotifyMultiplayerProgress();
}

void ApplySchedulerJitter(SlotContext& context)
{
    const std::uint32_t profile = State.schedulerJitterProfile.load(std::memory_order_relaxed);
    if (profile == 0)
        return;
    const std::uint64_t call = context.multiplayerCalls++;
    std::uint32_t value = profile ^ (static_cast<std::uint32_t>(context.id) * 0x9E3779B9U)
        ^ (static_cast<std::uint32_t>(call) * 0x85EBCA6BU);
    value ^= value >> 16;
#ifndef REBIT_MELONDS_DUAL_COOPERATIVE
    for (std::uint32_t iteration = 0; iteration < (value & 0x3FU); ++iteration)
        std::this_thread::yield();
#else
    (void)value;
#endif
}

std::uint64_t Fnv1a(const void* data, std::size_t size, std::uint64_t hash = 1469598103934665603ULL)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

void Append32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 24));
}

void Append64(std::vector<std::uint8_t>& output, std::uint64_t value)
{
    Append32(output, static_cast<std::uint32_t>(value));
    Append32(output, static_cast<std::uint32_t>(value >> 32));
}

bool Read32(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint32_t& value)
{
    if (end - cursor < 4)
        return false;
    value = static_cast<std::uint32_t>(cursor[0])
        | (static_cast<std::uint32_t>(cursor[1]) << 8)
        | (static_cast<std::uint32_t>(cursor[2]) << 16)
        | (static_cast<std::uint32_t>(cursor[3]) << 24);
    cursor += 4;
    return true;
}

bool Read64(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint64_t& value)
{
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (!Read32(cursor, end, low) || !Read32(cursor, end, high))
        return false;
    value = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
    return true;
}

bool ReadSlice(
    const std::uint8_t*& cursor,
    const std::uint8_t* end,
    std::uint32_t maximumLength,
    const std::uint8_t*& data,
    std::uint32_t& length)
{
    if (!Read32(cursor, end, length)
        || length > maximumLength
        || length > static_cast<std::uint32_t>(end - cursor))
        return false;
    data = cursor;
    cursor += length;
    return true;
}

struct CheckpointSlot
{
    InputState input;
    std::array<std::uint64_t, 5> counters {};
    std::uint64_t replyBaseline = 0;
    bool awaitingReplies = false;
    const std::uint8_t* state = nullptr;
    std::uint32_t stateLength = 0;
};

void CopyFramebuffer(Slot& slot);

bool RuntimeAtCheckpointBoundary()
{
    return !State.multiplayerFrameActive.load(std::memory_order_acquire)
        && State.multiplayerTurn.load(std::memory_order_acquire) < 0;
}

bool ExportCheckpoint()
{
    State.error.clear();
    if (!State.loaded || State.slots.empty() || !State.multiplayer || !RuntimeAtCheckpointBoundary()
#ifdef REBIT_MELONDS_ROLLBACK
        || RollbackFaulted
#endif
    )
    {
        State.error = "NDS Local Wireless checkpoint requested outside a completed frame.";
        return false;
    }

    std::lock_guard<std::mutex> guard(State.frameMutex);
    const std::uint32_t frame = State.slots.front()->console->NumFrames;
    for (const auto& slot : State.slots)
    {
        if (!slot->console || slot->console->NumFrames != frame)
        {
            State.error = "NDS Local Wireless consoles are not at the same checkpoint frame.";
            return false;
        }
    }

    std::vector<std::uint8_t> checkpoint;
    checkpoint.reserve(32 * 1024 * 1024);
    Append32(checkpoint, CheckpointMagic);
    Append32(checkpoint, CheckpointVersion);
    Append32(checkpoint, static_cast<std::uint32_t>(State.slots.size()));
    Append32(checkpoint, frame);
    Append64(checkpoint, State.multiplayerReplySequence.load(std::memory_order_acquire));
    for (const auto& slot : State.slots)
    {
        Append32(checkpoint, slot->input.keys);
        Append32(checkpoint, slot->input.touching ? 1U : 0U);
        Append32(checkpoint, slot->input.touchX);
        Append32(checkpoint, slot->input.touchY);
        Append64(checkpoint, slot->context.packetsSent);
        Append64(checkpoint, slot->context.packetsReceived);
        Append64(checkpoint, slot->context.commands);
        Append64(checkpoint, slot->context.replies);
        Append64(checkpoint, slot->context.multiplayerCalls);
        Append64(checkpoint, slot->context.replyBaseline);
        Append32(checkpoint, slot->context.awaitingReplies ? 1U : 0U);

        melonDS::Savestate state;
        if (state.Error || !slot->console->DoSavestate(&state) || state.Error || state.Length() > MaximumConsoleStateBytes)
        {
            State.error = "Could not serialize a Nintendo DS checkpoint.";
            return false;
        }
        Append32(checkpoint, state.Length());
        const auto* stateBytes = static_cast<const std::uint8_t*>(state.Buffer());
        checkpoint.insert(checkpoint.end(), stateBytes, stateBytes + state.Length());
    }

    const std::vector<std::uint8_t> multiplayer = State.multiplayer->SerializeState();
    if (multiplayer.empty() || multiplayer.size() > MaximumMultiplayerStateBytes)
    {
        State.error = "Could not serialize the NDS Local Wireless radio checkpoint.";
        return false;
    }
    Append32(checkpoint, static_cast<std::uint32_t>(multiplayer.size()));
    checkpoint.insert(checkpoint.end(), multiplayer.begin(), multiplayer.end());
    if (checkpoint.size() > MaximumCheckpointBytes)
    {
        State.error = "NDS Local Wireless checkpoint exceeded its safety limit.";
        return false;
    }

    State.checkpoint = std::move(checkpoint);
    return true;
}

bool ImportCheckpoint(std::uint8_t* data, std::uint32_t length)
{
    State.error.clear();
    if (!State.loaded
#ifdef REBIT_MELONDS_ROLLBACK
        || RollbackFaulted
#endif
        || !data
        || length < 20
        || length > MaximumCheckpointBytes
        || !State.multiplayer
        || !RuntimeAtCheckpointBoundary())
    {
        State.error = "NDS Local Wireless checkpoint is unavailable or unsafe to import.";
        return false;
    }

    const std::uint8_t* cursor = data;
    const std::uint8_t* end = data + length;
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t players = 0;
    std::uint32_t frame = 0;
    std::uint64_t replySequence = 0;
    if (!Read32(cursor, end, magic)
        || !Read32(cursor, end, version)
        || !Read32(cursor, end, players)
        || !Read32(cursor, end, frame)
        || !Read64(cursor, end, replySequence)
        || magic != CheckpointMagic
        || version != CheckpointVersion
        || players != State.slots.size())
    {
        State.error = "NDS Local Wireless checkpoint header does not match this runtime.";
        return false;
    }

    std::array<CheckpointSlot, MaximumPlayers> slots {};
    for (std::uint32_t player = 0; player < players; ++player)
    {
        std::uint32_t touching = 0;
        std::uint32_t touchX = 0;
        std::uint32_t touchY = 0;
        if (!Read32(cursor, end, slots[player].input.keys)
            || !Read32(cursor, end, touching)
            || !Read32(cursor, end, touchX)
            || !Read32(cursor, end, touchY)
            || touching > 1
            || touchX > 255
            || touchY > 191)
        {
            State.error = "NDS Local Wireless checkpoint input state is invalid.";
            return false;
        }
        slots[player].input.touching = touching != 0;
        slots[player].input.touchX = static_cast<std::uint16_t>(touchX);
        slots[player].input.touchY = static_cast<std::uint16_t>(touchY);
        for (std::uint64_t& counter : slots[player].counters)
        {
            if (!Read64(cursor, end, counter))
            {
                State.error = "NDS Local Wireless checkpoint counters are truncated.";
                return false;
            }
        }
        std::uint32_t awaitingReplies = 0;
        if (!Read64(cursor, end, slots[player].replyBaseline)
            || !Read32(cursor, end, awaitingReplies)
            || awaitingReplies > 1)
        {
            State.error = "NDS Local Wireless checkpoint reply state is invalid.";
            return false;
        }
        slots[player].awaitingReplies = awaitingReplies != 0;
        if (!ReadSlice(cursor, end, MaximumConsoleStateBytes, slots[player].state, slots[player].stateLength))
        {
            State.error = "NDS Local Wireless console checkpoint is invalid.";
            return false;
        }
    }

    const std::uint8_t* multiplayer = nullptr;
    std::uint32_t multiplayerLength = 0;
    if (!ReadSlice(cursor, end, MaximumMultiplayerStateBytes, multiplayer, multiplayerLength) || cursor != end)
    {
        State.error = "NDS Local Wireless radio checkpoint is invalid.";
        return false;
    }

    std::lock_guard<std::mutex> guard(State.frameMutex);
#ifdef REBIT_MELONDS_ROLLBACK
    for (auto& record : RollbackRing) record.frame = UINT32_MAX;
    // Deserialization can fail after changing one console. Never execute or
    // export that half-restored timeline; only a fully restored pair is usable.
    RollbackFaulted = true;
#endif
    for (std::uint32_t player = 0; player < players; ++player)
    {
        melonDS::Savestate state(const_cast<std::uint8_t*>(slots[player].state), slots[player].stateLength, false);
        if (state.Error || !State.slots[player]->console->DoSavestate(&state) || state.Error
            || State.slots[player]->console->NumFrames != frame)
        {
            State.error = "Could not restore a Nintendo DS checkpoint.";
            return false;
        }
    }
    if (!State.multiplayer->DeserializeState(multiplayer, multiplayerLength))
    {
        State.error = "Could not restore the NDS Local Wireless radio checkpoint.";
        return false;
    }
    for (std::uint32_t player = 0; player < players; ++player)
    {
        auto& slot = *State.slots[player];
        slot.input = slots[player].input;
        slot.context.packetsSent = slots[player].counters[0];
        slot.context.packetsReceived = slots[player].counters[1];
        slot.context.commands = slots[player].counters[2];
        slot.context.replies = slots[player].counters[3];
        slot.context.multiplayerCalls = slots[player].counters[4];
        slot.context.replyBaseline = slots[player].replyBaseline;
        slot.context.awaitingReplies = slots[player].awaitingReplies;
        CopyFramebuffer(slot);
    }
    State.multiplayerReplySequence.store(replySequence, std::memory_order_release);
#ifdef REBIT_MELONDS_ROLLBACK
    RollbackFaulted = false;
#endif
    return true;
}

void CopyFramebuffer(Slot& slot)
{
    void* topRaw = nullptr;
    void* bottomRaw = nullptr;
    if (!slot.console || !slot.console->GPU.GetFramebuffers(&topRaw, &bottomRaw) || !topRaw || !bottomRaw)
        return;

    const auto copyScreen = [&](const auto* source, int rowOffset) {
        auto* destination = slot.framebuffer.data() + rowOffset * ScreenWidth;
        for (int pixel = 0; pixel < ScreenWidth * ScreenHeight; ++pixel)
        {
            const std::uint32_t bgra = source[pixel];
            const std::uint32_t red = (bgra >> 16) & 0xFF;
            const std::uint32_t green = (bgra >> 8) & 0xFF;
            const std::uint32_t blue = bgra & 0xFF;
            destination[pixel] = red | (green << 8) | (blue << 16) | 0xFF000000U;
        }
    };

    copyScreen(static_cast<const std::uint32_t*>(topRaw), 0);
    copyScreen(static_cast<const std::uint32_t*>(bottomRaw), ScreenHeight);
}

#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
bool RunCooperativeFrame()
{
    State.cooperativeSlices = 0;
    for (auto& slot : State.slots)
    {
        slot->cooperativeFrameComplete = false;
        const InputState input = slot->input;
        slot->console->SetKeyMask(input.keys);
        if (input.touching)
            slot->console->TouchScreen(input.touchX, input.touchY);
        else
            slot->console->ReleaseScreen();
        if (!slot->console->BeginCooperativeFrame())
        {
            State.error = "NDS Local Wireless cooperative console was already advancing a frame.";
            return false;
        }
    }

    BeginMultiplayerFrame();
    constexpr std::uint64_t MaximumCooperativeSlicesPerFrame = 2'000'000;
    std::size_t nextSlot = State.slots.size() > 1 ? 1 : 0;
    while (true)
    {
        bool allComplete = true;
        for (std::size_t offset = 0; offset < State.slots.size(); ++offset)
        {
            const std::size_t index = (nextSlot + offset) % State.slots.size();
            auto& slot = State.slots[index];
            if (slot->cooperativeFrameComplete)
                continue;
            allComplete = false;
            slot->cooperativeFrameComplete = slot->console->RunCooperativeFrameSlice();
            nextSlot = (index + 1) % State.slots.size();
            ++State.cooperativeSlices;
            if (slot->cooperativeFrameComplete)
                CompleteMultiplayerFrame(slot->context.id);
            if (State.cooperativeSlices > MaximumCooperativeSlicesPerFrame)
            {
                State.error = "NDS Local Wireless cooperative scheduler exceeded its synchronized frame budget.";
                CancelMultiplayerFrame();
                return false;
            }
            break;
        }
        if (allComplete)
            return true;
    }
}
#else
void WorkerLoop(Slot* slot)
{
    std::unique_lock<std::mutex> lock(State.frameMutex);
    while (true)
    {
        State.frameStart.wait(lock, [&] { return State.shuttingDown || State.generation > slot->observedGeneration; });
        if (State.shuttingDown)
            break;

        slot->observedGeneration = State.generation;
        const InputState input = slot->input;
        lock.unlock();

        slot->console->SetKeyMask(input.keys);
        if (input.touching)
            slot->console->TouchScreen(input.touchX, input.touchY);
        else
            slot->console->ReleaseScreen();
        slot->console->RunFrame();
        CompleteMultiplayerFrame(slot->context.id);

        lock.lock();
        ++State.completed;
        if (State.completed == static_cast<int>(State.slots.size()))
            State.frameComplete.notify_one();
    }
}
#endif

void StopRuntime()
{
#ifdef REBIT_MELONDS_ROLLBACK
    RollbackRing.clear();
    RollbackBytes = 0;
    RollbackFaulted = false;
#endif
    CancelMultiplayerFrame();
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    State.shuttingDown = true;
#else
    {
        std::lock_guard<std::mutex> guard(State.frameMutex);
        State.shuttingDown = true;
        ++State.generation;
    }
    State.frameStart.notify_all();

    for (auto& slot : State.slots)
    {
        if (slot->worker.joinable())
            slot->worker.join();
    }
#endif

    for (auto& slot : State.slots)
    {
        if (slot->console)
            slot->console->Stop();
        slot->console.reset();
    }

    State.slots.clear();
    State.multiplayer.reset();
    State.checkpoint.clear();
    State.visiblePlayer = 0;
    State.generation = 0;
    State.completed = 0;
    State.shuttingDown = false;
    State.multiplayerTurn.store(-1, std::memory_order_release);
    State.multiplayerReplySequence.store(0, std::memory_order_release);
    State.schedulerJitterProfile.store(0, std::memory_order_relaxed);
    State.loaded = false;
    State.lastFrameMs = 0.0;
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    State.cooperativeSlices = 0;
#endif
}

std::unique_ptr<melonDS::NDS> CreateConsole(const std::uint8_t* rom, std::uint32_t romLength, Slot& slot, std::uint64_t seed)
{
    melonDS::Firmware firmware(0);
    auto& header = firmware.GetHeader();
    header.MacAddr = {
        0x02,
        0x09,
        0xBF,
        static_cast<std::uint8_t>((seed >> 0) + slot.context.id),
        static_cast<std::uint8_t>((seed >> 8) + slot.context.id * 0x44),
        static_cast<std::uint8_t>((seed >> 16) + slot.context.id * 0x10),
    };
    header.UpdateChecksum();
    firmware.UpdateChecksums();

    melonDS::NDSArgs arguments;
    arguments.Firmware = std::move(firmware);
#ifdef REBIT_MELONDS_DUAL_NATIVE_JIT
    arguments.JIT = melonDS::JITArgs();
#else
    arguments.JIT = std::nullopt;
#endif
    arguments.BitDepth = melonDS::AudioBitDepth::_16Bit;
    arguments.Interpolation = melonDS::AudioInterpolation::None;
    arguments.OutputSampleRate = 48000.0;

    auto console = std::make_unique<melonDS::NDS>(std::move(arguments), &slot.context);
    auto cart = melonDS::NDSCart::ParseROM(rom, romLength, &slot.context);
    if (!cart)
        return nullptr;

    console->SetNDSCart(std::move(cart));
    console->Reset();
    console->RTC.SetDateTime(2000, 1, 1, 0, 0, 0);
    console->SetupDirectBoot("rebit.nds");

    melonDS::RendererSettings rendererSettings {
        .ScaleFactor = 1,
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
        .Threaded = false,
#else
        .Threaded = true,
#endif
        .HiresCoordinates = false,
        .BetterPolygons = false,
    };
    console->GetRenderer().SetOutputEnabled(slot.context.id == State.visiblePlayer);
    console->GetRenderer().SetRenderSettings(rendererSettings);
    console->SPU.SetOutputEnabled(slot.context.id == State.visiblePlayer);
    console->Start();
    return console;
}

Slot* GetSlot(int player) noexcept
{
    if (player < 0 || player >= static_cast<int>(State.slots.size()))
        return nullptr;
    return State.slots[player].get();
}

}

int InstanceId(void* userdata) noexcept
{
    auto* context = static_cast<SlotContext*>(userdata);
    return context ? context->id : 0;
}

SlotContext* Context(void* userdata) noexcept
{
    return static_cast<SlotContext*>(userdata);
}

melonDS::LocalMP* LocalMultiplayer() noexcept
{
    return State.multiplayer.get();
}

#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
bool MultiplayerPacketsReady(void* userdata) noexcept
{
    return State.multiplayer && State.multiplayer->PacketsReady(InstanceId(userdata));
}

bool MultiplayerRepliesReady(void* userdata) noexcept
{
    return State.multiplayer && State.multiplayer->RepliesReady(InstanceId(userdata));
}

void RequestMultiplayerYield(void* userdata) noexcept
{
    const int player = InstanceId(userdata);
    if (auto* slot = GetSlot(player); slot && slot->console)
        slot->console->RequestCooperativeYield();
}
#endif

bool EnterMultiplayerTurn(void* userdata, MultiplayerOperation operation) noexcept
{
    auto* context = Context(userdata);
    const int player = InstanceId(userdata);
    if (!context || player < 0 || player >= static_cast<int>(State.slots.size()))
        return false;
    ApplySchedulerJitter(*context);
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    (void)operation;
    // The cooperative runtime invokes exactly one console at a time. Every
    // LocalMP call is therefore already serialized, and the RAII scope asks
    // the NDS frame stepper to hand control to the next console immediately
    // after the operation completes.
    return State.multiplayerFrameActive.load(std::memory_order_acquire)
        && (State.multiplayerDoneMask.load(std::memory_order_acquire) & (1U << player)) == 0;
#else

    const std::uint32_t playerBit = 1U << player;
    if (operation == MultiplayerOperation::RecvReplies && context->awaitingReplies)
    {
        context->awaitingReplies = false;
        const std::uint64_t baseline = context->replyBaseline;
        const std::uint32_t allPlayersMask = (1U << State.slots.size()) - 1U;
        const std::uint32_t otherPlayersMask = allPlayersMask & ~playerBit;
        while (State.multiplayerFrameActive.load(std::memory_order_acquire)
            && State.multiplayerReplySequence.load(std::memory_order_acquire) <= baseline
            && (State.multiplayerDoneMask.load(std::memory_order_acquire) & otherPlayersMask) != otherPlayersMask)
        {
            const std::uint32_t observedSequence = State.multiplayerProgressSequence.load(std::memory_order_acquire);
            int expected = player;
            const std::uint32_t doneMask = State.multiplayerDoneMask.load(std::memory_order_acquire);
            const int next = NextMultiplayerTurn(player, doneMask);
            if (next >= 0 && next != player)
            {
                if (State.multiplayerTurn.compare_exchange_strong(expected, next, std::memory_order_acq_rel))
                    NotifyMultiplayerProgress();
            }
            if (State.multiplayerFrameActive.load(std::memory_order_acquire)
                && State.multiplayerReplySequence.load(std::memory_order_acquire) <= baseline
                && (State.multiplayerDoneMask.load(std::memory_order_acquire) & otherPlayersMask) != otherPlayersMask)
                WaitForMultiplayerProgress(observedSequence);
        }
    }

    while (State.multiplayerFrameActive.load(std::memory_order_acquire)
        && (State.multiplayerDoneMask.load(std::memory_order_acquire) & playerBit) == 0
        && State.multiplayerTurn.load(std::memory_order_acquire) != player)
    {
        const std::uint32_t observedSequence = State.multiplayerProgressSequence.load(std::memory_order_acquire);
        if (State.multiplayerFrameActive.load(std::memory_order_acquire)
            && (State.multiplayerDoneMask.load(std::memory_order_acquire) & playerBit) == 0
            && State.multiplayerTurn.load(std::memory_order_acquire) != player)
            WaitForMultiplayerProgress(observedSequence);
    }

    return State.multiplayerFrameActive.load(std::memory_order_acquire)
        && (State.multiplayerDoneMask.load(std::memory_order_acquire) & playerBit) == 0
        && State.multiplayerTurn.load(std::memory_order_acquire) == player;
#endif
}

bool DeferMultiplayerReceivePoll(void* userdata, MultiplayerOperation operation) noexcept
{
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    (void)userdata;
    (void)operation;
    return false;
#else
    const int player = InstanceId(userdata);
    if (!State.multiplayerFrameActive.load(std::memory_order_acquire)
        || player < 0
        || player >= static_cast<int>(State.slots.size()))
        return false;
    auto& slot = *State.slots[player];
    int* polls = nullptr;
    int batch = 0;
    if (operation == MultiplayerOperation::RecvPacket)
    {
        polls = &slot.multiplayerPacketReceivePolls;
        batch = MultiplayerPacketReceivePollBatch;
    }
    else if (operation == MultiplayerOperation::RecvHostPacket)
    {
        polls = &slot.multiplayerHostReceivePolls;
        batch = MultiplayerHostReceivePollBatch;
    }
    if (!polls)
        return false;
    if (++*polls < batch)
        return true;
    *polls = 0;
    return false;
#endif
}

void LeaveMultiplayerTurn(void* userdata, bool scheduled) noexcept
{
    if (!scheduled)
        return;
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    RequestMultiplayerYield(userdata);
#else
    const int player = InstanceId(userdata);
    if (player >= 0 && player < static_cast<int>(State.slots.size()))
    {
        int expected = player;
        const std::uint32_t doneMask = State.multiplayerDoneMask.load(std::memory_order_acquire);
        State.multiplayerTurn.compare_exchange_strong(
            expected,
            NextMultiplayerTurn(player, doneMask),
            std::memory_order_acq_rel);
        NotifyMultiplayerProgress();
    }
#endif
}

void NoteMultiplayerCommand(void* userdata) noexcept
{
    auto* context = Context(userdata);
    if (!context)
        return;
    context->replyBaseline = State.multiplayerReplySequence.load(std::memory_order_acquire);
    context->awaitingReplies = true;
}

void NoteMultiplayerReply() noexcept
{
    State.multiplayerReplySequence.fetch_add(1, std::memory_order_acq_rel);
    NotifyMultiplayerProgress();
}

void SignalStopped(void* userdata) noexcept
{
    if (auto* context = Context(userdata))
        context->stopped = true;
}

void StoreSave(const std::uint8_t* data, std::uint32_t length, void* userdata)
{
    auto* context = Context(userdata);
    if (!context || !data || length == 0)
        return;
    context->latestSave.assign(data, data + length);
}

}

extern "C"
{

REBIT_EXPORT std::uint32_t md_api_version()
{
    return REBIT_MELONDS_DUAL_API_VERSION;
}

REBIT_EXPORT const char* md_runtime_abi()
{
    return REBIT_MELONDS_DUAL_RUNTIME_ABI;
}

REBIT_EXPORT int md_load(const std::uint8_t* rom, std::uint32_t romLength, int players, std::uint32_t seedLow, std::uint32_t seedHigh)
{
    using namespace rebit;
    StopRuntime();
    State.error.clear();

    if (!rom || romLength < 4096)
    {
        State.error = "Nintendo DS ROM data is missing or too small.";
        return 0;
    }
    if (players < MinimumPlayers || players > MaximumPlayers)
    {
        State.error = "NDS Local Wireless requires between two and four players.";
        return 0;
    }

    try
    {
        State.multiplayer = std::make_unique<melonDS::LocalMP>();
        State.multiplayer->SetRecvTimeout(LocalMultiplayerReceiveTimeoutMs);
        const std::uint64_t seed = static_cast<std::uint64_t>(seedLow) | (static_cast<std::uint64_t>(seedHigh) << 32);

        for (int player = 0; player < players; ++player)
        {
            auto slot = std::make_unique<Slot>();
            slot->context.id = player;
            slot->console = CreateConsole(rom, romLength, *slot, seed);
            if (!slot->console)
            {
                State.error = "Standalone melonDS rejected the Nintendo DS ROM.";
                StopRuntime();
                return 0;
            }
            State.slots.push_back(std::move(slot));
        }

#ifndef REBIT_MELONDS_DUAL_COOPERATIVE
        for (auto& slot : State.slots)
            slot->worker = std::thread(WorkerLoop, slot.get());
#endif

        State.loaded = true;
        return 1;
    }
    catch (const std::exception& exception)
    {
        State.error = exception.what();
    }
    catch (...)
    {
        State.error = "Unknown error while starting NDS Local Wireless.";
    }

    StopRuntime();
    return 0;
}

REBIT_EXPORT void md_destroy()
{
    rebit::StopRuntime();
}

REBIT_EXPORT int md_is_loaded()
{
    return rebit::State.loaded ? 1 : 0;
}

REBIT_EXPORT int md_player_count()
{
    return static_cast<int>(rebit::State.slots.size());
}

REBIT_EXPORT void md_set_visible_player(int player)
{
    if (rebit::GetSlot(player))
    {
#ifdef REBIT_MELONDS_ROLLBACK
        if (rebit::RollbackFaulted || !rebit::RuntimeAtCheckpointBoundary())
        { rebit::State.error = "NDS rollback view change requires a healthy frame boundary."; return; }
        std::lock_guard<std::mutex> guard(rebit::State.frameMutex);
        if (rebit::State.visiblePlayer != player)
            for (auto& record : rebit::RollbackRing) record.frame = UINT32_MAX;
#endif
        rebit::State.visiblePlayer = player;
        for (const auto& slot : rebit::State.slots)
        {
            slot->console->GetRenderer().SetOutputEnabled(slot->context.id == player);
            slot->console->SPU.SetOutputEnabled(slot->context.id == player);
        }
    }
}

REBIT_EXPORT int md_visible_player()
{
    return rebit::State.visiblePlayer;
}

REBIT_EXPORT int md_set_input(int player, std::uint32_t keys, int touching, int touchX, int touchY)
{
    auto* slot = rebit::GetSlot(player);
    if (!slot)
        return 0;
    std::lock_guard<std::mutex> guard(rebit::State.frameMutex);
    slot->input.keys = keys & 0x0FFF;
    slot->input.touching = touching != 0;
    slot->input.touchX = static_cast<std::uint16_t>(std::clamp(touchX, 0, 255));
    slot->input.touchY = static_cast<std::uint16_t>(std::clamp(touchY, 0, 191));
    return 1;
}

REBIT_EXPORT int md_run_frame()
{
    using namespace rebit;
    if (!State.loaded || State.slots.empty())
        return 0;
#ifdef REBIT_MELONDS_ROLLBACK
    for (const auto& slot : State.slots)
        if (!slot->console->GetRenderer().RollbackHealthy())
        {
            RollbackFaulted = true;
            State.error = "NDS rollback renderer watchdog fired; rebuild the runtime for recovery.";
        }
    if (RollbackFaulted) return 0;
#endif

    const auto started = std::chrono::steady_clock::now();
#ifdef REBIT_MELONDS_DUAL_COOPERATIVE
    if (!RunCooperativeFrame())
        return 0;
#else
    BeginMultiplayerFrame();
    {
        std::unique_lock<std::mutex> lock(State.frameMutex);
        State.completed = 0;
        ++State.generation;
        State.frameStart.notify_all();
        const auto completed = [&] {
            return State.completed == static_cast<int>(State.slots.size()) || State.shuttingDown;
        };
        if (!State.frameComplete.wait_for(lock, MultiplayerFrameWatchdog, completed))
        {
#ifdef REBIT_MELONDS_ROLLBACK
            // Finish joining the workers below, but never accept a speculative
            // frame produced through a wall-clock-dependent emergency schedule.
            RollbackFaulted = true;
#endif
            // Never strand the browser on a starved LocalMP turn. Cancelling
            // only the per-frame turn barrier lets both consoles finish; the
            // periodic cross-peer hash and checkpoint protocol repair any
            // state divergence caused by this emergency path.
            CancelMultiplayerFrame();
            if (!State.frameComplete.wait_for(lock, MultiplayerRecoveryWatchdog, completed))
            {
                State.error = "NDS Local Wireless console workers did not finish a synchronized frame.";
                return 0;
            }
        }
    }
#endif

    State.lastFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
#ifdef REBIT_MELONDS_ROLLBACK
    if (RollbackFaulted)
    {
        State.error = "NDS rollback worker watchdog fired; coordinated recovery is required.";
        return 0;
    }
#endif
    return 1;
}

#ifdef REBIT_MELONDS_ROLLBACK
// These APIs intentionally accept no remote state bytes. Ring slots are
// private, frame-tagged, and invalidated at every lifecycle/recovery boundary.
REBIT_EXPORT std::uint32_t md_rollback_configure(std::uint32_t capacity)
{
    using namespace rebit;
    State.error.clear();
    if (!State.loaded || RollbackFaulted || !RuntimeAtCheckpointBoundary() || capacity < 2 || capacity > 9)
    { State.error = "NDS rollback ring configuration is invalid."; return 0; }
    std::lock_guard<std::mutex> guard(State.frameMutex);
    try
    {
        RollbackRing.clear();
        RollbackBytes = 0;
        std::array<std::uint32_t, MaximumPlayers + 1> lengths {};
        for (std::size_t index = 0; index <= State.slots.size(); ++index)
        {
            melonDS::Savestate state(12 * 1024 * 1024);
            state.Rollback = true;
            if (index == State.slots.size()) State.multiplayer->DoRollbackState(&state);
            else if (!State.slots[index]->console->DoSavestate(&state)) state.Error = true;
            if (state.Error) { State.error = "Could not measure NDS rollback state."; return 0; }
            lengths[index] = state.Length();
            RollbackBytes += state.Length();
        }
        if (static_cast<std::uint64_t>(RollbackBytes) * capacity > 192 * 1024 * 1024)
        { RollbackBytes = 0; State.error = "NDS rollback ring exceeds its memory budget."; return 0; }
        RollbackRing.resize(capacity);
        for (auto& record : RollbackRing)
            for (std::size_t index = 0; index <= State.slots.size(); ++index)
                record.parts[index].resize(lengths[index]);
    }
    catch (const std::bad_alloc&)
    {
        RollbackRing.clear();
        RollbackBytes = 0;
        State.error = "Not enough memory for the NDS rollback ring.";
    }
    return RollbackBytes;
}

REBIT_EXPORT int md_rollback_save_slot(std::uint32_t index, std::uint32_t frame)
{
    using namespace rebit;
    if (!State.loaded || RollbackFaulted || !RuntimeAtCheckpointBoundary()
        || index >= RollbackRing.size() || frame == UINT32_MAX)
    { State.error = "NDS rollback save requested outside a valid frame boundary."; return 0; }
    std::lock_guard<std::mutex> guard(State.frameMutex);
    auto& record = RollbackRing[index];
    record.frame = UINT32_MAX;
    for (const auto& slot : State.slots)
        if (slot->console->NumFrames != frame)
        { State.error = "NDS rollback save frame does not match the consoles."; return 0; }
    for (std::size_t player = 0; player <= State.slots.size(); ++player)
    {
        auto& bytes = record.parts[player];
        melonDS::Savestate state(bytes.data(), bytes.size(), true);
        state.Rollback = true;
        if (player == State.slots.size()) State.multiplayer->DoRollbackState(&state);
        else
        {
            auto& slot = *State.slots[player];
            if (!slot.console->DoSavestate(&state)) state.Error = true;
            record.inputs[player] = slot.input;
            const auto& context = slot.context;
            record.context[player] = { context.packetsSent, context.packetsReceived, context.commands,
                context.replies, context.multiplayerCalls, context.replyBaseline, context.awaitingReplies ? 1U : 0U };
        }
        if (state.Error || state.Length() != bytes.size())
        { State.error = "NDS rollback state geometry changed or serialization failed."; return 0; }
    }
    record.replySequence = State.multiplayerReplySequence.load(std::memory_order_acquire);
    record.frame = frame;
    return 1;
}

REBIT_EXPORT int md_rollback_load_slot(std::uint32_t index, std::uint32_t frame)
{
    using namespace rebit;
    if (!State.loaded || RollbackFaulted || !RuntimeAtCheckpointBoundary()
        || index >= RollbackRing.size() || frame == UINT32_MAX || RollbackRing[index].frame != frame)
    { State.error = "NDS rollback slot is unavailable or has been overwritten."; return 0; }
    std::lock_guard<std::mutex> guard(State.frameMutex);
    const auto& record = RollbackRing[index];
    for (std::size_t player = 0; player <= State.slots.size(); ++player)
    {
        const auto& bytes = record.parts[player];
        melonDS::Savestate state(const_cast<std::uint8_t*>(bytes.data()), bytes.size(), false);
        state.Rollback = true;
        if (player == State.slots.size()) State.multiplayer->DoRollbackState(&state);
        else
        {
            auto& slot = *State.slots[player];
            if (!slot.console->DoSavestate(&state) || slot.console->NumFrames != frame) state.Error = true;
            slot.input = record.inputs[player];
            auto& context = slot.context;
            const auto& values = record.context[player];
            context.packetsSent = values[0]; context.packetsReceived = values[1];
            context.commands = values[2]; context.replies = values[3]; context.multiplayerCalls = values[4];
            context.replyBaseline = values[5]; context.awaitingReplies = values[6] != 0;
        }
        if (state.Error)
        {
            RollbackFaulted = true;
            State.error = "NDS rollback restoration failed; coordinated recovery is required.";
            return 0;
        }
    }
    State.multiplayerReplySequence.store(record.replySequence, std::memory_order_release);
    return 1;
}

REBIT_EXPORT std::uint32_t md_rollback_part_size(std::uint32_t index, std::uint32_t part)
{
    using namespace rebit;
    return index < RollbackRing.size() && part <= State.slots.size() && RollbackRing[index].frame != UINT32_MAX
        ? RollbackRing[index].parts[part].size() : 0;
}

REBIT_EXPORT const std::uint8_t* md_rollback_part_data(std::uint32_t index, std::uint32_t part)
{
    return md_rollback_part_size(index, part) ? rebit::RollbackRing[index].parts[part].data() : nullptr;
}
#endif

REBIT_EXPORT std::uint32_t md_frame(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot && slot->console ? slot->console->NumFrames : 0;
}

REBIT_EXPORT const std::uint32_t* md_framebuffer(int player)
{
    auto* slot = rebit::GetSlot(player);
    if (!slot)
        return nullptr;
    rebit::CopyFramebuffer(*slot);
    return slot->framebuffer.data();
}

REBIT_EXPORT int md_width() { return rebit::ScreenWidth; }
REBIT_EXPORT int md_height() { return rebit::CombinedHeight; }
REBIT_EXPORT int md_audio_sample_rate() { return 48000; }

REBIT_EXPORT int md_audio_available()
{
    auto* slot = rebit::GetSlot(rebit::State.visiblePlayer);
    return slot && slot->console ? slot->console->SPU.GetOutputSize() : 0;
}

REBIT_EXPORT int md_audio_read(int maximumFrames)
{
    auto* slot = rebit::GetSlot(rebit::State.visiblePlayer);
    if (!slot || !slot->console)
        return 0;
    const int requested = std::clamp(maximumFrames, 0, rebit::AudioScratchFrames);
    return slot->console->SPU.ReadOutput(rebit::State.audio.data(), requested);
}

REBIT_EXPORT const std::int16_t* md_audio_buffer()
{
    return rebit::State.audio.data();
}

REBIT_EXPORT std::uint32_t md_save_size(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot && slot->console ? slot->console->GetNDSSaveLength() : 0;
}

REBIT_EXPORT const std::uint8_t* md_save_data(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot && slot->console ? slot->console->GetNDSSave() : nullptr;
}

REBIT_EXPORT int md_import_save(int player, const std::uint8_t* data, std::uint32_t length)
{
    auto* slot = rebit::GetSlot(player);
    if (!slot || !slot->console)
        return 0;

    const std::uint32_t expected = slot->console->GetNDSSaveLength();
    if (!data || length == 0 || expected == 0 || length != expected)
        return 0;

#ifdef REBIT_MELONDS_ROLLBACK
    if (rebit::RollbackFaulted || !rebit::RuntimeAtCheckpointBoundary())
    { rebit::State.error = "NDS rollback save import requires a healthy frame boundary."; return 0; }
    std::lock_guard<std::mutex> guard(rebit::State.frameMutex);
    for (auto& record : rebit::RollbackRing) record.frame = UINT32_MAX;
#endif
    slot->console->SetNDSSave(data, length);
    slot->context.latestSave.assign(data, data + length);
    return 1;
}

REBIT_EXPORT int md_export_checkpoint()
{
    return rebit::ExportCheckpoint() ? 1 : 0;
}

REBIT_EXPORT std::uint32_t md_checkpoint_size()
{
    return static_cast<std::uint32_t>(rebit::State.checkpoint.size());
}

REBIT_EXPORT const std::uint8_t* md_checkpoint_data()
{
    return rebit::State.checkpoint.empty() ? nullptr : rebit::State.checkpoint.data();
}

REBIT_EXPORT void md_clear_checkpoint()
{
    std::vector<std::uint8_t>().swap(rebit::State.checkpoint);
}

REBIT_EXPORT int md_import_checkpoint(std::uint8_t* data, std::uint32_t length)
{
    return rebit::ImportCheckpoint(data, length) ? 1 : 0;
}

REBIT_EXPORT void md_set_scheduler_jitter(std::uint32_t profile)
{
    rebit::State.schedulerJitterProfile.store(profile, std::memory_order_relaxed);
}

REBIT_EXPORT int md_inject_desync_for_test(int player, std::uint32_t offset, std::uint32_t value)
{
    auto* slot = rebit::GetSlot(player);
    if (!slot || !slot->console || value == 0 || !rebit::RuntimeAtCheckpointBoundary())
        return 0;
    if (offset == std::numeric_limits<std::uint32_t>::max())
        offset = slot->console->MainRAMMask;
    if (offset > slot->console->MainRAMMask)
        return 0;
    slot->console->MainRAM[offset] ^= static_cast<std::uint8_t>(value);
    return 1;
}

REBIT_EXPORT std::uint32_t md_state_hash()
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& slot : rebit::State.slots)
    {
        hash = rebit::Fnv1a(&slot->console->NumFrames, sizeof(slot->console->NumFrames), hash);
        hash = rebit::Fnv1a(slot->console->MainRAM, slot->console->MainRAMMask + 1, hash);
    }
    return static_cast<std::uint32_t>(hash ^ (hash >> 32));
}

REBIT_EXPORT std::uint32_t md_mp_packets_sent(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot ? static_cast<std::uint32_t>(slot->context.packetsSent) : 0;
}

REBIT_EXPORT std::uint32_t md_mp_packets_received(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot ? static_cast<std::uint32_t>(slot->context.packetsReceived) : 0;
}

REBIT_EXPORT std::uint32_t md_mp_commands(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot ? static_cast<std::uint32_t>(slot->context.commands) : 0;
}

REBIT_EXPORT std::uint32_t md_mp_replies(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot ? static_cast<std::uint32_t>(slot->context.replies) : 0;
}

REBIT_EXPORT double md_last_frame_ms()
{
    return rebit::State.lastFrameMs;
}

REBIT_EXPORT const char* md_build_id()
{
    return REBIT_MELONDS_DUAL_BUILD;
}

REBIT_EXPORT const char* md_last_error()
{
    return rebit::State.error.c_str();
}

}
