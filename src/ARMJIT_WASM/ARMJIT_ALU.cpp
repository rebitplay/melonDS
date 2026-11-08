/*
    Rebit WASM JIT: data processing instructions (ARM and Thumb), with the interpreter's
    semantics (ARMInterpreter_ALU.cpp) and timing. Flags are only computed when a later
    instruction or the end of the block needs them (the front end's SetFlags).
*/
#include "ARMJIT_Compiler.h"

#include "../ARM.h"
#include "ARMJIT_Locals.h"

namespace melonDS
{
using namespace WasmLocals;
using namespace ARMInstrInfo;

namespace
{
enum
{
    op_AND, op_EOR, op_SUB, op_RSB, op_ADD, op_ADC, op_SBC, op_RSC,
    op_ORR, op_MOV, op_BIC, op_MVN, op_TST, op_TEQ, op_CMP, op_CMN,
};

bool IsLogical(int op)
{
    return op == op_AND || op == op_EOR || op == op_ORR || op == op_MOV || op == op_BIC
        || op == op_MVN || op == op_TST || op == op_TEQ;
}

bool IsTest(int op)
{
    return op >= op_TST;
}
}

void Compiler::Comp_WriteFlags(u8 flags)
{
    const u8 written = flags & CurInstr.SetFlags;
    if (!written)
        return;

    u32 mask = 0;
    if (written & flag_N) mask |= 1u << 31;
    if (written & flag_Z) mask |= 1u << 30;
    if (written & flag_C) mask |= 1u << 29;
    if (written & flag_V) mask |= 1u << 28;

    Code.LocalGet(LocalCPSR);
    Code.I32Const(~mask);
    Code.Op(W_I32And);
    if (written & flag_N)
    {
        Code.LocalGet(LocalRes);
        Code.I32Const(0x80000000);
        Code.Op(W_I32And);
        Code.Op(W_I32Or);
    }
    if (written & flag_Z)
    {
        Code.LocalGet(LocalRes);
        Code.Op(W_I32Eqz);
        Code.I32Const(30);
        Code.Op(W_I32Shl);
        Code.Op(W_I32Or);
    }
    if (written & flag_C)
    {
        Code.LocalGet(LocalC);
        Code.I32Const(29);
        Code.Op(W_I32Shl);
        Code.Op(W_I32Or);
    }
    if (written & flag_V)
    {
        Code.LocalGet(LocalV);
        Code.I32Const(28);
        Code.Op(W_I32Shl);
        Code.Op(W_I32Or);
    }
    Code.LocalSet(LocalCPSR);
    CPSRDirty = true;
}

// LocalB shifted by a constant (the *_IMM shifter forms). With carry, LocalC gets the shifter's
// carry out, or the old C flag when the shift leaves it.
void Compiler::Comp_ShiftImm(int op, int amount, bool carry)
{
    switch (op)
    {
    case 0: // LSL
        if (amount == 0)
        {
            if (carry)
            {
                PushCarry();
                Code.LocalSet(LocalC);
            }
            return;
        }
        if (carry)
        {
            Code.LocalGet(LocalB);
            Code.I32Const(32 - amount);
            Code.Op(W_I32ShrU);
            Code.I32Const(1);
            Code.Op(W_I32And);
            Code.LocalSet(LocalC);
        }
        Code.LocalGet(LocalB);
        Code.I32Const(amount);
        Code.Op(W_I32Shl);
        Code.LocalSet(LocalB);
        return;
    case 1: // LSR (0 means 32)
    case 2: // ASR (0 means 32)
        if (carry)
        {
            Code.LocalGet(LocalB);
            Code.I32Const(amount == 0 ? 31 : amount - 1);
            Code.Op(W_I32ShrU);
            Code.I32Const(1);
            Code.Op(W_I32And);
            Code.LocalSet(LocalC);
        }
        if (op == 1 && amount == 0)
        {
            Code.I32Const(0);
        }
        else
        {
            Code.LocalGet(LocalB);
            Code.I32Const(amount == 0 ? 31 : amount);
            Code.Op(op == 1 ? W_I32ShrU : W_I32ShrS);
        }
        Code.LocalSet(LocalB);
        return;
    case 3: // ROR (0 means RRX)
        if (amount == 0)
        {
            if (carry)
            {
                Code.LocalGet(LocalB);
                Code.I32Const(1);
                Code.Op(W_I32And);
                Code.LocalSet(LocalT);
            }
            Code.LocalGet(LocalB);
            Code.I32Const(1);
            Code.Op(W_I32ShrU);
            PushCarry();
            Code.I32Const(31);
            Code.Op(W_I32Shl);
            Code.Op(W_I32Or);
            Code.LocalSet(LocalB);
            if (carry)
            {
                Code.LocalGet(LocalT);
                Code.LocalSet(LocalC);
            }
            return;
        }
        if (carry)
        {
            Code.LocalGet(LocalB);
            Code.I32Const(amount - 1);
            Code.Op(W_I32ShrU);
            Code.I32Const(1);
            Code.Op(W_I32And);
            Code.LocalSet(LocalC);
        }
        Code.LocalGet(LocalB);
        Code.I32Const(amount);
        Code.Op(W_I32Rotr);
        Code.LocalSet(LocalB);
        return;
    }
}

// LocalB shifted by LocalT (0-255), the interpreter's *_REG and *_REG_S shifter macros.
void Compiler::Comp_ShiftReg(int op, bool carry)
{
    if (carry)
    {
        // the carry stays when the amount is 0
        PushCarry();
        Code.LocalSet(LocalC);

        Code.LocalGet(LocalT);
        Code.If();
        switch (op)
        {
        case 0: // LSL: >32: 0, 32: bit 0, else bit 32-s
        case 1: // LSR: >32: 0, 32: bit 31, else bit s-1
            Code.LocalGet(LocalT);
            Code.I32Const(32);
            Code.Op(W_I32GtU);
            Code.If();
            Code.I32Const(0);
            Code.LocalSet(LocalC);
            Code.Else();
            Code.LocalGet(LocalB);
            if (op == 0)
            {
                Code.I32Const(32);
                Code.LocalGet(LocalT);
                Code.Op(W_I32Sub); // 32 - s, which is 0 for s == 32: bit 0
            }
            else
            {
                Code.LocalGet(LocalT);
                Code.I32Const(1);
                Code.Op(W_I32Sub); // s - 1, which is 31 for s == 32
            }
            Code.Op(W_I32ShrU);
            Code.I32Const(1);
            Code.Op(W_I32And);
            Code.LocalSet(LocalC);
            Code.End();
            break;
        case 2: // ASR: >=32: bit 31, else bit s-1
            Code.LocalGet(LocalB);
            Code.LocalGet(LocalT);
            Code.I32Const(1);
            Code.Op(W_I32Sub);
            Code.I32Const(31);
            Code.LocalGet(LocalT);
            Code.I32Const(31);
            Code.Op(W_I32GtU);
            Code.Op(W_Select); // min(s - 1, 31)
            Code.Op(W_I32ShrU);
            Code.I32Const(1);
            Code.Op(W_I32And);
            Code.LocalSet(LocalC);
            break;
        case 3: // ROR: bit (s-1) & 31
            Code.LocalGet(LocalB);
            Code.LocalGet(LocalT);
            Code.I32Const(1);
            Code.Op(W_I32Sub);
            Code.Op(W_I32ShrU); // the shift count is taken mod 32
            Code.I32Const(1);
            Code.Op(W_I32And);
            Code.LocalSet(LocalC);
            break;
        }
        Code.End();
    }

    switch (op)
    {
    case 0: // LSL: s > 31 ? 0 : x << s
    case 1: // LSR
        Code.I32Const(0);
        Code.LocalGet(LocalB);
        Code.LocalGet(LocalT);
        Code.Op(op == 0 ? W_I32Shl : W_I32ShrU);
        Code.LocalGet(LocalT);
        Code.I32Const(31);
        Code.Op(W_I32GtU);
        Code.Op(W_Select);
        break;
    case 2: // ASR: x >> min(s, 31)
        Code.LocalGet(LocalB);
        Code.I32Const(31);
        Code.LocalGet(LocalT);
        Code.LocalGet(LocalT);
        Code.I32Const(31);
        Code.Op(W_I32GtU);
        Code.Op(W_Select);
        Code.Op(W_I32ShrS);
        break;
    case 3: // ROR by s & 31
        Code.LocalGet(LocalB);
        Code.LocalGet(LocalT);
        Code.Op(W_I32Rotr);
        break;
    }
    Code.LocalSet(LocalB);
}

// res = a op b into LocalRes, the flags, and rd unless it is a test.
void Compiler::Comp_ALUOp(int op, int rd, bool setFlags, bool shifterCarry)
{
    const u8 needed = setFlags ? CurInstr.SetFlags : 0;

    switch (op)
    {
    case op_AND: case op_TST:
        Code.LocalGet(LocalA); Code.LocalGet(LocalB); Code.Op(W_I32And); break;
    case op_EOR: case op_TEQ:
        Code.LocalGet(LocalA); Code.LocalGet(LocalB); Code.Op(W_I32Xor); break;
    case op_ORR:
        Code.LocalGet(LocalA); Code.LocalGet(LocalB); Code.Op(W_I32Or); break;
    case op_MOV:
        Code.LocalGet(LocalB); break;
    case op_BIC:
        Code.LocalGet(LocalA); Code.LocalGet(LocalB); Code.I32Const(-1); Code.Op(W_I32Xor); Code.Op(W_I32And); break;
    case op_MVN:
        Code.LocalGet(LocalB); Code.I32Const(-1); Code.Op(W_I32Xor); break;
    case op_SUB: case op_CMP: case op_RSB: case op_ADD: case op_CMN:
    {
        const bool reverse = op == op_RSB;
        const bool add = op == op_ADD || op == op_CMN;
        const u32 x = reverse ? LocalB : LocalA;
        const u32 y = reverse ? LocalA : LocalB;
        Code.LocalGet(x);
        Code.LocalGet(y);
        Code.Op(add ? W_I32Add : W_I32Sub);
        Code.LocalSet(LocalRes);
        if (needed & flag_C)
        {
            if (add) // carry: res < x
            {
                Code.LocalGet(LocalRes);
                Code.LocalGet(x);
                Code.Op(W_I32LtU);
            }
            else // no borrow: x >= y
            {
                Code.LocalGet(x);
                Code.LocalGet(y);
                Code.Op(W_I32GeU);
            }
            Code.LocalSet(LocalC);
        }
        if (needed & flag_V)
        {
            // add: ~(x ^ y) & (x ^ res); sub: (x ^ y) & (x ^ res); bit 31
            Code.LocalGet(x);
            Code.LocalGet(y);
            Code.Op(W_I32Xor);
            if (add)
            {
                Code.I32Const(-1);
                Code.Op(W_I32Xor);
            }
            Code.LocalGet(x);
            Code.LocalGet(LocalRes);
            Code.Op(W_I32Xor);
            Code.Op(W_I32And);
            Code.I32Const(31);
            Code.Op(W_I32ShrU);
            Code.LocalSet(LocalV);
        }
        Code.LocalGet(LocalRes);
        break;
    }
    case op_ADC: case op_SBC: case op_RSC:
    {
        const bool reverse = op == op_RSC;
        const bool add = op == op_ADC;
        const u32 x = reverse ? LocalB : LocalA;
        const u32 y = reverse ? LocalA : LocalB;
        // low word
        Code.LocalGet(x);
        Code.LocalGet(y);
        Code.Op(add ? W_I32Add : W_I32Sub);
        PushCarry();
        if (!add)
        {
            Code.I32Const(1);
            Code.Op(W_I32Xor);
        }
        Code.Op(add ? W_I32Add : W_I32Sub);
        Code.LocalSet(LocalRes);
        if (needed & flag_C)
        {
            Code.LocalGet(x);
            Code.Op(W_I64ExtendI32U);
            Code.LocalGet(y);
            Code.Op(W_I64ExtendI32U);
            Code.Op(add ? W_I64Add : W_I64Sub);
            PushCarry();
            if (!add)
            {
                Code.I32Const(1);
                Code.Op(W_I32Xor);
            }
            Code.Op(W_I64ExtendI32U);
            Code.Op(add ? W_I64Add : W_I64Sub);
            Code.I64Const(32);
            Code.Op(W_I64ShrU);
            Code.Op(W_I32WrapI64);
            // add: the carry is bit 32 (0 or 1); sub: no borrow when the high word is 0
            if (!add)
                Code.Op(W_I32Eqz);
            else
            {
                Code.I32Const(1);
                Code.Op(W_I32And);
            }
            Code.LocalSet(LocalC);
        }
        if (needed & flag_V)
        {
            Code.LocalGet(x);
            Code.LocalGet(y);
            Code.Op(W_I32Xor);
            if (add)
            {
                Code.I32Const(-1);
                Code.Op(W_I32Xor);
            }
            Code.LocalGet(x);
            Code.LocalGet(LocalRes);
            Code.Op(W_I32Xor);
            Code.Op(W_I32And);
            Code.I32Const(31);
            Code.Op(W_I32ShrU);
            Code.LocalSet(LocalV);
        }
        Code.LocalGet(LocalRes);
        break;
    }
    }
    Code.LocalSet(LocalRes);

    if (setFlags)
    {
        u8 flags = flag_N | flag_Z;
        if (IsLogical(op))
        {
            if (shifterCarry)
                flags |= flag_C;
        }
        else
            flags |= flag_C | flag_V;
        Comp_WriteFlags(flags);
    }

    if (!IsTest(op))
    {
        Code.LocalGet(LocalRes);
        SetReg(rd);
    }
}

void Compiler::A_Comp_ALU()
{
    const u16 kind = CurInstr.Info.Kind;
    int op, form;
    bool s;
    if (kind < ak_TST_REG_LSL_IMM)
    {
        op = kind / 18;
        form = (kind % 18) % 9;
        s = (kind % 18) >= 9;
    }
    else
    {
        op = op_TST + (kind - ak_TST_REG_LSL_IMM) / 9;
        form = (kind - ak_TST_REG_LSL_IMM) % 9;
        s = true;
    }

    const u32 instr = CurInstr.Instr;
    const int rd = (instr >> 12) & 0xF;
    const int rn = (instr >> 16) & 0xF;
    const int rm = instr & 0xF;
    const bool shifterCarry = s && IsLogical(op) && (CurInstr.SetFlags & flag_C);

    // the second operand
    if (form == 8)
    {
        const u32 rot = (instr >> 7) & 0x1E;
        const u32 b = ROR(instr & 0xFF, rot);
        Code.I32Const(b);
        Code.LocalSet(LocalB);
        if (shifterCarry)
        {
            if (rot)
                Code.I32Const(b >> 31);
            else
                PushCarry();
            Code.LocalSet(LocalC);
        }
    }
    else if (form < 4)
    {
        PushReg(rm);
        Code.LocalSet(LocalB);
        Comp_ShiftImm(form, (instr >> 7) & 0x1F, shifterCarry);
    }
    else
    {
        if (rm == 15)
            Code.I32Const(R15 + 4);
        else
            PushReg(rm);
        Code.LocalSet(LocalB);
        PushReg((instr >> 8) & 0xF);
        Code.I32Const(0xFF);
        Code.Op(W_I32And);
        Code.LocalSet(LocalT);
        Comp_ShiftReg(form - 4, shifterCarry);
    }

    if (op != op_MOV && op != op_MVN)
    {
        PushReg(rn);
        Code.LocalSet(LocalA);
    }

    if (form >= 4 && form < 8)
        Comp_AddCycles_CI(1);
    else
        Comp_AddCycles_C();

    Comp_ALUOp(op, rd, s, shifterCarry);
}

void Compiler::T_Comp_ShiftImm()
{
    // LSL/LSR/ASR rd, rs, #imm: NZ and the shifter's carry
    const u32 instr = CurInstr.Instr;
    const int op = CurInstr.Info.Kind - tk_LSL_IMM;
    PushReg((instr >> 3) & 0x7);
    Code.LocalSet(LocalB);
    Comp_ShiftImm(op, (instr >> 6) & 0x1F, true);
    Code.LocalGet(LocalB);
    Code.LocalSet(LocalRes);
    // LSL #0 leaves C; Comp_ShiftImm put the old C in LocalC
    Comp_WriteFlags(flag_N | flag_Z | flag_C);
    Code.LocalGet(LocalRes);
    SetReg(instr & 0x7);
    Comp_AddCycles_C();
}

void Compiler::T_Comp_AddSub_()
{
    const u32 instr = CurInstr.Instr;
    const u16 kind = CurInstr.Info.Kind;
    PushReg((instr >> 3) & 0x7);
    Code.LocalSet(LocalA);
    if (kind == tk_ADD_REG_ || kind == tk_SUB_REG_)
        PushReg((instr >> 6) & 0x7);
    else
        Code.I32Const((instr >> 6) & 0x7);
    Code.LocalSet(LocalB);
    const bool add = kind == tk_ADD_REG_ || kind == tk_ADD_IMM_;
    Comp_ALUOp(add ? op_ADD : op_SUB, instr & 0x7, true, false);
    Comp_AddCycles_C();
}

void Compiler::T_Comp_ALU_Imm8()
{
    const u32 instr = CurInstr.Instr;
    const int rd = (instr >> 8) & 0x7;
    const u32 imm = instr & 0xFF;
    switch (CurInstr.Info.Kind)
    {
    case tk_MOV_IMM:
        // N is always cleared (SetNZ(0, !b))
        Code.I32Const(imm);
        Code.LocalSet(LocalRes);
        Comp_WriteFlags(flag_N | flag_Z);
        Code.I32Const(imm);
        SetReg(rd);
        break;
    case tk_CMP_IMM:
    case tk_ADD_IMM:
    case tk_SUB_IMM:
    {
        PushReg(rd);
        Code.LocalSet(LocalA);
        Code.I32Const(imm);
        Code.LocalSet(LocalB);
        const u16 kind = CurInstr.Info.Kind;
        Comp_ALUOp(kind == tk_CMP_IMM ? op_CMP : kind == tk_ADD_IMM ? op_ADD : op_SUB, rd, true, false);
        break;
    }
    }
    Comp_AddCycles_C();
}

void Compiler::T_Comp_ALU()
{
    const u32 instr = CurInstr.Instr;
    const int rd = instr & 0x7;
    const int rs = (instr >> 3) & 0x7;
    const u16 kind = CurInstr.Info.Kind;

    switch (kind)
    {
    case tk_LSL_REG: case tk_LSR_REG: case tk_ASR_REG: case tk_ROR_REG:
    {
        const int op = kind == tk_LSL_REG ? 0 : kind == tk_LSR_REG ? 1 : kind == tk_ASR_REG ? 2 : 3;
        PushReg(rd);
        Code.LocalSet(LocalB);
        PushReg(rs);
        Code.I32Const(0xFF);
        Code.Op(W_I32And);
        Code.LocalSet(LocalT);
        const bool carry = CurInstr.SetFlags & flag_C;
        Comp_ShiftReg(op, carry);
        Code.LocalGet(LocalB);
        Code.LocalSet(LocalRes);
        Comp_WriteFlags(flag_N | flag_Z | (carry ? flag_C : 0));
        Code.LocalGet(LocalRes);
        SetReg(rd);
        Comp_AddCycles_CI(1);
        return;
    }
    case tk_NEG_REG: // 0 - rs
        Code.I32Const(0);
        Code.LocalSet(LocalA);
        PushReg(rs);
        Code.LocalSet(LocalB);
        Comp_ALUOp(op_SUB, rd, true, false);
        Comp_AddCycles_C();
        return;
    default:
        break;
    }

    int op;
    switch (kind)
    {
    case tk_AND_REG: op = op_AND; break;
    case tk_EOR_REG: op = op_EOR; break;
    case tk_ADC_REG: op = op_ADC; break;
    case tk_SBC_REG: op = op_SBC; break;
    case tk_TST_REG: op = op_TST; break;
    case tk_CMP_REG: op = op_CMP; break;
    case tk_CMN_REG: op = op_CMN; break;
    case tk_ORR_REG: op = op_ORR; break;
    case tk_BIC_REG: op = op_BIC; break;
    default: op = op_MVN; break;
    }
    PushReg(rd);
    Code.LocalSet(LocalA);
    PushReg(rs);
    Code.LocalSet(LocalB);
    // the logical ones set N and Z only
    Comp_ALUOp(op, rd, true, false);
    Comp_AddCycles_C();
}

void Compiler::T_Comp_ALU_HiReg()
{
    const u32 instr = CurInstr.Instr;
    const int rd = (instr & 0x7) | ((instr >> 4) & 0x8);
    const int rs = (instr >> 3) & 0xF;
    switch (CurInstr.Info.Kind)
    {
    case tk_ADD_HIREG:
        Comp_AddCycles_C();
        PushReg(rd);
        PushReg(rs);
        Code.Op(W_I32Add);
        SetReg(rd);
        break;
    case tk_MOV_HIREG:
        Comp_AddCycles_C();
        PushReg(rs);
        SetReg(rd);
        break;
    case tk_CMP_HIREG:
        PushReg(rd);
        Code.LocalSet(LocalA);
        PushReg(rs);
        Code.LocalSet(LocalB);
        Comp_ALUOp(op_CMP, rd, true, false);
        Comp_AddCycles_C();
        break;
    }
}

void Compiler::T_Comp_RelAddr()
{
    const u32 instr = CurInstr.Instr;
    const int rd = (instr >> 8) & 0x7;
    const u32 offset = (instr & 0xFF) << 2;
    if (CurInstr.Info.Kind == tk_ADD_PCREL)
    {
        Code.I32Const((R15 & ~2) + offset);
    }
    else
    {
        PushReg(13);
        Code.I32Const(offset);
        Code.Op(W_I32Add);
    }
    SetReg(rd);
    Comp_AddCycles_C();
}

void Compiler::T_Comp_AddSP()
{
    const u32 instr = CurInstr.Instr;
    const u32 offset = (instr & 0x7F) << 2;
    PushReg(13);
    Code.I32Const(offset);
    Code.Op((instr & (1 << 7)) ? W_I32Sub : W_I32Add);
    SetReg(13);
    Comp_AddCycles_C();
}

}
