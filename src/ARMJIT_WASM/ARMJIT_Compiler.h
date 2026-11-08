/*
    Rebit: a WebAssembly back end for melonDS's JIT.

    melonDS's JIT front end (ARMJIT.cpp) finds a block of guest ARM/Thumb code, decodes it and
    works out its timing; a back end turns the block into host code. This back end writes each
    block as a small WebAssembly module with one function, f(cpu), which the browser compiles;
    the function goes into the module's function table, and its table index is the block's
    JitBlockEntry. ARM_Dispatch calls it through the table.

    Guest state stays in the ARM object in linear memory (no register allocation across
    instructions yet). Instructions without a native translation call the interpreter's handler,
    as the x64 and ARM64 back ends do.
*/
#ifndef ARMJIT_WASM_COMPILER_H
#define ARMJIT_WASM_COMPILER_H

#if defined(JIT_ENABLED) && defined(__wasm__)

#include <vector>

#include "../ARMJIT_Internal.h"

namespace melonDS
{
class ARMJIT;
class ARMJIT_Memory;
class NDS;

// The bytes of one WebAssembly function body, with the encodings this back end needs.
class WasmCode
{
public:
    std::vector<u8> Bytes;

    void Byte(u8 b) { Bytes.push_back(b); }
    void U32(u32 v);  // unsigned LEB128
    void S32(s32 v);  // signed LEB128

    void LocalGet(u32 i) { Byte(0x20); U32(i); }
    void LocalSet(u32 i) { Byte(0x21); U32(i); }
    void LocalTee(u32 i) { Byte(0x22); U32(i); }
    void I32Const(s32 v) { Byte(0x41); S32(v); }
    void I32Load(u32 offset) { Byte(0x28); U32(2); U32(offset); }
    void I32Store(u32 offset) { Byte(0x36); U32(2); U32(offset); }
    void I32Load8U(u32 offset) { Byte(0x2D); U32(0); U32(offset); }
    void I32Store8(u32 offset) { Byte(0x3A); U32(0); U32(offset); }
    void Call(u32 func) { Byte(0x10); U32(func); }
    void If() { Byte(0x04); Byte(0x40); }
    void Else() { Byte(0x05); }
    void End() { Byte(0x0B); }
    void Return() { Byte(0x0F); }
    void Op(u8 op) { Byte(op); }
};

class Compiler
{
public:
    explicit Compiler(melonDS::NDS& nds);
    ~Compiler();

    void Reset();

    JitBlockEntry CompileBlock(ARM* cpu, bool thumb, FetchedInstr instrs[], int instrsCount, bool hasMemoryInstr);

    bool CanCompile(bool thumb, u16 kind) const;

    // a block's entry is its function table index
    JitBlockEntry AddEntryOffset(u32 offset) { return reinterpret_cast<JitBlockEntry>(static_cast<uintptr_t>(offset)); }
    u32 SubEntryOffset(JitBlockEntry entry) { return static_cast<u32>(reinterpret_cast<uintptr_t>(entry)); }

    bool IsJITFault(const u8*) { return false; }
    u8* RewriteMemAccess(u8* pc) { return pc; }

    u8* CodeMemBase = nullptr;

    // statistics, for the frontend's diagnostics
    u32 BlocksCompiled = 0;
    u64 BytesEmitted = 0;
    u64 CompileMicroseconds = 0;

private:
    melonDS::NDS& NDS;

    // per block
    WasmCode Code;
    std::vector<u32> Imports;  // function pointers (table indices) the block calls
    FetchedInstr CurInstr {};
    bool Thumb = false;
    u32 Num = 0;
    u32 R15 = 0;
    u32 CodeRegion = 0;
    u32 ConstantCycles = 0;

    u32 ImportFunc(void (*func)(ARM*));

    void Comp_CallInterpreter(void (*func)(ARM*));
    void Comp_Condition(u32 cond);  // pushes 1 when the condition holds
    void Comp_AddCycles(u32 cycles);
    void Comp_AddCycles_C(bool forceNonConstant = false);
    void Comp_SpecialBranchBehaviour(bool taken);

    u32 InstalledBlocks = 0;
};

}

#endif

#endif
