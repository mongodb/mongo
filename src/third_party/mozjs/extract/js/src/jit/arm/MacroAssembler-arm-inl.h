/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_MacroAssembler_arm_inl_h
#define jit_arm_MacroAssembler_arm_inl_h

#include "jit/arm/MacroAssembler-arm.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style

void MacroAssembler::move64(Register64 src, Register64 dest) {
  move32(src.low, dest.low);
  move32(src.high, dest.high);
}

void MacroAssembler::move64(Imm64 imm, Register64 dest) {
  move32(Imm32(imm.value & 0xFFFFFFFFL), dest.low);
  move32(Imm32((imm.value >> 32) & 0xFFFFFFFFL), dest.high);
}

void MacroAssembler::moveFloat32ToGPR(FloatRegister src, Register dest) {
  ma_vxfer(src, dest);
}

void MacroAssembler::moveGPRToFloat32(Register src, FloatRegister dest) {
  ma_vxfer(src, dest);
}

void MacroAssembler::move8ZeroExtend(Register src, Register dest) {
  as_uxtb(dest, src, 0);
}

void MacroAssembler::move8SignExtend(Register src, Register dest) {
  as_sxtb(dest, src, 0);
}

void MacroAssembler::move16SignExtend(Register src, Register dest) {
  as_sxth(dest, src, 0);
}

void MacroAssembler::moveDoubleToGPR64(FloatRegister src, Register64 dest) {
  ma_vxfer(src, dest.low, dest.high);
}

void MacroAssembler::moveGPR64ToDouble(Register64 src, FloatRegister dest) {
  ma_vxfer(src.low, src.high, dest);
}

void MacroAssembler::move64To32(Register64 src, Register dest) {
  if (src.low != dest) {
    move32(src.low, dest);
  }
}

void MacroAssembler::move32To64ZeroExtend(Register src, Register64 dest) {
  if (src != dest.low) {
    move32(src, dest.low);
  }
  move32(Imm32(0), dest.high);
}

void MacroAssembler::move8To64SignExtend(Register src, Register64 dest) {
  as_sxtb(dest.low, src, 0);
  ma_asr(Imm32(31), dest.low, dest.high);
}

void MacroAssembler::move16To64SignExtend(Register src, Register64 dest) {
  as_sxth(dest.low, src, 0);
  ma_asr(Imm32(31), dest.low, dest.high);
}

void MacroAssembler::move32To64SignExtend(Register src, Register64 dest) {
  if (src != dest.low) {
    move32(src, dest.low);
  }
  ma_asr(Imm32(31), dest.low, dest.high);
}

void MacroAssembler::move32SignExtendToPtr(Register src, Register dest) {
  move32(src, dest);
}

void MacroAssembler::move32ZeroExtendToPtr(Register src, Register dest) {
  move32(src, dest);
}

// ===============================================================
// Load instructions

void MacroAssembler::load32SignExtendToPtr(const Address& src, Register dest) {
  load32(src, dest);
}

void MacroAssembler::loadAbiReturnAddress(Register dest) { movePtr(lr, dest); }

// ===============================================================
// Logical instructions

void MacroAssembler::not32(Register reg) { ma_mvn(reg, reg); }

void MacroAssembler::notPtr(Register reg) { ma_mvn(reg, reg); }

void MacroAssembler::and32(Register src, Register dest) {
  ma_and(src, dest, SetCC);
}

void MacroAssembler::and32(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_and(imm, dest, scratch, SetCC);
}

void MacroAssembler::and32(Imm32 imm, const Address& dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(dest, scratch, scratch2);
  ma_and(imm, scratch, scratch2);
  ma_str(scratch, dest, scratch2);
}

void MacroAssembler::and32(const Address& src, Register dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(src, scratch, scratch2);
  ma_and(scratch, dest, SetCC);
}

void MacroAssembler::andPtr(Register src, Register dest) { ma_and(src, dest); }

void MacroAssembler::andPtr(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_and(imm, dest, scratch);
}

void MacroAssembler::and64(Imm64 imm, Register64 dest) {
  if (imm.low().value != int32_t(0xFFFFFFFF)) {
    and32(imm.low(), dest.low);
  }
  if (imm.hi().value != int32_t(0xFFFFFFFF)) {
    and32(imm.hi(), dest.high);
  }
}

void MacroAssembler::or64(Imm64 imm, Register64 dest) {
  if (imm.low().value) {
    or32(imm.low(), dest.low);
  }
  if (imm.hi().value) {
    or32(imm.hi(), dest.high);
  }
}

void MacroAssembler::xor64(Imm64 imm, Register64 dest) {
  if (imm.low().value) {
    xor32(imm.low(), dest.low);
  }
  if (imm.hi().value) {
    xor32(imm.hi(), dest.high);
  }
}

void MacroAssembler::or32(Register src, Register dest) { ma_orr(src, dest); }

void MacroAssembler::or32(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_orr(imm, dest, scratch);
}

void MacroAssembler::or32(Imm32 imm, const Address& dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(dest, scratch, scratch2);
  ma_orr(imm, scratch, scratch2);
  ma_str(scratch, dest, scratch2);
}

void MacroAssembler::orPtr(Register src, Register dest) { ma_orr(src, dest); }

void MacroAssembler::orPtr(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_orr(imm, dest, scratch);
}

void MacroAssembler::and64(Register64 src, Register64 dest) {
  and32(src.low, dest.low);
  and32(src.high, dest.high);
}

void MacroAssembler::or64(Register64 src, Register64 dest) {
  or32(src.low, dest.low);
  or32(src.high, dest.high);
}

void MacroAssembler::xor64(Register64 src, Register64 dest) {
  ma_eor(src.low, dest.low);
  ma_eor(src.high, dest.high);
}

void MacroAssembler::xor32(Register src, Register dest) {
  ma_eor(src, dest, SetCC);
}

void MacroAssembler::xor32(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_eor(imm, dest, scratch, SetCC);
}

void MacroAssembler::xor32(Imm32 imm, const Address& dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(dest, scratch, scratch2);
  ma_eor(imm, scratch, scratch2);
  ma_str(scratch, dest, scratch2);
}

void MacroAssembler::xor32(const Address& src, Register dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(src, scratch, scratch2);
  ma_eor(scratch, dest, SetCC);
}

void MacroAssembler::xorPtr(Register src, Register dest) { ma_eor(src, dest); }

void MacroAssembler::xorPtr(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_eor(imm, dest, scratch);
}

// ===============================================================
// Swap instructions

void MacroAssembler::byteSwap16SignExtend(Register reg) { as_revsh(reg, reg); }

void MacroAssembler::byteSwap16ZeroExtend(Register reg) {
  as_rev16(reg, reg);
  as_uxth(reg, reg, 0);
}

void MacroAssembler::byteSwap32(Register reg) { as_rev(reg, reg); }

void MacroAssembler::byteSwap64(Register64 reg) {
  as_rev(reg.high, reg.high);
  as_rev(reg.low, reg.low);

  ScratchRegisterScope scratch(*this);
  ma_mov(reg.high, scratch);
  ma_mov(reg.low, reg.high);
  ma_mov(scratch, reg.low);
}

// ===============================================================
// Arithmetic functions

void MacroAssembler::add32(Register src, Register dest) {
  ma_add(src, dest, SetCC);
}

void MacroAssembler::add32(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_add(imm, dest, scratch, SetCC);
}

void MacroAssembler::add32(Imm32 imm, Register src, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_add(src, imm, dest, scratch, SetCC);
}

void MacroAssembler::add32(Imm32 imm, const Address& dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(dest, scratch, scratch2);
  ma_add(imm, scratch, scratch2, SetCC);
  ma_str(scratch, dest, scratch2);
}

void MacroAssembler::addPtr(Register src, Register dest) { ma_add(src, dest); }

void MacroAssembler::addPtr(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_add(imm, dest, scratch);
}

void MacroAssembler::addPtr(ImmWord imm, Register dest) {
  addPtr(Imm32(imm.value), dest);
}

void MacroAssembler::addPtr(Imm32 imm, const Address& dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(dest, scratch, scratch2);
  ma_add(imm, scratch, scratch2);
  ma_str(scratch, dest, scratch2);
}

void MacroAssembler::addPtr(const Address& src, Register dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(src, scratch, scratch2);
  ma_add(scratch, dest, SetCC);
}

void MacroAssembler::add64(Register64 src, Register64 dest) {
  ma_add(src.low, dest.low, SetCC);
  ma_adc(src.high, dest.high);
}

void MacroAssembler::add64(Imm32 imm, Register64 dest) {
  ScratchRegisterScope scratch(*this);
  ma_add(imm, dest.low, scratch, SetCC);
  as_adc(dest.high, dest.high, Imm8(0), LeaveCC);
}

void MacroAssembler::add64(Imm64 imm, Register64 dest) {
  ScratchRegisterScope scratch(*this);
  ma_add(imm.low(), dest.low, scratch, SetCC);
  ma_adc(imm.hi(), dest.high, scratch, LeaveCC);
}

CodeOffset MacroAssembler::sub32FromStackPtrWithPatch(Register dest) {
  ScratchRegisterScope scratch(*this);
  CodeOffset offs = CodeOffset(currentOffset());
  ma_movPatchable(Imm32(0), scratch, Always);
  ma_sub(getStackPointer(), scratch, dest);
  return offs;
}

void MacroAssembler::patchSub32FromStackPtr(CodeOffset offset, Imm32 imm) {
  ScratchRegisterScope scratch(*this);
  BufferInstructionIterator iter(BufferOffset(offset.offset()), &m_buffer);
  iter.maybeSkipAutomaticInstructions();
  ma_mov_patch(imm, scratch, Always, HasMOVWT() ? L_MOVWT : L_LDR, iter);
}

void MacroAssembler::addDouble(FloatRegister src, FloatRegister dest) {
  ma_vadd(dest, src, dest);
}

void MacroAssembler::addFloat32(FloatRegister src, FloatRegister dest) {
  ma_vadd_f32(dest, src, dest);
}

void MacroAssembler::sub32(Register src, Register dest) {
  ma_sub(src, dest, SetCC);
}

void MacroAssembler::sub32(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_sub(imm, dest, scratch, SetCC);
}

void MacroAssembler::sub32(const Address& src, Register dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(src, scratch, scratch2);
  ma_sub(scratch, dest, SetCC);
}

void MacroAssembler::subPtr(Register src, Register dest) { ma_sub(src, dest); }

void MacroAssembler::subPtr(Register src, const Address& dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(dest, scratch, scratch2);
  ma_sub(src, scratch);
  ma_str(scratch, dest, scratch2);
}

void MacroAssembler::subPtr(Imm32 imm, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_sub(imm, dest, scratch);
}

void MacroAssembler::subPtr(const Address& addr, Register dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(addr, scratch, scratch2);
  ma_sub(scratch, dest);
}

void MacroAssembler::sub64(Register64 src, Register64 dest) {
  ma_sub(src.low, dest.low, SetCC);
  ma_sbc(src.high, dest.high, LeaveCC);
}

void MacroAssembler::sub64(Imm64 imm, Register64 dest) {
  ScratchRegisterScope scratch(*this);
  ma_sub(imm.low(), dest.low, scratch, SetCC);
  ma_sbc(imm.hi(), dest.high, scratch, LeaveCC);
}

void MacroAssembler::subDouble(FloatRegister src, FloatRegister dest) {
  ma_vsub(dest, src, dest);
}

void MacroAssembler::subFloat32(FloatRegister src, FloatRegister dest) {
  ma_vsub_f32(dest, src, dest);
}

void MacroAssembler::mul32(Register rhs, Register srcDest) {
  as_mul(srcDest, srcDest, rhs);
}

void MacroAssembler::mul32(Imm32 imm, Register srcDest) {
  ScratchRegisterScope scratch(*this);
  move32(imm, scratch);
  mul32(scratch, srcDest);
}

void MacroAssembler::mulHighUnsigned32(Imm32 imm, Register src, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_umull(src, imm, dest, scratch, scratch);
}

void MacroAssembler::mulPtr(Register rhs, Register srcDest) {
  as_mul(srcDest, srcDest, rhs);
}

void MacroAssembler::mul64(Imm64 imm, const Register64& dest) {
  // LOW32  = LOW(LOW(dest) * LOW(imm));
  // HIGH32 = LOW(HIGH(dest) * LOW(imm)) [multiply imm into upper bits]
  //        + LOW(LOW(dest) * HIGH(imm)) [multiply dest into upper bits]
  //        + HIGH(LOW(dest) * LOW(imm)) [carry]

  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  // HIGH(dest) = LOW(HIGH(dest) * LOW(imm));
  ma_mov(Imm32(imm.value & 0xFFFFFFFFL), scratch);
  as_mul(dest.high, dest.high, scratch);

  // high:low = LOW(dest) * LOW(imm);
  as_umull(scratch2, scratch, dest.low, scratch);

  // HIGH(dest) += high;
  as_add(dest.high, dest.high, O2Reg(scratch2));

  // HIGH(dest) += LOW(LOW(dest) * HIGH(imm));
  if (((imm.value >> 32) & 0xFFFFFFFFL) == 5) {
    as_add(scratch2, dest.low, lsl(dest.low, 2));
  } else {
    MOZ_CRASH("Not supported imm");
  }
  as_add(dest.high, dest.high, O2Reg(scratch2));

  // LOW(dest) = low;
  ma_mov(scratch, dest.low);
}

void MacroAssembler::mul64(Imm64 imm, const Register64& dest,
                           const Register temp) {
  // LOW32  = LOW(LOW(dest) * LOW(src));                                  (1)
  // HIGH32 = LOW(HIGH(dest) * LOW(src)) [multiply src into upper bits]   (2)
  //        + LOW(LOW(dest) * HIGH(src)) [multiply dest into upper bits]  (3)
  //        + HIGH(LOW(dest) * LOW(src)) [carry]                          (4)

  MOZ_ASSERT(temp != dest.high && temp != dest.low);

  // Compute mul64
  ScratchRegisterScope scratch(*this);
  ma_mul(dest.high, imm.low(), dest.high, scratch);  // (2)
  ma_mul(dest.low, imm.hi(), temp, scratch);         // (3)
  ma_add(dest.high, temp, temp);
  ma_umull(dest.low, imm.low(), dest.high, dest.low, scratch);  // (4) + (1)
  ma_add(temp, dest.high, dest.high);
}

void MacroAssembler::mul64(const Register64& src, const Register64& dest,
                           const Register temp) {
  // LOW32  = LOW(LOW(dest) * LOW(src));                                  (1)
  // HIGH32 = LOW(HIGH(dest) * LOW(src)) [multiply src into upper bits]   (2)
  //        + LOW(LOW(dest) * HIGH(src)) [multiply dest into upper bits]  (3)
  //        + HIGH(LOW(dest) * LOW(src)) [carry]                          (4)

  MOZ_ASSERT(dest != src);
  MOZ_ASSERT(dest.low != src.high && dest.high != src.low);

  // Compute mul64
  ma_mul(dest.high, src.low, dest.high);  // (2)
  ma_mul(src.high, dest.low, temp);       // (3)
  ma_add(dest.high, temp, temp);
  ma_umull(dest.low, src.low, dest.high, dest.low);  // (4) + (1)
  ma_add(temp, dest.high, dest.high);
}

void MacroAssembler::mulBy3(Register src, Register dest) {
  as_add(dest, src, lsl(src, 1));
}

void MacroAssembler::mulFloat32(FloatRegister src, FloatRegister dest) {
  ma_vmul_f32(dest, src, dest);
}

void MacroAssembler::mulDouble(FloatRegister src, FloatRegister dest) {
  ma_vmul(dest, src, dest);
}

void MacroAssembler::mulDoublePtr(ImmPtr imm, Register temp,
                                  FloatRegister dest) {
  ScratchRegisterScope scratch(*this);
  ScratchDoubleScope scratchDouble(*this);

  movePtr(imm, scratch);
  ma_vldr(Operand(Address(scratch, 0)).toVFPAddr(), scratchDouble);
  mulDouble(scratchDouble, dest);
}

void MacroAssembler::quotient32(Register rhs, Register srcDest,
                                bool isUnsigned) {
  MOZ_ASSERT(HasIDIV());
  if (isUnsigned) {
    ma_udiv(srcDest, rhs, srcDest);
  } else {
    ma_sdiv(srcDest, rhs, srcDest);
  }
}

void MacroAssembler::remainder32(Register rhs, Register srcDest,
                                 bool isUnsigned) {
  MOZ_ASSERT(HasIDIV());

  ScratchRegisterScope scratch(*this);
  if (isUnsigned) {
    ma_umod(srcDest, rhs, srcDest, scratch);
  } else {
    ma_smod(srcDest, rhs, srcDest, scratch);
  }
}

void MacroAssembler::divFloat32(FloatRegister src, FloatRegister dest) {
  ma_vdiv_f32(dest, src, dest);
}

void MacroAssembler::divDouble(FloatRegister src, FloatRegister dest) {
  ma_vdiv(dest, src, dest);
}

void MacroAssembler::inc64(AbsoluteAddress dest) {
  ScratchRegisterScope scratch(*this);

  ma_strd(r0, r1, EDtrAddr(sp, EDtrOffImm(-8)), PreIndex);

  ma_mov(Imm32((int32_t)dest.addr), scratch);
  ma_ldrd(EDtrAddr(scratch, EDtrOffImm(0)), r0, r1);

  as_add(r0, r0, Imm8(1), SetCC);
  as_adc(r1, r1, Imm8(0), LeaveCC);

  ma_strd(r0, r1, EDtrAddr(scratch, EDtrOffImm(0)));
  ma_ldrd(EDtrAddr(sp, EDtrOffImm(8)), r0, r1, PostIndex);
}

void MacroAssembler::neg32(Register reg) { ma_neg(reg, reg, SetCC); }

void MacroAssembler::neg64(Register64 reg) {
  as_rsb(reg.low, reg.low, Imm8(0), SetCC);
  as_rsc(reg.high, reg.high, Imm8(0));
}

void MacroAssembler::negPtr(Register reg) { neg32(reg); }

void MacroAssembler::negateDouble(FloatRegister reg) { ma_vneg(reg, reg); }

void MacroAssembler::negateFloat(FloatRegister reg) { ma_vneg_f32(reg, reg); }

void MacroAssembler::abs32(Register src, Register dest) {
  as_cmp(src, Imm8(0));
  as_rsb(dest, src, Imm8(0), LeaveCC, LessThan);
  if (dest != src) {
    as_mov(dest, O2Reg(src), LeaveCC, GreaterThanOrEqual);
  }
}

void MacroAssembler::absFloat32(FloatRegister src, FloatRegister dest) {
  ma_vabs_f32(src, dest);
}

void MacroAssembler::absDouble(FloatRegister src, FloatRegister dest) {
  ma_vabs(src, dest);
}

void MacroAssembler::sqrtFloat32(FloatRegister src, FloatRegister dest) {
  ma_vsqrt_f32(src, dest);
}

void MacroAssembler::sqrtDouble(FloatRegister src, FloatRegister dest) {
  ma_vsqrt(src, dest);
}

void MacroAssembler::minFloat32(FloatRegister other, FloatRegister srcDest,
                                bool handleNaN) {
  minMaxFloat32(srcDest, other, handleNaN, false);
}

void MacroAssembler::minDouble(FloatRegister other, FloatRegister srcDest,
                               bool handleNaN) {
  minMaxDouble(srcDest, other, handleNaN, false);
}

void MacroAssembler::maxFloat32(FloatRegister other, FloatRegister srcDest,
                                bool handleNaN) {
  minMaxFloat32(srcDest, other, handleNaN, true);
}

void MacroAssembler::maxDouble(FloatRegister other, FloatRegister srcDest,
                               bool handleNaN) {
  minMaxDouble(srcDest, other, handleNaN, true);
}

// ===============================================================
// Shift functions

void MacroAssembler::lshiftPtr(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 32);
  ma_lsl(imm, dest, dest);
}

void MacroAssembler::lshiftPtr(Register src, Register dest) {
  ma_lsl(src, dest, dest);
}

void MacroAssembler::lshift64(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  if (imm.value == 0) {
    return;
  }

  if (imm.value < 32) {
    as_mov(dest.high, lsl(dest.high, imm.value));
    as_orr(dest.high, dest.high, lsr(dest.low, 32 - imm.value));
    as_mov(dest.low, lsl(dest.low, imm.value));
  } else {
    as_mov(dest.high, lsl(dest.low, imm.value - 32));
    ma_mov(Imm32(0), dest.low);
  }
}

void MacroAssembler::lshift64(Register unmaskedShift, Register64 dest) {
  // dest.high = dest.high << shift | dest.low << shift - 32 | dest.low >> 32 -
  // shift Note: one of the two dest.low shift will always yield zero due to
  // negative shift.

  ScratchRegisterScope shift(*this);
  as_and(shift, unmaskedShift, Imm8(0x3f));
  as_mov(dest.high, lsl(dest.high, shift));
  as_sub(shift, shift, Imm8(32));
  as_orr(dest.high, dest.high, lsl(dest.low, shift));
  ma_neg(shift, shift);
  as_orr(dest.high, dest.high, lsr(dest.low, shift));
  as_and(shift, unmaskedShift, Imm8(0x3f));
  as_mov(dest.low, lsl(dest.low, shift));
}

void MacroAssembler::lshift32(Register src, Register dest) {
  ma_lsl(src, dest, dest);
}

void MacroAssembler::flexibleLshift32(Register src, Register dest) {
  ScratchRegisterScope scratch(*this);
  as_and(scratch, src, Imm8(0x1F));
  lshift32(scratch, dest);
}

void MacroAssembler::lshift32(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 32);
  lshiftPtr(imm, dest);
}

void MacroAssembler::rshiftPtr(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 32);
  if (imm.value) {
    ma_lsr(imm, dest, dest);
  }
}

void MacroAssembler::rshiftPtr(Register src, Register dest) {
  ma_lsr(src, dest, dest);
}

void MacroAssembler::rshift32(Register src, Register dest) {
  ma_lsr(src, dest, dest);
}

void MacroAssembler::flexibleRshift32(Register src, Register dest) {
  ScratchRegisterScope scratch(*this);
  as_and(scratch, src, Imm8(0x1F));
  rshift32(scratch, dest);
}

void MacroAssembler::rshift32(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 32);
  rshiftPtr(imm, dest);
}

void MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 32);
  if (imm.value) {
    ma_asr(imm, dest, dest);
  }
}

void MacroAssembler::rshift64Arithmetic(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  if (!imm.value) {
    return;
  }

  if (imm.value < 32) {
    as_mov(dest.low, lsr(dest.low, imm.value));
    as_orr(dest.low, dest.low, lsl(dest.high, 32 - imm.value));
    as_mov(dest.high, asr(dest.high, imm.value));
  } else if (imm.value == 32) {
    as_mov(dest.low, O2Reg(dest.high));
    as_mov(dest.high, asr(dest.high, 31));
  } else {
    as_mov(dest.low, asr(dest.high, imm.value - 32));
    as_mov(dest.high, asr(dest.high, 31));
  }
}

void MacroAssembler::rshift64Arithmetic(Register unmaskedShift,
                                        Register64 dest) {
  Label proceed;

  // dest.low = dest.low >>> shift | dest.high <<< 32 - shift
  // if (shift - 32 >= 0)
  //   dest.low |= dest.high >>> shift - 32
  // Note: Negative shifts yield a zero as result, except for the signed
  //       right shift. Therefore we need to test for it and only do it if
  //       it isn't negative.
  ScratchRegisterScope shift(*this);

  as_and(shift, unmaskedShift, Imm8(0x3f));
  as_mov(dest.low, lsr(dest.low, shift));
  as_rsb(shift, shift, Imm8(32));
  as_orr(dest.low, dest.low, lsl(dest.high, shift));
  ma_neg(shift, shift, SetCC);
  ma_b(&proceed, Signed);

  as_orr(dest.low, dest.low, asr(dest.high, shift));

  bind(&proceed);
  as_and(shift, unmaskedShift, Imm8(0x3f));
  as_mov(dest.high, asr(dest.high, shift));
}

void MacroAssembler::rshift32Arithmetic(Register src, Register dest) {
  ma_asr(src, dest, dest);
}

void MacroAssembler::rshift32Arithmetic(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 32);
  rshiftPtrArithmetic(imm, dest);
}

void MacroAssembler::flexibleRshift32Arithmetic(Register src, Register dest) {
  ScratchRegisterScope scratch(*this);
  as_and(scratch, src, Imm8(0x1F));
  rshift32Arithmetic(scratch, dest);
}

void MacroAssembler::rshift64(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  if (!imm.value) {
    return;
  }

  if (imm.value < 32) {
    as_mov(dest.low, lsr(dest.low, imm.value));
    as_orr(dest.low, dest.low, lsl(dest.high, 32 - imm.value));
    as_mov(dest.high, lsr(dest.high, imm.value));
  } else if (imm.value == 32) {
    ma_mov(dest.high, dest.low);
    ma_mov(Imm32(0), dest.high);
  } else {
    ma_lsr(Imm32(imm.value - 32), dest.high, dest.low);
    ma_mov(Imm32(0), dest.high);
  }
}

void MacroAssembler::rshift64(Register unmaskedShift, Register64 dest) {
  // dest.low = dest.low >> shift | dest.high >> shift - 32 | dest.high << 32 -
  // shift Note: one of the two dest.high shifts will always yield zero due to
  // negative shift.

  ScratchRegisterScope shift(*this);
  as_and(shift, unmaskedShift, Imm8(0x3f));
  as_mov(dest.low, lsr(dest.low, shift));
  as_sub(shift, shift, Imm8(32));
  as_orr(dest.low, dest.low, lsr(dest.high, shift));
  ma_neg(shift, shift);
  as_orr(dest.low, dest.low, lsl(dest.high, shift));
  as_and(shift, unmaskedShift, Imm8(0x3f));
  as_mov(dest.high, lsr(dest.high, shift));
}

// ===============================================================
// Rotate functions
void MacroAssembler::rotateLeft(Imm32 count, Register input, Register dest) {
  if (count.value) {
    ma_rol(count, input, dest);
  } else {
    ma_mov(input, dest);
  }
}

void MacroAssembler::rotateLeft(Register count, Register input, Register dest) {
  ScratchRegisterScope scratch(*this);
  ma_rol(count, input, dest, scratch);
}

void MacroAssembler::rotateLeft64(Imm32 count, Register64 input,
                                  Register64 dest, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  MOZ_ASSERT(input.low != dest.high && input.high != dest.low);

  int32_t amount = count.value & 0x3f;
  if (amount > 32) {
    rotateRight64(Imm32(64 - amount), input, dest, temp);
  } else {
    ScratchRegisterScope scratch(*this);
    if (amount == 0) {
      ma_mov(input.low, dest.low);
      ma_mov(input.high, dest.high);
    } else if (amount == 32) {
      ma_mov(input.low, scratch);
      ma_mov(input.high, dest.low);
      ma_mov(scratch, dest.high);
    } else {
      MOZ_ASSERT(0 < amount && amount < 32);
      ma_mov(dest.high, scratch);
      as_mov(dest.high, lsl(dest.high, amount));
      as_orr(dest.high, dest.high, lsr(dest.low, 32 - amount));
      as_mov(dest.low, lsl(dest.low, amount));
      as_orr(dest.low, dest.low, lsr(scratch, 32 - amount));
    }
  }
}

void MacroAssembler::rotateLeft64(Register shift, Register64 src,
                                  Register64 dest, Register temp) {
  MOZ_ASSERT(shift != temp);
  MOZ_ASSERT(src == dest);
  MOZ_ASSERT(temp != src.low && temp != src.high);
  MOZ_ASSERT(shift != src.low && shift != src.high);
  MOZ_ASSERT(temp != InvalidReg);

  ScratchRegisterScope shift_value(*this);
  Label high, done;

  ma_mov(src.high, temp);
  as_and(shift_value, shift, Imm8(0x3f));
  as_cmp(shift_value, Imm8(32));
  ma_b(&high, GreaterThanOrEqual);

  // high = high << shift | low >> 32 - shift
  // low = low << shift | high >> 32 - shift
  as_mov(dest.high, lsl(src.high, shift_value));
  as_rsb(shift_value, shift_value, Imm8(32));
  as_orr(dest.high, dest.high, lsr(src.low, shift_value));

  as_rsb(shift_value, shift_value, Imm8(32));
  as_mov(dest.low, lsl(src.low, shift_value));
  as_rsb(shift_value, shift_value, Imm8(32));
  as_orr(dest.low, dest.low, lsr(temp, shift_value));

  ma_b(&done);

  // A 32 - 64 shift is a 0 - 32 shift in the other direction.
  bind(&high);
  as_rsb(shift_value, shift_value, Imm8(64));

  as_mov(dest.high, lsr(src.high, shift_value));
  as_rsb(shift_value, shift_value, Imm8(32));
  as_orr(dest.high, dest.high, lsl(src.low, shift_value));

  as_rsb(shift_value, shift_value, Imm8(32));
  as_mov(dest.low, lsr(src.low, shift_value));
  as_rsb(shift_value, shift_value, Imm8(32));
  as_orr(dest.low, dest.low, lsl(temp, shift_value));

  bind(&done);
}

void MacroAssembler::rotateRight(Imm32 count, Register input, Register dest) {
  if (count.value) {
    ma_ror(count, input, dest);
  } else {
    ma_mov(input, dest);
  }
}

void MacroAssembler::rotateRight(Register count, Register input,
                                 Register dest) {
  ma_ror(count, input, dest);
}

void MacroAssembler::rotateRight64(Imm32 count, Register64 input,
                                   Register64 dest, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  MOZ_ASSERT(input.low != dest.high && input.high != dest.low);

  int32_t amount = count.value & 0x3f;
  if (amount > 32) {
    rotateLeft64(Imm32(64 - amount), input, dest, temp);
  } else {
    ScratchRegisterScope scratch(*this);
    if (amount == 0) {
      ma_mov(input.low, dest.low);
      ma_mov(input.high, dest.high);
    } else if (amount == 32) {
      ma_mov(input.low, scratch);
      ma_mov(input.high, dest.low);
      ma_mov(scratch, dest.high);
    } else {
      MOZ_ASSERT(0 < amount && amount < 32);
      ma_mov(dest.high, scratch);
      as_mov(dest.high, lsr(dest.high, amount));
      as_orr(dest.high, dest.high, lsl(dest.low, 32 - amount));
      as_mov(dest.low, lsr(dest.low, amount));
      as_orr(dest.low, dest.low, lsl(scratch, 32 - amount));
    }
  }
}

void MacroAssembler::rotateRight64(Register shift, Register64 src,
                                   Register64 dest, Register temp) {
  MOZ_ASSERT(shift != temp);
  MOZ_ASSERT(src == dest);
  MOZ_ASSERT(temp != src.low && temp != src.high);
  MOZ_ASSERT(shift != src.low && shift != src.high);
  MOZ_ASSERT(temp != InvalidReg);

  ScratchRegisterScope shift_value(*this);
  Label high, done;

  ma_mov(src.high, temp);
  as_and(shift_value, shift, Imm8(0x3f));
  as_cmp(shift_value, Imm8(32));
  ma_b(&high, GreaterThanOrEqual);

  // high = high >> shift | low << 32 - shift
  // low = low >> shift | high << 32 - shift
  as_mov(dest.high, lsr(src.high, shift_value));
  as_rsb(shift_value, shift_value, Imm8(32));
  as_orr(dest.high, dest.high, lsl(src.low, shift_value));

  as_rsb(shift_value, shift_value, Imm8(32));
  as_mov(dest.low, lsr(src.low, shift_value));
  as_rsb(shift_value, shift_value, Imm8(32));
  as_orr(dest.low, dest.low, lsl(temp, shift_value));

  ma_b(&done);

  // A 32 - 64 shift is a 0 - 32 shift in the other direction.
  bind(&high);
  as_rsb(shift_value, shift_value, Imm8(64));

  as_mov(dest.high, lsl(src.high, shift_value));
  as_rsb(shift_value, shift_value, Imm8(32));
  as_orr(dest.high, dest.high, lsr(src.low, shift_value));

  as_rsb(shift_value, shift_value, Imm8(32));
  as_mov(dest.low, lsl(src.low, shift_value));
  as_rsb(shift_value, shift_value, Imm8(32));
  as_orr(dest.low, dest.low, lsr(temp, shift_value));

  bind(&done);
}

// ===============================================================
// Condition functions

void MacroAssembler::cmp8Set(Condition cond, Address lhs, Imm32 rhs,
                             Register dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  // Inlined calls to load8{Zero,Sign}Extend() and cmp32Set() to acquire
  // exclusive access to scratch registers.

  bool isSigned;
  Imm32 imm(0);
  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      isSigned = false;
      imm = Imm32(uint8_t(rhs.value));
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      isSigned = true;
      imm = Imm32(int8_t(rhs.value));
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }

  ma_dataTransferN(IsLoad, 8, isSigned, lhs.base, Imm32(lhs.offset), scratch,
                   scratch2);
  ma_cmp(scratch, imm, scratch2);
  emitSet(cond, dest);
}

void MacroAssembler::cmp16Set(Condition cond, Address lhs, Imm32 rhs,
                              Register dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  // Inlined calls to load16{Zero,Sign}Extend() and cmp32Set() to acquire
  // exclusive access to scratch registers.

  bool isSigned;
  Imm32 imm(0);
  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      isSigned = false;
      imm = Imm32(uint16_t(rhs.value));
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      isSigned = true;
      imm = Imm32(int16_t(rhs.value));
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }

  ma_dataTransferN(IsLoad, 16, isSigned, lhs.base, Imm32(lhs.offset), scratch,
                   scratch2);
  ma_cmp(scratch, imm, scratch2);
  emitSet(cond, dest);
}

template <typename T1, typename T2>
void MacroAssembler::cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest) {
  cmp32(lhs, rhs);
  emitSet(cond, dest);
}

void MacroAssembler::cmp64Set(Condition cond, Address lhs, Imm64 rhs,
                              Register dest) {
  Label success, done;

  branch64(cond, lhs, rhs, &success);
  move32(Imm32(0), dest);
  jump(&done);
  bind(&success);
  move32(Imm32(1), dest);
  bind(&done);
}

template <typename T1, typename T2>
void MacroAssembler::cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest) {
  cmpPtr(lhs, rhs);
  emitSet(cond, dest);
}

// ===============================================================
// Bit counting functions

void MacroAssembler::clz32(Register src, Register dest, bool knownNotZero) {
  ma_clz(src, dest);
}

void MacroAssembler::clz64(Register64 src, Register dest) {
  ScratchRegisterScope scratch(*this);

  ma_clz(src.high, scratch);
  as_cmp(scratch, Imm8(32));
  ma_mov(scratch, dest, LeaveCC, NotEqual);
  ma_clz(src.low, dest, Equal);
  as_add(dest, dest, Imm8(32), LeaveCC, Equal);
}

void MacroAssembler::ctz32(Register src, Register dest, bool knownNotZero) {
  ScratchRegisterScope scratch(*this);
  ma_ctz(src, dest, scratch);
}

void MacroAssembler::ctz64(Register64 src, Register dest) {
  Label done, high;
  as_cmp(src.low, Imm8(0));
  ma_b(&high, Equal);

  ctz32(src.low, dest, /* knownNotZero = */ true);
  ma_b(&done);

  bind(&high);
  ctz32(src.high, dest, /* knownNotZero = */ false);
  as_add(dest, dest, Imm8(32));

  bind(&done);
}

void MacroAssembler::popcnt32(Register input, Register output, Register tmp) {
  // Equivalent to GCC output of mozilla::CountPopulation32()

  ScratchRegisterScope scratch(*this);

  if (input != output) {
    ma_mov(input, output);
  }
  as_mov(tmp, asr(output, 1));
  ma_and(Imm32(0x55555555), tmp, scratch);
  ma_sub(output, tmp, output);
  as_mov(tmp, asr(output, 2));
  ma_mov(Imm32(0x33333333), scratch);
  ma_and(scratch, output);
  ma_and(scratch, tmp);
  ma_add(output, tmp, output);
  as_add(output, output, lsr(output, 4));
  ma_and(Imm32(0xF0F0F0F), output, scratch);
  as_add(output, output, lsl(output, 8));
  as_add(output, output, lsl(output, 16));
  as_mov(output, asr(output, 24));
}

void MacroAssembler::popcnt64(Register64 src, Register64 dest, Register tmp) {
  MOZ_ASSERT(dest.low != tmp);
  MOZ_ASSERT(dest.high != tmp);
  MOZ_ASSERT(dest.low != dest.high);
  // The source and destination can overlap. Therefore make sure we don't
  // clobber the source before we have the data.
  if (dest.low != src.high) {
    popcnt32(src.low, dest.low, tmp);
    popcnt32(src.high, dest.high, tmp);
  } else {
    MOZ_ASSERT(dest.high != src.high);
    popcnt32(src.low, dest.high, tmp);
    popcnt32(src.high, dest.low, tmp);
  }
  ma_add(dest.high, dest.low);
  ma_mov(Imm32(0), dest.high);
}

// ===============================================================
// Branch functions

void MacroAssembler::branch8(Condition cond, const Address& lhs, Imm32 rhs,
                             Label* label) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  // Inlined calls to load8{Zero,Sign}Extend() and branch32() to acquire
  // exclusive access to scratch registers.

  bool isSigned;
  Imm32 imm(0);
  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      isSigned = false;
      imm = Imm32(uint8_t(rhs.value));
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      isSigned = true;
      imm = Imm32(int8_t(rhs.value));
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }

  ma_dataTransferN(IsLoad, 8, isSigned, lhs.base, Imm32(lhs.offset), scratch,
                   scratch2);
  ma_cmp(scratch, imm, scratch2);
  ma_b(label, cond);
}

void MacroAssembler::branch8(Condition cond, const BaseIndex& lhs, Register rhs,
                             Label* label) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  // Inlined calls to load8{Zero,Sign}Extend() and branch32() to acquire
  // exclusive access to scratch registers.

  bool isSigned;
  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      isSigned = false;
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      isSigned = true;
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }

  if (isSigned) {
    Register index = lhs.index;

    // ARMv7 does not have LSL on an index register with an extended load.
    if (lhs.scale != TimesOne) {
      ma_lsl(Imm32::ShiftOf(lhs.scale), index, scratch);
      index = scratch;
    }

    if (lhs.offset != 0) {
      if (index != scratch) {
        ma_mov(index, scratch);
        index = scratch;
      }
      ma_add(Imm32(lhs.offset), index, scratch2);
    }
    ma_ldrsb(EDtrAddr(lhs.base, EDtrOffReg(index)), scratch);
  } else {
    Register base = lhs.base;
    uint32_t scale = Imm32::ShiftOf(lhs.scale).value;

    if (lhs.offset == 0) {
      ma_ldrb(DTRAddr(base, DtrRegImmShift(lhs.index, LSL, scale)), scratch);
    } else {
      ma_add(base, Imm32(lhs.offset), scratch, scratch2);
      ma_ldrb(DTRAddr(scratch, DtrRegImmShift(lhs.index, LSL, scale)), scratch);
    }
  }

  ma_cmp(scratch, rhs);
  ma_b(label, cond);
}

void MacroAssembler::branch16(Condition cond, const Address& lhs, Imm32 rhs,
                              Label* label) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  // Inlined calls to load16{Zero,Sign}Extend() and branch32() to acquire
  // exclusive access to scratch registers.

  bool isSigned;
  Imm32 imm(0);
  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      isSigned = false;
      imm = Imm32(uint16_t(rhs.value));
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      isSigned = true;
      imm = Imm32(int16_t(rhs.value));
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }

  ma_dataTransferN(IsLoad, 16, isSigned, lhs.base, Imm32(lhs.offset), scratch,
                   scratch2);
  ma_cmp(scratch, imm, scratch2);
  ma_b(label, cond);
}

template <class L>
void MacroAssembler::branch32(Condition cond, Register lhs, Register rhs,
                              L label) {
  ma_cmp(lhs, rhs);
  ma_b(label, cond);
}

template <class L>
void MacroAssembler::branch32(Condition cond, Register lhs, Imm32 rhs,
                              L label) {
  ScratchRegisterScope scratch(*this);

  ma_cmp(lhs, rhs, scratch);
  ma_b(label, cond);
}

void MacroAssembler::branch32(Condition cond, const Address& lhs, Register rhs,
                              Label* label) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(lhs, scratch, scratch2);
  ma_cmp(scratch, rhs);
  ma_b(label, cond);
}

void MacroAssembler::branch32(Condition cond, const Address& lhs, Imm32 rhs,
                              Label* label) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  ma_ldr(lhs, scratch, scratch2);
  ma_cmp(scratch, rhs, scratch2);
  ma_b(label, cond);
}

void MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs,
                              Register rhs, Label* label) {
  ScratchRegisterScope scratch(*this);

  // Load into scratch.
  movePtr(ImmWord(uintptr_t(lhs.addr)), scratch);
  ma_ldr(DTRAddr(scratch, DtrOffImm(0)), scratch);

  ma_cmp(scratch, rhs);
  ma_b(label, cond);
}

void MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs,
                              Imm32 rhs, Label* label) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  // Load into scratch.
  movePtr(ImmWord(uintptr_t(lhs.addr)), scratch);
  ma_ldr(DTRAddr(scratch, DtrOffImm(0)), scratch);

  ma_cmp(scratch, rhs, scratch2);
  ma_b(label, cond);
}

void MacroAssembler::branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs,
                              Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  {
    ScratchRegisterScope scratch(*this);

    Register base = lhs.base;
    uint32_t scale = Imm32::ShiftOf(lhs.scale).value;

    // Load lhs into scratch2.
    if (lhs.offset != 0) {
      ma_add(base, Imm32(lhs.offset), scratch, scratch2);
      ma_ldr(DTRAddr(scratch, DtrRegImmShift(lhs.index, LSL, scale)), scratch2);
    } else {
      ma_ldr(DTRAddr(base, DtrRegImmShift(lhs.index, LSL, scale)), scratch2);
    }
  }
  branch32(cond, scratch2, rhs, label);
}

void MacroAssembler::branch32(Condition cond, const BaseIndex& lhs,
                              Register rhs, Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  {
    ScratchRegisterScope scratch(*this);

    Register base = lhs.base;
    uint32_t scale = Imm32::ShiftOf(lhs.scale).value;

    // Load lhs into scratch2.
    if (lhs.offset != 0) {
      ma_add(base, Imm32(lhs.offset), scratch, scratch2);
      ma_ldr(DTRAddr(scratch, DtrRegImmShift(lhs.index, LSL, scale)), scratch2);
    } else {
      ma_ldr(DTRAddr(base, DtrRegImmShift(lhs.index, LSL, scale)), scratch2);
    }
  }
  branch32(cond, scratch2, rhs, label);
}

void MacroAssembler::branch32(Condition cond, wasm::SymbolicAddress lhs,
                              Imm32 rhs, Label* label) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);

  movePtr(lhs, scratch);
  ma_ldr(DTRAddr(scratch, DtrOffImm(0)), scratch);

  ma_cmp(scratch, rhs, scratch2);
  ma_b(label, cond);
}

void MacroAssembler::branch64(Condition cond, const Address& lhs, Imm64 val,
                              Label* label) {
  MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
             "other condition codes not supported");

  Label done;

  if (cond == Assembler::Equal) {
    branch32(Assembler::NotEqual, lhs, val.firstHalf(), &done);
  } else {
    branch32(Assembler::NotEqual, lhs, val.firstHalf(), label);
  }
  branch32(cond, Address(lhs.base, lhs.offset + sizeof(uint32_t)),
           val.secondHalf(), label);

  bind(&done);
}

void MacroAssembler::branch64(Condition cond, const Address& lhs,
                              Register64 rhs, Label* label) {
  MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
             "other condition codes not supported");

  Label done;

  if (cond == Assembler::Equal) {
    branch32(Assembler::NotEqual, lhs, rhs.low, &done);
  } else {
    branch32(Assembler::NotEqual, lhs, rhs.low, label);
  }
  branch32(cond, Address(lhs.base, lhs.offset + sizeof(uint32_t)), rhs.high,
           label);

  bind(&done);
}

void MacroAssembler::branch64(Condition cond, const Address& lhs,
                              const Address& rhs, Register scratch,
                              Label* label) {
  MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
             "other condition codes not supported");
  MOZ_ASSERT(lhs.base != scratch);
  MOZ_ASSERT(rhs.base != scratch);

  Label done;

  load32(rhs, scratch);
  if (cond == Assembler::Equal) {
    branch32(Assembler::NotEqual, lhs, scratch, &done);
  } else {
    branch32(Assembler::NotEqual, lhs, scratch, label);
  }

  load32(Address(rhs.base, rhs.offset + sizeof(uint32_t)), scratch);
  branch32(cond, Address(lhs.base, lhs.offset + sizeof(uint32_t)), scratch,
           label);

  bind(&done);
}

void MacroAssembler::branch64(Condition cond, Register64 lhs, Imm64 val,
                              Label* success, Label* fail) {
  bool fallthrough = false;
  Label fallthroughLabel;

  if (!fail) {
    fail = &fallthroughLabel;
    fallthrough = true;
  }

  switch (cond) {
    case Assembler::Equal:
      branch32(Assembler::NotEqual, lhs.low, val.low(), fail);
      branch32(Assembler::Equal, lhs.high, val.hi(), success);
      if (!fallthrough) {
        jump(fail);
      }
      break;
    case Assembler::NotEqual:
      branch32(Assembler::NotEqual, lhs.low, val.low(), success);
      branch32(Assembler::NotEqual, lhs.high, val.hi(), success);
      if (!fallthrough) {
        jump(fail);
      }
      break;
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual: {
      Assembler::Condition cond1 = Assembler::ConditionWithoutEqual(cond);
      Assembler::Condition cond2 =
          Assembler::ConditionWithoutEqual(Assembler::InvertCondition(cond));
      Assembler::Condition cond3 = Assembler::UnsignedCondition(cond);

      cmp32(lhs.high, val.hi());
      ma_b(success, cond1);
      ma_b(fail, cond2);
      cmp32(lhs.low, val.low());
      ma_b(success, cond3);
      if (!fallthrough) {
        jump(fail);
      }
      break;
    }
    default:
      MOZ_CRASH("Condition code not supported");
      break;
  }

  if (fallthrough) {
    bind(fail);
  }
}

void MacroAssembler::branch64(Condition cond, Register64 lhs, Register64 rhs,
                              Label* success, Label* fail) {
  bool fallthrough = false;
  Label fallthroughLabel;

  if (!fail) {
    fail = &fallthroughLabel;
    fallthrough = true;
  }

  switch (cond) {
    case Assembler::Equal:
      branch32(Assembler::NotEqual, lhs.low, rhs.low, fail);
      branch32(Assembler::Equal, lhs.high, rhs.high, success);
      if (!fallthrough) {
        jump(fail);
      }
      break;
    case Assembler::NotEqual:
      branch32(Assembler::NotEqual, lhs.low, rhs.low, success);
      branch32(Assembler::NotEqual, lhs.high, rhs.high, success);
      if (!fallthrough) {
        jump(fail);
      }
      break;
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual: {
      Assembler::Condition cond1 = Assembler::ConditionWithoutEqual(cond);
      Assembler::Condition cond2 =
          Assembler::ConditionWithoutEqual(Assembler::InvertCondition(cond));
      Assembler::Condition cond3 = Assembler::UnsignedCondition(cond);

      cmp32(lhs.high, rhs.high);
      ma_b(success, cond1);
      ma_b(fail, cond2);
      cmp32(lhs.low, rhs.low);
      ma_b(success, cond3);
      if (!fallthrough) {
        jump(fail);
      }
      break;
    }
    default:
      MOZ_CRASH("Condition code not supported");
      break;
  }

  if (fallthrough) {
    bind(fail);
  }
}

template <class L>
void MacroAssembler::branchPtr(Condition cond, Register lhs, Register rhs,
                               L label) {
  branch32(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, Imm32 rhs,
                               Label* label) {
  branch32(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmPtr rhs,
                               Label* label) {
  branchPtr(cond, lhs, ImmWord(uintptr_t(rhs.value)), label);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmGCPtr rhs,
                               Label* label) {
  ScratchRegisterScope scratch(*this);
  movePtr(rhs, scratch);
  branchPtr(cond, lhs, scratch, label);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmWord rhs,
                               Label* label) {
  branch32(cond, lhs, Imm32(rhs.value), label);
}

template <class L>
void MacroAssembler::branchPtr(Condition cond, const Address& lhs, Register rhs,
                               L label) {
  branch32(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmPtr rhs,
                               Label* label) {
  branchPtr(cond, lhs, ImmWord(uintptr_t(rhs.value)), label);
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmGCPtr rhs,
                               Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmWord rhs,
                               Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs,
                               Register rhs, Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs,
                               ImmWord rhs, Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, wasm::SymbolicAddress lhs,
                               Register rhs, Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs,
                               ImmWord rhs, Label* label) {
  branch32(cond, lhs, Imm32(rhs.value), label);
}

void MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs,
                               Register rhs, Label* label) {
  branch32(cond, lhs, rhs, label);
}

void MacroAssembler::branchPrivatePtr(Condition cond, const Address& lhs,
                                      Register rhs, Label* label) {
  branchPtr(cond, lhs, rhs, label);
}

void MacroAssembler::branchFloat(DoubleCondition cond, FloatRegister lhs,
                                 FloatRegister rhs, Label* label) {
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

void MacroAssembler::branchTruncateFloat32MaybeModUint32(FloatRegister src,
                                                         Register dest,
                                                         Label* fail) {
  branchTruncateFloat32ToInt32(src, dest, fail);
}

void MacroAssembler::branchTruncateFloat32ToInt32(FloatRegister src,
                                                  Register dest, Label* fail) {
  ScratchFloat32Scope scratchFloat32(*this);
  ScratchRegisterScope scratch(*this);

  ma_vcvt_F32_I32(src, scratchFloat32.sintOverlay());
  ma_vxfer(scratchFloat32, dest);
  ma_cmp(dest, Imm32(0x7fffffff), scratch);
  ma_cmp(dest, Imm32(0x80000000), scratch, Assembler::NotEqual);
  ma_b(fail, Assembler::Equal);
}

void MacroAssembler::branchDouble(DoubleCondition cond, FloatRegister lhs,
                                  FloatRegister rhs, Label* label) {
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

void MacroAssembler::branchTruncateDoubleMaybeModUint32(FloatRegister src,
                                                        Register dest,
                                                        Label* fail) {
  branchTruncateDoubleToInt32(src, dest, fail);
}

// There are two options for implementing branchTruncateDoubleToInt32:
//
// 1. Convert the floating point value to an integer, if it did not fit, then it
// was clamped to INT_MIN/INT_MAX, and we can test it. NOTE: if the value
// really was supposed to be INT_MAX / INT_MIN then it will be wrong.
//
// 2. Convert the floating point value to an integer, if it did not fit, then it
// set one or two bits in the fpcsr. Check those.
void MacroAssembler::branchTruncateDoubleToInt32(FloatRegister src,
                                                 Register dest, Label* fail) {
  ScratchDoubleScope scratchDouble(*this);
  FloatRegister scratchSIntReg = scratchDouble.sintOverlay();
  ScratchRegisterScope scratch(*this);

  ma_vcvt_F64_I32(src, scratchSIntReg);
  ma_vxfer(scratchSIntReg, dest);
  ma_cmp(dest, Imm32(0x7fffffff), scratch);
  ma_cmp(dest, Imm32(0x80000000), scratch, Assembler::NotEqual);
  ma_b(fail, Assembler::Equal);
}

template <typename T>
void MacroAssembler::branchAdd32(Condition cond, T src, Register dest,
                                 Label* label) {
  add32(src, dest);
  as_b(label, cond);
}

template <typename T>
void MacroAssembler::branchSub32(Condition cond, T src, Register dest,
                                 Label* label) {
  sub32(src, dest);
  j(cond, label);
}

template <typename T>
void MacroAssembler::branchMul32(Condition cond, T src, Register dest,
                                 Label* label) {
  MOZ_ASSERT(cond == Assembler::Overflow);
  ScratchRegisterScope scratch(*this);
  Assembler::Condition overflow_cond =
      ma_check_mul(src, dest, dest, scratch, cond);
  j(overflow_cond, label);
}

template <typename T>
void MacroAssembler::branchRshift32(Condition cond, T src, Register dest,
                                    Label* label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero);
  rshift32(src, dest);
  branch32(cond == Zero ? Equal : NotEqual, dest, Imm32(0), label);
}

void MacroAssembler::branchNeg32(Condition cond, Register reg, Label* label) {
  MOZ_ASSERT(cond == Overflow);
  neg32(reg);
  j(cond, label);
}

void MacroAssembler::branchAdd64(Condition cond, Imm64 imm, Register64 dest,
                                 Label* label) {
  ScratchRegisterScope scratch(*this);
  ma_add(imm.low(), dest.low, scratch, SetCC);
  ma_adc(imm.hi(), dest.high, scratch, SetCC);
  j(cond, label);
}

template <typename T>
void MacroAssembler::branchAddPtr(Condition cond, T src, Register dest,
                                  Label* label) {
  branchAdd32(cond, src, dest, label);
}

template <typename T>
void MacroAssembler::branchSubPtr(Condition cond, T src, Register dest,
                                  Label* label) {
  branchSub32(cond, src, dest, label);
}

void MacroAssembler::branchMulPtr(Condition cond, Register src, Register dest,
                                  Label* label) {
  branchMul32(cond, src, dest, label);
}

void MacroAssembler::decBranchPtr(Condition cond, Register lhs, Imm32 rhs,
                                  Label* label) {
  ScratchRegisterScope scratch(*this);
  ma_sub(rhs, lhs, scratch, SetCC);
  as_b(label, cond);
}

template <class L>
void MacroAssembler::branchTest32(Condition cond, Register lhs, Register rhs,
                                  L label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  // x86 likes test foo, foo rather than cmp foo, #0.
  // Convert the former into the latter.
  if (lhs == rhs && (cond == Zero || cond == NonZero)) {
    as_cmp(lhs, Imm8(0));
  } else {
    ma_tst(lhs, rhs);
  }
  ma_b(label, cond);
}

template <class L>
void MacroAssembler::branchTest32(Condition cond, Register lhs, Imm32 rhs,
                                  L label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  ScratchRegisterScope scratch(*this);
  ma_tst(lhs, rhs, scratch);
  ma_b(label, cond);
}

void MacroAssembler::branchTest32(Condition cond, const Address& lhs, Imm32 rhs,
                                  Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  load32(lhs, scratch2);
  branchTest32(cond, scratch2, rhs, label);
}

void MacroAssembler::branchTest32(Condition cond, const AbsoluteAddress& lhs,
                                  Imm32 rhs, Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  load32(lhs, scratch2);
  branchTest32(cond, scratch2, rhs, label);
}

template <class L>
void MacroAssembler::branchTestPtr(Condition cond, Register lhs, Register rhs,
                                   L label) {
  branchTest32(cond, lhs, rhs, label);
}

void MacroAssembler::branchTestPtr(Condition cond, Register lhs, Imm32 rhs,
                                   Label* label) {
  branchTest32(cond, lhs, rhs, label);
}

void MacroAssembler::branchTestPtr(Condition cond, const Address& lhs,
                                   Imm32 rhs, Label* label) {
  branchTest32(cond, lhs, rhs, label);
}

template <class L>
void MacroAssembler::branchTest64(Condition cond, Register64 lhs,
                                  Register64 rhs, Register temp, L label) {
  if (cond == Assembler::Zero || cond == Assembler::NonZero) {
    ScratchRegisterScope scratch(*this);

    MOZ_ASSERT(lhs.low == rhs.low);
    MOZ_ASSERT(lhs.high == rhs.high);
    ma_orr(lhs.low, lhs.high, scratch);
    branchTestPtr(cond, scratch, scratch, label);
  } else if (cond == Assembler::Signed || cond == Assembler::NotSigned) {
    branchTest32(cond, lhs.high, rhs.high, label);
  } else {
    MOZ_CRASH("Unsupported condition");
  }
}

void MacroAssembler::branchTestUndefined(Condition cond, Register tag,
                                         Label* label) {
  branchTestUndefinedImpl(cond, tag, label);
}

void MacroAssembler::branchTestUndefined(Condition cond, const Address& address,
                                         Label* label) {
  branchTestUndefinedImpl(cond, address, label);
}

void MacroAssembler::branchTestUndefined(Condition cond,
                                         const BaseIndex& address,
                                         Label* label) {
  branchTestUndefinedImpl(cond, address, label);
}

void MacroAssembler::branchTestUndefined(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  branchTestUndefinedImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestUndefinedImpl(Condition cond, const T& t,
                                             Label* label) {
  Condition c = testUndefined(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestInt32(Condition cond, Register tag,
                                     Label* label) {
  branchTestInt32Impl(cond, tag, label);
}

void MacroAssembler::branchTestInt32(Condition cond, const Address& address,
                                     Label* label) {
  branchTestInt32Impl(cond, address, label);
}

void MacroAssembler::branchTestInt32(Condition cond, const BaseIndex& address,
                                     Label* label) {
  branchTestInt32Impl(cond, address, label);
}

void MacroAssembler::branchTestInt32(Condition cond, const ValueOperand& value,
                                     Label* label) {
  branchTestInt32Impl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestInt32Impl(Condition cond, const T& t,
                                         Label* label) {
  Condition c = testInt32(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestInt32Truthy(bool truthy,
                                           const ValueOperand& value,
                                           Label* label) {
  Condition c = testInt32Truthy(truthy, value);
  ma_b(label, c);
}

void MacroAssembler::branchTestDouble(Condition cond, Register tag,
                                      Label* label) {
  branchTestDoubleImpl(cond, tag, label);
}

void MacroAssembler::branchTestDouble(Condition cond, const Address& address,
                                      Label* label) {
  branchTestDoubleImpl(cond, address, label);
}

void MacroAssembler::branchTestDouble(Condition cond, const BaseIndex& address,
                                      Label* label) {
  branchTestDoubleImpl(cond, address, label);
}

void MacroAssembler::branchTestDouble(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestDoubleImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestDoubleImpl(Condition cond, const T& t,
                                          Label* label) {
  Condition c = testDouble(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestDoubleTruthy(bool truthy, FloatRegister reg,
                                            Label* label) {
  Condition c = testDoubleTruthy(truthy, reg);
  ma_b(label, c);
}

void MacroAssembler::branchTestNumber(Condition cond, Register tag,
                                      Label* label) {
  branchTestNumberImpl(cond, tag, label);
}

void MacroAssembler::branchTestNumber(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestNumberImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestNumberImpl(Condition cond, const T& t,
                                          Label* label) {
  cond = testNumber(cond, t);
  ma_b(label, cond);
}

void MacroAssembler::branchTestBoolean(Condition cond, Register tag,
                                       Label* label) {
  branchTestBooleanImpl(cond, tag, label);
}

void MacroAssembler::branchTestBoolean(Condition cond, const Address& address,
                                       Label* label) {
  branchTestBooleanImpl(cond, address, label);
}

void MacroAssembler::branchTestBoolean(Condition cond, const BaseIndex& address,
                                       Label* label) {
  branchTestBooleanImpl(cond, address, label);
}

void MacroAssembler::branchTestBoolean(Condition cond,
                                       const ValueOperand& value,
                                       Label* label) {
  branchTestBooleanImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestBooleanImpl(Condition cond, const T& t,
                                           Label* label) {
  Condition c = testBoolean(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestBooleanTruthy(bool truthy,
                                             const ValueOperand& value,
                                             Label* label) {
  Condition c = testBooleanTruthy(truthy, value);
  ma_b(label, c);
}

void MacroAssembler::branchTestString(Condition cond, Register tag,
                                      Label* label) {
  branchTestStringImpl(cond, tag, label);
}

void MacroAssembler::branchTestString(Condition cond, const Address& address,
                                      Label* label) {
  branchTestStringImpl(cond, address, label);
}

void MacroAssembler::branchTestString(Condition cond, const BaseIndex& address,
                                      Label* label) {
  branchTestStringImpl(cond, address, label);
}

void MacroAssembler::branchTestString(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestStringImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestStringImpl(Condition cond, const T& t,
                                          Label* label) {
  Condition c = testString(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestStringTruthy(bool truthy,
                                            const ValueOperand& value,
                                            Label* label) {
  Condition c = testStringTruthy(truthy, value);
  ma_b(label, c);
}

void MacroAssembler::branchTestSymbol(Condition cond, Register tag,
                                      Label* label) {
  branchTestSymbolImpl(cond, tag, label);
}

void MacroAssembler::branchTestSymbol(Condition cond, const Address& address,
                                      Label* label) {
  branchTestSymbolImpl(cond, address, label);
}

void MacroAssembler::branchTestSymbol(Condition cond, const BaseIndex& address,
                                      Label* label) {
  branchTestSymbolImpl(cond, address, label);
}

void MacroAssembler::branchTestSymbol(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestSymbolImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestSymbolImpl(Condition cond, const T& t,
                                          Label* label) {
  Condition c = testSymbol(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestBigInt(Condition cond, Register tag,
                                      Label* label) {
  branchTestBigIntImpl(cond, tag, label);
}

void MacroAssembler::branchTestBigInt(Condition cond, const Address& address,
                                      Label* label) {
  branchTestBigIntImpl(cond, address, label);
}

void MacroAssembler::branchTestBigInt(Condition cond, const BaseIndex& address,
                                      Label* label) {
  branchTestBigIntImpl(cond, address, label);
}

void MacroAssembler::branchTestBigInt(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestBigIntImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestBigIntImpl(Condition cond, const T& t,
                                          Label* label) {
  Condition c = testBigInt(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestBigIntTruthy(bool truthy,
                                            const ValueOperand& value,
                                            Label* label) {
  Condition c = testBigIntTruthy(truthy, value);
  ma_b(label, c);
}

void MacroAssembler::branchTestNull(Condition cond, Register tag,
                                    Label* label) {
  branchTestNullImpl(cond, tag, label);
}

void MacroAssembler::branchTestNull(Condition cond, const Address& address,
                                    Label* label) {
  branchTestNullImpl(cond, address, label);
}

void MacroAssembler::branchTestNull(Condition cond, const BaseIndex& address,
                                    Label* label) {
  branchTestNullImpl(cond, address, label);
}

void MacroAssembler::branchTestNull(Condition cond, const ValueOperand& value,
                                    Label* label) {
  branchTestNullImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestNullImpl(Condition cond, const T& t,
                                        Label* label) {
  Condition c = testNull(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestObject(Condition cond, Register tag,
                                      Label* label) {
  branchTestObjectImpl(cond, tag, label);
}

void MacroAssembler::branchTestObject(Condition cond, const Address& address,
                                      Label* label) {
  branchTestObjectImpl(cond, address, label);
}

void MacroAssembler::branchTestObject(Condition cond, const BaseIndex& address,
                                      Label* label) {
  branchTestObjectImpl(cond, address, label);
}

void MacroAssembler::branchTestObject(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestObjectImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestObjectImpl(Condition cond, const T& t,
                                          Label* label) {
  Condition c = testObject(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestGCThing(Condition cond, const Address& address,
                                       Label* label) {
  branchTestGCThingImpl(cond, address, label);
}

void MacroAssembler::branchTestGCThing(Condition cond, const BaseIndex& address,
                                       Label* label) {
  branchTestGCThingImpl(cond, address, label);
}

void MacroAssembler::branchTestGCThing(Condition cond,
                                       const ValueOperand& value,
                                       Label* label) {
  branchTestGCThingImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestGCThingImpl(Condition cond, const T& t,
                                           Label* label) {
  Condition c = testGCThing(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestPrimitive(Condition cond, Register tag,
                                         Label* label) {
  branchTestPrimitiveImpl(cond, tag, label);
}

void MacroAssembler::branchTestPrimitive(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  branchTestPrimitiveImpl(cond, value, label);
}

template <typename T>
void MacroAssembler::branchTestPrimitiveImpl(Condition cond, const T& t,
                                             Label* label) {
  Condition c = testPrimitive(cond, t);
  ma_b(label, c);
}

void MacroAssembler::branchTestMagic(Condition cond, Register tag,
                                     Label* label) {
  branchTestMagicImpl(cond, tag, label);
}

void MacroAssembler::branchTestMagic(Condition cond, const Address& address,
                                     Label* label) {
  branchTestMagicImpl(cond, address, label);
}

void MacroAssembler::branchTestMagic(Condition cond, const BaseIndex& address,
                                     Label* label) {
  branchTestMagicImpl(cond, address, label);
}

template <class L>
void MacroAssembler::branchTestMagic(Condition cond, const ValueOperand& value,
                                     L label) {
  branchTestMagicImpl(cond, value, label);
}

template <typename T, class L>
void MacroAssembler::branchTestMagicImpl(Condition cond, const T& t, L label) {
  cond = testMagic(cond, t);
  ma_b(label, cond);
}

void MacroAssembler::branchTestMagic(Condition cond, const Address& valaddr,
                                     JSWhyMagic why, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  Label notMagic;
  if (cond == Assembler::Equal) {
    branchTestMagic(Assembler::NotEqual, valaddr, &notMagic);
  } else {
    branchTestMagic(Assembler::NotEqual, valaddr, label);
  }

  branch32(cond, ToPayload(valaddr), Imm32(why), label);
  bind(&notMagic);
}

void MacroAssembler::branchTestValue(Condition cond, const BaseIndex& lhs,
                                     const ValueOperand& rhs, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  Label notSameValue;
  if (cond == Assembler::Equal) {
    branch32(Assembler::NotEqual, ToType(lhs), rhs.typeReg(), &notSameValue);
  } else {
    branch32(Assembler::NotEqual, ToType(lhs), rhs.typeReg(), label);
  }

  branch32(cond, ToPayload(lhs), rhs.payloadReg(), label);
  bind(&notSameValue);
}

template <typename T>
void MacroAssembler::testNumberSet(Condition cond, const T& src,
                                   Register dest) {
  cond = testNumber(cond, src);
  emitSet(cond, dest);
}

template <typename T>
void MacroAssembler::testBooleanSet(Condition cond, const T& src,
                                    Register dest) {
  cond = testBoolean(cond, src);
  emitSet(cond, dest);
}

template <typename T>
void MacroAssembler::testStringSet(Condition cond, const T& src,
                                   Register dest) {
  cond = testString(cond, src);
  emitSet(cond, dest);
}

template <typename T>
void MacroAssembler::testSymbolSet(Condition cond, const T& src,
                                   Register dest) {
  cond = testSymbol(cond, src);
  emitSet(cond, dest);
}

template <typename T>
void MacroAssembler::testBigIntSet(Condition cond, const T& src,
                                   Register dest) {
  cond = testBigInt(cond, src);
  emitSet(cond, dest);
}

void MacroAssembler::branchToComputedAddress(const BaseIndex& addr) {
  MOZ_ASSERT(
      addr.offset == 0,
      "NYI: offsets from pc should be shifted by the number of instructions.");

  Register base = addr.base;
  uint32_t scale = Imm32::ShiftOf(addr.scale).value;

  ma_ldr(DTRAddr(base, DtrRegImmShift(addr.index, LSL, scale)), pc);

  if (base == pc) {
    // When loading from pc, the pc is shifted to the next instruction, we
    // add one extra instruction to accomodate for this shifted offset.
    breakpoint();
  }
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs, Imm32 rhs,
                                 Register src, Register dest) {
  cmp32(lhs, rhs);
  ma_mov(src, dest, LeaveCC, cond);
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs, Register rhs,
                                 Register src, Register dest) {
  cmp32(lhs, rhs);
  ma_mov(src, dest, LeaveCC, cond);
}

void MacroAssembler::cmp32MovePtr(Condition cond, Register lhs, Imm32 rhs,
                                  Register src, Register dest) {
  cmp32(lhs, rhs);
  ma_mov(src, dest, LeaveCC, cond);
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs,
                                 const Address& rhs, Register src,
                                 Register dest) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);
  ma_ldr(rhs, scratch, scratch2);
  cmp32Move32(cond, lhs, scratch, src, dest);
}

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs, Register rhs,
                                   Register src, Register dest) {
  cmp32Move32(cond, lhs, rhs, src, dest);
}

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs,
                                   const Address& rhs, Register src,
                                   Register dest) {
  cmp32Move32(cond, lhs, rhs, src, dest);
}

void MacroAssembler::cmp32Load32(Condition cond, Register lhs,
                                 const Address& rhs, const Address& src,
                                 Register dest) {
  // This is never used, but must be present to facilitate linking on arm.
  MOZ_CRASH("No known use cases");
}

void MacroAssembler::cmp32Load32(Condition cond, Register lhs, Register rhs,
                                 const Address& src, Register dest) {
  // This is never used, but must be present to facilitate linking on arm.
  MOZ_CRASH("No known use cases");
}

void MacroAssembler::cmp32Load32(Condition cond, Register lhs, Imm32 rhs,
                                 const Address& src, Register dest) {
  cmp32(lhs, rhs);
  ScratchRegisterScope scratch(*this);
  ma_ldr(src, dest, scratch, Offset, cond);
}

void MacroAssembler::cmp32LoadPtr(Condition cond, const Address& lhs, Imm32 rhs,
                                  const Address& src, Register dest) {
  cmp32(lhs, rhs);
  ScratchRegisterScope scratch(*this);
  ma_ldr(src, dest, scratch, Offset, cond);
}

void MacroAssembler::test32LoadPtr(Condition cond, const Address& addr,
                                   Imm32 mask, const Address& src,
                                   Register dest) {
  MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);
  test32(addr, mask);
  ScratchRegisterScope scratch(*this);
  ma_ldr(src, dest, scratch, Offset, cond);
}

void MacroAssembler::test32MovePtr(Condition cond, const Address& addr,
                                   Imm32 mask, Register src, Register dest) {
  MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);
  test32(addr, mask);
  ma_mov(src, dest, LeaveCC, cond);
}

void MacroAssembler::spectreMovePtr(Condition cond, Register src,
                                    Register dest) {
  ma_mov(src, dest, LeaveCC, cond);
}

void MacroAssembler::spectreZeroRegister(Condition cond, Register,
                                         Register dest) {
  ma_mov(Imm32(0), dest, cond);
}

void MacroAssembler::spectreBoundsCheck32(Register index, Register length,
                                          Register maybeScratch,
                                          Label* failure) {
  MOZ_ASSERT(length != maybeScratch);
  MOZ_ASSERT(index != maybeScratch);

  branch32(Assembler::BelowOrEqual, length, index, failure);

  if (JitOptions.spectreIndexMasking) {
    ma_mov(Imm32(0), index, Assembler::BelowOrEqual);
  }
}

void MacroAssembler::spectreBoundsCheck32(Register index, const Address& length,
                                          Register maybeScratch,
                                          Label* failure) {
  MOZ_ASSERT(index != length.base);
  MOZ_ASSERT(length.base != maybeScratch);
  MOZ_ASSERT(index != maybeScratch);

  branch32(Assembler::BelowOrEqual, length, index, failure);

  if (JitOptions.spectreIndexMasking) {
    ma_mov(Imm32(0), index, Assembler::BelowOrEqual);
  }
}

void MacroAssembler::spectreBoundsCheckPtr(Register index, Register length,
                                           Register maybeScratch,
                                           Label* failure) {
  spectreBoundsCheck32(index, length, maybeScratch, failure);
}

void MacroAssembler::spectreBoundsCheckPtr(Register index,
                                           const Address& length,
                                           Register maybeScratch,
                                           Label* failure) {
  spectreBoundsCheck32(index, length, maybeScratch, failure);
}

// ========================================================================
// Memory access primitives.
FaultingCodeOffset MacroAssembler::storeUncanonicalizedDouble(
    FloatRegister src, const Address& addr) {
  ScratchRegisterScope scratch(*this);
  BufferOffset offset = ma_vstr(src, addr, scratch);
  return FaultingCodeOffset(offset.getOffset());
}
FaultingCodeOffset MacroAssembler::storeUncanonicalizedDouble(
    FloatRegister src, const BaseIndex& addr) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);
  uint32_t scale = Imm32::ShiftOf(addr.scale).value;
  BufferOffset offset = ma_vstr(src, addr.base, addr.index, scratch, scratch2,
                                scale, addr.offset);
  return FaultingCodeOffset(offset.getOffset());
}

FaultingCodeOffset MacroAssembler::storeUncanonicalizedFloat32(
    FloatRegister src, const Address& addr) {
  ScratchRegisterScope scratch(*this);
  BufferOffset offset = ma_vstr(src.asSingle(), addr, scratch);
  return FaultingCodeOffset(offset.getOffset());
}
FaultingCodeOffset MacroAssembler::storeUncanonicalizedFloat32(
    FloatRegister src, const BaseIndex& addr) {
  ScratchRegisterScope scratch(*this);
  SecondScratchRegisterScope scratch2(*this);
  uint32_t scale = Imm32::ShiftOf(addr.scale).value;
  BufferOffset offset = ma_vstr(src.asSingle(), addr.base, addr.index, scratch,
                                scratch2, scale, addr.offset);
  return FaultingCodeOffset(offset.getOffset());
}

void MacroAssembler::memoryBarrier(MemoryBarrierBits barrier) {
  // On ARMv6 the optional argument (BarrierST, etc) is ignored.
  if (barrier == (MembarStoreStore | MembarSynchronizing)) {
    ma_dsb(BarrierST);
  } else if (barrier & MembarSynchronizing) {
    ma_dsb();
  } else if (barrier == MembarStoreStore) {
    ma_dmb(BarrierST);
  } else if (barrier) {
    ma_dmb();
  }
}

// ===============================================================
// Clamping functions.

void MacroAssembler::clampIntToUint8(Register reg) {
  // Look at (reg >> 8) if it is 0, then reg shouldn't be clamped if it is
  // <0, then we want to clamp to 0, otherwise, we wish to clamp to 255
  ScratchRegisterScope scratch(*this);
  as_mov(scratch, asr(reg, 8), SetCC);
  ma_mov(Imm32(0xff), reg, NotEqual);
  ma_mov(Imm32(0), reg, Signed);
}

template <typename T>
void MacroAssemblerARMCompat::fallibleUnboxPtrImpl(const T& src, Register dest,
                                                   JSValueType type,
                                                   Label* fail) {
  switch (type) {
    case JSVAL_TYPE_OBJECT:
      asMasm().branchTestObject(Assembler::NotEqual, src, fail);
      break;
    case JSVAL_TYPE_STRING:
      asMasm().branchTestString(Assembler::NotEqual, src, fail);
      break;
    case JSVAL_TYPE_SYMBOL:
      asMasm().branchTestSymbol(Assembler::NotEqual, src, fail);
      break;
    case JSVAL_TYPE_BIGINT:
      asMasm().branchTestBigInt(Assembler::NotEqual, src, fail);
      break;
    default:
      MOZ_CRASH("Unexpected type");
  }
  unboxNonDouble(src, dest, type);
}

void MacroAssembler::fallibleUnboxPtr(const ValueOperand& src, Register dest,
                                      JSValueType type, Label* fail) {
  fallibleUnboxPtrImpl(src, dest, type, fail);
}

void MacroAssembler::fallibleUnboxPtr(const Address& src, Register dest,
                                      JSValueType type, Label* fail) {
  fallibleUnboxPtrImpl(src, dest, type, fail);
}

void MacroAssembler::fallibleUnboxPtr(const BaseIndex& src, Register dest,
                                      JSValueType type, Label* fail) {
  fallibleUnboxPtrImpl(src, dest, type, fail);
}

//}}} check_macroassembler_style
// ===============================================================

void MacroAssemblerARMCompat::incrementInt32Value(const Address& addr) {
  asMasm().add32(Imm32(1), ToPayload(addr));
}

}  // namespace jit
}  // namespace js

#endif /* jit_arm_MacroAssembler_arm_inl_h */
