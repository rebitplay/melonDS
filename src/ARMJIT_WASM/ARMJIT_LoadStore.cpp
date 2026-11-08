/*
    Rebit WASM JIT: loads, stores, block transfers and branches to a register.

    Without fastmem (it needs memory mapping) the x64 back end calls melonDS's slow memory
    functions for every access. Here the common cases are inline: the ARM9's DTCM and main RAM,
    the ARM7's main RAM and WRAM, each with the JIT's check for code in the written bytes; the
    rest calls the same slow functions (SlowRead9 etc. in ARMJIT.cpp). Timing as the x64 back end.
*/
#include "ARMJIT_Compiler.h"

#include "../ARM.h"
#include "../ARMJIT.h"
#include "../ARMJIT_Memory.h"
#include "../NDS.h"
#include "ARMJIT_Locals.h"

namespace melonDS
{
using namespace WasmLocals;
using namespace ARMInstrInfo;

namespace
{
enum
{
    memop_Writeback = 1 << 0,
    memop_Post = 1 << 1,
    memop_SignExtend = 1 << 2,
    memop_Store = 1 << 3,
    memop_SubtractOffset = 1 << 4,
};

void JumpTo9(ARMv5* cpu, u32 addr, u32 restoreCPSR)
{
    cpu->JumpTo(addr, restoreCPSR);
}

void JumpTo7(ARMv4* cpu, u32 addr, u32 restoreCPSR)
{
    cpu->JumpTo(addr, restoreCPSR);
}

template <typename F>
u32 Pointer(F* func)
{
    return static_cast<u32>(reinterpret_cast<uintptr_t>(func));
}

u32 Pointer(const void* ptr)
{
    return static_cast<u32>(reinterpret_cast<uintptr_t>(ptr));
}
}

void Compiler::Comp_AddCycles_CDI()
{
    if (Num == 0)
        Comp_AddCycles_CD();
    else
    {
        IrregularCycles = true;

        s32 cycles;

        s32 numC = NDS.ARM7MemTimings[CurInstr.CodeCycles][Thumb ? 0 : 2];
        s32 numD = CurInstr.DataCycles;

        if ((CurInstr.DataRegion >> 24) == 0x02) // mainRAM
        {
            if (CodeRegion == 0x02)
                cycles = numC + numD;
            else
            {
                numC++;
                cycles = std::max(numC + numD - 3, std::max(numC, numD));
            }
        }
        else if (CodeRegion == 0x02)
        {
            numD++;
            cycles = std::max(numC + numD - 3, std::max(numC, numD));
        }
        else
        {
            cycles = numC + numD + 1;
        }

        if (!Thumb && CurInstr.Cond() < 0xE)
            Comp_AddCycles(cycles);
        else
            ConstantCycles += cycles;
    }
}

void Compiler::Comp_AddCycles_CD()
{
    u32 cycles = 0;
    if (Num == 0)
    {
        s32 numC = (R15 & 0x2) ? 0 : CurInstr.CodeCycles;
        s32 numD = CurInstr.DataCycles;

        cycles = std::max(numC + numD - 6, std::max(numC, numD));

        IrregularCycles = cycles != numC;
    }
    else
    {
        s32 numC = NDS.ARM7MemTimings[CurInstr.CodeCycles][Thumb ? 0 : 2];
        s32 numD = CurInstr.DataCycles;

        if ((CurInstr.DataRegion >> 4) == 0x02)
        {
            if (CodeRegion == 0x02)
                cycles += numC + numD;
            else
                cycles += std::max(numC + numD - 3, std::max(numC, numD));
        }
        else if (CodeRegion == 0x02)
        {
            cycles += std::max(numC + numD - 3, std::max(numC, numD));
        }
        else
        {
            cycles += numC + numD;
        }

        IrregularCycles = true;
    }

    if (!Thumb && CurInstr.Cond() < 0xE)
        Comp_AddCycles(cycles);
    else
        ConstantCycles += cycles;
}

// A load from the code's own literal pool, read now (the x64 back end's Comp_MemLoadLiteral):
// the front end invalidates the block if the literal is ever written.
bool Compiler::Comp_MemLoadLiteral(int size, bool signExtend, int rd, u32 addr)
{
    u32 localAddr = NDS.JIT.LocaliseCodeAddress(Num, addr);

    int invalidLiteralIdx = NDS.JIT.InvalidLiterals.Find(localAddr);
    if (invalidLiteralIdx != -1)
        return false;

    Comp_AddCycles_CDI();

    u32 val;
    // make sure arm7 bios is accessible
    u32 tmpR15 = CurCPU->R[15];
    CurCPU->R[15] = R15;
    if (size == 32)
    {
        CurCPU->DataRead32(addr & ~0x3, &val);
        val = melonDS::ROR(val, (addr & 0x3) << 3);
    }
    else if (size == 16)
    {
        CurCPU->DataRead16(addr & ~0x1, &val);
        if (signExtend)
            val = ((s32)val << 16) >> 16;
    }
    else
    {
        CurCPU->DataRead8(addr, &val);
        if (signExtend)
            val = ((s32)val << 24) >> 24;
    }
    CurCPU->R[15] = tmpR15;

    Code.I32Const(val);
    SetReg(rd);
    return true;
}

namespace
{
// a host address: base + (addr & mask), aligned for the access
void HostAddress(WasmCode& code, u32 base, u32 mask, int size)
{
    code.LocalGet(LocalT);
    code.I32Const(mask & ~(size / 8 - 1));
    code.Op(W_I32And);
    code.I32Const(base);
    code.Op(W_I32Add);
}

void LoadSized(WasmCode& code, int size, bool signExtend)
{
    if (size == 32)
    {
        code.I32Load(0);
        // a misaligned word is rotated
        code.LocalGet(LocalT);
        code.I32Const(3);
        code.Op(W_I32And);
        code.I32Const(3);
        code.Op(W_I32Shl);
        code.Op(W_I32Rotr);
    }
    else if (size == 16)
    {
        if (signExtend)
            code.I32Load16S(0);
        else
            code.I32Load16U(0);
    }
    else
    {
        if (signExtend)
            code.I32Load8S(0);
        else
            code.I32Load8U(0);
    }
}

void StoreSized(WasmCode& code, int size)
{
    code.LocalGet(LocalT2);
    if (size == 32)
        code.I32Store(0);
    else if (size == 16)
        code.I32Store16(0);
    else
        code.I32Store8(0);
}

// pushes 1 when the JIT has code in the 16 bytes at (addr & mask) of a region's code index
void HasCode(WasmCode& code, const AddressRange* index, u32 mask)
{
    code.LocalGet(LocalT);
    code.I32Const(mask);
    code.Op(W_I32And);
    code.I32Const(9);
    code.Op(W_I32ShrU);
    code.I32Const(sizeof(AddressRange));
    code.Op(W_I32Mul);
    code.I32Load(Pointer(index) + offsetof(AddressRange, Code));
    code.LocalGet(LocalT);
    code.I32Const(4);
    code.Op(W_I32ShrU); // shift counts are taken mod 32: bit (addr & 0x1FF) / 16
    code.Op(W_I32ShrU);
    code.I32Const(1);
    code.Op(W_I32And);
}
}

void Compiler::Comp_MemRead(int size, bool signExtend)
{
    const int sizeIndex = size == 32 ? 0 : size == 16 ? 1 : 2;
    auto slow = [&]() {
        if (Num == 0)
        {
            static const u32 funcs[3] = {Pointer(&SlowRead9<u32, 0>), Pointer(&SlowRead9<u16, 0>), Pointer(&SlowRead9<u8, 0>)};
            Code.LocalGet(LocalT);
            Code.LocalGet(LocalCPU);
            Code.Call(ImportFunc(funcs[sizeIndex], Type_I32_2));
        }
        else
        {
            static const u32 funcs[3] = {Pointer(&SlowRead7<u32, 0>), Pointer(&SlowRead7<u16, 0>), Pointer(&SlowRead7<u8, 0>)};
            Code.LocalGet(LocalT);
            Code.Call(ImportFunc(funcs[sizeIndex], Type_I32_1));
        }
        if (signExtend && size < 32)
        {
            Code.I32Const(32 - size);
            Code.Op(W_I32Shl);
            Code.I32Const(32 - size);
            Code.Op(W_I32ShrS);
        }
    };

    if (Num == 0)
    {
        ARMv5* cpu9 = (ARMv5*)CurCPU;
        // ITCM first (slow), then DTCM, then main RAM
        Code.LocalGet(LocalT);
        Code.LocalGet(LocalCPU);
        Code.I32Load(offsetof(ARMv5, ITCMSize));
        Code.Op(W_I32LtU);
        Code.IfI32();
        slow();
        Code.Else();
        Code.LocalGet(LocalT);
        Code.LocalGet(LocalCPU);
        Code.I32Load(offsetof(ARMv5, DTCMMask));
        Code.Op(W_I32And);
        Code.LocalGet(LocalCPU);
        Code.I32Load(offsetof(ARMv5, DTCMBase));
        Code.Op(W_I32Eq);
        Code.IfI32();
        HostAddress(Code, Pointer(cpu9->DTCM), 0x3FFF, size);
        LoadSized(Code, size, signExtend);
        Code.Else();
        Code.LocalGet(LocalT);
        Code.I32Const(24);
        Code.Op(W_I32ShrU);
        Code.I32Const(0x02);
        Code.Op(W_I32Eq);
        Code.IfI32();
        HostAddress(Code, Pointer(NDS.MainRAM), NDS.MainRAMMask, size);
        LoadSized(Code, size, signExtend);
        Code.Else();
        slow();
        Code.End();
        Code.End();
        Code.End();
    }
    else
    {
        // main RAM (both halves of 0x02), then the ARM7's own WRAM (0x038)
        Code.LocalGet(LocalT);
        Code.I32Const(24);
        Code.Op(W_I32ShrU);
        Code.I32Const(0x02);
        Code.Op(W_I32Eq);
        Code.IfI32();
        HostAddress(Code, Pointer(NDS.MainRAM), NDS.MainRAMMask, size);
        LoadSized(Code, size, signExtend);
        Code.Else();
        Code.LocalGet(LocalT);
        Code.I32Const(0xFF800000);
        Code.Op(W_I32And);
        Code.I32Const(0x03800000);
        Code.Op(W_I32Eq);
        Code.IfI32();
        HostAddress(Code, Pointer(NDS.ARM7WRAM), ARM7WRAMSize - 1, size);
        LoadSized(Code, size, signExtend);
        Code.Else();
        slow();
        Code.End();
        Code.End();
    }
}

void Compiler::Comp_MemWrite(int size)
{
    const int sizeIndex = size == 32 ? 0 : size == 16 ? 1 : 2;
    auto slow = [&]() {
        if (Num == 0)
        {
            static const u32 funcs[3] = {Pointer(&SlowWrite9<u32, 0>), Pointer(&SlowWrite9<u16, 0>), Pointer(&SlowWrite9<u8, 0>)};
            Code.LocalGet(LocalT);
            Code.LocalGet(LocalCPU);
            Code.LocalGet(LocalT2);
            Code.Call(ImportFunc(funcs[sizeIndex], Type_Void3));
        }
        else
        {
            static const u32 funcs[3] = {Pointer(&SlowWrite7<u32, 0>), Pointer(&SlowWrite7<u16, 0>), Pointer(&SlowWrite7<u8, 0>)};
            Code.LocalGet(LocalT);
            Code.LocalGet(LocalT2);
            Code.Call(ImportFunc(funcs[sizeIndex], Type_Void2));
        }
    };

    if (Num == 0)
    {
        ARMv5* cpu9 = (ARMv5*)CurCPU;
        Code.LocalGet(LocalT);
        Code.LocalGet(LocalCPU);
        Code.I32Load(offsetof(ARMv5, ITCMSize));
        Code.Op(W_I32LtU);
        Code.If();
        slow();
        Code.Else();
        Code.LocalGet(LocalT);
        Code.LocalGet(LocalCPU);
        Code.I32Load(offsetof(ARMv5, DTCMMask));
        Code.Op(W_I32And);
        Code.LocalGet(LocalCPU);
        Code.I32Load(offsetof(ARMv5, DTCMBase));
        Code.Op(W_I32Eq);
        Code.If();
        HostAddress(Code, Pointer(cpu9->DTCM), 0x3FFF, size);
        StoreSized(Code, size);
        Code.Else();
        // main RAM without code in the written bytes
        Code.LocalGet(LocalT);
        Code.I32Const(24);
        Code.Op(W_I32ShrU);
        Code.I32Const(0x02);
        Code.Op(W_I32Eq);
        HasCode(Code, NDS.JIT.CodeIndexMainRAM, NDS.MainRAMMask);
        Code.Op(W_I32Eqz);
        Code.Op(W_I32And);
        Code.If();
        HostAddress(Code, Pointer(NDS.MainRAM), NDS.MainRAMMask, size);
        StoreSized(Code, size);
        Code.Else();
        slow();
        Code.End();
        Code.End();
        Code.End();
    }
    else
    {
        Code.LocalGet(LocalT);
        Code.I32Const(24);
        Code.Op(W_I32ShrU);
        Code.I32Const(0x02);
        Code.Op(W_I32Eq);
        HasCode(Code, NDS.JIT.CodeIndexMainRAM, NDS.MainRAMMask);
        Code.Op(W_I32Eqz);
        Code.Op(W_I32And);
        Code.If();
        HostAddress(Code, Pointer(NDS.MainRAM), NDS.MainRAMMask, size);
        StoreSized(Code, size);
        Code.Else();
        Code.LocalGet(LocalT);
        Code.I32Const(0xFF800000);
        Code.Op(W_I32And);
        Code.I32Const(0x03800000);
        Code.Op(W_I32Eq);
        HasCode(Code, NDS.JIT.CodeIndexARM7WRAM, ARM7WRAMSize - 1);
        Code.Op(W_I32Eqz);
        Code.Op(W_I32And);
        Code.If();
        HostAddress(Code, Pointer(NDS.ARM7WRAM), ARM7WRAMSize - 1, size);
        StoreSized(Code, size);
        Code.Else();
        slow();
        Code.End();
        Code.End();
    }
}

void Compiler::Comp_MemAccess(int rd, int rn, const MemOffset& offset, int size, int flags)
{
    if (NDS.JIT.LiteralOptimizationsEnabled() && rn == 15 && rd != 15 && offset.Imm
        && !(flags & (memop_Post | memop_Store | memop_Writeback)))
    {
        u32 addr = R15 + offset.Value * ((flags & memop_SubtractOffset) ? -1 : 1);
        if (Comp_MemLoadLiteral(size, flags & memop_SignExtend, rd, addr))
            return;
    }

    if (flags & memop_Store)
        Comp_AddCycles_CD();
    else
        Comp_AddCycles_CDI();

    // the value to store, before any writeback changes rn
    if (flags & memop_Store)
    {
        PushReg(rd);
        Code.LocalSet(LocalT2);
    }

    // base
    if (Thumb && rn == 15)
        Code.I32Const(R15 & ~0x2);
    else
        PushReg(rn);
    Code.LocalSet(LocalA);

    // offset into LocalB
    if (offset.Imm)
    {
        Code.I32Const(offset.Value);
        Code.LocalSet(LocalB);
    }
    else
    {
        PushReg(offset.Reg);
        Code.LocalSet(LocalB);
        Comp_ShiftImm(offset.Op, offset.Amount, false);
    }

    // the address in LocalT; writeback
    if (flags & memop_Post)
    {
        Code.LocalGet(LocalA);
        Code.LocalSet(LocalT);
        Code.LocalGet(LocalA);
        Code.LocalGet(LocalB);
        Code.Op((flags & memop_SubtractOffset) ? W_I32Sub : W_I32Add);
        SetReg(rn);
    }
    else
    {
        Code.LocalGet(LocalA);
        Code.LocalGet(LocalB);
        Code.Op((flags & memop_SubtractOffset) ? W_I32Sub : W_I32Add);
        Code.LocalSet(LocalT);
        if (flags & memop_Writeback)
        {
            Code.LocalGet(LocalT);
            SetReg(rn);
        }
    }

    if (flags & memop_Store)
    {
        Comp_MemWrite(size);
    }
    else
    {
        Comp_MemRead(size, flags & memop_SignExtend);
        SetReg(rd);
    }
}

// The guest registers in `regs` to or from memory at rn (+/- before/after) through the slow block
// transfer functions; returns how far rn moves.
s32 Compiler::Comp_MemAccessBlock(int rn, u16 regs, bool store, bool preinc, bool decrement, bool skipLoadingRn)
{
    int regsCount = __builtin_popcount(regs);
    if (regsCount == 0)
        return 0; // actually not the right behaviour TODO: fix me (as the x64 back end)

    s32 offset = (regsCount * 4) * (decrement ? -1 : 1);

    if (!store)
        Comp_AddCycles_CDI();
    else
        Comp_AddCycles_CD();

    // the first address
    PushReg(rn);
    if (decrement)
        Code.I32Const(-regsCount * 4 + (preinc ? 0 : 4));
    else
        Code.I32Const(preinc ? 4 : 0);
    Code.Op(W_I32Add);
    Code.LocalSet(LocalT);

    const u32 data = Pointer(BlockTransferData);
    if (store)
    {
        int i = 0;
        for (int reg = 0; reg < 16; reg++)
        {
            if (!(regs & (1 << reg)))
                continue;
            Code.I32Const(data + i * 8);
            PushReg(reg);
            Code.I32Store(0);
            i++;
        }
    }

    Code.LocalGet(LocalT);
    Code.I32Const(data);
    Code.I32Const(regsCount);
    if (Num == 0)
    {
        Code.LocalGet(LocalCPU);
        Code.Call(ImportFunc(store ? Pointer(&SlowBlockTransfer9<true, 0>) : Pointer(&SlowBlockTransfer9<false, 0>), Type_Void4));
    }
    else
    {
        Code.Call(ImportFunc(store ? Pointer(&SlowBlockTransfer7<true, 0>) : Pointer(&SlowBlockTransfer7<false, 0>), Type_Void3));
    }

    if (!store)
    {
        int i = 0;
        for (int reg = 0; reg < 16; reg++)
        {
            if (!(regs & (1 << reg)))
                continue;
            if (reg != 15 && !(reg == rn && skipLoadingRn))
            {
                Code.I32Const(data + i * 8);
                Code.I32Load(0);
                SetReg(reg);
            }
            i++;
        }
    }

    return offset;
}

// A branch to the address on the stack. Only for unconditional instructions: it hands the
// registers back to memory, as ARM::JumpTo may switch modes.
void Compiler::Comp_JumpToReg(bool restoreCPSR)
{
    IrregularCycles = true;
    Code.LocalSet(LocalA);
    SaveCPSR();
    RegCache.Flush();
    Code.LocalGet(LocalCPU);
    Code.LocalGet(LocalA);
    Code.I32Const(restoreCPSR ? 1 : 0);
    Code.Call(ImportFunc(Num == 0 ? Pointer(&JumpTo9) : Pointer(&JumpTo7), Type_Void3));
    LoadCPSR();
}

void Compiler::A_Comp_MemWB()
{
    bool load = CurInstr.Instr & (1 << 20);
    bool byte = CurInstr.Instr & (1 << 22);
    int size = byte ? 8 : 32;

    int flags = 0;
    if (!load)
        flags |= memop_Store;
    if (!(CurInstr.Instr & (1 << 24)))
        flags |= memop_Post;
    if (CurInstr.Instr & (1 << 21))
        flags |= memop_Writeback;
    if (!(CurInstr.Instr & (1 << 23)))
        flags |= memop_SubtractOffset;

    MemOffset offset {};
    if (!(CurInstr.Instr & (1 << 25)))
    {
        offset.Imm = true;
        offset.Value = CurInstr.Instr & 0xFFF;
    }
    else
    {
        offset.Op = (CurInstr.Instr >> 5) & 0x3;
        offset.Amount = (CurInstr.Instr >> 7) & 0x1F;
        offset.Reg = CurInstr.A_Reg(0);
    }

    Comp_MemAccess(CurInstr.A_Reg(12), CurInstr.A_Reg(16), offset, size, flags);
}

void Compiler::A_Comp_MemHalf()
{
    MemOffset offset {};
    if (CurInstr.Instr & (1 << 22))
    {
        offset.Imm = true;
        offset.Value = (CurInstr.Instr & 0xF) | ((CurInstr.Instr >> 4) & 0xF0);
    }
    else
    {
        offset.Reg = CurInstr.A_Reg(0);
    }

    int op = (CurInstr.Instr >> 5) & 0x3;
    bool load = CurInstr.Instr & (1 << 20);

    bool signExtend = false;
    int size = 16;
    if (load)
    {
        size = op == 2 ? 8 : 16;
        signExtend = op > 1;
    }

    int flags = 0;
    if (signExtend)
        flags |= memop_SignExtend;
    if (!load)
        flags |= memop_Store;
    if (!(CurInstr.Instr & (1 << 24)))
        flags |= memop_Post;
    if (!(CurInstr.Instr & (1 << 23)))
        flags |= memop_SubtractOffset;
    if (CurInstr.Instr & (1 << 21))
        flags |= memop_Writeback;

    Comp_MemAccess(CurInstr.A_Reg(12), CurInstr.A_Reg(16), offset, size, flags);
}

void Compiler::A_Comp_LDM_STM()
{
    u16 regs = CurInstr.Instr & 0xFFFF;

    bool load = CurInstr.Instr & (1 << 20);
    bool pre = CurInstr.Instr & (1 << 24);
    bool add = CurInstr.Instr & (1 << 23);
    bool writeback = CurInstr.Instr & (1 << 21);
    int rn = CurInstr.A_Reg(16);

    if (load && writeback && (regs & (1 << rn)))
        writeback = (Num == 0 && !(regs & ~(1 << rn))) || (regs & ~((2 << rn) - 1));

    s32 offset = Comp_MemAccessBlock(rn, regs, !load, pre, !add, load && writeback);

    if (writeback && offset)
    {
        PushReg(rn);
        Code.I32Const(offset);
        Code.Op(W_I32Add);
        SetReg(rn);
    }

    if (load && (regs & (1 << 15)))
    {
        // the loaded PC: the last word of the transfer
        const int count = __builtin_popcount(regs);
        Code.I32Const(Pointer(BlockTransferData) + (count - 1) * 8);
        Code.I32Load(0);
        if (Num == 1)
        {
            Code.I32Const(~1);
            Code.Op(W_I32And);
        }
        Comp_JumpToReg(false);
    }
}

void Compiler::A_Comp_BranchXchangeReg()
{
    PushReg(CurInstr.A_Reg(0));
    Code.LocalSet(LocalT);
    if ((CurInstr.Instr & 0xF0) == 0x30) // BLX_reg
    {
        Code.I32Const(R15 - 4);
        SetReg(14);
    }
    Code.LocalGet(LocalT);
    Comp_JumpToReg(false);
}

void Compiler::T_Comp_MemReg()
{
    int op = (CurInstr.Instr >> 10) & 0x3;
    bool load = op & 0x2;
    bool byte = op & 0x1;

    MemOffset offset {};
    offset.Reg = CurInstr.T_Reg(6);
    Comp_MemAccess(CurInstr.T_Reg(0), CurInstr.T_Reg(3), offset, byte ? 8 : 32, load ? 0 : memop_Store);
}

void Compiler::T_Comp_MemImm()
{
    int op = (CurInstr.Instr >> 11) & 0x3;
    bool load = op & 0x1;
    bool byte = op & 0x2;

    MemOffset offset {};
    offset.Imm = true;
    offset.Value = ((CurInstr.Instr >> 6) & 0x1F) * (byte ? 1 : 4);
    Comp_MemAccess(CurInstr.T_Reg(0), CurInstr.T_Reg(3), offset, byte ? 8 : 32, load ? 0 : memop_Store);
}

void Compiler::T_Comp_MemRegHalf()
{
    int op = (CurInstr.Instr >> 10) & 0x3;
    bool load = op != 0;
    int size = op != 1 ? 16 : 8;
    bool signExtend = op & 1;

    int flags = 0;
    if (signExtend)
        flags |= memop_SignExtend;
    if (!load)
        flags |= memop_Store;

    MemOffset offset {};
    offset.Reg = CurInstr.T_Reg(6);
    Comp_MemAccess(CurInstr.T_Reg(0), CurInstr.T_Reg(3), offset, size, flags);
}

void Compiler::T_Comp_MemImmHalf()
{
    bool load = CurInstr.Instr & (1 << 11);
    MemOffset offset {};
    offset.Imm = true;
    offset.Value = (CurInstr.Instr >> 5) & 0x3E;
    Comp_MemAccess(CurInstr.T_Reg(0), CurInstr.T_Reg(3), offset, 16, load ? 0 : memop_Store);
}

void Compiler::T_Comp_LoadPCRel()
{
    u32 imm = (CurInstr.Instr & 0xFF) << 2;
    u32 addr = (R15 & ~0x2) + imm;
    if (!NDS.JIT.LiteralOptimizationsEnabled() || !Comp_MemLoadLiteral(32, false, CurInstr.T_Reg(8), addr))
    {
        MemOffset offset {};
        offset.Imm = true;
        offset.Value = imm;
        Comp_MemAccess(CurInstr.T_Reg(8), 15, offset, 32, 0);
    }
}

void Compiler::T_Comp_MemSPRel()
{
    bool load = CurInstr.Instr & (1 << 11);
    MemOffset offset {};
    offset.Imm = true;
    offset.Value = (CurInstr.Instr & 0xFF) * 4;
    Comp_MemAccess(CurInstr.T_Reg(8), 13, offset, 32, load ? 0 : memop_Store);
}

void Compiler::T_Comp_PUSH_POP()
{
    bool load = CurInstr.Instr & (1 << 11);
    u16 regs = CurInstr.Instr & 0xFF;
    if (CurInstr.Instr & (1 << 8))
    {
        if (load)
            regs |= 1 << 15;
        else
            regs |= 1 << 14;
    }

    s32 offset = Comp_MemAccessBlock(13, regs, !load, !load, !load, false);

    if (offset)
    {
        PushReg(13);
        Code.I32Const(offset);
        Code.Op(W_I32Add);
        SetReg(13);
    }

    if (load && (regs & (1 << 15)))
    {
        const int count = __builtin_popcount(regs);
        Code.I32Const(Pointer(BlockTransferData) + (count - 1) * 8);
        Code.I32Load(0);
        if (Num == 1)
        {
            Code.I32Const(1);
            Code.Op(W_I32Or);
        }
        Comp_JumpToReg(false);
    }
}

void Compiler::T_Comp_LDMIA_STMIA()
{
    u16 regs = CurInstr.Instr & 0xFF;
    int rb = CurInstr.T_Reg(8);
    bool load = CurInstr.Instr & (1 << 11);

    bool writeback = !load || !(regs & (1 << rb));

    s32 offset = Comp_MemAccessBlock(rb, regs, !load, false, false, load && writeback);

    if (writeback && offset)
    {
        PushReg(rb);
        Code.I32Const(offset);
        Code.Op(W_I32Add);
        SetReg(rb);
    }
}

void Compiler::T_Comp_BranchXchangeReg()
{
    bool link = CurInstr.Instr & (1 << 7);
    PushReg(CurInstr.A_Reg(3));
    Code.LocalSet(LocalT);
    if (link)
    {
        Code.I32Const(R15 - 1);
        SetReg(14);
    }
    Code.LocalGet(LocalT);
    Comp_JumpToReg(false);
}

}
