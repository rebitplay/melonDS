/*
    Rebit: a WebAssembly back end for melonDS's JIT.

    melonDS's JIT front end (ARMJIT.cpp) finds a block of guest ARM/Thumb code, decodes it and
    works out its timing; a back end turns the block into host code. This back end writes each
    block as a small WebAssembly module with one function, f(cpu), which the browser compiles;
    the function goes into the module's function table, and its table index is the block's
    JitBlockEntry. ARM_Dispatch calls it through the table.

    It follows the x64 back end's structure and timing: guest registers live in WebAssembly
    locals for the length of a block (melonDS's RegisterCache), the CPSR in a local, and
    instructions without a translation call the interpreter's handler.
*/
#ifndef ARMJIT_WASM_COMPILER_H
#define ARMJIT_WASM_COMPILER_H

#if defined(JIT_ENABLED) && defined(__wasm__)

#include <vector>

#include "../ARMJIT_Internal.h"
#include "../ARMJIT_RegisterCache.h"

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
    void S64(s64 v);  // signed LEB128

    void LocalGet(u32 i) { Byte(0x20); U32(i); }
    void LocalSet(u32 i) { Byte(0x21); U32(i); }
    void LocalTee(u32 i) { Byte(0x22); U32(i); }
    void I32Const(s32 v) { Byte(0x41); S32(v); }
    void I64Const(s64 v) { Byte(0x42); S64(v); }
    void I32Load(u32 offset) { Byte(0x28); U32(2); U32(offset); }
    void I32Store(u32 offset) { Byte(0x36); U32(2); U32(offset); }
    void I32Load8U(u32 offset) { Byte(0x2D); U32(0); U32(offset); }
    void I32Load8S(u32 offset) { Byte(0x2C); U32(0); U32(offset); }
    void I32Load16U(u32 offset) { Byte(0x2F); U32(1); U32(offset); }
    void I32Load16S(u32 offset) { Byte(0x2E); U32(1); U32(offset); }
    void I32Store8(u32 offset) { Byte(0x3A); U32(0); U32(offset); }
    void I32Store16(u32 offset) { Byte(0x3B); U32(1); U32(offset); }
    void Call(u32 func) { Byte(0x10); U32(func); }
    void If() { Byte(0x04); Byte(0x40); }
    void IfI32() { Byte(0x04); Byte(0x7F); }
    void Else() { Byte(0x05); }
    void End() { Byte(0x0B); }
    void Return() { Byte(0x0F); }
    void Op(u8 op) { Byte(op); }
};

// WebAssembly opcodes used by the back end
enum : u8
{
    W_Drop = 0x1A,
    W_Select = 0x1B,
    W_I32Eqz = 0x45, W_I32Eq = 0x46, W_I32Ne = 0x47, W_I32LtS = 0x48, W_I32LtU = 0x49,
    W_I32GtS = 0x4A, W_I32GtU = 0x4B, W_I32LeU = 0x4D, W_I32GeU = 0x4F,
    W_I64Eqz = 0x50,
    W_I32Clz = 0x67,
    W_I32Add = 0x6A, W_I32Sub = 0x6B, W_I32Mul = 0x6C,
    W_I32And = 0x71, W_I32Or = 0x72, W_I32Xor = 0x73,
    W_I32Shl = 0x74, W_I32ShrS = 0x75, W_I32ShrU = 0x76, W_I32Rotr = 0x78,
    W_I64Add = 0x7C, W_I64Sub = 0x7D, W_I64Mul = 0x7E, W_I64ShrS = 0x87, W_I64ShrU = 0x88,
    W_I32WrapI64 = 0xA7, W_I64ExtendI32S = 0xAC, W_I64ExtendI32U = 0xAD,
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

    // RegisterCache's interface: a guest register to and from its local
    void LoadReg(int reg, int local);
    void SaveReg(int reg, int local);

    u8* CodeMemBase = nullptr;

    // statistics, for the frontend's diagnostics
    u32 BlocksCompiled = 0;
    u64 BytesEmitted = 0;
    u64 CompileMicroseconds = 0;

    typedef void (Compiler::*CompileFunc)();

private:
    melonDS::NDS& NDS;

    // per block
    WasmCode Code;
    std::vector<u32> Imports;  // function pointers (table indices) the block calls
    std::vector<u8> ImportTypes;
    RegisterCache<Compiler, int> RegCache;
    FetchedInstr CurInstr {};
    ARM* CurCPU = nullptr;
    bool Thumb = false;
    u32 Num = 0;
    u32 R15 = 0;
    u32 CodeRegion = 0;
    u32 ConstantCycles = 0;
    bool CPSRDirty = false;
    bool IrregularCycles = false;
    bool Exit = false;

    CompileFunc GetCompileFunc() const;
    CompileFunc GetCompileFuncUnmasked() const;

    u32 ImportFunc(u32 pointer, u8 type);
    void CallFunc(void (*func)(ARM*));

    void LoadCPSR();
    void SaveCPSR();
    void PushReg(int reg);    // a guest register's value (R15 is a constant)
    void SetReg(int reg);     // pops into a guest register
    void PushCarry();         // the CPSR's C flag, 0 or 1

    void Comp_CallInterpreter(void (*func)(ARM*));
    void Comp_Condition(u32 cond);  // pushes 1 when the condition holds
    void Comp_AddCycles(u32 cycles);
    void Comp_AddCycles_C(bool forceNonConstant = false);
    void Comp_AddCycles_CI(u32 i);
    void Comp_SpecialBranchBehaviour(bool taken);
    void Comp_JumpTo(u32 addr, bool forceNonConstantCycles = false);

    // flags from LocalRes (N, Z), LocalC and LocalV; only those the block needs
    void Comp_WriteFlags(u8 flags);
    void Comp_ShiftImm(int op, int amount, bool carry);  // LocalB, carry into LocalC
    void Comp_ShiftReg(int op, bool carry);             // LocalB by LocalT, carry into LocalC
    void Comp_ALUOp(int op, int rd, bool setFlags, bool shifterCarry);  // a in LocalA, b in LocalB

    // loads and stores (ARMJIT_LoadStore.cpp)
    struct MemOffset
    {
        bool Imm;
        u32 Value;            // the immediate
        int Reg, Op, Amount;  // or a register shifted by a constant
    };
    void Comp_AddCycles_CDI();
    void Comp_AddCycles_CD();
    bool Comp_MemLoadLiteral(int size, bool signExtend, int rd, u32 addr);
    void Comp_MemAccess(int rd, int rn, const MemOffset& offset, int size, int flags);
    void Comp_MemRead(int size, bool signExtend);  // address in LocalT; pushes the value
    void Comp_MemWrite(int size);                 // address in LocalT, value in LocalT2
    s32 Comp_MemAccessBlock(int rn, u16 regs, bool store, bool preinc, bool decrement, bool skipLoadingRn);
    void Comp_JumpToReg(bool restoreCPSR);        // to the address on the stack, through ARM::JumpTo

    void A_Comp_MemWB();
    void A_Comp_MemHalf();
    void A_Comp_LDM_STM();
    void A_Comp_BranchXchangeReg();
    void T_Comp_MemReg();
    void T_Comp_MemImm();
    void T_Comp_MemRegHalf();
    void T_Comp_MemImmHalf();
    void T_Comp_LoadPCRel();
    void T_Comp_MemSPRel();
    void T_Comp_PUSH_POP();
    void T_Comp_LDMIA_STMIA();
    void T_Comp_BranchXchangeReg();

    // LDM/STM data for the slow block transfer functions
    alignas(8) u64 BlockTransferData[16] {};

    void A_Comp_ALU();
    void A_Comp_BranchImm();
    void T_Comp_ShiftImm();
    void T_Comp_AddSub_();
    void T_Comp_ALU_Imm8();
    void T_Comp_ALU();
    void T_Comp_ALU_HiReg();
    void T_Comp_RelAddr();
    void T_Comp_AddSP();
    void T_Comp_BCOND();
    void T_Comp_B();
    void T_Comp_BL_Merged();

    u32 InstalledBlocks = 0;
};

}

#endif

#endif
