#include "runtime_internal.h"

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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
constexpr int MultiplayerProgressSpinYields = 8;
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
    std::thread worker;
    InputState input;
    std::array<std::uint32_t, ScreenWidth * CombinedHeight> framebuffer {};
    std::uint64_t observedGeneration = 0;
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
};

Runtime State;

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
    State.multiplayerProgressSequence.notify_all();
}

void WaitForMultiplayerProgress(std::uint32_t observedSequence)
{
    // Most LocalMP turns are handed off within a few scheduler yields. Keep
    // that fast path lock-free, but bound it so a delayed browser worker
    // always falls back to the lossless sequence wait below.
    for (int attempt = 0; attempt < MultiplayerProgressSpinYields; ++attempt)
    {
        if (State.multiplayerProgressSequence.load(std::memory_order_acquire) != observedSequence)
            return;
        std::this_thread::yield();
    }
    // C++20 atomic wait closes the notify-before-wait race without a mutex or
    // a fixed wall-clock delay. It also maps directly to the browser futex.
    State.multiplayerProgressSequence.wait(observedSequence, std::memory_order_acquire);
}

void BeginMultiplayerFrame()
{
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
    const std::uint64_t call = context.multiplayerCalls.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t value = profile ^ (static_cast<std::uint32_t>(context.id) * 0x9E3779B9U)
        ^ (static_cast<std::uint32_t>(call) * 0x85EBCA6BU);
    value ^= value >> 16;
    for (std::uint32_t iteration = 0; iteration < (value & 0x3FU); ++iteration)
        std::this_thread::yield();
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
    if (!State.loaded || State.slots.empty() || !State.multiplayer || !RuntimeAtCheckpointBoundary())
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
        Append64(checkpoint, slot->context.packetsSent.load(std::memory_order_relaxed));
        Append64(checkpoint, slot->context.packetsReceived.load(std::memory_order_relaxed));
        Append64(checkpoint, slot->context.commands.load(std::memory_order_relaxed));
        Append64(checkpoint, slot->context.replies.load(std::memory_order_relaxed));
        Append64(checkpoint, slot->context.multiplayerCalls.load(std::memory_order_relaxed));
        Append64(checkpoint, slot->context.replyBaseline.load(std::memory_order_relaxed));
        Append32(checkpoint, slot->context.awaitingReplies.load(std::memory_order_relaxed) ? 1U : 0U);

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
        slot.context.packetsSent.store(slots[player].counters[0], std::memory_order_relaxed);
        slot.context.packetsReceived.store(slots[player].counters[1], std::memory_order_relaxed);
        slot.context.commands.store(slots[player].counters[2], std::memory_order_relaxed);
        slot.context.replies.store(slots[player].counters[3], std::memory_order_relaxed);
        slot.context.multiplayerCalls.store(slots[player].counters[4], std::memory_order_relaxed);
        slot.context.replyBaseline.store(slots[player].replyBaseline, std::memory_order_relaxed);
        slot.context.awaitingReplies.store(slots[player].awaitingReplies, std::memory_order_relaxed);
        CopyFramebuffer(slot);
    }
    State.multiplayerReplySequence.store(replySequence, std::memory_order_release);
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

void StopRuntime()
{
    CancelMultiplayerFrame();
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
    arguments.JIT = std::nullopt;
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
        .Threaded = true,
        .HiresCoordinates = false,
        .BetterPolygons = false,
    };
    console->GetRenderer().SetRenderSettings(rendererSettings);
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

bool EnterMultiplayerTurn(void* userdata, MultiplayerOperation operation) noexcept
{
    auto* context = Context(userdata);
    const int player = InstanceId(userdata);
    if (!context || player < 0 || player >= static_cast<int>(State.slots.size()))
        return false;
    ApplySchedulerJitter(*context);

    const std::uint32_t playerBit = 1U << player;
    if (operation == MultiplayerOperation::RecvReplies
        && context->awaitingReplies.exchange(false, std::memory_order_acq_rel))
    {
        const std::uint64_t baseline = context->replyBaseline.load(std::memory_order_acquire);
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
}

void LeaveMultiplayerTurn(void* userdata, bool scheduled) noexcept
{
    if (!scheduled)
        return;
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
}

void NoteMultiplayerCommand(void* userdata) noexcept
{
    auto* context = Context(userdata);
    if (!context)
        return;
    context->replyBaseline.store(State.multiplayerReplySequence.load(std::memory_order_acquire), std::memory_order_release);
    context->awaitingReplies.store(true, std::memory_order_release);
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

        for (auto& slot : State.slots)
            slot->worker = std::thread(WorkerLoop, slot.get());

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
        rebit::State.visiblePlayer = player;
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

    const auto started = std::chrono::steady_clock::now();
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

    if (auto* visible = GetSlot(State.visiblePlayer))
        CopyFramebuffer(*visible);

    State.lastFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return 1;
}

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
    return slot ? static_cast<std::uint32_t>(slot->context.packetsSent.load()) : 0;
}

REBIT_EXPORT std::uint32_t md_mp_packets_received(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot ? static_cast<std::uint32_t>(slot->context.packetsReceived.load()) : 0;
}

REBIT_EXPORT std::uint32_t md_mp_commands(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot ? static_cast<std::uint32_t>(slot->context.commands.load()) : 0;
}

REBIT_EXPORT std::uint32_t md_mp_replies(int player)
{
    auto* slot = rebit::GetSlot(player);
    return slot ? static_cast<std::uint32_t>(slot->context.replies.load()) : 0;
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

#ifndef __EMSCRIPTEN__
namespace
{

bool WritePpm(const std::string& path, const std::uint32_t* pixels, int width, int height)
{
    std::ofstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    stream << "P6\n" << width << ' ' << height << "\n255\n";
    for (int index = 0; index < width * height; ++index)
    {
        const std::uint32_t pixel = pixels[index];
        const char rgb[3] = {
            static_cast<char>(pixel & 0xFF),
            static_cast<char>((pixel >> 8) & 0xFF),
            static_cast<char>((pixel >> 16) & 0xFF),
        };
        stream.write(rgb, sizeof(rgb));
    }
    return stream.good();
}

}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: melonds_dual <rom.nds> [frames] [screenshot-prefix]\n");
        return 2;
    }

    std::ifstream romStream(argv[1], std::ios::binary | std::ios::ate);
    if (!romStream)
    {
        std::fprintf(stderr, "could not open ROM: %s\n", argv[1]);
        return 2;
    }
    const auto length = romStream.tellg();
    romStream.seekg(0);
    std::vector<std::uint8_t> rom(static_cast<std::size_t>(length));
    romStream.read(reinterpret_cast<char*>(rom.data()), length);

    const int frames = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 600;
    if (!md_load(rom.data(), static_cast<std::uint32_t>(rom.size()), 2, 0x12345678U, 0x9ABCDEF0U))
    {
        std::fprintf(stderr, "load failed: %s\n", md_last_error());
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    double slowest = 0.0;
    for (int frame = 0; frame < frames; ++frame)
    {
        if (!md_run_frame())
        {
            std::fprintf(stderr, "frame %d failed\n", frame);
            md_destroy();
            return 1;
        }
        slowest = std::max(slowest, md_last_frame_ms());
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    if (argc >= 4)
    {
        for (int player = 0; player < 2; ++player)
            WritePpm(std::string(argv[3]) + "-p" + std::to_string(player + 1) + ".ppm", md_framebuffer(player), md_width(), md_height());
    }

    std::printf(
        "{\"build\":\"%s\",\"frames\":%d,\"seconds\":%.6f,\"fps\":%.3f,\"slowest_frame_ms\":%.3f,\"state_hash\":%u,\"players\":[{\"frame\":%u,\"sent\":%u,\"received\":%u},{\"frame\":%u,\"sent\":%u,\"received\":%u}]}\n",
        md_build_id(), frames, seconds, frames / seconds, slowest, md_state_hash(),
        md_frame(0), md_mp_packets_sent(0), md_mp_packets_received(0),
        md_frame(1), md_mp_packets_sent(1), md_mp_packets_received(1));
    md_destroy();
    return 0;
}
#endif
