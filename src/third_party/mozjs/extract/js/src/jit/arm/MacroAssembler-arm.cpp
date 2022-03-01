/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/MacroAssembler-arm.h"

#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"

#include "jsmath.h"

#include "jit/arm/Simulator-arm.h"
#include "jit/AtomicOp.h"
#include "jit/AtomicOperations.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/MacroAssembler.h"
#include "jit/MoveEmitter.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "util/Memory.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace jit;

using mozilla::Abs;
using mozilla::BitwiseCast;
using mozilla::DebugOnly;
using mozilla::IsPositiveZero;
using mozilla::Maybe;

bool isValueDTRDCandidate(ValueOperand& val) {
  // In order to be used for a DTRD memory function, the two target registers
  // need to be a) Adjacent, with the tag larger than the payload, and b)
  // Aligned to a multiple of two.
  if ((val.typeReg().code() != (val.payloadReg().code() + 1))) {
    return false;
  }
  if ((val.payloadReg().code() & 1) != 0) {
    return false;
  }
  return true;
}

void MacroAssemblerARM::convertBoolToInt32(Register source, Register dest) {
  // Note that C++ bool is only 1 byte, so zero extend it to clear the
  // higher-order bits.
  as_and(dest, source, Imm8(0xff));
}

void MacroAssemblerARM::convertInt32ToDouble(Register src,
                                             FloatRegister dest_) {
  // Direct conversions aren't possible.
  VFPRegister dest = VFPRegister(dest_);
  as_vxfer(src, InvalidReg, dest.sintOverlay(), CoreToFloat);
  as_vcvt(dest, dest.sintOverlay());
}

void MacroAssemblerARM::convertInt32ToDouble(const Address& src,
                                             FloatRegister dest) {
  ScratchDoubleScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_vldr(src, scratch, scratch2);
  as_vcvt(dest, VFPRegister(scratch).sintOverlay());
}

void MacroAssemblerARM::convertInt32ToDouble(const BaseIndex& src,
                                             FloatRegister dest) {
  Register base = src.base;
  uint32_t scale = Imm32::ShiftOf(src.scale).value;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (src.offset != 0) {
    ma_add(base, Imm32(src.offset), scratch, scratch2);
    base = scratch;
  }
  ma_ldr(DTRAddr(base, DtrRegImmShift(src.index, LSL, scale)), scratch);
  convertInt32ToDouble(scratch, dest);
}

void MacroAssemblerARM::convertUInt32ToDouble(Register src,
                                              FloatRegister dest_) {
  // Direct conversions aren't possible.
  VFPRegister dest = VFPRegister(dest_);
  as_vxfer(src, InvalidReg, dest.uintOverlay(), CoreToFloat);
  as_vcvt(dest, dest.uintOverlay());
}

static const double TO_DOUBLE_HIGH_SCALE = 0x100000000;

void MacroAssemblerARM::convertUInt32ToFloat32(Register src,
                                               FloatRegister dest_) {
  // Direct conversions aren't possible.
  VFPRegister dest = VFPRegister(dest_);
  as_vxfer(src, InvalidReg, dest.uintOverlay(), CoreToFloat);
  as_vcvt(VFPRegister(dest).singleOverlay(), dest.uintOverlay());
}

void MacroAssemblerARM::convertDoubleToFloat32(FloatRegister src,
                                               FloatRegister dest,
                                               Condition c) {
  as_vcvt(VFPRegister(dest).singleOverlay(), VFPRegister(src), false, c);
}

// Checks whether a double is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void MacroAssemblerARM::convertDoubleToInt32(FloatRegister src, Register dest,
                                             Label* fail,
                                             bool negativeZeroCheck) {
  // Convert the floating point value to an integer, if it did not fit, then
  // when we convert it *back* to a float, it will have a different value,
  // which we can test.
  ScratchDoubleScope scratchDouble(asMasm());
  ScratchRegisterScope scratch(asMasm());

  FloatRegister scratchSIntReg = scratchDouble.sintOverlay();

  ma_vcvt_F64_I32(src, scratchSIntReg);
  // Move the value into the dest register.
  ma_vxfer(scratchSIntReg, dest);
  ma_vcvt_I32_F64(scratchSIntReg, scratchDouble);
  ma_vcmp(src, scratchDouble);
  as_vmrs(pc);
  ma_b(fail, Assembler::VFP_NotEqualOrUnordered);

  if (negativeZeroCheck) {
    as_cmp(dest, Imm8(0));
    // Test and bail for -0.0, when integer result is 0. Move the top word
    // of the double into the output reg, if it is non-zero, then the
    // original value was -0.0.
    as_vxfer(dest, InvalidReg, src, FloatToCore, Assembler::Equal, 1);
    ma_cmp(dest, Imm32(0x80000000), scratch, Assembler::Equal);
    ma_b(fail, Assembler::Equal);
  }
}

// Checks whether a float32 is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void MacroAssemblerARM::convertFloat32ToInt32(FloatRegister src, Register dest,
                                              Label* fail,
                                              bool negativeZeroCheck) {
  // Converting the floating point value to an integer and then converting it
  // back to a float32 would not work, as float to int32 conversions are
  // clamping (e.g. float(INT32_MAX + 1) would get converted into INT32_MAX
  // and then back to float(INT32_MAX + 1)).  If this ever happens, we just
  // bail out.
  ScratchFloat32Scope scratchFloat(asMasm());
  ScratchRegisterScope scratch(asMasm());

  FloatRegister ScratchSIntReg = scratchFloat.sintOverlay();
  ma_vcvt_F32_I32(src, ScratchSIntReg);

  // Store the result
  ma_vxfer(ScratchSIntReg, dest);

  ma_vcvt_I32_F32(ScratchSIntReg, scratchFloat);
  ma_vcmp(src, scratchFloat);
  as_vmrs(pc);
  ma_b(fail, Assembler::VFP_NotEqualOrUnordered);

  // Bail out in the clamped cases.
  ma_cmp(dest, Imm32(0x7fffffff), scratch);
  ma_cmp(dest, Imm32(0x80000000), scratch, Assembler::NotEqual);
  ma_b(fail, Assembler::Equal);

  if (negativeZeroCheck) {
    as_cmp(dest, Imm8(0));
    // Test and bail for -0.0, when integer result is 0. Move the float into
    // the output reg, and if it is non-zero then the original value was
    // -0.0
    as_vxfer(dest, InvalidReg, VFPRegister(src).singleOverlay(), FloatToCore,
             Assembler::Equal, 0);
    ma_cmp(dest, Imm32(0x80000000), scratch, Assembler::Equal);
    ma_b(fail, Assembler::Equal);
  }
}

void MacroAssemblerARM::convertFloat32ToDouble(FloatRegister src,
                                               FloatRegister dest) {
  MOZ_ASSERT(dest.isDouble());
  MOZ_ASSERT(src.isSingle());
  as_vcvt(VFPRegister(dest), VFPRegister(src).singleOverlay());
}

void MacroAssemblerARM::convertInt32ToFloat32(Register src,
                                              FloatRegister dest) {
  // Direct conversions aren't possible.
  as_vxfer(src, InvalidReg, dest.sintOverlay(), CoreToFloat);
  as_vcvt(dest.singleOverlay(), dest.sintOverlay());
}

void MacroAssemblerARM::convertInt32ToFloat32(const Address& src,
                                              FloatRegister dest) {
  ScratchFloat32Scope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_vldr(src, scratch, scratch2);
  as_vcvt(dest, VFPRegister(scratch).sintOverlay());
}

bool MacroAssemblerARM::alu_dbl(Register src1, Imm32 imm, Register dest,
                                ALUOp op, SBit s, Condition c) {
  if ((s == SetCC && !condsAreSafe(op)) || !can_dbl(op)) {
    return false;
  }

  ALUOp interop = getDestVariant(op);
  Imm8::TwoImm8mData both = Imm8::EncodeTwoImms(imm.value);
  if (both.fst().invalid()) {
    return false;
  }

  // For the most part, there is no good reason to set the condition codes for
  // the first instruction. We can do better things if the second instruction
  // doesn't have a dest, such as check for overflow by doing first operation
  // don't do second operation if first operation overflowed. This preserves
  // the overflow condition code. Unfortunately, it is horribly brittle.
  as_alu(dest, src1, Operand2(both.fst()), interop, LeaveCC, c);
  as_alu(dest, dest, Operand2(both.snd()), op, s, c);
  return true;
}

void MacroAssemblerARM::ma_alu(Register src1, Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, ALUOp op, SBit s,
                               Condition c) {
  // ma_mov should be used for moves.
  MOZ_ASSERT(op != OpMov);
  MOZ_ASSERT(op != OpMvn);
  MOZ_ASSERT(src1 != scratch);

  // As it turns out, if you ask for a compare-like instruction you *probably*
  // want it to set condition codes.
  MOZ_ASSERT_IF(dest == InvalidReg, s == SetCC);

  // The operator gives us the ability to determine how this can be used.
  Imm8 imm8 = Imm8(imm.value);
  // One instruction: If we can encode it using an imm8m, then do so.
  if (!imm8.invalid()) {
    as_alu(dest, src1, imm8, op, s, c);
    return;
  }

  // One instruction, negated:
  Imm32 negImm = imm;
  Register negDest;
  ALUOp negOp = ALUNeg(op, dest, scratch, &negImm, &negDest);
  Imm8 negImm8 = Imm8(negImm.value);
  // 'add r1, r2, -15' can be replaced with 'sub r1, r2, 15'.
  // The dest can be replaced (InvalidReg => scratch).
  // This is useful if we wish to negate tst. tst has an invalid (aka not
  // used) dest, but its negation bic requires a dest.
  if (negOp != OpInvalid && !negImm8.invalid()) {
    as_alu(negDest, src1, negImm8, negOp, s, c);
    return;
  }

  // Start by attempting to generate a two instruction form. Some things
  // cannot be made into two-inst forms correctly. Namely, adds dest, src,
  // 0xffff. Since we want the condition codes (and don't know which ones
  // will be checked), we need to assume that the overflow flag will be
  // checked and add{,s} dest, src, 0xff00; add{,s} dest, dest, 0xff is not
  // guaranteed to set the overflof flag the same as the (theoretical) one
  // instruction variant.
  if (alu_dbl(src1, imm, dest, op, s, c)) {
    return;
  }

  // And try with its negative.
  if (negOp != OpInvalid && alu_dbl(src1, negImm, negDest, negOp, s, c)) {
    return;
  }

  ma_mov(imm, scratch, c);
  as_alu(dest, src1, O2Reg(scratch), op, s, c);
}

void MacroAssemblerARM::ma_alu(Register src1, Operand op2, Register dest,
                               ALUOp op, SBit s, Assembler::Condition c) {
  MOZ_ASSERT(op2.tag() == Operand::Tag::OP2);
  as_alu(dest, src1, op2.toOp2(), op, s, c);
}

void MacroAssemblerARM::ma_alu(Register src1, Operand2 op2, Register dest,
                               ALUOp op, SBit s, Condition c) {
  as_alu(dest, src1, op2, op, s, c);
}

void MacroAssemblerARM::ma_nop() { as_nop(); }

BufferOffset MacroAssemblerARM::ma_movPatchable(Imm32 imm_, Register dest,
                                                Assembler::Condition c) {
  int32_t imm = imm_.value;
  if (HasMOVWT()) {
    BufferOffset offset = as_movw(dest, Imm16(imm & 0xffff), c);
    as_movt(dest, Imm16(imm >> 16 & 0xffff), c);
    return offset;
  } else {
    return as_Imm32Pool(dest, imm, c);
  }
}

BufferOffset MacroAssemblerARM::ma_movPatchable(ImmPtr imm, Register dest,
                                                Assembler::Condition c) {
  return ma_movPatchable(Imm32(int32_t(imm.value)), dest, c);
}

/* static */
template <class Iter>
void MacroAssemblerARM::ma_mov_patch(Imm32 imm32, Register dest,
                                     Assembler::Condition c, RelocStyle rs,
                                     Iter iter) {
  // The current instruction must be an actual instruction,
  // not automatically-inserted boilerplate.
  MOZ_ASSERT(iter.cur());
  MOZ_ASSERT(iter.cur() == iter.maybeSkipAutomaticInstructions());

  int32_t imm = imm32.value;
  switch (rs) {
    case L_MOVWT:
      Assembler::as_movw_patch(dest, Imm16(imm & 0xffff), c, iter.cur());
      Assembler::as_movt_patch(dest, Imm16(imm >> 16 & 0xffff), c, iter.next());
      break;
    case L_LDR:
      Assembler::WritePoolEntry(iter.cur(), c, imm);
      break;
  }
}

template void MacroAssemblerARM::ma_mov_patch(Imm32 imm32, Register dest,
                                              Assembler::Condition c,
                                              RelocStyle rs,
                                              InstructionIterator iter);
template void MacroAssemblerARM::ma_mov_patch(Imm32 imm32, Register dest,
                                              Assembler::Condition c,
                                              RelocStyle rs,
                                              BufferInstructionIterator iter);

void MacroAssemblerARM::ma_mov(Register src, Register dest, SBit s,
                               Assembler::Condition c) {
  if (s == SetCC || dest != src) {
    as_mov(dest, O2Reg(src), s, c);
  }
}

void MacroAssemblerARM::ma_mov(Imm32 imm, Register dest,
                               Assembler::Condition c) {
  // Try mov with Imm8 operand.
  Imm8 imm8 = Imm8(imm.value);
  if (!imm8.invalid()) {
    as_alu(dest, InvalidReg, imm8, OpMov, LeaveCC, c);
    return;
  }

  // Try mvn with Imm8 operand.
  Imm8 negImm8 = Imm8(~imm.value);
  if (!negImm8.invalid()) {
    as_alu(dest, InvalidReg, negImm8, OpMvn, LeaveCC, c);
    return;
  }

  // Try movw/movt.
  if (HasMOVWT()) {
    // ARMv7 supports movw/movt. movw zero-extends its 16 bit argument,
    // so we can set the register this way. movt leaves the bottom 16
    // bits in tact, so we always need a movw.
    as_movw(dest, Imm16(imm.value & 0xffff), c);
    if (uint32_t(imm.value) >> 16) {
      as_movt(dest, Imm16(uint32_t(imm.value) >> 16), c);
    }
    return;
  }

  // If we don't have movw/movt, we need a load.
  as_Imm32Pool(dest, imm.value, c);
}

void MacroAssemblerARM::ma_mov(ImmWord imm, Register dest,
                               Assembler::Condition c) {
  ma_mov(Imm32(imm.value), dest, c);
}

void MacroAssemblerARM::ma_mov(ImmGCPtr ptr, Register dest) {
  BufferOffset offset =
      ma_movPatchable(Imm32(uintptr_t(ptr.value)), dest, Always);
  writeDataRelocation(offset, ptr);
}

// Shifts (just a move with a shifting op2)
void MacroAssemblerARM::ma_lsl(Imm32 shift, Register src, Register dst) {
  as_mov(dst, lsl(src, shift.value));
}

void MacroAssemblerARM::ma_lsr(Imm32 shift, Register src, Register dst) {
  as_mov(dst, lsr(src, shift.value));
}

void MacroAssemblerARM::ma_asr(Imm32 shift, Register src, Register dst) {
  as_mov(dst, asr(src, shift.value));
}

void MacroAssemblerARM::ma_ror(Imm32 shift, Register src, Register dst) {
  as_mov(dst, ror(src, shift.value));
}

void MacroAssemblerARM::ma_rol(Imm32 shift, Register src, Register dst) {
  as_mov(dst, rol(src, shift.value));
}

// Shifts (just a move with a shifting op2)
void MacroAssemblerARM::ma_lsl(Register shift, Register src, Register dst) {
  as_mov(dst, lsl(src, shift));
}

void MacroAssemblerARM::ma_lsr(Register shift, Register src, Register dst) {
  as_mov(dst, lsr(src, shift));
}

void MacroAssemblerARM::ma_asr(Register shift, Register src, Register dst) {
  as_mov(dst, asr(src, shift));
}

void MacroAssemblerARM::ma_ror(Register shift, Register src, Register dst) {
  as_mov(dst, ror(src, shift));
}

void MacroAssemblerARM::ma_rol(Register shift, Register src, Register dst,
                               AutoRegisterScope& scratch) {
  as_rsb(scratch, shift, Imm8(32));
  as_mov(dst, ror(src, scratch));
}

// Move not (dest <- ~src)
void MacroAssemblerARM::ma_mvn(Register src1, Register dest, SBit s,
                               Assembler::Condition c) {
  as_alu(dest, InvalidReg, O2Reg(src1), OpMvn, s, c);
}

// Negate (dest <- -src), src is a register, rather than a general op2.
void MacroAssemblerARM::ma_neg(Register src1, Register dest, SBit s,
                               Assembler::Condition c) {
  as_rsb(dest, src1, Imm8(0), s, c);
}

void MacroAssemblerARM::ma_neg(Register64 src, Register64 dest) {
  as_rsb(dest.low, src.low, Imm8(0), SetCC);
  as_rsc(dest.high, src.high, Imm8(0));
}

// And.
void MacroAssemblerARM::ma_and(Register src, Register dest, SBit s,
                               Assembler::Condition c) {
  ma_and(dest, src, dest);
}

void MacroAssemblerARM::ma_and(Register src1, Register src2, Register dest,
                               SBit s, Assembler::Condition c) {
  as_and(dest, src1, O2Reg(src2), s, c);
}

void MacroAssemblerARM::ma_and(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Assembler::Condition c) {
  ma_alu(dest, imm, dest, scratch, OpAnd, s, c);
}

void MacroAssemblerARM::ma_and(Imm32 imm, Register src1, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Assembler::Condition c) {
  ma_alu(src1, imm, dest, scratch, OpAnd, s, c);
}

// Bit clear (dest <- dest & ~imm) or (dest <- src1 & ~src2).
void MacroAssemblerARM::ma_bic(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Assembler::Condition c) {
  ma_alu(dest, imm, dest, scratch, OpBic, s, c);
}

// Exclusive or.
void MacroAssemblerARM::ma_eor(Register src, Register dest, SBit s,
                               Assembler::Condition c) {
  ma_eor(dest, src, dest, s, c);
}

void MacroAssemblerARM::ma_eor(Register src1, Register src2, Register dest,
                               SBit s, Assembler::Condition c) {
  as_eor(dest, src1, O2Reg(src2), s, c);
}

void MacroAssemblerARM::ma_eor(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Assembler::Condition c) {
  ma_alu(dest, imm, dest, scratch, OpEor, s, c);
}

void MacroAssemblerARM::ma_eor(Imm32 imm, Register src1, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Assembler::Condition c) {
  ma_alu(src1, imm, dest, scratch, OpEor, s, c);
}

// Or.
void MacroAssemblerARM::ma_orr(Register src, Register dest, SBit s,
                               Assembler::Condition c) {
  ma_orr(dest, src, dest, s, c);
}

void MacroAssemblerARM::ma_orr(Register src1, Register src2, Register dest,
                               SBit s, Assembler::Condition c) {
  as_orr(dest, src1, O2Reg(src2), s, c);
}

void MacroAssemblerARM::ma_orr(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Assembler::Condition c) {
  ma_alu(dest, imm, dest, scratch, OpOrr, s, c);
}

void MacroAssemblerARM::ma_orr(Imm32 imm, Register src1, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Assembler::Condition c) {
  ma_alu(src1, imm, dest, scratch, OpOrr, s, c);
}

// Arithmetic-based ops.
// Add with carry.
void MacroAssemblerARM::ma_adc(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Condition c) {
  ma_alu(dest, imm, dest, scratch, OpAdc, s, c);
}

void MacroAssemblerARM::ma_adc(Register src, Register dest, SBit s,
                               Condition c) {
  as_alu(dest, dest, O2Reg(src), OpAdc, s, c);
}

void MacroAssemblerARM::ma_adc(Register src1, Register src2, Register dest,
                               SBit s, Condition c) {
  as_alu(dest, src1, O2Reg(src2), OpAdc, s, c);
}

// Add.
void MacroAssemblerARM::ma_add(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Condition c) {
  ma_alu(dest, imm, dest, scratch, OpAdd, s, c);
}

void MacroAssemblerARM::ma_add(Register src1, Register dest, SBit s,
                               Condition c) {
  ma_alu(dest, O2Reg(src1), dest, OpAdd, s, c);
}

void MacroAssemblerARM::ma_add(Register src1, Register src2, Register dest,
                               SBit s, Condition c) {
  as_alu(dest, src1, O2Reg(src2), OpAdd, s, c);
}

void MacroAssemblerARM::ma_add(Register src1, Operand op, Register dest, SBit s,
                               Condition c) {
  ma_alu(src1, op, dest, OpAdd, s, c);
}

void MacroAssemblerARM::ma_add(Register src1, Imm32 op, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Condition c) {
  ma_alu(src1, op, dest, scratch, OpAdd, s, c);
}

// Subtract with carry.
void MacroAssemblerARM::ma_sbc(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Condition c) {
  ma_alu(dest, imm, dest, scratch, OpSbc, s, c);
}

void MacroAssemblerARM::ma_sbc(Register src1, Register dest, SBit s,
                               Condition c) {
  as_alu(dest, dest, O2Reg(src1), OpSbc, s, c);
}

void MacroAssemblerARM::ma_sbc(Register src1, Register src2, Register dest,
                               SBit s, Condition c) {
  as_alu(dest, src1, O2Reg(src2), OpSbc, s, c);
}

// Subtract.
void MacroAssemblerARM::ma_sub(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Condition c) {
  ma_alu(dest, imm, dest, scratch, OpSub, s, c);
}

void MacroAssemblerARM::ma_sub(Register src1, Register dest, SBit s,
                               Condition c) {
  ma_alu(dest, Operand(src1), dest, OpSub, s, c);
}

void MacroAssemblerARM::ma_sub(Register src1, Register src2, Register dest,
                               SBit s, Condition c) {
  ma_alu(src1, Operand(src2), dest, OpSub, s, c);
}

void MacroAssemblerARM::ma_sub(Register src1, Operand op, Register dest, SBit s,
                               Condition c) {
  ma_alu(src1, op, dest, OpSub, s, c);
}

void MacroAssemblerARM::ma_sub(Register src1, Imm32 op, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Condition c) {
  ma_alu(src1, op, dest, scratch, OpSub, s, c);
}

// Reverse subtract.
void MacroAssemblerARM::ma_rsb(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Condition c) {
  ma_alu(dest, imm, dest, scratch, OpRsb, s, c);
}

void MacroAssemblerARM::ma_rsb(Register src1, Register dest, SBit s,
                               Condition c) {
  as_alu(dest, src1, O2Reg(dest), OpRsb, s, c);
}

void MacroAssemblerARM::ma_rsb(Register src1, Register src2, Register dest,
                               SBit s, Condition c) {
  as_alu(dest, src1, O2Reg(src2), OpRsb, s, c);
}

void MacroAssemblerARM::ma_rsb(Register src1, Imm32 op2, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Condition c) {
  ma_alu(src1, op2, dest, scratch, OpRsb, s, c);
}

// Reverse subtract with carry.
void MacroAssemblerARM::ma_rsc(Imm32 imm, Register dest,
                               AutoRegisterScope& scratch, SBit s,
                               Condition c) {
  ma_alu(dest, imm, dest, scratch, OpRsc, s, c);
}

void MacroAssemblerARM::ma_rsc(Register src1, Register dest, SBit s,
                               Condition c) {
  as_alu(dest, dest, O2Reg(src1), OpRsc, s, c);
}

void MacroAssemblerARM::ma_rsc(Register src1, Register src2, Register dest,
                               SBit s, Condition c) {
  as_alu(dest, src1, O2Reg(src2), OpRsc, s, c);
}

// Compares/tests.
// Compare negative (sets condition codes as src1 + src2 would).
void MacroAssemblerARM::ma_cmn(Register src1, Imm32 imm,
                               AutoRegisterScope& scratch, Condition c) {
  ma_alu(src1, imm, InvalidReg, scratch, OpCmn, SetCC, c);
}

void MacroAssemblerARM::ma_cmn(Register src1, Register src2, Condition c) {
  as_alu(InvalidReg, src2, O2Reg(src1), OpCmn, SetCC, c);
}

void MacroAssemblerARM::ma_cmn(Register src1, Operand op, Condition c) {
  MOZ_CRASH("Feature NYI");
}

// Compare (src - src2).
void MacroAssemblerARM::ma_cmp(Register src1, Imm32 imm,
                               AutoRegisterScope& scratch, Condition c) {
  ma_alu(src1, imm, InvalidReg, scratch, OpCmp, SetCC, c);
}

void MacroAssemblerARM::ma_cmp(Register src1, ImmTag tag, Condition c) {
  // ImmTag comparisons can always be done without use of a scratch register.
  Imm8 negtag = Imm8(-tag.value);
  MOZ_ASSERT(!negtag.invalid());
  as_cmn(src1, negtag, c);
}

void MacroAssemblerARM::ma_cmp(Register src1, ImmWord ptr,
                               AutoRegisterScope& scratch, Condition c) {
  ma_cmp(src1, Imm32(ptr.value), scratch, c);
}

void MacroAssemblerARM::ma_cmp(Register src1, ImmGCPtr ptr,
                               AutoRegisterScope& scratch, Condition c) {
  ma_mov(ptr, scratch);
  ma_cmp(src1, scratch, c);
}

void MacroAssemblerARM::ma_cmp(Register src1, Operand op,
                               AutoRegisterScope& scratch,
                               AutoRegisterScope& scratch2, Condition c) {
  switch (op.tag()) {
    case Operand::Tag::OP2:
      as_cmp(src1, op.toOp2(), c);
      break;
    case Operand::Tag::MEM:
      ma_ldr(op.toAddress(), scratch, scratch2);
      as_cmp(src1, O2Reg(scratch), c);
      break;
    default:
      MOZ_CRASH("trying to compare FP and integer registers");
  }
}

void MacroAssemblerARM::ma_cmp(Register src1, Register src2, Condition c) {
  as_cmp(src1, O2Reg(src2), c);
}

// Test for equality, (src1 ^ src2).
void MacroAssemblerARM::ma_teq(Register src1, Imm32 imm,
                               AutoRegisterScope& scratch, Condition c) {
  ma_alu(src1, imm, InvalidReg, scratch, OpTeq, SetCC, c);
}

void MacroAssemblerARM::ma_teq(Register src1, Register src2, Condition c) {
  as_tst(src1, O2Reg(src2), c);
}

void MacroAssemblerARM::ma_teq(Register src1, Operand op, Condition c) {
  as_teq(src1, op.toOp2(), c);
}

// Test (src1 & src2).
void MacroAssemblerARM::ma_tst(Register src1, Imm32 imm,
                               AutoRegisterScope& scratch, Condition c) {
  ma_alu(src1, imm, InvalidReg, scratch, OpTst, SetCC, c);
}

void MacroAssemblerARM::ma_tst(Register src1, Register src2, Condition c) {
  as_tst(src1, O2Reg(src2), c);
}

void MacroAssemblerARM::ma_tst(Register src1, Operand op, Condition c) {
  as_tst(src1, op.toOp2(), c);
}

void MacroAssemblerARM::ma_mul(Register src1, Register src2, Register dest) {
  as_mul(dest, src1, src2);
}

void MacroAssemblerARM::ma_mul(Register src1, Imm32 imm, Register dest,
                               AutoRegisterScope& scratch) {
  ma_mov(imm, scratch);
  as_mul(dest, src1, scratch);
}

Assembler::Condition MacroAssemblerARM::ma_check_mul(Register src1,
                                                     Register src2,
                                                     Register dest,
                                                     AutoRegisterScope& scratch,
                                                     Condition cond) {
  // TODO: this operation is illegal on armv6 and earlier
  // if src2 == scratch or src2 == dest.
  if (cond == Equal || cond == NotEqual) {
    as_smull(scratch, dest, src1, src2, SetCC);
    return cond;
  }

  if (cond == Overflow) {
    as_smull(scratch, dest, src1, src2);
    as_cmp(scratch, asr(dest, 31));
    return NotEqual;
  }

  MOZ_CRASH("Condition NYI");
}

Assembler::Condition MacroAssemblerARM::ma_check_mul(Register src1, Imm32 imm,
                                                     Register dest,
                                                     AutoRegisterScope& scratch,
                                                     Condition cond) {
  ma_mov(imm, scratch);

  if (cond == Equal || cond == NotEqual) {
    as_smull(scratch, dest, scratch, src1, SetCC);
    return cond;
  }

  if (cond == Overflow) {
    as_smull(scratch, dest, scratch, src1);
    as_cmp(scratch, asr(dest, 31));
    return NotEqual;
  }

  MOZ_CRASH("Condition NYI");
}

void MacroAssemblerARM::ma_umull(Register src1, Imm32 imm, Register destHigh,
                                 Register destLow, AutoRegisterScope& scratch) {
  ma_mov(imm, scratch);
  as_umull(destHigh, destLow, src1, scratch);
}

void MacroAssemblerARM::ma_umull(Register src1, Register src2,
                                 Register destHigh, Register destLow) {
  as_umull(destHigh, destLow, src1, src2);
}

void MacroAssemblerARM::ma_mod_mask(Register src, Register dest, Register hold,
                                    Register tmp, AutoRegisterScope& scratch,
                                    AutoRegisterScope& scratch2,
                                    int32_t shift) {
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
  // scratch reg holds the remaining bits that have not been processed lr
  // serves as a temporary location to store extracted bits into as well as
  // holding the trial subtraction as a temp value dest is the accumulator
  // (and holds the final result)
  //
  // Move the whole value into tmp, setting the codition codes so we can muck
  // with them later.
  as_mov(tmp, O2Reg(src), SetCC);
  // Zero out the dest.
  ma_mov(Imm32(0), dest);
  // Set the hold appropriately.
  ma_mov(Imm32(1), hold);
  ma_mov(Imm32(-1), hold, Signed);
  as_rsb(tmp, tmp, Imm8(0), SetCC, Signed);

  // Begin the main loop.
  bind(&head);
  {
    // Extract the bottom bits.
    ma_and(Imm32(mask), tmp, scratch, scratch2);
    // Add those bits to the accumulator.
    ma_add(scratch, dest, dest);
    // Do a trial subtraction, this is the same operation as cmp, but we store
    // the dest.
    ma_sub(dest, Imm32(mask), scratch, scratch2, SetCC);
    // If (sum - C) > 0, store sum - C back into sum, thus performing a modulus.
    ma_mov(scratch, dest, LeaveCC, NotSigned);
    // Get rid of the bits that we extracted before, and set the condition
    // codes.
    as_mov(tmp, lsr(tmp, shift), SetCC);
    // If the shift produced zero, finish, otherwise, continue in the loop.
    ma_b(&head, NonZero);
  }

  // Check the hold to see if we need to negate the result. Hold can only be
  // 1 or -1, so this will never set the 0 flag.
  as_cmp(hold, Imm8(0));
  // If the hold was non-zero, negate the result to be in line with what JS
  // wants this will set the condition codes if we try to negate.
  as_rsb(dest, dest, Imm8(0), SetCC, Signed);
  // Since the Zero flag is not set by the compare, we can *only* set the Zero
  // flag in the rsb, so Zero is set iff we negated zero (e.g. the result of
  // the computation was -0.0).
}

void MacroAssemblerARM::ma_smod(Register num, Register div, Register dest,
                                AutoRegisterScope& scratch) {
  as_sdiv(scratch, num, div);
  as_mls(dest, num, scratch, div);
}

void MacroAssemblerARM::ma_umod(Register num, Register div, Register dest,
                                AutoRegisterScope& scratch) {
  as_udiv(scratch, num, div);
  as_mls(dest, num, scratch, div);
}

// Division
void MacroAssemblerARM::ma_sdiv(Register num, Register div, Register dest,
                                Condition cond) {
  as_sdiv(dest, num, div, cond);
}

void MacroAssemblerARM::ma_udiv(Register num, Register div, Register dest,
                                Condition cond) {
  as_udiv(dest, num, div, cond);
}

// Miscellaneous instructions.
void MacroAssemblerARM::ma_clz(Register src, Register dest, Condition cond) {
  as_clz(dest, src, cond);
}

void MacroAssemblerARM::ma_ctz(Register src, Register dest,
                               AutoRegisterScope& scratch) {
  // int c = __clz(a & -a);
  // return a ? 31 - c : c;
  as_rsb(scratch, src, Imm8(0), SetCC);
  as_and(dest, src, O2Reg(scratch), LeaveCC);
  as_clz(dest, dest);
  as_rsb(dest, dest, Imm8(0x1F), LeaveCC, Assembler::NotEqual);
}

// Memory.
// Shortcut for when we know we're transferring 32 bits of data.
void MacroAssemblerARM::ma_dtr(LoadStore ls, Register rn, Imm32 offset,
                               Register rt, AutoRegisterScope& scratch,
                               Index mode, Assembler::Condition cc) {
  ma_dataTransferN(ls, 32, true, rn, offset, rt, scratch, mode, cc);
}

void MacroAssemblerARM::ma_dtr(LoadStore ls, Register rt, const Address& addr,
                               AutoRegisterScope& scratch, Index mode,
                               Condition cc) {
  ma_dataTransferN(ls, 32, true, addr.base, Imm32(addr.offset), rt, scratch,
                   mode, cc);
}

void MacroAssemblerARM::ma_str(Register rt, DTRAddr addr, Index mode,
                               Condition cc) {
  as_dtr(IsStore, 32, mode, rt, addr, cc);
}

void MacroAssemblerARM::ma_str(Register rt, const Address& addr,
                               AutoRegisterScope& scratch, Index mode,
                               Condition cc) {
  ma_dtr(IsStore, rt, addr, scratch, mode, cc);
}

void MacroAssemblerARM::ma_strd(Register rt, DebugOnly<Register> rt2,
                                EDtrAddr addr, Index mode, Condition cc) {
  MOZ_ASSERT((rt.code() & 1) == 0);
  MOZ_ASSERT(rt2.value.code() == rt.code() + 1);
  as_extdtr(IsStore, 64, true, mode, rt, addr, cc);
}

void MacroAssemblerARM::ma_ldr(DTRAddr addr, Register rt, Index mode,
                               Condition cc) {
  as_dtr(IsLoad, 32, mode, rt, addr, cc);
}

void MacroAssemblerARM::ma_ldr(const Address& addr, Register rt,
                               AutoRegisterScope& scratch, Index mode,
                               Condition cc) {
  ma_dtr(IsLoad, rt, addr, scratch, mode, cc);
}

void MacroAssemblerARM::ma_ldrb(DTRAddr addr, Register rt, Index mode,
                                Condition cc) {
  as_dtr(IsLoad, 8, mode, rt, addr, cc);
}

void MacroAssemblerARM::ma_ldrsh(EDtrAddr addr, Register rt, Index mode,
                                 Condition cc) {
  as_extdtr(IsLoad, 16, true, mode, rt, addr, cc);
}

void MacroAssemblerARM::ma_ldrh(EDtrAddr addr, Register rt, Index mode,
                                Condition cc) {
  as_extdtr(IsLoad, 16, false, mode, rt, addr, cc);
}

void MacroAssemblerARM::ma_ldrsb(EDtrAddr addr, Register rt, Index mode,
                                 Condition cc) {
  as_extdtr(IsLoad, 8, true, mode, rt, addr, cc);
}

void MacroAssemblerARM::ma_ldrd(EDtrAddr addr, Register rt,
                                DebugOnly<Register> rt2, Index mode,
                                Condition cc) {
  MOZ_ASSERT((rt.code() & 1) == 0);
  MOZ_ASSERT(rt2.value.code() == rt.code() + 1);
  MOZ_ASSERT(addr.maybeOffsetRegister() !=
             rt);  // Undefined behavior if rm == rt/rt2.
  MOZ_ASSERT(addr.maybeOffsetRegister() != rt2);
  as_extdtr(IsLoad, 64, true, mode, rt, addr, cc);
}

void MacroAssemblerARM::ma_strh(Register rt, EDtrAddr addr, Index mode,
                                Condition cc) {
  as_extdtr(IsStore, 16, false, mode, rt, addr, cc);
}

void MacroAssemblerARM::ma_strb(Register rt, DTRAddr addr, Index mode,
                                Condition cc) {
  as_dtr(IsStore, 8, mode, rt, addr, cc);
}

// Specialty for moving N bits of data, where n == 8,16,32,64.
BufferOffset MacroAssemblerARM::ma_dataTransferN(
    LoadStore ls, int size, bool IsSigned, Register rn, Register rm,
    Register rt, AutoRegisterScope& scratch, Index mode,
    Assembler::Condition cc, Scale scale) {
  MOZ_ASSERT(size == 8 || size == 16 || size == 32 || size == 64);

  if (size == 32 || (size == 8 && !IsSigned)) {
    return as_dtr(ls, size, mode, rt,
                  DTRAddr(rn, DtrRegImmShift(rm, LSL, scale)), cc);
  }

  if (scale != TimesOne) {
    ma_lsl(Imm32(scale), rm, scratch);
    rm = scratch;
  }

  return as_extdtr(ls, size, IsSigned, mode, rt, EDtrAddr(rn, EDtrOffReg(rm)),
                   cc);
}

// No scratch register is required if scale is TimesOne.
BufferOffset MacroAssemblerARM::ma_dataTransferN(LoadStore ls, int size,
                                                 bool IsSigned, Register rn,
                                                 Register rm, Register rt,
                                                 Index mode,
                                                 Assembler::Condition cc) {
  MOZ_ASSERT(size == 8 || size == 16 || size == 32 || size == 64);
  if (size == 32 || (size == 8 && !IsSigned)) {
    return as_dtr(ls, size, mode, rt,
                  DTRAddr(rn, DtrRegImmShift(rm, LSL, TimesOne)), cc);
  }
  return as_extdtr(ls, size, IsSigned, mode, rt, EDtrAddr(rn, EDtrOffReg(rm)),
                   cc);
}

BufferOffset MacroAssemblerARM::ma_dataTransferN(LoadStore ls, int size,
                                                 bool IsSigned, Register rn,
                                                 Imm32 offset, Register rt,
                                                 AutoRegisterScope& scratch,
                                                 Index mode,
                                                 Assembler::Condition cc) {
  MOZ_ASSERT(!(ls == IsLoad && mode == PostIndex && rt == pc),
             "Large-offset PostIndex loading into PC requires special logic: "
             "see ma_popn_pc().");

  int off = offset.value;

  // We can encode this as a standard ldr.
  if (size == 32 || (size == 8 && !IsSigned)) {
    if (off < 4096 && off > -4096) {
      // This encodes as a single instruction, Emulating mode's behavior
      // in a multi-instruction sequence is not necessary.
      return as_dtr(ls, size, mode, rt, DTRAddr(rn, DtrOffImm(off)), cc);
    }

    // We cannot encode this offset in a single ldr. For mode == index,
    // try to encode it as |add scratch, base, imm; ldr dest, [scratch,
    // +offset]|. This does not wark for mode == PreIndex or mode == PostIndex.
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

    int bottom = off & 0xfff;
    int neg_bottom = 0x1000 - bottom;

    MOZ_ASSERT(rn != scratch);
    MOZ_ASSERT(mode != PostIndex);

    // At this point, both off - bottom and off + neg_bottom will be
    // reasonable-ish quantities.
    //
    // Note a neg_bottom of 0x1000 can not be encoded as an immediate
    // negative offset in the instruction and this occurs when bottom is
    // zero, so this case is guarded against below.
    if (off < 0) {
      Operand2 sub_off = Imm8(-(off - bottom));  // sub_off = bottom - off
      if (!sub_off.invalid()) {
        // - sub_off = off - bottom
        as_sub(scratch, rn, sub_off, LeaveCC, cc);
        return as_dtr(ls, size, Offset, rt, DTRAddr(scratch, DtrOffImm(bottom)),
                      cc);
      }

      // sub_off = -neg_bottom - off
      sub_off = Imm8(-(off + neg_bottom));
      if (!sub_off.invalid() && bottom != 0) {
        // Guarded against by: bottom != 0
        MOZ_ASSERT(neg_bottom < 0x1000);
        // - sub_off = neg_bottom + off
        as_sub(scratch, rn, sub_off, LeaveCC, cc);
        return as_dtr(ls, size, Offset, rt,
                      DTRAddr(scratch, DtrOffImm(-neg_bottom)), cc);
      }
    } else {
      // sub_off = off - bottom
      Operand2 sub_off = Imm8(off - bottom);
      if (!sub_off.invalid()) {
        //  sub_off = off - bottom
        as_add(scratch, rn, sub_off, LeaveCC, cc);
        return as_dtr(ls, size, Offset, rt, DTRAddr(scratch, DtrOffImm(bottom)),
                      cc);
      }

      // sub_off = neg_bottom + off
      sub_off = Imm8(off + neg_bottom);
      if (!sub_off.invalid() && bottom != 0) {
        // Guarded against by: bottom != 0
        MOZ_ASSERT(neg_bottom < 0x1000);
        // sub_off = neg_bottom + off
        as_add(scratch, rn, sub_off, LeaveCC, cc);
        return as_dtr(ls, size, Offset, rt,
                      DTRAddr(scratch, DtrOffImm(-neg_bottom)), cc);
      }
    }

    ma_mov(offset, scratch);
    return as_dtr(ls, size, mode, rt,
                  DTRAddr(rn, DtrRegImmShift(scratch, LSL, 0)));
  } else {
    // Should attempt to use the extended load/store instructions.
    if (off < 256 && off > -256) {
      return as_extdtr(ls, size, IsSigned, mode, rt,
                       EDtrAddr(rn, EDtrOffImm(off)), cc);
    }

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
      if (!sub_off.invalid()) {
        // - sub_off = off - bottom
        as_sub(scratch, rn, sub_off, LeaveCC, cc);
        return as_extdtr(ls, size, IsSigned, Offset, rt,
                         EDtrAddr(scratch, EDtrOffImm(bottom)), cc);
      }
      // sub_off = -neg_bottom - off
      sub_off = Imm8(-(off + neg_bottom));
      if (!sub_off.invalid() && bottom != 0) {
        // Guarded against by: bottom != 0
        MOZ_ASSERT(neg_bottom < 0x100);
        // - sub_off = neg_bottom + off
        as_sub(scratch, rn, sub_off, LeaveCC, cc);
        return as_extdtr(ls, size, IsSigned, Offset, rt,
                         EDtrAddr(scratch, EDtrOffImm(-neg_bottom)), cc);
      }
    } else {
      // sub_off = off - bottom
      Operand2 sub_off = Imm8(off - bottom);
      if (!sub_off.invalid()) {
        // sub_off = off - bottom
        as_add(scratch, rn, sub_off, LeaveCC, cc);
        return as_extdtr(ls, size, IsSigned, Offset, rt,
                         EDtrAddr(scratch, EDtrOffImm(bottom)), cc);
      }
      // sub_off = neg_bottom + off
      sub_off = Imm8(off + neg_bottom);
      if (!sub_off.invalid() && bottom != 0) {
        // Guarded against by: bottom != 0
        MOZ_ASSERT(neg_bottom < 0x100);
        // sub_off = neg_bottom + off
        as_add(scratch, rn, sub_off, LeaveCC, cc);
        return as_extdtr(ls, size, IsSigned, Offset, rt,
                         EDtrAddr(scratch, EDtrOffImm(-neg_bottom)), cc);
      }
    }
    ma_mov(offset, scratch);
    return as_extdtr(ls, size, IsSigned, mode, rt,
                     EDtrAddr(rn, EDtrOffReg(scratch)), cc);
  }
}

void MacroAssemblerARM::ma_pop(Register r) {
  as_dtr(IsLoad, 32, PostIndex, r, DTRAddr(sp, DtrOffImm(4)));
}

void MacroAssemblerARM::ma_popn_pc(Imm32 n, AutoRegisterScope& scratch,
                                   AutoRegisterScope& scratch2) {
  // pc <- [sp]; sp += n
  int32_t nv = n.value;

  if (nv < 4096 && nv >= -4096) {
    as_dtr(IsLoad, 32, PostIndex, pc, DTRAddr(sp, DtrOffImm(nv)));
  } else {
    ma_mov(sp, scratch);
    ma_add(Imm32(n), sp, scratch2);
    as_dtr(IsLoad, 32, Offset, pc, DTRAddr(scratch, DtrOffImm(0)));
  }
}

void MacroAssemblerARM::ma_push(Register r) {
  MOZ_ASSERT(r != sp, "Use ma_push_sp().");
  as_dtr(IsStore, 32, PreIndex, r, DTRAddr(sp, DtrOffImm(-4)));
}

void MacroAssemblerARM::ma_push_sp(Register r, AutoRegisterScope& scratch) {
  // Pushing sp is not well-defined: use two instructions.
  MOZ_ASSERT(r == sp);
  ma_mov(sp, scratch);
  as_dtr(IsStore, 32, PreIndex, scratch, DTRAddr(sp, DtrOffImm(-4)));
}

void MacroAssemblerARM::ma_vpop(VFPRegister r) {
  startFloatTransferM(IsLoad, sp, IA, WriteBack);
  transferFloatReg(r);
  finishFloatTransfer();
}

void MacroAssemblerARM::ma_vpush(VFPRegister r) {
  startFloatTransferM(IsStore, sp, DB, WriteBack);
  transferFloatReg(r);
  finishFloatTransfer();
}

// Barriers
void MacroAssemblerARM::ma_dmb(BarrierOption option) {
  if (HasDMBDSBISB()) {
    as_dmb(option);
  } else {
    as_dmb_trap();
  }
}

void MacroAssemblerARM::ma_dsb(BarrierOption option) {
  if (HasDMBDSBISB()) {
    as_dsb(option);
  } else {
    as_dsb_trap();
  }
}

// Branches when done from within arm-specific code.
BufferOffset MacroAssemblerARM::ma_b(Label* dest, Assembler::Condition c) {
  return as_b(dest, c);
}

void MacroAssemblerARM::ma_bx(Register dest, Assembler::Condition c) {
  as_bx(dest, c);
}

void MacroAssemblerARM::ma_b(void* target, Assembler::Condition c) {
  // An immediate pool is used for easier patching.
  as_Imm32Pool(pc, uint32_t(target), c);
}

// This is almost NEVER necessary: we'll basically never be calling a label,
// except possibly in the crazy bailout-table case.
void MacroAssemblerARM::ma_bl(Label* dest, Assembler::Condition c) {
  as_bl(dest, c);
}

void MacroAssemblerARM::ma_blx(Register reg, Assembler::Condition c) {
  as_blx(reg, c);
}

// VFP/ALU
void MacroAssemblerARM::ma_vadd(FloatRegister src1, FloatRegister src2,
                                FloatRegister dst) {
  as_vadd(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void MacroAssemblerARM::ma_vadd_f32(FloatRegister src1, FloatRegister src2,
                                    FloatRegister dst) {
  as_vadd(VFPRegister(dst).singleOverlay(), VFPRegister(src1).singleOverlay(),
          VFPRegister(src2).singleOverlay());
}

void MacroAssemblerARM::ma_vsub(FloatRegister src1, FloatRegister src2,
                                FloatRegister dst) {
  as_vsub(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void MacroAssemblerARM::ma_vsub_f32(FloatRegister src1, FloatRegister src2,
                                    FloatRegister dst) {
  as_vsub(VFPRegister(dst).singleOverlay(), VFPRegister(src1).singleOverlay(),
          VFPRegister(src2).singleOverlay());
}

void MacroAssemblerARM::ma_vmul(FloatRegister src1, FloatRegister src2,
                                FloatRegister dst) {
  as_vmul(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void MacroAssemblerARM::ma_vmul_f32(FloatRegister src1, FloatRegister src2,
                                    FloatRegister dst) {
  as_vmul(VFPRegister(dst).singleOverlay(), VFPRegister(src1).singleOverlay(),
          VFPRegister(src2).singleOverlay());
}

void MacroAssemblerARM::ma_vdiv(FloatRegister src1, FloatRegister src2,
                                FloatRegister dst) {
  as_vdiv(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void MacroAssemblerARM::ma_vdiv_f32(FloatRegister src1, FloatRegister src2,
                                    FloatRegister dst) {
  as_vdiv(VFPRegister(dst).singleOverlay(), VFPRegister(src1).singleOverlay(),
          VFPRegister(src2).singleOverlay());
}

void MacroAssemblerARM::ma_vmov(FloatRegister src, FloatRegister dest,
                                Condition cc) {
  as_vmov(dest, src, cc);
}

void MacroAssemblerARM::ma_vmov_f32(FloatRegister src, FloatRegister dest,
                                    Condition cc) {
  as_vmov(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay(),
          cc);
}

void MacroAssemblerARM::ma_vneg(FloatRegister src, FloatRegister dest,
                                Condition cc) {
  as_vneg(dest, src, cc);
}

void MacroAssemblerARM::ma_vneg_f32(FloatRegister src, FloatRegister dest,
                                    Condition cc) {
  as_vneg(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay(),
          cc);
}

void MacroAssemblerARM::ma_vabs(FloatRegister src, FloatRegister dest,
                                Condition cc) {
  as_vabs(dest, src, cc);
}

void MacroAssemblerARM::ma_vabs_f32(FloatRegister src, FloatRegister dest,
                                    Condition cc) {
  as_vabs(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay(),
          cc);
}

void MacroAssemblerARM::ma_vsqrt(FloatRegister src, FloatRegister dest,
                                 Condition cc) {
  as_vsqrt(dest, src, cc);
}

void MacroAssemblerARM::ma_vsqrt_f32(FloatRegister src, FloatRegister dest,
                                     Condition cc) {
  as_vsqrt(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay(),
           cc);
}

static inline uint32_t DoubleHighWord(double d) {
  return static_cast<uint32_t>(BitwiseCast<uint64_t>(d) >> 32);
}

static inline uint32_t DoubleLowWord(double d) {
  return static_cast<uint32_t>(BitwiseCast<uint64_t>(d)) & uint32_t(0xffffffff);
}

void MacroAssemblerARM::ma_vimm(double value, FloatRegister dest,
                                Condition cc) {
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

void MacroAssemblerARM::ma_vimm_f32(float value, FloatRegister dest,
                                    Condition cc) {
  VFPRegister vd = VFPRegister(dest).singleOverlay();
  if (HasVFPv3()) {
    if (IsPositiveZero(value)) {
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
    double doubleValue(value);
    if (DoubleLowWord(doubleValue) == 0) {
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

void MacroAssemblerARM::ma_vcmp(FloatRegister src1, FloatRegister src2,
                                Condition cc) {
  as_vcmp(VFPRegister(src1), VFPRegister(src2), cc);
}

void MacroAssemblerARM::ma_vcmp_f32(FloatRegister src1, FloatRegister src2,
                                    Condition cc) {
  as_vcmp(VFPRegister(src1).singleOverlay(), VFPRegister(src2).singleOverlay(),
          cc);
}

void MacroAssemblerARM::ma_vcmpz(FloatRegister src1, Condition cc) {
  as_vcmpz(VFPRegister(src1), cc);
}

void MacroAssemblerARM::ma_vcmpz_f32(FloatRegister src1, Condition cc) {
  as_vcmpz(VFPRegister(src1).singleOverlay(), cc);
}

void MacroAssemblerARM::ma_vcvt_F64_I32(FloatRegister src, FloatRegister dest,
                                        Condition cc) {
  MOZ_ASSERT(src.isDouble());
  MOZ_ASSERT(dest.isSInt());
  as_vcvt(dest, src, false, cc);
}

void MacroAssemblerARM::ma_vcvt_F64_U32(FloatRegister src, FloatRegister dest,
                                        Condition cc) {
  MOZ_ASSERT(src.isDouble());
  MOZ_ASSERT(dest.isUInt());
  as_vcvt(dest, src, false, cc);
}

void MacroAssemblerARM::ma_vcvt_I32_F64(FloatRegister src, FloatRegister dest,
                                        Condition cc) {
  MOZ_ASSERT(src.isSInt());
  MOZ_ASSERT(dest.isDouble());
  as_vcvt(dest, src, false, cc);
}

void MacroAssemblerARM::ma_vcvt_U32_F64(FloatRegister src, FloatRegister dest,
                                        Condition cc) {
  MOZ_ASSERT(src.isUInt());
  MOZ_ASSERT(dest.isDouble());
  as_vcvt(dest, src, false, cc);
}

void MacroAssemblerARM::ma_vcvt_F32_I32(FloatRegister src, FloatRegister dest,
                                        Condition cc) {
  MOZ_ASSERT(src.isSingle());
  MOZ_ASSERT(dest.isSInt());
  as_vcvt(VFPRegister(dest).sintOverlay(), VFPRegister(src).singleOverlay(),
          false, cc);
}

void MacroAssemblerARM::ma_vcvt_F32_U32(FloatRegister src, FloatRegister dest,
                                        Condition cc) {
  MOZ_ASSERT(src.isSingle());
  MOZ_ASSERT(dest.isUInt());
  as_vcvt(VFPRegister(dest).uintOverlay(), VFPRegister(src).singleOverlay(),
          false, cc);
}

void MacroAssemblerARM::ma_vcvt_I32_F32(FloatRegister src, FloatRegister dest,
                                        Condition cc) {
  MOZ_ASSERT(src.isSInt());
  MOZ_ASSERT(dest.isSingle());
  as_vcvt(VFPRegister(dest).singleOverlay(), VFPRegister(src).sintOverlay(),
          false, cc);
}

void MacroAssemblerARM::ma_vcvt_U32_F32(FloatRegister src, FloatRegister dest,
                                        Condition cc) {
  MOZ_ASSERT(src.isUInt());
  MOZ_ASSERT(dest.isSingle());
  as_vcvt(VFPRegister(dest).singleOverlay(), VFPRegister(src).uintOverlay(),
          false, cc);
}

void MacroAssemblerARM::ma_vxfer(FloatRegister src, Register dest,
                                 Condition cc) {
  as_vxfer(dest, InvalidReg, VFPRegister(src).singleOverlay(), FloatToCore, cc);
}

void MacroAssemblerARM::ma_vxfer(FloatRegister src, Register dest1,
                                 Register dest2, Condition cc) {
  as_vxfer(dest1, dest2, VFPRegister(src), FloatToCore, cc);
}

void MacroAssemblerARM::ma_vxfer(Register src, FloatRegister dest,
                                 Condition cc) {
  as_vxfer(src, InvalidReg, VFPRegister(dest).singleOverlay(), CoreToFloat, cc);
}

void MacroAssemblerARM::ma_vxfer(Register src1, Register src2,
                                 FloatRegister dest, Condition cc) {
  as_vxfer(src1, src2, VFPRegister(dest), CoreToFloat, cc);
}

BufferOffset MacroAssemblerARM::ma_vdtr(LoadStore ls, const Address& addr,
                                        VFPRegister rt,
                                        AutoRegisterScope& scratch,
                                        Condition cc) {
  int off = addr.offset;
  MOZ_ASSERT((off & 3) == 0);
  Register base = addr.base;
  if (off > -1024 && off < 1024) {
    return as_vdtr(ls, rt, Operand(addr).toVFPAddr(), cc);
  }

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
    if (!sub_off.invalid()) {
      // - sub_off = off - bottom
      as_sub(scratch, base, sub_off, LeaveCC, cc);
      return as_vdtr(ls, rt, VFPAddr(scratch, VFPOffImm(bottom)), cc);
    }
    // sub_off = -neg_bottom - off
    sub_off = Imm8(-(off + neg_bottom));
    if (!sub_off.invalid() && bottom != 0) {
      // Guarded against by: bottom != 0
      MOZ_ASSERT(neg_bottom < 0x400);
      // - sub_off = neg_bottom + off
      as_sub(scratch, base, sub_off, LeaveCC, cc);
      return as_vdtr(ls, rt, VFPAddr(scratch, VFPOffImm(-neg_bottom)), cc);
    }
  } else {
    // sub_off = off - bottom
    Operand2 sub_off = Imm8(off - bottom);
    if (!sub_off.invalid()) {
      // sub_off = off - bottom
      as_add(scratch, base, sub_off, LeaveCC, cc);
      return as_vdtr(ls, rt, VFPAddr(scratch, VFPOffImm(bottom)), cc);
    }
    // sub_off = neg_bottom + off
    sub_off = Imm8(off + neg_bottom);
    if (!sub_off.invalid() && bottom != 0) {
      // Guarded against by: bottom != 0
      MOZ_ASSERT(neg_bottom < 0x400);
      // sub_off = neg_bottom + off
      as_add(scratch, base, sub_off, LeaveCC, cc);
      return as_vdtr(ls, rt, VFPAddr(scratch, VFPOffImm(-neg_bottom)), cc);
    }
  }

  // Safe to use scratch as dest, since ma_add() overwrites dest at the end
  // and can't use it as internal scratch since it may also == base.
  ma_add(base, Imm32(off), scratch, scratch, LeaveCC, cc);
  return as_vdtr(ls, rt, VFPAddr(scratch, VFPOffImm(0)), cc);
}

BufferOffset MacroAssemblerARM::ma_vldr(VFPAddr addr, VFPRegister dest,
                                        Condition cc) {
  return as_vdtr(IsLoad, dest, addr, cc);
}

BufferOffset MacroAssemblerARM::ma_vldr(const Address& addr, VFPRegister dest,
                                        AutoRegisterScope& scratch,
                                        Condition cc) {
  return ma_vdtr(IsLoad, addr, dest, scratch, cc);
}

BufferOffset MacroAssemblerARM::ma_vldr(VFPRegister src, Register base,
                                        Register index,
                                        AutoRegisterScope& scratch,
                                        int32_t shift, Condition cc) {
  as_add(scratch, base, lsl(index, shift), LeaveCC, cc);
  return as_vdtr(IsLoad, src, Operand(Address(scratch, 0)).toVFPAddr(), cc);
}

BufferOffset MacroAssemblerARM::ma_vstr(VFPRegister src, VFPAddr addr,
                                        Condition cc) {
  return as_vdtr(IsStore, src, addr, cc);
}

BufferOffset MacroAssemblerARM::ma_vstr(VFPRegister src, const Address& addr,
                                        AutoRegisterScope& scratch,
                                        Condition cc) {
  return ma_vdtr(IsStore, addr, src, scratch, cc);
}

BufferOffset MacroAssemblerARM::ma_vstr(
    VFPRegister src, Register base, Register index, AutoRegisterScope& scratch,
    AutoRegisterScope& scratch2, int32_t shift, int32_t offset, Condition cc) {
  as_add(scratch, base, lsl(index, shift), LeaveCC, cc);
  return ma_vstr(src, Address(scratch, offset), scratch2, cc);
}

// Without an offset, no second scratch register is necessary.
BufferOffset MacroAssemblerARM::ma_vstr(VFPRegister src, Register base,
                                        Register index,
                                        AutoRegisterScope& scratch,
                                        int32_t shift, Condition cc) {
  as_add(scratch, base, lsl(index, shift), LeaveCC, cc);
  return as_vdtr(IsStore, src, Operand(Address(scratch, 0)).toVFPAddr(), cc);
}

bool MacroAssemblerARMCompat::buildOOLFakeExitFrame(void* fakeReturnAddr) {
  uint32_t descriptor = MakeFrameDescriptor(
      asMasm().framePushed(), FrameType::IonJS, ExitFrameLayout::Size());

  asMasm().Push(Imm32(descriptor));  // descriptor_
  asMasm().Push(ImmPtr(fakeReturnAddr));

  return true;
}

void MacroAssemblerARMCompat::move32(Imm32 imm, Register dest) {
  ma_mov(imm, dest);
}

void MacroAssemblerARMCompat::move32(Register src, Register dest) {
  ma_mov(src, dest);
}

void MacroAssemblerARMCompat::movePtr(Register src, Register dest) {
  ma_mov(src, dest);
}

void MacroAssemblerARMCompat::movePtr(ImmWord imm, Register dest) {
  ma_mov(Imm32(imm.value), dest);
}

void MacroAssemblerARMCompat::movePtr(ImmGCPtr imm, Register dest) {
  ma_mov(imm, dest);
}

void MacroAssemblerARMCompat::movePtr(ImmPtr imm, Register dest) {
  movePtr(ImmWord(uintptr_t(imm.value)), dest);
}

void MacroAssemblerARMCompat::movePtr(wasm::SymbolicAddress imm,
                                      Register dest) {
  append(wasm::SymbolicAccess(CodeOffset(currentOffset()), imm));
  ma_movPatchable(Imm32(-1), dest, Always);
}

void MacroAssemblerARMCompat::load8ZeroExtend(const Address& address,
                                              Register dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_dataTransferN(IsLoad, 8, false, address.base, Imm32(address.offset), dest,
                   scratch);
}

void MacroAssemblerARMCompat::load8ZeroExtend(const BaseIndex& src,
                                              Register dest) {
  Register base = src.base;
  uint32_t scale = Imm32::ShiftOf(src.scale).value;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (src.offset == 0) {
    ma_ldrb(DTRAddr(base, DtrRegImmShift(src.index, LSL, scale)), dest);
  } else {
    ma_add(base, Imm32(src.offset), scratch, scratch2);
    ma_ldrb(DTRAddr(scratch, DtrRegImmShift(src.index, LSL, scale)), dest);
  }
}

void MacroAssemblerARMCompat::load8SignExtend(const Address& address,
                                              Register dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_dataTransferN(IsLoad, 8, true, address.base, Imm32(address.offset), dest,
                   scratch);
}

void MacroAssemblerARMCompat::load8SignExtend(const BaseIndex& src,
                                              Register dest) {
  Register index = src.index;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  // ARMv7 does not have LSL on an index register with an extended load.
  if (src.scale != TimesOne) {
    ma_lsl(Imm32::ShiftOf(src.scale), index, scratch);
    index = scratch;
  }

  if (src.offset != 0) {
    if (index != scratch) {
      ma_mov(index, scratch);
      index = scratch;
    }
    ma_add(Imm32(src.offset), index, scratch2);
  }
  ma_ldrsb(EDtrAddr(src.base, EDtrOffReg(index)), dest);
}

void MacroAssemblerARMCompat::load16ZeroExtend(const Address& address,
                                               Register dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_dataTransferN(IsLoad, 16, false, address.base, Imm32(address.offset), dest,
                   scratch);
}

void MacroAssemblerARMCompat::load16ZeroExtend(const BaseIndex& src,
                                               Register dest) {
  Register index = src.index;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  // ARMv7 does not have LSL on an index register with an extended load.
  if (src.scale != TimesOne) {
    ma_lsl(Imm32::ShiftOf(src.scale), index, scratch);
    index = scratch;
  }

  if (src.offset != 0) {
    if (index != scratch) {
      ma_mov(index, scratch);
      index = scratch;
    }
    ma_add(Imm32(src.offset), index, scratch2);
  }
  ma_ldrh(EDtrAddr(src.base, EDtrOffReg(index)), dest);
}

void MacroAssemblerARMCompat::load16SignExtend(const Address& address,
                                               Register dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_dataTransferN(IsLoad, 16, true, address.base, Imm32(address.offset), dest,
                   scratch);
}

void MacroAssemblerARMCompat::load16SignExtend(const BaseIndex& src,
                                               Register dest) {
  Register index = src.index;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  // We don't have LSL on index register yet.
  if (src.scale != TimesOne) {
    ma_lsl(Imm32::ShiftOf(src.scale), index, scratch);
    index = scratch;
  }

  if (src.offset != 0) {
    if (index != scratch) {
      ma_mov(index, scratch);
      index = scratch;
    }
    ma_add(Imm32(src.offset), index, scratch2);
  }
  ma_ldrsh(EDtrAddr(src.base, EDtrOffReg(index)), dest);
}

void MacroAssemblerARMCompat::load32(const Address& address, Register dest) {
  loadPtr(address, dest);
}

void MacroAssemblerARMCompat::load32(const BaseIndex& address, Register dest) {
  loadPtr(address, dest);
}

void MacroAssemblerARMCompat::load32(AbsoluteAddress address, Register dest) {
  loadPtr(address, dest);
}

void MacroAssemblerARMCompat::loadPtr(const Address& address, Register dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_ldr(address, dest, scratch);
}

void MacroAssemblerARMCompat::loadPtr(const BaseIndex& src, Register dest) {
  Register base = src.base;
  uint32_t scale = Imm32::ShiftOf(src.scale).value;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (src.offset != 0) {
    ma_add(base, Imm32(src.offset), scratch, scratch2);
    ma_ldr(DTRAddr(scratch, DtrRegImmShift(src.index, LSL, scale)), dest);
  } else {
    ma_ldr(DTRAddr(base, DtrRegImmShift(src.index, LSL, scale)), dest);
  }
}

void MacroAssemblerARMCompat::loadPtr(AbsoluteAddress address, Register dest) {
  MOZ_ASSERT(dest != pc);  // Use dest as a scratch register.
  movePtr(ImmWord(uintptr_t(address.addr)), dest);
  loadPtr(Address(dest, 0), dest);
}

void MacroAssemblerARMCompat::loadPtr(wasm::SymbolicAddress address,
                                      Register dest) {
  MOZ_ASSERT(dest != pc);  // Use dest as a scratch register.
  movePtr(address, dest);
  loadPtr(Address(dest, 0), dest);
}

void MacroAssemblerARMCompat::loadPrivate(const Address& address,
                                          Register dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_ldr(ToPayload(address), dest, scratch);
}

void MacroAssemblerARMCompat::loadDouble(const Address& address,
                                         FloatRegister dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_vldr(address, dest, scratch);
}

void MacroAssemblerARMCompat::loadDouble(const BaseIndex& src,
                                         FloatRegister dest) {
  // VFP instructions don't even support register Base + register Index modes,
  // so just add the index, then handle the offset like normal.
  Register base = src.base;
  Register index = src.index;
  uint32_t scale = Imm32::ShiftOf(src.scale).value;
  int32_t offset = src.offset;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  as_add(scratch, base, lsl(index, scale));
  ma_vldr(Address(scratch, offset), dest, scratch2);
}

void MacroAssemblerARMCompat::loadFloatAsDouble(const Address& address,
                                                FloatRegister dest) {
  ScratchRegisterScope scratch(asMasm());

  VFPRegister rt = dest;
  ma_vldr(address, rt.singleOverlay(), scratch);
  as_vcvt(rt, rt.singleOverlay());
}

void MacroAssemblerARMCompat::loadFloatAsDouble(const BaseIndex& src,
                                                FloatRegister dest) {
  // VFP instructions don't even support register Base + register Index modes,
  // so just add the index, then handle the offset like normal.
  Register base = src.base;
  Register index = src.index;
  uint32_t scale = Imm32::ShiftOf(src.scale).value;
  int32_t offset = src.offset;
  VFPRegister rt = dest;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  as_add(scratch, base, lsl(index, scale));
  ma_vldr(Address(scratch, offset), rt.singleOverlay(), scratch2);
  as_vcvt(rt, rt.singleOverlay());
}

void MacroAssemblerARMCompat::loadFloat32(const Address& address,
                                          FloatRegister dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_vldr(address, VFPRegister(dest).singleOverlay(), scratch);
}

void MacroAssemblerARMCompat::loadFloat32(const BaseIndex& src,
                                          FloatRegister dest) {
  // VFP instructions don't even support register Base + register Index modes,
  // so just add the index, then handle the offset like normal.
  Register base = src.base;
  Register index = src.index;
  uint32_t scale = Imm32::ShiftOf(src.scale).value;
  int32_t offset = src.offset;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  as_add(scratch, base, lsl(index, scale));
  ma_vldr(Address(scratch, offset), VFPRegister(dest).singleOverlay(),
          scratch2);
}

void MacroAssemblerARMCompat::store8(Imm32 imm, const Address& address) {
  SecondScratchRegisterScope scratch2(asMasm());
  ma_mov(imm, scratch2);
  store8(scratch2, address);
}

void MacroAssemblerARMCompat::store8(Register src, const Address& address) {
  ScratchRegisterScope scratch(asMasm());
  ma_dataTransferN(IsStore, 8, false, address.base, Imm32(address.offset), src,
                   scratch);
}

void MacroAssemblerARMCompat::store8(Imm32 imm, const BaseIndex& dest) {
  Register base = dest.base;
  uint32_t scale = Imm32::ShiftOf(dest.scale).value;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (dest.offset != 0) {
    ma_add(base, Imm32(dest.offset), scratch, scratch2);
    ma_mov(imm, scratch2);
    ma_strb(scratch2, DTRAddr(scratch, DtrRegImmShift(dest.index, LSL, scale)));
  } else {
    ma_mov(imm, scratch2);
    ma_strb(scratch2, DTRAddr(base, DtrRegImmShift(dest.index, LSL, scale)));
  }
}

void MacroAssemblerARMCompat::store8(Register src, const BaseIndex& dest) {
  Register base = dest.base;
  uint32_t scale = Imm32::ShiftOf(dest.scale).value;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (dest.offset != 0) {
    ma_add(base, Imm32(dest.offset), scratch, scratch2);
    ma_strb(src, DTRAddr(scratch, DtrRegImmShift(dest.index, LSL, scale)));
  } else {
    ma_strb(src, DTRAddr(base, DtrRegImmShift(dest.index, LSL, scale)));
  }
}

void MacroAssemblerARMCompat::store16(Imm32 imm, const Address& address) {
  SecondScratchRegisterScope scratch2(asMasm());
  ma_mov(imm, scratch2);
  store16(scratch2, address);
}

void MacroAssemblerARMCompat::store16(Register src, const Address& address) {
  ScratchRegisterScope scratch(asMasm());
  ma_dataTransferN(IsStore, 16, false, address.base, Imm32(address.offset), src,
                   scratch);
}

void MacroAssemblerARMCompat::store16(Imm32 imm, const BaseIndex& dest) {
  Register index = dest.index;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  // We don't have LSL on index register yet.
  if (dest.scale != TimesOne) {
    ma_lsl(Imm32::ShiftOf(dest.scale), index, scratch);
    index = scratch;
  }

  if (dest.offset != 0) {
    ma_add(index, Imm32(dest.offset), scratch, scratch2);
    index = scratch;
  }

  ma_mov(imm, scratch2);
  ma_strh(scratch2, EDtrAddr(dest.base, EDtrOffReg(index)));
}

void MacroAssemblerARMCompat::store16(Register src, const BaseIndex& address) {
  Register index = address.index;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  // We don't have LSL on index register yet.
  if (address.scale != TimesOne) {
    ma_lsl(Imm32::ShiftOf(address.scale), index, scratch);
    index = scratch;
  }

  if (address.offset != 0) {
    ma_add(index, Imm32(address.offset), scratch, scratch2);
    index = scratch;
  }
  ma_strh(src, EDtrAddr(address.base, EDtrOffReg(index)));
}

void MacroAssemblerARMCompat::store32(Register src, AbsoluteAddress address) {
  storePtr(src, address);
}

void MacroAssemblerARMCompat::store32(Register src, const Address& address) {
  storePtr(src, address);
}

void MacroAssemblerARMCompat::store32(Imm32 src, const Address& address) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  move32(src, scratch);
  ma_str(scratch, address, scratch2);
}

void MacroAssemblerARMCompat::store32(Imm32 imm, const BaseIndex& dest) {
  Register base = dest.base;
  uint32_t scale = Imm32::ShiftOf(dest.scale).value;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (dest.offset != 0) {
    ma_add(base, Imm32(dest.offset), scratch, scratch2);
    ma_mov(imm, scratch2);
    ma_str(scratch2, DTRAddr(scratch, DtrRegImmShift(dest.index, LSL, scale)));
  } else {
    ma_mov(imm, scratch);
    ma_str(scratch, DTRAddr(base, DtrRegImmShift(dest.index, LSL, scale)));
  }
}

void MacroAssemblerARMCompat::store32(Register src, const BaseIndex& dest) {
  Register base = dest.base;
  uint32_t scale = Imm32::ShiftOf(dest.scale).value;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (dest.offset != 0) {
    ma_add(base, Imm32(dest.offset), scratch, scratch2);
    ma_str(src, DTRAddr(scratch, DtrRegImmShift(dest.index, LSL, scale)));
  } else {
    ma_str(src, DTRAddr(base, DtrRegImmShift(dest.index, LSL, scale)));
  }
}

void MacroAssemblerARMCompat::storePtr(ImmWord imm, const Address& address) {
  store32(Imm32(imm.value), address);
}

void MacroAssemblerARMCompat::storePtr(ImmWord imm, const BaseIndex& address) {
  store32(Imm32(imm.value), address);
}

void MacroAssemblerARMCompat::storePtr(ImmPtr imm, const Address& address) {
  store32(Imm32(uintptr_t(imm.value)), address);
}

void MacroAssemblerARMCompat::storePtr(ImmPtr imm, const BaseIndex& address) {
  store32(Imm32(uintptr_t(imm.value)), address);
}

void MacroAssemblerARMCompat::storePtr(ImmGCPtr imm, const Address& address) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_mov(imm, scratch);
  ma_str(scratch, address, scratch2);
}

void MacroAssemblerARMCompat::storePtr(ImmGCPtr imm, const BaseIndex& address) {
  Register base = address.base;
  uint32_t scale = Imm32::ShiftOf(address.scale).value;

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (address.offset != 0) {
    ma_add(base, Imm32(address.offset), scratch, scratch2);
    ma_mov(imm, scratch2);
    ma_str(scratch2,
           DTRAddr(scratch, DtrRegImmShift(address.index, LSL, scale)));
  } else {
    ma_mov(imm, scratch);
    ma_str(scratch, DTRAddr(base, DtrRegImmShift(address.index, LSL, scale)));
  }
}

void MacroAssemblerARMCompat::storePtr(Register src, const Address& address) {
  SecondScratchRegisterScope scratch2(asMasm());
  ma_str(src, address, scratch2);
}

void MacroAssemblerARMCompat::storePtr(Register src, const BaseIndex& address) {
  store32(src, address);
}

void MacroAssemblerARMCompat::storePtr(Register src, AbsoluteAddress dest) {
  ScratchRegisterScope scratch(asMasm());
  movePtr(ImmWord(uintptr_t(dest.addr)), scratch);
  ma_str(src, DTRAddr(scratch, DtrOffImm(0)));
}

// Note: this function clobbers the input register.
void MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output) {
  if (HasVFPv3()) {
    Label notSplit;
    {
      ScratchDoubleScope scratchDouble(*this);
      MOZ_ASSERT(input != scratchDouble);
      loadConstantDouble(0.5, scratchDouble);

      ma_vadd(input, scratchDouble, scratchDouble);
      // Convert the double into an unsigned fixed point value with 24 bits of
      // precision. The resulting number will look like 0xII.DDDDDD
      as_vcvtFixed(scratchDouble, false, 24, true);
    }

    // Move the fixed point value into an integer register.
    {
      ScratchFloat32Scope scratchFloat(*this);
      as_vxfer(output, InvalidReg, scratchFloat.uintOverlay(), FloatToCore);
    }

    ScratchRegisterScope scratch(*this);

    // See if this value *might* have been an exact integer after adding
    // 0.5. This tests the 1/2 through 1/16,777,216th places, but 0.5 needs
    // to be tested out to the 1/140,737,488,355,328th place.
    ma_tst(output, Imm32(0x00ffffff), scratch);
    // Convert to a uint8 by shifting out all of the fraction bits.
    ma_lsr(Imm32(24), output, output);
    // If any of the bottom 24 bits were non-zero, then we're good, since
    // this number can't be exactly XX.0
    ma_b(&notSplit, NonZero);
    as_vxfer(scratch, InvalidReg, input, FloatToCore);
    as_cmp(scratch, Imm8(0));
    // If the lower 32 bits of the double were 0, then this was an exact number,
    // and it should be even.
    as_bic(output, output, Imm8(1), LeaveCC, Zero);
    bind(&notSplit);
  } else {
    ScratchDoubleScope scratchDouble(*this);
    MOZ_ASSERT(input != scratchDouble);
    loadConstantDouble(0.5, scratchDouble);

    Label outOfRange;
    ma_vcmpz(input);
    // Do the add, in place so we can reference it later.
    ma_vadd(input, scratchDouble, input);
    // Do the conversion to an integer.
    as_vcvt(VFPRegister(scratchDouble).uintOverlay(), VFPRegister(input));
    // Copy the converted value out.
    as_vxfer(output, InvalidReg, scratchDouble, FloatToCore);
    as_vmrs(pc);
    ma_mov(Imm32(0), output, Overflow);  // NaN => 0
    ma_b(&outOfRange, Overflow);         // NaN
    as_cmp(output, Imm8(0xff));
    ma_mov(Imm32(0xff), output, Above);
    ma_b(&outOfRange, Above);
    // Convert it back to see if we got the same value back.
    as_vcvt(scratchDouble, VFPRegister(scratchDouble).uintOverlay());
    // Do the check.
    as_vcmp(scratchDouble, input);
    as_vmrs(pc);
    as_bic(output, output, Imm8(1), LeaveCC, Zero);
    bind(&outOfRange);
  }
}

void MacroAssemblerARMCompat::cmp32(Register lhs, Imm32 rhs) {
  ScratchRegisterScope scratch(asMasm());
  ma_cmp(lhs, rhs, scratch);
}

void MacroAssemblerARMCompat::cmp32(Register lhs, Register rhs) {
  ma_cmp(lhs, rhs);
}

void MacroAssemblerARMCompat::cmp32(const Address& lhs, Imm32 rhs) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_ldr(lhs, scratch, scratch2);
  ma_cmp(scratch, rhs, scratch2);
}

void MacroAssemblerARMCompat::cmp32(const Address& lhs, Register rhs) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_ldr(lhs, scratch, scratch2);
  ma_cmp(scratch, rhs);
}

void MacroAssemblerARMCompat::cmpPtr(Register lhs, ImmWord rhs) {
  cmp32(lhs, Imm32(rhs.value));
}

void MacroAssemblerARMCompat::cmpPtr(Register lhs, ImmPtr rhs) {
  cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
}

void MacroAssemblerARMCompat::cmpPtr(Register lhs, Register rhs) {
  ma_cmp(lhs, rhs);
}

void MacroAssemblerARMCompat::cmpPtr(Register lhs, ImmGCPtr rhs) {
  ScratchRegisterScope scratch(asMasm());
  ma_cmp(lhs, rhs, scratch);
}

void MacroAssemblerARMCompat::cmpPtr(Register lhs, Imm32 rhs) {
  cmp32(lhs, rhs);
}

void MacroAssemblerARMCompat::cmpPtr(const Address& lhs, Register rhs) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_ldr(lhs, scratch, scratch2);
  ma_cmp(scratch, rhs);
}

void MacroAssemblerARMCompat::cmpPtr(const Address& lhs, ImmWord rhs) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_ldr(lhs, scratch, scratch2);
  ma_cmp(scratch, Imm32(rhs.value), scratch2);
}

void MacroAssemblerARMCompat::cmpPtr(const Address& lhs, ImmPtr rhs) {
  cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
}

void MacroAssemblerARMCompat::cmpPtr(const Address& lhs, ImmGCPtr rhs) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_ldr(lhs, scratch, scratch2);
  ma_cmp(scratch, rhs, scratch2);
}

void MacroAssemblerARMCompat::cmpPtr(const Address& lhs, Imm32 rhs) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_ldr(lhs, scratch, scratch2);
  ma_cmp(scratch, rhs, scratch2);
}

void MacroAssemblerARMCompat::setStackArg(Register reg, uint32_t arg) {
  ScratchRegisterScope scratch(asMasm());
  ma_dataTransferN(IsStore, 32, true, sp, Imm32(arg * sizeof(intptr_t)), reg,
                   scratch);
}

void MacroAssemblerARMCompat::minMaxDouble(FloatRegister srcDest,
                                           FloatRegister second, bool canBeNaN,
                                           bool isMax) {
  FloatRegister first = srcDest;

  Label nan, equal, returnSecond, done;

  Assembler::Condition cond = isMax ? Assembler::VFP_LessThanOrEqual
                                    : Assembler::VFP_GreaterThanOrEqual;

  compareDouble(first, second);
  // First or second is NaN, result is NaN.
  ma_b(&nan, Assembler::VFP_Unordered);
  // Make sure we handle -0 and 0 right.
  ma_b(&equal, Assembler::VFP_Equal);
  ma_b(&returnSecond, cond);
  ma_b(&done);

  // Check for zero.
  bind(&equal);
  compareDouble(first, NoVFPRegister);
  // First wasn't 0 or -0, so just return it.
  ma_b(&done, Assembler::VFP_NotEqualOrUnordered);
  // So now both operands are either -0 or 0.
  if (isMax) {
    // -0 + -0 = -0 and -0 + 0 = 0.
    ma_vadd(second, first, first);
  } else {
    ma_vneg(first, first);
    ma_vsub(first, second, first);
    ma_vneg(first, first);
  }
  ma_b(&done);

  bind(&nan);
  // If the first argument is the NaN, return it; otherwise return the second
  // operand.
  compareDouble(first, first);
  ma_vmov(first, srcDest, Assembler::VFP_Unordered);
  ma_b(&done, Assembler::VFP_Unordered);

  bind(&returnSecond);
  ma_vmov(second, srcDest);

  bind(&done);
}

void MacroAssemblerARMCompat::minMaxFloat32(FloatRegister srcDest,
                                            FloatRegister second, bool canBeNaN,
                                            bool isMax) {
  FloatRegister first = srcDest;

  Label nan, equal, returnSecond, done;

  Assembler::Condition cond = isMax ? Assembler::VFP_LessThanOrEqual
                                    : Assembler::VFP_GreaterThanOrEqual;

  compareFloat(first, second);
  // First or second is NaN, result is NaN.
  ma_b(&nan, Assembler::VFP_Unordered);
  // Make sure we handle -0 and 0 right.
  ma_b(&equal, Assembler::VFP_Equal);
  ma_b(&returnSecond, cond);
  ma_b(&done);

  // Check for zero.
  bind(&equal);
  compareFloat(first, NoVFPRegister);
  // First wasn't 0 or -0, so just return it.
  ma_b(&done, Assembler::VFP_NotEqualOrUnordered);
  // So now both operands are either -0 or 0.
  if (isMax) {
    // -0 + -0 = -0 and -0 + 0 = 0.
    ma_vadd_f32(second, first, first);
  } else {
    ma_vneg_f32(first, first);
    ma_vsub_f32(first, second, first);
    ma_vneg_f32(first, first);
  }
  ma_b(&done);

  bind(&nan);
  // See comment in minMaxDouble.
  compareFloat(first, first);
  ma_vmov_f32(first, srcDest, Assembler::VFP_Unordered);
  ma_b(&done, Assembler::VFP_Unordered);

  bind(&returnSecond);
  ma_vmov_f32(second, srcDest);

  bind(&done);
}

void MacroAssemblerARMCompat::compareDouble(FloatRegister lhs,
                                            FloatRegister rhs) {
  // Compare the doubles, setting vector status flags.
  if (rhs.isMissing()) {
    ma_vcmpz(lhs);
  } else {
    ma_vcmp(lhs, rhs);
  }

  // Move vector status bits to normal status flags.
  as_vmrs(pc);
}

void MacroAssemblerARMCompat::compareFloat(FloatRegister lhs,
                                           FloatRegister rhs) {
  // Compare the doubles, setting vector status flags.
  if (rhs.isMissing()) {
    as_vcmpz(VFPRegister(lhs).singleOverlay());
  } else {
    as_vcmp(VFPRegister(lhs).singleOverlay(), VFPRegister(rhs).singleOverlay());
  }

  // Move vector status bits to normal status flags.
  as_vmrs(pc);
}

Assembler::Condition MacroAssemblerARMCompat::testInt32(
    Assembler::Condition cond, const ValueOperand& value) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_INT32));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testBoolean(
    Assembler::Condition cond, const ValueOperand& value) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_BOOLEAN));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testDouble(
    Assembler::Condition cond, const ValueOperand& value) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  Assembler::Condition actual = (cond == Equal) ? Below : AboveOrEqual;
  ScratchRegisterScope scratch(asMasm());
  ma_cmp(value.typeReg(), ImmTag(JSVAL_TAG_CLEAR), scratch);
  return actual;
}

Assembler::Condition MacroAssemblerARMCompat::testNull(
    Assembler::Condition cond, const ValueOperand& value) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_NULL));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testUndefined(
    Assembler::Condition cond, const ValueOperand& value) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_UNDEFINED));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testString(
    Assembler::Condition cond, const ValueOperand& value) {
  return testString(cond, value.typeReg());
}

Assembler::Condition MacroAssemblerARMCompat::testSymbol(
    Assembler::Condition cond, const ValueOperand& value) {
  return testSymbol(cond, value.typeReg());
}

Assembler::Condition MacroAssemblerARMCompat::testBigInt(
    Assembler::Condition cond, const ValueOperand& value) {
  return testBigInt(cond, value.typeReg());
}

Assembler::Condition MacroAssemblerARMCompat::testObject(
    Assembler::Condition cond, const ValueOperand& value) {
  return testObject(cond, value.typeReg());
}

Assembler::Condition MacroAssemblerARMCompat::testNumber(
    Assembler::Condition cond, const ValueOperand& value) {
  return testNumber(cond, value.typeReg());
}

Assembler::Condition MacroAssemblerARMCompat::testMagic(
    Assembler::Condition cond, const ValueOperand& value) {
  return testMagic(cond, value.typeReg());
}

Assembler::Condition MacroAssemblerARMCompat::testPrimitive(
    Assembler::Condition cond, const ValueOperand& value) {
  return testPrimitive(cond, value.typeReg());
}

Assembler::Condition MacroAssemblerARMCompat::testGCThing(
    Assembler::Condition cond, const ValueOperand& value) {
  return testGCThing(cond, value.typeReg());
}

// Register-based tests.
Assembler::Condition MacroAssemblerARMCompat::testInt32(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JSVAL_TAG_INT32));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testBoolean(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JSVAL_TAG_BOOLEAN));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testNull(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JSVAL_TAG_NULL));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testUndefined(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JSVAL_TAG_UNDEFINED));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testString(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JSVAL_TAG_STRING));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testSymbol(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JSVAL_TAG_SYMBOL));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testBigInt(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JSVAL_TAG_BIGINT));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testObject(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JSVAL_TAG_OBJECT));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testMagic(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JSVAL_TAG_MAGIC));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testPrimitive(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JS::detail::ValueUpperExclPrimitiveTag));
  return cond == Equal ? Below : AboveOrEqual;
}

Assembler::Condition MacroAssemblerARMCompat::testGCThing(
    Assembler::Condition cond, Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JS::detail::ValueLowerInclGCThingTag));
  return cond == Equal ? AboveOrEqual : Below;
}

Assembler::Condition MacroAssemblerARMCompat::testGCThing(
    Assembler::Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  ma_cmp(tag, ImmTag(JS::detail::ValueLowerInclGCThingTag));
  return cond == Equal ? AboveOrEqual : Below;
}

Assembler::Condition MacroAssemblerARMCompat::testMagic(
    Assembler::Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_MAGIC));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testInt32(
    Assembler::Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_INT32));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testDouble(
    Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  return testDouble(cond, tag);
}

Assembler::Condition MacroAssemblerARMCompat::testBoolean(
    Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  return testBoolean(cond, tag);
}

Assembler::Condition MacroAssemblerARMCompat::testNull(Condition cond,
                                                       const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  return testNull(cond, tag);
}

Assembler::Condition MacroAssemblerARMCompat::testUndefined(
    Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  return testUndefined(cond, tag);
}

Assembler::Condition MacroAssemblerARMCompat::testString(
    Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  return testString(cond, tag);
}

Assembler::Condition MacroAssemblerARMCompat::testSymbol(
    Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  return testSymbol(cond, tag);
}

Assembler::Condition MacroAssemblerARMCompat::testBigInt(
    Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  return testBigInt(cond, tag);
}

Assembler::Condition MacroAssemblerARMCompat::testObject(
    Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  return testObject(cond, tag);
}

Assembler::Condition MacroAssemblerARMCompat::testNumber(
    Condition cond, const Address& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  return testNumber(cond, tag);
}

Assembler::Condition MacroAssemblerARMCompat::testDouble(Condition cond,
                                                         Register tag) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  Condition actual = (cond == Equal) ? Below : AboveOrEqual;
  ma_cmp(tag, ImmTag(JSVAL_TAG_CLEAR));
  return actual;
}

Assembler::Condition MacroAssemblerARMCompat::testNumber(Condition cond,
                                                         Register tag) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp(tag, ImmTag(JS::detail::ValueUpperInclNumberTag));
  return cond == Equal ? BelowOrEqual : Above;
}

Assembler::Condition MacroAssemblerARMCompat::testUndefined(
    Condition cond, const BaseIndex& src) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(src, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_UNDEFINED));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testNull(Condition cond,
                                                       const BaseIndex& src) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(src, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_NULL));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testBoolean(
    Condition cond, const BaseIndex& src) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(src, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_BOOLEAN));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testString(Condition cond,
                                                         const BaseIndex& src) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(src, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_STRING));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testSymbol(Condition cond,
                                                         const BaseIndex& src) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(src, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_SYMBOL));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testBigInt(Condition cond,
                                                         const BaseIndex& src) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(src, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_BIGINT));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testInt32(Condition cond,
                                                        const BaseIndex& src) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(src, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_INT32));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testObject(Condition cond,
                                                         const BaseIndex& src) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(src, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_OBJECT));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testDouble(Condition cond,
                                                         const BaseIndex& src) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  Assembler::Condition actual = (cond == Equal) ? Below : AboveOrEqual;
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(src, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_CLEAR));
  return actual;
}

Assembler::Condition MacroAssemblerARMCompat::testMagic(
    Condition cond, const BaseIndex& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  ma_cmp(tag, ImmTag(JSVAL_TAG_MAGIC));
  return cond;
}

Assembler::Condition MacroAssemblerARMCompat::testGCThing(
    Condition cond, const BaseIndex& address) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(asMasm());
  Register tag = extractTag(address, scratch);
  ma_cmp(tag, ImmTag(JS::detail::ValueLowerInclGCThingTag));
  return cond == Equal ? AboveOrEqual : Below;
}

// Unboxing code.
void MacroAssemblerARMCompat::unboxNonDouble(const ValueOperand& operand,
                                             Register dest, JSValueType type) {
  auto movPayloadToDest = [&]() {
    if (operand.payloadReg() != dest) {
      ma_mov(operand.payloadReg(), dest, LeaveCC);
    }
  };
  if (!JitOptions.spectreValueMasking) {
    movPayloadToDest();
    return;
  }

  // Spectre mitigation: We zero the payload if the tag does not match the
  // expected type and if this is a pointer type.
  if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
    movPayloadToDest();
    return;
  }

  // We zero the destination register and move the payload into it if
  // the tag corresponds to the given type.
  ma_cmp(operand.typeReg(), ImmType(type));
  movPayloadToDest();
  ma_mov(Imm32(0), dest, NotEqual);
}

void MacroAssemblerARMCompat::unboxNonDouble(const Address& src, Register dest,
                                             JSValueType type) {
  ScratchRegisterScope scratch(asMasm());
  if (!JitOptions.spectreValueMasking) {
    ma_ldr(ToPayload(src), dest, scratch);
    return;
  }

  // Spectre mitigation: We zero the payload if the tag does not match the
  // expected type and if this is a pointer type.
  if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
    ma_ldr(ToPayload(src), dest, scratch);
    return;
  }

  // We zero the destination register and move the payload into it if
  // the tag corresponds to the given type.
  ma_ldr(ToType(src), scratch, scratch);
  ma_cmp(scratch, ImmType(type));
  ma_ldr(ToPayload(src), dest, scratch, Offset, Equal);
  ma_mov(Imm32(0), dest, NotEqual);
}

void MacroAssemblerARMCompat::unboxNonDouble(const BaseIndex& src,
                                             Register dest, JSValueType type) {
  SecondScratchRegisterScope scratch2(asMasm());
  ma_alu(src.base, lsl(src.index, src.scale), scratch2, OpAdd);
  Address value(scratch2, src.offset);
  unboxNonDouble(value, dest, type);
}

void MacroAssemblerARMCompat::unboxDouble(const ValueOperand& operand,
                                          FloatRegister dest) {
  MOZ_ASSERT(dest.isDouble());
  as_vxfer(operand.payloadReg(), operand.typeReg(), VFPRegister(dest),
           CoreToFloat);
}

void MacroAssemblerARMCompat::unboxDouble(const Address& src,
                                          FloatRegister dest) {
  MOZ_ASSERT(dest.isDouble());
  loadDouble(src, dest);
}

void MacroAssemblerARMCompat::unboxDouble(const BaseIndex& src,
                                          FloatRegister dest) {
  MOZ_ASSERT(dest.isDouble());
  loadDouble(src, dest);
}

void MacroAssemblerARMCompat::unboxValue(const ValueOperand& src,
                                         AnyRegister dest, JSValueType type) {
  if (dest.isFloat()) {
    Label notInt32, end;
    asMasm().branchTestInt32(Assembler::NotEqual, src, &notInt32);
    convertInt32ToDouble(src.payloadReg(), dest.fpu());
    ma_b(&end);
    bind(&notInt32);
    unboxDouble(src, dest.fpu());
    bind(&end);
  } else {
    unboxNonDouble(src, dest.gpr(), type);
  }
}

void MacroAssemblerARMCompat::boxDouble(FloatRegister src,
                                        const ValueOperand& dest,
                                        FloatRegister) {
  as_vxfer(dest.payloadReg(), dest.typeReg(), VFPRegister(src), FloatToCore);
}

void MacroAssemblerARMCompat::boxNonDouble(JSValueType type, Register src,
                                           const ValueOperand& dest) {
  if (src != dest.payloadReg()) {
    ma_mov(src, dest.payloadReg());
  }
  ma_mov(ImmType(type), dest.typeReg());
}

void MacroAssemblerARMCompat::boolValueToDouble(const ValueOperand& operand,
                                                FloatRegister dest) {
  VFPRegister d = VFPRegister(dest);
  loadConstantDouble(1.0, dest);
  as_cmp(operand.payloadReg(), Imm8(0));
  // If the source is 0, then subtract the dest from itself, producing 0.
  as_vsub(d, d, d, Equal);
}

void MacroAssemblerARMCompat::int32ValueToDouble(const ValueOperand& operand,
                                                 FloatRegister dest) {
  // Transfer the integral value to a floating point register.
  VFPRegister vfpdest = VFPRegister(dest);
  as_vxfer(operand.payloadReg(), InvalidReg, vfpdest.sintOverlay(),
           CoreToFloat);
  // Convert the value to a double.
  as_vcvt(vfpdest, vfpdest.sintOverlay());
}

void MacroAssemblerARMCompat::boolValueToFloat32(const ValueOperand& operand,
                                                 FloatRegister dest) {
  VFPRegister d = VFPRegister(dest).singleOverlay();
  loadConstantFloat32(1.0, dest);
  as_cmp(operand.payloadReg(), Imm8(0));
  // If the source is 0, then subtract the dest from itself, producing 0.
  as_vsub(d, d, d, Equal);
}

void MacroAssemblerARMCompat::int32ValueToFloat32(const ValueOperand& operand,
                                                  FloatRegister dest) {
  // Transfer the integral value to a floating point register.
  VFPRegister vfpdest = VFPRegister(dest).singleOverlay();
  as_vxfer(operand.payloadReg(), InvalidReg, vfpdest.sintOverlay(),
           CoreToFloat);
  // Convert the value to a float.
  as_vcvt(vfpdest, vfpdest.sintOverlay());
}

void MacroAssemblerARMCompat::loadConstantFloat32(float f, FloatRegister dest) {
  ma_vimm_f32(f, dest);
}

void MacroAssemblerARMCompat::loadInt32OrDouble(const Address& src,
                                                FloatRegister dest) {
  Label notInt32, end;

  // If it's an int, convert to a double.
  {
    ScratchRegisterScope scratch(asMasm());
    SecondScratchRegisterScope scratch2(asMasm());

    ma_ldr(ToType(src), scratch, scratch2);
    asMasm().branchTestInt32(Assembler::NotEqual, scratch, &notInt32);
    ma_ldr(ToPayload(src), scratch, scratch2);
    convertInt32ToDouble(scratch, dest);
    ma_b(&end);
  }

  // Not an int, just load as double.
  bind(&notInt32);
  {
    ScratchRegisterScope scratch(asMasm());
    ma_vldr(src, dest, scratch);
  }
  bind(&end);
}

void MacroAssemblerARMCompat::loadInt32OrDouble(Register base, Register index,
                                                FloatRegister dest,
                                                int32_t shift) {
  Label notInt32, end;

  static_assert(NUNBOX32_PAYLOAD_OFFSET == 0);

  ScratchRegisterScope scratch(asMasm());

  // If it's an int, convert it to double.
  ma_alu(base, lsl(index, shift), scratch, OpAdd);

  // Since we only have one scratch register, we need to stomp over it with
  // the tag.
  ma_ldr(DTRAddr(scratch, DtrOffImm(NUNBOX32_TYPE_OFFSET)), scratch);
  asMasm().branchTestInt32(Assembler::NotEqual, scratch, &notInt32);

  // Implicitly requires NUNBOX32_PAYLOAD_OFFSET == 0: no offset provided
  ma_ldr(DTRAddr(base, DtrRegImmShift(index, LSL, shift)), scratch);
  convertInt32ToDouble(scratch, dest);
  ma_b(&end);

  // Not an int, just load as double.
  bind(&notInt32);
  // First, recompute the offset that had been stored in the scratch register
  // since the scratch register was overwritten loading in the type.
  ma_alu(base, lsl(index, shift), scratch, OpAdd);
  ma_vldr(VFPAddr(scratch, VFPOffImm(0)), dest);
  bind(&end);
}

void MacroAssemblerARMCompat::loadConstantDouble(double dp,
                                                 FloatRegister dest) {
  ma_vimm(dp, dest);
}

// Treat the value as a boolean, and set condition codes accordingly.
Assembler::Condition MacroAssemblerARMCompat::testInt32Truthy(
    bool truthy, const ValueOperand& operand) {
  ma_tst(operand.payloadReg(), operand.payloadReg());
  return truthy ? NonZero : Zero;
}

Assembler::Condition MacroAssemblerARMCompat::testBooleanTruthy(
    bool truthy, const ValueOperand& operand) {
  ma_tst(operand.payloadReg(), operand.payloadReg());
  return truthy ? NonZero : Zero;
}

Assembler::Condition MacroAssemblerARMCompat::testDoubleTruthy(
    bool truthy, FloatRegister reg) {
  as_vcmpz(VFPRegister(reg));
  as_vmrs(pc);
  as_cmp(r0, O2Reg(r0), Overflow);
  return truthy ? NonZero : Zero;
}

Register MacroAssemblerARMCompat::extractObject(const Address& address,
                                                Register scratch) {
  SecondScratchRegisterScope scratch2(asMasm());
  ma_ldr(ToPayload(address), scratch, scratch2);
  return scratch;
}

Register MacroAssemblerARMCompat::extractTag(const Address& address,
                                             Register scratch) {
  SecondScratchRegisterScope scratch2(asMasm());
  ma_ldr(ToType(address), scratch, scratch2);
  return scratch;
}

Register MacroAssemblerARMCompat::extractTag(const BaseIndex& address,
                                             Register scratch) {
  ma_alu(address.base, lsl(address.index, address.scale), scratch, OpAdd,
         LeaveCC);
  return extractTag(Address(scratch, address.offset), scratch);
}

/////////////////////////////////////////////////////////////////
// X86/X64-common (ARM too now) interface.
/////////////////////////////////////////////////////////////////
void MacroAssemblerARMCompat::storeValue(ValueOperand val, const Address& dst) {
  SecondScratchRegisterScope scratch2(asMasm());
  ma_str(val.payloadReg(), ToPayload(dst), scratch2);
  ma_str(val.typeReg(), ToType(dst), scratch2);
}

void MacroAssemblerARMCompat::storeValue(ValueOperand val,
                                         const BaseIndex& dest) {
  ScratchRegisterScope scratch(asMasm());

  if (isValueDTRDCandidate(val) && Abs(dest.offset) <= 255) {
    Register tmpIdx;
    if (dest.offset == 0) {
      if (dest.scale == TimesOne) {
        tmpIdx = dest.index;
      } else {
        ma_lsl(Imm32(dest.scale), dest.index, scratch);
        tmpIdx = scratch;
      }
      ma_strd(val.payloadReg(), val.typeReg(),
              EDtrAddr(dest.base, EDtrOffReg(tmpIdx)));
    } else {
      ma_alu(dest.base, lsl(dest.index, dest.scale), scratch, OpAdd);
      ma_strd(val.payloadReg(), val.typeReg(),
              EDtrAddr(scratch, EDtrOffImm(dest.offset)));
    }
  } else {
    ma_alu(dest.base, lsl(dest.index, dest.scale), scratch, OpAdd);
    storeValue(val, Address(scratch, dest.offset));
  }
}

void MacroAssemblerARMCompat::loadValue(const BaseIndex& addr,
                                        ValueOperand val) {
  ScratchRegisterScope scratch(asMasm());

  if (isValueDTRDCandidate(val) && Abs(addr.offset) <= 255) {
    Register tmpIdx;
    if (addr.offset == 0) {
      if (addr.scale == TimesOne) {
        // If the offset register is the same as one of the destination
        // registers, LDRD's behavior is undefined. Use the scratch
        // register to avoid this.
        if (val.aliases(addr.index)) {
          ma_mov(addr.index, scratch);
          tmpIdx = scratch;
        } else {
          tmpIdx = addr.index;
        }
      } else {
        ma_lsl(Imm32(addr.scale), addr.index, scratch);
        tmpIdx = scratch;
      }
      ma_ldrd(EDtrAddr(addr.base, EDtrOffReg(tmpIdx)), val.payloadReg(),
              val.typeReg());
    } else {
      ma_alu(addr.base, lsl(addr.index, addr.scale), scratch, OpAdd);
      ma_ldrd(EDtrAddr(scratch, EDtrOffImm(addr.offset)), val.payloadReg(),
              val.typeReg());
    }
  } else {
    ma_alu(addr.base, lsl(addr.index, addr.scale), scratch, OpAdd);
    loadValue(Address(scratch, addr.offset), val);
  }
}

void MacroAssemblerARMCompat::loadValue(Address src, ValueOperand val) {
  // TODO: copy this code into a generic function that acts on all sequences
  // of memory accesses
  if (isValueDTRDCandidate(val)) {
    // If the value we want is in two consecutive registers starting with an
    // even register, they can be combined as a single ldrd.
    int offset = src.offset;
    if (offset < 256 && offset > -256) {
      ma_ldrd(EDtrAddr(src.base, EDtrOffImm(src.offset)), val.payloadReg(),
              val.typeReg());
      return;
    }
  }
  // If the value is lower than the type, then we may be able to use an ldm
  // instruction.

  if (val.payloadReg().code() < val.typeReg().code()) {
    if (src.offset <= 4 && src.offset >= -8 && (src.offset & 3) == 0) {
      // Turns out each of the 4 value -8, -4, 0, 4 corresponds exactly
      // with one of LDM{DB, DA, IA, IB}
      DTMMode mode;
      switch (src.offset) {
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
      startDataTransferM(IsLoad, src.base, mode);
      transferReg(val.payloadReg());
      transferReg(val.typeReg());
      finishDataTransfer();
      return;
    }
  }

  loadUnalignedValue(src, val);
}

void MacroAssemblerARMCompat::loadUnalignedValue(const Address& src,
                                                 ValueOperand dest) {
  Address payload = ToPayload(src);
  Address type = ToType(src);

  // Ensure that loading the payload does not erase the pointer to the Value
  // in memory.
  if (type.base != dest.payloadReg()) {
    SecondScratchRegisterScope scratch2(asMasm());
    ma_ldr(payload, dest.payloadReg(), scratch2);
    ma_ldr(type, dest.typeReg(), scratch2);
  } else {
    SecondScratchRegisterScope scratch2(asMasm());
    ma_ldr(type, dest.typeReg(), scratch2);
    ma_ldr(payload, dest.payloadReg(), scratch2);
  }
}

void MacroAssemblerARMCompat::tagValue(JSValueType type, Register payload,
                                       ValueOperand dest) {
  MOZ_ASSERT(dest.typeReg() != dest.payloadReg());
  if (payload != dest.payloadReg()) {
    ma_mov(payload, dest.payloadReg());
  }
  ma_mov(ImmType(type), dest.typeReg());
}

void MacroAssemblerARMCompat::pushValue(ValueOperand val) {
  ma_push(val.typeReg());
  ma_push(val.payloadReg());
}

void MacroAssemblerARMCompat::pushValue(const Address& addr) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  ma_ldr(ToType(addr), scratch, scratch2);
  ma_push(scratch);
  ma_ldr(ToPayloadAfterStackPush(addr), scratch, scratch2);
  ma_push(scratch);
}

void MacroAssemblerARMCompat::popValue(ValueOperand val) {
  ma_pop(val.payloadReg());
  ma_pop(val.typeReg());
}

void MacroAssemblerARMCompat::storePayload(const Value& val,
                                           const Address& dest) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (val.isGCThing()) {
    ma_mov(ImmGCPtr(val.toGCThing()), scratch);
  } else {
    ma_mov(Imm32(val.toNunboxPayload()), scratch);
  }
  ma_str(scratch, ToPayload(dest), scratch2);
}

void MacroAssemblerARMCompat::storePayload(Register src, const Address& dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_str(src, ToPayload(dest), scratch);
}

void MacroAssemblerARMCompat::storePayload(const Value& val,
                                           const BaseIndex& dest) {
  unsigned shift = ScaleToShift(dest.scale);

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  if (val.isGCThing()) {
    ma_mov(ImmGCPtr(val.toGCThing()), scratch);
  } else {
    ma_mov(Imm32(val.toNunboxPayload()), scratch);
  }

  // If NUNBOX32_PAYLOAD_OFFSET is not zero, the memory operand [base + index
  // << shift + imm] cannot be encoded into a single instruction, and cannot
  // be integrated into the as_dtr call.
  static_assert(NUNBOX32_PAYLOAD_OFFSET == 0);

  // If an offset is used, modify the base so that a [base + index << shift]
  // instruction format can be used.
  if (dest.offset != 0) {
    ma_add(dest.base, Imm32(dest.offset), dest.base, scratch2);
  }

  as_dtr(IsStore, 32, Offset, scratch,
         DTRAddr(dest.base, DtrRegImmShift(dest.index, LSL, shift)));

  // Restore the original value of the base, if necessary.
  if (dest.offset != 0) {
    ma_sub(dest.base, Imm32(dest.offset), dest.base, scratch);
  }
}

void MacroAssemblerARMCompat::storePayload(Register src,
                                           const BaseIndex& dest) {
  unsigned shift = ScaleToShift(dest.scale);
  MOZ_ASSERT(shift < 32);

  ScratchRegisterScope scratch(asMasm());

  // If NUNBOX32_PAYLOAD_OFFSET is not zero, the memory operand [base + index
  // << shift + imm] cannot be encoded into a single instruction, and cannot
  // be integrated into the as_dtr call.
  static_assert(NUNBOX32_PAYLOAD_OFFSET == 0);

  // Save/restore the base if the BaseIndex has an offset, as above.
  if (dest.offset != 0) {
    ma_add(dest.base, Imm32(dest.offset), dest.base, scratch);
  }

  // Technically, shift > -32 can be handle by changing LSL to ASR, but should
  // never come up, and this is one less code path to get wrong.
  as_dtr(IsStore, 32, Offset, src,
         DTRAddr(dest.base, DtrRegImmShift(dest.index, LSL, shift)));

  if (dest.offset != 0) {
    ma_sub(dest.base, Imm32(dest.offset), dest.base, scratch);
  }
}

void MacroAssemblerARMCompat::storeTypeTag(ImmTag tag, const Address& dest) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  ma_mov(tag, scratch);
  ma_str(scratch, ToType(dest), scratch2);
}

void MacroAssemblerARMCompat::storeTypeTag(ImmTag tag, const BaseIndex& dest) {
  Register base = dest.base;
  Register index = dest.index;
  unsigned shift = ScaleToShift(dest.scale);

  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  MOZ_ASSERT(base != scratch && base != scratch2);
  MOZ_ASSERT(index != scratch && index != scratch2);

  ma_add(base, Imm32(dest.offset + NUNBOX32_TYPE_OFFSET), scratch2, scratch);
  ma_mov(tag, scratch);
  ma_str(scratch, DTRAddr(scratch2, DtrRegImmShift(index, LSL, shift)));
}

void MacroAssemblerARM::ma_call(ImmPtr dest) {
  ma_movPatchable(dest, CallReg, Always);
  as_blx(CallReg);
}

void MacroAssemblerARMCompat::breakpoint() { as_bkpt(); }

void MacroAssemblerARMCompat::simulatorStop(const char* msg) {
#ifdef JS_SIMULATOR_ARM
  MOZ_ASSERT(sizeof(char*) == 4);
  writeInst(0xefffffff);
  writeInst((int)msg);
#endif
}

void MacroAssemblerARMCompat::ensureDouble(const ValueOperand& source,
                                           FloatRegister dest, Label* failure) {
  Label isDouble, done;
  asMasm().branchTestDouble(Assembler::Equal, source.typeReg(), &isDouble);
  asMasm().branchTestInt32(Assembler::NotEqual, source.typeReg(), failure);

  convertInt32ToDouble(source.payloadReg(), dest);
  jump(&done);

  bind(&isDouble);
  unboxDouble(source, dest);

  bind(&done);
}

void MacroAssemblerARMCompat::breakpoint(Condition cc) {
  ma_ldr(DTRAddr(r12, DtrRegImmShift(r12, LSL, 0, IsDown)), r12, Offset, cc);
}

void MacroAssemblerARMCompat::checkStackAlignment() {
  asMasm().assertStackAlignment(ABIStackAlignment);
}

void MacroAssemblerARMCompat::handleFailureWithHandlerTail(
    Label* profilerExitTail) {
  // Reserve space for exception information.
  int size = (sizeof(ResumeFromException) + 7) & ~7;

  Imm8 size8(size);
  as_sub(sp, sp, size8);
  ma_mov(sp, r0);

  // Call the handler.
  using Fn = void (*)(ResumeFromException * rfe);
  asMasm().setupUnalignedABICall(r1);
  asMasm().passABIArg(r0);
  asMasm().callWithABI<Fn, HandleException>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  Label entryFrame;
  Label catch_;
  Label finally;
  Label return_;
  Label bailout;
  Label wasm;
  Label wasmCatch;

  {
    ScratchRegisterScope scratch(asMasm());
    ma_ldr(Address(sp, offsetof(ResumeFromException, kind)), r0, scratch);
  }

  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_ENTRY_FRAME),
                    &entryFrame);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_CATCH), &catch_);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_FINALLY), &finally);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_FORCED_RETURN), &return_);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_WASM), &wasm);
  asMasm().branch32(Assembler::Equal, r0,
                    Imm32(ResumeFromException::RESUME_WASM_CATCH), &wasmCatch);

  breakpoint();  // Invalid kind.

  // No exception handler. Load the error value, load the new stack pointer
  // and return from the entry frame.
  bind(&entryFrame);
  asMasm().moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
  {
    ScratchRegisterScope scratch(asMasm());
    ma_ldr(Address(sp, offsetof(ResumeFromException, stackPointer)), sp,
           scratch);
  }

  // We're going to be returning by the ion calling convention, which returns
  // by ??? (for now, I think ldr pc, [sp]!)
  as_dtr(IsLoad, 32, PostIndex, pc, DTRAddr(sp, DtrOffImm(4)));

  // If we found a catch handler, this must be a baseline frame. Restore state
  // and jump to the catch block.
  bind(&catch_);
  {
    ScratchRegisterScope scratch(asMasm());
    ma_ldr(Address(sp, offsetof(ResumeFromException, target)), r0, scratch);
    ma_ldr(Address(sp, offsetof(ResumeFromException, framePointer)), r11,
           scratch);
    ma_ldr(Address(sp, offsetof(ResumeFromException, stackPointer)), sp,
           scratch);
  }
  jump(r0);

  // If we found a finally block, this must be a baseline frame. Push two
  // values expected by JSOp::Retsub: BooleanValue(true) and the exception.
  bind(&finally);
  ValueOperand exception = ValueOperand(r1, r2);
  loadValue(Operand(sp, offsetof(ResumeFromException, exception)), exception);
  {
    ScratchRegisterScope scratch(asMasm());
    ma_ldr(Address(sp, offsetof(ResumeFromException, target)), r0, scratch);
    ma_ldr(Address(sp, offsetof(ResumeFromException, framePointer)), r11,
           scratch);
    ma_ldr(Address(sp, offsetof(ResumeFromException, stackPointer)), sp,
           scratch);
  }

  pushValue(BooleanValue(true));
  pushValue(exception);
  jump(r0);

  // Only used in debug mode. Return BaselineFrame->returnValue() to the
  // caller.
  bind(&return_);
  {
    ScratchRegisterScope scratch(asMasm());
    ma_ldr(Address(sp, offsetof(ResumeFromException, framePointer)), r11,
           scratch);
    ma_ldr(Address(sp, offsetof(ResumeFromException, stackPointer)), sp,
           scratch);
  }
  loadValue(Address(r11, BaselineFrame::reverseOffsetOfReturnValue()),
            JSReturnOperand);
  ma_mov(r11, sp);
  pop(r11);

  // If profiling is enabled, then update the lastProfilingFrame to refer to
  // caller frame before returning.
  {
    Label skipProfilingInstrumentation;
    // Test if profiler enabled.
    AbsoluteAddress addressOfEnabled(
        GetJitContext()->runtime->geckoProfiler().addressOfEnabled());
    asMasm().branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                      &skipProfilingInstrumentation);
    jump(profilerExitTail);
    bind(&skipProfilingInstrumentation);
  }

  ret();

  // If we are bailing out to baseline to handle an exception, jump to the
  // bailout tail stub. Load 1 (true) in ReturnReg to indicate success.
  bind(&bailout);
  {
    ScratchRegisterScope scratch(asMasm());
    ma_ldr(Address(sp, offsetof(ResumeFromException, bailoutInfo)), r2,
           scratch);
    ma_mov(Imm32(1), ReturnReg);
    ma_ldr(Address(sp, offsetof(ResumeFromException, target)), r1, scratch);
  }
  jump(r1);

  // If we are throwing and the innermost frame was a wasm frame, reset SP and
  // FP; SP is pointing to the unwound return address to the wasm entry, so
  // we can just ret().
  bind(&wasm);
  {
    ScratchRegisterScope scratch(asMasm());
    ma_ldr(Address(sp, offsetof(ResumeFromException, framePointer)), r11,
           scratch);
    ma_ldr(Address(sp, offsetof(ResumeFromException, stackPointer)), sp,
           scratch);
  }
  as_dtr(IsLoad, 32, PostIndex, pc, DTRAddr(sp, DtrOffImm(4)));

  // Found a wasm catch handler, restore state and jump to it.
  bind(&wasmCatch);
  {
    ScratchRegisterScope scratch(asMasm());
    ma_ldr(Address(sp, offsetof(ResumeFromException, target)), r1, scratch);
    ma_ldr(Address(sp, offsetof(ResumeFromException, framePointer)), r11,
           scratch);
    ma_ldr(Address(sp, offsetof(ResumeFromException, stackPointer)), sp,
           scratch);
  }
  jump(r1);
}

Assembler::Condition MacroAssemblerARMCompat::testStringTruthy(
    bool truthy, const ValueOperand& value) {
  Register string = value.payloadReg();
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  ma_dtr(IsLoad, string, Imm32(JSString::offsetOfLength()), scratch, scratch2);
  as_cmp(scratch, Imm8(0));
  return truthy ? Assembler::NotEqual : Assembler::Equal;
}

Assembler::Condition MacroAssemblerARMCompat::testBigIntTruthy(
    bool truthy, const ValueOperand& value) {
  Register bi = value.payloadReg();
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());

  ma_dtr(IsLoad, bi, Imm32(BigInt::offsetOfDigitLength()), scratch, scratch2);
  as_cmp(scratch, Imm8(0));
  return truthy ? Assembler::NotEqual : Assembler::Equal;
}

void MacroAssemblerARMCompat::floor(FloatRegister input, Register output,
                                    Label* bail) {
  Label handleZero;
  Label handleNeg;
  Label fin;

  ScratchDoubleScope scratchDouble(asMasm());

  compareDouble(input, NoVFPRegister);
  ma_b(&handleZero, Assembler::Equal);
  ma_b(&handleNeg, Assembler::Signed);
  // NaN is always a bail condition, just bail directly.
  ma_b(bail, Assembler::Overflow);

  // The argument is a positive number, truncation is the path to glory. Since
  // it is known to be > 0.0, explicitly convert to a larger range, then a
  // value that rounds to INT_MAX is explicitly different from an argument
  // that clamps to INT_MAX.
  ma_vcvt_F64_U32(input, scratchDouble.uintOverlay());
  ma_vxfer(scratchDouble.uintOverlay(), output);
  ma_mov(output, output, SetCC);
  ma_b(bail, Signed);
  ma_b(&fin);

  bind(&handleZero);
  // Move the top word of the double into the output reg, if it is non-zero,
  // then the original value was -0.0.
  as_vxfer(output, InvalidReg, input, FloatToCore, Always, 1);
  as_cmp(output, Imm8(0));
  ma_b(bail, NonZero);
  ma_b(&fin);

  bind(&handleNeg);
  // Negative case, negate, then start dancing.
  ma_vneg(input, input);
  ma_vcvt_F64_U32(input, scratchDouble.uintOverlay());
  ma_vxfer(scratchDouble.uintOverlay(), output);
  ma_vcvt_U32_F64(scratchDouble.uintOverlay(), scratchDouble);
  compareDouble(scratchDouble, input);
  as_add(output, output, Imm8(1), LeaveCC, NotEqual);
  // Negate the output. Since INT_MIN < -INT_MAX, even after adding 1, the
  // result will still be a negative number.
  as_rsb(output, output, Imm8(0), SetCC);
  // Flip the negated input back to its original value.
  ma_vneg(input, input);
  // If the result looks non-negative, then this value didn't actually fit
  // into the int range, and special handling is required. Zero is also caught
  // by this case, but floor of a negative number should never be zero.
  ma_b(bail, NotSigned);

  bind(&fin);
}

void MacroAssemblerARMCompat::floorf(FloatRegister input, Register output,
                                     Label* bail) {
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
  {
    ScratchFloat32Scope scratch(asMasm());
    ma_vcvt_F32_U32(input, scratch.uintOverlay());
    ma_vxfer(VFPRegister(scratch).uintOverlay(), output);
  }
  ma_mov(output, output, SetCC);
  ma_b(bail, Signed);
  ma_b(&fin);

  bind(&handleZero);
  // Move the top word of the double into the output reg, if it is non-zero,
  // then the original value was -0.0.
  as_vxfer(output, InvalidReg, VFPRegister(input).singleOverlay(), FloatToCore,
           Always, 0);
  as_cmp(output, Imm8(0));
  ma_b(bail, NonZero);
  ma_b(&fin);

  bind(&handleNeg);
  // Negative case, negate, then start dancing.
  {
    ScratchFloat32Scope scratch(asMasm());
    ma_vneg_f32(input, input);
    ma_vcvt_F32_U32(input, scratch.uintOverlay());
    ma_vxfer(VFPRegister(scratch).uintOverlay(), output);
    ma_vcvt_U32_F32(scratch.uintOverlay(), scratch);
    compareFloat(scratch, input);
    as_add(output, output, Imm8(1), LeaveCC, NotEqual);
  }
  // Negate the output. Since INT_MIN < -INT_MAX, even after adding 1, the
  // result will still be a negative number.
  as_rsb(output, output, Imm8(0), SetCC);
  // Flip the negated input back to its original value.
  ma_vneg_f32(input, input);
  // If the result looks non-negative, then this value didn't actually fit
  // into the int range, and special handling is required. Zero is also caught
  // by this case, but floor of a negative number should never be zero.
  ma_b(bail, NotSigned);

  bind(&fin);
}

void MacroAssemblerARMCompat::ceil(FloatRegister input, Register output,
                                   Label* bail) {
  Label handleZero;
  Label handlePos;
  Label fin;

  compareDouble(input, NoVFPRegister);
  // NaN is always a bail condition, just bail directly.
  ma_b(bail, Assembler::Overflow);
  ma_b(&handleZero, Assembler::Equal);
  ma_b(&handlePos, Assembler::NotSigned);

  ScratchDoubleScope scratchDouble(asMasm());

  // We are in the ]-Inf; 0[ range
  // If we are in the ]-1; 0[ range => bailout
  loadConstantDouble(-1.0, scratchDouble);
  compareDouble(input, scratchDouble);
  ma_b(bail, Assembler::GreaterThan);

  // We are in the ]-Inf; -1] range: ceil(x) == -floor(-x) and floor can be
  // computed with direct truncation here (x > 0).
  ma_vneg(input, scratchDouble);
  FloatRegister ScratchUIntReg = scratchDouble.uintOverlay();
  ma_vcvt_F64_U32(scratchDouble, ScratchUIntReg);
  ma_vxfer(ScratchUIntReg, output);
  ma_neg(output, output, SetCC);
  ma_b(bail, NotSigned);
  ma_b(&fin);

  // Test for 0.0 / -0.0: if the top word of the input double is not zero,
  // then it was -0 and we need to bail out.
  bind(&handleZero);
  as_vxfer(output, InvalidReg, input, FloatToCore, Always, 1);
  as_cmp(output, Imm8(0));
  ma_b(bail, NonZero);
  ma_b(&fin);

  // We are in the ]0; +inf] range: truncate integer values, maybe add 1 for
  // non integer values, maybe bail if overflow.
  bind(&handlePos);
  ma_vcvt_F64_U32(input, ScratchUIntReg);
  ma_vxfer(ScratchUIntReg, output);
  ma_vcvt_U32_F64(ScratchUIntReg, scratchDouble);
  compareDouble(scratchDouble, input);
  as_add(output, output, Imm8(1), LeaveCC, NotEqual);
  // Bail out if the add overflowed or the result is non positive.
  ma_mov(output, output, SetCC);
  ma_b(bail, Signed);
  ma_b(bail, Zero);

  bind(&fin);
}

void MacroAssemblerARMCompat::ceilf(FloatRegister input, Register output,
                                    Label* bail) {
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
  {
    ScratchFloat32Scope scratch(asMasm());
    loadConstantFloat32(-1.f, scratch);
    compareFloat(input, scratch);
    ma_b(bail, Assembler::GreaterThan);
  }

  // We are in the ]-Inf; -1] range: ceil(x) == -floor(-x) and floor can be
  // computed with direct truncation here (x > 0).
  {
    ScratchDoubleScope scratchDouble(asMasm());
    FloatRegister scratchFloat = scratchDouble.asSingle();
    FloatRegister scratchUInt = scratchDouble.uintOverlay();

    ma_vneg_f32(input, scratchFloat);
    ma_vcvt_F32_U32(scratchFloat, scratchUInt);
    ma_vxfer(scratchUInt, output);
    ma_neg(output, output, SetCC);
    ma_b(bail, NotSigned);
    ma_b(&fin);
  }

  // Test for 0.0 / -0.0: if the top word of the input double is not zero,
  // then it was -0 and we need to bail out.
  bind(&handleZero);
  as_vxfer(output, InvalidReg, VFPRegister(input).singleOverlay(), FloatToCore,
           Always, 0);
  as_cmp(output, Imm8(0));
  ma_b(bail, NonZero);
  ma_b(&fin);

  // We are in the ]0; +inf] range: truncate integer values, maybe add 1 for
  // non integer values, maybe bail if overflow.
  bind(&handlePos);
  {
    ScratchDoubleScope scratchDouble(asMasm());
    FloatRegister scratchFloat = scratchDouble.asSingle();
    FloatRegister scratchUInt = scratchDouble.uintOverlay();

    ma_vcvt_F32_U32(input, scratchUInt);
    ma_vxfer(scratchUInt, output);
    ma_vcvt_U32_F32(scratchUInt, scratchFloat);
    compareFloat(scratchFloat, input);
    as_add(output, output, Imm8(1), LeaveCC, NotEqual);

    // Bail on overflow or non-positive result.
    ma_mov(output, output, SetCC);
    ma_b(bail, Signed);
    ma_b(bail, Zero);
  }

  bind(&fin);
}

CodeOffset MacroAssemblerARMCompat::toggledJump(Label* label) {
  // Emit a B that can be toggled to a CMP. See ToggleToJmp(), ToggleToCmp().
  BufferOffset b = ma_b(label, Always);
  CodeOffset ret(b.getOffset());
  return ret;
}

CodeOffset MacroAssemblerARMCompat::toggledCall(JitCode* target, bool enabled) {
  BufferOffset bo = nextOffset();
  addPendingJump(bo, ImmPtr(target->raw()), RelocationKind::JITCODE);
  ScratchRegisterScope scratch(asMasm());
  ma_movPatchable(ImmPtr(target->raw()), scratch, Always);
  if (enabled) {
    ma_blx(scratch);
  } else {
    ma_nop();
  }
  return CodeOffset(bo.getOffset());
}

void MacroAssemblerARMCompat::round(FloatRegister input, Register output,
                                    Label* bail, FloatRegister tmp) {
  Label handleZero;
  Label handleNeg;
  Label fin;

  ScratchDoubleScope scratchDouble(asMasm());

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
  loadConstantDouble(GetBiggestNumberLessThan(0.5), scratchDouble);
  ma_vadd(scratchDouble, tmp, tmp);

  ma_vcvt_F64_U32(tmp, scratchDouble.uintOverlay());
  ma_vxfer(VFPRegister(scratchDouble).uintOverlay(), output);
  ma_mov(output, output, SetCC);
  ma_b(bail, Signed);
  ma_b(&fin);

  bind(&handleZero);
  // Move the top word of the double into the output reg, if it is non-zero,
  // then the original value was -0.0
  as_vxfer(output, InvalidReg, input, FloatToCore, Always, 1);
  as_cmp(output, Imm8(0));
  ma_b(bail, NonZero);
  ma_b(&fin);

  bind(&handleNeg);
  // Negative case, negate, then start dancing. This number may be positive,
  // since we added 0.5.

  // Add 0.5 to negative numbers, store the result into tmp
  loadConstantDouble(0.5, scratchDouble);
  ma_vadd(scratchDouble, tmp, tmp);

  ma_vcvt_F64_U32(tmp, scratchDouble.uintOverlay());
  ma_vxfer(VFPRegister(scratchDouble).uintOverlay(), output);

  // -output is now a correctly rounded value, unless the original value was
  // exactly halfway between two integers, at which point, it has been rounded
  // away from zero, when it should be rounded towards \infty.
  ma_vcvt_U32_F64(scratchDouble.uintOverlay(), scratchDouble);
  compareDouble(scratchDouble, tmp);
  as_sub(output, output, Imm8(1), LeaveCC, Equal);
  // Negate the output. Since INT_MIN < -INT_MAX, even after adding 1, the
  // result will still be a negative number.
  as_rsb(output, output, Imm8(0), SetCC);

  // If the result looks non-negative, then this value didn't actually fit
  // into the int range, and special handling is required, or it was zero,
  // which means the result is actually -0.0 which also requires special
  // handling.
  ma_b(bail, NotSigned);

  bind(&fin);
}

void MacroAssemblerARMCompat::roundf(FloatRegister input, Register output,
                                     Label* bail, FloatRegister tmp) {
  Label handleZero;
  Label handleNeg;
  Label fin;

  ScratchFloat32Scope scratchFloat(asMasm());

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
  loadConstantFloat32(GetBiggestNumberLessThan(0.5f), scratchFloat);
  ma_vadd_f32(scratchFloat, input, tmp);

  // Note: it doesn't matter whether x + .5 === x or not here, as it doesn't
  // affect the semantics of the float to unsigned conversion (in particular,
  // we are not applying any fixup after the operation).
  ma_vcvt_F32_U32(tmp, scratchFloat.uintOverlay());
  ma_vxfer(VFPRegister(scratchFloat).uintOverlay(), output);
  ma_mov(output, output, SetCC);
  ma_b(bail, Signed);
  ma_b(&fin);

  bind(&handleZero);

  // Move the whole float32 into the output reg, if it is non-zero, then the
  // original value was -0.0.
  as_vxfer(output, InvalidReg, input, FloatToCore, Always, 0);
  as_cmp(output, Imm8(0));
  ma_b(bail, NonZero);
  ma_b(&fin);

  bind(&handleNeg);

  // Add 0.5 to negative numbers, storing the result into tmp.
  ma_vneg_f32(input, tmp);
  loadConstantFloat32(0.5f, scratchFloat);
  ma_vadd_f32(tmp, scratchFloat, scratchFloat);

  // Adding 0.5 to a float input has chances to yield the wrong result, if
  // the input is too large. In this case, skip the -1 adjustment made below.
  compareFloat(scratchFloat, tmp);

  // Negative case, negate, then start dancing. This number may be positive,
  // since we added 0.5.
  // /!\ The conditional jump afterwards depends on these two instructions
  //     *not* setting the status flags. They need to not change after the
  //     comparison above.
  ma_vcvt_F32_U32(scratchFloat, tmp.uintOverlay());
  ma_vxfer(VFPRegister(tmp).uintOverlay(), output);

  Label flipSign;
  ma_b(&flipSign, Equal);

  // -output is now a correctly rounded value, unless the original value was
  // exactly halfway between two integers, at which point, it has been rounded
  // away from zero, when it should be rounded towards \infty.
  ma_vcvt_U32_F32(tmp.uintOverlay(), tmp);
  compareFloat(tmp, scratchFloat);
  as_sub(output, output, Imm8(1), LeaveCC, Equal);

  // Negate the output. Since INT_MIN < -INT_MAX, even after adding 1, the
  // result will still be a negative number.
  bind(&flipSign);
  as_rsb(output, output, Imm8(0), SetCC);

  // If the result looks non-negative, then this value didn't actually fit
  // into the int range, and special handling is required, or it was zero,
  // which means the result is actually -0.0 which also requires special
  // handling.
  ma_b(bail, NotSigned);

  bind(&fin);
}

void MacroAssemblerARMCompat::trunc(FloatRegister input, Register output,
                                    Label* bail) {
  Label handleZero;
  Label handlePos;
  Label fin;

  compareDouble(input, NoVFPRegister);
  // NaN is always a bail condition, just bail directly.
  ma_b(bail, Assembler::Overflow);
  ma_b(&handleZero, Assembler::Equal);
  ma_b(&handlePos, Assembler::NotSigned);

  ScratchDoubleScope scratchDouble(asMasm());

  // We are in the ]-Inf; 0[ range
  // If we are in the ]-1; 0[ range => bailout
  loadConstantDouble(-1.0, scratchDouble);
  compareDouble(input, scratchDouble);
  ma_b(bail, Assembler::GreaterThan);

  // We are in the ]-Inf; -1] range: trunc(x) == -floor(-x) and floor can be
  // computed with direct truncation here (x > 0).
  ma_vneg(input, scratchDouble);
  ma_vcvt_F64_U32(scratchDouble, scratchDouble.uintOverlay());
  ma_vxfer(scratchDouble.uintOverlay(), output);
  ma_neg(output, output, SetCC);
  ma_b(bail, NotSigned);
  ma_b(&fin);

  // Test for 0.0 / -0.0: if the top word of the input double is not zero,
  // then it was -0 and we need to bail out.
  bind(&handleZero);
  as_vxfer(output, InvalidReg, input, FloatToCore, Always, 1);
  as_cmp(output, Imm8(0));
  ma_b(bail, NonZero);
  ma_b(&fin);

  // We are in the ]0; +inf] range: truncation is the path to glory. Since
  // it is known to be > 0.0, explicitly convert to a larger range, then a
  // value that rounds to INT_MAX is explicitly different from an argument
  // that clamps to INT_MAX.
  bind(&handlePos);
  ma_vcvt_F64_U32(input, scratchDouble.uintOverlay());
  ma_vxfer(scratchDouble.uintOverlay(), output);
  ma_mov(output, output, SetCC);
  ma_b(bail, Signed);

  bind(&fin);
}

void MacroAssemblerARMCompat::truncf(FloatRegister input, Register output,
                                     Label* bail) {
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
  {
    ScratchFloat32Scope scratch(asMasm());
    loadConstantFloat32(-1.f, scratch);
    compareFloat(input, scratch);
    ma_b(bail, Assembler::GreaterThan);
  }

  // We are in the ]-Inf; -1] range: trunc(x) == -floor(-x) and floor can be
  // computed with direct truncation here (x > 0).
  {
    ScratchDoubleScope scratchDouble(asMasm());
    FloatRegister scratchFloat = scratchDouble.asSingle();
    FloatRegister scratchUInt = scratchDouble.uintOverlay();

    ma_vneg_f32(input, scratchFloat);
    ma_vcvt_F32_U32(scratchFloat, scratchUInt);
    ma_vxfer(scratchUInt, output);
    ma_neg(output, output, SetCC);
    ma_b(bail, NotSigned);
    ma_b(&fin);
  }

  // Test for 0.0 / -0.0: if the top word of the input double is not zero,
  // then it was -0 and we need to bail out.
  bind(&handleZero);
  as_vxfer(output, InvalidReg, VFPRegister(input).singleOverlay(), FloatToCore,
           Always, 0);
  as_cmp(output, Imm8(0));
  ma_b(bail, NonZero);
  ma_b(&fin);

  // We are in the ]0; +inf] range: truncation is the path to glory; Since
  // it is known to be > 0.0, explicitly convert to a larger range, then a
  // value that rounds to INT_MAX is explicitly different from an argument
  bind(&handlePos);
  {
    // The argument is a positive number,
    // that clamps to INT_MAX.
    {
      ScratchFloat32Scope scratch(asMasm());
      ma_vcvt_F32_U32(input, scratch.uintOverlay());
      ma_vxfer(VFPRegister(scratch).uintOverlay(), output);
    }
    ma_mov(output, output, SetCC);
    ma_b(bail, Signed);
  }

  bind(&fin);
}

void MacroAssemblerARMCompat::profilerEnterFrame(Register framePtr,
                                                 Register scratch) {
  asMasm().loadJSContext(scratch);
  loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
  storePtr(framePtr,
           Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
  storePtr(ImmPtr(nullptr),
           Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void MacroAssemblerARMCompat::profilerExitFrame() {
  jump(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
}

MacroAssembler& MacroAssemblerARM::asMasm() {
  return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler& MacroAssemblerARM::asMasm() const {
  return *static_cast<const MacroAssembler*>(this);
}

MacroAssembler& MacroAssemblerARMCompat::asMasm() {
  return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler& MacroAssemblerARMCompat::asMasm() const {
  return *static_cast<const MacroAssembler*>(this);
}

void MacroAssembler::subFromStackPtr(Imm32 imm32) {
  ScratchRegisterScope scratch(*this);
  if (imm32.value) {
    ma_sub(imm32, sp, scratch);
  }
}

//{{{ check_macroassembler_style
// ===============================================================
// MacroAssembler high-level usage.

void MacroAssembler::flush() { Assembler::flush(); }

void MacroAssembler::comment(const char* msg) { Assembler::comment(msg); }

// ===============================================================
// Stack manipulation functions.

size_t MacroAssembler::PushRegsInMaskSizeInBytes(LiveRegisterSet set) {
  return set.gprs().size() * sizeof(intptr_t) + set.fpus().getPushSizeInBytes();
}

void MacroAssembler::PushRegsInMask(LiveRegisterSet set) {
  mozilla::DebugOnly<size_t> framePushedInitial = framePushed();

  int32_t diffF = set.fpus().getPushSizeInBytes();
  int32_t diffG = set.gprs().size() * sizeof(intptr_t);

  if (set.gprs().size() > 1) {
    adjustFrame(diffG);
    startDataTransferM(IsStore, StackPointer, DB, WriteBack);
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more();
         ++iter) {
      diffG -= sizeof(intptr_t);
      transferReg(*iter);
    }
    finishDataTransfer();
  } else {
    reserveStack(diffG);
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more();
         ++iter) {
      diffG -= sizeof(intptr_t);
      storePtr(*iter, Address(StackPointer, diffG));
    }
  }
  MOZ_ASSERT(diffG == 0);

  // It's possible that the logic is just fine as it is if the reduced set
  // maps SIMD pairs to plain doubles and transferMultipleByRuns() stores
  // and loads doubles.
#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  adjustFrame(diffF);
  diffF += transferMultipleByRuns(set.fpus(), IsStore, StackPointer, DB);
  MOZ_ASSERT(diffF == 0);

  MOZ_ASSERT(framePushed() - framePushedInitial ==
             PushRegsInMaskSizeInBytes(set));
}

void MacroAssembler::storeRegsInMask(LiveRegisterSet set, Address dest,
                                     Register scratch) {
  mozilla::DebugOnly<size_t> offsetInitial = dest.offset;

  int32_t diffF = set.fpus().getPushSizeInBytes();
  int32_t diffG = set.gprs().size() * sizeof(intptr_t);

  MOZ_ASSERT(dest.offset >= diffF + diffG);

  if (set.gprs().size() > 1) {
    computeEffectiveAddress(dest, scratch);

    startDataTransferM(IsStore, scratch, DB, WriteBack);
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more();
         ++iter) {
      diffG -= sizeof(intptr_t);
      dest.offset -= sizeof(intptr_t);
      transferReg(*iter);
    }
    finishDataTransfer();
  } else {
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more();
         ++iter) {
      diffG -= sizeof(intptr_t);
      dest.offset -= sizeof(intptr_t);
      storePtr(*iter, dest);
    }
  }
  MOZ_ASSERT(diffG == 0);

  // See above.
#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  MOZ_ASSERT(diffF >= 0);
  if (diffF > 0) {
    computeEffectiveAddress(dest, scratch);
    diffF += transferMultipleByRuns(set.fpus(), IsStore, scratch, DB);
  }

  MOZ_ASSERT(diffF == 0);

  // "The amount of space actually used does not exceed what
  // `PushRegsInMaskSizeInBytes` claims will be used."
  MOZ_ASSERT(offsetInitial - dest.offset <= PushRegsInMaskSizeInBytes(set));
}

void MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set,
                                         LiveRegisterSet ignore) {
  mozilla::DebugOnly<size_t> framePushedInitial = framePushed();

  int32_t diffG = set.gprs().size() * sizeof(intptr_t);
  int32_t diffF = set.fpus().getPushSizeInBytes();
  const int32_t reservedG = diffG;
  const int32_t reservedF = diffF;

  // See above.
#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  // ARM can load multiple registers at once, but only if we want back all
  // the registers we previously saved to the stack.
  if (ignore.emptyFloat()) {
    diffF -= transferMultipleByRuns(set.fpus(), IsLoad, StackPointer, IA);
    adjustFrame(-reservedF);
  } else {
    LiveFloatRegisterSet fpset(set.fpus().reduceSetForPush());
    LiveFloatRegisterSet fpignore(ignore.fpus().reduceSetForPush());
    for (FloatRegisterBackwardIterator iter(fpset); iter.more(); ++iter) {
      diffF -= (*iter).size();
      if (!fpignore.has(*iter)) {
        loadDouble(Address(StackPointer, diffF), *iter);
      }
    }
    freeStack(reservedF);
  }
  MOZ_ASSERT(diffF == 0);

  if (set.gprs().size() > 1 && ignore.emptyGeneral()) {
    startDataTransferM(IsLoad, StackPointer, IA, WriteBack);
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more();
         ++iter) {
      diffG -= sizeof(intptr_t);
      transferReg(*iter);
    }
    finishDataTransfer();
    adjustFrame(-reservedG);
  } else {
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more();
         ++iter) {
      diffG -= sizeof(intptr_t);
      if (!ignore.has(*iter)) {
        loadPtr(Address(StackPointer, diffG), *iter);
      }
    }
    freeStack(reservedG);
  }
  MOZ_ASSERT(diffG == 0);

  MOZ_ASSERT(framePushedInitial - framePushed() ==
             PushRegsInMaskSizeInBytes(set));
}

void MacroAssembler::Push(Register reg) {
  push(reg);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(const Imm32 imm) {
  push(imm);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(const ImmWord imm) {
  push(imm);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(const ImmPtr imm) {
  Push(ImmWord(uintptr_t(imm.value)));
}

void MacroAssembler::Push(const ImmGCPtr ptr) {
  push(ptr);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(FloatRegister reg) {
  VFPRegister r = VFPRegister(reg);
  ma_vpush(VFPRegister(reg));
  adjustFrame(r.size());
}

void MacroAssembler::PushBoxed(FloatRegister reg) {
  MOZ_ASSERT(reg.isDouble());
  Push(reg);
}

void MacroAssembler::Pop(Register reg) {
  ma_pop(reg);
  adjustFrame(-sizeof(intptr_t));
}

void MacroAssembler::Pop(FloatRegister reg) {
  ma_vpop(reg);
  adjustFrame(-reg.size());
}

void MacroAssembler::Pop(const ValueOperand& val) {
  popValue(val);
  adjustFrame(-sizeof(Value));
}

void MacroAssembler::PopStackPtr() {
  as_dtr(IsLoad, 32, Offset, sp, DTRAddr(sp, DtrOffImm(0)));
  adjustFrame(-sizeof(intptr_t));
}

// ===============================================================
// Simple call functions.

CodeOffset MacroAssembler::call(Register reg) {
  as_blx(reg);
  return CodeOffset(currentOffset());
}

CodeOffset MacroAssembler::call(Label* label) {
  // For now, assume that it'll be nearby.
  as_bl(label, Always);
  return CodeOffset(currentOffset());
}

void MacroAssembler::call(ImmWord imm) { call(ImmPtr((void*)imm.value)); }

void MacroAssembler::call(ImmPtr imm) {
  BufferOffset bo = m_buffer.nextOffset();
  addPendingJump(bo, imm, RelocationKind::HARDCODED);
  ma_call(imm);
}

CodeOffset MacroAssembler::call(wasm::SymbolicAddress imm) {
  movePtr(imm, CallReg);
  return call(CallReg);
}

void MacroAssembler::call(const Address& addr) {
  loadPtr(addr, CallReg);
  call(CallReg);
}

void MacroAssembler::call(JitCode* c) {
  BufferOffset bo = m_buffer.nextOffset();
  addPendingJump(bo, ImmPtr(c->raw()), RelocationKind::JITCODE);
  ScratchRegisterScope scratch(*this);
  ma_movPatchable(ImmPtr(c->raw()), scratch, Always);
  callJitNoProfiler(scratch);
}

CodeOffset MacroAssembler::callWithPatch() {
  // The caller ensures that the call is always in range using thunks (below)
  // as necessary.
  as_bl(BOffImm(), Always, /* documentation */ nullptr);
  return CodeOffset(currentOffset());
}

void MacroAssembler::patchCall(uint32_t callerOffset, uint32_t calleeOffset) {
  BufferOffset inst(callerOffset - 4);
  BOffImm off = BufferOffset(calleeOffset).diffB<BOffImm>(inst);
  MOZ_RELEASE_ASSERT(!off.isInvalid(),
                     "Failed to insert necessary far jump islands");
  as_bl(off, Always, inst);
}

CodeOffset MacroAssembler::farJumpWithPatch() {
  static_assert(32 * 1024 * 1024 - JumpImmediateRange >
                    wasm::MaxFuncs * 3 * sizeof(Instruction),
                "always enough space for thunks");

  // The goal of the thunk is to be able to jump to any address without the
  // usual 32MiB branch range limitation. Additionally, to make the thunk
  // simple to use, the thunk does not use the constant pool or require
  // patching an absolute address. Instead, a relative offset is used which
  // can be patched during compilation.

  // Inhibit pools since these three words must be contiguous so that the offset
  // calculations below are valid.
  AutoForbidPoolsAndNops afp(this, 3);

  // When pc is used, the read value is the address of the instruction + 8.
  // This is exactly the address of the uint32 word we want to load.
  ScratchRegisterScope scratch(*this);
  ma_ldr(DTRAddr(pc, DtrOffImm(0)), scratch);

  // Branch by making pc the destination register.
  ma_add(pc, scratch, pc, LeaveCC, Always);

  // Allocate space which will be patched by patchFarJump().
  CodeOffset farJump(currentOffset());
  writeInst(UINT32_MAX);

  return farJump;
}

void MacroAssembler::patchFarJump(CodeOffset farJump, uint32_t targetOffset) {
  uint32_t* u32 =
      reinterpret_cast<uint32_t*>(editSrc(BufferOffset(farJump.offset())));
  MOZ_ASSERT(*u32 == UINT32_MAX);

  uint32_t addOffset = farJump.offset() - 4;
  MOZ_ASSERT(editSrc(BufferOffset(addOffset))->is<InstALU>());

  // When pc is read as the operand of the add, its value is the address of
  // the add instruction + 8.
  *u32 = (targetOffset - addOffset) - 8;
}

CodeOffset MacroAssembler::nopPatchableToCall() {
  AutoForbidPoolsAndNops afp(this,
                             /* max number of instructions in scope = */ 1);
  ma_nop();
  return CodeOffset(currentOffset());
}

void MacroAssembler::patchNopToCall(uint8_t* call, uint8_t* target) {
  uint8_t* inst = call - 4;
  MOZ_ASSERT(reinterpret_cast<Instruction*>(inst)->is<InstBLImm>() ||
             reinterpret_cast<Instruction*>(inst)->is<InstNOP>());

  new (inst) InstBLImm(BOffImm(target - inst), Assembler::Always);
}

void MacroAssembler::patchCallToNop(uint8_t* call) {
  uint8_t* inst = call - 4;
  MOZ_ASSERT(reinterpret_cast<Instruction*>(inst)->is<InstBLImm>() ||
             reinterpret_cast<Instruction*>(inst)->is<InstNOP>());
  new (inst) InstNOP();
}

void MacroAssembler::pushReturnAddress() { push(lr); }

void MacroAssembler::popReturnAddress() { pop(lr); }

// ===============================================================
// ABI function calls.

void MacroAssembler::setupUnalignedABICall(Register scratch) {
  setupNativeABICall();
  dynamicAlignment_ = true;

  ma_mov(sp, scratch);
  // Force sp to be aligned.
  as_bic(sp, sp, Imm8(ABIStackAlignment - 1));
  ma_push(scratch);
}

void MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromWasm) {
  MOZ_ASSERT(inCall_);
  uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

  if (dynamicAlignment_) {
    // sizeof(intptr_t) accounts for the saved stack pointer pushed by
    // setupUnalignedABICall.
    stackForCall += ComputeByteAlignment(stackForCall + sizeof(intptr_t),
                                         ABIStackAlignment);
  } else {
    uint32_t alignmentAtPrologue = callFromWasm ? sizeof(wasm::Frame) : 0;
    stackForCall += ComputeByteAlignment(
        stackForCall + framePushed() + alignmentAtPrologue, ABIStackAlignment);
  }

  *stackAdjust = stackForCall;
  reserveStack(stackForCall);

  // Position all arguments.
  {
    enoughMemory_ &= moveResolver_.resolve();
    if (!enoughMemory_) {
      return;
    }

    MoveEmitter emitter(*this);
    emitter.emit(moveResolver_);
    emitter.finish();
  }

  assertStackAlignment(ABIStackAlignment);

  // Save the lr register if we need to preserve it.
  if (secondScratchReg_ != lr) {
    ma_mov(lr, secondScratchReg_);
  }
}

void MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result,
                                     bool callFromWasm) {
  if (secondScratchReg_ != lr) {
    ma_mov(secondScratchReg_, lr);
  }

  // Calls to native functions in wasm pass through a thunk which already
  // fixes up the return value for us.
  if (!callFromWasm && !UseHardFpABI()) {
    switch (result) {
      case MoveOp::DOUBLE:
        // Move double from r0/r1 to ReturnFloatReg.
        ma_vxfer(r0, r1, ReturnDoubleReg);
        break;
      case MoveOp::FLOAT32:
        // Move float32 from r0 to ReturnFloatReg.
        ma_vxfer(r0, ReturnFloat32Reg);
        break;
      case MoveOp::GENERAL:
        break;
      default:
        MOZ_CRASH("unexpected callWithABI result");
    }
  }

  freeStack(stackAdjust);

  if (dynamicAlignment_) {
    // While the x86 supports pop esp, on ARM that isn't well defined, so
    // just do it manually.
    as_dtr(IsLoad, 32, Offset, sp, DTRAddr(sp, DtrOffImm(0)));
  }

#ifdef DEBUG
  MOZ_ASSERT(inCall_);
  inCall_ = false;
#endif
}

void MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result) {
  // Load the callee in r12, as above.
  ma_mov(fun, r12);
  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(r12);
  callWithABIPost(stackAdjust, result);
}

void MacroAssembler::callWithABINoProfiler(const Address& fun,
                                           MoveOp::Type result) {
  // Load the callee in r12, no instruction between the ldr and call should
  // clobber it. Note that we can't use fun.base because it may be one of the
  // IntArg registers clobbered before the call.
  {
    ScratchRegisterScope scratch(*this);
    ma_ldr(fun, r12, scratch);
  }
  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(r12);
  callWithABIPost(stackAdjust, result);
}

// ===============================================================
// Jit Frames.

uint32_t MacroAssembler::pushFakeReturnAddress(Register scratch) {
  // On ARM any references to the pc, adds an additional 8 to it, which
  // correspond to 2 instructions of 4 bytes.  Thus we use an additional nop
  // to pad until we reach the pushed pc.
  //
  // Note: In practice this should not be necessary, as this fake return
  // address is never used for resuming any execution. Thus theoriticaly we
  // could just do a Push(pc), and ignore the nop as well as the pool.
  enterNoPool(2);
  DebugOnly<uint32_t> offsetBeforePush = currentOffset();
  Push(pc);  // actually pushes $pc + 8.
  ma_nop();
  uint32_t pseudoReturnOffset = currentOffset();
  leaveNoPool();

  MOZ_ASSERT_IF(!oom(), pseudoReturnOffset - offsetBeforePush == 8);
  return pseudoReturnOffset;
}

void MacroAssembler::enterFakeExitFrameForWasm(Register cxreg, Register scratch,
                                               ExitFrameType type) {
  enterFakeExitFrame(cxreg, scratch, type);
}

// ===============================================================
// Move instructions

void MacroAssembler::moveValue(const TypedOrValueRegister& src,
                               const ValueOperand& dest) {
  if (src.hasValue()) {
    moveValue(src.valueReg(), dest);
    return;
  }

  MIRType type = src.type();
  AnyRegister reg = src.typedReg();

  if (!IsFloatingPointType(type)) {
    if (reg.gpr() != dest.payloadReg()) {
      mov(reg.gpr(), dest.payloadReg());
    }
    mov(ImmWord(MIRTypeToTag(type)), dest.typeReg());
    return;
  }

  ScratchFloat32Scope scratch(*this);
  FloatRegister freg = reg.fpu();
  if (type == MIRType::Float32) {
    convertFloat32ToDouble(freg, scratch);
    freg = scratch;
  }
  ma_vxfer(freg, dest.payloadReg(), dest.typeReg());
}

void MacroAssembler::moveValue(const ValueOperand& src,
                               const ValueOperand& dest) {
  Register s0 = src.typeReg();
  Register s1 = src.payloadReg();
  Register d0 = dest.typeReg();
  Register d1 = dest.payloadReg();

  // Either one or both of the source registers could be the same as a
  // destination register.
  if (s1 == d0) {
    if (s0 == d1) {
      // If both are, this is just a swap of two registers.
      ScratchRegisterScope scratch(*this);
      MOZ_ASSERT(d1 != scratch);
      MOZ_ASSERT(d0 != scratch);
      ma_mov(d1, scratch);
      ma_mov(d0, d1);
      ma_mov(scratch, d0);
      return;
    }
    // If only one is, copy that source first.
    std::swap(s0, s1);
    std::swap(d0, d1);
  }

  if (s0 != d0) {
    ma_mov(s0, d0);
  }
  if (s1 != d1) {
    ma_mov(s1, d1);
  }
}

void MacroAssembler::moveValue(const Value& src, const ValueOperand& dest) {
  ma_mov(Imm32(src.toNunboxTag()), dest.typeReg());
  if (src.isGCThing()) {
    ma_mov(ImmGCPtr(src.toGCThing()), dest.payloadReg());
  } else {
    ma_mov(Imm32(src.toNunboxPayload()), dest.payloadReg());
  }
}

// ===============================================================
// Branch functions

void MacroAssembler::loadStoreBuffer(Register ptr, Register buffer) {
  ma_lsr(Imm32(gc::ChunkShift), ptr, buffer);
  ma_lsl(Imm32(gc::ChunkShift), buffer, buffer);
  load32(Address(buffer, gc::ChunkStoreBufferOffset), buffer);
}

void MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr,
                                             Register temp, Label* label) {
  Maybe<SecondScratchRegisterScope> scratch2;
  if (temp == Register::Invalid()) {
    scratch2.emplace(*this);
    temp = scratch2.ref();
  }

  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  MOZ_ASSERT(ptr != temp);

  ma_lsr(Imm32(gc::ChunkShift), ptr, temp);
  ma_lsl(Imm32(gc::ChunkShift), temp, temp);
  loadPtr(Address(temp, gc::ChunkStoreBufferOffset), temp);
  branchPtr(InvertCondition(cond), temp, ImmWord(0), label);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              const Address& address,
                                              Register temp, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  Label done;

  branchTestGCThing(Assembler::NotEqual, address,
                    cond == Assembler::Equal ? &done : label);

  loadPtr(ToPayload(address), temp);
  SecondScratchRegisterScope scratch2(*this);
  branchPtrInNurseryChunk(cond, temp, scratch2, label);

  bind(&done);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              ValueOperand value, Register temp,
                                              Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  Label done;

  branchTestGCThing(Assembler::NotEqual, value,
                    cond == Assembler::Equal ? &done : label);
  branchPtrInNurseryChunk(cond, value.payloadReg(), temp, label);

  bind(&done);
}

void MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                     const Value& rhs, Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  // If cond == NotEqual, branch when a.payload != b.payload || a.tag !=
  // b.tag. If the payloads are equal, compare the tags. If the payloads are
  // not equal, short circuit true (NotEqual).
  //
  // If cand == Equal, branch when a.payload == b.payload && a.tag == b.tag.
  // If the payloads are equal, compare the tags. If the payloads are not
  // equal, short circuit false (NotEqual).
  ScratchRegisterScope scratch(*this);

  if (rhs.isGCThing()) {
    ma_cmp(lhs.payloadReg(), ImmGCPtr(rhs.toGCThing()), scratch);
  } else {
    ma_cmp(lhs.payloadReg(), Imm32(rhs.toNunboxPayload()), scratch);
  }
  ma_cmp(lhs.typeReg(), Imm32(rhs.toNunboxTag()), scratch, Equal);
  ma_b(label, cond);
}

// ========================================================================
// Memory access primitives.
template <typename T>
void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                       MIRType valueType, const T& dest,
                                       MIRType slotType) {
  if (valueType == MIRType::Double) {
    storeDouble(value.reg().typedReg().fpu(), dest);
    return;
  }

  // Store the type tag if needed.
  if (valueType != slotType) {
    storeTypeTag(ImmType(ValueTypeFromMIRType(valueType)), dest);
  }

  // Store the payload.
  if (value.constant()) {
    storePayload(value.value(), dest);
  } else {
    storePayload(value.reg().typedReg().gpr(), dest);
  }
}

template void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                                MIRType valueType,
                                                const Address& dest,
                                                MIRType slotType);
template void MacroAssembler::storeUnboxedValue(
    const ConstantOrRegister& value, MIRType valueType,
    const BaseObjectElementIndex& dest, MIRType slotType);

CodeOffset MacroAssembler::wasmTrapInstruction() {
  return CodeOffset(as_illegal_trap().getOffset());
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Register boundsCheckLimit,
                                       Label* label) {
  as_cmp(index, O2Reg(boundsCheckLimit));
  as_b(label, cond);
  if (JitOptions.spectreIndexMasking) {
    ma_mov(boundsCheckLimit, index, LeaveCC, cond);
  }
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Address boundsCheckLimit, Label* label) {
  ScratchRegisterScope scratch(*this);
  ma_ldr(DTRAddr(boundsCheckLimit.base, DtrOffImm(boundsCheckLimit.offset)),
         scratch);
  as_cmp(index, O2Reg(scratch));
  as_b(label, cond);
  if (JitOptions.spectreIndexMasking) {
    ma_mov(scratch, index, LeaveCC, cond);
  }
}

void MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input,
                                                Register output,
                                                bool isSaturating,
                                                Label* oolEntry) {
  wasmTruncateToInt32(input, output, MIRType::Double, /* isUnsigned= */ true,
                      isSaturating, oolEntry);
}

void MacroAssembler::wasmTruncateDoubleToInt32(FloatRegister input,
                                               Register output,
                                               bool isSaturating,
                                               Label* oolEntry) {
  wasmTruncateToInt32(input, output, MIRType::Double, /* isUnsigned= */ false,
                      isSaturating, oolEntry);
}

void MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input,
                                                 Register output,
                                                 bool isSaturating,
                                                 Label* oolEntry) {
  wasmTruncateToInt32(input, output, MIRType::Float32, /* isUnsigned= */ true,
                      isSaturating, oolEntry);
}

void MacroAssembler::wasmTruncateFloat32ToInt32(FloatRegister input,
                                                Register output,
                                                bool isSaturating,
                                                Label* oolEntry) {
  wasmTruncateToInt32(input, output, MIRType::Float32, /* isUnsigned= */ false,
                      isSaturating, oolEntry);
}

void MacroAssembler::oolWasmTruncateCheckF32ToI32(FloatRegister input,
                                                  Register output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  outOfLineWasmTruncateToIntCheck(input, MIRType::Float32, MIRType::Int32,
                                  flags, rejoin, off);
}

void MacroAssembler::oolWasmTruncateCheckF64ToI32(FloatRegister input,
                                                  Register output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  outOfLineWasmTruncateToIntCheck(input, MIRType::Double, MIRType::Int32, flags,
                                  rejoin, off);
}

void MacroAssembler::oolWasmTruncateCheckF32ToI64(FloatRegister input,
                                                  Register64 output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  outOfLineWasmTruncateToIntCheck(input, MIRType::Float32, MIRType::Int64,
                                  flags, rejoin, off);
}

void MacroAssembler::oolWasmTruncateCheckF64ToI64(FloatRegister input,
                                                  Register64 output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  outOfLineWasmTruncateToIntCheck(input, MIRType::Double, MIRType::Int64, flags,
                                  rejoin, off);
}

void MacroAssembler::wasmLoad(const wasm::MemoryAccessDesc& access,
                              Register memoryBase, Register ptr,
                              Register ptrScratch, AnyRegister output) {
  wasmLoadImpl(access, memoryBase, ptr, ptrScratch, output,
               Register64::Invalid());
}

void MacroAssembler::wasmLoadI64(const wasm::MemoryAccessDesc& access,
                                 Register memoryBase, Register ptr,
                                 Register ptrScratch, Register64 output) {
  MOZ_ASSERT_IF(access.isAtomic(), access.byteSize() <= 4);
  wasmLoadImpl(access, memoryBase, ptr, ptrScratch, AnyRegister(), output);
}

void MacroAssembler::wasmStore(const wasm::MemoryAccessDesc& access,
                               AnyRegister value, Register memoryBase,
                               Register ptr, Register ptrScratch) {
  wasmStoreImpl(access, value, Register64::Invalid(), memoryBase, ptr,
                ptrScratch);
}

void MacroAssembler::wasmStoreI64(const wasm::MemoryAccessDesc& access,
                                  Register64 value, Register memoryBase,
                                  Register ptr, Register ptrScratch) {
  MOZ_ASSERT(!access.isAtomic());
  wasmStoreImpl(access, AnyRegister(), value, memoryBase, ptr, ptrScratch);
}

// ========================================================================
// Primitive atomic operations.

static Register ComputePointerForAtomic(MacroAssembler& masm,
                                        const BaseIndex& src, Register r) {
  Register base = src.base;
  Register index = src.index;
  uint32_t scale = Imm32::ShiftOf(src.scale).value;
  int32_t offset = src.offset;

  ScratchRegisterScope scratch(masm);

  masm.as_add(r, base, lsl(index, scale));
  if (offset != 0) {
    masm.ma_add(r, Imm32(offset), r, scratch);
  }
  return r;
}

static Register ComputePointerForAtomic(MacroAssembler& masm,
                                        const Address& src, Register r) {
  ScratchRegisterScope scratch(masm);
  if (src.offset == 0) {
    return src.base;
  }
  masm.ma_add(src.base, Imm32(src.offset), r, scratch);
  return r;
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

template <typename T>
static void CompareExchange(MacroAssembler& masm,
                            const wasm::MemoryAccessDesc* access,
                            Scalar::Type type, const Synchronization& sync,
                            const T& mem, Register oldval, Register newval,
                            Register output) {
  bool signExtend = Scalar::isSignedIntType(type);
  unsigned nbytes = Scalar::byteSize(type);

  MOZ_ASSERT(nbytes <= 4);

  Label again;
  Label done;

  SecondScratchRegisterScope scratch2(masm);
  Register ptr = ComputePointerForAtomic(masm, mem, scratch2);

  ScratchRegisterScope scratch(masm);

  masm.memoryBarrierBefore(sync);

  masm.bind(&again);

  BufferOffset firstAccess;
  switch (nbytes) {
    case 1:
      firstAccess = masm.as_ldrexb(output, ptr);
      if (signExtend) {
        masm.as_sxtb(output, output, 0);
        masm.as_sxtb(scratch, oldval, 0);
      } else {
        masm.as_uxtb(scratch, oldval, 0);
      }
      break;
    case 2:
      firstAccess = masm.as_ldrexh(output, ptr);
      if (signExtend) {
        masm.as_sxth(output, output, 0);
        masm.as_sxth(scratch, oldval, 0);
      } else {
        masm.as_uxth(scratch, oldval, 0);
      }
      break;
    case 4:
      firstAccess = masm.as_ldrex(output, ptr);
      break;
  }
  if (access) {
    masm.append(*access, firstAccess.getOffset());
  }

  if (nbytes < 4) {
    masm.as_cmp(output, O2Reg(scratch));
  } else {
    masm.as_cmp(output, O2Reg(oldval));
  }
  masm.as_b(&done, MacroAssembler::NotEqual);
  switch (nbytes) {
    case 1:
      masm.as_strexb(scratch, newval, ptr);
      break;
    case 2:
      masm.as_strexh(scratch, newval, ptr);
      break;
    case 4:
      masm.as_strex(scratch, newval, ptr);
      break;
  }
  masm.as_cmp(scratch, Imm8(1));
  masm.as_b(&again, MacroAssembler::Equal);
  masm.bind(&done);

  masm.memoryBarrierAfter(sync);
}

void MacroAssembler::compareExchange(Scalar::Type type,
                                     const Synchronization& sync,
                                     const Address& address, Register oldval,
                                     Register newval, Register output) {
  CompareExchange(*this, nullptr, type, sync, address, oldval, newval, output);
}

void MacroAssembler::compareExchange(Scalar::Type type,
                                     const Synchronization& sync,
                                     const BaseIndex& address, Register oldval,
                                     Register newval, Register output) {
  CompareExchange(*this, nullptr, type, sync, address, oldval, newval, output);
}

void MacroAssembler::wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                                         const Address& mem, Register oldval,
                                         Register newval, Register output) {
  CompareExchange(*this, &access, access.type(), access.sync(), mem, oldval,
                  newval, output);
}

void MacroAssembler::wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                                         const BaseIndex& mem, Register oldval,
                                         Register newval, Register output) {
  CompareExchange(*this, &access, access.type(), access.sync(), mem, oldval,
                  newval, output);
}

template <typename T>
static void AtomicExchange(MacroAssembler& masm,
                           const wasm::MemoryAccessDesc* access,
                           Scalar::Type type, const Synchronization& sync,
                           const T& mem, Register value, Register output) {
  bool signExtend = Scalar::isSignedIntType(type);
  unsigned nbytes = Scalar::byteSize(type);

  MOZ_ASSERT(nbytes <= 4);

  // Bug 1077321: We may further optimize for ARMv8 (AArch32) here.
  Label again;
  Label done;

  SecondScratchRegisterScope scratch2(masm);
  Register ptr = ComputePointerForAtomic(masm, mem, scratch2);

  ScratchRegisterScope scratch(masm);

  masm.memoryBarrierBefore(sync);

  masm.bind(&again);

  BufferOffset firstAccess;
  switch (nbytes) {
    case 1:
      firstAccess = masm.as_ldrexb(output, ptr);
      if (signExtend) {
        masm.as_sxtb(output, output, 0);
      }
      masm.as_strexb(scratch, value, ptr);
      break;
    case 2:
      firstAccess = masm.as_ldrexh(output, ptr);
      if (signExtend) {
        masm.as_sxth(output, output, 0);
      }
      masm.as_strexh(scratch, value, ptr);
      break;
    case 4:
      firstAccess = masm.as_ldrex(output, ptr);
      masm.as_strex(scratch, value, ptr);
      break;
  }
  if (access) {
    masm.append(*access, firstAccess.getOffset());
  }

  masm.as_cmp(scratch, Imm8(1));
  masm.as_b(&again, MacroAssembler::Equal);
  masm.bind(&done);

  masm.memoryBarrierAfter(sync);
}

void MacroAssembler::atomicExchange(Scalar::Type type,
                                    const Synchronization& sync,
                                    const Address& address, Register value,
                                    Register output) {
  AtomicExchange(*this, nullptr, type, sync, address, value, output);
}

void MacroAssembler::atomicExchange(Scalar::Type type,
                                    const Synchronization& sync,
                                    const BaseIndex& address, Register value,
                                    Register output) {
  AtomicExchange(*this, nullptr, type, sync, address, value, output);
}

void MacroAssembler::wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                                        const Address& mem, Register value,
                                        Register output) {
  AtomicExchange(*this, &access, access.type(), access.sync(), mem, value,
                 output);
}

void MacroAssembler::wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                                        const BaseIndex& mem, Register value,
                                        Register output) {
  AtomicExchange(*this, &access, access.type(), access.sync(), mem, value,
                 output);
}

// General algorithm:
//
//     ...    ptr, <addr>         ; compute address of item
//     dmb
// L0  ldrex* output, [ptr]
//     sxt*   output, output, 0   ; sign-extend if applicable
//     OP     tmp, output, value  ; compute value to store
//     strex* tmp2, tmp, [ptr]    ; tmp2 required by strex
//     cmp    tmp2, 1
//     beq    L0                  ; failed - location is dirty, retry
//     dmb                        ; ordering barrier required
//
// Also see notes above at compareExchange re the barrier strategy.
//
// Observe that the value being operated into the memory element need
// not be sign-extended because no OP will make use of bits to the
// left of the bits indicated by the width of the element, and neither
// output nor the bits stored are affected by OP.

template <typename T>
static void AtomicFetchOp(MacroAssembler& masm,
                          const wasm::MemoryAccessDesc* access,
                          Scalar::Type type, const Synchronization& sync,
                          AtomicOp op, const Register& value, const T& mem,
                          Register flagTemp, Register output) {
  bool signExtend = Scalar::isSignedIntType(type);
  unsigned nbytes = Scalar::byteSize(type);

  MOZ_ASSERT(nbytes <= 4);
  MOZ_ASSERT(flagTemp != InvalidReg);
  MOZ_ASSERT(output != value);

  Label again;

  SecondScratchRegisterScope scratch2(masm);
  Register ptr = ComputePointerForAtomic(masm, mem, scratch2);

  masm.memoryBarrierBefore(sync);

  ScratchRegisterScope scratch(masm);

  masm.bind(&again);

  BufferOffset firstAccess;
  switch (nbytes) {
    case 1:
      firstAccess = masm.as_ldrexb(output, ptr);
      if (signExtend) {
        masm.as_sxtb(output, output, 0);
      }
      break;
    case 2:
      firstAccess = masm.as_ldrexh(output, ptr);
      if (signExtend) {
        masm.as_sxth(output, output, 0);
      }
      break;
    case 4:
      firstAccess = masm.as_ldrex(output, ptr);
      break;
  }
  if (access) {
    masm.append(*access, firstAccess.getOffset());
  }

  switch (op) {
    case AtomicFetchAddOp:
      masm.as_add(scratch, output, O2Reg(value));
      break;
    case AtomicFetchSubOp:
      masm.as_sub(scratch, output, O2Reg(value));
      break;
    case AtomicFetchAndOp:
      masm.as_and(scratch, output, O2Reg(value));
      break;
    case AtomicFetchOrOp:
      masm.as_orr(scratch, output, O2Reg(value));
      break;
    case AtomicFetchXorOp:
      masm.as_eor(scratch, output, O2Reg(value));
      break;
  }
  // Rd must differ from the two other arguments to strex.
  switch (nbytes) {
    case 1:
      masm.as_strexb(flagTemp, scratch, ptr);
      break;
    case 2:
      masm.as_strexh(flagTemp, scratch, ptr);
      break;
    case 4:
      masm.as_strex(flagTemp, scratch, ptr);
      break;
  }
  masm.as_cmp(flagTemp, Imm8(1));
  masm.as_b(&again, MacroAssembler::Equal);

  masm.memoryBarrierAfter(sync);
}

void MacroAssembler::atomicFetchOp(Scalar::Type type,
                                   const Synchronization& sync, AtomicOp op,
                                   Register value, const Address& mem,
                                   Register temp, Register output) {
  AtomicFetchOp(*this, nullptr, type, sync, op, value, mem, temp, output);
}

void MacroAssembler::atomicFetchOp(Scalar::Type type,
                                   const Synchronization& sync, AtomicOp op,
                                   Register value, const BaseIndex& mem,
                                   Register temp, Register output) {
  AtomicFetchOp(*this, nullptr, type, sync, op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access,
                                       AtomicOp op, Register value,
                                       const Address& mem, Register temp,
                                       Register output) {
  AtomicFetchOp(*this, &access, access.type(), access.sync(), op, value, mem,
                temp, output);
}

void MacroAssembler::wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access,
                                       AtomicOp op, Register value,
                                       const BaseIndex& mem, Register temp,
                                       Register output) {
  AtomicFetchOp(*this, &access, access.type(), access.sync(), op, value, mem,
                temp, output);
}

// Uses both scratch registers, one for the address and one for a temp,
// but needs two temps for strex:
//
//     ...    ptr, <addr>         ; compute address of item
//     dmb
// L0  ldrex* temp, [ptr]
//     OP     temp, temp, value   ; compute value to store
//     strex* temp2, temp, [ptr]
//     cmp    temp2, 1
//     beq    L0                  ; failed - location is dirty, retry
//     dmb                        ; ordering barrier required

template <typename T>
static void AtomicEffectOp(MacroAssembler& masm,
                           const wasm::MemoryAccessDesc* access,
                           Scalar::Type type, const Synchronization& sync,
                           AtomicOp op, const Register& value, const T& mem,
                           Register flagTemp) {
  unsigned nbytes = Scalar::byteSize(type);

  MOZ_ASSERT(nbytes <= 4);
  MOZ_ASSERT(flagTemp != InvalidReg);

  Label again;

  SecondScratchRegisterScope scratch2(masm);
  Register ptr = ComputePointerForAtomic(masm, mem, scratch2);

  masm.memoryBarrierBefore(sync);

  ScratchRegisterScope scratch(masm);

  masm.bind(&again);

  BufferOffset firstAccess;
  switch (nbytes) {
    case 1:
      firstAccess = masm.as_ldrexb(scratch, ptr);
      break;
    case 2:
      firstAccess = masm.as_ldrexh(scratch, ptr);
      break;
    case 4:
      firstAccess = masm.as_ldrex(scratch, ptr);
      break;
  }
  if (access) {
    masm.append(*access, firstAccess.getOffset());
  }

  switch (op) {
    case AtomicFetchAddOp:
      masm.as_add(scratch, scratch, O2Reg(value));
      break;
    case AtomicFetchSubOp:
      masm.as_sub(scratch, scratch, O2Reg(value));
      break;
    case AtomicFetchAndOp:
      masm.as_and(scratch, scratch, O2Reg(value));
      break;
    case AtomicFetchOrOp:
      masm.as_orr(scratch, scratch, O2Reg(value));
      break;
    case AtomicFetchXorOp:
      masm.as_eor(scratch, scratch, O2Reg(value));
      break;
  }
  // Rd must differ from the two other arguments to strex.
  switch (nbytes) {
    case 1:
      masm.as_strexb(flagTemp, scratch, ptr);
      break;
    case 2:
      masm.as_strexh(flagTemp, scratch, ptr);
      break;
    case 4:
      masm.as_strex(flagTemp, scratch, ptr);
      break;
  }
  masm.as_cmp(flagTemp, Imm8(1));
  masm.as_b(&again, MacroAssembler::Equal);

  masm.memoryBarrierAfter(sync);
}

void MacroAssembler::wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access,
                                        AtomicOp op, Register value,
                                        const Address& mem, Register temp) {
  AtomicEffectOp(*this, &access, access.type(), access.sync(), op, value, mem,
                 temp);
}

void MacroAssembler::wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access,
                                        AtomicOp op, Register value,
                                        const BaseIndex& mem, Register temp) {
  AtomicEffectOp(*this, &access, access.type(), access.sync(), op, value, mem,
                 temp);
}

template <typename T>
static void AtomicLoad64(MacroAssembler& masm,
                         const wasm::MemoryAccessDesc* access,
                         const Synchronization& sync, const T& mem,
                         Register64 output) {
  MOZ_ASSERT((output.low.code() & 1) == 0);
  MOZ_ASSERT(output.low.code() + 1 == output.high.code());

  masm.memoryBarrierBefore(sync);

  SecondScratchRegisterScope scratch2(masm);
  Register ptr = ComputePointerForAtomic(masm, mem, scratch2);

  BufferOffset load = masm.as_ldrexd(output.low, output.high, ptr);
  if (access) {
    masm.append(*access, load.getOffset());
  }
  masm.as_clrex();

  masm.memoryBarrierAfter(sync);
}

template <typename T>
static void WasmAtomicLoad64(MacroAssembler& masm,
                             const wasm::MemoryAccessDesc& access, const T& mem,
                             Register64 temp, Register64 output) {
  MOZ_ASSERT(temp.low == InvalidReg && temp.high == InvalidReg);

  AtomicLoad64(masm, &access, access.sync(), mem, output);
}

void MacroAssembler::wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                                      const Address& mem, Register64 temp,
                                      Register64 output) {
  WasmAtomicLoad64(*this, access, mem, temp, output);
}

void MacroAssembler::wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                                      const BaseIndex& mem, Register64 temp,
                                      Register64 output) {
  WasmAtomicLoad64(*this, access, mem, temp, output);
}

template <typename T>
static void CompareExchange64(MacroAssembler& masm,
                              const wasm::MemoryAccessDesc* access,
                              const Synchronization& sync, const T& mem,
                              Register64 expect, Register64 replace,
                              Register64 output) {
  MOZ_ASSERT(expect != replace && replace != output && output != expect);

  MOZ_ASSERT((replace.low.code() & 1) == 0);
  MOZ_ASSERT(replace.low.code() + 1 == replace.high.code());

  MOZ_ASSERT((output.low.code() & 1) == 0);
  MOZ_ASSERT(output.low.code() + 1 == output.high.code());

  Label again;
  Label done;

  SecondScratchRegisterScope scratch2(masm);
  Register ptr = ComputePointerForAtomic(masm, mem, scratch2);

  masm.memoryBarrierBefore(sync);

  masm.bind(&again);
  BufferOffset load = masm.as_ldrexd(output.low, output.high, ptr);
  if (access) {
    masm.append(*access, load.getOffset());
  }

  masm.as_cmp(output.low, O2Reg(expect.low));
  masm.as_cmp(output.high, O2Reg(expect.high), MacroAssembler::Equal);
  masm.as_b(&done, MacroAssembler::NotEqual);

  ScratchRegisterScope scratch(masm);

  // Rd (temp) must differ from the two other arguments to strex.
  masm.as_strexd(scratch, replace.low, replace.high, ptr);
  masm.as_cmp(scratch, Imm8(1));
  masm.as_b(&again, MacroAssembler::Equal);
  masm.bind(&done);

  masm.memoryBarrierAfter(sync);
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const Address& mem,
                                           Register64 expect,
                                           Register64 replace,
                                           Register64 output) {
  CompareExchange64(*this, &access, access.sync(), mem, expect, replace,
                    output);
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const BaseIndex& mem,
                                           Register64 expect,
                                           Register64 replace,
                                           Register64 output) {
  CompareExchange64(*this, &access, access.sync(), mem, expect, replace,
                    output);
}

void MacroAssembler::compareExchange64(const Synchronization& sync,
                                       const Address& mem, Register64 expect,
                                       Register64 replace, Register64 output) {
  CompareExchange64(*this, nullptr, sync, mem, expect, replace, output);
}

void MacroAssembler::compareExchange64(const Synchronization& sync,
                                       const BaseIndex& mem, Register64 expect,
                                       Register64 replace, Register64 output) {
  CompareExchange64(*this, nullptr, sync, mem, expect, replace, output);
}

template <typename T>
static void AtomicExchange64(MacroAssembler& masm,
                             const wasm::MemoryAccessDesc* access,
                             const Synchronization& sync, const T& mem,
                             Register64 value, Register64 output) {
  MOZ_ASSERT(output != value);

  MOZ_ASSERT((value.low.code() & 1) == 0);
  MOZ_ASSERT(value.low.code() + 1 == value.high.code());

  MOZ_ASSERT((output.low.code() & 1) == 0);
  MOZ_ASSERT(output.low.code() + 1 == output.high.code());

  Label again;

  SecondScratchRegisterScope scratch2(masm);
  Register ptr = ComputePointerForAtomic(masm, mem, scratch2);

  masm.memoryBarrierBefore(sync);

  masm.bind(&again);
  BufferOffset load = masm.as_ldrexd(output.low, output.high, ptr);
  if (access) {
    masm.append(*access, load.getOffset());
  }

  ScratchRegisterScope scratch(masm);

  masm.as_strexd(scratch, value.low, value.high, ptr);
  masm.as_cmp(scratch, Imm8(1));
  masm.as_b(&again, MacroAssembler::Equal);

  masm.memoryBarrierAfter(sync);
}

template <typename T>
static void WasmAtomicExchange64(MacroAssembler& masm,
                                 const wasm::MemoryAccessDesc& access,
                                 const T& mem, Register64 value,
                                 Register64 output) {
  AtomicExchange64(masm, &access, access.sync(), mem, value, output);
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const Address& mem, Register64 value,
                                          Register64 output) {
  WasmAtomicExchange64(*this, access, mem, value, output);
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const BaseIndex& mem,
                                          Register64 value, Register64 output) {
  WasmAtomicExchange64(*this, access, mem, value, output);
}

void MacroAssembler::atomicExchange64(const Synchronization& sync,
                                      const Address& mem, Register64 value,
                                      Register64 output) {
  AtomicExchange64(*this, nullptr, sync, mem, value, output);
}

void MacroAssembler::atomicExchange64(const Synchronization& sync,
                                      const BaseIndex& mem, Register64 value,
                                      Register64 output) {
  AtomicExchange64(*this, nullptr, sync, mem, value, output);
}

template <typename T>
static void AtomicFetchOp64(MacroAssembler& masm,
                            const wasm::MemoryAccessDesc* access,
                            const Synchronization& sync, AtomicOp op,
                            Register64 value, const T& mem, Register64 temp,
                            Register64 output) {
  MOZ_ASSERT(temp.low != InvalidReg && temp.high != InvalidReg);
  MOZ_ASSERT(output != value);
  MOZ_ASSERT(temp != value);

  MOZ_ASSERT((temp.low.code() & 1) == 0);
  MOZ_ASSERT(temp.low.code() + 1 == temp.high.code());

  // We could avoid this pair requirement but in that case we would end up
  // with two moves in the loop to preserve the loaded value in output.  The
  // prize would be less register spilling around this op since the pair
  // requirement will tend to force more spilling.

  MOZ_ASSERT((output.low.code() & 1) == 0);
  MOZ_ASSERT(output.low.code() + 1 == output.high.code());

  Label again;

  SecondScratchRegisterScope scratch2(masm);
  Register ptr = ComputePointerForAtomic(masm, mem, scratch2);

  masm.memoryBarrierBefore(sync);

  masm.bind(&again);
  BufferOffset load = masm.as_ldrexd(output.low, output.high, ptr);
  if (access) {
    masm.append(*access, load.getOffset());
  }
  switch (op) {
    case AtomicFetchAddOp:
      masm.as_add(temp.low, output.low, O2Reg(value.low), SetCC);
      masm.as_adc(temp.high, output.high, O2Reg(value.high));
      break;
    case AtomicFetchSubOp:
      masm.as_sub(temp.low, output.low, O2Reg(value.low), SetCC);
      masm.as_sbc(temp.high, output.high, O2Reg(value.high));
      break;
    case AtomicFetchAndOp:
      masm.as_and(temp.low, output.low, O2Reg(value.low));
      masm.as_and(temp.high, output.high, O2Reg(value.high));
      break;
    case AtomicFetchOrOp:
      masm.as_orr(temp.low, output.low, O2Reg(value.low));
      masm.as_orr(temp.high, output.high, O2Reg(value.high));
      break;
    case AtomicFetchXorOp:
      masm.as_eor(temp.low, output.low, O2Reg(value.low));
      masm.as_eor(temp.high, output.high, O2Reg(value.high));
      break;
  }

  ScratchRegisterScope scratch(masm);

  // Rd (temp) must differ from the two other arguments to strex.
  masm.as_strexd(scratch, temp.low, temp.high, ptr);
  masm.as_cmp(scratch, Imm8(1));
  masm.as_b(&again, MacroAssembler::Equal);

  masm.memoryBarrierAfter(sync);
}

template <typename T>
static void WasmAtomicFetchOp64(MacroAssembler& masm,
                                const wasm::MemoryAccessDesc& access,
                                AtomicOp op, Register64 value, const T& mem,
                                Register64 temp, Register64 output) {
  AtomicFetchOp64(masm, &access, access.sync(), op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const Address& mem, Register64 temp,
                                         Register64 output) {
  WasmAtomicFetchOp64(*this, access, op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const BaseIndex& mem, Register64 temp,
                                         Register64 output) {
  WasmAtomicFetchOp64(*this, access, op, value, mem, temp, output);
}

void MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                                     Register64 value, const Address& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp64(*this, nullptr, sync, op, value, mem, temp, output);
}

void MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                                     Register64 value, const BaseIndex& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp64(*this, nullptr, sync, op, value, mem, temp, output);
}

void MacroAssembler::atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                                      Register64 value, const Address& mem,
                                      Register64 temp) {
  AtomicFetchOp64(*this, nullptr, sync, op, value, mem, temp, temp);
}

void MacroAssembler::atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                                      Register64 value, const BaseIndex& mem,
                                      Register64 temp) {
  AtomicFetchOp64(*this, nullptr, sync, op, value, mem, temp, temp);
}

// ========================================================================
// JS atomic operations.

template <typename T>
static void CompareExchangeJS(MacroAssembler& masm, Scalar::Type arrayType,
                              const Synchronization& sync, const T& mem,
                              Register oldval, Register newval, Register temp,
                              AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.compareExchange(arrayType, sync, mem, oldval, newval, temp);
    masm.convertUInt32ToDouble(temp, output.fpu());
  } else {
    masm.compareExchange(arrayType, sync, mem, oldval, newval, output.gpr());
  }
}

void MacroAssembler::compareExchangeJS(Scalar::Type arrayType,
                                       const Synchronization& sync,
                                       const Address& mem, Register oldval,
                                       Register newval, Register temp,
                                       AnyRegister output) {
  CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

void MacroAssembler::compareExchangeJS(Scalar::Type arrayType,
                                       const Synchronization& sync,
                                       const BaseIndex& mem, Register oldval,
                                       Register newval, Register temp,
                                       AnyRegister output) {
  CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

template <typename T>
static void AtomicExchangeJS(MacroAssembler& masm, Scalar::Type arrayType,
                             const Synchronization& sync, const T& mem,
                             Register value, Register temp,
                             AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.atomicExchange(arrayType, sync, mem, value, temp);
    masm.convertUInt32ToDouble(temp, output.fpu());
  } else {
    masm.atomicExchange(arrayType, sync, mem, value, output.gpr());
  }
}

void MacroAssembler::atomicExchangeJS(Scalar::Type arrayType,
                                      const Synchronization& sync,
                                      const Address& mem, Register value,
                                      Register temp, AnyRegister output) {
  AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

void MacroAssembler::atomicExchangeJS(Scalar::Type arrayType,
                                      const Synchronization& sync,
                                      const BaseIndex& mem, Register value,
                                      Register temp, AnyRegister output) {
  AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

template <typename T>
static void AtomicFetchOpJS(MacroAssembler& masm, Scalar::Type arrayType,
                            const Synchronization& sync, AtomicOp op,
                            Register value, const T& mem, Register temp1,
                            Register temp2, AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp2, temp1);
    masm.convertUInt32ToDouble(temp1, output.fpu());
  } else {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp1, output.gpr());
  }
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     const Synchronization& sync, AtomicOp op,
                                     Register value, const Address& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     const Synchronization& sync, AtomicOp op,
                                     Register value, const BaseIndex& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType,
                                      const Synchronization& sync, AtomicOp op,
                                      Register value, const BaseIndex& mem,
                                      Register temp) {
  AtomicEffectOp(*this, nullptr, arrayType, sync, op, value, mem, temp);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType,
                                      const Synchronization& sync, AtomicOp op,
                                      Register value, const Address& mem,
                                      Register temp) {
  AtomicEffectOp(*this, nullptr, arrayType, sync, op, value, mem, temp);
}

// ========================================================================
// Primitive atomic operations.

void MacroAssembler::atomicLoad64(const Synchronization& sync,
                                  const Address& mem, Register64 output) {
  AtomicLoad64(*this, nullptr, sync, mem, output);
}

void MacroAssembler::atomicLoad64(const Synchronization& sync,
                                  const BaseIndex& mem, Register64 output) {
  AtomicLoad64(*this, nullptr, sync, mem, output);
}

void MacroAssembler::atomicStore64(const Synchronization& sync,
                                   const Address& mem, Register64 value,
                                   Register64 temp) {
  AtomicExchange64(*this, nullptr, sync, mem, value, temp);
}

void MacroAssembler::atomicStore64(const Synchronization& sync,
                                   const BaseIndex& mem, Register64 value,
                                   Register64 temp) {
  AtomicExchange64(*this, nullptr, sync, mem, value, temp);
}

// ========================================================================
// Convert floating point.

bool MacroAssembler::convertUInt64ToDoubleNeedsTemp() { return false; }

void MacroAssembler::convertUInt64ToDouble(Register64 src, FloatRegister dest,
                                           Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  ScratchDoubleScope scratchDouble(*this);

  convertUInt32ToDouble(src.high, dest);
  {
    ScratchRegisterScope scratch(*this);
    movePtr(ImmPtr(&TO_DOUBLE_HIGH_SCALE), scratch);
    ma_vldr(Operand(Address(scratch, 0)).toVFPAddr(), scratchDouble);
  }
  mulDouble(scratchDouble, dest);
  convertUInt32ToDouble(src.low, scratchDouble);
  addDouble(scratchDouble, dest);
}

void MacroAssembler::convertInt64ToDouble(Register64 src, FloatRegister dest) {
  ScratchDoubleScope scratchDouble(*this);

  convertInt32ToDouble(src.high, dest);
  {
    ScratchRegisterScope scratch(*this);
    movePtr(ImmPtr(&TO_DOUBLE_HIGH_SCALE), scratch);
    ma_vldr(Operand(Address(scratch, 0)).toVFPAddr(), scratchDouble);
  }
  mulDouble(scratchDouble, dest);
  convertUInt32ToDouble(src.low, scratchDouble);
  addDouble(scratchDouble, dest);
}

void MacroAssembler::convertIntPtrToDouble(Register src, FloatRegister dest) {
  convertInt32ToDouble(src, dest);
}

extern "C" {
extern MOZ_EXPORT int64_t __aeabi_idivmod(int, int);
extern MOZ_EXPORT int64_t __aeabi_uidivmod(int, int);
}

inline void EmitRemainderOrQuotient(bool isRemainder, MacroAssembler& masm,
                                    Register rhs, Register lhsOutput,
                                    bool isUnsigned,
                                    const LiveRegisterSet& volatileLiveRegs) {
  // Currently this helper can't handle this situation.
  MOZ_ASSERT(lhsOutput != rhs);

  if (HasIDIV()) {
    if (isRemainder) {
      masm.remainder32(rhs, lhsOutput, isUnsigned);
    } else {
      masm.quotient32(rhs, lhsOutput, isUnsigned);
    }
  } else {
    // Ensure that the output registers are saved and restored properly,
    MOZ_ASSERT(volatileLiveRegs.has(ReturnRegVal0));
    MOZ_ASSERT(volatileLiveRegs.has(ReturnRegVal1));

    masm.PushRegsInMask(volatileLiveRegs);
    using Fn = int64_t (*)(int, int);
    {
      ScratchRegisterScope scratch(masm);
      masm.setupUnalignedABICall(scratch);
    }
    masm.passABIArg(lhsOutput);
    masm.passABIArg(rhs);
    if (isUnsigned) {
      masm.callWithABI<Fn, __aeabi_uidivmod>(
          MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);
    } else {
      masm.callWithABI<Fn, __aeabi_idivmod>(
          MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);
    }
    if (isRemainder) {
      masm.mov(ReturnRegVal1, lhsOutput);
    } else {
      masm.mov(ReturnRegVal0, lhsOutput);
    }

    LiveRegisterSet ignore;
    ignore.add(lhsOutput);
    masm.PopRegsInMaskIgnore(volatileLiveRegs, ignore);
  }
}

void MacroAssembler::flexibleQuotient32(
    Register rhs, Register srcDest, bool isUnsigned,
    const LiveRegisterSet& volatileLiveRegs) {
  EmitRemainderOrQuotient(false, *this, rhs, srcDest, isUnsigned,
                          volatileLiveRegs);
}

void MacroAssembler::flexibleRemainder32(
    Register rhs, Register srcDest, bool isUnsigned,
    const LiveRegisterSet& volatileLiveRegs) {
  EmitRemainderOrQuotient(true, *this, rhs, srcDest, isUnsigned,
                          volatileLiveRegs);
}

void MacroAssembler::flexibleDivMod32(Register rhs, Register lhsOutput,
                                      Register remOutput, bool isUnsigned,
                                      const LiveRegisterSet& volatileLiveRegs) {
  // Currently this helper can't handle this situation.
  MOZ_ASSERT(lhsOutput != rhs);

  if (HasIDIV()) {
    mov(lhsOutput, remOutput);
    remainder32(rhs, remOutput, isUnsigned);
    quotient32(rhs, lhsOutput, isUnsigned);
  } else {
    // Ensure that the output registers are saved and restored properly,
    MOZ_ASSERT(volatileLiveRegs.has(ReturnRegVal0));
    MOZ_ASSERT(volatileLiveRegs.has(ReturnRegVal1));
    PushRegsInMask(volatileLiveRegs);

    using Fn = int64_t (*)(int, int);
    {
      ScratchRegisterScope scratch(*this);
      setupUnalignedABICall(scratch);
    }
    passABIArg(lhsOutput);
    passABIArg(rhs);
    if (isUnsigned) {
      callWithABI<Fn, __aeabi_uidivmod>(MoveOp::GENERAL,
                                        CheckUnsafeCallWithABI::DontCheckOther);
    } else {
      callWithABI<Fn, __aeabi_idivmod>(MoveOp::GENERAL,
                                       CheckUnsafeCallWithABI::DontCheckOther);
    }
    moveRegPair(ReturnRegVal0, ReturnRegVal1, lhsOutput, remOutput);

    LiveRegisterSet ignore;
    ignore.add(remOutput);
    ignore.add(lhsOutput);
    PopRegsInMaskIgnore(volatileLiveRegs, ignore);
  }
}

CodeOffset MacroAssembler::moveNearAddressWithPatch(Register dest) {
  return movWithPatch(ImmPtr(nullptr), dest);
}

void MacroAssembler::patchNearAddressMove(CodeLocationLabel loc,
                                          CodeLocationLabel target) {
  PatchDataWithValueCheck(loc, ImmPtr(target.raw()), ImmPtr(nullptr));
}

// ========================================================================
// Spectre Mitigations.

void MacroAssembler::speculationBarrier() {
  // Spectre mitigation recommended by ARM for cases where csel/cmov cannot be
  // used.
  as_csdb();
}

void MacroAssembler::floorFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail) {
  floorf(src, dest, fail);
}

void MacroAssembler::floorDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  floor(src, dest, fail);
}

void MacroAssembler::ceilFloat32ToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  ceilf(src, dest, fail);
}

void MacroAssembler::ceilDoubleToInt32(FloatRegister src, Register dest,
                                       Label* fail) {
  ceil(src, dest, fail);
}

void MacroAssembler::roundFloat32ToInt32(FloatRegister src, Register dest,
                                         FloatRegister temp, Label* fail) {
  roundf(src, dest, fail, temp);
}

void MacroAssembler::roundDoubleToInt32(FloatRegister src, Register dest,
                                        FloatRegister temp, Label* fail) {
  round(src, dest, fail, temp);
}

void MacroAssembler::truncFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail) {
  truncf(src, dest, fail);
}

void MacroAssembler::truncDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  trunc(src, dest, fail);
}

void MacroAssembler::nearbyIntDouble(RoundingMode mode, FloatRegister src,
                                     FloatRegister dest) {
  MOZ_CRASH("not supported on this platform");
}

void MacroAssembler::nearbyIntFloat32(RoundingMode mode, FloatRegister src,
                                      FloatRegister dest) {
  MOZ_CRASH("not supported on this platform");
}

void MacroAssembler::copySignDouble(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister output) {
  MOZ_CRASH("not supported on this platform");
}

//}}} check_macroassembler_style

void MacroAssemblerARM::wasmTruncateToInt32(FloatRegister input,
                                            Register output, MIRType fromType,
                                            bool isUnsigned, bool isSaturating,
                                            Label* oolEntry) {
  ScratchDoubleScope scratchScope(asMasm());
  ScratchRegisterScope scratchReg(asMasm());
  FloatRegister scratch = scratchScope.uintOverlay();

  // ARM conversion instructions clamp the value to ensure it fits within the
  // target's type bounds, so every time we see those, we need to check the
  // input. A NaN check is not necessary because NaN is converted to zero and
  // on a zero result we branch out of line to do further processing anyway.
  if (isUnsigned) {
    if (fromType == MIRType::Double) {
      ma_vcvt_F64_U32(input, scratch);
    } else if (fromType == MIRType::Float32) {
      ma_vcvt_F32_U32(input, scratch);
    } else {
      MOZ_CRASH("unexpected type in visitWasmTruncateToInt32");
    }

    ma_vxfer(scratch, output);

    if (!isSaturating) {
      // int32_t(UINT32_MAX) == -1.
      ma_cmp(output, Imm32(-1), scratchReg);
      as_cmp(output, Imm8(0), Assembler::NotEqual);
      ma_b(oolEntry, Assembler::Equal);
    }

    return;
  }

  // vcvt* converts NaN into 0, so check for NaNs here.
  if (!isSaturating) {
    if (fromType == MIRType::Double) {
      asMasm().compareDouble(input, input);
    } else if (fromType == MIRType::Float32) {
      asMasm().compareFloat(input, input);
    } else {
      MOZ_CRASH("unexpected type in visitWasmTruncateToInt32");
    }

    ma_b(oolEntry, Assembler::VFP_Unordered);
  }

  scratch = scratchScope.sintOverlay();

  if (fromType == MIRType::Double) {
    ma_vcvt_F64_I32(input, scratch);
  } else if (fromType == MIRType::Float32) {
    ma_vcvt_F32_I32(input, scratch);
  } else {
    MOZ_CRASH("unexpected type in visitWasmTruncateToInt32");
  }

  ma_vxfer(scratch, output);

  if (!isSaturating) {
    ma_cmp(output, Imm32(INT32_MAX), scratchReg);
    ma_cmp(output, Imm32(INT32_MIN), scratchReg, Assembler::NotEqual);
    ma_b(oolEntry, Assembler::Equal);
  }
}

void MacroAssemblerARM::outOfLineWasmTruncateToIntCheck(
    FloatRegister input, MIRType fromType, MIRType toType, TruncFlags flags,
    Label* rejoin, wasm::BytecodeOffset trapOffset) {
  // On ARM, saturating truncation codegen handles saturating itself rather
  // than relying on out-of-line fixup code.
  if (flags & TRUNC_SATURATING) {
    return;
  }

  bool isUnsigned = flags & TRUNC_UNSIGNED;
  ScratchDoubleScope scratchScope(asMasm());
  FloatRegister scratch;

  // Eagerly take care of NaNs.
  Label inputIsNaN;
  if (fromType == MIRType::Double) {
    asMasm().branchDouble(Assembler::DoubleUnordered, input, input,
                          &inputIsNaN);
  } else if (fromType == MIRType::Float32) {
    asMasm().branchFloat(Assembler::DoubleUnordered, input, input, &inputIsNaN);
  } else {
    MOZ_CRASH("unexpected type in visitOutOfLineWasmTruncateCheck");
  }

  // Handle special values.
  Label fail;

  // By default test for the following inputs and bail:
  // signed:   ] -Inf, INTXX_MIN - 1.0 ] and [ INTXX_MAX + 1.0 : +Inf [
  // unsigned: ] -Inf, -1.0 ] and [ UINTXX_MAX + 1.0 : +Inf [
  // Note: we cannot always represent those exact values. As a result
  // this changes the actual comparison a bit.
  double minValue, maxValue;
  Assembler::DoubleCondition minCond = Assembler::DoubleLessThanOrEqual;
  Assembler::DoubleCondition maxCond = Assembler::DoubleGreaterThanOrEqual;
  if (toType == MIRType::Int64) {
    if (isUnsigned) {
      minValue = -1;
      maxValue = double(UINT64_MAX) + 1.0;
    } else {
      // In the float32/double range there exists no value between
      // INT64_MIN and INT64_MIN - 1.0. Making INT64_MIN the lower-bound.
      minValue = double(INT64_MIN);
      minCond = Assembler::DoubleLessThan;
      maxValue = double(INT64_MAX) + 1.0;
    }
  } else {
    if (isUnsigned) {
      minValue = -1;
      maxValue = double(UINT32_MAX) + 1.0;
    } else {
      if (fromType == MIRType::Float32) {
        // In the float32 range there exists no value between
        // INT32_MIN and INT32_MIN - 1.0. Making INT32_MIN the lower-bound.
        minValue = double(INT32_MIN);
        minCond = Assembler::DoubleLessThan;
      } else {
        minValue = double(INT32_MIN) - 1.0;
      }
      maxValue = double(INT32_MAX) + 1.0;
    }
  }

  if (fromType == MIRType::Double) {
    scratch = scratchScope.doubleOverlay();
    asMasm().loadConstantDouble(minValue, scratch);
    asMasm().branchDouble(minCond, input, scratch, &fail);

    asMasm().loadConstantDouble(maxValue, scratch);
    asMasm().branchDouble(maxCond, input, scratch, &fail);
  } else {
    MOZ_ASSERT(fromType == MIRType::Float32);
    scratch = scratchScope.singleOverlay();
    asMasm().loadConstantFloat32(float(minValue), scratch);
    asMasm().branchFloat(minCond, input, scratch, &fail);

    asMasm().loadConstantFloat32(float(maxValue), scratch);
    asMasm().branchFloat(maxCond, input, scratch, &fail);
  }

  // We had an actual correct value, get back to where we were.
  ma_b(rejoin);

  // Handle errors.
  bind(&fail);
  asMasm().wasmTrap(wasm::Trap::IntegerOverflow, trapOffset);

  bind(&inputIsNaN);
  asMasm().wasmTrap(wasm::Trap::InvalidConversionToInteger, trapOffset);
}

void MacroAssemblerARM::wasmLoadImpl(const wasm::MemoryAccessDesc& access,
                                     Register memoryBase, Register ptr,
                                     Register ptrScratch, AnyRegister output,
                                     Register64 out64) {
  MOZ_ASSERT(ptr == ptrScratch);
  MOZ_ASSERT(!access.isZeroExtendSimd128Load());
  MOZ_ASSERT(!access.isSplatSimd128Load());
  MOZ_ASSERT(!access.isWidenSimd128Load());

  uint32_t offset = access.offset();
  MOZ_ASSERT(offset < asMasm().wasmMaxOffsetGuardLimit());

  Scalar::Type type = access.type();

  // Maybe add the offset.
  if (offset || type == Scalar::Int64) {
    ScratchRegisterScope scratch(asMasm());
    if (offset) {
      ma_add(Imm32(offset), ptr, scratch);
    }
  }

  bool isSigned = type == Scalar::Int8 || type == Scalar::Int16 ||
                  type == Scalar::Int32 || type == Scalar::Int64;
  unsigned byteSize = access.byteSize();

  asMasm().memoryBarrierBefore(access.sync());

  BufferOffset load;
  if (out64 != Register64::Invalid()) {
    if (type == Scalar::Int64) {
      static_assert(INT64LOW_OFFSET == 0);

      load = ma_dataTransferN(IsLoad, 32, /* signed = */ false, memoryBase, ptr,
                              out64.low);
      append(access, load.getOffset());

      as_add(ptr, ptr, Imm8(INT64HIGH_OFFSET));

      load =
          ma_dataTransferN(IsLoad, 32, isSigned, memoryBase, ptr, out64.high);
      append(access, load.getOffset());
    } else {
      load = ma_dataTransferN(IsLoad, byteSize * 8, isSigned, memoryBase, ptr,
                              out64.low);
      append(access, load.getOffset());

      if (isSigned) {
        ma_asr(Imm32(31), out64.low, out64.high);
      } else {
        ma_mov(Imm32(0), out64.high);
      }
    }
  } else {
    bool isFloat = output.isFloat();
    if (isFloat) {
      MOZ_ASSERT((byteSize == 4) == output.fpu().isSingle());
      ScratchRegisterScope scratch(asMasm());
      FloatRegister dest = output.fpu();
      ma_add(memoryBase, ptr, scratch);

      // FP loads can't use VLDR as that has stringent alignment checks and will
      // SIGBUS on unaligned accesses.  Choose a different strategy depending on
      // the available hardware. We don't gate Wasm on the presence of NEON.
      if (HasNEON()) {
        // NEON available: The VLD1 multiple-single-elements variant will only
        // trap if SCTRL.A==1, but we already assume (for integer accesses) that
        // the hardware/OS handles that transparently.
        //
        // An additional complication is that if we're targeting the high single
        // then an unaligned load is not possible, and we may need to go via the
        // FPR scratch.
        if (byteSize == 4 && dest.code() & 1) {
          ScratchFloat32Scope fscratch(asMasm());
          load = as_vldr_unaligned(fscratch, scratch);
          as_vmov(dest, fscratch);
        } else {
          load = as_vldr_unaligned(dest, scratch);
        }
      } else {
        // NEON not available: Load to GPR scratch, move to FPR destination.  We
        // don't have adjacent scratches for the f64, so use individual LDRs,
        // not LDRD.
        SecondScratchRegisterScope scratch2(asMasm());
        if (byteSize == 4) {
          load = as_dtr(IsLoad, 32, Offset, scratch2,
                        DTRAddr(scratch, DtrOffImm(0)), Always);
          as_vxfer(scratch2, InvalidReg, VFPRegister(dest), CoreToFloat,
                   Always);
        } else {
          // The trap information is associated with the load of the high word,
          // which must be done first.
          load = as_dtr(IsLoad, 32, Offset, scratch2,
                        DTRAddr(scratch, DtrOffImm(4)), Always);
          as_dtr(IsLoad, 32, Offset, scratch, DTRAddr(scratch, DtrOffImm(0)),
                 Always);
          as_vxfer(scratch, scratch2, VFPRegister(dest), CoreToFloat, Always);
        }
      }
      append(access, load.getOffset());
    } else {
      load = ma_dataTransferN(IsLoad, byteSize * 8, isSigned, memoryBase, ptr,
                              output.gpr());
      append(access, load.getOffset());
    }
  }

  asMasm().memoryBarrierAfter(access.sync());
}

void MacroAssemblerARM::wasmStoreImpl(const wasm::MemoryAccessDesc& access,
                                      AnyRegister value, Register64 val64,
                                      Register memoryBase, Register ptr,
                                      Register ptrScratch) {
  static_assert(INT64LOW_OFFSET == 0);
  static_assert(INT64HIGH_OFFSET == 4);

  MOZ_ASSERT(ptr == ptrScratch);

  uint32_t offset = access.offset();
  MOZ_ASSERT(offset < asMasm().wasmMaxOffsetGuardLimit());

  unsigned byteSize = access.byteSize();
  Scalar::Type type = access.type();

  // Maybe add the offset.
  if (offset || type == Scalar::Int64) {
    ScratchRegisterScope scratch(asMasm());
    // We need to store the high word of an Int64 first, so always adjust the
    // pointer to point to the high word in this case.  The adjustment is always
    // OK because wasmMaxOffsetGuardLimit is computed so that we can add up to
    // sizeof(LargestValue)-1 without skipping past the guard page, and we
    // assert above that offset < wasmMaxOffsetGuardLimit.
    if (type == Scalar::Int64) {
      offset += INT64HIGH_OFFSET;
    }
    if (offset) {
      ma_add(Imm32(offset), ptr, scratch);
    }
  }

  asMasm().memoryBarrierAfter(access.sync());

  BufferOffset store;
  if (type == Scalar::Int64) {
    store = ma_dataTransferN(IsStore, 32 /* bits */, /* signed */ false,
                             memoryBase, ptr, val64.high);
    append(access, store.getOffset());

    as_sub(ptr, ptr, Imm8(INT64HIGH_OFFSET));

    store = ma_dataTransferN(IsStore, 32 /* bits */, /* signed */ true,
                             memoryBase, ptr, val64.low);
    append(access, store.getOffset());
  } else {
    if (value.isFloat()) {
      ScratchRegisterScope scratch(asMasm());
      FloatRegister val = value.fpu();
      MOZ_ASSERT((byteSize == 4) == val.isSingle());
      ma_add(memoryBase, ptr, scratch);

      // See comments above at wasmLoadImpl for more about this logic.
      if (HasNEON()) {
        if (byteSize == 4 && (val.code() & 1)) {
          ScratchFloat32Scope fscratch(asMasm());
          as_vmov(fscratch, val);
          store = as_vstr_unaligned(fscratch, scratch);
        } else {
          store = as_vstr_unaligned(val, scratch);
        }
      } else {
        // NEON not available: Move FPR to GPR scratch, store GPR.  We have only
        // one scratch to hold the value, so for f64 we must do two separate
        // moves.  That's OK - this is really a corner case.  If we really cared
        // we would pass in a temp to avoid the second move.
        SecondScratchRegisterScope scratch2(asMasm());
        if (byteSize == 4) {
          as_vxfer(scratch2, InvalidReg, VFPRegister(val), FloatToCore, Always);
          store = as_dtr(IsStore, 32, Offset, scratch2,
                         DTRAddr(scratch, DtrOffImm(0)), Always);
        } else {
          // The trap information is associated with the store of the high word,
          // which must be done first.
          as_vxfer(scratch2, InvalidReg, VFPRegister(val).singleOverlay(1),
                   FloatToCore, Always);
          store = as_dtr(IsStore, 32, Offset, scratch2,
                         DTRAddr(scratch, DtrOffImm(4)), Always);
          as_vxfer(scratch2, InvalidReg, VFPRegister(val).singleOverlay(0),
                   FloatToCore, Always);
          as_dtr(IsStore, 32, Offset, scratch2, DTRAddr(scratch, DtrOffImm(0)),
                 Always);
        }
      }
      append(access, store.getOffset());
    } else {
      bool isSigned = type == Scalar::Uint32 ||
                      type == Scalar::Int32;  // see AsmJSStoreHeap;
      Register val = value.gpr();

      store = ma_dataTransferN(IsStore, 8 * byteSize /* bits */, isSigned,
                               memoryBase, ptr, val);
      append(access, store.getOffset());
    }
  }

  asMasm().memoryBarrierAfter(access.sync());
}
