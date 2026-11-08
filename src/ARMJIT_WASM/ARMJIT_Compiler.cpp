/*
    Rebit: a WebAssembly back end for melonDS's JIT. See ARMJIT_Compiler.h.
*/
#include "ARMJIT_Compiler.h"

#include <cstddef>

#include <emscripten.h>

#include "../ARM.h"
#include "../ARMJIT.h"
#include "../ARMJIT_Global.h"
#include "../ARMInterpreter.h"
#include "../ARMInterpreter_Branch.h"
#include "../NDS.h"
#include "../Platform.h"

// Compiles a block module and puts its function into the module's function table; returns the
// table index. `imports` are function pointers (table indices) of the C++ functions the block
// calls; the block imports them as e.f0, e.f1, ... and the memory as e.m.
EM_JS(int, rebit_jit_install, (const uint8_t* bytes, int length, const uint32_t* imports, int importCount), {
    if (!Module.rebitJit) Module.rebitJit = { free: [], used: [], table: wasmExports["__indirect_function_table"] };
    const jit = Module.rebitJit;
    const wasmTable = jit.table;
    const module = new WebAssembly.Module(HEAPU8.slice(bytes, bytes + length));
    const env = { m: wasmMemory };
    for (let i = 0; i < importCount; i++) env["f" + i] = wasmTable.get(HEAPU32[(imports >> 2) + i]);
    const instance = new WebAssembly.Instance(module, { e: env });
    let index = jit.free.pop();
    if (index === undefined) index = wasmTable.grow(1);
    wasmTable.set(index, instance.exports.f);
    jit.used.push(index);
    return index;
});

// Drops every block function; their table slots are reused.
EM_JS(void, rebit_jit_reset, (), {
    const jit = Module.rebitJit;
    if (!jit) return;
    const wasmTable = jit.table;
    for (const index of jit.used) {
        wasmTable.set(index, null);
        jit.free.push(index);
    }
    jit.used = [];
});

extern "C" void ARM_Dispatch(melonDS::ARM* cpu, melonDS::JitBlockEntry entry)
{
    reinterpret_cast<void (*)(melonDS::ARM*)>(entry)(cpu);
}

namespace melonDS
{

namespace ARMJIT_Global
{
// the WASM back end has no executable memory of its own: the browser owns the compiled code
void Init() {}
void DeInit() {}
}

using Platform::Log;
using Platform::LogLevel;

// After this many blocks the cache is reset, as the native back ends do when their code memory
// runs low: each block keeps a compiled module alive.
static constexpr u32 MaxInstalledBlocks = 24 * 1024;

void WasmCode::U32(u32 v)
{
    do
    {
        u8 b = v & 0x7F;
        v >>= 7;
        if (v)
            b |= 0x80;
        Byte(b);
    } while (v);
}

void WasmCode::S32(s32 v)
{
    bool more = true;
    while (more)
    {
        u8 b = v & 0x7F;
        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
            more = false;
        else
            b |= 0x80;
        Byte(b);
    }
}

namespace
{
constexpr u32 OffR = offsetof(ARM, R);
constexpr u32 OffCycles = offsetof(ARM, Cycles);
constexpr u32 OffCodeCycles = offsetof(ARM, CodeCycles);
constexpr u32 OffCurInstr = offsetof(ARM, CurInstr);
constexpr u32 OffCPSR = offsetof(ARM, CPSR);
constexpr u32 OffIdleLoop = offsetof(ARM, IdleLoop);

// The block function's parameter, then its locals (all i32)
constexpr u32 LocalCPU = 0;
constexpr u32 LocalCond = 1;
constexpr u32 LocalCount = 1;

void Section(std::vector<u8>& out, u8 id, const WasmCode& content)
{
    WasmCode header;
    header.Byte(id);
    header.U32(content.Bytes.size());
    out.insert(out.end(), header.Bytes.begin(), header.Bytes.end());
    out.insert(out.end(), content.Bytes.begin(), content.Bytes.end());
}

void Name(WasmCode& code, const char* name)
{
    u32 length = 0;
    while (name[length])
        length++;
    code.U32(length);
    for (u32 i = 0; i < length; i++)
        code.Byte(name[i]);
}
}

Compiler::Compiler(melonDS::NDS& nds) : NDS(nds)
{
}

Compiler::~Compiler()
{
    rebit_jit_reset();
}

void Compiler::Reset()
{
    rebit_jit_reset();
    InstalledBlocks = 0;
}

bool Compiler::CanCompile(bool thumb, u16 kind) const
{
    // every instruction is interpreted from the block for now
    return false;
}

u32 Compiler::ImportFunc(void (*func)(ARM*))
{
    const u32 pointer = static_cast<u32>(reinterpret_cast<uintptr_t>(func));
    for (u32 i = 0; i < Imports.size(); i++)
        if (Imports[i] == pointer)
            return i;
    Imports.push_back(pointer);
    return Imports.size() - 1;
}

void Compiler::Comp_CallInterpreter(void (*func)(ARM*))
{
    const u32 index = ImportFunc(func);
    Code.LocalGet(LocalCPU);
    Code.Call(index);
}

void Compiler::Comp_Condition(u32 cond)
{
    // ARM::CheckCondition: (ConditionTable[cond] >> (CPSR >> 28)) & 1
    Code.I32Const(ARM::ConditionTable[cond]);
    Code.LocalGet(LocalCPU);
    Code.I32Load(OffCPSR);
    Code.I32Const(28);
    Code.Op(0x76); // i32.shr_u
    Code.Op(0x76); // i32.shr_u
    Code.I32Const(1);
    Code.Op(0x71); // i32.and
}

void Compiler::Comp_AddCycles(u32 cycles)
{
    if (!cycles)
        return;
    Code.LocalGet(LocalCPU);
    Code.LocalGet(LocalCPU);
    Code.I32Load(OffCycles);
    Code.I32Const(cycles);
    Code.Op(0x6A); // i32.add
    Code.I32Store(OffCycles);
}

void Compiler::Comp_AddCycles_C(bool forceNonConstant)
{
    const s32 cycles = Num
        ? NDS.ARM7MemTimings[CurInstr.CodeCycles][Thumb ? 1 : 3]
        : ((R15 & 0x2) ? 0 : CurInstr.CodeCycles);

    if ((!Thumb && CurInstr.Cond() < 0xE) || forceNonConstant)
        Comp_AddCycles(cycles);
    else
        ConstantCycles += cycles;
}

// What the front end's branch following asks of a branch (as the native back ends do): an idle
// loop flags itself, and a block that went on past a conditional branch leaves when the branch did
// not go the way it was followed.
void Compiler::Comp_SpecialBranchBehaviour(bool taken)
{
    if (taken && (CurInstr.BranchFlags & branch_IdleBranch))
    {
        Code.LocalGet(LocalCPU);
        Code.LocalGet(LocalCPU);
        Code.I32Load8U(OffIdleLoop);
        Code.I32Const(1);
        Code.Op(0x72); // i32.or
        Code.I32Store8(OffIdleLoop);
    }

    if ((CurInstr.BranchFlags & branch_FollowCondNotTaken && taken)
        || (CurInstr.BranchFlags & branch_FollowCondTaken && !taken))
    {
        Comp_AddCycles(ConstantCycles);
        Code.Return();
    }
}

JitBlockEntry Compiler::CompileBlock(ARM* cpu, bool thumb, FetchedInstr instrs[], int instrsCount, bool hasMemoryInstr)
{
    if (InstalledBlocks >= MaxInstalledBlocks)
    {
        Log(LogLevel::Debug, "WASM JIT: %u blocks, resetting the block cache\n", InstalledBlocks);
        NDS.JIT.ResetBlockCache();
    }

    Code.Bytes.clear();
    Imports.clear();
    ConstantCycles = 0;
    Thumb = thumb;
    Num = cpu->Num;
    CodeRegion = instrs[0].Addr >> 24;

    for (int i = 0; i < instrsCount; i++)
    {
        CurInstr = instrs[i];
        R15 = CurInstr.Addr + (Thumb ? 4 : 8);
        CodeRegion = R15 >> 24;

        // what the interpreter's handler reads: the pipeline PC, the instruction and its fetch timing
        Code.LocalGet(LocalCPU);
        Code.I32Const(R15);
        Code.I32Store(OffR + 15 * 4);
        Code.LocalGet(LocalCPU);
        Code.I32Const(CurInstr.CodeCycles);
        Code.I32Store(OffCodeCycles);
        Code.LocalGet(LocalCPU);
        Code.I32Const(CurInstr.Instr);
        Code.I32Store(OffCurInstr);

        if (Thumb)
        {
            if (CurInstr.Info.Kind == ARMInstrInfo::tk_BL_LONG)
            {
                // the front end merged BL's two halves into one instruction; the interpreter runs
                // them one at a time, each with its own half in CurInstr and its own PC
                Code.LocalGet(LocalCPU);
                Code.I32Const(CurInstr.Instr & 0xFFFF);
                Code.I32Store(OffCurInstr);
                Comp_CallInterpreter(ARMInterpreter::T_BL_LONG_1);
                Code.LocalGet(LocalCPU);
                Code.I32Const(R15 + 2);
                Code.I32Store(OffR + 15 * 4);
                Code.LocalGet(LocalCPU);
                Code.I32Const(CurInstr.Instr >> 16);
                Code.I32Store(OffCurInstr);
                Comp_CallInterpreter(ARMInterpreter::T_BL_LONG_2);
                Comp_SpecialBranchBehaviour(true);
            }
            else if (CurInstr.Info.Kind == ARMInstrInfo::tk_BCOND)
            {
                // the handler checks the condition itself; the block needs to know too
                Comp_Condition((CurInstr.Instr >> 8) & 0xF);
                Code.LocalSet(LocalCond);
                Comp_CallInterpreter(InterpretTHUMB[CurInstr.Info.Kind]);
                Code.LocalGet(LocalCond);
                Code.If();
                Comp_SpecialBranchBehaviour(true);
                Code.Else();
                Comp_SpecialBranchBehaviour(false);
                Code.End();
            }
            else
            {
                Comp_CallInterpreter(InterpretTHUMB[CurInstr.Info.Kind]);
                Comp_SpecialBranchBehaviour(true);
            }
        }
        else
        {
            const u32 cond = CurInstr.Cond();
            if (CurInstr.Info.Kind == ARMInstrInfo::ak_BLX_IMM)
            {
                Comp_CallInterpreter(ARMInterpreter::A_BLX_IMM);
            }
            else if (cond == 0xF)
            {
                Comp_AddCycles_C();
            }
            else if (cond < 0xE)
            {
                Comp_Condition(cond);
                Code.If();
                Comp_CallInterpreter(InterpretARM[CurInstr.Info.Kind]);
                Comp_SpecialBranchBehaviour(true);
                Code.Else();
                Comp_AddCycles_C(true);
                Comp_SpecialBranchBehaviour(false);
                Code.End();
            }
            else
            {
                Comp_CallInterpreter(InterpretARM[CurInstr.Info.Kind]);
                Comp_SpecialBranchBehaviour(true);
            }
        }
    }

    Comp_AddCycles(ConstantCycles);
    Code.End();

    // the module: one type (i32) -> (), the memory and the called functions as imports,
    // the block as the exported function f
    std::vector<u8> module = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

    WasmCode types;
    types.U32(1);
    types.Byte(0x60);
    types.U32(1);
    types.Byte(0x7F);
    types.U32(0);
    Section(module, 1, types);

    WasmCode imports;
    imports.U32(1 + Imports.size());
    Name(imports, "e");
    Name(imports, "m");
    imports.Byte(0x02); // memory
    imports.Byte(0x00); // no maximum
    imports.U32(0);
    for (u32 i = 0; i < Imports.size(); i++)
    {
        char name[12];
        snprintf(name, sizeof(name), "f%u", i);
        Name(imports, "e");
        Name(imports, name);
        imports.Byte(0x00); // function
        imports.U32(0);     // of type 0
    }
    Section(module, 2, imports);

    WasmCode functions;
    functions.U32(1);
    functions.U32(0);
    Section(module, 3, functions);

    WasmCode exports;
    exports.U32(1);
    Name(exports, "f");
    exports.Byte(0x00);
    exports.U32(Imports.size());
    Section(module, 7, exports);

    WasmCode body;
    body.U32(1); // one group of locals: LocalCount i32
    body.U32(LocalCount);
    body.Byte(0x7F);
    body.Bytes.insert(body.Bytes.end(), Code.Bytes.begin(), Code.Bytes.end());
    WasmCode code;
    code.U32(1);
    code.U32(body.Bytes.size());
    code.Bytes.insert(code.Bytes.end(), body.Bytes.begin(), body.Bytes.end());
    Section(module, 10, code);

    const u64 started = Platform::GetUSCount();
    const int index = rebit_jit_install(module.data(), module.size(), Imports.data(), Imports.size());
    CompileMicroseconds += Platform::GetUSCount() - started;
    InstalledBlocks++;
    BlocksCompiled++;
    BytesEmitted += module.size();
    return AddEntryOffset(index);
}

}
