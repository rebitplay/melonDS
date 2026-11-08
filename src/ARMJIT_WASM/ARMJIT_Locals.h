// Rebit WASM JIT: the block function's locals and the ARM fields it touches.
#ifndef ARMJIT_WASM_LOCALS_H
#define ARMJIT_WASM_LOCALS_H

#include <cstddef>

#include "../ARM.h"

namespace melonDS::WasmLocals
{
// the parameter, then the locals (all i32)
constexpr unsigned LocalCPU = 0;
constexpr unsigned LocalCond = 1;
constexpr unsigned LocalR0 = 2;    // R0..R15 in 2..17, mapped by the register cache
constexpr unsigned LocalCPSR = 18;
constexpr unsigned LocalA = 19;    // ALU operands and results
constexpr unsigned LocalB = 20;
constexpr unsigned LocalRes = 21;
constexpr unsigned LocalC = 22;    // carry out, 0 or 1
constexpr unsigned LocalV = 23;    // overflow, 0 or 1
constexpr unsigned LocalT = 24;    // shift amounts, addresses
constexpr unsigned LocalT2 = 25;
constexpr unsigned LocalT3 = 26;
constexpr unsigned LocalCount = 26; // besides the parameter

constexpr unsigned OffR = offsetof(ARM, R);
constexpr unsigned OffCycles = offsetof(ARM, Cycles);
constexpr unsigned OffCodeRegion = offsetof(ARM, CodeRegion);
constexpr unsigned OffCodeCycles = offsetof(ARM, CodeCycles);
constexpr unsigned OffCurInstr = offsetof(ARM, CurInstr);
constexpr unsigned OffCPSR = offsetof(ARM, CPSR);
constexpr unsigned OffIdleLoop = offsetof(ARM, IdleLoop);

// function types (the module's type section, ARMJIT_Compiler.cpp)
enum : unsigned char
{
    Type_Void1 = 0,
    Type_I32_2,
    Type_Void3,
    Type_I32_1,
    Type_Void2,
    Type_Void4,
};
}

#endif
