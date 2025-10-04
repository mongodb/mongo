/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_loong64_MacroAssembler_loong64_inl_h
#define jit_loong64_MacroAssembler_loong64_inl_h

#include "jit/loong64/MacroAssembler-loong64.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style

void MacroAssembler::move64(Register64 src, Register64 dest) {
  movePtr(src.reg, dest.reg);
}

void MacroAssembler::move64(Imm64 imm, Register64 dest) {
  movePtr(ImmWord(imm.value), dest.reg);
}

void MacroAssembler::moveDoubleToGPR64(FloatRegister src, Register64 dest) {
  moveFromDouble(src, dest.reg);
}

void MacroAssembler::moveGPR64ToDouble(Register64 src, FloatRegister dest) {
  moveToDouble(src.reg, dest);
}

void MacroAssembler::move64To32(Register64 src, Register dest) {
  as_slli_w(dest, src.reg, 0);
}

void MacroAssembler::move32To64ZeroExtend(Register src, Register64 dest) {
  as_bstrpick_d(dest.reg, src, 31, 0);
}

void MacroAssembler::move8To64SignExtend(Register src, Register64 dest) {
  move32To64SignExtend(src, dest);
  move8SignExtend(dest.reg, dest.reg);
}

void MacroAssembler::move16To64SignExtend(Register src, Register64 dest) {
  move32To64SignExtend(src, dest);
  move16SignExtend(dest.reg, dest.reg);
}

void MacroAssembler::move32To64SignExtend(Register src, Register64 dest) {
  as_slli_w(dest.reg, src, 0);
}

void MacroAssembler::moveFloat32ToGPR(FloatRegister src, Register dest) {
  moveFromFloat32(src, dest);
}

void MacroAssembler::moveGPRToFloat32(Register src, FloatRegister dest) {
  moveToFloat32(src, dest);
}

void MacroAssembler::move8ZeroExtend(Register src, Register dest) {
  as_bstrpick_d(dest, src, 7, 0);
}

void MacroAssembler::move8SignExtend(Register src, Register dest) {
  as_ext_w_b(dest, src);
}

void MacroAssembler::move16SignExtend(Register src, Register dest) {
  as_ext_w_h(dest, src);
}

void MacroAssembler::move32SignExtendToPtr(Register src, Register dest) {
  as_slli_w(dest, src, 0);
}

void MacroAssembler::move32ZeroExtendToPtr(Register src, Register dest) {
  as_bstrpick_d(dest, src, 31, 0);
}

// ===============================================================
// Load instructions

void MacroAssembler::load32SignExtendToPtr(const Address& src, Register dest) {
  load32(src, dest);
}

void MacroAssembler::loadAbiReturnAddress(Register dest) { movePtr(ra, dest); }

// ===============================================================
// Logical instructions

void MacroAssembler::not32(Register reg) { as_nor(reg, reg, zero); }

void MacroAssembler::notPtr(Register reg) { as_nor(reg, reg, zero); }

void MacroAssembler::andPtr(Register src, Register dest) {
  as_and(dest, dest, src);
}

void MacroAssembler::andPtr(Imm32 imm, Register dest) {
  ma_and(dest, dest, imm);
}

void MacroAssembler::and64(Imm64 imm, Register64 dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_li(scratch, ImmWord(imm.value));
  as_and(dest.reg, dest.reg, scratch);
}

void MacroAssembler::and64(Register64 src, Register64 dest) {
  as_and(dest.reg, dest.reg, src.reg);
}

void MacroAssembler::and64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    ScratchRegisterScope scratch(*this);
    Register64 scratch64(scratch);

    load64(src.toAddress(), scratch64);
    and64(scratch64, dest);
  } else {
    and64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::and32(Register src, Register dest) {
  as_and(dest, dest, src);
}

void MacroAssembler::and32(Imm32 imm, Register dest) {
  ma_and(dest, dest, imm);
}

void MacroAssembler::and32(Imm32 imm, const Address& dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(dest, scratch2);
  and32(imm, scratch2);
  store32(scratch2, dest);
}

void MacroAssembler::and32(const Address& src, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(src, scratch2);
  as_and(dest, dest, scratch2);
}

void MacroAssembler::or64(Imm64 imm, Register64 dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_li(scratch, ImmWord(imm.value));
  as_or(dest.reg, dest.reg, scratch);
}

void MacroAssembler::or32(Register src, Register dest) {
  as_or(dest, dest, src);
}

void MacroAssembler::or32(Imm32 imm, Register dest) { ma_or(dest, dest, imm); }

void MacroAssembler::or32(Imm32 imm, const Address& dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(dest, scratch2);
  or32(imm, scratch2);
  store32(scratch2, dest);
}

void MacroAssembler::xor64(Imm64 imm, Register64 dest) {
  ScratchRegisterScope scratch(asMasm());
  ma_li(scratch, ImmWord(imm.value));
  as_xor(dest.reg, dest.reg, scratch);
}

void MacroAssembler::orPtr(Register src, Register dest) {
  as_or(dest, dest, src);
}

void MacroAssembler::orPtr(Imm32 imm, Register dest) { ma_or(dest, dest, imm); }

void MacroAssembler::or64(Register64 src, Register64 dest) {
  as_or(dest.reg, dest.reg, src.reg);
}

void MacroAssembler::or64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    ScratchRegisterScope scratch(asMasm());
    Register64 scratch64(scratch);

    load64(src.toAddress(), scratch64);
    or64(scratch64, dest);
  } else {
    or64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::xor64(Register64 src, Register64 dest) {
  as_xor(dest.reg, dest.reg, src.reg);
}

void MacroAssembler::xor64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    ScratchRegisterScope scratch(asMasm());
    Register64 scratch64(scratch);

    load64(src.toAddress(), scratch64);
    xor64(scratch64, dest);
  } else {
    xor64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::xorPtr(Register src, Register dest) {
  as_xor(dest, dest, src);
}

void MacroAssembler::xorPtr(Imm32 imm, Register dest) {
  ma_xor(dest, dest, imm);
}

void MacroAssembler::xor32(Register src, Register dest) {
  as_xor(dest, dest, src);
}

void MacroAssembler::xor32(Imm32 imm, Register dest) {
  ma_xor(dest, dest, imm);
}

void MacroAssembler::xor32(Imm32 imm, const Address& dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(dest, scratch2);
  xor32(imm, scratch2);
  store32(scratch2, dest);
}

void MacroAssembler::xor32(const Address& src, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(src, scratch2);
  xor32(scratch2, dest);
}

// ===============================================================
// Swap instructions

void MacroAssembler::byteSwap16SignExtend(Register reg) {
  as_revb_2h(reg, reg);
  as_ext_w_h(reg, reg);
}

void MacroAssembler::byteSwap16ZeroExtend(Register reg) {
  as_revb_2h(reg, reg);
  as_bstrpick_d(reg, reg, 15, 0);
}

void MacroAssembler::byteSwap32(Register reg) {
  as_revb_2w(reg, reg);
  as_slli_w(reg, reg, 0);
}

void MacroAssembler::byteSwap64(Register64 reg64) {
  as_revb_d(reg64.reg, reg64.reg);
}

// ===============================================================
// Arithmetic functions

void MacroAssembler::addPtr(Register src, Register dest) {
  as_add_d(dest, dest, src);
}

void MacroAssembler::addPtr(Imm32 imm, Register dest) {
  ma_add_d(dest, dest, imm);
}

void MacroAssembler::addPtr(ImmWord imm, Register dest) {
  ScratchRegisterScope scratch(asMasm());
  movePtr(imm, scratch);
  addPtr(scratch, dest);
}

void MacroAssembler::add64(Register64 src, Register64 dest) {
  addPtr(src.reg, dest.reg);
}

void MacroAssembler::add64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    ScratchRegisterScope scratch(asMasm());
    Register64 scratch64(scratch);

    load64(src.toAddress(), scratch64);
    add64(scratch64, dest);
  } else {
    add64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::add64(Imm32 imm, Register64 dest) {
  ma_add_d(dest.reg, dest.reg, imm);
}

void MacroAssembler::add64(Imm64 imm, Register64 dest) {
  ScratchRegisterScope scratch(asMasm());
  MOZ_ASSERT(dest.reg != scratch);
  mov(ImmWord(imm.value), scratch);
  as_add_d(dest.reg, dest.reg, scratch);
}

void MacroAssembler::add32(Register src, Register dest) {
  as_add_w(dest, dest, src);
}

void MacroAssembler::add32(Imm32 imm, Register dest) {
  ma_add_w(dest, dest, imm);
}

void MacroAssembler::add32(Imm32 imm, Register src, Register dest) {
  ma_add_w(dest, src, imm);
}

void MacroAssembler::add32(Imm32 imm, const Address& dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(dest, scratch2);
  ma_add_w(scratch2, scratch2, imm);
  store32(scratch2, dest);
}

void MacroAssembler::addPtr(Imm32 imm, const Address& dest) {
  ScratchRegisterScope scratch(asMasm());
  loadPtr(dest, scratch);
  addPtr(imm, scratch);
  storePtr(scratch, dest);
}

void MacroAssembler::addPtr(const Address& src, Register dest) {
  ScratchRegisterScope scratch(asMasm());
  loadPtr(src, scratch);
  addPtr(scratch, dest);
}

void MacroAssembler::addDouble(FloatRegister src, FloatRegister dest) {
  as_fadd_d(dest, dest, src);
}

void MacroAssembler::addFloat32(FloatRegister src, FloatRegister dest) {
  as_fadd_s(dest, dest, src);
}

CodeOffset MacroAssembler::sub32FromStackPtrWithPatch(Register dest) {
  CodeOffset offset = CodeOffset(currentOffset());
  MacroAssemblerLOONG64::ma_liPatchable(dest, Imm32(0));
  as_sub_d(dest, StackPointer, dest);
  return offset;
}

void MacroAssembler::patchSub32FromStackPtr(CodeOffset offset, Imm32 imm) {
  // TODO: by wangqing
  Instruction* inst0 =
      (Instruction*)m_buffer.getInst(BufferOffset(offset.offset()));

  InstImm* i0 = (InstImm*)inst0;
  InstImm* i1 = (InstImm*)i0->next();

  MOZ_ASSERT((i0->extractBitField(31, 25)) == ((uint32_t)op_lu12i_w >> 25));
  MOZ_ASSERT((i1->extractBitField(31, 22)) == ((uint32_t)op_ori >> 22));

  *i0 = InstImm(op_lu12i_w, (int32_t)((imm.value >> 12) & 0xfffff),
                Register::FromCode(i0->extractRD()), false);
  *i1 = InstImm(op_ori, (int32_t)(imm.value & 0xfff),
                Register::FromCode(i1->extractRJ()),
                Register::FromCode(i1->extractRD()), 12);
}

void MacroAssembler::subPtr(Register src, Register dest) {
  as_sub_d(dest, dest, src);
}

void MacroAssembler::subPtr(Imm32 imm, Register dest) {
  ma_sub_d(dest, dest, imm);
}

void MacroAssembler::sub64(Register64 src, Register64 dest) {
  as_sub_d(dest.reg, dest.reg, src.reg);
}

void MacroAssembler::sub64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    ScratchRegisterScope scratch(asMasm());
    Register64 scratch64(scratch);

    load64(src.toAddress(), scratch64);
    sub64(scratch64, dest);
  } else {
    sub64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::sub64(Imm64 imm, Register64 dest) {
  ScratchRegisterScope scratch(asMasm());
  MOZ_ASSERT(dest.reg != scratch);
  mov(ImmWord(imm.value), scratch);
  as_sub_d(dest.reg, dest.reg, scratch);
}

void MacroAssembler::sub32(Register src, Register dest) {
  as_sub_w(dest, dest, src);
}

void MacroAssembler::sub32(Imm32 imm, Register dest) {
  ma_sub_w(dest, dest, imm);
}

void MacroAssembler::sub32(const Address& src, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(src, scratch2);
  as_sub_w(dest, dest, scratch2);
}

void MacroAssembler::subPtr(Register src, const Address& dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(dest, scratch2);
  subPtr(src, scratch2);
  storePtr(scratch2, dest);
}

void MacroAssembler::subPtr(const Address& addr, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(addr, scratch2);
  subPtr(scratch2, dest);
}

void MacroAssembler::subDouble(FloatRegister src, FloatRegister dest) {
  as_fsub_d(dest, dest, src);
}

void MacroAssembler::subFloat32(FloatRegister src, FloatRegister dest) {
  as_fsub_s(dest, dest, src);
}

void MacroAssembler::mul64(Imm64 imm, const Register64& dest) {
  ScratchRegisterScope scratch(asMasm());
  MOZ_ASSERT(dest.reg != scratch);
  mov(ImmWord(imm.value), scratch);
  as_mul_d(dest.reg, dest.reg, scratch);
}

void MacroAssembler::mul64(Imm64 imm, const Register64& dest,
                           const Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  mul64(imm, dest);
}

void MacroAssembler::mul64(const Register64& src, const Register64& dest,
                           const Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  as_mul_d(dest.reg, dest.reg, src.reg);
}

void MacroAssembler::mul64(const Operand& src, const Register64& dest,
                           const Register temp) {
  if (src.getTag() == Operand::MEM) {
    ScratchRegisterScope scratch(asMasm());
    Register64 scratch64(scratch);

    load64(src.toAddress(), scratch64);
    mul64(scratch64, dest, temp);
  } else {
    mul64(Register64(src.toReg()), dest, temp);
  }
}

void MacroAssembler::mulPtr(Register rhs, Register srcDest) {
  as_mul_d(srcDest, srcDest, rhs);
}

void MacroAssembler::mulBy3(Register src, Register dest) {
  ScratchRegisterScope scratch(asMasm());
  MOZ_ASSERT(src != scratch);
  as_add_d(scratch, src, src);
  as_add_d(dest, scratch, src);
}

void MacroAssembler::mul32(Register rhs, Register srcDest) {
  as_mul_w(srcDest, srcDest, rhs);
}

void MacroAssembler::mul32(Imm32 imm, Register srcDest) {
  ScratchRegisterScope scratch(asMasm());
  move32(imm, scratch);
  mul32(scratch, srcDest);
}

void MacroAssembler::mulHighUnsigned32(Imm32 imm, Register src, Register dest) {
  ScratchRegisterScope scratch(asMasm());
  MOZ_ASSERT(src != scratch);
  move32(imm, scratch);
  as_mulh_wu(dest, src, scratch);
}

void MacroAssembler::mulFloat32(FloatRegister src, FloatRegister dest) {
  as_fmul_s(dest, dest, src);
}

void MacroAssembler::mulDouble(FloatRegister src, FloatRegister dest) {
  as_fmul_d(dest, dest, src);
}

void MacroAssembler::mulDoublePtr(ImmPtr imm, Register temp,
                                  FloatRegister dest) {
  ScratchRegisterScope scratch(asMasm());
  ScratchDoubleScope fpscratch(asMasm());
  movePtr(imm, scratch);
  loadDouble(Address(scratch, 0), fpscratch);
  mulDouble(fpscratch, dest);
}

void MacroAssembler::inc64(AbsoluteAddress dest) {
  ScratchRegisterScope scratch(asMasm());
  SecondScratchRegisterScope scratch2(asMasm());
  ma_li(scratch, ImmWord(uintptr_t(dest.addr)));
  as_ld_d(scratch2, scratch, 0);
  as_addi_d(scratch2, scratch2, 1);
  as_st_d(scratch2, scratch, 0);
}

void MacroAssembler::quotient32(Register rhs, Register srcDest,
                                bool isUnsigned) {
  if (isUnsigned) {
    as_div_wu(srcDest, srcDest, rhs);
  } else {
    as_div_w(srcDest, srcDest, rhs);
  }
}

void MacroAssembler::remainder32(Register rhs, Register srcDest,
                                 bool isUnsigned) {
  if (isUnsigned) {
    as_mod_wu(srcDest, srcDest, rhs);
  } else {
    as_mod_w(srcDest, srcDest, rhs);
  }
}

void MacroAssembler::divFloat32(FloatRegister src, FloatRegister dest) {
  as_fdiv_s(dest, dest, src);
}

void MacroAssembler::divDouble(FloatRegister src, FloatRegister dest) {
  as_fdiv_d(dest, dest, src);
}

void MacroAssembler::neg64(Register64 reg) { as_sub_d(reg.reg, zero, reg.reg); }

void MacroAssembler::negPtr(Register reg) { as_sub_d(reg, zero, reg); }

void MacroAssembler::neg32(Register reg) { as_sub_w(reg, zero, reg); }

void MacroAssembler::negateDouble(FloatRegister reg) { as_fneg_d(reg, reg); }

void MacroAssembler::negateFloat(FloatRegister reg) { as_fneg_s(reg, reg); }

void MacroAssembler::abs32(Register src, Register dest) {
  ScratchRegisterScope scratch(asMasm());
  as_srai_w(scratch, src, 31);
  as_xor(dest, src, scratch);
  as_sub_w(dest, dest, scratch);
}

void MacroAssembler::absFloat32(FloatRegister src, FloatRegister dest) {
  as_fabs_s(dest, src);
}

void MacroAssembler::absDouble(FloatRegister src, FloatRegister dest) {
  as_fabs_d(dest, src);
}

void MacroAssembler::sqrtFloat32(FloatRegister src, FloatRegister dest) {
  as_fsqrt_s(dest, src);
}

void MacroAssembler::sqrtDouble(FloatRegister src, FloatRegister dest) {
  as_fsqrt_d(dest, src);
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

void MacroAssembler::lshift32(Register src, Register dest) {
  as_sll_w(dest, dest, src);
}

void MacroAssembler::lshift32(Imm32 imm, Register dest) {
  as_slli_w(dest, dest, imm.value % 32);
}

void MacroAssembler::flexibleLshift32(Register src, Register dest) {
  lshift32(src, dest);
}

void MacroAssembler::lshift64(Register shift, Register64 dest) {
  as_sll_d(dest.reg, dest.reg, shift);
}

void MacroAssembler::lshift64(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  as_slli_d(dest.reg, dest.reg, imm.value);
}

void MacroAssembler::lshiftPtr(Register shift, Register dest) {
  as_sll_d(dest, dest, shift);
}

void MacroAssembler::lshiftPtr(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  as_slli_d(dest, dest, imm.value);
}

void MacroAssembler::rshift32(Register src, Register dest) {
  as_srl_w(dest, dest, src);
}

void MacroAssembler::rshift32(Imm32 imm, Register dest) {
  as_srli_w(dest, dest, imm.value % 32);
}

void MacroAssembler::flexibleRshift32(Register src, Register dest) {
  rshift32(src, dest);
}

void MacroAssembler::rshift32Arithmetic(Register src, Register dest) {
  as_sra_w(dest, dest, src);
}

void MacroAssembler::rshift32Arithmetic(Imm32 imm, Register dest) {
  as_srai_w(dest, dest, imm.value % 32);
}

void MacroAssembler::flexibleRshift32Arithmetic(Register src, Register dest) {
  rshift32Arithmetic(src, dest);
}

void MacroAssembler::rshift64(Register shift, Register64 dest) {
  as_srl_d(dest.reg, dest.reg, shift);
}

void MacroAssembler::rshift64(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  as_srli_d(dest.reg, dest.reg, imm.value);
}

void MacroAssembler::rshift64Arithmetic(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  as_srai_d(dest.reg, dest.reg, imm.value);
}

void MacroAssembler::rshift64Arithmetic(Register shift, Register64 dest) {
  as_sra_d(dest.reg, dest.reg, shift);
}

void MacroAssembler::rshiftPtr(Register shift, Register dest) {
  as_srl_d(dest, dest, shift);
}

void MacroAssembler::rshiftPtr(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  as_srli_d(dest, dest, imm.value);
}

void MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  as_srai_d(dest, dest, imm.value);
}

// ===============================================================
// Rotation functions

void MacroAssembler::rotateLeft(Register count, Register input, Register dest) {
  ScratchRegisterScope scratch(asMasm());
  as_sub_w(scratch, zero, count);
  as_rotr_w(dest, input, scratch);
}

void MacroAssembler::rotateLeft(Imm32 count, Register input, Register dest) {
  as_rotri_w(dest, input, (32 - count.value) & 31);
}

void MacroAssembler::rotateLeft64(Register count, Register64 src,
                                  Register64 dest, Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  ScratchRegisterScope scratch(asMasm());
  as_sub_d(scratch, zero, count);
  as_rotr_d(dest.reg, src.reg, scratch);
}

void MacroAssembler::rotateLeft64(Imm32 count, Register64 src, Register64 dest,
                                  Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  as_rotri_d(dest.reg, src.reg, (64 - count.value) & 63);
}

void MacroAssembler::rotateRight(Register count, Register input,
                                 Register dest) {
  as_rotr_w(dest, input, count);
}

void MacroAssembler::rotateRight(Imm32 count, Register input, Register dest) {
  as_rotri_w(dest, input, count.value & 31);
}

void MacroAssembler::rotateRight64(Register count, Register64 src,
                                   Register64 dest, Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  as_rotr_d(dest.reg, src.reg, count);
}

void MacroAssembler::rotateRight64(Imm32 count, Register64 src, Register64 dest,
                                   Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  as_rotri_d(dest.reg, src.reg, count.value & 63);
}

// Bit counting functions

void MacroAssembler::clz64(Register64 src, Register dest) {
  as_clz_d(dest, src.reg);
}

void MacroAssembler::ctz64(Register64 src, Register dest) {
  as_ctz_d(dest, src.reg);
}

void MacroAssembler::popcnt64(Register64 input, Register64 output,
                              Register tmp) {
  ScratchRegisterScope scratch(asMasm());
  as_or(output.reg, input.reg, zero);
  as_srai_d(tmp, input.reg, 1);
  ma_li(scratch, Imm32(0x55555555));
  as_bstrins_d(scratch, scratch, 63, 32);
  as_and(tmp, tmp, scratch);
  as_sub_d(output.reg, output.reg, tmp);
  as_srai_d(tmp, output.reg, 2);
  ma_li(scratch, Imm32(0x33333333));
  as_bstrins_d(scratch, scratch, 63, 32);
  as_and(output.reg, output.reg, scratch);
  as_and(tmp, tmp, scratch);
  as_add_d(output.reg, output.reg, tmp);
  as_srli_d(tmp, output.reg, 4);
  as_add_d(output.reg, output.reg, tmp);
  ma_li(scratch, Imm32(0xF0F0F0F));
  as_bstrins_d(scratch, scratch, 63, 32);
  as_and(output.reg, output.reg, scratch);
  ma_li(tmp, Imm32(0x1010101));
  as_bstrins_d(tmp, tmp, 63, 32);
  as_mul_d(output.reg, output.reg, tmp);
  as_srai_d(output.reg, output.reg, 56);
}

void MacroAssembler::clz32(Register src, Register dest, bool knownNotZero) {
  as_clz_w(dest, src);
}

void MacroAssembler::ctz32(Register src, Register dest, bool knownNotZero) {
  as_ctz_w(dest, src);
}

void MacroAssembler::popcnt32(Register input, Register output, Register tmp) {
  // Equivalent to GCC output of mozilla::CountPopulation32()
  as_or(output, input, zero);
  as_srai_w(tmp, input, 1);
  ma_and(tmp, tmp, Imm32(0x55555555));
  as_sub_w(output, output, tmp);
  as_srai_w(tmp, output, 2);
  ma_and(output, output, Imm32(0x33333333));
  ma_and(tmp, tmp, Imm32(0x33333333));
  as_add_w(output, output, tmp);
  as_srli_w(tmp, output, 4);
  as_add_w(output, output, tmp);
  ma_and(output, output, Imm32(0xF0F0F0F));
  as_slli_w(tmp, output, 8);
  as_add_w(output, output, tmp);
  as_slli_w(tmp, output, 16);
  as_add_w(output, output, tmp);
  as_srai_w(output, output, 24);
}

// ===============================================================
// Condition functions

void MacroAssembler::cmp8Set(Condition cond, Address lhs, Imm32 rhs,
                             Register dest) {
  SecondScratchRegisterScope scratch2(*this);
  MOZ_ASSERT(scratch2 != lhs.base);

  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      load8ZeroExtend(lhs, scratch2);
      ma_cmp_set(dest, scratch2, Imm32(uint8_t(rhs.value)), cond);
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      load8SignExtend(lhs, scratch2);
      ma_cmp_set(dest, scratch2, Imm32(int8_t(rhs.value)), cond);
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }
}

void MacroAssembler::cmp16Set(Condition cond, Address lhs, Imm32 rhs,
                              Register dest) {
  SecondScratchRegisterScope scratch2(*this);
  MOZ_ASSERT(scratch2 != lhs.base);

  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      load16ZeroExtend(lhs, scratch2);
      ma_cmp_set(dest, scratch2, Imm32(uint16_t(rhs.value)), cond);
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      load16SignExtend(lhs, scratch2);
      ma_cmp_set(dest, scratch2, Imm32(int16_t(rhs.value)), cond);
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }
}

// Also see below for specializations of cmpPtrSet.
template <typename T1, typename T2>
void MacroAssembler::cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest) {
  ma_cmp_set(dest, lhs, rhs, cond);
}

void MacroAssembler::cmp64Set(Condition cond, Address lhs, Imm64 rhs,
                              Register dest) {
  ma_cmp_set(dest, lhs, ImmWord(uint64_t(rhs.value)), cond);
}

template <typename T1, typename T2>
void MacroAssembler::cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest) {
  ma_cmp_set(dest, lhs, rhs, cond);
}

// ===============================================================
// Branch functions

void MacroAssembler::branch8(Condition cond, const Address& lhs, Imm32 rhs,
                             Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  MOZ_ASSERT(scratch2 != lhs.base);

  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      load8ZeroExtend(lhs, scratch2);
      branch32(cond, scratch2, Imm32(uint8_t(rhs.value)), label);
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      load8SignExtend(lhs, scratch2);
      branch32(cond, scratch2, Imm32(int8_t(rhs.value)), label);
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }
}

void MacroAssembler::branch8(Condition cond, const BaseIndex& lhs, Register rhs,
                             Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  MOZ_ASSERT(scratch2 != lhs.base);

  computeScaledAddress(lhs, scratch2);

  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      load8ZeroExtend(Address(scratch2, lhs.offset), scratch2);
      branch32(cond, scratch2, rhs, label);
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      load8SignExtend(Address(scratch2, lhs.offset), scratch2);
      branch32(cond, scratch2, rhs, label);
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }
}

void MacroAssembler::branch16(Condition cond, const Address& lhs, Imm32 rhs,
                              Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  MOZ_ASSERT(scratch2 != lhs.base);

  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
    case Assembler::Above:
    case Assembler::AboveOrEqual:
    case Assembler::Below:
    case Assembler::BelowOrEqual:
      load16ZeroExtend(lhs, scratch2);
      branch32(cond, scratch2, Imm32(uint16_t(rhs.value)), label);
      break;

    case Assembler::GreaterThan:
    case Assembler::GreaterThanOrEqual:
    case Assembler::LessThan:
    case Assembler::LessThanOrEqual:
      load16SignExtend(lhs, scratch2);
      branch32(cond, scratch2, Imm32(int16_t(rhs.value)), label);
      break;

    default:
      MOZ_CRASH("unexpected condition");
  }
}

template <class L>
void MacroAssembler::branch32(Condition cond, Register lhs, Register rhs,
                              L label) {
  ma_b(lhs, rhs, label, cond);
}

template <class L>
void MacroAssembler::branch32(Condition cond, Register lhs, Imm32 imm,
                              L label) {
  ma_b(lhs, imm, label, cond);
}

void MacroAssembler::branch32(Condition cond, const Address& lhs, Register rhs,
                              Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(lhs, scratch2);
  ma_b(scratch2, rhs, label, cond);
}

void MacroAssembler::branch32(Condition cond, const Address& lhs, Imm32 rhs,
                              Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(lhs, scratch2);
  ma_b(scratch2, rhs, label, cond);
}

void MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs,
                              Register rhs, Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(lhs, scratch2);
  ma_b(scratch2, rhs, label, cond);
}

void MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs,
                              Imm32 rhs, Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(lhs, scratch2);
  ma_b(scratch2, rhs, label, cond);
}

void MacroAssembler::branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs,
                              Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(lhs, scratch2);
  ma_b(scratch2, rhs, label, cond);
}

void MacroAssembler::branch32(Condition cond, wasm::SymbolicAddress addr,
                              Imm32 imm, Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  load32(addr, scratch2);
  ma_b(scratch2, imm, label, cond);
}

void MacroAssembler::branch64(Condition cond, Register64 lhs, Imm64 val,
                              Label* success, Label* fail) {
  MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal ||
                 cond == Assembler::LessThan ||
                 cond == Assembler::LessThanOrEqual ||
                 cond == Assembler::GreaterThan ||
                 cond == Assembler::GreaterThanOrEqual ||
                 cond == Assembler::Below || cond == Assembler::BelowOrEqual ||
                 cond == Assembler::Above || cond == Assembler::AboveOrEqual,
             "other condition codes not supported");

  branchPtr(cond, lhs.reg, ImmWord(val.value), success);
  if (fail) {
    jump(fail);
  }
}

void MacroAssembler::branch64(Condition cond, Register64 lhs, Register64 rhs,
                              Label* success, Label* fail) {
  MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal ||
                 cond == Assembler::LessThan ||
                 cond == Assembler::LessThanOrEqual ||
                 cond == Assembler::GreaterThan ||
                 cond == Assembler::GreaterThanOrEqual ||
                 cond == Assembler::Below || cond == Assembler::BelowOrEqual ||
                 cond == Assembler::Above || cond == Assembler::AboveOrEqual,
             "other condition codes not supported");

  branchPtr(cond, lhs.reg, rhs.reg, success);
  if (fail) {
    jump(fail);
  }
}

void MacroAssembler::branch64(Condition cond, const Address& lhs, Imm64 val,
                              Label* label) {
  MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
             "other condition codes not supported");

  branchPtr(cond, lhs, ImmWord(val.value), label);
}

void MacroAssembler::branch64(Condition cond, const Address& lhs,
                              Register64 rhs, Label* label) {
  MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
             "other condition codes not supported");

  branchPtr(cond, lhs, rhs.reg, label);
}

void MacroAssembler::branch64(Condition cond, const Address& lhs,
                              const Address& rhs, Register scratch,
                              Label* label) {
  MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
             "other condition codes not supported");
  MOZ_ASSERT(lhs.base != scratch);
  MOZ_ASSERT(rhs.base != scratch);

  loadPtr(rhs, scratch);
  branchPtr(cond, lhs, scratch, label);
}

template <class L>
void MacroAssembler::branchPtr(Condition cond, Register lhs, Register rhs,
                               L label) {
  ma_b(lhs, rhs, label, cond);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, Imm32 rhs,
                               Label* label) {
  ma_b(lhs, rhs, label, cond);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmPtr rhs,
                               Label* label) {
  ma_b(lhs, rhs, label, cond);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmGCPtr rhs,
                               Label* label) {
  ma_b(lhs, rhs, label, cond);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmWord rhs,
                               Label* label) {
  ma_b(lhs, rhs, label, cond);
}

template <class L>
void MacroAssembler::branchPtr(Condition cond, const Address& lhs, Register rhs,
                               L label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmPtr rhs,
                               Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmGCPtr rhs,
                               Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmWord rhs,
                               Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs,
                               Register rhs, Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs,
                               ImmWord rhs, Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, wasm::SymbolicAddress lhs,
                               Register rhs, Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs,
                               Register rhs, Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs,
                               ImmWord rhs, Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchPtr(cond, scratch2, rhs, label);
}

void MacroAssembler::branchPrivatePtr(Condition cond, const Address& lhs,
                                      Register rhs, Label* label) {
  branchPtr(cond, lhs, rhs, label);
}

void MacroAssembler::branchFloat(DoubleCondition cond, FloatRegister lhs,
                                 FloatRegister rhs, Label* label) {
  ma_bc_s(lhs, rhs, label, cond);
}

void MacroAssembler::branchTruncateFloat32MaybeModUint32(FloatRegister src,
                                                         Register dest,
                                                         Label* fail) {
  ScratchRegisterScope scratch(asMasm());
  ScratchDoubleScope fpscratch(asMasm());
  as_ftintrz_l_s(fpscratch, src);
  as_movfcsr2gr(scratch);
  moveFromDouble(fpscratch, dest);
  MOZ_ASSERT(Assembler::CauseV < 32);
  as_bstrpick_w(scratch, scratch, Assembler::CauseV, Assembler::CauseV);
  ma_b(scratch, Imm32(0), fail, Assembler::NotEqual);

  as_slli_w(dest, dest, 0);
}

void MacroAssembler::branchTruncateFloat32ToInt32(FloatRegister src,
                                                  Register dest, Label* fail) {
  convertFloat32ToInt32(src, dest, fail, false);
}

void MacroAssembler::branchDouble(DoubleCondition cond, FloatRegister lhs,
                                  FloatRegister rhs, Label* label) {
  ma_bc_d(lhs, rhs, label, cond);
}

void MacroAssembler::branchTruncateDoubleMaybeModUint32(FloatRegister src,
                                                        Register dest,
                                                        Label* fail) {
  ScratchRegisterScope scratch(asMasm());
  ScratchDoubleScope fpscratch(asMasm());
  as_ftintrz_l_d(fpscratch, src);
  as_movfcsr2gr(scratch);
  moveFromDouble(fpscratch, dest);
  MOZ_ASSERT(Assembler::CauseV < 32);
  as_bstrpick_w(scratch, scratch, Assembler::CauseV, Assembler::CauseV);
  ma_b(scratch, Imm32(0), fail, Assembler::NotEqual);

  as_slli_w(dest, dest, 0);
}

void MacroAssembler::branchTruncateDoubleToInt32(FloatRegister src,
                                                 Register dest, Label* fail) {
  ScratchRegisterScope scratch(asMasm());
  ScratchDoubleScope fpscratch(asMasm());

  // Convert scalar to signed 64-bit fixed-point, rounding toward zero.
  // In the case of overflow, the output is saturated.
  // In the case of NaN and -0, the output is zero.
  as_ftintrz_l_d(fpscratch, src);
  moveFromDouble(fpscratch, dest);

  // Fail on overflow cases.
  as_slli_w(scratch, dest, 0);
  ma_b(dest, scratch, fail, Assembler::NotEqual);
}

template <typename T>
void MacroAssembler::branchAdd32(Condition cond, T src, Register dest,
                                 Label* overflow) {
  switch (cond) {
    case Overflow:
      ma_add32TestOverflow(dest, dest, src, overflow);
      break;
    case CarryClear:
    case CarrySet:
      ma_add32TestCarry(cond, dest, dest, src, overflow);
      break;
    default:
      MOZ_CRASH("NYI");
  }
}

// the type of 'T src' maybe a Register, maybe a Imm32,depends on who call it.
template <typename T>
void MacroAssembler::branchSub32(Condition cond, T src, Register dest,
                                 Label* overflow) {
  switch (cond) {
    case Overflow:
      ma_sub32TestOverflow(dest, dest, src, overflow);
      break;
    case NonZero:
    case Zero:
    case Signed:
    case NotSigned:
      ma_sub_w(dest, dest, src);
      ma_b(dest, dest, overflow, cond);
      break;
    default:
      MOZ_CRASH("NYI");
  }
}

template <typename T>
void MacroAssembler::branchMul32(Condition cond, T src, Register dest,
                                 Label* overflow) {
  MOZ_ASSERT(cond == Assembler::Overflow);
  ma_mul32TestOverflow(dest, dest, src, overflow);
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
  branch32(Assembler::Equal, reg, Imm32(INT32_MIN), label);
}

template <typename T>
void MacroAssembler::branchAddPtr(Condition cond, T src, Register dest,
                                  Label* label) {
  switch (cond) {
    case Overflow:
      ma_addPtrTestOverflow(dest, dest, src, label);
      break;
    case CarryClear:
    case CarrySet:
      ma_addPtrTestCarry(cond, dest, dest, src, label);
      break;
    default:
      MOZ_CRASH("NYI");
  }
}

template <typename T>
void MacroAssembler::branchSubPtr(Condition cond, T src, Register dest,
                                  Label* label) {
  switch (cond) {
    case Overflow:
      ma_subPtrTestOverflow(dest, dest, src, label);
      break;
    case NonZero:
    case Zero:
    case Signed:
    case NotSigned:
      subPtr(src, dest);
      ma_b(dest, dest, label, cond);
      break;
    default:
      MOZ_CRASH("NYI");
  }
}

void MacroAssembler::branchMulPtr(Condition cond, Register src, Register dest,
                                  Label* label) {
  MOZ_ASSERT(cond == Assembler::Overflow);
  ma_mulPtrTestOverflow(dest, dest, src, label);
}

void MacroAssembler::decBranchPtr(Condition cond, Register lhs, Imm32 rhs,
                                  Label* label) {
  subPtr(rhs, lhs);
  branchPtr(cond, lhs, Imm32(0), label);
}

template <class L>
void MacroAssembler::branchTest32(Condition cond, Register lhs, Register rhs,
                                  L label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  if (lhs == rhs) {
    ma_b(lhs, rhs, label, cond);
  } else {
    ScratchRegisterScope scratch(asMasm());
    as_and(scratch, lhs, rhs);
    ma_b(scratch, scratch, label, cond);
  }
}

template <class L>
void MacroAssembler::branchTest32(Condition cond, Register lhs, Imm32 rhs,
                                  L label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  SecondScratchRegisterScope scratch2(asMasm());
  ma_and(scratch2, lhs, rhs);
  ma_b(scratch2, scratch2, label, cond);
}

void MacroAssembler::branchTest32(Condition cond, const Address& lhs, Imm32 rhs,
                                  Label* label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  SecondScratchRegisterScope scratch2(asMasm());
  load32(lhs, scratch2);
  and32(rhs, scratch2);
  ma_b(scratch2, scratch2, label, cond);
}

void MacroAssembler::branchTest32(Condition cond, const AbsoluteAddress& lhs,
                                  Imm32 rhs, Label* label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  SecondScratchRegisterScope scratch2(asMasm());
  load32(lhs, scratch2);
  and32(rhs, scratch2);
  ma_b(scratch2, scratch2, label, cond);
}

template <class L>
void MacroAssembler::branchTestPtr(Condition cond, Register lhs, Register rhs,
                                   L label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  if (lhs == rhs) {
    ma_b(lhs, rhs, label, cond);
  } else {
    ScratchRegisterScope scratch(asMasm());
    as_and(scratch, lhs, rhs);
    ma_b(scratch, scratch, label, cond);
  }
}

void MacroAssembler::branchTestPtr(Condition cond, Register lhs, Imm32 rhs,
                                   Label* label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  ScratchRegisterScope scratch(asMasm());
  ma_and(scratch, lhs, rhs);
  ma_b(scratch, scratch, label, cond);
}

void MacroAssembler::branchTestPtr(Condition cond, const Address& lhs,
                                   Imm32 rhs, Label* label) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  branchTestPtr(cond, scratch2, rhs, label);
}

template <class L>
void MacroAssembler::branchTest64(Condition cond, Register64 lhs,
                                  Register64 rhs, Register temp, L label) {
  branchTestPtr(cond, lhs.reg, rhs.reg, label);
}

void MacroAssembler::branchTestUndefined(Condition cond, Register tag,
                                         Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JSVAL_TAG_UNDEFINED), label, cond);
}

void MacroAssembler::branchTestUndefined(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestUndefined(cond, scratch2, label);
}

void MacroAssembler::branchTestUndefined(Condition cond, const Address& address,
                                         Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestUndefined(cond, tag, label);
}

void MacroAssembler::branchTestUndefined(Condition cond,
                                         const BaseIndex& address,
                                         Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestUndefined(cond, tag, label);
}

void MacroAssembler::branchTestInt32(Condition cond, Register tag,
                                     Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JSVAL_TAG_INT32), label, cond);
}

void MacroAssembler::branchTestInt32(Condition cond, const ValueOperand& value,
                                     Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestInt32(cond, scratch2, label);
}

void MacroAssembler::branchTestInt32(Condition cond, const Address& address,
                                     Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestInt32(cond, tag, label);
}

void MacroAssembler::branchTestInt32(Condition cond, const BaseIndex& address,
                                     Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestInt32(cond, tag, label);
}

void MacroAssembler::branchTestInt32Truthy(bool b, const ValueOperand& value,
                                           Label* label) {
  ScratchRegisterScope scratch(*this);
  as_bstrpick_d(scratch, value.valueReg(), 31, 0);
  ma_b(scratch, scratch, label, b ? NonZero : Zero);
}

void MacroAssembler::branchTestDouble(Condition cond, Register tag,
                                      Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  Condition actual = (cond == Equal) ? BelowOrEqual : Above;
  ma_b(tag, ImmTag(JSVAL_TAG_MAX_DOUBLE), label, actual);
}

void MacroAssembler::branchTestDouble(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestDouble(cond, scratch2, label);
}

void MacroAssembler::branchTestDouble(Condition cond, const Address& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestDouble(cond, tag, label);
}

void MacroAssembler::branchTestDouble(Condition cond, const BaseIndex& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestDouble(cond, tag, label);
}

void MacroAssembler::branchTestDoubleTruthy(bool b, FloatRegister value,
                                            Label* label) {
  ScratchDoubleScope fpscratch(*this);
  ma_lid(fpscratch, 0.0);
  DoubleCondition cond = b ? DoubleNotEqual : DoubleEqualOrUnordered;
  ma_bc_d(value, fpscratch, label, cond);
}

void MacroAssembler::branchTestNumber(Condition cond, Register tag,
                                      Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  Condition actual = cond == Equal ? BelowOrEqual : Above;
  ma_b(tag, ImmTag(JS::detail::ValueUpperInclNumberTag), label, actual);
}

void MacroAssembler::branchTestNumber(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestNumber(cond, scratch2, label);
}

void MacroAssembler::branchTestBoolean(Condition cond, Register tag,
                                       Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JSVAL_TAG_BOOLEAN), label, cond);
}

void MacroAssembler::branchTestBoolean(Condition cond,
                                       const ValueOperand& value,
                                       Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestBoolean(cond, scratch2, label);
}

void MacroAssembler::branchTestBoolean(Condition cond, const Address& address,
                                       Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestBoolean(cond, tag, label);
}

void MacroAssembler::branchTestBoolean(Condition cond, const BaseIndex& address,
                                       Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestBoolean(cond, tag, label);
}

void MacroAssembler::branchTestBooleanTruthy(bool b, const ValueOperand& value,
                                             Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  unboxBoolean(value, scratch2);
  ma_b(scratch2, scratch2, label, b ? NonZero : Zero);
}

void MacroAssembler::branchTestString(Condition cond, Register tag,
                                      Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JSVAL_TAG_STRING), label, cond);
}

void MacroAssembler::branchTestString(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestString(cond, scratch2, label);
}

void MacroAssembler::branchTestString(Condition cond, const Address& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestString(cond, tag, label);
}

void MacroAssembler::branchTestString(Condition cond, const BaseIndex& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestString(cond, tag, label);
}

void MacroAssembler::branchTestStringTruthy(bool b, const ValueOperand& value,
                                            Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  unboxString(value, scratch2);
  load32(Address(scratch2, JSString::offsetOfLength()), scratch2);
  ma_b(scratch2, Imm32(0), label, b ? NotEqual : Equal);
}

void MacroAssembler::branchTestSymbol(Condition cond, Register tag,
                                      Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JSVAL_TAG_SYMBOL), label, cond);
}

void MacroAssembler::branchTestSymbol(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestSymbol(cond, scratch2, label);
}

void MacroAssembler::branchTestSymbol(Condition cond, const BaseIndex& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestSymbol(cond, tag, label);
}

void MacroAssembler::branchTestSymbol(Condition cond, const Address& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestSymbol(cond, tag, label);
}

void MacroAssembler::branchTestBigInt(Condition cond, Register tag,
                                      Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JSVAL_TAG_BIGINT), label, cond);
}

void MacroAssembler::branchTestBigInt(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestBigInt(cond, scratch2, label);
}

void MacroAssembler::branchTestBigInt(Condition cond, const Address& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestBigInt(cond, tag, label);
}

void MacroAssembler::branchTestBigInt(Condition cond, const BaseIndex& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  computeEffectiveAddress(address, scratch2);
  splitTag(scratch2, scratch2);
  branchTestBigInt(cond, scratch2, label);
}

void MacroAssembler::branchTestBigIntTruthy(bool b, const ValueOperand& value,
                                            Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  unboxBigInt(value, scratch2);
  load32(Address(scratch2, BigInt::offsetOfDigitLength()), scratch2);
  ma_b(scratch2, Imm32(0), label, b ? NotEqual : Equal);
}

void MacroAssembler::branchTestNull(Condition cond, Register tag,
                                    Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JSVAL_TAG_NULL), label, cond);
}

void MacroAssembler::branchTestNull(Condition cond, const ValueOperand& value,
                                    Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestNull(cond, scratch2, label);
}

void MacroAssembler::branchTestNull(Condition cond, const Address& address,
                                    Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestNull(cond, tag, label);
}

void MacroAssembler::branchTestNull(Condition cond, const BaseIndex& address,
                                    Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestNull(cond, tag, label);
}

void MacroAssembler::branchTestObject(Condition cond, Register tag,
                                      Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JSVAL_TAG_OBJECT), label, cond);
}

void MacroAssembler::branchTestObject(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestObject(cond, scratch2, label);
}

void MacroAssembler::branchTestObject(Condition cond, const Address& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestObject(cond, tag, label);
}

void MacroAssembler::branchTestObject(Condition cond, const BaseIndex& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestObject(cond, tag, label);
}

void MacroAssembler::branchTestPrimitive(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestPrimitive(cond, scratch2, label);
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
                                       const ValueOperand& address,
                                       Label* label) {
  branchTestGCThingImpl(cond, address, label);
}

template <typename T>
void MacroAssembler::branchTestGCThingImpl(Condition cond, const T& address,
                                           Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  ma_b(tag, ImmTag(JS::detail::ValueLowerInclGCThingTag), label,
       (cond == Equal) ? AboveOrEqual : Below);
}

void MacroAssembler::branchTestPrimitive(Condition cond, Register tag,
                                         Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JS::detail::ValueUpperExclPrimitiveTag), label,
       (cond == Equal) ? Below : AboveOrEqual);
}

void MacroAssembler::branchTestMagic(Condition cond, Register tag,
                                     Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(tag, ImmTag(JSVAL_TAG_MAGIC), label, cond);
}

void MacroAssembler::branchTestMagic(Condition cond, const Address& address,
                                     Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestMagic(cond, tag, label);
}

void MacroAssembler::branchTestMagic(Condition cond, const BaseIndex& address,
                                     Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestMagic(cond, tag, label);
}

template <class L>
void MacroAssembler::branchTestMagic(Condition cond, const ValueOperand& value,
                                     L label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  ma_b(scratch2, ImmTag(JSVAL_TAG_MAGIC), label, cond);
}

void MacroAssembler::branchTestMagic(Condition cond, const Address& valaddr,
                                     JSWhyMagic why, Label* label) {
  uint64_t magic = MagicValue(why).asRawBits();
  SecondScratchRegisterScope scratch(*this);
  loadPtr(valaddr, scratch);
  ma_b(scratch, ImmWord(magic), label, cond);
}

void MacroAssembler::branchTestValue(Condition cond, const BaseIndex& lhs,
                                     const ValueOperand& rhs, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  branchPtr(cond, lhs, rhs.valueReg(), label);
}

template <typename T>
void MacroAssembler::testNumberSet(Condition cond, const T& src,
                                   Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(src, scratch2);
  ma_cmp_set(dest, tag, ImmTag(JS::detail::ValueUpperInclNumberTag),
             cond == Equal ? BelowOrEqual : Above);
}

template <typename T>
void MacroAssembler::testBooleanSet(Condition cond, const T& src,
                                    Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(src, scratch2);
  ma_cmp_set(dest, tag, ImmTag(JSVAL_TAG_BOOLEAN), cond);
}

template <typename T>
void MacroAssembler::testStringSet(Condition cond, const T& src,
                                   Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(src, scratch2);
  ma_cmp_set(dest, tag, ImmTag(JSVAL_TAG_STRING), cond);
}

template <typename T>
void MacroAssembler::testSymbolSet(Condition cond, const T& src,
                                   Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(src, scratch2);
  ma_cmp_set(dest, tag, ImmTag(JSVAL_TAG_SYMBOL), cond);
}

template <typename T>
void MacroAssembler::testBigIntSet(Condition cond, const T& src,
                                   Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(src, scratch2);
  ma_cmp_set(dest, tag, ImmTag(JSVAL_TAG_BIGINT), cond);
}

void MacroAssembler::branchToComputedAddress(const BaseIndex& addr) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(addr, scratch2);
  branch(scratch2);
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs, Imm32 rhs,
                                 Register src, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  cmp32Set(cond, lhs, rhs, scratch2);
  moveIfNotZero(dest, src, scratch2);
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs, Register rhs,
                                 Register src, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  cmp32Set(cond, lhs, rhs, scratch2);
  moveIfNotZero(dest, src, scratch2);
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs,
                                 const Address& rhs, Register src,
                                 Register dest) {
  SecondScratchRegisterScope scratch2(*this);
  MOZ_ASSERT(lhs != scratch2 && src != scratch2 && dest != scratch2);
  load32(rhs, scratch2);
  cmp32Move32(cond, lhs, scratch2, src, dest);
}

void MacroAssembler::cmp32MovePtr(Condition cond, Register lhs, Imm32 rhs,
                                  Register src, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  cmp32Set(cond, lhs, rhs, scratch2);
  moveIfNotZero(dest, src, scratch2);
}

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs, Register rhs,
                                   Register src, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  cmpPtrSet(cond, lhs, rhs, scratch2);
  moveIfNotZero(dest, src, scratch2);
}

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs,
                                   const Address& rhs, Register src,
                                   Register dest) {
  MOZ_CRASH("NYI");
}

void MacroAssembler::cmp32Load32(Condition cond, Register lhs,
                                 const Address& rhs, const Address& src,
                                 Register dest) {
  ScratchRegisterScope scratch(*this);
  MOZ_ASSERT(lhs != scratch && dest != scratch);
  load32(rhs, scratch);
  cmp32Load32(cond, lhs, scratch, src, dest);
}

void MacroAssembler::cmp32Load32(Condition cond, Register lhs, Register rhs,
                                 const Address& src, Register dest) {
  Label skip;
  branch32(Assembler::InvertCondition(cond), lhs, rhs, &skip);
  load32(src, dest);
  bind(&skip);
}

void MacroAssembler::cmp32Load32(Condition cond, Register lhs, Imm32 rhs,
                                 const Address& src, Register dest) {
  Label skip;
  branch32(Assembler::InvertCondition(cond), lhs, rhs, &skip);
  load32(src, dest);
  bind(&skip);
}

void MacroAssembler::cmp32LoadPtr(Condition cond, const Address& lhs, Imm32 rhs,
                                  const Address& src, Register dest) {
  Label skip;
  branch32(Assembler::InvertCondition(cond), lhs, rhs, &skip);
  loadPtr(src, dest);
  bind(&skip);
}

void MacroAssembler::test32LoadPtr(Condition cond, const Address& addr,
                                   Imm32 mask, const Address& src,
                                   Register dest) {
  MOZ_RELEASE_ASSERT(!JitOptions.spectreStringMitigations);
  Label skip;
  branchTest32(Assembler::InvertCondition(cond), addr, mask, &skip);
  loadPtr(src, dest);
  bind(&skip);
}

void MacroAssembler::test32MovePtr(Condition cond, const Address& addr,
                                   Imm32 mask, Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::spectreMovePtr(Condition cond, Register src,
                                    Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::spectreZeroRegister(Condition cond, Register scratch,
                                         Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::spectreBoundsCheck32(Register index, Register length,
                                          Register maybeScratch,
                                          Label* failure) {
  MOZ_RELEASE_ASSERT(!JitOptions.spectreIndexMasking);
  branch32(Assembler::BelowOrEqual, length, index, failure);
}

void MacroAssembler::spectreBoundsCheck32(Register index, const Address& length,
                                          Register maybeScratch,
                                          Label* failure) {
  MOZ_RELEASE_ASSERT(!JitOptions.spectreIndexMasking);
  branch32(Assembler::BelowOrEqual, length, index, failure);
}

void MacroAssembler::spectreBoundsCheckPtr(Register index, Register length,
                                           Register maybeScratch,
                                           Label* failure) {
  MOZ_RELEASE_ASSERT(!JitOptions.spectreIndexMasking);
  branchPtr(Assembler::BelowOrEqual, length, index, failure);
}

void MacroAssembler::spectreBoundsCheckPtr(Register index,
                                           const Address& length,
                                           Register maybeScratch,
                                           Label* failure) {
  MOZ_RELEASE_ASSERT(!JitOptions.spectreIndexMasking);
  branchPtr(Assembler::BelowOrEqual, length, index, failure);
}

// ========================================================================
// Memory access primitives.

FaultingCodeOffset MacroAssembler::storeUncanonicalizedFloat32(
    FloatRegister src, const Address& addr) {
  return ma_fst_s(src, addr);
}
FaultingCodeOffset MacroAssembler::storeUncanonicalizedFloat32(
    FloatRegister src, const BaseIndex& addr) {
  return ma_fst_s(src, addr);
}

FaultingCodeOffset MacroAssembler::storeUncanonicalizedDouble(
    FloatRegister src, const Address& addr) {
  return ma_fst_d(src, addr);
}
FaultingCodeOffset MacroAssembler::storeUncanonicalizedDouble(
    FloatRegister src, const BaseIndex& addr) {
  return ma_fst_d(src, addr);
}

void MacroAssembler::memoryBarrier(MemoryBarrierBits barrier) {
  if (barrier) {
    as_dbar(0);
  }
}

// ===============================================================
// Clamping functions.

void MacroAssembler::clampIntToUint8(Register reg) {
  ScratchRegisterScope scratch(*this);
  // If reg is < 0, then we want to clamp to 0.
  as_slti(scratch, reg, 0);
  as_masknez(reg, reg, scratch);

  // If reg is >= 255, then we want to clamp to 255.
  as_addi_d(reg, reg, -255);
  as_slt(scratch, reg, zero);
  as_maskeqz(reg, reg, scratch);
  as_addi_d(reg, reg, 255);
}

void MacroAssembler::fallibleUnboxPtr(const ValueOperand& src, Register dest,
                                      JSValueType type, Label* fail) {
  MOZ_ASSERT(type == JSVAL_TYPE_OBJECT || type == JSVAL_TYPE_STRING ||
             type == JSVAL_TYPE_SYMBOL || type == JSVAL_TYPE_BIGINT);
  // dest := src XOR mask
  // scratch := dest >> JSVAL_TAG_SHIFT
  // fail if scratch != 0
  //
  // Note: src and dest can be the same register
  ScratchRegisterScope scratch(asMasm());
  MOZ_ASSERT(src.valueReg() != scratch);
  mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), scratch);
  as_xor(dest, src.valueReg(), scratch);
  as_srli_d(scratch, dest, JSVAL_TAG_SHIFT);
  ma_b(scratch, Imm32(0), fail, Assembler::NotEqual);
}

void MacroAssembler::fallibleUnboxPtr(const Address& src, Register dest,
                                      JSValueType type, Label* fail) {
  loadValue(src, ValueOperand(dest));
  fallibleUnboxPtr(ValueOperand(dest), dest, type, fail);
}

void MacroAssembler::fallibleUnboxPtr(const BaseIndex& src, Register dest,
                                      JSValueType type, Label* fail) {
  loadValue(src, ValueOperand(dest));
  fallibleUnboxPtr(ValueOperand(dest), dest, type, fail);
}

//}}} check_macroassembler_style
// ===============================================================

// The specializations for cmpPtrSet are outside the braces because
// check_macroassembler_style can't yet deal with specializations.

template <>
inline void MacroAssembler::cmpPtrSet(Assembler::Condition cond, Address lhs,
                                      ImmPtr rhs, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  loadPtr(lhs, scratch2);
  cmpPtrSet(cond, Register(scratch2), rhs, dest);
}

template <>
inline void MacroAssembler::cmpPtrSet(Assembler::Condition cond, Register lhs,
                                      Address rhs, Register dest) {
  ScratchRegisterScope scratch(asMasm());
  MOZ_ASSERT(lhs != scratch);
  loadPtr(rhs, scratch);
  cmpPtrSet(cond, lhs, Register(scratch), dest);
}

template <>
inline void MacroAssembler::cmpPtrSet(Assembler::Condition cond, Address lhs,
                                      Register rhs, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  MOZ_ASSERT(rhs != scratch2);
  loadPtr(lhs, scratch2);
  cmpPtrSet(cond, Register(scratch2), rhs, dest);
}

template <>
inline void MacroAssembler::cmp32Set(Assembler::Condition cond, Register lhs,
                                     Address rhs, Register dest) {
  ScratchRegisterScope scratch(asMasm());
  MOZ_ASSERT(lhs != scratch);
  load32(rhs, scratch);
  cmp32Set(cond, lhs, Register(scratch), dest);
}

template <>
inline void MacroAssembler::cmp32Set(Assembler::Condition cond, Address lhs,
                                     Register rhs, Register dest) {
  SecondScratchRegisterScope scratch2(asMasm());
  MOZ_ASSERT(rhs != scratch2);
  load32(lhs, scratch2);
  cmp32Set(cond, Register(scratch2), rhs, dest);
}

void MacroAssemblerLOONG64Compat::incrementInt32Value(const Address& addr) {
  asMasm().add32(Imm32(1), addr);
}

void MacroAssemblerLOONG64Compat::retn(Imm32 n) {
  // pc <- [sp]; sp += n
  loadPtr(Address(StackPointer, 0), ra);
  asMasm().addPtr(n, StackPointer);
  as_jirl(zero, ra, BOffImm16(0));
}

// If source is a double, load into dest.
// If source is int32, convert to double and store in dest.
// Else, branch to failure.
void MacroAssemblerLOONG64Compat::ensureDouble(const ValueOperand& source,
                                               FloatRegister dest,
                                               Label* failure) {
  Label isDouble, done;
  {
    ScratchTagScope tag(asMasm(), source);
    splitTagForTest(source, tag);
    asMasm().branchTestDouble(Assembler::Equal, tag, &isDouble);
    asMasm().branchTestInt32(Assembler::NotEqual, tag, failure);
  }

  convertInt32ToDouble(source.valueReg(), dest);
  jump(&done);

  bind(&isDouble);
  unboxDouble(source, dest);

  bind(&done);
}

}  // namespace jit
}  // namespace js

#endif /* jit_loong64_MacroAssembler_loong64_inl_h */
