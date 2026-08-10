#include "runtime_internal.h"

#include "Args.h"
#include "GPU.h"
#include "LocalMP.h"
#include "NDS.h"
#include "NDSCart.h"
#include "SPI_Firmware.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
// All consoles advance concurrently behind the same frame barrier, so a peer
// only needs a short scheduling window to publish its LocalMP packet. The Qt
// frontend's longer network-oriented timeout needlessly stalls browser frames.
constexpr int LocalMultiplayerReceiveTimeoutMs = 5;

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
    std::string error;
    std::uint64_t generation = 0;
    int completed = 0;
    int visiblePlayer = 0;
    bool shuttingDown = false;
    bool loaded = false;
    double lastFrameMs = 0.0;
};

Runtime State;

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

        lock.lock();
        ++State.completed;
        if (State.completed == static_cast<int>(State.slots.size()))
            State.frameComplete.notify_one();
    }
}

void StopRuntime()
{
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
    State.visiblePlayer = 0;
    State.generation = 0;
    State.completed = 0;
    State.shuttingDown = false;
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
        .Threaded = false,
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
    {
        std::unique_lock<std::mutex> lock(State.frameMutex);
        State.completed = 0;
        ++State.generation;
        State.frameStart.notify_all();
        State.frameComplete.wait(lock, [&] { return State.completed == static_cast<int>(State.slots.size()) || State.shuttingDown; });
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
