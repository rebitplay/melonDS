#include "rebit_melonds_dual.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

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
            std::fprintf(stderr, "frame %d failed: %s\n", frame, md_last_error());
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
        "{\"build\":\"%s\",\"runtime_abi\":\"%s\",\"api_version\":%u,\"frames\":%d,\"seconds\":%.6f,\"fps\":%.3f,\"slowest_frame_ms\":%.3f,\"state_hash\":%u,\"players\":[{\"frame\":%u,\"sent\":%u,\"received\":%u},{\"frame\":%u,\"sent\":%u,\"received\":%u}]}\n",
        md_build_id(), md_runtime_abi(), md_api_version(), frames, seconds, frames / seconds, slowest, md_state_hash(),
        md_frame(0), md_mp_packets_sent(0), md_mp_packets_received(0),
        md_frame(1), md_mp_packets_sent(1), md_mp_packets_received(1));
    md_destroy();
    return 0;
}
