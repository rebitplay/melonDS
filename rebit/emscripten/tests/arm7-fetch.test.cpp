// Ported from Rebit's remelonds ARM7 shared-WRAM fetch research fixture.
// WASM differential test of the exact generated fetch function.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
namespace melonDS {
using u32 = uint32_t;
using u8 = uint8_t;
struct TestNDS {
    int ConsoleType = 0;
    struct { u8* Mem; u32 Mask; } SWRAM_ARM7;
    static constexpr u32 ARM7WRAMSize = 0x10000;
    u8* ARM7WRAM;
};
struct ARMv4 {
    TestNDS& NDS;
    u32 busCalls = 0, lastAddress = 0;
    u32 CodeRead32(u32);
    u32 BusRead32(u32 address) { ++busCalls; lastAddress = address; return address ^ 0xA51F87C3U; }
};
}
#include "ARM7FastFetch.inc"
int main() {
    using namespace melonDS;
    alignas(4) static u8 shared[0x8000], privateRAM[0x10000];
    for (u32 i = 0; i < sizeof(shared); i++) shared[i] = (i * 19 + (i >> 8)) & 255;
    for (u32 i = 0; i < sizeof(privateRAM); i++) privateRAM[i] = (i * 31 + (i >> 7)) & 255;
    TestNDS nds; nds.ARM7WRAM = privateRAM;
    ARMv4 cpu{nds};
    u32 seed = 12345, checks = 0;
    const u32 edges[] = {0, 1, 0x3FFF, 0x02000000, 0x02FFFFFF, 0x03000000, 0x03000001,
        0x03003FFF, 0x03007FFF, 0x0300FFFF, 0x037FFFFC, 0x037FFFFF, 0x03800000,
        0x04000000, 0x04800000, 0x06000000, 0xFFFFFFFF};
    // Current shared mappings, including reassignment between successive calls.
    for (int console = 0; console < 2; console++) for (int mapping = 0; mapping < 4; mapping++) {
        nds.ConsoleType = console;
        nds.SWRAM_ARM7.Mem = mapping == 0 ? nullptr : shared + (mapping == 3 ? 0x4000 : 0);
        nds.SWRAM_ARM7.Mask = mapping == 1 ? 0x7FFF : 0x3FFF;
        for (u32 test = 0; test < 100000 + sizeof(edges) / sizeof(edges[0]); test++) {
            seed = seed * 1664525U + 1013904223U;
            const u32 address = test < sizeof(edges) / sizeof(edges[0]) ? edges[test] :
                test & 1 ? (0x03000000U | (seed & 0x7FFFFFU)) : seed;
            const u32 calls = cpu.busCalls;
            u32 expected;
            if ((address & 0xFF800000U) == 0x03000000U && console == 0) {
                const u32 aligned = address & ~3U;
                // Original reader branch: align first, then apply live mask.
                const u8* source = nds.SWRAM_ARM7.Mem ? nds.SWRAM_ARM7.Mem + (aligned & nds.SWRAM_ARM7.Mask) :
                    nds.ARM7WRAM + (aligned & (nds.ARM7WRAMSize - 1));
                std::memcpy(&expected, source, sizeof(expected));
                assert(cpu.CodeRead32(address) == expected);
                assert(cpu.busCalls == calls);
            } else {
                assert(cpu.CodeRead32(address) == (address ^ 0xA51F87C3U));
                assert(cpu.busCalls == calls + 1 && cpu.lastAddress == address);
            }
            // Observe writes immediately; no instruction/value cache.
            shared[seed & 0x7FFF] ^= 0x5A;
            privateRAM[seed & 0xFFFF] ^= 0xA5;
            checks++;
        }
    }
    std::printf("PASS: %u live-mapping/alignment/fallback fetch checks\n", checks);
}
