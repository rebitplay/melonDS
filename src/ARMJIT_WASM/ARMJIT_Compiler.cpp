/*
    Rebit: a WebAssembly back end for melonDS's JIT. See ARMJIT_Compiler.h.
*/
#include "ARMJIT_Compiler.h"

#include <cstddef>
#include <cstdio>

#include <emscripten.h>

#include "../ARM.h"
#include "../ARMJIT.h"
#include "../ARMJIT_Global.h"
#include "../ARMInterpreter.h"
#include "../ARMInterpreter_Branch.h"
#include "../NDS.h"
#include "../Platform.h"
#include "ARMJIT_Locals.h"

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
using namespace WasmLocals;

// Guest registers get a local each, so the cache never has to evict one.
template <>
const int RegisterCache<Compiler, int>::NativeRegAllocOrder[] =
    {LocalR0, LocalR0 + 1, LocalR0 + 2, LocalR0 + 3, LocalR0 + 4, LocalR0 + 5, LocalR0 + 6, LocalR0 + 7,
     LocalR0 + 8, LocalR0 + 9, LocalR0 + 10, LocalR0 + 11, LocalR0 + 12, LocalR0 + 13, LocalR0 + 14, LocalR0 + 15};
template <>
const int RegisterCache<Compiler, int>::NativeRegsAvailable = 16;

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
    S64(v);
}

void WasmCode::S64(s64 v)
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

// The function types blocks use: params (all i32) and whether it returns an i32
struct FuncType
{
    u8 Params;
    bool Result;
};
const FuncType FuncTypes[] =
{
    {1, false}, // Type_Void1: the block itself, interpreter handlers
    {2, true},  // Type_I32_2: SlowRead9(addr, cpu)
    {3, false}, // Type_Void3: SlowWrite9(addr, cpu, val), SlowBlockTransfer7(addr, data, num)
    {1, true},  // Type_I32_1: SlowRead7(addr)
    {2, false}, // Type_Void2: SlowWrite7(addr, val)
    {4, false}, // Type_Void4: SlowBlockTransfer9(addr, data, num, cpu)
};
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

void Compiler::LoadReg(int reg, int local)
{
    Code.LocalGet(LocalCPU);
    Code.I32Load(OffR + reg * 4);
    Code.LocalSet(local);
}

void Compiler::SaveReg(int reg, int local)
{
    Code.LocalGet(LocalCPU);
    Code.LocalGet(local);
    Code.I32Store(OffR + reg * 4);
}

void Compiler::LoadCPSR()
{
    Code.LocalGet(LocalCPU);
    Code.I32Load(OffCPSR);
    Code.LocalSet(LocalCPSR);
    CPSRDirty = false;
}

void Compiler::SaveCPSR()
{
    if (!CPSRDirty)
        return;
    Code.LocalGet(LocalCPU);
    Code.LocalGet(LocalCPSR);
    Code.I32Store(OffCPSR);
    CPSRDirty = false;
}

void Compiler::PushReg(int reg)
{
    if (reg == 15)
    {
        Code.I32Const(R15);
        return;
    }
    if (RegCache.Mapping[reg] == -1)
    {
        Log(LogLevel::Error, "WASM JIT bug: r%d is not loaded (%08x)\n", reg, CurInstr.Instr);
        abort();
    }
    Code.LocalGet(RegCache.Mapping[reg]);
}

void Compiler::SetReg(int reg)
{
    if (reg == 15 || RegCache.Mapping[reg] == -1)
    {
        Log(LogLevel::Error, "WASM JIT bug: writing r%d (%08x)\n", reg, CurInstr.Instr);
        abort();
    }
    Code.LocalSet(RegCache.Mapping[reg]);
}

void Compiler::PushCarry()
{
    Code.LocalGet(LocalCPSR);
    Code.I32Const(29);
    Code.Op(W_I32ShrU);
    Code.I32Const(1);
    Code.Op(W_I32And);
}

u32 Compiler::ImportFunc(u32 pointer, u8 type)
{
    for (u32 i = 0; i < Imports.size(); i++)
        if (Imports[i] == pointer)
            return i;
    Imports.push_back(pointer);
    ImportTypes.push_back(type);
    return Imports.size() - 1;
}

void Compiler::CallFunc(void (*func)(ARM*))
{
    Code.Call(ImportFunc(static_cast<u32>(reinterpret_cast<uintptr_t>(func)), Type_Void1));
}

void Compiler::Comp_CallInterpreter(void (*func)(ARM*))
{
    Code.LocalGet(LocalCPU);
    CallFunc(func);
    // the handler may have changed the CPSR (flags, mode, Thumb bit); a block exit right after
    // it (branch following) stores the local
    Code.LocalGet(LocalCPU);
    Code.I32Load(OffCPSR);
    Code.LocalSet(LocalCPSR);
}

void Compiler::Comp_Condition(u32 cond)
{
    // ARM::CheckCondition: (ConditionTable[cond] >> (CPSR >> 28)) & 1
    Code.I32Const(ARM::ConditionTable[cond]);
    Code.LocalGet(LocalCPSR);
    Code.I32Const(28);
    Code.Op(W_I32ShrU);
    Code.Op(W_I32ShrU);
    Code.I32Const(1);
    Code.Op(W_I32And);
}

void Compiler::Comp_AddCycles(u32 cycles)
{
    if (!cycles)
        return;
    Code.LocalGet(LocalCPU);
    Code.LocalGet(LocalCPU);
    Code.I32Load(OffCycles);
    Code.I32Const(cycles);
    Code.Op(W_I32Add);
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

void Compiler::Comp_AddCycles_CI(u32 i)
{
    const s32 cycles = (Num
        ? NDS.ARM7MemTimings[CurInstr.CodeCycles][Thumb ? 0 : 2]
        : ((R15 & 0x2) ? 0 : CurInstr.CodeCycles)) + i;

    if (!Thumb && CurInstr.Cond() < 0xE)
        Comp_AddCycles(cycles);
    else
        ConstantCycles += cycles;
}

// What the front end's branch following asks of a branch (as the native back ends do): an idle
// loop flags itself, and a block that went on past a conditional branch leaves when the branch did
// not go the way it was followed.
void Compiler::Comp_SpecialBranchBehaviour(bool taken)
{
    extern u32 RebitJitNativeMask;
    if (taken && (CurInstr.BranchFlags & branch_IdleBranch) && (RebitJitNativeMask & 0x10000))
    {
        Code.LocalGet(LocalCPU);
        Code.LocalGet(LocalCPU);
        Code.I32Load8U(OffIdleLoop);
        Code.I32Const(1);
        Code.Op(W_I32Or);
        Code.I32Store8(OffIdleLoop);
    }

    if ((CurInstr.BranchFlags & branch_FollowCondNotTaken && taken)
        || (CurInstr.BranchFlags & branch_FollowCondTaken && !taken))
    {
        RegCache.PrepareExit();
        // the CPSR local may be newer than memory on this path only; store it unconditionally
        Code.LocalGet(LocalCPU);
        Code.LocalGet(LocalCPSR);
        Code.I32Store(OffCPSR);
        Comp_AddCycles(ConstantCycles);
        Code.Return();
    }
}

// A branch to a known address, with the cycles of the pipeline refill worked out now (the x64
// back end's Comp_JumpTo(u32)).
void Compiler::Comp_JumpTo(u32 addr, bool forceNonConstantCycles)
{
    IrregularCycles = true;

    u32 newPC;
    u32 cycles = 0;

    if (addr & 0x1 && !Thumb)
    {
        CPSRDirty = true;
        Code.LocalGet(LocalCPSR);
        Code.I32Const(0x20);
        Code.Op(W_I32Or);
        Code.LocalSet(LocalCPSR);
    }
    else if (!(addr & 0x1) && Thumb)
    {
        CPSRDirty = true;
        Code.LocalGet(LocalCPSR);
        Code.I32Const(~0x20);
        Code.Op(W_I32And);
        Code.LocalSet(LocalCPSR);
    }

    if (Num == 0)
    {
        ARMv5* cpu9 = (ARMv5*)CurCPU;

        u32 regionCodeCycles = cpu9->MemTimings[addr >> 12][0];
        u32 compileTimeCodeCycles = cpu9->RegionCodeCycles;
        cpu9->RegionCodeCycles = regionCodeCycles;

        if (Exit)
        {
            Code.LocalGet(LocalCPU);
            Code.I32Const(regionCodeCycles);
            Code.I32Store(offsetof(ARMv5, RegionCodeCycles));
        }

        if (addr & 0x1)
        {
            addr &= ~0x1;
            newPC = addr + 2;

            // two-opcodes-at-once fetch
            if (addr & 0x2)
            {
                cpu9->CodeRead32(addr - 2, true);
                cycles += cpu9->CodeCycles;
                cpu9->CodeRead32(addr + 2, false);
                cycles += CurCPU->CodeCycles;
            }
            else
            {
                cpu9->CodeRead32(addr, true);
                cycles += cpu9->CodeCycles;
            }
        }
        else
        {
            addr &= ~0x3;
            newPC = addr + 4;

            cpu9->CodeRead32(addr, true);
            cycles += cpu9->CodeCycles;
            cpu9->CodeRead32(addr + 4, false);
            cycles += cpu9->CodeCycles;
        }

        cpu9->RegionCodeCycles = compileTimeCodeCycles;
    }
    else
    {
        ARMv4* cpu7 = (ARMv4*)CurCPU;

        u32 codeRegion = addr >> 24;
        u32 codeCycles = addr >> 15; // cheato

        cpu7->CodeRegion = codeRegion;
        cpu7->CodeCycles = codeCycles;

        if (Exit)
        {
            Code.LocalGet(LocalCPU);
            Code.I32Const(codeRegion);
            Code.I32Store(OffCodeRegion);
            Code.LocalGet(LocalCPU);
            Code.I32Const(codeCycles);
            Code.I32Store(OffCodeCycles);
        }

        if (addr & 0x1)
        {
            addr &= ~0x1;
            newPC = addr + 2;

            // this is necessary because ARM7 bios protection
            u32 compileTimePC = CurCPU->R[15];
            CurCPU->R[15] = newPC;

            cycles += NDS.ARM7MemTimings[codeCycles][0] + NDS.ARM7MemTimings[codeCycles][1];

            CurCPU->R[15] = compileTimePC;
        }
        else
        {
            addr &= ~0x3;
            newPC = addr + 4;

            u32 compileTimePC = CurCPU->R[15];
            CurCPU->R[15] = newPC;

            cycles += NDS.ARM7MemTimings[codeCycles][2] + NDS.ARM7MemTimings[codeCycles][3];

            CurCPU->R[15] = compileTimePC;
        }

        cpu7->CodeRegion = R15 >> 24;
        cpu7->CodeCycles = addr >> 15;
    }

    if (Exit)
    {
        Code.LocalGet(LocalCPU);
        Code.I32Const(newPC);
        Code.I32Store(OffR + 15 * 4);
    }
    if ((Thumb || CurInstr.Cond() >= 0xE) && !forceNonConstantCycles)
        ConstantCycles += cycles;
    else
        Comp_AddCycles(cycles);
}

void Compiler::A_Comp_BranchImm()
{
    int op = (CurInstr.Instr >> 24) & 1;
    s32 offset = (s32)(CurInstr.Instr << 8) >> 6;
    u32 target = R15 + offset;
    bool link = op;

    if (CurInstr.Cond() == 0xF) // BLX_imm
    {
        target += (op << 1) + 1;
        link = true;
    }

    if (link)
    {
        Code.I32Const(R15 - 4);
        SetReg(14);
    }

    Comp_JumpTo(target);
}

void Compiler::T_Comp_BCOND()
{
    u32 cond = (CurInstr.Instr >> 8) & 0xF;
    Comp_Condition(cond);
    Code.If();

    s32 offset = (s32)(CurInstr.Instr << 24) >> 23;
    Comp_JumpTo(R15 + offset + 1, true);
    Comp_SpecialBranchBehaviour(true);

    Code.Else();
    Comp_SpecialBranchBehaviour(false);
    Comp_AddCycles_C(true);
    Code.End();
}

void Compiler::T_Comp_B()
{
    s32 offset = (s32)((CurInstr.Instr & 0x7FF) << 21) >> 20;
    Comp_JumpTo(R15 + offset + 1);
}

void Compiler::T_Comp_BL_Merged()
{
    Comp_AddCycles_C();

    R15 += 2;

    u32 upperPart = CurInstr.Instr >> 16;
    u32 target = (R15 - 2) + ((s32)((CurInstr.Instr & 0x7FF) << 21) >> 9);
    target += (upperPart & 0x7FF) << 1;

    if (Num == 1 || upperPart & (1 << 12))
        target |= 1;

    Code.I32Const((R15 - 2) | 1);
    SetReg(14);

    Comp_JumpTo(target);
}

// diagnostics: which groups of instructions get native code (melonds_jit_native)
u32 RebitJitNativeMask = ~0u;

Compiler::CompileFunc Compiler::GetCompileFunc() const
{
    CompileFunc func = GetCompileFuncUnmasked();
    if (!func)
        return nullptr;
    u32 group;
    if (func == &Compiler::A_Comp_BranchXchangeReg || func == &Compiler::T_Comp_BranchXchangeReg)
        group = 32;
    else if (func == &Compiler::A_Comp_MemWB || func == &Compiler::A_Comp_MemHalf || func == &Compiler::A_Comp_LDM_STM
        || func == &Compiler::T_Comp_MemReg || func == &Compiler::T_Comp_MemImm || func == &Compiler::T_Comp_MemRegHalf
        || func == &Compiler::T_Comp_MemImmHalf || func == &Compiler::T_Comp_LoadPCRel || func == &Compiler::T_Comp_MemSPRel
        || func == &Compiler::T_Comp_PUSH_POP || func == &Compiler::T_Comp_LDMIA_STMIA)
        group = 16;
    else if (Thumb)
        group = (func == &Compiler::T_Comp_BCOND || func == &Compiler::T_Comp_B || func == &Compiler::T_Comp_BL_Merged) ? 8 : 4;
    else
        group = func == &Compiler::A_Comp_BranchImm ? 2 : 1;
    // finer: bit 16 + kind for single kinds when the mask's top bit is set
    if (RebitJitNativeMask & 0x80000000)
        return (RebitJitNativeMask & group) ? func : nullptr;
    return (RebitJitNativeMask & group) ? func : nullptr;
}

Compiler::CompileFunc Compiler::GetCompileFuncUnmasked() const
{
    const u16 kind = CurInstr.Info.Kind;
    if (Thumb)
    {
        switch (kind)
        {
        case ARMInstrInfo::tk_LSL_IMM: case ARMInstrInfo::tk_LSR_IMM: case ARMInstrInfo::tk_ASR_IMM:
            return &Compiler::T_Comp_ShiftImm;
        case ARMInstrInfo::tk_ADD_REG_: case ARMInstrInfo::tk_SUB_REG_:
        case ARMInstrInfo::tk_ADD_IMM_: case ARMInstrInfo::tk_SUB_IMM_:
            return &Compiler::T_Comp_AddSub_;
        case ARMInstrInfo::tk_MOV_IMM: case ARMInstrInfo::tk_CMP_IMM:
        case ARMInstrInfo::tk_ADD_IMM: case ARMInstrInfo::tk_SUB_IMM:
            return &Compiler::T_Comp_ALU_Imm8;
        case ARMInstrInfo::tk_AND_REG: case ARMInstrInfo::tk_EOR_REG: case ARMInstrInfo::tk_LSL_REG:
        case ARMInstrInfo::tk_LSR_REG: case ARMInstrInfo::tk_ASR_REG: case ARMInstrInfo::tk_ADC_REG:
        case ARMInstrInfo::tk_SBC_REG: case ARMInstrInfo::tk_ROR_REG: case ARMInstrInfo::tk_TST_REG:
        case ARMInstrInfo::tk_NEG_REG: case ARMInstrInfo::tk_CMP_REG: case ARMInstrInfo::tk_CMN_REG:
        case ARMInstrInfo::tk_ORR_REG: case ARMInstrInfo::tk_BIC_REG: case ARMInstrInfo::tk_MVN_REG:
            return &Compiler::T_Comp_ALU;
        case ARMInstrInfo::tk_ADD_HIREG: case ARMInstrInfo::tk_MOV_HIREG:
        {
            const u32 rd = (CurInstr.Instr & 0x7) | ((CurInstr.Instr >> 4) & 0x8);
            if (rd == 15)
                return nullptr;
            // the nocash debug print hook (mov r12, r12) stays with the interpreter
            if ((CurInstr.Instr & 0xFFFF) == 0x46E4)
                return nullptr;
            return &Compiler::T_Comp_ALU_HiReg;
        }
        case ARMInstrInfo::tk_CMP_HIREG:
            return &Compiler::T_Comp_ALU_HiReg;
        case ARMInstrInfo::tk_ADD_PCREL: case ARMInstrInfo::tk_ADD_SPREL:
            return &Compiler::T_Comp_RelAddr;
        case ARMInstrInfo::tk_ADD_SP:
            return &Compiler::T_Comp_AddSP;
        case ARMInstrInfo::tk_LDR_PCREL:
            return &Compiler::T_Comp_LoadPCRel;
        case ARMInstrInfo::tk_STR_REG: case ARMInstrInfo::tk_STRB_REG:
        case ARMInstrInfo::tk_LDR_REG: case ARMInstrInfo::tk_LDRB_REG:
            return &Compiler::T_Comp_MemReg;
        case ARMInstrInfo::tk_STRH_REG: case ARMInstrInfo::tk_LDRSB_REG:
        case ARMInstrInfo::tk_LDRH_REG: case ARMInstrInfo::tk_LDRSH_REG:
            return &Compiler::T_Comp_MemRegHalf;
        case ARMInstrInfo::tk_STR_IMM: case ARMInstrInfo::tk_LDR_IMM:
        case ARMInstrInfo::tk_STRB_IMM: case ARMInstrInfo::tk_LDRB_IMM:
            return &Compiler::T_Comp_MemImm;
        case ARMInstrInfo::tk_STRH_IMM: case ARMInstrInfo::tk_LDRH_IMM:
            return &Compiler::T_Comp_MemImmHalf;
        case ARMInstrInfo::tk_STR_SPREL: case ARMInstrInfo::tk_LDR_SPREL:
            return &Compiler::T_Comp_MemSPRel;
        case ARMInstrInfo::tk_PUSH: case ARMInstrInfo::tk_POP:
            return &Compiler::T_Comp_PUSH_POP;
        case ARMInstrInfo::tk_LDMIA: case ARMInstrInfo::tk_STMIA:
            return &Compiler::T_Comp_LDMIA_STMIA;
        case ARMInstrInfo::tk_BX:
            return &Compiler::T_Comp_BranchXchangeReg;
        case ARMInstrInfo::tk_BLX_REG:
            return Num == 0 ? &Compiler::T_Comp_BranchXchangeReg : nullptr;
        case ARMInstrInfo::tk_BCOND:
            return &Compiler::T_Comp_BCOND;
        case ARMInstrInfo::tk_B:
            return &Compiler::T_Comp_B;
        case ARMInstrInfo::tk_BL_LONG:
            return &Compiler::T_Comp_BL_Merged;
        default:
            return nullptr;
        }
    }

    if (kind <= ARMInstrInfo::ak_CMN_IMM)
    {
        const bool test = kind >= ARMInstrInfo::ak_TST_REG_LSL_IMM;
        if (!test && ((CurInstr.Instr >> 12) & 0xF) == 15)
            return nullptr; // writes the PC
        return &Compiler::A_Comp_ALU;
    }
    const u32 instr = CurInstr.Instr;
    const u32 rd = (instr >> 12) & 0xF;
    const u32 rn = (instr >> 16) & 0xF;
    const bool unconditional = CurInstr.Cond() == 0xE;
    if (kind >= ARMInstrInfo::ak_STR_REG_LSL && kind <= ARMInstrInfo::ak_LDRB_POST_IMM)
    {
        // the PC as data or base with writeback stays with the interpreter
        if (rd == 15 || (rn == 15 && (!(instr & (1 << 24)) || (instr & (1 << 21)))))
            return nullptr;
        return &Compiler::A_Comp_MemWB;
    }
    if ((kind >= ARMInstrInfo::ak_STRH_REG && kind <= ARMInstrInfo::ak_STRH_POST_IMM)
        || (kind >= ARMInstrInfo::ak_LDRH_REG && kind <= ARMInstrInfo::ak_LDRSH_POST_IMM))
    {
        if (rd == 15 || (rn == 15 && (!(instr & (1 << 24)) || (instr & (1 << 21)))))
            return nullptr;
        return &Compiler::A_Comp_MemHalf;
    }
    switch (kind)
    {
    case ARMInstrInfo::ak_B: case ARMInstrInfo::ak_BL: case ARMInstrInfo::ak_BLX_IMM:
        return &Compiler::A_Comp_BranchImm;
    case ARMInstrInfo::ak_LDM: case ARMInstrInfo::ak_STM:
    {
        const bool load = instr & (1 << 20);
        if (rn == 15 || (instr & (1 << 22)))
            return nullptr; // user mode registers, or the PC as base
        if ((instr & (1 << 15)) && (!load || !unconditional))
            return nullptr; // storing the PC; a conditional jump through it
        return &Compiler::A_Comp_LDM_STM;
    }
    case ARMInstrInfo::ak_BX: case ARMInstrInfo::ak_BLX_REG:
        return unconditional ? &Compiler::A_Comp_BranchXchangeReg : nullptr;
    default:
        return nullptr;
    }
}

bool Compiler::CanCompile(bool thumb, u16 kind) const
{
    // what the front end asks before it knows the operands; GetCompileFunc decides per
    // instruction (an instruction it turns down is interpreted, which reads and writes the
    // flags it should, so the front end's flag analysis stays right)
    if (thumb)
    {
        if (kind == ARMInstrInfo::tk_MUL_REG)
            return false;
        return kind <= ARMInstrInfo::tk_BLX_REG || kind == ARMInstrInfo::tk_B || kind == ARMInstrInfo::tk_BL_LONG;
    }
    return kind <= ARMInstrInfo::ak_CMN_IMM
        || (kind >= ARMInstrInfo::ak_STR_REG_LSL && kind <= ARMInstrInfo::ak_STRH_POST_IMM)
        || (kind >= ARMInstrInfo::ak_LDRH_REG && kind <= ARMInstrInfo::ak_LDRSH_POST_IMM)
        || kind == ARMInstrInfo::ak_LDM || kind == ARMInstrInfo::ak_STM
        || kind == ARMInstrInfo::ak_B || kind == ARMInstrInfo::ak_BL || kind == ARMInstrInfo::ak_BLX_IMM
        || kind == ARMInstrInfo::ak_BX || kind == ARMInstrInfo::ak_BLX_REG;
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
    ImportTypes.clear();
    ConstantCycles = 0;
    Thumb = thumb;
    Num = cpu->Num;
    CurCPU = cpu;
    CodeRegion = instrs[0].Addr >> 24;

    RegCache = RegisterCache<Compiler, int>(this, instrs, instrsCount);

    LoadCPSR();

    for (int i = 0; i < instrsCount; i++)
    {
        CurInstr = instrs[i];
        R15 = CurInstr.Addr + (Thumb ? 4 : 8);
        CodeRegion = R15 >> 24;

        Exit = i == instrsCount - 1 || (CurInstr.BranchFlags & branch_FollowCondNotTaken);

        CompileFunc comp = GetCompileFunc();

        bool isConditional = Thumb ? CurInstr.Info.Kind == ARMInstrInfo::tk_BCOND : CurInstr.Cond() < 0xE;
        if (comp == nullptr || (CurInstr.BranchFlags & branch_FollowCondTaken)
            || (i == instrsCount - 1 && (!CurInstr.Info.Branches() || isConditional)))
        {
            Code.LocalGet(LocalCPU);
            Code.I32Const(R15);
            Code.I32Store(OffR + 15 * 4);
            if (comp == nullptr)
            {
                // what the interpreter's handler reads: the instruction and its fetch timing
                Code.LocalGet(LocalCPU);
                Code.I32Const(CurInstr.CodeCycles);
                Code.I32Store(OffCodeCycles);
                Code.LocalGet(LocalCPU);
                Code.I32Const(CurInstr.Instr);
                Code.I32Store(OffCurInstr);

                SaveCPSR();
            }
        }

        if (comp != nullptr)
            RegCache.Prepare(Thumb, i);
        else
            RegCache.Flush();

        if (Thumb)
        {
            if (comp != nullptr)
            {
                (this->*comp)();
            }
            else if (CurInstr.Info.Kind == ARMInstrInfo::tk_BL_LONG)
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
                if (comp != nullptr)
                    (this->*comp)();
                else
                    Comp_CallInterpreter(ARMInterpreter::A_BLX_IMM);
            }
            else if (cond == 0xF)
            {
                Comp_AddCycles_C();
            }
            else
            {
                IrregularCycles = comp == nullptr;

                if (cond < 0xE)
                {
                    Comp_Condition(cond);
                    Code.If();
                }

                if (comp == nullptr)
                    Comp_CallInterpreter(InterpretARM[CurInstr.Info.Kind]);
                else
                    (this->*comp)();

                Comp_SpecialBranchBehaviour(true);

                if (cond < 0xE)
                {
                    // a skipped instruction still takes its fetch
                    Code.Else();
                    Comp_AddCycles_C(true);
                    Comp_SpecialBranchBehaviour(false);
                    Code.End();
                }
            }
        }

        if (comp == nullptr)
            LoadCPSR();
    }

    RegCache.Flush();
    SaveCPSR();
    Comp_AddCycles(ConstantCycles);
    Code.End();

    // the module: the function types, the memory and the called functions as imports, the
    // block as the exported function f
    std::vector<u8> module = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

    WasmCode types;
    types.U32(sizeof(FuncTypes) / sizeof(FuncTypes[0]));
    for (const FuncType& type : FuncTypes)
    {
        types.Byte(0x60);
        types.U32(type.Params);
        for (u32 i = 0; i < type.Params; i++)
            types.Byte(0x7F);
        types.U32(type.Result ? 1 : 0);
        if (type.Result)
            types.Byte(0x7F);
    }
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
        imports.U32(ImportTypes[i]);
    }
    Section(module, 2, imports);

    WasmCode functions;
    functions.U32(1);
    functions.U32(Type_Void1);
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
