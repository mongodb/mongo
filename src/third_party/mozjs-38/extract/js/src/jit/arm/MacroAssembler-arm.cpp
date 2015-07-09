/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/MacroAssembler-arm.h"

#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/arm/Simulator-arm.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/MoveEmitter.h"

using namespace js;
using namespace jit;

using mozilla::Abs;
using mozilla::BitwiseCast;

bool
isValueDTRDCandidate(ValueOperand& val)
{
    // In order to be used for a DTRD memory function, the two target registers
    // need to be a) Adjacent, with the tag larger than the payload, and b)
    // Aligned to a multiple of two.
    if ((val.typeReg().code() != (val.payloadReg().code() + 1)))
        return false;
    if ((val.payloadReg().code() & 1) != 0)
        return false;
    return true;
}

void
MacroAssemblerARM::convertBoolToInt32(Register source, Register dest)
{
    // Note that C++ bool is only 1 byte, so zero extend it to clear the
    // higher-order bits.
    ma_and(Imm32(0xff), source, dest);
}

void
MacroAssemblerARM::convertInt32ToDouble(Register src, FloatRegister dest_)
{
    // Direct conversions aren't possible.
    VFPRegister dest = VFPRegister(dest_);
    as_vxfer(src, InvalidReg, dest.sintOverlay(),
             CoreToFloat);
    as_vcvt(dest, dest.sintOverlay());
}

void
MacroAssemblerARM::convertInt32ToDouble(const Address& src, FloatRegister dest)
{
    ma_vldr(Operand(src), ScratchDoubleReg);
    as_vcvt(dest, VFPRegister(ScratchDoubleReg).sintOverlay());
}

void
MacroAssemblerARM::convertUInt32ToDouble(Register src, FloatRegister dest_)
{
    // Direct conversions aren't possible.
    VFPRegister dest = VFPRegister(dest_);
    as_vxfer(src, InvalidReg, dest.uintOverlay(), CoreToFloat);
    as_vcvt(dest, dest.uintOverlay());
}

void
MacroAssemblerARM::convertUInt32ToFloat32(Register src, FloatRegister dest_)
{
    // Direct conversions aren't possible.
    VFPRegister dest = VFPRegister(dest_);
    as_vxfer(src, InvalidReg, dest.uintOverlay(), CoreToFloat);
    as_vcvt(VFPRegister(dest).singleOverlay(), dest.uintOverlay());
}

void MacroAssemblerARM::convertDoubleToFloat32(FloatRegister src, FloatRegister dest,
                                               Condition c)
{
    as_vcvt(VFPRegister(dest).singleOverlay(), VFPRegister(src), false, c);
}

// There are two options for implementing emitTruncateDouble:
//
// 1. Convert the floating point value to an integer, if it did not fit, then it
// was clamped to INT_MIN/INT_MAX, and we can test it. NOTE: if the value
// really was supposed to be INT_MAX / INT_MIN then it will be wrong.
//
// 2. Convert the floating point value to an integer, if it did not fit, then it
// set one or two bits in the fpcsr. Check those.
void
MacroAssemblerARM::branchTruncateDouble(FloatRegister src, Register dest, Label* fail)
{
    FloatRegister ScratchSIntReg = ScratchDoubleReg.sintOverlay();
    ma_vcvt_F64_I32(src, ScratchSIntReg);
    ma_vxfer(ScratchSIntReg, dest);
    ma_cmp(dest, Imm32(0x7fffffff));
    ma_cmp(dest, Imm32(0x80000000), Assembler::NotEqual);
    ma_b(fail, Assembler::Equal);
}

// Checks whether a double is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void
MacroAssemblerARM::convertDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail, bool negativeZeroCheck)
{
    // Convert the floating point value to an integer, if it did not fit, then
    // when we convert it *back* to a float, it will have a different value,
    // which we can test.
    FloatRegister ScratchSIntReg = ScratchDoubleReg.sintOverlay();
    ma_vcvt_F64_I32(src, ScratchSIntReg);
    // Move the value into the dest register.
    ma_vxfer(ScratchSIntReg, dest);
    ma_vcvt_I32_F64(ScratchSIntReg, ScratchDoubleReg);
    ma_vcmp(src, ScratchDoubleReg);
    as_vmrs(pc);
    ma_b(fail, Assembler::VFP_NotEqualOrUnordered);

    if (negativeZeroCheck) {
        ma_cmp(dest, Imm32(0));
        // Test and bail for -0.0, when integer result is 0. Move the top word
        // of the double into the output reg, if it is non-zero, then the
        // original value was -0.0.
        as_vxfer(dest, InvalidReg, src, FloatToCore, Assembler::Equal, 1);
        ma_cmp(dest, Imm32(0x80000000), Assembler::Equal);
        ma_b(fail, Assembler::Equal);
    }
}

// Checks whether a float32 is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void
MacroAssemblerARM::convertFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail, bool negativeZeroCheck)
{
    // Converting the floating point value to an integer and then converting it
    // back to a float32 would not work, as float to int32 conversions are
    // clamping (e.g. float(INT32_MAX + 1) would get converted into INT32_MAX
    // and then back to float(INT32_MAX + 1)).  If this ever happens, we just
    // bail out.
    FloatRegister ScratchSIntReg = ScratchFloat32Reg.sintOverlay();
    ma_vcvt_F32_I32(src, ScratchSIntReg);

    // Store the result
    ma_vxfer(ScratchSIntReg, dest);

    ma_vcvt_I32_F32(ScratchSIntReg, ScratchFloat32Reg);
    ma_vcmp(src, ScratchFloat32Reg);
    as_vmrs(pc);
    ma_b(fail, Assembler::VFP_NotEqualOrUnordered);

    // Bail out in the clamped cases.
    ma_cmp(dest, Imm32(0x7fffffff));
    ma_cmp(dest, Imm32(0x80000000), Assembler::NotEqual);
    ma_b(fail, Assembler::Equal);

    if (negativeZeroCheck) {
        ma_cmp(dest, Imm32(0));
        // Test and bail for -0.0, when integer result is 0. Move the float into
        // the output reg, and if it is non-zero then the original value was
        // -0.0
        as_vxfer(dest, InvalidReg, VFPRegister(src).singleOverlay(), FloatToCore, Assembler::Equal, 0);
        ma_cmp(dest, Imm32(0x80000000), Assembler::Equal);
        ma_b(fail, Assembler::Equal);
    }
}

void
MacroAssemblerARM::convertFloat32ToDouble(FloatRegister src, FloatRegister dest) {
    MOZ_ASSERT(dest.isDouble());
    MOZ_ASSERT(src.isSingle());
    as_vcvt(VFPRegister(dest), VFPRegister(src).singleOverlay());
}

void
MacroAssemblerARM::branchTruncateFloat32(FloatRegister src, Register dest, Label* fail) {
    ma_vcvt_F32_I32(src, ScratchFloat32Reg.sintOverlay());
    ma_vxfer(ScratchFloat32Reg, dest);
    ma_cmp(dest, Imm32(0x7fffffff));
    ma_cmp(dest, Imm32(0x80000000), Assembler::NotEqual);
    ma_b(fail, Assembler::Equal);
}

void
MacroAssemblerARM::convertInt32ToFloat32(Register src, FloatRegister dest) {
    // Direct conversions aren't possible.
    as_vxfer(src, InvalidReg, dest.sintOverlay(),
             CoreToFloat);
    as_vcvt(dest.singleOverlay(), dest.sintOverlay());
}

void
MacroAssemblerARM::convertInt32ToFloat32(const Address& src, FloatRegister dest) {
    ma_vldr(Operand(src), ScratchFloat32Reg);
    as_vcvt(dest, VFPRegister(ScratchFloat32Reg).sintOverlay());
}

void
MacroAssemblerARM::addDouble(FloatRegister src, FloatRegister dest)
{
    ma_vadd(dest, src, dest);
}

void
MacroAssemblerARM::subDouble(FloatRegister src, FloatRegister dest)
{
    ma_vsub(dest, src, dest);
}

void
MacroAssemblerARM::mulDouble(FloatRegister src, FloatRegister dest)
{
    ma_vmul(dest, src, dest);
}

void
MacroAssemblerARM::divDouble(FloatRegister src, FloatRegister dest)
{
    ma_vdiv(dest, src, dest);
}

void
MacroAssemblerARM::negateDouble(FloatRegister reg)
{
    ma_vneg(reg, reg);
}

void
MacroAssemblerARM::inc64(AbsoluteAddress dest)
{

    ma_strd(r0, r1, EDtrAddr(sp, EDtrOffImm(-8)), PreIndex);

    ma_mov(Imm32((int32_t)dest.addr), ScratchRegister);

    ma_ldrd(EDtrAddr(ScratchRegister, EDtrOffImm(0)), r0, r1);

    ma_add(Imm32(1), r0, SetCond);
    ma_adc(Imm32(0), r1, NoSetCond);

    ma_strd(r0, r1, EDtrAddr(ScratchRegister, EDtrOffImm(0)));

    ma_ldrd(EDtrAddr(sp, EDtrOffImm(8)), r0, r1, PostIndex);

}

bool
MacroAssemblerARM::alu_dbl(Register src1, Imm32 imm, Register dest, ALUOp op,
                           SetCond_ sc, Condition c)
{
    if ((sc == SetCond && ! condsAreSafe(op)) || !can_dbl(op))
        return false;
    ALUOp interop = getDestVariant(op);
    Imm8::TwoImm8mData both = Imm8::EncodeTwoImms(imm.value);
    if (both.fst.invalid)
        return false;
    // For the most part, there is no good reason to set the condition codes for
    // the first instruction. We can do better things if the second instruction
    // doesn't have a dest, such as check for overflow by doing first operation
    // don't do second operation if first operation overflowed. This preserves
    // the overflow condition code. Unfortunately, it is horribly brittle.
    as_alu(ScratchRegister, src1, both.fst, interop, NoSetCond, c);
    as_alu(dest, ScratchRegister, both.snd, op, sc, c);
    return true;
}


void
MacroAssemblerARM::ma_alu(Register src1, Imm32 imm, Register dest,
                          ALUOp op,
                          SetCond_ sc, Condition c)
{
    // As it turns out, if you ask for a compare-like instruction you *probably*
    // want it to set condition codes.
    if (dest == InvalidReg)
        MOZ_ASSERT(sc == SetCond);

    // The operator gives us the ability to determine how this can be used.
    Imm8 imm8 = Imm8(imm.value);
    // One instruction: If we can encode it using an imm8m, then do so.
    if (!imm8.invalid) {
        as_alu(dest, src1, imm8, op, sc, c);
        return;
    }
    // One instruction, negated:
    Imm32 negImm = imm;
    Register negDest;
    ALUOp negOp = ALUNeg(op, dest, &negImm, &negDest);
    Imm8 negImm8 = Imm8(negImm.value);
    // 'add r1, r2, -15' can be replaced with 'sub r1, r2, 15'. For bonus
    // points, dest can be replaced (nearly always invalid => ScratchRegister)
    // This is useful if we wish to negate tst. tst has an invalid (aka not
    // used) dest, but its negation is bic *requires* a dest. We can accomodate,
    // but it will need to clobber *something*, and the scratch register isn't
    // being used, so...
    if (negOp != OpInvalid && !negImm8.invalid) {
        as_alu(negDest, src1, negImm8, negOp, sc, c);
        return;
    }

    if (HasMOVWT()) {
        // If the operation is a move-a-like then we can try to use movw to move
        // the bits into the destination. Otherwise, we'll need to fall back on
        // a multi-instruction format :(
        // movw/movt does not set condition codes, so don't hold your breath.
        if (sc == NoSetCond && (op == OpMov || op == OpMvn)) {
            // ARMv7 supports movw/movt. movw zero-extends its 16 bit argument,
            // so we can set the register this way. movt leaves the bottom 16
            // bits in tact, so it is unsuitable to move a constant that
            if (op == OpMov && ((imm.value & ~ 0xffff) == 0)) {
                MOZ_ASSERT(src1 == InvalidReg);
                as_movw(dest, (uint16_t)imm.value, c);
                return;
            }

            // If they asked for a mvn rfoo, imm, where ~imm fits into 16 bits
            // then do it.
            if (op == OpMvn && (((~imm.value) & ~ 0xffff) == 0)) {
                MOZ_ASSERT(src1 == InvalidReg);
                as_movw(dest, (uint16_t)~imm.value, c);
                return;
            }

            // TODO: constant dedup may enable us to add dest, r0, 23 *if* we
            // are attempting to load a constant that looks similar to one that
            // already exists. If it can't be done with a single movw then we
            // *need* to use two instructions since this must be some sort of a
            // move operation, we can just use a movw/movt pair and get the
            // whole thing done in two moves. This does not work for ops like
            // add, since we'd need to do: movw tmp; movt tmp; add dest, tmp,
            // src1.
            if (op == OpMvn)
                imm.value = ~imm.value;
            as_movw(dest, imm.value & 0xffff, c);
            as_movt(dest, (imm.value >> 16) & 0xffff, c);
            return;
        }
        // If we weren't doing a movalike, a 16 bit immediate will require 2
        // instructions. With the same amount of space and (less)time, we can do
        // two 8 bit operations, reusing the dest register. e.g.
        //  movw tmp, 0xffff; add dest, src, tmp ror 4
        // vs.
        //  add dest, src, 0xff0; add dest, dest, 0xf000000f
        //
        // It turns out that there are some immediates that we miss with the
        // second approach. A sample value is: add dest, src, 0x1fffe this can
        // be done by movw tmp, 0xffff; add dest, src, tmp lsl 1 since imm8m's
        // only get even offsets, we cannot encode this. I'll try to encode as
        // two imm8's first, since they are faster. Both operations should take
        // 1 cycle, where as add dest, tmp ror 4 takes two cycles to execute.
    }

    // Either a) this isn't ARMv7 b) this isn't a move start by attempting to
    // generate a two instruction form. Some things cannot be made into two-inst
    // forms correctly. Namely, adds dest, src, 0xffff. Since we want the
    // condition codes (and don't know which ones will be checked), we need to
    // assume that the overflow flag will be checked and add{,s} dest, src,
    // 0xff00; add{,s} dest, dest, 0xff is not guaranteed to set the overflow
    // flag the same as the (theoretical) one instruction variant.
    if (alu_dbl(src1, imm, dest, op, sc, c))
        return;

    // And try with its negative.
    if (negOp != OpInvalid &&
        alu_dbl(src1, negImm, negDest, negOp, sc, c))
        return;

    // Well, damn. We can use two 16 bit mov's, then do the op or we can do a
    // single load from a pool then op.
    if (HasMOVWT()) {
        // Try to load the immediate into a scratch register then use that
        as_movw(ScratchRegister, imm.value & 0xffff, c);
        if ((imm.value >> 16) != 0)
            as_movt(ScratchRegister, (imm.value >> 16) & 0xffff, c);
    } else {
        // Going to have to use a load. If the operation is a move, then just
        // move it into the destination register
        if (op == OpMov) {
            as_Imm32Pool(dest, imm.value, c);
            return;
        } else {
            // If this isn't just going into a register, then stick it in a
            // temp, and then proceed.
            as_Imm32Pool(ScratchRegister, imm.value, c);
        }
    }
    as_alu(dest, src1, O2Reg(ScratchRegister), op, sc, c);
}

void
MacroAssemblerARM::ma_alu(Register src1, Operand op2, Register dest, ALUOp op,
            SetCond_ sc, Assembler::Condition c)
{
    MOZ_ASSERT(op2.getTag() == Operand::OP2);
    as_alu(dest, src1, op2.toOp2(), op, sc, c);
}

void
MacroAssemblerARM::ma_alu(Register src1, Operand2 op2, Register dest, ALUOp op, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, op2, op, sc, c);
}

void
MacroAssemblerARM::ma_nop()
{
    as_nop();
}

void
MacroAssemblerARM::ma_movPatchable(Imm32 imm_, Register dest, Assembler::Condition c,
                                   RelocStyle rs)
{
    int32_t imm = imm_.value;
    switch(rs) {
      case L_MOVWT:
        as_movw(dest, Imm16(imm & 0xffff), c);
        as_movt(dest, Imm16(imm >> 16 & 0xffff), c);
        break;
      case L_LDR:
        as_Imm32Pool(dest, imm, c);
        break;
    }
}

void
MacroAssemblerARM::ma_movPatchable(ImmPtr imm, Register dest, Assembler::Condition c,
                                   RelocStyle rs)
{
    ma_movPatchable(Imm32(int32_t(imm.value)), dest, c, rs);
}

/* static */ void
MacroAssemblerARM::ma_mov_patch(Imm32 imm_, Register dest, Assembler::Condition c,
                                RelocStyle rs, Instruction* i)
{
    MOZ_ASSERT(i);
    int32_t imm = imm_.value;

    // Make sure the current instruction is not an artificial guard inserted
    // by the assembler buffer.
    i = i->skipPool();

    switch(rs) {
      case L_MOVWT:
        Assembler::as_movw_patch(dest, Imm16(imm & 0xffff), c, i);
        i = i->next();
        Assembler::as_movt_patch(dest, Imm16(imm >> 16 & 0xffff), c, i);
        break;
      case L_LDR:
        Assembler::WritePoolEntry(i, c, imm);
        break;
    }
}

/* static */ void
MacroAssemblerARM::ma_mov_patch(ImmPtr imm, Register dest, Assembler::Condition c,
                                RelocStyle rs, Instruction* i)
{
    ma_mov_patch(Imm32(int32_t(imm.value)), dest, c, rs, i);
}

void
MacroAssemblerARM::ma_mov(Register src, Register dest, SetCond_ sc, Assembler::Condition c)
{
    if (sc == SetCond || dest != src)
        as_mov(dest, O2Reg(src), sc, c);
}

void
MacroAssemblerARM::ma_mov(Imm32 imm, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(InvalidReg, imm, dest, OpMov, sc, c);
}

void
MacroAssemblerARM::ma_mov(ImmWord imm, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(InvalidReg, Imm32(imm.value), dest, OpMov, sc, c);
}

void
MacroAssemblerARM::ma_mov(ImmGCPtr ptr, Register dest)
{
    // As opposed to x86/x64 version, the data relocation has to be executed
    // before to recover the pointer, and not after.
    writeDataRelocation(ptr);
    RelocStyle rs;
    if (HasMOVWT())
        rs = L_MOVWT;
    else
        rs = L_LDR;

    ma_movPatchable(Imm32(uintptr_t(ptr.value)), dest, Always, rs);
}

// Shifts (just a move with a shifting op2)
void
MacroAssemblerARM::ma_lsl(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, lsl(src, shift.value));
}
void
MacroAssemblerARM::ma_lsr(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, lsr(src, shift.value));
}
void
MacroAssemblerARM::ma_asr(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, asr(src, shift.value));
}
void
MacroAssemblerARM::ma_ror(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, ror(src, shift.value));
}
void
MacroAssemblerARM::ma_rol(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, rol(src, shift.value));
}

// Shifts (just a move with a shifting op2)
void
MacroAssemblerARM::ma_lsl(Register shift, Register src, Register dst)
{
    as_mov(dst, lsl(src, shift));
}
void
MacroAssemblerARM::ma_lsr(Register shift, Register src, Register dst)
{
    as_mov(dst, lsr(src, shift));
}
void
MacroAssemblerARM::ma_asr(Register shift, Register src, Register dst)
{
    as_mov(dst, asr(src, shift));
}
void
MacroAssemblerARM::ma_ror(Register shift, Register src, Register dst)
{
    as_mov(dst, ror(src, shift));
}
void
MacroAssemblerARM::ma_rol(Register shift, Register src, Register dst)
{
    ma_rsb(shift, Imm32(32), ScratchRegister);
    as_mov(dst, ror(src, ScratchRegister));
}

// Move not (dest <- ~src)
void
MacroAssemblerARM::ma_mvn(Imm32 imm, Register dest, SetCond_ sc, Assembler::Condition c)
{
    ma_alu(InvalidReg, imm, dest, OpMvn, sc, c);
}

void
MacroAssemblerARM::ma_mvn(Register src1, Register dest, SetCond_ sc, Assembler::Condition c)
{
    as_alu(dest, InvalidReg, O2Reg(src1), OpMvn, sc, c);
}

// Negate (dest <- -src), src is a register, rather than a general op2.
void
MacroAssemblerARM::ma_neg(Register src1, Register dest, SetCond_ sc, Assembler::Condition c)
{
    as_rsb(dest, src1, Imm8(0), sc, c);
}

// And.
void
MacroAssemblerARM::ma_and(Register src, Register dest, SetCond_ sc, Assembler::Condition c)
{
    ma_and(dest, src, dest);
}
void
MacroAssemblerARM::ma_and(Register src1, Register src2, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    as_and(dest, src1, O2Reg(src2), sc, c);
}
void
MacroAssemblerARM::ma_and(Imm32 imm, Register dest, SetCond_ sc, Assembler::Condition c)
{
    ma_alu(dest, imm, dest, OpAnd, sc, c);
}
void
MacroAssemblerARM::ma_and(Imm32 imm, Register src1, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(src1, imm, dest, OpAnd, sc, c);
}

// Bit clear (dest <- dest & ~imm) or (dest <- src1 & ~src2).
void
MacroAssemblerARM::ma_bic(Imm32 imm, Register dest, SetCond_ sc, Assembler::Condition c)
{
    ma_alu(dest, imm, dest, OpBic, sc, c);
}

// Exclusive or.
void
MacroAssemblerARM::ma_eor(Register src, Register dest, SetCond_ sc, Assembler::Condition c)
{
    ma_eor(dest, src, dest, sc, c);
}
void
MacroAssemblerARM::ma_eor(Register src1, Register src2, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    as_eor(dest, src1, O2Reg(src2), sc, c);
}
void
MacroAssemblerARM::ma_eor(Imm32 imm, Register dest, SetCond_ sc, Assembler::Condition c)
{
    ma_alu(dest, imm, dest, OpEor, sc, c);
}
void
MacroAssemblerARM::ma_eor(Imm32 imm, Register src1, Register dest,
       SetCond_ sc, Assembler::Condition c)
{
    ma_alu(src1, imm, dest, OpEor, sc, c);
}

// Or.
void
MacroAssemblerARM::ma_orr(Register src, Register dest, SetCond_ sc, Assembler::Condition c)
{
    ma_orr(dest, src, dest, sc, c);
}
void
MacroAssemblerARM::ma_orr(Register src1, Register src2, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    as_orr(dest, src1, O2Reg(src2), sc, c);
}
void
MacroAssemblerARM::ma_orr(Imm32 imm, Register dest, SetCond_ sc, Assembler::Condition c)
{
    ma_alu(dest, imm, dest, OpOrr, sc, c);
}
void
MacroAssemblerARM::ma_orr(Imm32 imm, Register src1, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(src1, imm, dest, OpOrr, sc, c);
}

// Arithmetic-based ops.
// Add with carry.
void
MacroAssemblerARM::ma_adc(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, OpAdc, sc, c);
}
void
MacroAssemblerARM::ma_adc(Register src, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, dest, O2Reg(src), OpAdc, sc, c);
}
void
MacroAssemblerARM::ma_adc(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), OpAdc, sc, c);
}

// Add.
void
MacroAssemblerARM::ma_add(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, OpAdd, sc, c);
}

void
MacroAssemblerARM::ma_add(Register src1, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, O2Reg(src1), dest, OpAdd, sc, c);
}
void
MacroAssemblerARM::ma_add(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), OpAdd, sc, c);
}
void
MacroAssemblerARM::ma_add(Register src1, Operand op, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op, dest, OpAdd, sc, c);
}
void
MacroAssemblerARM::ma_add(Register src1, Imm32 op, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op, dest, OpAdd, sc, c);
}

// Subtract with carry.
void
MacroAssemblerARM::ma_sbc(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, OpSbc, sc, c);
}
void
MacroAssemblerARM::ma_sbc(Register src1, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, dest, O2Reg(src1), OpSbc, sc, c);
}
void
MacroAssemblerARM::ma_sbc(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), OpSbc, sc, c);
}

// Subtract.
void
MacroAssemblerARM::ma_sub(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, OpSub, sc, c);
}
void
MacroAssemblerARM::ma_sub(Register src1, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, Operand(src1), dest, OpSub, sc, c);
}
void
MacroAssemblerARM::ma_sub(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, Operand(src2), dest, OpSub, sc, c);
}
void
MacroAssemblerARM::ma_sub(Register src1, Operand op, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op, dest, OpSub, sc, c);
}
void
MacroAssemblerARM::ma_sub(Register src1, Imm32 op, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op, dest, OpSub, sc, c);
}

// Severse subtract.
void
MacroAssemblerARM::ma_rsb(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, OpRsb, sc, c);
}
void
MacroAssemblerARM::ma_rsb(Register src1, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, dest, O2Reg(src1), OpAdd, sc, c);
}
void
MacroAssemblerARM::ma_rsb(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), OpRsb, sc, c);
}
void
MacroAssemblerARM::ma_rsb(Register src1, Imm32 op2, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op2, dest, OpRsb, sc, c);
}

// Reverse subtract with carry.
void
MacroAssemblerARM::ma_rsc(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, OpRsc, sc, c);
}
void
MacroAssemblerARM::ma_rsc(Register src1, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, dest, O2Reg(src1), OpRsc, sc, c);
}
void
MacroAssemblerARM::ma_rsc(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), OpRsc, sc, c);
}

// Compares/tests.
// Compare negative (sets condition codes as src1 + src2 would).
void
MacroAssemblerARM::ma_cmn(Register src1, Imm32 imm, Condition c)
{
    ma_alu(src1, imm, InvalidReg, OpCmn, SetCond, c);
}
void
MacroAssemblerARM::ma_cmn(Register src1, Register src2, Condition c)
{
    as_alu(InvalidReg, src2, O2Reg(src1), OpCmn, SetCond, c);
}
void
MacroAssemblerARM::ma_cmn(Register src1, Operand op, Condition c)
{
    MOZ_CRASH("Feature NYI");
}

// Compare (src - src2).
void
MacroAssemblerARM::ma_cmp(Register src1, Imm32 imm, Condition c)
{
    ma_alu(src1, imm, InvalidReg, OpCmp, SetCond, c);
}

void
MacroAssemblerARM::ma_cmp(Register src1, ImmWord ptr, Condition c)
{
    ma_cmp(src1, Imm32(ptr.value), c);
}

void
MacroAssemblerARM::ma_cmp(Register src1, ImmGCPtr ptr, Condition c)
{
    ma_mov(ptr, ScratchRegister);
    ma_cmp(src1, ScratchRegister, c);
}
void
MacroAssemblerARM::ma_cmp(Register src1, Operand op, Condition c)
{
    switch (op.getTag()) {
      case Operand::OP2:
        as_cmp(src1, op.toOp2(), c);
        break;
      case Operand::MEM:
        ma_ldr(op, ScratchRegister);
        as_cmp(src1, O2Reg(ScratchRegister), c);
        break;
      default:
        MOZ_CRASH("trying to compare FP and integer registers");
    }
}
void
MacroAssemblerARM::ma_cmp(Register src1, Register src2, Condition c)
{
    as_cmp(src1, O2Reg(src2), c);
}

// Test for equality, (src1 ^ src2).
void
MacroAssemblerARM::ma_teq(Register src1, Imm32 imm, Condition c)
{
    ma_alu(src1, imm, InvalidReg, OpTeq, SetCond, c);
}
void
MacroAssemblerARM::ma_teq(Register src1, Register src2, Condition c)
{
    as_tst(src1, O2Reg(src2), c);
}
void
MacroAssemblerARM::ma_teq(Register src1, Operand op, Condition c)
{
    as_teq(src1, op.toOp2(), c);
}


// Test (src1 & src2).
void
MacroAssemblerARM::ma_tst(Register src1, Imm32 imm, Condition c)
{
    ma_alu(src1, imm, InvalidReg, OpTst, SetCond, c);
}
void
MacroAssemblerARM::ma_tst(Register src1, Register src2, Condition c)
{
    as_tst(src1, O2Reg(src2), c);
}
void
MacroAssemblerARM::ma_tst(Register src1, Operand op, Condition c)
{
    as_tst(src1, op.toOp2(), c);
}

void
MacroAssemblerARM::ma_mul(Register src1, Register src2, Register dest)
{
    as_mul(dest, src1, src2);
}
void
MacroAssemblerARM::ma_mul(Register src1, Imm32 imm, Register dest)
{

    ma_mov(imm, ScratchRegister);
    as_mul( dest, src1, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARM::ma_check_mul(Register src1, Register src2, Register dest, Condition cond)
{
    // TODO: this operation is illegal on armv6 and earlier if src2 ==
    // ScratchRegister or src2 == dest.
    if (cond == Equal || cond == NotEqual) {
        as_smull(ScratchRegister, dest, src1, src2, SetCond);
        return cond;
    }

    if (cond == Overflow) {
        as_smull(ScratchRegister, dest, src1, src2);
        as_cmp(ScratchRegister, asr(dest, 31));
        return NotEqual;
    }

    MOZ_CRASH("Condition NYI");
}

Assembler::Condition
MacroAssemblerARM::ma_check_mul(Register src1, Imm32 imm, Register dest, Condition cond)
{
    ma_mov(imm, ScratchRegister);
    if (cond == Equal || cond == NotEqual) {
        as_smull(ScratchRegister, dest, ScratchRegister, src1, SetCond);
        return cond;
    }

    if (cond == Overflow) {
        as_smull(ScratchRegister, dest, ScratchRegister, src1);
        as_cmp(ScratchRegister, asr(dest, 31));
        return NotEqual;
    }

    MOZ_CRASH("Condition NYI");
}

void
MacroAssemblerARM::ma_mod_mask(Register src, Register dest, Register hold, Register tmp,
                               int32_t shift)
{
    // We wish to compute x % (1<<y) - 1 for a known constant, y.
    //
    // 1. Let b = (1<<y) and C = (1<<y)-1, then think of the 32 bit dividend as
    // a number in base b, namely c_0*1 + c_1*b + c_2*b^2 ... c_n*b^n
    //
    // 2. Since both addition and multiplication commute with modulus:
    //   x % C == (c_0 + c_1*b + ... + c_n*b^n) % C ==
    //    (c_0 % C) + (c_1%C) * (b % C) + (c_2 % C) * (b^2 % C)...
    //
    // 3. Since b == C + 1, b % C == 1, and b^n % C == 1 the whole thing
    // simplifies to: c_0 + c_1 + c_2 ... c_n % C
    //
    // Each c_n can easily be computed by a shift/bitextract, and the modulus
    // can be maintained by simply subtracting by C whenever the number gets
    // over C.
    int32_t mask = (1 << shift) - 1;
    Label head;

    // Register 'hold' holds -1 if the value was negative, 1 otherwise. The
    // ScratchRegister holds the remaining bits that have not been processed lr
    // serves as a temporary location to store extracted bits into as well as
    // holding the trial subtraction as a temp value dest is the accumulator
    // (and holds the final result)
    //
    // Move the whole value into tmp, setting the codition codes so we can muck
    // with them later.
    //
    // Note that we cannot use ScratchRegister in place of tmp here, as ma_and
    // below on certain architectures move the mask into ScratchRegister before
    // performing the bitwise and.
    as_mov(tmp, O2Reg(src), SetCond);
    // Zero out the dest.
    ma_mov(Imm32(0), dest);
    // Set the hold appropriately.
    ma_mov(Imm32(1), hold);
    ma_mov(Imm32(-1), hold, NoSetCond, Signed);
    ma_rsb(Imm32(0), tmp, SetCond, Signed);
    // Begin the main loop.
    bind(&head);

    // Extract the bottom bits into lr.
    ma_and(Imm32(mask), tmp, secondScratchReg_);
    // Add those bits to the accumulator.
    ma_add(secondScratchReg_, dest, dest);
    // Do a trial subtraction, this is the same operation as cmp, but we store
    // the dest.
    ma_sub(dest, Imm32(mask), secondScratchReg_, SetCond);
    // If (sum - C) > 0, store sum - C back into sum, thus performing a modulus.
    ma_mov(secondScratchReg_, dest, NoSetCond, NotSigned);
    // Get rid of the bits that we extracted before, and set the condition codes.
    as_mov(tmp, lsr(tmp, shift), SetCond);
    // If the shift produced zero, finish, otherwise, continue in the loop.
    ma_b(&head, NonZero);
    // Check the hold to see if we need to negate the result. Hold can only be
    // 1 or -1, so this will never set the 0 flag.
    ma_cmp(hold, Imm32(0));
    // If the hold was non-zero, negate the result to be in line with what JS
    // wants this will set the condition codes if we try to negate.
    ma_rsb(Imm32(0), dest, SetCond, Signed);
    // Since the Zero flag is not set by the compare, we can *only* set the Zero
    // flag in the rsb, so Zero is set iff we negated zero (e.g. the result of
    // the computation was -0.0).
}

void
MacroAssemblerARM::ma_smod(Register num, Register div, Register dest)
{
    as_sdiv(ScratchRegister, num, div);
    as_mls(dest, num, ScratchRegister, div);
}

void
MacroAssemblerARM::ma_umod(Register num, Register div, Register dest)
{
    as_udiv(ScratchRegister, num, div);
    as_mls(dest, num, ScratchRegister, div);
}

// Division
void
MacroAssemblerARM::ma_sdiv(Register num, Register div, Register dest, Condition cond)
{
    as_sdiv(dest, num, div, cond);
}

void
MacroAssemblerARM::ma_udiv(Register num, Register div, Register dest, Condition cond)
{
    as_udiv(dest, num, div, cond);
}
// Miscelanous instructions
void
MacroAssemblerARM::ma_clz(Register src, Register dest, Condition cond)
{
    as_clz(dest, src, cond);
}
// Memory.
// Shortcut for when we know we're transferring 32 bits of data.
void
MacroAssemblerARM::ma_dtr(LoadStore ls, Register rn, Imm32 offset, Register rt,
                          Index mode, Assembler::Condition cc)
{
    ma_dataTransferN(ls, 32, true, rn, offset, rt, mode, cc);
}

void
MacroAssemblerARM::ma_dtr(LoadStore ls, Register rn, Register rm, Register rt,
                          Index mode, Assembler::Condition cc)
{
    MOZ_CRASH("Feature NYI");
}

void
MacroAssemblerARM::ma_str(Register rt, DTRAddr addr, Index mode, Condition cc)
{
    as_dtr(IsStore, 32, mode, rt, addr, cc);
}

void
MacroAssemblerARM::ma_dtr(LoadStore ls, Register rt, const Operand& addr, Index mode, Condition cc)
{
    ma_dataTransferN(ls, 32, true,
                     Register::FromCode(addr.base()), Imm32(addr.disp()),
                     rt, mode, cc);
}

void
MacroAssemblerARM::ma_str(Register rt, const Operand& addr, Index mode, Condition cc)
{
    ma_dtr(IsStore, rt, addr, mode, cc);
}
void
MacroAssemblerARM::ma_strd(Register rt, DebugOnly<Register> rt2, EDtrAddr addr, Index mode, Condition cc)
{
    MOZ_ASSERT((rt.code() & 1) == 0);
    MOZ_ASSERT(rt2.value.code() == rt.code() + 1);
    as_extdtr(IsStore, 64, true, mode, rt, addr, cc);
}

void
MacroAssemblerARM::ma_ldr(DTRAddr addr, Register rt, Index mode, Condition cc)
{
    as_dtr(IsLoad, 32, mode, rt, addr, cc);
}
void
MacroAssemblerARM::ma_ldr(const Operand& addr, Register rt, Index mode, Condition cc)
{
    ma_dtr(IsLoad, rt, addr, mode, cc);
}

void
MacroAssemblerARM::ma_ldrb(DTRAddr addr, Register rt, Index mode, Condition cc)
{
    as_dtr(IsLoad, 8, mode, rt, addr, cc);
}

void
MacroAssemblerARM::ma_ldrsh(EDtrAddr addr, Register rt, Index mode, Condition cc)
{
    as_extdtr(IsLoad, 16, true, mode, rt, addr, cc);
}

void
MacroAssemblerARM::ma_ldrh(EDtrAddr addr, Register rt, Index mode, Condition cc)
{
    as_extdtr(IsLoad, 16, false, mode, rt, addr, cc);
}
void
MacroAssemblerARM::ma_ldrsb(EDtrAddr addr, Register rt, Index mode, Condition cc)
{
    as_extdtr(IsLoad, 8, true, mode, rt, addr, cc);
}
void
MacroAssemblerARM::ma_ldrd(EDtrAddr addr, Register rt, DebugOnly<Register> rt2,
                           Index mode, Condition cc)
{
    MOZ_ASSERT((rt.code() & 1) == 0);
    MOZ_ASSERT(rt2.value.code() == rt.code() + 1);
    as_extdtr(IsLoad, 64, true, mode, rt, addr, cc);
}
void
MacroAssemblerARM::ma_strh(Register rt, EDtrAddr addr, Index mode, Condition cc)
{
    as_extdtr(IsStore, 16, false, mode, rt, addr, cc);
}

void
MacroAssemblerARM::ma_strb(Register rt, DTRAddr addr, Index mode, Condition cc)
{
    as_dtr(IsStore, 8, mode, rt, addr, cc);
}

// Specialty for moving N bits of data, where n == 8,16,32,64.
BufferOffset
MacroAssemblerARM::ma_dataTransferN(LoadStore ls, int size, bool IsSigned,
                          Register rn, Register rm, Register rt,
                                    Index mode, Assembler::Condition cc, unsigned shiftAmount)
{
    if (size == 32 || (size == 8 && !IsSigned)) {
        return as_dtr(ls, size, mode, rt, DTRAddr(rn, DtrRegImmShift(rm, LSL, shiftAmount)), cc);
    } else {
        if (shiftAmount != 0) {
            MOZ_ASSERT(rn != ScratchRegister);
            MOZ_ASSERT(rt != ScratchRegister);
            ma_lsl(Imm32(shiftAmount), rm, ScratchRegister);
            rm = ScratchRegister;
        }
        return as_extdtr(ls, size, IsSigned, mode, rt, EDtrAddr(rn, EDtrOffReg(rm)), cc);
    }
}

BufferOffset
MacroAssemblerARM::ma_dataTransferN(LoadStore ls, int size, bool IsSigned,
                                    Register rn, Imm32 offset, Register rt,
                                    Index mode, Assembler::Condition cc)
{
    int off = offset.value;
    // We can encode this as a standard ldr.
    if (size == 32 || (size == 8 && !IsSigned) ) {
        if (off < 4096 && off > -4096) {
            // This encodes as a single instruction, Emulating mode's behavior
            // in a multi-instruction sequence is not necessary.
            return as_dtr(ls, size, mode, rt, DTRAddr(rn, DtrOffImm(off)), cc);
        }

        // We cannot encode this offset in a a single ldr. For mode == index,
        // try to encode it as |add scratch, base, imm; ldr dest, [scratch, +offset]|.
        // This does not wark for mode == PreIndex or mode == PostIndex.
        // PreIndex is simple, just do the add into the base register first,
        // then do a PreIndex'ed load. PostIndexed loads can be tricky.
        // Normally, doing the load with an index of 0, then doing an add would
        // work, but if the destination is the PC, you don't get to execute the
        // instruction after the branch, which will lead to the base register
        // not being updated correctly. Explicitly handle this case, without
        // doing anything fancy, then handle all of the other cases.

        // mode == Offset
        //  add   scratch, base, offset_hi
        //  ldr   dest, [scratch, +offset_lo]
        //
        // mode == PreIndex
        //  add   base, base, offset_hi
        //  ldr   dest, [base, +offset_lo]!
        //
        // mode == PostIndex, dest == pc
        //  ldr   scratch, [base]
        //  add   base, base, offset_hi
        //  add   base, base, offset_lo
        //  mov   dest, scratch
        // PostIndex with the pc as the destination needs to be handled
        // specially, since in the code below, the write into 'dest' is going to
        // alter the control flow, so the following instruction would never get
        // emitted.
        //
        // mode == PostIndex, dest != pc
        //  ldr   dest, [base], offset_lo
        //  add   base, base, offset_hi

        if (rt == pc && mode == PostIndex && ls == IsLoad) {
            ma_mov(rn, ScratchRegister);
            ma_alu(rn, offset, rn, OpAdd);
            return as_dtr(IsLoad, size, Offset, pc, DTRAddr(ScratchRegister, DtrOffImm(0)), cc);
        }

        int bottom = off & 0xfff;
        int neg_bottom = 0x1000 - bottom;
        // For a regular offset, base == ScratchRegister does what we want.
        // Modify the scratch register, leaving the actual base unscathed.
        Register base = ScratchRegister;
        // For the preindex case, we want to just re-use rn as the base
        // register, so when the base register is updated *before* the load, rn
        // is updated.
        if (mode == PreIndex)
            base = rn;
        MOZ_ASSERT(mode != PostIndex);
        // At this point, both off - bottom and off + neg_bottom will be
        // reasonable-ish quantities.
        //
        // Note a neg_bottom of 0x1000 can not be encoded as an immediate
        // negative offset in the instruction and this occurs when bottom is
        // zero, so this case is guarded against below.
        if (off < 0) {
            Operand2 sub_off = Imm8(-(off - bottom)); // sub_off = bottom - off
            if (!sub_off.invalid) {
                // - sub_off = off - bottom
                as_sub(ScratchRegister, rn, sub_off, NoSetCond, cc);
                return as_dtr(ls, size, Offset, rt, DTRAddr(ScratchRegister, DtrOffImm(bottom)), cc);
            }
            // sub_off = -neg_bottom - off
            sub_off = Imm8(-(off + neg_bottom));
            if (!sub_off.invalid && bottom != 0) {
                // Guarded against by: bottom != 0
                MOZ_ASSERT(neg_bottom < 0x1000);
                // - sub_off = neg_bottom + off
                as_sub(ScratchRegister, rn, sub_off, NoSetCond, cc);
                return as_dtr(ls, size, Offset, rt, DTRAddr(ScratchRegister, DtrOffImm(-neg_bottom)), cc);
            }
        } else {
            // sub_off = off - bottom
            Operand2 sub_off = Imm8(off - bottom);
            if (!sub_off.invalid) {
                //  sub_off = off - bottom
                as_add(ScratchRegister, rn, sub_off, NoSetCond, cc);
                return as_dtr(ls, size, Offset, rt, DTRAddr(ScratchRegister, DtrOffImm(bottom)), cc);
            }
            // sub_off = neg_bottom + off
            sub_off = Imm8(off + neg_bottom);
            if (!sub_off.invalid && bottom != 0) {
                // Guarded against by: bottom != 0
                MOZ_ASSERT(neg_bottom < 0x1000);
                // sub_off = neg_bottom + off
                as_add(ScratchRegister, rn, sub_off, NoSetCond,  cc);
                return as_dtr(ls, size, Offset, rt, DTRAddr(ScratchRegister, DtrOffImm(-neg_bottom)), cc);
            }
        }
        ma_mov(offset, ScratchRegister);
        return as_dtr(ls, size, mode, rt, DTRAddr(rn, DtrRegImmShift(ScratchRegister, LSL, 0)));
    } else {
        // Should attempt to use the extended load/store instructions.
        if (off < 256 && off > -256)
            return as_extdtr(ls, size, IsSigned, mode, rt, EDtrAddr(rn, EDtrOffImm(off)), cc);

        // We cannot encode this offset in a single extldr. Try to encode it as
        // an add scratch, base, imm; extldr dest, [scratch, +offset].
        int bottom = off & 0xff;
        int neg_bottom = 0x100 - bottom;
        // At this point, both off - bottom and off + neg_bottom will be
        // reasonable-ish quantities.
        //
        // Note a neg_bottom of 0x100 can not be encoded as an immediate
        // negative offset in the instruction and this occurs when bottom is
        // zero, so this case is guarded against below.
        if (off < 0) {
            // sub_off = bottom - off
            Operand2 sub_off = Imm8(-(off - bottom));
            if (!sub_off.invalid) {
                // - sub_off = off - bottom
                as_sub(ScratchRegister, rn, sub_off, NoSetCond, cc);
                return as_extdtr(ls, size, IsSigned, Offset, rt,
                                 EDtrAddr(ScratchRegister, EDtrOffImm(bottom)),
                                 cc);
            }
            // sub_off = -neg_bottom - off
            sub_off = Imm8(-(off + neg_bottom));
            if (!sub_off.invalid && bottom != 0) {
                // Guarded against by: bottom != 0
                MOZ_ASSERT(neg_bottom < 0x100);
                // - sub_off = neg_bottom + off
                as_sub(ScratchRegister, rn, sub_off, NoSetCond, cc);
                return as_extdtr(ls, size, IsSigned, Offset, rt,
                                 EDtrAddr(ScratchRegister, EDtrOffImm(-neg_bottom)),
                                 cc);
            }
        } else {
            // sub_off = off - bottom
            Operand2 sub_off = Imm8(off - bottom);
            if (!sub_off.invalid) {
                // sub_off = off - bottom
                as_add(ScratchRegister, rn, sub_off, NoSetCond, cc);
                return as_extdtr(ls, size, IsSigned, Offset, rt,
                                 EDtrAddr(ScratchRegister, EDtrOffImm(bottom)),
                                 cc);
            }
            // sub_off = neg_bottom + off
            sub_off = Imm8(off + neg_bottom);
            if (!sub_off.invalid && bottom != 0) {
                // Guarded against by: bottom != 0
                MOZ_ASSERT(neg_bottom < 0x100);
                // sub_off = neg_bottom + off
                as_add(ScratchRegister, rn, sub_off, NoSetCond,  cc);
                return as_extdtr(ls, size, IsSigned, Offset, rt,
                                 EDtrAddr(ScratchRegister, EDtrOffImm(-neg_bottom)),
                                 cc);
            }
        }
        ma_mov(offset, ScratchRegister);
        return as_extdtr(ls, size, IsSigned, mode, rt, EDtrAddr(rn, EDtrOffReg(ScratchRegister)), cc);
    }
}

void
MacroAssemblerARM::ma_pop(Register r)
{
    ma_dtr(IsLoad, sp, Imm32(4), r, PostIndex);
}
void
MacroAssemblerARM::ma_push(Register r)
{
    // Pushing sp is not well defined: use two instructions.
    if (r == sp) {
        ma_mov(sp, ScratchRegister);
        r = ScratchRegister;
    }
    ma_dtr(IsStore, sp,Imm32(-4), r, PreIndex);
}

void
MacroAssemblerARM::ma_vpop(VFPRegister r)
{
    startFloatTransferM(IsLoad, sp, IA, WriteBack);
    transferFloatReg(r);
    finishFloatTransfer();
}
void
MacroAssemblerARM::ma_vpush(VFPRegister r)
{
    startFloatTransferM(IsStore, sp, DB, WriteBack);
    transferFloatReg(r);
    finishFloatTransfer();
}

// Barriers
void
MacroAssemblerARM::ma_dmb(BarrierOption option)
{
    if (HasDMBDSBISB())
        as_dmb(option);
    else
        as_dmb_trap();
}
void
MacroAssemblerARM::ma_dsb(BarrierOption option)
{
    if (HasDMBDSBISB())
        as_dsb(option);
    else
        as_dsb_trap();
}

// Branches when done from within arm-specific code.
BufferOffset
MacroAssemblerARM::ma_b(Label* dest, Assembler::Condition c)
{
    return as_b(dest, c);
}

void
MacroAssemblerARM::ma_bx(Register dest, Assembler::Condition c)
{
    as_bx(dest, c);
}

static Assembler::RelocBranchStyle
b_type()
{
    return Assembler::B_LDR;
}
void
MacroAssemblerARM::ma_b(void* target, Relocation::Kind reloc, Assembler::Condition c)
{
    // We know the absolute address of the target, but not our final location
    // (with relocating GC, we *can't* know our final location) for now, I'm
    // going to be conservative, and load this with an absolute address
    uint32_t trg = (uint32_t)target;
    switch (b_type()) {
      case Assembler::B_MOVWT:
        as_movw(ScratchRegister, Imm16(trg & 0xffff), c);
        as_movt(ScratchRegister, Imm16(trg >> 16), c);
        // This is going to get the branch predictor pissed off.
        as_bx(ScratchRegister, c);
        break;
      case Assembler::B_LDR_BX:
        as_Imm32Pool(ScratchRegister, trg, c);
        as_bx(ScratchRegister, c);
        break;
      case Assembler::B_LDR:
        as_Imm32Pool(pc, trg, c);
        break;
      default:
        MOZ_CRASH("Other methods of generating tracable jumps NYI");
    }
}

// This is almost NEVER necessary: we'll basically never be calling a label,
// except possibly in the crazy bailout-table case.
void
MacroAssemblerARM::ma_bl(Label* dest, Assembler::Condition c)
{
    as_bl(dest, c);
}

void
MacroAssemblerARM::ma_blx(Register reg, Assembler::Condition c)
{
    as_blx(reg, c);
}

// VFP/ALU
void
MacroAssemblerARM::ma_vadd(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vadd(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void
MacroAssemblerARM::ma_vadd_f32(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vadd(VFPRegister(dst).singleOverlay(), VFPRegister(src1).singleOverlay(),
            VFPRegister(src2).singleOverlay());
}

void
MacroAssemblerARM::ma_vsub(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vsub(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void
MacroAssemblerARM::ma_vsub_f32(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vsub(VFPRegister(dst).singleOverlay(), VFPRegister(src1).singleOverlay(),
            VFPRegister(src2).singleOverlay());
}

void
MacroAssemblerARM::ma_vmul(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vmul(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void
MacroAssemblerARM::ma_vmul_f32(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vmul(VFPRegister(dst).singleOverlay(), VFPRegister(src1).singleOverlay(),
            VFPRegister(src2).singleOverlay());
}

void
MacroAssemblerARM::ma_vdiv(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vdiv(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void
MacroAssemblerARM::ma_vdiv_f32(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vdiv(VFPRegister(dst).singleOverlay(), VFPRegister(src1).singleOverlay(),
            VFPRegister(src2).singleOverlay());
}

void
MacroAssemblerARM::ma_vmov(FloatRegister src, FloatRegister dest, Condition cc)
{
    as_vmov(dest, src, cc);
}

void
MacroAssemblerARM::ma_vmov_f32(FloatRegister src, FloatRegister dest, Condition cc)
{
    as_vmov(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay(), cc);
}

void
MacroAssemblerARM::ma_vneg(FloatRegister src, FloatRegister dest, Condition cc)
{
    as_vneg(dest, src, cc);
}

void
MacroAssemblerARM::ma_vneg_f32(FloatRegister src, FloatRegister dest, Condition cc)
{
    as_vneg(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay(), cc);
}

void
MacroAssemblerARM::ma_vabs(FloatRegister src, FloatRegister dest, Condition cc)
{
    as_vabs(dest, src, cc);
}

void
MacroAssemblerARM::ma_vabs_f32(FloatRegister src, FloatRegister dest, Condition cc)
{
    as_vabs(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay(), cc);
}

void
MacroAssemblerARM::ma_vsqrt(FloatRegister src, FloatRegister dest, Condition cc)
{
    as_vsqrt(dest, src, cc);
}

void
MacroAssemblerARM::ma_vsqrt_f32(FloatRegister src, FloatRegister dest, Condition cc)
{
    as_vsqrt(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay(), cc);
}

static inline uint32_t
DoubleHighWord(const double value)
{
    return static_cast<uint32_t>(BitwiseCast<uint64_t>(value) >> 32);
}

static inline uint32_t
DoubleLowWord(const double value)
{
    return BitwiseCast<uint64_t>(value) & uint32_t(0xffffffff);
}

void
MacroAssemblerARM::ma_vimm(double value, FloatRegister dest, Condition cc)
{
    if (HasVFPv3()) {
        if (DoubleLowWord(value) == 0) {
            if (DoubleHighWord(value) == 0) {
                // To zero a register, load 1.0, then execute dN <- dN - dN
                as_vimm(dest, VFPImm::One, cc);
                as_vsub(dest, dest, dest, cc);
                return;
            }

            VFPImm enc(DoubleHighWord(value));
            if (enc.isValid()) {
                as_vimm(dest, enc, cc);
                return;
            }

        }
    }
    // Fall back to putting the value in a pool.
    as_FImm64Pool(dest, value, cc);
}

static inline uint32_t
Float32Word(const float value)
{
    return BitwiseCast<uint32_t>(value);
}

void
MacroAssemblerARM::ma_vimm_f32(float value, FloatRegister dest, Condition cc)
{
    VFPRegister vd = VFPRegister(dest).singleOverlay();
    if (HasVFPv3()) {
        if (Float32Word(value) == 0) {
            // To zero a register, load 1.0, then execute sN <- sN - sN.
            as_vimm(vd, VFPImm::One, cc);
            as_vsub(vd, vd, vd, cc);
            return;
        }

        // Note that the vimm immediate float32 instruction encoding differs
        // from the vimm immediate double encoding, but this difference matches
        // the difference in the floating point formats, so it is possible to
        // convert the float32 to a double and then use the double encoding
        // paths. It is still necessary to firstly check that the double low
        // word is zero because some float32 numbers set these bits and this can
        // not be ignored.
        double doubleValue = value;
        if (DoubleLowWord(value) == 0) {
            VFPImm enc(DoubleHighWord(doubleValue));
            if (enc.isValid()) {
                as_vimm(vd, enc, cc);
                return;
            }
        }
    }
    // Fall back to putting the value in a pool.
    as_FImm32Pool(vd, value, cc);
}

void
MacroAssemblerARM::ma_vcmp(FloatRegister src1, FloatRegister src2, Condition cc)
{
    as_vcmp(VFPRegister(src1), VFPRegister(src2), cc);
}
void
MacroAssemblerARM::ma_vcmp_f32(FloatRegister src1, FloatRegister src2, Condition cc)
{
    as_vcmp(VFPRegister(src1).singleOverlay(), VFPRegister(src2).singleOverlay(), cc);
}
void
MacroAssemblerARM::ma_vcmpz(FloatRegister src1, Condition cc)
{
    as_vcmpz(VFPRegister(src1), cc);
}
void
MacroAssemblerARM::ma_vcmpz_f32(FloatRegister src1, Condition cc)
{
    as_vcmpz(VFPRegister(src1).singleOverlay(), cc);
}

void
MacroAssemblerARM::ma_vcvt_F64_I32(FloatRegister src, FloatRegister dest, Condition cc)
{
    MOZ_ASSERT(src.isDouble());
    MOZ_ASSERT(dest.isSInt());
    as_vcvt(dest, src, false, cc);
}
void
MacroAssemblerARM::ma_vcvt_F64_U32(FloatRegister src, FloatRegister dest, Condition cc)
{
    MOZ_ASSERT(src.isDouble());
    MOZ_ASSERT(dest.isUInt());
    as_vcvt(dest, src, false, cc);
}
void
MacroAssemblerARM::ma_vcvt_I32_F64(FloatRegister src, FloatRegister dest, Condition cc)
{
    MOZ_ASSERT(src.isSInt());
    MOZ_ASSERT(dest.isDouble());
    as_vcvt(dest, src, false, cc);
}
void
MacroAssemblerARM::ma_vcvt_U32_F64(FloatRegister src, FloatRegister dest, Condition cc)
{
    MOZ_ASSERT(src.isUInt());
    MOZ_ASSERT(dest.isDouble());
    as_vcvt(dest, src, false, cc);
}

void
MacroAssemblerARM::ma_vcvt_F32_I32(FloatRegister src, FloatRegister dest, Condition cc)
{
    MOZ_ASSERT(src.isSingle());
    MOZ_ASSERT(dest.isSInt());
    as_vcvt(VFPRegister(dest).sintOverlay(), VFPRegister(src).singleOverlay(), false, cc);
}
void
MacroAssemblerARM::ma_vcvt_F32_U32(FloatRegister src, FloatRegister dest, Condition cc)
{
    MOZ_ASSERT(src.isSingle());
    MOZ_ASSERT(dest.isUInt());
    as_vcvt(VFPRegister(dest).uintOverlay(), VFPRegister(src).singleOverlay(), false, cc);
}
void
MacroAssemblerARM::ma_vcvt_I32_F32(FloatRegister src, FloatRegister dest, Condition cc)
{
    MOZ_ASSERT(src.isSInt());
    MOZ_ASSERT(dest.isSingle());
    as_vcvt(VFPRegister(dest).singleOverlay(), VFPRegister(src).sintOverlay(), false, cc);
}
void
MacroAssemblerARM::ma_vcvt_U32_F32(FloatRegister src, FloatRegister dest, Condition cc)
{
    MOZ_ASSERT(src.isUInt());
    MOZ_ASSERT(dest.isSingle());
    as_vcvt(VFPRegister(dest).singleOverlay(), VFPRegister(src).uintOverlay(), false, cc);
}

void
MacroAssemblerARM::ma_vxfer(FloatRegister src, Register dest, Condition cc)
{
    as_vxfer(dest, InvalidReg, VFPRegister(src).singleOverlay(), FloatToCore, cc);
}

void
MacroAssemblerARM::ma_vxfer(FloatRegister src, Register dest1, Register dest2, Condition cc)
{
    as_vxfer(dest1, dest2, VFPRegister(src), FloatToCore, cc);
}

void
MacroAssemblerARM::ma_vxfer(Register src1, Register src2, FloatRegister dest, Condition cc)
{
    as_vxfer(src1, src2, VFPRegister(dest), CoreToFloat, cc);
}

BufferOffset
MacroAssemblerARM::ma_vdtr(LoadStore ls, const Operand& addr, VFPRegister rt, Condition cc)
{
    int off = addr.disp();
    MOZ_ASSERT((off & 3) == 0);
    Register base = Register::FromCode(addr.base());
    if (off > -1024 && off < 1024)
        return as_vdtr(ls, rt, addr.toVFPAddr(), cc);

    // We cannot encode this offset in a a single ldr. Try to encode it as an
    // add scratch, base, imm; ldr dest, [scratch, +offset].
    int bottom = off & (0xff << 2);
    int neg_bottom = (0x100 << 2) - bottom;
    // At this point, both off - bottom and off + neg_bottom will be
    // reasonable-ish quantities.
    //
    // Note a neg_bottom of 0x400 can not be encoded as an immediate negative
    // offset in the instruction and this occurs when bottom is zero, so this
    // case is guarded against below.
    if (off < 0) {
        // sub_off = bottom - off
        Operand2 sub_off = Imm8(-(off - bottom));
        if (!sub_off.invalid) {
            // - sub_off = off - bottom
            as_sub(ScratchRegister, base, sub_off, NoSetCond, cc);
            return as_vdtr(ls, rt, VFPAddr(ScratchRegister, VFPOffImm(bottom)), cc);
        }
        // sub_off = -neg_bottom - off
        sub_off = Imm8(-(off + neg_bottom));
        if (!sub_off.invalid && bottom != 0) {
            // Guarded against by: bottom != 0
            MOZ_ASSERT(neg_bottom < 0x400);
            // - sub_off = neg_bottom + off
            as_sub(ScratchRegister, base, sub_off, NoSetCond, cc);
            return as_vdtr(ls, rt, VFPAddr(ScratchRegister, VFPOffImm(-neg_bottom)), cc);
        }
    } else {
        // sub_off = off - bottom
        Operand2 sub_off = Imm8(off - bottom);
        if (!sub_off.invalid) {
            // sub_off = off - bottom
            as_add(ScratchRegister, base, sub_off, NoSetCond, cc);
            return as_vdtr(ls, rt, VFPAddr(ScratchRegister, VFPOffImm(bottom)), cc);
        }
        // sub_off = neg_bottom + off
        sub_off = Imm8(off + neg_bottom);
        if (!sub_off.invalid && bottom != 0) {
            // Guarded against by: bottom != 0
            MOZ_ASSERT(neg_bottom < 0x400);
            // sub_off = neg_bottom + off
            as_add(ScratchRegister, base, sub_off, NoSetCond, cc);
            return as_vdtr(ls, rt, VFPAddr(ScratchRegister, VFPOffImm(-neg_bottom)), cc);
        }
    }
    ma_add(base, Imm32(off), ScratchRegister, NoSetCond, cc);
    return as_vdtr(ls, rt, VFPAddr(ScratchRegister, VFPOffImm(0)), cc);
}

BufferOffset
MacroAssemblerARM::ma_vldr(VFPAddr addr, VFPRegister dest, Condition cc)
{
    return as_vdtr(IsLoad, dest, addr, cc);
}
BufferOffset
MacroAssemblerARM::ma_vldr(const Operand& addr, VFPRegister dest, Condition cc)
{
    return ma_vdtr(IsLoad, addr, dest, cc);
}
BufferOffset
MacroAssemblerARM::ma_vldr(VFPRegister src, Register base, Register index, int32_t shift, Condition cc)
{
    as_add(ScratchRegister, base, lsl(index, shift), NoSetCond, cc);
    return ma_vldr(Operand(ScratchRegister, 0), src, cc);
}

BufferOffset
MacroAssemblerARM::ma_vstr(VFPRegister src, VFPAddr addr, Condition cc)
{
    return as_vdtr(IsStore, src, addr, cc);
}

BufferOffset
MacroAssemblerARM::ma_vstr(VFPRegister src, const Operand& addr, Condition cc)
{
    return ma_vdtr(IsStore, addr, src, cc);
}
BufferOffset
MacroAssemblerARM::ma_vstr(VFPRegister src, Register base, Register index, int32_t shift,
                           int32_t offset, Condition cc)
{
    as_add(ScratchRegister, base, lsl(index, shift), NoSetCond, cc);
    return ma_vstr(src, Operand(ScratchRegister, offset), cc);
}

void
MacroAssemblerARMCompat::buildFakeExitFrame(Register scratch, uint32_t* offset)
{
    DebugOnly<uint32_t> initialDepth = framePushed();
    uint32_t descriptor = MakeFrameDescriptor(framePushed(), JitFrame_IonJS);

    Push(Imm32(descriptor)); // descriptor_

    enterNoPool(2);
    DebugOnly<uint32_t> offsetBeforePush = currentOffset();
    Push(pc); // actually pushes $pc + 8.

    // Consume an additional 4 bytes. The start of the next instruction will
    // then be 8 bytes after the instruction for Push(pc); this offset can
    // therefore be fed to the safepoint.
    ma_nop();
    uint32_t pseudoReturnOffset = currentOffset();
    leaveNoPool();

    MOZ_ASSERT(framePushed() == initialDepth + ExitFrameLayout::Size());
    MOZ_ASSERT(pseudoReturnOffset - offsetBeforePush == 8);

    *offset = pseudoReturnOffset;
}

bool
MacroAssemblerARMCompat::buildOOLFakeExitFrame(void* fakeReturnAddr)
{
    DebugOnly<uint32_t> initialDepth = framePushed();
    uint32_t descriptor = MakeFrameDescriptor(framePushed(), JitFrame_IonJS);

    Push(Imm32(descriptor)); // descriptor_
    Push(ImmPtr(fakeReturnAddr));

    return true;
}

void
MacroAssemblerARMCompat::callWithExitFrame(Label* target)
{
    uint32_t descriptor = MakeFrameDescriptor(framePushed(), JitFrame_IonJS);
    Push(Imm32(descriptor)); // descriptor

    ma_callJitHalfPush(target);
}

void
MacroAssemblerARMCompat::callWithExitFrame(JitCode* target)
{
    uint32_t descriptor = MakeFrameDescriptor(framePushed(), JitFrame_IonJS);
    Push(Imm32(descriptor)); // descriptor

    addPendingJump(m_buffer.nextOffset(), ImmPtr(target->raw()), Relocation::JITCODE);
    RelocStyle rs;
    if (HasMOVWT())
        rs = L_MOVWT;
    else
        rs = L_LDR;

    ma_movPatchable(ImmPtr(target->raw()), ScratchRegister, Always, rs);
    ma_callJitHalfPush(ScratchRegister);
}

void
MacroAssemblerARMCompat::callWithExitFrame(JitCode* target, Register dynStack)
{
    ma_add(Imm32(framePushed()), dynStack);
    makeFrameDescriptor(dynStack, JitFrame_IonJS);
    Push(dynStack); // descriptor

    addPendingJump(m_buffer.nextOffset(), ImmPtr(target->raw()), Relocation::JITCODE);
    RelocStyle rs;
    if (HasMOVWT())
        rs = L_MOVWT;
    else
        rs = L_LDR;

    ma_movPatchable(ImmPtr(target->raw()), ScratchRegister, Always, rs);
    ma_callJitHalfPush(ScratchRegister);
}

void
MacroAssemblerARMCompat::callJit(Register callee)
{
    MOZ_ASSERT((framePushed() & 3) == 0);
    if ((framePushed() & 7) == 4) {
        ma_callJitHalfPush(callee);
    } else {
        adjustFrame(sizeof(void*));
        ma_callJit(callee);
    }
}

void
MacroAssembler::alignFrameForICArguments(AfterICSaveLive& aic)
{
    // Exists for MIPS compatibility.
}

void
MacroAssembler::restoreFrameAlignmentForICArguments(AfterICSaveLive& aic)
{
    // Exists for MIPS compatibility.
}

void
MacroAssemblerARMCompat::reserveStack(uint32_t amount)
{
    if (amount)
        ma_sub(Imm32(amount), sp);
    adjustFrame(amount);
}
void
MacroAssemblerARMCompat::freeStack(uint32_t amount)
{
    MOZ_ASSERT(amount <= framePushed_);
    if (amount)
        ma_add(Imm32(amount), sp);
    adjustFrame(-amount);
}
void
MacroAssemblerARMCompat::freeStack(Register amount)
{
    ma_add(amount, sp);
}

void
MacroAssembler::PushRegsInMask(RegisterSet set, FloatRegisterSet simdSet)
{
    MOZ_ASSERT(!SupportsSimd() && simdSet.size() == 0);
    int32_t diffF = set.fpus().getPushSizeInBytes();
    int32_t diffG = set.gprs().size() * sizeof(intptr_t);

    if (set.gprs().size() > 1) {
        adjustFrame(diffG);
        startDataTransferM(IsStore, StackPointer, DB, WriteBack);
        for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); iter++) {
            diffG -= sizeof(intptr_t);
            transferReg(*iter);
        }
        finishDataTransfer();
    } else {
        reserveStack(diffG);
        for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); iter++) {
            diffG -= sizeof(intptr_t);
            storePtr(*iter, Address(StackPointer, diffG));
        }
    }
    MOZ_ASSERT(diffG == 0);

    adjustFrame(diffF);
    diffF += transferMultipleByRuns(set.fpus(), IsStore, StackPointer, DB);
    MOZ_ASSERT(diffF == 0);
}

void
MacroAssembler::PopRegsInMaskIgnore(RegisterSet set, RegisterSet ignore, FloatRegisterSet simdSet)
{
    MOZ_ASSERT(!SupportsSimd() && simdSet.size() == 0);
    int32_t diffG = set.gprs().size() * sizeof(intptr_t);
    int32_t diffF = set.fpus().getPushSizeInBytes();
    const int32_t reservedG = diffG;
    const int32_t reservedF = diffF;

    // ARM can load multiple registers at once, but only if we want back all
    // the registers we previously saved to the stack.
    if (ignore.empty(true)) {
        diffF -= transferMultipleByRuns(set.fpus(), IsLoad, StackPointer, IA);
        adjustFrame(-reservedF);
    } else {
        TypedRegisterSet<VFPRegister> fpset = set.fpus().reduceSetForPush();
        TypedRegisterSet<VFPRegister> fpignore = ignore.fpus().reduceSetForPush();
        for (FloatRegisterBackwardIterator iter(fpset); iter.more(); iter++) {
            diffF -= (*iter).size();
            if (!fpignore.has(*iter))
                loadDouble(Address(StackPointer, diffF), *iter);
        }
        freeStack(reservedF);
    }
    MOZ_ASSERT(diffF == 0);

    if (set.gprs().size() > 1 && ignore.empty(false)) {
        startDataTransferM(IsLoad, StackPointer, IA, WriteBack);
        for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); iter++) {
            diffG -= sizeof(intptr_t);
            transferReg(*iter);
        }
        finishDataTransfer();
        adjustFrame(-reservedG);
    } else {
        for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); iter++) {
            diffG -= sizeof(intptr_t);
            if (!ignore.has(*iter))
                loadPtr(Address(StackPointer, diffG), *iter);
        }
        freeStack(reservedG);
    }
    MOZ_ASSERT(diffG == 0);
}

void
MacroAssemblerARMCompat::add32(Register src, Register dest)
{
    ma_add(src, dest, SetCond);
}

void
MacroAssemblerARMCompat::add32(Imm32 imm, Register dest)
{
    ma_add(imm, dest, SetCond);
}

void
MacroAssemblerARMCompat::xor32(Imm32 imm, Register dest)
{
    ma_eor(imm, dest, SetCond);
}

void
MacroAssemblerARMCompat::add32(Imm32 imm, const Address& dest)
{
    load32(dest, ScratchRegister);
    ma_add(imm, ScratchRegister, SetCond);
    store32(ScratchRegister, dest);
}

void
MacroAssemblerARMCompat::sub32(Imm32 imm, Register dest)
{
    ma_sub(imm, dest, SetCond);
}

void
MacroAssemblerARMCompat::sub32(Register src, Register dest)
{
    ma_sub(src, dest, SetCond);
}

void
MacroAssemblerARMCompat::and32(Register src, Register dest)
{
    ma_and(src, dest, SetCond);
}

void
MacroAssemblerARMCompat::and32(Imm32 imm, Register dest)
{
    ma_and(imm, dest, SetCond);
}

void
MacroAssemblerARMCompat::and32(const Address& src, Register dest)
{
    load32(src, ScratchRegister);
    ma_and(ScratchRegister, dest, SetCond);
}

void
MacroAssemblerARMCompat::addPtr(Register src, Register dest)
{
    ma_add(src, dest);
}

void
MacroAssemblerARMCompat::addPtr(const Address& src, Register dest)
{
    load32(src, ScratchRegister);
    ma_add(ScratchRegister, dest, SetCond);
}

void
MacroAssemblerARMCompat::not32(Register reg)
{
    ma_mvn(reg, reg);
}

void
MacroAssemblerARMCompat::and32(Imm32 imm, const Address& dest)
{
    load32(dest, ScratchRegister);
    ma_and(imm, ScratchRegister);
    store32(ScratchRegister, dest);
}

void
MacroAssemblerARMCompat::or32(Imm32 imm, const Address& dest)
{
    load32(dest, ScratchRegister);
    ma_orr(imm, ScratchRegister);
    store32(ScratchRegister, dest);
}

void
MacroAssemblerARMCompat::or32(Imm32 imm, Register dest)
{
    ma_orr(imm, dest);
}

void
MacroAssemblerARMCompat::xorPtr(Imm32 imm, Register dest)
{
    ma_eor(imm, dest);
}

void
MacroAssemblerARMCompat::xorPtr(Register src, Register dest)
{
    ma_eor(src, dest);
}

void
MacroAssemblerARMCompat::orPtr(Imm32 imm, Register dest)
{
    ma_orr(imm, dest);
}

void
MacroAssemblerARMCompat::orPtr(Register src, Register dest)
{
    ma_orr(src, dest);
}

void
MacroAssemblerARMCompat::andPtr(Imm32 imm, Register dest)
{
    ma_and(imm, dest);
}

void
MacroAssemblerARMCompat::andPtr(Register src, Register dest)
{
    ma_and(src, dest);
}

void
MacroAssemblerARMCompat::move32(Imm32 imm, Register dest)
{
    ma_mov(imm, dest);
}

void
MacroAssemblerARMCompat::move32(Register src, Register dest) {
    ma_mov(src, dest);
}

void
MacroAssemblerARMCompat::movePtr(Register src, Register dest)
{
    ma_mov(src, dest);
}
void
MacroAssemblerARMCompat::movePtr(ImmWord imm, Register dest)
{
    ma_mov(Imm32(imm.value), dest);
}
void
MacroAssemblerARMCompat::movePtr(ImmGCPtr imm, Register dest)
{
    ma_mov(imm, dest);
}
void
MacroAssemblerARMCompat::movePtr(ImmMaybeNurseryPtr imm, Register dest)
{
    movePtr(noteMaybeNurseryPtr(imm), dest);
}
void
MacroAssemblerARMCompat::movePtr(ImmPtr imm, Register dest)
{
    movePtr(ImmWord(uintptr_t(imm.value)), dest);
}
void
MacroAssemblerARMCompat::movePtr(AsmJSImmPtr imm, Register dest)
{
    RelocStyle rs;
    if (HasMOVWT())
        rs = L_MOVWT;
    else
        rs = L_LDR;

    append(AsmJSAbsoluteLink(CodeOffsetLabel(currentOffset()), imm.kind()));
    ma_movPatchable(Imm32(-1), dest, Always, rs);
}
void
MacroAssemblerARMCompat::load8ZeroExtend(const Address& address, Register dest)
{
    ma_dataTransferN(IsLoad, 8, false, address.base, Imm32(address.offset), dest);
}

void
MacroAssemblerARMCompat::load8ZeroExtend(const BaseIndex& src, Register dest)
{
    Register base = src.base;
    uint32_t scale = Imm32::ShiftOf(src.scale).value;

    if (src.offset != 0) {
        ma_mov(base, ScratchRegister);
        base = ScratchRegister;
        ma_add(base, Imm32(src.offset), base);
    }
    ma_ldrb(DTRAddr(base, DtrRegImmShift(src.index, LSL, scale)), dest);

}

void
MacroAssemblerARMCompat::load8SignExtend(const Address& address, Register dest)
{
    ma_dataTransferN(IsLoad, 8, true, address.base, Imm32(address.offset), dest);
}

void
MacroAssemblerARMCompat::load8SignExtend(const BaseIndex& src, Register dest)
{
    Register index = src.index;

    // ARMv7 does not have LSL on an index register with an extended load.
    if (src.scale != TimesOne) {
        ma_lsl(Imm32::ShiftOf(src.scale), index, ScratchRegister);
        index = ScratchRegister;
    }

    if (src.offset != 0) {
        if (index != ScratchRegister) {
            ma_mov(index, ScratchRegister);
            index = ScratchRegister;
        }
        ma_add(Imm32(src.offset), index);
    }
    ma_ldrsb(EDtrAddr(src.base, EDtrOffReg(index)), dest);
}

void
MacroAssemblerARMCompat::load16ZeroExtend(const Address& address, Register dest)
{
    ma_dataTransferN(IsLoad, 16, false, address.base, Imm32(address.offset), dest);
}

void
MacroAssemblerARMCompat::load16ZeroExtend(const BaseIndex& src, Register dest)
{
    Register index = src.index;

    // ARMv7 does not have LSL on an index register with an extended load.
    if (src.scale != TimesOne) {
        ma_lsl(Imm32::ShiftOf(src.scale), index, ScratchRegister);
        index = ScratchRegister;
    }

    if (src.offset != 0) {
        if (index != ScratchRegister) {
            ma_mov(index, ScratchRegister);
            index = ScratchRegister;
        }
        ma_add(Imm32(src.offset), index);
    }
    ma_ldrh(EDtrAddr(src.base, EDtrOffReg(index)), dest);
}

void
MacroAssemblerARMCompat::load16SignExtend(const Address& address, Register dest)
{
    ma_dataTransferN(IsLoad, 16, true, address.base, Imm32(address.offset), dest);
}

void
MacroAssemblerARMCompat::load16SignExtend(const BaseIndex& src, Register dest)
{
    Register index = src.index;

    // We don't have LSL on index register yet.
    if (src.scale != TimesOne) {
        ma_lsl(Imm32::ShiftOf(src.scale), index, ScratchRegister);
        index = ScratchRegister;
    }

    if (src.offset != 0) {
        if (index != ScratchRegister) {
            ma_mov(index, ScratchRegister);
            index = ScratchRegister;
        }
        ma_add(Imm32(src.offset), index);
    }
    ma_ldrsh(EDtrAddr(src.base, EDtrOffReg(index)), dest);
}

void
MacroAssemblerARMCompat::load32(const Address& address, Register dest)
{
    loadPtr(address, dest);
}

void
MacroAssemblerARMCompat::load32(const BaseIndex& address, Register dest)
{
    loadPtr(address, dest);
}

void
MacroAssemblerARMCompat::load32(AbsoluteAddress address, Register dest)
{
    loadPtr(address, dest);
}
void
MacroAssemblerARMCompat::loadPtr(const Address& address, Register dest)
{
    ma_ldr(Operand(address), dest);
}

void
MacroAssemblerARMCompat::loadPtr(const BaseIndex& src, Register dest)
{
    Register base = src.base;
    uint32_t scale = Imm32::ShiftOf(src.scale).value;

    if (src.offset != 0) {
        ma_mov(base, ScratchRegister);
        base = ScratchRegister;
        ma_add(Imm32(src.offset), base);
    }
    ma_ldr(DTRAddr(base, DtrRegImmShift(src.index, LSL, scale)), dest);
}
void
MacroAssemblerARMCompat::loadPtr(AbsoluteAddress address, Register dest)
{
    movePtr(ImmWord(uintptr_t(address.addr)), ScratchRegister);
    loadPtr(Address(ScratchRegister, 0x0), dest);
}
void
MacroAssemblerARMCompat::loadPtr(AsmJSAbsoluteAddress address, Register dest)
{
    movePtr(AsmJSImmPtr(address.kind()), ScratchRegister);
    loadPtr(Address(ScratchRegister, 0x0), dest);
}

Operand payloadOf(const Address& address) {
    return Operand(address.base, address.offset);
}
Operand tagOf(const Address& address) {
    return Operand(address.base, address.offset + 4);
}

void
MacroAssemblerARMCompat::loadPrivate(const Address& address, Register dest)
{
    ma_ldr(payloadOf(address), dest);
}

void
MacroAssemblerARMCompat::loadDouble(const Address& address, FloatRegister dest)
{
    ma_vldr(Operand(address), dest);
}

void
MacroAssemblerARMCompat::loadDouble(const BaseIndex& src, FloatRegister dest)
{
    // VFP instructions don't even support register Base + register Index modes,
    // so just add the index, then handle the offset like normal.
    Register base = src.base;
    Register index = src.index;
    uint32_t scale = Imm32::ShiftOf(src.scale).value;
    int32_t offset = src.offset;
    as_add(ScratchRegister, base, lsl(index, scale));

    ma_vldr(Operand(ScratchRegister, offset), dest);
}

void
MacroAssemblerARMCompat::loadFloatAsDouble(const Address& address, FloatRegister dest)
{
    VFPRegister rt = dest;
    ma_vldr(Operand(address), rt.singleOverlay());
    as_vcvt(rt, rt.singleOverlay());
}

void
MacroAssemblerARMCompat::loadFloatAsDouble(const BaseIndex& src, FloatRegister dest)
{
    // VFP instructions don't even support register Base + register Index modes,
    // so just add the index, then handle the offset like normal.
    Register base = src.base;
    Register index = src.index;
    uint32_t scale = Imm32::ShiftOf(src.scale).value;
    int32_t offset = src.offset;
    VFPRegister rt = dest;
    as_add(ScratchRegister, base, lsl(index, scale));

    ma_vldr(Operand(ScratchRegister, offset), rt.singleOverlay());
    as_vcvt(rt, rt.singleOverlay());
}

void
MacroAssemblerARMCompat::loadFloat32(const Address& address, FloatRegister dest)
{
    ma_vldr(Operand(address), VFPRegister(dest).singleOverlay());
}

void
MacroAssemblerARMCompat::loadFloat32(const BaseIndex& src, FloatRegister dest)
{
    // VFP instructions don't even support register Base + register Index modes,
    // so just add the index, then handle the offset like normal.
    Register base = src.base;
    Register index = src.index;
    uint32_t scale = Imm32::ShiftOf(src.scale).value;
    int32_t offset = src.offset;
    as_add(ScratchRegister, base, lsl(index, scale));

    ma_vldr(Operand(ScratchRegister, offset), VFPRegister(dest).singleOverlay());
}

void
MacroAssemblerARMCompat::store8(Imm32 imm, const Address& address)
{
    ma_mov(imm, secondScratchReg_);
    store8(secondScratchReg_, address);
}

void
MacroAssemblerARMCompat::store8(Register src, const Address& address)
{
    ma_dataTransferN(IsStore, 8, false, address.base, Imm32(address.offset), src);
}

void
MacroAssemblerARMCompat::store8(Imm32 imm, const BaseIndex& dest)
{
    ma_mov(imm, secondScratchReg_);
    store8(secondScratchReg_, dest);
}

void
MacroAssemblerARMCompat::store8(Register src, const BaseIndex& dest)
{
    Register base = dest.base;
    uint32_t scale = Imm32::ShiftOf(dest.scale).value;

    if (dest.offset != 0) {
        ma_add(base, Imm32(dest.offset), ScratchRegister);
        base = ScratchRegister;
    }
    ma_strb(src, DTRAddr(base, DtrRegImmShift(dest.index, LSL, scale)));
}

void
MacroAssemblerARMCompat::store16(Imm32 imm, const Address& address)
{
    ma_mov(imm, secondScratchReg_);
    store16(secondScratchReg_, address);
}

void
MacroAssemblerARMCompat::store16(Register src, const Address& address)
{
    ma_dataTransferN(IsStore, 16, false, address.base, Imm32(address.offset), src);
}

void
MacroAssemblerARMCompat::store16(Imm32 imm, const BaseIndex& dest)
{
    ma_mov(imm, secondScratchReg_);
    store16(secondScratchReg_, dest);
}
void
MacroAssemblerARMCompat::store16(Register src, const BaseIndex& address)
{
    Register index = address.index;

    // We don't have LSL on index register yet.
    if (address.scale != TimesOne) {
        ma_lsl(Imm32::ShiftOf(address.scale), index, ScratchRegister);
        index = ScratchRegister;
    }

    if (address.offset != 0) {
        ma_add(index, Imm32(address.offset), ScratchRegister);
        index = ScratchRegister;
    }
    ma_strh(src, EDtrAddr(address.base, EDtrOffReg(index)));
}
void
MacroAssemblerARMCompat::store32(Register src, AbsoluteAddress address)
{
    storePtr(src, address);
}

void
MacroAssemblerARMCompat::store32(Register src, const Address& address)
{
    storePtr(src, address);
}

void
MacroAssemblerARMCompat::store32(Imm32 src, const Address& address)
{
    move32(src, secondScratchReg_);
    storePtr(secondScratchReg_, address);
}

void
MacroAssemblerARMCompat::store32(Imm32 imm, const BaseIndex& dest)
{
    ma_mov(imm, secondScratchReg_);
    store32(secondScratchReg_, dest);
}

void
MacroAssemblerARMCompat::store32(Register src, const BaseIndex& dest)
{
    Register base = dest.base;
    uint32_t scale = Imm32::ShiftOf(dest.scale).value;

    if (dest.offset != 0) {
        ma_add(base, Imm32(dest.offset), ScratchRegister);
        base = ScratchRegister;
    }
    ma_str(src, DTRAddr(base, DtrRegImmShift(dest.index, LSL, scale)));
}

void
MacroAssemblerARMCompat::store32_NoSecondScratch(Imm32 src, const Address& address)
{
    move32(src, ScratchRegister);
    storePtr(ScratchRegister, address);
}

template <typename T>
void
MacroAssemblerARMCompat::storePtr(ImmWord imm, T address)
{
    movePtr(imm, ScratchRegister);
    storePtr(ScratchRegister, address);
}

template void MacroAssemblerARMCompat::storePtr<Address>(ImmWord imm, Address address);
template void MacroAssemblerARMCompat::storePtr<BaseIndex>(ImmWord imm, BaseIndex address);

template <typename T>
void
MacroAssemblerARMCompat::storePtr(ImmPtr imm, T address)
{
    storePtr(ImmWord(uintptr_t(imm.value)), address);
}

template void MacroAssemblerARMCompat::storePtr<Address>(ImmPtr imm, Address address);
template void MacroAssemblerARMCompat::storePtr<BaseIndex>(ImmPtr imm, BaseIndex address);

template <typename T>
void
MacroAssemblerARMCompat::storePtr(ImmGCPtr imm, T address)
{
    movePtr(imm, ScratchRegister);
    storePtr(ScratchRegister, address);
}

template void MacroAssemblerARMCompat::storePtr<Address>(ImmGCPtr imm, Address address);
template void MacroAssemblerARMCompat::storePtr<BaseIndex>(ImmGCPtr imm, BaseIndex address);

void
MacroAssemblerARMCompat::storePtr(Register src, const Address& address)
{
    ma_str(src, Operand(address));
}

void
MacroAssemblerARMCompat::storePtr(Register src, const BaseIndex& address)
{
    store32(src, address);
}

void
MacroAssemblerARMCompat::storePtr(Register src, AbsoluteAddress dest)
{
    movePtr(ImmWord(uintptr_t(dest.addr)), ScratchRegister);
    storePtr(src, Address(ScratchRegister, 0x0));
}

// Note: this function clobbers the input register.
void
MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output)
{
    MOZ_ASSERT(input != ScratchDoubleReg);
    ma_vimm(0.5, ScratchDoubleReg);
    if (HasVFPv3()) {
        Label notSplit;
        ma_vadd(input, ScratchDoubleReg, ScratchDoubleReg);
        // Convert the double into an unsigned fixed point value with 24 bits of
        // precision. The resulting number will look like 0xII.DDDDDD
        as_vcvtFixed(ScratchDoubleReg, false, 24, true);
        // Move the fixed point value into an integer register.
        as_vxfer(output, InvalidReg, ScratchFloat32Reg.uintOverlay(), FloatToCore);
        // See if this value *might* have been an exact integer after adding
        // 0.5. This tests the 1/2 through 1/16,777,216th places, but 0.5 needs
        // to be tested out to the 1/140,737,488,355,328th place.
        ma_tst(output, Imm32(0x00ffffff));
        // Convert to a uint8 by shifting out all of the fraction bits.
        ma_lsr(Imm32(24), output, output);
        // If any of the bottom 24 bits were non-zero, then we're good, since
        // this number can't be exactly XX.0
        ma_b(&notSplit, NonZero);
        as_vxfer(ScratchRegister, InvalidReg, input, FloatToCore);
        ma_cmp(ScratchRegister, Imm32(0));
        // If the lower 32 bits of the double were 0, then this was an exact number,
        // and it should be even.
        ma_bic(Imm32(1), output, NoSetCond, Zero);
        bind(&notSplit);
    } else {
        Label outOfRange;
        ma_vcmpz(input);
        // Do the add, in place so we can reference it later.
        ma_vadd(input, ScratchDoubleReg, input);
        // Do the conversion to an integer.
        as_vcvt(VFPRegister(ScratchDoubleReg).uintOverlay(), VFPRegister(input));
        // Copy the converted value out.
        as_vxfer(output, InvalidReg, ScratchDoubleReg, FloatToCore);
        as_vmrs(pc);
        ma_mov(Imm32(0), output, NoSetCond, Overflow);  // NaN => 0
        ma_b(&outOfRange, Overflow);  // NaN
        ma_cmp(output, Imm32(0xff));
        ma_mov(Imm32(0xff), output, NoSetCond, Above);
        ma_b(&outOfRange, Above);
        // Convert it back to see if we got the same value back.
        as_vcvt(ScratchDoubleReg, VFPRegister(ScratchDoubleReg).uintOverlay());
        // Do the check.
        as_vcmp(ScratchDoubleReg, input);
        as_vmrs(pc);
        ma_bic(Imm32(1), output, NoSetCond, Zero);
        bind(&outOfRange);
    }
}

void
MacroAssemblerARMCompat::cmp32(Register lhs, Imm32 rhs)
{
    MOZ_ASSERT(lhs != ScratchRegister);
    ma_cmp(lhs, rhs);
}

void
MacroAssemblerARMCompat::cmp32(const Operand& lhs, Register rhs)
{
    ma_cmp(lhs.toReg(), rhs);
}

void
MacroAssemblerARMCompat::cmp32(const Operand& lhs, Imm32 rhs)
{
    MOZ_ASSERT(lhs.toReg() != ScratchRegister);
    ma_cmp(lhs.toReg(), rhs);
}

void
MacroAssemblerARMCompat::cmp32(Register lhs, Register rhs)
{
    ma_cmp(lhs, rhs);
}

void
MacroAssemblerARMCompat::cmpPtr(Register lhs, ImmWord rhs)
{
    MOZ_ASSERT(lhs != ScratchRegister);
    ma_cmp(lhs, Imm32(rhs.value));
}

void
MacroAssemblerARMCompat::cmpPtr(Register lhs, ImmPtr rhs)
{
    return cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
}

void
MacroAssemblerARMCompat::cmpPtr(Register lhs, Register rhs)
{
    ma_cmp(lhs, rhs);
}

void
MacroAssemblerARMCompat::cmpPtr(Register lhs, ImmGCPtr rhs)
{
    ma_cmp(lhs, rhs);
}

void
MacroAssemblerARMCompat::cmpPtr(Register lhs, Imm32 rhs)
{
    ma_cmp(lhs, rhs);
}

void
MacroAssemblerARMCompat::cmpPtr(const Address& lhs, Register rhs)
{
    loadPtr(lhs, ScratchRegister);
    cmpPtr(ScratchRegister, rhs);
}

void
MacroAssemblerARMCompat::cmpPtr(const Address& lhs, ImmWord rhs)
{
    loadPtr(lhs, secondScratchReg_);
    ma_cmp(secondScratchReg_, Imm32(rhs.value));
}

void
MacroAssemblerARMCompat::cmpPtr(const Address& lhs, ImmPtr rhs)
{
    cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
}

void
MacroAssemblerARMCompat::cmpPtr(const Address& lhs, ImmGCPtr rhs)
{
    loadPtr(lhs, secondScratchReg_);
    ma_cmp(secondScratchReg_, rhs);
}

void
MacroAssemblerARMCompat::cmpPtr(const Address& lhs, Imm32 rhs)
{
    loadPtr(lhs, secondScratchReg_);
    ma_cmp(secondScratchReg_, rhs);
}

void
MacroAssemblerARMCompat::setStackArg(Register reg, uint32_t arg)
{
    ma_dataTransferN(IsStore, 32, true, sp, Imm32(arg * sizeof(intptr_t)), reg);

}

void
MacroAssemblerARMCompat::subPtr(Imm32 imm, const Register dest)
{
    ma_sub(imm, dest);
}

void
MacroAssemblerARMCompat::subPtr(const Address& addr, const Register dest)
{
    loadPtr(addr, ScratchRegister);
    ma_sub(ScratchRegister, dest);
}

void
MacroAssemblerARMCompat::subPtr(Register src, Register dest)
{
    ma_sub(src, dest);
}

void
MacroAssemblerARMCompat::subPtr(Register src, const Address& dest)
{
    loadPtr(dest, ScratchRegister);
    ma_sub(src, ScratchRegister);
    storePtr(ScratchRegister, dest);
}

void
MacroAssemblerARMCompat::addPtr(Imm32 imm, const Register dest)
{
    ma_add(imm, dest);
}

void
MacroAssemblerARMCompat::addPtr(Imm32 imm, const Address& dest)
{
    loadPtr(dest, ScratchRegister);
    addPtr(imm, ScratchRegister);
    storePtr(ScratchRegister, dest);
}

void
MacroAssemblerARMCompat::compareDouble(FloatRegister lhs, FloatRegister rhs)
{
    // Compare the doubles, setting vector status flags.
    if (rhs.isMissing())
        ma_vcmpz(lhs);
    else
        ma_vcmp(lhs, rhs);

    // Move vector status bits to normal status flags.
    as_vmrs(pc);
}

void
MacroAssemblerARMCompat::branchDouble(DoubleCondition cond, FloatRegister lhs,
                                      FloatRegister rhs, Label* label)
{
    compareDouble(lhs, rhs);

    if (cond == DoubleNotEqual) {
        // Force the unordered cases not to jump.
        Label unordered;
        ma_b(&unordered, VFP_Unordered);
        ma_b(label, VFP_NotEqualOrUnordered);
        bind(&unordered);
        return;
    }

    if (cond == DoubleEqualOrUnordered) {
        ma_b(label, VFP_Unordered);
        ma_b(label, VFP_Equal);
        return;
    }

    ma_b(label, ConditionFromDoubleCondition(cond));
}

void
MacroAssemblerARMCompat::compareFloat(FloatRegister lhs, FloatRegister rhs)
{
    // Compare the doubles, setting vector status flags.
    if (rhs.isMissing())
        as_vcmpz(VFPRegister(lhs).singleOverlay());
    else
        as_vcmp(VFPRegister(lhs).singleOverlay(), VFPRegister(rhs).singleOverlay());

    // Move vector status bits to normal status flags.
    as_vmrs(pc);
}

void
MacroAssemblerARMCompat::branchFloat(DoubleCondition cond, FloatRegister lhs,
                                     FloatRegister rhs, Label* label)
{
    compareFloat(lhs, rhs);

    if (cond == DoubleNotEqual) {
        // Force the unordered cases not to jump.
        Label unordered;
        ma_b(&unordered, VFP_Unordered);
        ma_b(label, VFP_NotEqualOrUnordered);
        bind(&unordered);
        return;
    }

    if (cond == DoubleEqualOrUnordered) {
        ma_b(label, VFP_Unordered);
        ma_b(label, VFP_Equal);
        return;
    }

    ma_b(label, ConditionFromDoubleCondition(cond));
}

Assembler::Condition
MacroAssemblerARMCompat::testInt32(Assembler::Condition cond, const ValueOperand& value)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_INT32));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testBoolean(Assembler::Condition cond, const ValueOperand& value)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_BOOLEAN));
    return cond;
}
Assembler::Condition
MacroAssemblerARMCompat::testDouble(Assembler::Condition cond, const ValueOperand& value)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    Assembler::Condition actual = (cond == Equal) ? Below : AboveOrEqual;
    ma_cmp(value.typeReg(), ImmTag(JSVAL_TAG_CLEAR));
    return actual;
}

Assembler::Condition
MacroAssemblerARMCompat::testNull(Assembler::Condition cond, const ValueOperand& value)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_NULL));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testUndefined(Assembler::Condition cond, const ValueOperand& value)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_UNDEFINED));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testString(Assembler::Condition cond, const ValueOperand& value)
{
    return testString(cond, value.typeReg());
}

Assembler::Condition
MacroAssemblerARMCompat::testSymbol(Assembler::Condition cond, const ValueOperand& value)
{
    return testSymbol(cond, value.typeReg());
}

Assembler::Condition
MacroAssemblerARMCompat::testObject(Assembler::Condition cond, const ValueOperand& value)
{
    return testObject(cond, value.typeReg());
}

Assembler::Condition
MacroAssemblerARMCompat::testNumber(Assembler::Condition cond, const ValueOperand& value)
{
    return testNumber(cond, value.typeReg());
}

Assembler::Condition
MacroAssemblerARMCompat::testMagic(Assembler::Condition cond, const ValueOperand& value)
{
    return testMagic(cond, value.typeReg());
}

Assembler::Condition
MacroAssemblerARMCompat::testPrimitive(Assembler::Condition cond, const ValueOperand& value)
{
    return testPrimitive(cond, value.typeReg());
}

// Register-based tests.
Assembler::Condition
MacroAssemblerARMCompat::testInt32(Assembler::Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_INT32));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testBoolean(Assembler::Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_BOOLEAN));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testNull(Assembler::Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_NULL));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testUndefined(Assembler::Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_UNDEFINED));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testString(Assembler::Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_STRING));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testSymbol(Assembler::Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_SYMBOL));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testObject(Assembler::Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_OBJECT));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testMagic(Assembler::Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_MAGIC));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testPrimitive(Assembler::Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET));
    return cond == Equal ? Below : AboveOrEqual;
}

Assembler::Condition
MacroAssemblerARMCompat::testGCThing(Assembler::Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET));
    return cond == Equal ? AboveOrEqual : Below;
}

Assembler::Condition
MacroAssemblerARMCompat::testMagic(Assembler::Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_MAGIC));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testInt32(Assembler::Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_INT32));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testDouble(Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    return testDouble(cond, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARMCompat::testBoolean(Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    return testBoolean(cond, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARMCompat::testNull(Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    return testNull(cond, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARMCompat::testUndefined(Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    return testUndefined(cond, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARMCompat::testString(Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    return testString(cond, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARMCompat::testSymbol(Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    return testSymbol(cond, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARMCompat::testObject(Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    return testObject(cond, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARMCompat::testNumber(Condition cond, const Address& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    return testNumber(cond, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARMCompat::testDouble(Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    Condition actual = (cond == Equal) ? Below : AboveOrEqual;
    ma_cmp(tag, ImmTag(JSVAL_TAG_CLEAR));
    return actual;
}

Assembler::Condition
MacroAssemblerARMCompat::testNumber(Condition cond, Register tag)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET));
    return cond == Equal ? BelowOrEqual : Above;
}

Assembler::Condition
MacroAssemblerARMCompat::testUndefined(Condition cond, const BaseIndex& src)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_UNDEFINED));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testNull(Condition cond, const BaseIndex& src)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_NULL));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testBoolean(Condition cond, const BaseIndex& src)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_BOOLEAN));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testString(Condition cond, const BaseIndex& src)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_STRING));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testSymbol(Condition cond, const BaseIndex& src)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_SYMBOL));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testInt32(Condition cond, const BaseIndex& src)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_INT32));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testObject(Condition cond, const BaseIndex& src)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_OBJECT));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testDouble(Condition cond, const BaseIndex& src)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    Assembler::Condition actual = (cond == Equal) ? Below : AboveOrEqual;
    extractTag(src, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_CLEAR));
    return actual;
}

Assembler::Condition
MacroAssemblerARMCompat::testMagic(Condition cond, const BaseIndex& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_MAGIC));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testGCThing(Condition cond, const BaseIndex& address)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET));
    return cond == Equal ? AboveOrEqual : Below;
}

void
MacroAssemblerARMCompat::branchTestValue(Condition cond, const ValueOperand& value, const Value& v,
                                         Label* label)
{
    // If cond == NotEqual, branch when a.payload != b.payload || a.tag !=
    // b.tag. If the payloads are equal, compare the tags. If the payloads are
    // not equal, short circuit true (NotEqual).
    //
    // If cand == Equal, branch when a.payload == b.payload && a.tag == b.tag.
    // If the payloads are equal, compare the tags. If the payloads are not
    // equal, short circuit false (NotEqual).
    jsval_layout jv = JSVAL_TO_IMPL(v);
    if (v.isMarkable())
        ma_cmp(value.payloadReg(), ImmGCPtr(reinterpret_cast<gc::Cell*>(v.toGCThing())));
    else
        ma_cmp(value.payloadReg(), Imm32(jv.s.payload.i32));
    ma_cmp(value.typeReg(), Imm32(jv.s.tag), Equal);
    ma_b(label, cond);
}

void
MacroAssemblerARMCompat::branchTestValue(Condition cond, const Address& valaddr,
                                         const ValueOperand& value, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);

    // Check payload before tag, since payload is more likely to differ.
    if (cond == NotEqual) {
        ma_ldr(payloadOf(valaddr), ScratchRegister);
        branchPtr(NotEqual, ScratchRegister, value.payloadReg(), label);

        ma_ldr(tagOf(valaddr), ScratchRegister);
        branchPtr(NotEqual, ScratchRegister, value.typeReg(), label);

    } else {
        Label fallthrough;

        ma_ldr(payloadOf(valaddr), ScratchRegister);
        branchPtr(NotEqual, ScratchRegister, value.payloadReg(), &fallthrough);

        ma_ldr(tagOf(valaddr), ScratchRegister);
        branchPtr(Equal, ScratchRegister, value.typeReg(), label);

        bind(&fallthrough);
    }
}

// Unboxing code.
void
MacroAssemblerARMCompat::unboxNonDouble(const ValueOperand& operand, Register dest)
{
    if (operand.payloadReg() != dest)
        ma_mov(operand.payloadReg(), dest);
}

void
MacroAssemblerARMCompat::unboxNonDouble(const Address& src, Register dest)
{
    ma_ldr(payloadOf(src), dest);
}

void
MacroAssemblerARMCompat::unboxNonDouble(const BaseIndex& src, Register dest)
{
    ma_alu(src.base, lsl(src.index, src.scale), ScratchRegister, OpAdd);
    ma_ldr(Address(ScratchRegister, src.offset), dest);
}

void
MacroAssemblerARMCompat::unboxDouble(const ValueOperand& operand, FloatRegister dest)
{
    MOZ_ASSERT(dest.isDouble());
    as_vxfer(operand.payloadReg(), operand.typeReg(),
             VFPRegister(dest), CoreToFloat);
}

void
MacroAssemblerARMCompat::unboxDouble(const Address& src, FloatRegister dest)
{
    MOZ_ASSERT(dest.isDouble());
    ma_vldr(Operand(src), dest);
}

void
MacroAssemblerARMCompat::unboxValue(const ValueOperand& src, AnyRegister dest)
{
    if (dest.isFloat()) {
        Label notInt32, end;
        branchTestInt32(Assembler::NotEqual, src, &notInt32);
        convertInt32ToDouble(src.payloadReg(), dest.fpu());
        ma_b(&end);
        bind(&notInt32);
        unboxDouble(src, dest.fpu());
        bind(&end);
    } else if (src.payloadReg() != dest.gpr()) {
        as_mov(dest.gpr(), O2Reg(src.payloadReg()));
    }
}

void
MacroAssemblerARMCompat::unboxPrivate(const ValueOperand& src, Register dest)
{
    ma_mov(src.payloadReg(), dest);
}

void
MacroAssemblerARMCompat::boxDouble(FloatRegister src, const ValueOperand& dest)
{
    as_vxfer(dest.payloadReg(), dest.typeReg(), VFPRegister(src), FloatToCore);
}

void
MacroAssemblerARMCompat::boxNonDouble(JSValueType type, Register src, const ValueOperand& dest) {
    if (src != dest.payloadReg())
        ma_mov(src, dest.payloadReg());
    ma_mov(ImmType(type), dest.typeReg());
}

void
MacroAssemblerARMCompat::boolValueToDouble(const ValueOperand& operand, FloatRegister dest)
{
    VFPRegister d = VFPRegister(dest);
    ma_vimm(1.0, dest);
    ma_cmp(operand.payloadReg(), Imm32(0));
    // If the source is 0, then subtract the dest from itself, producing 0.
    as_vsub(d, d, d, Equal);
}

void
MacroAssemblerARMCompat::int32ValueToDouble(const ValueOperand& operand, FloatRegister dest)
{
    // Transfer the integral value to a floating point register.
    VFPRegister vfpdest = VFPRegister(dest);
    as_vxfer(operand.payloadReg(), InvalidReg,
             ScratchFloat32Reg.sintOverlay(), CoreToFloat);
    // Convert the value to a double.
    as_vcvt(vfpdest, ScratchFloat32Reg.sintOverlay());
}

void
MacroAssemblerARMCompat::boolValueToFloat32(const ValueOperand& operand, FloatRegister dest)
{
    VFPRegister d = VFPRegister(dest).singleOverlay();
    ma_vimm_f32(1.0, dest);
    ma_cmp(operand.payloadReg(), Imm32(0));
    // If the source is 0, then subtract the dest from itself, producing 0.
    as_vsub(d, d, d, Equal);
}

void
MacroAssemblerARMCompat::int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest)
{
    // Transfer the integral value to a floating point register.
    VFPRegister vfpdest = VFPRegister(dest).singleOverlay();
    as_vxfer(operand.payloadReg(), InvalidReg,
             vfpdest.sintOverlay(), CoreToFloat);
    // Convert the value to a float.
    as_vcvt(vfpdest, vfpdest.sintOverlay());
}

void
MacroAssemblerARMCompat::loadConstantFloat32(float f, FloatRegister dest)
{
    ma_vimm_f32(f, dest);
}

void
MacroAssemblerARMCompat::loadInt32OrDouble(const Operand& src, FloatRegister dest)
{
    Label notInt32, end;
    // If it's an int, convert it to double.
    ma_ldr(ToType(src), ScratchRegister);
    branchTestInt32(Assembler::NotEqual, ScratchRegister, &notInt32);
    ma_ldr(ToPayload(src), ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
    ma_b(&end);

    // Not an int, just load as double.
    bind(&notInt32);
    ma_vldr(src, dest);
    bind(&end);
}

void
MacroAssemblerARMCompat::loadInt32OrDouble(Register base, Register index, FloatRegister dest, int32_t shift)
{
    Label notInt32, end;

    JS_STATIC_ASSERT(NUNBOX32_PAYLOAD_OFFSET == 0);

    // If it's an int, convert it to double.
    ma_alu(base, lsl(index, shift), ScratchRegister, OpAdd);

    // Since we only have one scratch register, we need to stomp over it with
    // the tag.
    ma_ldr(Address(ScratchRegister, NUNBOX32_TYPE_OFFSET), ScratchRegister);
    branchTestInt32(Assembler::NotEqual, ScratchRegister, &notInt32);

    // Implicitly requires NUNBOX32_PAYLOAD_OFFSET == 0: no offset provided
    ma_ldr(DTRAddr(base, DtrRegImmShift(index, LSL, shift)), ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
    ma_b(&end);

    // Not an int, just load as double.
    bind(&notInt32);
    // First, recompute the offset that had been stored in the scratch register
    // since the scratch register was overwritten loading in the type.
    ma_alu(base, lsl(index, shift), ScratchRegister, OpAdd);
    ma_vldr(Address(ScratchRegister, 0), dest);
    bind(&end);
}

void
MacroAssemblerARMCompat::loadConstantDouble(double dp, FloatRegister dest)
{
    as_FImm64Pool(dest, dp);
}

// Treat the value as a boolean, and set condition codes accordingly.

Assembler::Condition
MacroAssemblerARMCompat::testInt32Truthy(bool truthy, const ValueOperand& operand)
{
    ma_tst(operand.payloadReg(), operand.payloadReg());
    return truthy ? NonZero : Zero;
}

Assembler::Condition
MacroAssemblerARMCompat::testBooleanTruthy(bool truthy, const ValueOperand& operand)
{
    ma_tst(operand.payloadReg(), operand.payloadReg());
    return truthy ? NonZero : Zero;
}

Assembler::Condition
MacroAssemblerARMCompat::testDoubleTruthy(bool truthy, FloatRegister reg)
{
    as_vcmpz(VFPRegister(reg));
    as_vmrs(pc);
    as_cmp(r0, O2Reg(r0), Overflow);
    return truthy ? NonZero : Zero;
}

Register
MacroAssemblerARMCompat::extractObject(const Address& address, Register scratch)
{
    ma_ldr(payloadOf(address), scratch);
    return scratch;
}

Register
MacroAssemblerARMCompat::extractTag(const Address& address, Register scratch)
{
    ma_ldr(tagOf(address), scratch);
    return scratch;
}

Register
MacroAssemblerARMCompat::extractTag(const BaseIndex& address, Register scratch)
{
    ma_alu(address.base, lsl(address.index, address.scale), scratch, OpAdd, NoSetCond);
    return extractTag(Address(scratch, address.offset), scratch);
}

template <typename T>
void
MacroAssemblerARMCompat::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const T& dest,
                                           MIRType slotType)
{
    if (valueType == MIRType_Double) {
        storeDouble(value.reg().typedReg().fpu(), dest);
        return;
    }

    // Store the type tag if needed.
    if (valueType != slotType)
        storeTypeTag(ImmType(ValueTypeFromMIRType(valueType)), dest);

    // Store the payload.
    if (value.constant())
        storePayload(value.value(), dest);
    else
        storePayload(value.reg().typedReg().gpr(), dest);
}

template void
MacroAssemblerARMCompat::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const Address& dest,
                                           MIRType slotType);

template void
MacroAssemblerARMCompat::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const BaseIndex& dest,
                                           MIRType slotType);

void
MacroAssemblerARMCompat::moveValue(const Value& val, Register type, Register data)
{
    jsval_layout jv = JSVAL_TO_IMPL(val);
    ma_mov(Imm32(jv.s.tag), type);
    if (val.isMarkable())
        ma_mov(ImmGCPtr(reinterpret_cast<gc::Cell*>(val.toGCThing())), data);
    else
        ma_mov(Imm32(jv.s.payload.i32), data);
}
void
MacroAssemblerARMCompat::moveValue(const Value& val, const ValueOperand& dest)
{
    moveValue(val, dest.typeReg(), dest.payloadReg());
}

/////////////////////////////////////////////////////////////////
// X86/X64-common (ARM too now) interface.
/////////////////////////////////////////////////////////////////
void
MacroAssemblerARMCompat::storeValue(ValueOperand val, Operand dst)
{
    ma_str(val.payloadReg(), ToPayload(dst));
    ma_str(val.typeReg(), ToType(dst));
}

void
MacroAssemblerARMCompat::storeValue(ValueOperand val, const BaseIndex& dest)
{
    if (isValueDTRDCandidate(val) && Abs(dest.offset) <= 255) {
        Register tmpIdx;
        if (dest.offset == 0) {
            if (dest.scale == TimesOne) {
                tmpIdx = dest.index;
            } else {
                ma_lsl(Imm32(dest.scale), dest.index, ScratchRegister);
                tmpIdx = ScratchRegister;
            }
            ma_strd(val.payloadReg(), val.typeReg(), EDtrAddr(dest.base, EDtrOffReg(tmpIdx)));
        } else {
            ma_alu(dest.base, lsl(dest.index, dest.scale), ScratchRegister, OpAdd);
            ma_strd(val.payloadReg(), val.typeReg(),
                    EDtrAddr(ScratchRegister, EDtrOffImm(dest.offset)));
        }
    } else {
        ma_alu(dest.base, lsl(dest.index, dest.scale), ScratchRegister, OpAdd);
        storeValue(val, Address(ScratchRegister, dest.offset));
    }
}

void
MacroAssemblerARMCompat::loadValue(const BaseIndex& addr, ValueOperand val)
{
    if (isValueDTRDCandidate(val) && Abs(addr.offset) <= 255) {
        Register tmpIdx;
        if (addr.offset == 0) {
            if (addr.scale == TimesOne) {
                tmpIdx = addr.index;
            } else {
                ma_lsl(Imm32(addr.scale), addr.index, ScratchRegister);
                tmpIdx = ScratchRegister;
            }
            ma_ldrd(EDtrAddr(addr.base, EDtrOffReg(tmpIdx)), val.payloadReg(), val.typeReg());
        } else {
            ma_alu(addr.base, lsl(addr.index, addr.scale), ScratchRegister, OpAdd);
            ma_ldrd(EDtrAddr(ScratchRegister, EDtrOffImm(addr.offset)),
                    val.payloadReg(), val.typeReg());
        }
    } else {
        ma_alu(addr.base, lsl(addr.index, addr.scale), ScratchRegister, OpAdd);
        loadValue(Address(ScratchRegister, addr.offset), val);
    }
}

void
MacroAssemblerARMCompat::loadValue(Address src, ValueOperand val)
{
    Operand srcOp = Operand(src);
    Operand payload = ToPayload(srcOp);
    Operand type = ToType(srcOp);
    // TODO: copy this code into a generic function that acts on all sequences
    // of memory accesses
    if (isValueDTRDCandidate(val)) {
        // If the value we want is in two consecutive registers starting with an
        // even register, they can be combined as a single ldrd.
        int offset = srcOp.disp();
        if (offset < 256 && offset > -256) {
            ma_ldrd(EDtrAddr(Register::FromCode(srcOp.base()), EDtrOffImm(srcOp.disp())), val.payloadReg(), val.typeReg());
            return;
        }
    }
    // If the value is lower than the type, then we may be able to use an ldm
    // instruction.

    if (val.payloadReg().code() < val.typeReg().code()) {
        if (srcOp.disp() <= 4 && srcOp.disp() >= -8 && (srcOp.disp() & 3) == 0) {
            // Turns out each of the 4 value -8, -4, 0, 4 corresponds exactly
            // with one of LDM{DB, DA, IA, IB}
            DTMMode mode;
            switch(srcOp.disp()) {
              case -8:
                mode = DB;
                break;
              case -4:
                mode = DA;
                break;
              case 0:
                mode = IA;
                break;
              case 4:
                mode = IB;
                break;
              default:
                MOZ_CRASH("Bogus Offset for LoadValue as DTM");
            }
            startDataTransferM(IsLoad, Register::FromCode(srcOp.base()), mode);
            transferReg(val.payloadReg());
            transferReg(val.typeReg());
            finishDataTransfer();
            return;
        }
    }
    // Ensure that loading the payload does not erase the pointer to the Value
    // in memory.
    if (Register::FromCode(type.base()) != val.payloadReg()) {
        ma_ldr(payload, val.payloadReg());
        ma_ldr(type, val.typeReg());
    } else {
        ma_ldr(type, val.typeReg());
        ma_ldr(payload, val.payloadReg());
    }
}

void
MacroAssemblerARMCompat::tagValue(JSValueType type, Register payload, ValueOperand dest)
{
    MOZ_ASSERT(dest.typeReg() != dest.payloadReg());
    if (payload != dest.payloadReg())
        ma_mov(payload, dest.payloadReg());
    ma_mov(ImmType(type), dest.typeReg());
}

void
MacroAssemblerARMCompat::pushValue(ValueOperand val) {
    ma_push(val.typeReg());
    ma_push(val.payloadReg());
}
void
MacroAssemblerARMCompat::pushValue(const Address& addr)
{
    MOZ_ASSERT(addr.base != StackPointer);
    Operand srcOp = Operand(addr);
    Operand payload = ToPayload(srcOp);
    Operand type = ToType(srcOp);

    ma_ldr(type, ScratchRegister);
    ma_push(ScratchRegister);
    ma_ldr(payload, ScratchRegister);
    ma_push(ScratchRegister);
}

void
MacroAssemblerARMCompat::popValue(ValueOperand val) {
    ma_pop(val.payloadReg());
    ma_pop(val.typeReg());
}
void
MacroAssemblerARMCompat::storePayload(const Value& val, Operand dest)
{
    jsval_layout jv = JSVAL_TO_IMPL(val);
    if (val.isMarkable())
        ma_mov(ImmGCPtr((gc::Cell*)jv.s.payload.ptr), secondScratchReg_);
    else
        ma_mov(Imm32(jv.s.payload.i32), secondScratchReg_);
    ma_str(secondScratchReg_, ToPayload(dest));
}
void
MacroAssemblerARMCompat::storePayload(Register src, Operand dest)
{
    if (dest.getTag() == Operand::MEM) {
        ma_str(src, ToPayload(dest));
        return;
    }
    MOZ_CRASH("why do we do all of these things?");

}

void
MacroAssemblerARMCompat::storePayload(const Value& val, const BaseIndex& dest)
{
    unsigned shift = ScaleToShift(dest.scale);

    jsval_layout jv = JSVAL_TO_IMPL(val);
    if (val.isMarkable())
        ma_mov(ImmGCPtr((gc::Cell*)jv.s.payload.ptr), ScratchRegister);
    else
        ma_mov(Imm32(jv.s.payload.i32), ScratchRegister);

    // If NUNBOX32_PAYLOAD_OFFSET is not zero, the memory operand [base + index
    // << shift + imm] cannot be encoded into a single instruction, and cannot
    // be integrated into the as_dtr call.
    JS_STATIC_ASSERT(NUNBOX32_PAYLOAD_OFFSET == 0);

    // If an offset is used, modify the base so that a [base + index << shift]
    // instruction format can be used.
    if (dest.offset != 0)
        ma_add(dest.base, Imm32(dest.offset), dest.base);

    as_dtr(IsStore, 32, Offset, ScratchRegister,
           DTRAddr(dest.base, DtrRegImmShift(dest.index, LSL, shift)));

    // Restore the original value of the base, if necessary.
    if (dest.offset != 0)
        ma_sub(dest.base, Imm32(dest.offset), dest.base);
}

void
MacroAssemblerARMCompat::storePayload(Register src, const BaseIndex& dest)
{
    unsigned shift = ScaleToShift(dest.scale);
    MOZ_ASSERT(shift < 32);

    // If NUNBOX32_PAYLOAD_OFFSET is not zero, the memory operand [base + index
    // << shift + imm] cannot be encoded into a single instruction, and cannot
    // be integrated into the as_dtr call.
    JS_STATIC_ASSERT(NUNBOX32_PAYLOAD_OFFSET == 0);

    // Save/restore the base if the BaseIndex has an offset, as above.
    if (dest.offset != 0)
        ma_add(dest.base, Imm32(dest.offset), dest.base);

    // Technically, shift > -32 can be handle by changing LSL to ASR, but should
    // never come up, and this is one less code path to get wrong.
    as_dtr(IsStore, 32, Offset, src, DTRAddr(dest.base, DtrRegImmShift(dest.index, LSL, shift)));

    if (dest.offset != 0)
        ma_sub(dest.base, Imm32(dest.offset), dest.base);
}

void
MacroAssemblerARMCompat::storeTypeTag(ImmTag tag, Operand dest) {
    if (dest.getTag() == Operand::MEM) {
        ma_mov(tag, secondScratchReg_);
        ma_str(secondScratchReg_, ToType(dest));
        return;
    }

    MOZ_CRASH("why do we do all of these things?");

}

void
MacroAssemblerARMCompat::storeTypeTag(ImmTag tag, const BaseIndex& dest)
{
    Register base = dest.base;
    Register index = dest.index;
    unsigned shift = ScaleToShift(dest.scale);
    MOZ_ASSERT(base != ScratchRegister);
    MOZ_ASSERT(index != ScratchRegister);

    // A value needs to be store a value int base + index << shift + 4.
    // ARM cannot handle this in a single operand, so a temp register is
    // required. However, the scratch register is presently in use to hold the
    // immediate that is being stored into said memory location. Work around
    // this by modifying the base so the valid [base + index << shift] format
    // can be used, then restore it.
    ma_add(base, Imm32(NUNBOX32_TYPE_OFFSET + dest.offset), base);
    ma_mov(tag, ScratchRegister);
    ma_str(ScratchRegister, DTRAddr(base, DtrRegImmShift(index, LSL, shift)));
    ma_sub(base, Imm32(NUNBOX32_TYPE_OFFSET + dest.offset), base);
}

// ARM says that all reads of pc will return 8 higher than the address of the
// currently executing instruction. This means we are correctly storing the
// address of the instruction after the call in the register.
//
// Also ION is breaking the ARM EABI here (sort of). The ARM EABI says that a
// function call should move the pc into the link register, then branch to the
// function, and *sp is data that is owned by the caller, not the callee. The
// ION ABI says *sp should be the address that we will return to when leaving
// this function.
void
MacroAssemblerARM::ma_callJit(const Register r)
{
    // When the stack is 8 byte aligned, we want to decrement sp by 8, and write
    // pc + 8 into the new sp. When we return from this call, sp will be its
    // present value minus 4.
    as_sub(sp, sp, Imm8(4));
    as_blx(r);
}
void
MacroAssemblerARM::ma_callJitNoPush(const Register r)
{
    // Since we just write the return address into the stack, which is popped on
    // return, the net effect is removing 4 bytes from the stack.

    // Bug 1103108: remove this function, and refactor all uses.
    as_add(sp, sp, Imm8(4));
    as_blx(r);
}

void
MacroAssemblerARM::ma_callJitHalfPush(const Register r)
{
    // The stack is unaligned by 4 bytes. We push the pc to the stack to align
    // the stack before the call, when we return the pc is poped and the stack
    // is restored to its unaligned state.
    as_blx(r);
}

void
MacroAssemblerARM::ma_callJitHalfPush(Label* label)
{
    // The stack is unaligned by 4 bytes. The callee will push the lr to the stack to align
    // the stack after the call, when we return the pc is poped and the stack
    // is restored to its unaligned state.

    // leave the stack as-is so the callee-side can push when necessary.

    as_bl(label, Always);
}

void
MacroAssemblerARM::ma_call(ImmPtr dest)
{
    RelocStyle rs;
    if (HasMOVWT())
        rs = L_MOVWT;
    else
        rs = L_LDR;

    ma_movPatchable(dest, CallReg, Always, rs);
    as_blx(CallReg);
}

void
MacroAssemblerARMCompat::breakpoint()
{
    as_bkpt();
}

void
MacroAssemblerARMCompat::simulatorStop(const char* msg)
{
#if defined(JS_ARM_SIMULATOR)
    MOZ_ASSERT(sizeof(char*) == 4);
    writeInst(0xefffffff);
    writeInst((int)msg);
#endif
}

void
MacroAssemblerARMCompat::ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure)
{
    Label isDouble, done;
    branchTestDouble(Assembler::Equal, source.typeReg(), &isDouble);
    branchTestInt32(Assembler::NotEqual, source.typeReg(), failure);

    convertInt32ToDouble(source.payloadReg(), dest);
    jump(&done);

    bind(&isDouble);
    unboxDouble(source, dest);

    bind(&done);
}

void
MacroAssemblerARMCompat::breakpoint(Condition cc)
{
    ma_ldr(DTRAddr(r12, DtrRegImmShift(r12, LSL, 0, IsDown)), r12, Offset, cc);
}

void
MacroAssemblerARMCompat::setupABICall(uint32_t args)
{
    MOZ_ASSERT(!inCall_);
    inCall_ = true;
    args_ = args;
    passedArgs_ = 0;
    passedArgTypes_ = 0;
    usedIntSlots_ = 0;
#if defined(JS_CODEGEN_ARM_HARDFP) || defined(JS_ARM_SIMULATOR)
    usedFloatSlots_ = 0;
    usedFloat32_ = false;
    padding_ = 0;
#endif
    floatArgsInGPR[0] = MoveOperand();
    floatArgsInGPR[1] = MoveOperand();
    floatArgsInGPR[2] = MoveOperand();
    floatArgsInGPR[3] = MoveOperand();
    floatArgsInGPRValid[0] = false;
    floatArgsInGPRValid[1] = false;
    floatArgsInGPRValid[2] = false;
    floatArgsInGPRValid[3] = false;
}

void
MacroAssemblerARMCompat::setupAlignedABICall(uint32_t args)
{
    setupABICall(args);

    dynamicAlignment_ = false;
}

void
MacroAssemblerARMCompat::setupUnalignedABICall(uint32_t args, Register scratch)
{
    setupABICall(args);
    dynamicAlignment_ = true;

    ma_mov(sp, scratch);

    // Force sp to be aligned.
    ma_and(Imm32(~(ABIStackAlignment - 1)), sp, sp);
    ma_push(scratch);
}

#if defined(JS_CODEGEN_ARM_HARDFP) || defined(JS_ARM_SIMULATOR)
void
MacroAssemblerARMCompat::passHardFpABIArg(const MoveOperand& from, MoveOp::Type type)
{
    MoveOperand to;
    ++passedArgs_;
    if (!enoughMemory_)
        return;
    switch (type) {
      case MoveOp::FLOAT32: {
        FloatRegister fr;
        passedArgTypes_ = (passedArgTypes_ << ArgType_Shift) | ArgType_Float32;
        if (GetFloat32ArgReg(usedIntSlots_, usedFloatSlots_, &fr)) {
            if (from.isFloatReg() && from.floatReg() == fr) {
                // Nothing to do; the value is in the right register already.
                usedFloatSlots_++;
                passedArgTypes_ = (passedArgTypes_ << ArgType_Shift) | ArgType_Float32;
                return;
            }
            to = MoveOperand(fr);
        } else {
            // If (and only if) the integer registers have started spilling, do
            // we need to take the register's alignment into account.
            uint32_t disp = GetFloat32ArgStackDisp(usedIntSlots_, usedFloatSlots_, &padding_);
            to = MoveOperand(sp, disp);
        }
        usedFloatSlots_++;
        break;
      }

      case MoveOp::DOUBLE: {
          FloatRegister fr;
          passedArgTypes_ = (passedArgTypes_ << ArgType_Shift) | ArgType_Double;
          usedFloatSlots_ = (usedFloatSlots_ + 1) & -2;
          if (GetDoubleArgReg(usedIntSlots_, usedFloatSlots_, &fr)) {
              if (from.isFloatReg() && from.floatReg() == fr) {
                  // Nothing to do; the value is in the right register already.
                  usedFloatSlots_ += 2;
                  return;
              }
              to = MoveOperand(fr);
          } else {
              // If (and only if) the integer registers have started spilling, do we
              // need to take the register's alignment into account
              uint32_t disp = GetDoubleArgStackDisp(usedIntSlots_, usedFloatSlots_, &padding_);
              to = MoveOperand(sp, disp);
          }
          usedFloatSlots_+=2;
          break;
      }
      case MoveOp::GENERAL: {
        Register r;
        passedArgTypes_ = (passedArgTypes_ << ArgType_Shift) | ArgType_General;
        if (GetIntArgReg(usedIntSlots_, usedFloatSlots_, &r)) {
            if (from.isGeneralReg() && from.reg() == r) {
                // Nothing to do; the value is in the right register already.
                usedIntSlots_++;
                return;
            }
            to = MoveOperand(r);
        } else {
            uint32_t disp = GetIntArgStackDisp(usedIntSlots_, usedFloatSlots_, &padding_);
            to = MoveOperand(sp, disp);
        }
        usedIntSlots_++;
        break;
      }
      default:
        MOZ_CRASH("Unexpected argument type");
    }

    enoughMemory_ = moveResolver_.addMove(from, to, type);
}
#endif

#if !defined(JS_CODEGEN_ARM_HARDFP) || defined(JS_ARM_SIMULATOR)
void
MacroAssemblerARMCompat::passSoftFpABIArg(const MoveOperand& from, MoveOp::Type type)
{
    MoveOperand to;
    uint32_t increment = 1;
    bool useResolver = true;
    ++passedArgs_;
    switch (type) {
      case MoveOp::DOUBLE:
        // Double arguments need to be rounded up to the nearest doubleword
        // boundary, even if it is in a register!
        usedIntSlots_ = (usedIntSlots_ + 1) & ~1;
        increment = 2;
        passedArgTypes_ = (passedArgTypes_ << ArgType_Shift) | ArgType_Double;
        break;
      case MoveOp::FLOAT32:
        passedArgTypes_ = (passedArgTypes_ << ArgType_Shift) | ArgType_Float32;
        break;
      case MoveOp::GENERAL:
        passedArgTypes_ = (passedArgTypes_ << ArgType_Shift) | ArgType_General;
        break;
      default:
        MOZ_CRASH("Unexpected argument type");
    }

    Register destReg;
    MoveOperand dest;
    if (GetIntArgReg(usedIntSlots_, 0, &destReg)) {
        if (type == MoveOp::DOUBLE || type == MoveOp::FLOAT32) {
            floatArgsInGPR[destReg.code()] = from;
            floatArgsInGPRValid[destReg.code()] = true;
            useResolver = false;
        } else if (from.isGeneralReg() && from.reg() == destReg) {
            // No need to move anything.
            useResolver = false;
        } else {
            dest = MoveOperand(destReg);
        }
    } else {
        uint32_t disp = GetArgStackDisp(usedIntSlots_);
        dest = MoveOperand(sp, disp);
    }

    if (useResolver)
        enoughMemory_ = enoughMemory_ && moveResolver_.addMove(from, dest, type);
    usedIntSlots_ += increment;
}
#endif

void
MacroAssemblerARMCompat::passABIArg(const MoveOperand& from, MoveOp::Type type)
{
#if defined(JS_ARM_SIMULATOR)
    if (UseHardFpABI())
        MacroAssemblerARMCompat::passHardFpABIArg(from, type);
    else
        MacroAssemblerARMCompat::passSoftFpABIArg(from, type);
#elif defined(JS_CODEGEN_ARM_HARDFP)
    MacroAssemblerARMCompat::passHardFpABIArg(from, type);
#else
    MacroAssemblerARMCompat::passSoftFpABIArg(from, type);
#endif
}

void
MacroAssemblerARMCompat::passABIArg(Register reg)
{
    passABIArg(MoveOperand(reg), MoveOp::GENERAL);
}

void
MacroAssemblerARMCompat::passABIArg(FloatRegister freg, MoveOp::Type type)
{
    passABIArg(MoveOperand(freg), type);
}

void MacroAssemblerARMCompat::checkStackAlignment()
{
#ifdef DEBUG
    ma_tst(sp, Imm32(ABIStackAlignment - 1));
    breakpoint(NonZero);
#endif
}

void
MacroAssemblerARMCompat::callWithABIPre(uint32_t* stackAdjust, bool callFromAsmJS)
{
    MOZ_ASSERT(inCall_);

    *stackAdjust = ((usedIntSlots_ > NumIntArgRegs) ? usedIntSlots_ - NumIntArgRegs : 0) * sizeof(intptr_t);
#if defined(JS_CODEGEN_ARM_HARDFP) || defined(JS_ARM_SIMULATOR)
    if (UseHardFpABI())
        *stackAdjust += 2*((usedFloatSlots_ > NumFloatArgRegs) ? usedFloatSlots_ - NumFloatArgRegs : 0) * sizeof(intptr_t);
#endif
    uint32_t alignmentAtPrologue = callFromAsmJS ? sizeof(AsmJSFrame) : 0;

    if (!dynamicAlignment_) {
        *stackAdjust += ComputeByteAlignment(framePushed_ + *stackAdjust + alignmentAtPrologue,
                                             ABIStackAlignment);
    } else {
        // sizeof(intptr_t) accounts for the saved stack pointer pushed by
        // setupUnalignedABICall.
        *stackAdjust += ComputeByteAlignment(*stackAdjust + sizeof(intptr_t), ABIStackAlignment);
    }

    reserveStack(*stackAdjust);

    // Position all arguments.
    {
        enoughMemory_ = enoughMemory_ && moveResolver_.resolve();
        if (!enoughMemory_)
            return;

        MoveEmitter emitter(*this);
        emitter.emit(moveResolver_);
        emitter.finish();
    }
    for (int i = 0; i < 4; i++) {
        if (floatArgsInGPRValid[i]) {
            MoveOperand from = floatArgsInGPR[i];
            Register to0 = Register::FromCode(i);
            Register to1;

            if (!from.isFloatReg() || from.floatReg().isDouble()) {
                // Doubles need to be moved into a pair of aligned registers
                // whether they come from the stack, or VFP registers.
                to1 = Register::FromCode(i + 1);
                MOZ_ASSERT(i % 2 == 0);
            }

            if (from.isFloatReg()) {
                if (from.floatReg().isDouble())
                    ma_vxfer(from.floatReg(), to0, to1);
                else
                    ma_vxfer(from.floatReg(), to0);
            } else {
                MOZ_ASSERT(from.isMemory());
                // Note: We can safely use the MoveOperand's displacement here,
                // even if the base is SP: MoveEmitter::toOperand adjusts
                // SP-relative operands by the difference between the current
                // stack usage and stackAdjust, which emitter.finish() resets to
                // 0.
                //
                // Warning: if the offset isn't within [-255,+255] then this
                // will assert-fail (or, if non-debug, load the wrong words).
                // Nothing uses such an offset at the time of this writing.
                ma_ldrd(EDtrAddr(from.base(), EDtrOffImm(from.disp())), to0, to1);
            }
        }
    }
    checkStackAlignment();

    // Save the lr register if we need to preserve it.
    if (secondScratchReg_ != lr)
        ma_mov(lr, secondScratchReg_);
}

void
MacroAssemblerARMCompat::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result)
{
    if (secondScratchReg_ != lr)
        ma_mov(secondScratchReg_, lr);

    switch (result) {
      case MoveOp::DOUBLE:
        if (!UseHardFpABI()) {
            // Move double from r0/r1 to ReturnFloatReg.
            as_vxfer(r0, r1, ReturnDoubleReg, CoreToFloat);
            break;
        }
      case MoveOp::FLOAT32:
        if (!UseHardFpABI()) {
            // Move float32 from r0 to ReturnFloatReg.
            as_vxfer(r0, InvalidReg, ReturnFloat32Reg.singleOverlay(), CoreToFloat);
            break;
        }
      case MoveOp::GENERAL:
        break;

      default:
        MOZ_CRASH("unexpected callWithABI result");
    }

    freeStack(stackAdjust);

    if (dynamicAlignment_) {
        // While the x86 supports pop esp, on ARM that isn't well defined, so
        // just do it manually.
        as_dtr(IsLoad, 32, Offset, sp, DTRAddr(sp, DtrOffImm(0)));
    }

    MOZ_ASSERT(inCall_);
    inCall_ = false;
}

#if defined(DEBUG) && defined(JS_ARM_SIMULATOR)
static void
AssertValidABIFunctionType(uint32_t passedArgTypes)
{
    switch (passedArgTypes) {
      case Args_General0:
      case Args_General1:
      case Args_General2:
      case Args_General3:
      case Args_General4:
      case Args_General5:
      case Args_General6:
      case Args_General7:
      case Args_General8:
      case Args_Double_None:
      case Args_Int_Double:
      case Args_Float32_Float32:
      case Args_Double_Double:
      case Args_Double_Int:
      case Args_Double_DoubleInt:
      case Args_Double_DoubleDouble:
      case Args_Double_IntDouble:
      case Args_Int_IntDouble:
      case Args_Double_DoubleDoubleDouble:
      case Args_Double_DoubleDoubleDoubleDouble:
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }
}
#endif

void
MacroAssemblerARMCompat::callWithABI(void* fun, MoveOp::Type result)
{
#ifdef JS_ARM_SIMULATOR
    MOZ_ASSERT(passedArgs_ <= 15);
    passedArgTypes_ <<= ArgType_Shift;
    switch (result) {
      case MoveOp::GENERAL: passedArgTypes_ |= ArgType_General; break;
      case MoveOp::DOUBLE:  passedArgTypes_ |= ArgType_Double;  break;
      case MoveOp::FLOAT32: passedArgTypes_ |= ArgType_Float32; break;
      default: MOZ_CRASH("Invalid return type");
    }
#ifdef DEBUG
    AssertValidABIFunctionType(passedArgTypes_);
#endif
    ABIFunctionType type = ABIFunctionType(passedArgTypes_);
    fun = Simulator::RedirectNativeFunction(fun, type);
#endif

    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    ma_call(ImmPtr(fun));
    callWithABIPost(stackAdjust, result);
}

void
MacroAssemblerARMCompat::callWithABI(AsmJSImmPtr imm, MoveOp::Type result)
{
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust, /* callFromAsmJS = */ true);
    call(imm);
    callWithABIPost(stackAdjust, result);
}

void
MacroAssemblerARMCompat::callWithABI(const Address& fun, MoveOp::Type result)
{
    // Load the callee in r12, no instruction between the ldr and call should
    // clobber it. Note that we can't use fun.base because it may be one of the
    // IntArg registers clobbered before the call.
    ma_ldr(fun, r12);
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(r12);
    callWithABIPost(stackAdjust, result);
}

void
MacroAssemblerARMCompat::callWithABI(Register fun, MoveOp::Type result)
{
    // Load the callee in r12, as above.
    ma_mov(fun, r12);
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(r12);
    callWithABIPost(stackAdjust, result);
}

void
MacroAssemblerARMCompat::handleFailureWithHandlerTail(void* handler)
{
    // Reserve space for exception information.
    int size = (sizeof(ResumeFromException) + 7) & ~7;

    ma_sub(Imm32(size), sp);
    ma_mov(sp, r0);

    // Call the handler.
    setupUnalignedABICall(1, r1);
    passABIArg(r0);
    callWithABI(handler);

    Label entryFrame;
    Label catch_;
    Label finally;
    Label return_;
    Label bailout;

    ma_ldr(Operand(sp, offsetof(ResumeFromException, kind)), r0);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_ENTRY_FRAME), &entryFrame);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_CATCH), &catch_);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_FINALLY), &finally);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_FORCED_RETURN), &return_);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);

    breakpoint(); // Invalid kind.

    // No exception handler. Load the error value, load the new stack pointer
    // and return from the entry frame.
    bind(&entryFrame);
    moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, stackPointer)), sp);

    // We're going to be returning by the ion calling convention, which returns
    // by ??? (for now, I think ldr pc, [sp]!)
    as_dtr(IsLoad, 32, PostIndex, pc, DTRAddr(sp, DtrOffImm(4)));

    // If we found a catch handler, this must be a baseline frame. Restore state
    // and jump to the catch block.
    bind(&catch_);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, target)), r0);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, framePointer)), r11);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, stackPointer)), sp);
    jump(r0);

    // If we found a finally block, this must be a baseline frame. Push two
    // values expected by JSOP_RETSUB: BooleanValue(true) and the exception.
    bind(&finally);
    ValueOperand exception = ValueOperand(r1, r2);
    loadValue(Operand(sp, offsetof(ResumeFromException, exception)), exception);

    ma_ldr(Operand(sp, offsetof(ResumeFromException, target)), r0);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, framePointer)), r11);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, stackPointer)), sp);

    pushValue(BooleanValue(true));
    pushValue(exception);
    jump(r0);

    // Only used in debug mode. Return BaselineFrame->returnValue() to the
    // caller.
    bind(&return_);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, framePointer)), r11);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, stackPointer)), sp);
    loadValue(Address(r11, BaselineFrame::reverseOffsetOfReturnValue()), JSReturnOperand);
    ma_mov(r11, sp);
    pop(r11);

    // If profiling is enabled, then update the lastProfilingFrame to refer to caller
    // frame before returning.
    {
        Label skipProfilingInstrumentation;
        // Test if profiler enabled.
        AbsoluteAddress addressOfEnabled(GetJitContext()->runtime->spsProfiler().addressOfEnabled());
        branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skipProfilingInstrumentation);
        profilerExitFrame();
        bind(&skipProfilingInstrumentation);
    }

    ret();

    // If we are bailing out to baseline to handle an exception, jump to the
    // bailout tail stub.
    bind(&bailout);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, bailoutInfo)), r2);
    ma_mov(Imm32(BAILOUT_RETURN_OK), r0);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, target)), r1);
    jump(r1);
}

Assembler::Condition
MacroAssemblerARMCompat::testStringTruthy(bool truthy, const ValueOperand& value)
{
    Register string = value.payloadReg();
    ma_dtr(IsLoad, string, Imm32(JSString::offsetOfLength()), ScratchRegister);
    ma_cmp(ScratchRegister, Imm32(0));
    return truthy ? Assembler::NotEqual : Assembler::Equal;
}

void
MacroAssemblerARMCompat::floor(FloatRegister input, Register output, Label* bail)
{
    Label handleZero;
    Label handleNeg;
    Label fin;
    compareDouble(input, NoVFPRegister);
    ma_b(&handleZero, Assembler::Equal);
    ma_b(&handleNeg, Assembler::Signed);
    // NaN is always a bail condition, just bail directly.
    ma_b(bail, Assembler::Overflow);

    // The argument is a positive number, truncation is the path to glory. Since
    // it is known to be > 0.0, explicitly convert to a larger range, then a
    // value that rounds to INT_MAX is explicitly different from an argument
    // that clamps to INT_MAX.
    ma_vcvt_F64_U32(input, ScratchDoubleReg.uintOverlay());
    ma_vxfer(ScratchDoubleReg.uintOverlay(), output);
    ma_mov(output, output, SetCond);
    ma_b(bail, Signed);
    ma_b(&fin);

    bind(&handleZero);
    // Move the top word of the double into the output reg, if it is non-zero,
    // then the original value was -0.0.
    as_vxfer(output, InvalidReg, input, FloatToCore, Always, 1);
    ma_cmp(output, Imm32(0));
    ma_b(bail, NonZero);
    ma_b(&fin);

    bind(&handleNeg);
    // Negative case, negate, then start dancing.
    ma_vneg(input, input);
    ma_vcvt_F64_U32(input, ScratchDoubleReg.uintOverlay());
    ma_vxfer(ScratchDoubleReg.uintOverlay(), output);
    ma_vcvt_U32_F64(ScratchDoubleReg.uintOverlay(), ScratchDoubleReg);
    compareDouble(ScratchDoubleReg, input);
    ma_add(output, Imm32(1), output, NoSetCond, NotEqual);
    // Negate the output. Since INT_MIN < -INT_MAX, even after adding 1, the
    // result will still be a negative number.
    ma_rsb(output, Imm32(0), output, SetCond);
    // Flip the negated input back to its original value.
    ma_vneg(input, input);
    // If the result looks non-negative, then this value didn't actually fit
    // into the int range, and special handling is required. Zero is also caught
    // by this case, but floor of a negative number should never be zero.
    ma_b(bail, NotSigned);

    bind(&fin);
}

void
MacroAssemblerARMCompat::floorf(FloatRegister input, Register output, Label* bail)
{
    Label handleZero;
    Label handleNeg;
    Label fin;
    compareFloat(input, NoVFPRegister);
    ma_b(&handleZero, Assembler::Equal);
    ma_b(&handleNeg, Assembler::Signed);
    // NaN is always a bail condition, just bail directly.
    ma_b(bail, Assembler::Overflow);

    // The argument is a positive number, truncation is the path to glory; Since
    // it is known to be > 0.0, explicitly convert to a larger range, then a
    // value that rounds to INT_MAX is explicitly different from an argument
    // that clamps to INT_MAX.
    ma_vcvt_F32_U32(input, ScratchFloat32Reg.uintOverlay());
    ma_vxfer(VFPRegister(ScratchFloat32Reg).uintOverlay(), output);
    ma_mov(output, output, SetCond);
    ma_b(bail, Signed);
    ma_b(&fin);

    bind(&handleZero);
    // Move the top word of the double into the output reg, if it is non-zero,
    // then the original value was -0.0.
    as_vxfer(output, InvalidReg, VFPRegister(input).singleOverlay(), FloatToCore, Always, 0);
    ma_cmp(output, Imm32(0));
    ma_b(bail, NonZero);
    ma_b(&fin);

    bind(&handleNeg);
    // Negative case, negate, then start dancing.
    ma_vneg_f32(input, input);
    ma_vcvt_F32_U32(input, ScratchFloat32Reg.uintOverlay());
    ma_vxfer(VFPRegister(ScratchFloat32Reg).uintOverlay(), output);
    ma_vcvt_U32_F32(ScratchFloat32Reg.uintOverlay(), ScratchFloat32Reg);
    compareFloat(ScratchFloat32Reg, input);
    ma_add(output, Imm32(1), output, NoSetCond, NotEqual);
    // Negate the output. Since INT_MIN < -INT_MAX, even after adding 1, the
    // result will still be a negative number.
    ma_rsb(output, Imm32(0), output, SetCond);
    // Flip the negated input back to its original value.
    ma_vneg_f32(input, input);
    // If the result looks non-negative, then this value didn't actually fit
    // into the int range, and special handling is required. Zero is also caught
    // by this case, but floor of a negative number should never be zero.
    ma_b(bail, NotSigned);

    bind(&fin);
}

void
MacroAssemblerARMCompat::ceil(FloatRegister input, Register output, Label* bail)
{
    Label handleZero;
    Label handlePos;
    Label fin;

    compareDouble(input, NoVFPRegister);
    // NaN is always a bail condition, just bail directly.
    ma_b(bail, Assembler::Overflow);
    ma_b(&handleZero, Assembler::Equal);
    ma_b(&handlePos, Assembler::NotSigned);

    // We are in the ]-Inf; 0[ range
    // If we are in the ]-1; 0[ range => bailout
    ma_vimm(-1.0, ScratchDoubleReg);
    compareDouble(input, ScratchDoubleReg);
    ma_b(bail, Assembler::GreaterThan);

    // We are in the ]-Inf; -1] range: ceil(x) == -floor(-x) and floor can be
    // computed with direct truncation here (x > 0).
    ma_vneg(input, ScratchDoubleReg);
    FloatRegister ScratchUIntReg = ScratchDoubleReg.uintOverlay();
    ma_vcvt_F64_U32(ScratchDoubleReg, ScratchUIntReg);
    ma_vxfer(ScratchUIntReg, output);
    ma_neg(output, output, SetCond);
    ma_b(bail, NotSigned);
    ma_b(&fin);

    // Test for 0.0 / -0.0: if the top word of the input double is not zero,
    // then it was -0 and we need to bail out.
    bind(&handleZero);
    as_vxfer(output, InvalidReg, input, FloatToCore, Always, 1);
    ma_cmp(output, Imm32(0));
    ma_b(bail, NonZero);
    ma_b(&fin);

    // We are in the ]0; +inf] range: truncate integer values, maybe add 1 for
    // non integer values, maybe bail if overflow.
    bind(&handlePos);
    ma_vcvt_F64_U32(input, ScratchUIntReg);
    ma_vxfer(ScratchUIntReg, output);
    ma_vcvt_U32_F64(ScratchUIntReg, ScratchDoubleReg);
    compareDouble(ScratchDoubleReg, input);
    ma_add(output, Imm32(1), output, NoSetCond, NotEqual);
    // Bail out if the add overflowed or the result is non positive.
    ma_mov(output, output, SetCond);
    ma_b(bail, Signed);
    ma_b(bail, Zero);

    bind(&fin);
}

void
MacroAssemblerARMCompat::ceilf(FloatRegister input, Register output, Label* bail)
{
    Label handleZero;
    Label handlePos;
    Label fin;

    compareFloat(input, NoVFPRegister);
    // NaN is always a bail condition, just bail directly.
    ma_b(bail, Assembler::Overflow);
    ma_b(&handleZero, Assembler::Equal);
    ma_b(&handlePos, Assembler::NotSigned);

    // We are in the ]-Inf; 0[ range
    // If we are in the ]-1; 0[ range => bailout
    ma_vimm_f32(-1.f, ScratchFloat32Reg);
    compareFloat(input, ScratchFloat32Reg);
    ma_b(bail, Assembler::GreaterThan);

    // We are in the ]-Inf; -1] range: ceil(x) == -floor(-x) and floor can be
    // computed with direct truncation here (x > 0).
    ma_vneg_f32(input, ScratchFloat32Reg);
    FloatRegister ScratchUIntReg = ScratchDoubleReg.uintOverlay();
    ma_vcvt_F32_U32(ScratchFloat32Reg, ScratchUIntReg);
    ma_vxfer(ScratchUIntReg, output);
    ma_neg(output, output, SetCond);
    ma_b(bail, NotSigned);
    ma_b(&fin);

    // Test for 0.0 / -0.0: if the top word of the input double is not zero,
    // then it was -0 and we need to bail out.
    bind(&handleZero);
    as_vxfer(output, InvalidReg, VFPRegister(input).singleOverlay(), FloatToCore, Always, 0);
    ma_cmp(output, Imm32(0));
    ma_b(bail, NonZero);
    ma_b(&fin);

    // We are in the ]0; +inf] range: truncate integer values, maybe add 1 for
    // non integer values, maybe bail if overflow.
    bind(&handlePos);
    ma_vcvt_F32_U32(input, ScratchUIntReg);
    ma_vxfer(ScratchUIntReg, output);
    ma_vcvt_U32_F32(ScratchUIntReg, ScratchFloat32Reg);
    compareFloat(ScratchFloat32Reg, input);
    ma_add(output, Imm32(1), output, NoSetCond, NotEqual);
    // Bail out if the add overflowed or the result is non positive.
    ma_mov(output, output, SetCond);
    ma_b(bail, Signed);
    ma_b(bail, Zero);

    bind(&fin);
}

CodeOffsetLabel
MacroAssemblerARMCompat::toggledJump(Label* label)
{
    // Emit a B that can be toggled to a CMP. See ToggleToJmp(), ToggleToCmp().
    BufferOffset b = ma_b(label, Always);
    CodeOffsetLabel ret(b.getOffset());
    return ret;
}

CodeOffsetLabel
MacroAssemblerARMCompat::toggledCall(JitCode* target, bool enabled)
{
    BufferOffset bo = nextOffset();
    addPendingJump(bo, ImmPtr(target->raw()), Relocation::JITCODE);
    ma_movPatchable(ImmPtr(target->raw()), ScratchRegister, Always, HasMOVWT() ? L_MOVWT : L_LDR);
    if (enabled)
        ma_blx(ScratchRegister);
    else
        ma_nop();
    return CodeOffsetLabel(bo.getOffset());
}

void
MacroAssemblerARMCompat::round(FloatRegister input, Register output, Label* bail, FloatRegister tmp)
{
    Label handleZero;
    Label handleNeg;
    Label fin;
    // Do a compare based on the original value, then do most other things based
    // on the shifted value.
    ma_vcmpz(input);
    // Since we already know the sign bit, flip all numbers to be positive,
    // stored in tmp.
    ma_vabs(input, tmp);
    as_vmrs(pc);
    ma_b(&handleZero, Assembler::Equal);
    ma_b(&handleNeg, Assembler::Signed);
    // NaN is always a bail condition, just bail directly.
    ma_b(bail, Assembler::Overflow);

    // The argument is a positive number, truncation is the path to glory; Since
    // it is known to be > 0.0, explicitly convert to a larger range, then a
    // value that rounds to INT_MAX is explicitly different from an argument
    // that clamps to INT_MAX.

    // Add the biggest number less than 0.5 (not 0.5, because adding that to
    // the biggest number less than 0.5 would undesirably round up to 1), and
    // store the result into tmp.
    ma_vimm(GetBiggestNumberLessThan(0.5), ScratchDoubleReg);
    ma_vadd(ScratchDoubleReg, tmp, tmp);

    ma_vcvt_F64_U32(tmp, ScratchDoubleReg.uintOverlay());
    ma_vxfer(VFPRegister(ScratchDoubleReg).uintOverlay(), output);
    ma_mov(output, output, SetCond);
    ma_b(bail, Signed);
    ma_b(&fin);

    bind(&handleZero);
    // Move the top word of the double into the output reg, if it is non-zero,
    // then the original value was -0.0
    as_vxfer(output, InvalidReg, input, FloatToCore, Always, 1);
    ma_cmp(output, Imm32(0));
    ma_b(bail, NonZero);
    ma_b(&fin);

    bind(&handleNeg);
    // Negative case, negate, then start dancing. This number may be positive,
    // since we added 0.5.

    // Add 0.5 to negative numbers, store the result into tmp
    ma_vimm(0.5, ScratchDoubleReg);
    ma_vadd(ScratchDoubleReg, tmp, tmp);

    ma_vcvt_F64_U32(tmp, ScratchDoubleReg.uintOverlay());
    ma_vxfer(VFPRegister(ScratchDoubleReg).uintOverlay(), output);

    // -output is now a correctly rounded value, unless the original value was
    // exactly halfway between two integers, at which point, it has been rounded
    // away from zero, when it should be rounded towards \infty.
    ma_vcvt_U32_F64(ScratchDoubleReg.uintOverlay(), ScratchDoubleReg);
    compareDouble(ScratchDoubleReg, tmp);
    ma_sub(output, Imm32(1), output, NoSetCond, Equal);
    // Negate the output. Since INT_MIN < -INT_MAX, even after adding 1, the
    // result will still be a negative number.
    ma_rsb(output, Imm32(0), output, SetCond);

    // If the result looks non-negative, then this value didn't actually fit
    // into the int range, and special handling is required, or it was zero,
    // which means the result is actually -0.0 which also requires special
    // handling.
    ma_b(bail, NotSigned);

    bind(&fin);
}

void
MacroAssemblerARMCompat::roundf(FloatRegister input, Register output, Label* bail, FloatRegister tmp)
{
    Label handleZero;
    Label handleNeg;
    Label fin;

    // Do a compare based on the original value, then do most other things based
    // on the shifted value.
    compareFloat(input, NoVFPRegister);
    ma_b(&handleZero, Assembler::Equal);
    ma_b(&handleNeg, Assembler::Signed);

    // NaN is always a bail condition, just bail directly.
    ma_b(bail, Assembler::Overflow);

    // The argument is a positive number, truncation is the path to glory; Since
    // it is known to be > 0.0, explicitly convert to a larger range, then a
    // value that rounds to INT_MAX is explicitly different from an argument
    // that clamps to INT_MAX.

    // Add the biggest number less than 0.5f (not 0.5f, because adding that to
    // the biggest number less than 0.5f would undesirably round up to 1), and
    // store the result into tmp.
    ma_vimm_f32(GetBiggestNumberLessThan(0.5f), ScratchFloat32Reg);
    ma_vadd_f32(ScratchFloat32Reg, input, tmp);

    // Note: it doesn't matter whether x + .5 === x or not here, as it doesn't
    // affect the semantics of the float to unsigned conversion (in particular,
    // we are not applying any fixup after the operation).
    ma_vcvt_F32_U32(tmp, ScratchFloat32Reg.uintOverlay());
    ma_vxfer(VFPRegister(ScratchFloat32Reg).uintOverlay(), output);
    ma_mov(output, output, SetCond);
    ma_b(bail, Signed);
    ma_b(&fin);

    bind(&handleZero);

    // Move the whole float32 into the output reg, if it is non-zero, then the
    // original value was -0.0.
    as_vxfer(output, InvalidReg, input, FloatToCore, Always, 0);
    ma_cmp(output, Imm32(0));
    ma_b(bail, NonZero);
    ma_b(&fin);

    bind(&handleNeg);

    // Add 0.5 to negative numbers, storing the result into tmp.
    ma_vneg_f32(input, tmp);
    ma_vimm_f32(0.5f, ScratchFloat32Reg);
    ma_vadd_f32(tmp, ScratchFloat32Reg, ScratchFloat32Reg);

    // Adding 0.5 to a float input has chances to yield the wrong result, if
    // the input is too large. In this case, skip the -1 adjustment made below.
    compareFloat(ScratchFloat32Reg, tmp);

    // Negative case, negate, then start dancing. This number may be positive,
    // since we added 0.5.
    // /!\ The conditional jump afterwards depends on these two instructions
    //     *not* setting the status flags. They need to not change after the
    //     comparison above.
    ma_vcvt_F32_U32(ScratchFloat32Reg, tmp.uintOverlay());
    ma_vxfer(VFPRegister(tmp).uintOverlay(), output);

    Label flipSign;
    ma_b(&flipSign, Equal);

    // -output is now a correctly rounded value, unless the original value was
    // exactly halfway between two integers, at which point, it has been rounded
    // away from zero, when it should be rounded towards \infty.
    ma_vcvt_U32_F32(tmp.uintOverlay(), tmp);
    compareFloat(tmp, ScratchFloat32Reg);
    ma_sub(output, Imm32(1), output, NoSetCond, Equal);

    // Negate the output. Since INT_MIN < -INT_MAX, even after adding 1, the
    // result will still be a negative number.
    bind(&flipSign);
    ma_rsb(output, Imm32(0), output, SetCond);

    // If the result looks non-negative, then this value didn't actually fit
    // into the int range, and special handling is required, or it was zero,
    // which means the result is actually -0.0 which also requires special
    // handling.
    ma_b(bail, NotSigned);

    bind(&fin);
}

CodeOffsetJump
MacroAssemblerARMCompat::jumpWithPatch(RepatchLabel* label, Condition cond)
{
    ARMBuffer::PoolEntry pe;
    BufferOffset bo = as_BranchPool(0xdeadbeef, label, &pe, cond);
    // Fill in a new CodeOffset with both the load and the pool entry that the
    // instruction loads from.
    CodeOffsetJump ret(bo.getOffset(), pe.index());
    return ret;
}

void
MacroAssemblerARMCompat::branchPtrInNurseryRange(Condition cond, Register ptr, Register temp,
                                                 Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(ptr != temp);
    MOZ_ASSERT(ptr != secondScratchReg_);

    const Nursery& nursery = GetJitContext()->runtime->gcNursery();
    uintptr_t startChunk = nursery.start() >> Nursery::ChunkShift;

    ma_mov(Imm32(startChunk), secondScratchReg_);
    as_rsb(secondScratchReg_, secondScratchReg_, lsr(ptr, Nursery::ChunkShift));
    branch32(cond == Assembler::Equal ? Assembler::Below : Assembler::AboveOrEqual,
              secondScratchReg_, Imm32(nursery.numChunks()), label);
}

void
MacroAssemblerARMCompat::branchValueIsNurseryObject(Condition cond, ValueOperand value,
                                                    Register temp, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

    Label done;

    branchTestObject(Assembler::NotEqual, value, cond == Assembler::Equal ? &done : label);
    branchPtrInNurseryRange(cond, value.payloadReg(), temp, label);

    bind(&done);
}

namespace js {
namespace jit {

template<>
Register
MacroAssemblerARMCompat::computePointer<BaseIndex>(const BaseIndex& src, Register r)
{
    Register base = src.base;
    Register index = src.index;
    uint32_t scale = Imm32::ShiftOf(src.scale).value;
    int32_t offset = src.offset;
    as_add(r, base, lsl(index, scale));
    if (offset != 0)
        ma_add(r, Imm32(offset), r);
    return r;
}

template<>
Register
MacroAssemblerARMCompat::computePointer<Address>(const Address& src, Register r)
{
    if (src.offset == 0)
        return src.base;
    ma_add(src.base, Imm32(src.offset), r);
    return r;
}

} // namespace jit
} // namespace js

template<typename T>
void
MacroAssemblerARMCompat::compareExchange(int nbytes, bool signExtend, const T& mem,
                                         Register oldval, Register newval, Register output)
{
    // If LDREXB/H and STREXB/H are not available we use the
    // word-width operations with read-modify-add.  That does not
    // abstract well, so fork.
    //
    // Bug 1077321: We may further optimize for ARMv8 here.
    if (nbytes < 4 && !HasLDSTREXBHD())
        compareExchangeARMv6(nbytes, signExtend, mem, oldval, newval, output);
    else
        compareExchangeARMv7(nbytes, signExtend, mem, oldval, newval, output);
}

// General algorithm:
//
//     ...    ptr, <addr>         ; compute address of item
//     dmb
// L0  ldrex* output, [ptr]
//     sxt*   output, output, 0   ; sign-extend if applicable
//     *xt*   tmp, oldval, 0      ; sign-extend or zero-extend if applicable
//     cmp    output, tmp
//     bne    L1                  ; failed - values are different
//     strex* tmp, newval, [ptr]
//     cmp    tmp, 1
//     beq    L0                  ; failed - location is dirty, retry
// L1  dmb
//
// Discussion here:  http://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html.
// However note that that discussion uses 'isb' as the trailing fence.
// I've not quite figured out why, and I've gone with dmb here which
// is safe.  Also see the LLVM source, which uses 'dmb ish' generally.
// (Apple's Swift CPU apparently handles ish in a non-default, faster
// way.)

template<typename T>
void
MacroAssemblerARMCompat::compareExchangeARMv7(int nbytes, bool signExtend, const T& mem,
                                              Register oldval, Register newval, Register output)
{
    Label Lagain;
    Label Ldone;
    ma_dmb(BarrierST);
    Register ptr = computePointer(mem, secondScratchReg_);
    bind(&Lagain);
    switch (nbytes) {
      case 1:
        as_ldrexb(output, ptr);
        if (signExtend) {
            as_sxtb(output, output, 0);
            as_sxtb(ScratchRegister, oldval, 0);
        } else {
            as_uxtb(ScratchRegister, oldval, 0);
        }
        break;
      case 2:
        as_ldrexh(output, ptr);
        if (signExtend) {
            as_sxth(output, output, 0);
            as_sxth(ScratchRegister, oldval, 0);
        } else {
            as_uxth(ScratchRegister, oldval, 0);
        }
        break;
      case 4:
        MOZ_ASSERT(!signExtend);
        as_ldrex(output, ptr);
        break;
    }
    if (nbytes < 4)
        as_cmp(output, O2Reg(ScratchRegister));
    else
        as_cmp(output, O2Reg(oldval));
    as_b(&Ldone, NotEqual);
    switch (nbytes) {
      case 1:
        as_strexb(ScratchRegister, newval, ptr);
        break;
      case 2:
        as_strexh(ScratchRegister, newval, ptr);
        break;
      case 4:
        as_strex(ScratchRegister, newval, ptr);
        break;
    }
    as_cmp(ScratchRegister, Imm8(1));
    as_b(&Lagain, Equal);
    bind(&Ldone);
    ma_dmb();
}

template<typename T>
void
MacroAssemblerARMCompat::compareExchangeARMv6(int nbytes, bool signExtend, const T& mem,
                                              Register oldval, Register newval, Register output)
{
    // Bug 1077318: Must use read-modify-write with LDREX / STREX.
    MOZ_ASSERT(nbytes == 1 || nbytes == 2);
    MOZ_CRASH("NYI");
}

template void
js::jit::MacroAssemblerARMCompat::compareExchange(int nbytes, bool signExtend,
                                                  const Address& address, Register oldval,
                                                  Register newval, Register output);
template void
js::jit::MacroAssemblerARMCompat::compareExchange(int nbytes, bool signExtend,
                                                  const BaseIndex& address, Register oldval,
                                                  Register newval, Register output);

template<typename T>
void
MacroAssemblerARMCompat::atomicFetchOp(int nbytes, bool signExtend, AtomicOp op, const Imm32& value,
                                       const T& mem, Register temp, Register output)
{
    // The Imm32 value case is not needed yet because lowering always
    // forces the value into a register at present (bug 1077317).  But
    // the method must be present for the platform-independent code to
    // link.
    MOZ_CRASH("Feature NYI");
}

// General algorithm:
//
//     ...    ptr, <addr>         ; compute address of item
//     dmb
// L0  ldrex* output, [ptr]
//     sxt*   output, output, 0   ; sign-extend if applicable
//     OP     tmp, output, value  ; compute value to store
//     strex* tmp, tmp, [ptr]
//     cmp    tmp, 1
//     beq    L0                  ; failed - location is dirty, retry
//     dmb                        ; ordering barrier required
//
// Also see notes above at compareExchange re the barrier strategy.
//
// Observe that the value being operated into the memory element need
// not be sign-extended because no OP will make use of bits to the
// left of the bits indicated by the width of the element, and neither
// output nor the bits stored are affected by OP.

template<typename T>
void
MacroAssemblerARMCompat::atomicFetchOp(int nbytes, bool signExtend, AtomicOp op,
                                       const Register& value, const T& mem, Register temp,
                                       Register output)
{
    // Fork for non-word operations on ARMv6.
    //
    // Bug 1077321: We may further optimize for ARMv8 here.
    if (nbytes < 4 && !HasLDSTREXBHD())
        atomicFetchOpARMv6(nbytes, signExtend, op, value, mem, temp, output);
    else {
        MOZ_ASSERT(temp == InvalidReg);
        atomicFetchOpARMv7(nbytes, signExtend, op, value, mem, output);
    }
}

template<typename T>
void
MacroAssemblerARMCompat::atomicFetchOpARMv7(int nbytes, bool signExtend, AtomicOp op,
                                            const Register& value, const T& mem, Register output)
{
    Label Lagain;
    Register ptr = computePointer(mem, secondScratchReg_);
    ma_dmb();
    bind(&Lagain);
    switch (nbytes) {
      case 1:
        as_ldrexb(output, ptr);
        if (signExtend)
            as_sxtb(output, output, 0);
        break;
      case 2:
        as_ldrexh(output, ptr);
        if (signExtend)
            as_sxth(output, output, 0);
        break;
      case 4:
        MOZ_ASSERT(!signExtend);
        as_ldrex(output, ptr);
        break;
    }
    switch (op) {
      case AtomicFetchAddOp:
        as_add(ScratchRegister, output, O2Reg(value));
        break;
      case AtomicFetchSubOp:
        as_sub(ScratchRegister, output, O2Reg(value));
        break;
      case AtomicFetchAndOp:
        as_and(ScratchRegister, output, O2Reg(value));
        break;
      case AtomicFetchOrOp:
        as_orr(ScratchRegister, output, O2Reg(value));
        break;
      case AtomicFetchXorOp:
        as_eor(ScratchRegister, output, O2Reg(value));
        break;
    }
    switch (nbytes) {
      case 1:
        as_strexb(ScratchRegister, ScratchRegister, ptr);
        break;
      case 2:
        as_strexh(ScratchRegister, ScratchRegister, ptr);
        break;
      case 4:
        as_strex(ScratchRegister, ScratchRegister, ptr);
        break;
    }
    as_cmp(ScratchRegister, Imm8(1));
    as_b(&Lagain, Equal);
    ma_dmb();
}

template<typename T>
void
MacroAssemblerARMCompat::atomicFetchOpARMv6(int nbytes, bool signExtend, AtomicOp op,
                                            const Register& value, const T& mem, Register temp,
                                            Register output)
{
    // Bug 1077318: Must use read-modify-write with LDREX / STREX.
    MOZ_ASSERT(nbytes == 1 || nbytes == 2);
    MOZ_CRASH("NYI");
}

template void
js::jit::MacroAssemblerARMCompat::atomicFetchOp(int nbytes, bool signExtend, AtomicOp op,
                                                const Imm32& value, const Address& mem,
                                                Register temp, Register output);
template void
js::jit::MacroAssemblerARMCompat::atomicFetchOp(int nbytes, bool signExtend, AtomicOp op,
                                                const Imm32& value, const BaseIndex& mem,
                                                Register temp, Register output);
template void
js::jit::MacroAssemblerARMCompat::atomicFetchOp(int nbytes, bool signExtend, AtomicOp op,
                                                const Register& value, const Address& mem,
                                                Register temp, Register output);
template void
js::jit::MacroAssemblerARMCompat::atomicFetchOp(int nbytes, bool signExtend, AtomicOp op,
                                                const Register& value, const BaseIndex& mem,
                                                Register temp, Register output);

void
MacroAssemblerARMCompat::profilerEnterFrame(Register framePtr, Register scratch)
{
    AbsoluteAddress activation(GetJitContext()->runtime->addressOfProfilingActivation());
    loadPtr(activation, scratch);
    storePtr(framePtr, Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
    storePtr(ImmPtr(nullptr), Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void
MacroAssemblerARMCompat::profilerExitFrame()
{
    branch(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
}
