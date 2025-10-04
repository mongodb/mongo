/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_MacroAssembler_mips64_inl_h
#define jit_mips64_MacroAssembler_mips64_inl_h

#include "jit/mips64/MacroAssembler-mips64.h"

#include "vm/BigIntType.h"  // JS::BigInt

#include "jit/mips-shared/MacroAssembler-mips-shared-inl.h"

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
  ma_sll(dest, src.reg, Imm32(0));
}

void MacroAssembler::move32To64ZeroExtend(Register src, Register64 dest) {
  ma_dext(dest.reg, src, Imm32(0), Imm32(32));
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
  ma_sll(dest.reg, src, Imm32(0));
}

void MacroAssembler::move32SignExtendToPtr(Register src, Register dest) {
  ma_sll(dest, src, Imm32(0));
}

void MacroAssembler::move32ZeroExtendToPtr(Register src, Register dest) {
  ma_dext(dest, src, Imm32(0), Imm32(32));
}

// ===============================================================
// Load instructions

void MacroAssembler::load32SignExtendToPtr(const Address& src, Register dest) {
  load32(src, dest);
}

// ===============================================================
// Logical instructions

void MacroAssembler::notPtr(Register reg) { ma_not(reg, reg); }

void MacroAssembler::andPtr(Register src, Register dest) { ma_and(dest, src); }

void MacroAssembler::andPtr(Imm32 imm, Register dest) { ma_and(dest, imm); }

void MacroAssembler::and64(Imm64 imm, Register64 dest) {
  ma_li(ScratchRegister, ImmWord(imm.value));
  ma_and(dest.reg, ScratchRegister);
}

void MacroAssembler::and64(Register64 src, Register64 dest) {
  ma_and(dest.reg, src.reg);
}

void MacroAssembler::and64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    Register64 scratch(ScratchRegister);

    load64(src.toAddress(), scratch);
    and64(scratch, dest);
  } else {
    and64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::or64(Imm64 imm, Register64 dest) {
  ma_li(ScratchRegister, ImmWord(imm.value));
  ma_or(dest.reg, ScratchRegister);
}

void MacroAssembler::xor64(Imm64 imm, Register64 dest) {
  ma_li(ScratchRegister, ImmWord(imm.value));
  ma_xor(dest.reg, ScratchRegister);
}

void MacroAssembler::orPtr(Register src, Register dest) { ma_or(dest, src); }

void MacroAssembler::orPtr(Imm32 imm, Register dest) { ma_or(dest, imm); }

void MacroAssembler::or64(Register64 src, Register64 dest) {
  ma_or(dest.reg, src.reg);
}

void MacroAssembler::or64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    Register64 scratch(ScratchRegister);

    load64(src.toAddress(), scratch);
    or64(scratch, dest);
  } else {
    or64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::xor64(Register64 src, Register64 dest) {
  ma_xor(dest.reg, src.reg);
}

void MacroAssembler::xor64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    Register64 scratch(ScratchRegister);

    load64(src.toAddress(), scratch);
    xor64(scratch, dest);
  } else {
    xor64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::xorPtr(Register src, Register dest) { ma_xor(dest, src); }

void MacroAssembler::xorPtr(Imm32 imm, Register dest) { ma_xor(dest, imm); }

// ===============================================================
// Swap instructions

void MacroAssembler::byteSwap64(Register64 reg64) {
  Register reg = reg64.reg;
  ma_dsbh(reg, reg);
  ma_dshd(reg, reg);
}

// ===============================================================
// Arithmetic functions

void MacroAssembler::addPtr(Register src, Register dest) {
  ma_daddu(dest, src);
}

void MacroAssembler::addPtr(Imm32 imm, Register dest) { ma_daddu(dest, imm); }

void MacroAssembler::addPtr(ImmWord imm, Register dest) {
  movePtr(imm, ScratchRegister);
  addPtr(ScratchRegister, dest);
}

void MacroAssembler::add64(Register64 src, Register64 dest) {
  addPtr(src.reg, dest.reg);
}

void MacroAssembler::add64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    Register64 scratch(ScratchRegister);

    load64(src.toAddress(), scratch);
    add64(scratch, dest);
  } else {
    add64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::add64(Imm32 imm, Register64 dest) {
  ma_daddu(dest.reg, imm);
}

void MacroAssembler::add64(Imm64 imm, Register64 dest) {
  MOZ_ASSERT(dest.reg != ScratchRegister);
  mov(ImmWord(imm.value), ScratchRegister);
  ma_daddu(dest.reg, ScratchRegister);
}

CodeOffset MacroAssembler::sub32FromStackPtrWithPatch(Register dest) {
  CodeOffset offset = CodeOffset(currentOffset());
  MacroAssemblerMIPSShared::ma_liPatchable(dest, Imm32(0));
  as_dsubu(dest, StackPointer, dest);
  return offset;
}

void MacroAssembler::patchSub32FromStackPtr(CodeOffset offset, Imm32 imm) {
  Instruction* lui =
      (Instruction*)m_buffer.getInst(BufferOffset(offset.offset()));
  MOZ_ASSERT(lui->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
  MOZ_ASSERT(lui->next()->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

  MacroAssemblerMIPSShared::UpdateLuiOriValue(lui, lui->next(), imm.value);
}

void MacroAssembler::subPtr(Register src, Register dest) {
  as_dsubu(dest, dest, src);
}

void MacroAssembler::subPtr(Imm32 imm, Register dest) {
  ma_dsubu(dest, dest, imm);
}

void MacroAssembler::sub64(Register64 src, Register64 dest) {
  as_dsubu(dest.reg, dest.reg, src.reg);
}

void MacroAssembler::sub64(const Operand& src, Register64 dest) {
  if (src.getTag() == Operand::MEM) {
    Register64 scratch(ScratchRegister);

    load64(src.toAddress(), scratch);
    sub64(scratch, dest);
  } else {
    sub64(Register64(src.toReg()), dest);
  }
}

void MacroAssembler::sub64(Imm64 imm, Register64 dest) {
  MOZ_ASSERT(dest.reg != ScratchRegister);
  mov(ImmWord(imm.value), ScratchRegister);
  as_dsubu(dest.reg, dest.reg, ScratchRegister);
}

void MacroAssembler::mulHighUnsigned32(Imm32 imm, Register src, Register dest) {
  ScratchRegisterScope scratch(*this);
  MOZ_ASSERT(src != scratch);
  move32(imm, scratch);
#ifdef MIPSR6
  as_muhu(dest, src, scratch);
#else
  as_multu(src, scratch);
  as_mfhi(dest);
#endif
}

void MacroAssembler::mulPtr(Register rhs, Register srcDest) {
#ifdef MIPSR6
  as_dmulu(srcDest, srcDest, rhs);
#else
  as_dmultu(srcDest, rhs);
  as_mflo(srcDest);
#endif
}

void MacroAssembler::mul64(Imm64 imm, const Register64& dest) {
  MOZ_ASSERT(dest.reg != ScratchRegister);
  mov(ImmWord(imm.value), ScratchRegister);
#ifdef MIPSR6
  as_dmulu(dest.reg, ScratchRegister, dest.reg);
#else
  as_dmultu(dest.reg, ScratchRegister);
  as_mflo(dest.reg);
#endif
}

void MacroAssembler::mul64(Imm64 imm, const Register64& dest,
                           const Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  mul64(imm, dest);
}

void MacroAssembler::mul64(const Register64& src, const Register64& dest,
                           const Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
#ifdef MIPSR6
  as_dmulu(dest.reg, src.reg, dest.reg);
#else
  as_dmultu(dest.reg, src.reg);
  as_mflo(dest.reg);
#endif
}

void MacroAssembler::mul64(const Operand& src, const Register64& dest,
                           const Register temp) {
  if (src.getTag() == Operand::MEM) {
    Register64 scratch(ScratchRegister);

    load64(src.toAddress(), scratch);
    mul64(scratch, dest, temp);
  } else {
    mul64(Register64(src.toReg()), dest, temp);
  }
}

void MacroAssembler::mulBy3(Register src, Register dest) {
  MOZ_ASSERT(src != ScratchRegister);
  as_daddu(ScratchRegister, src, src);
  as_daddu(dest, ScratchRegister, src);
}

void MacroAssembler::inc64(AbsoluteAddress dest) {
  ma_li(ScratchRegister, ImmWord(uintptr_t(dest.addr)));
  as_ld(SecondScratchReg, ScratchRegister, 0);
  as_daddiu(SecondScratchReg, SecondScratchReg, 1);
  as_sd(SecondScratchReg, ScratchRegister, 0);
}

void MacroAssembler::neg64(Register64 reg) { as_dsubu(reg.reg, zero, reg.reg); }

void MacroAssembler::negPtr(Register reg) { as_dsubu(reg, zero, reg); }

// ===============================================================
// Shift functions

void MacroAssembler::lshiftPtr(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  ma_dsll(dest, dest, imm);
}

void MacroAssembler::lshiftPtr(Register shift, Register dest) {
  ma_dsll(dest, dest, shift);
}

void MacroAssembler::lshift64(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  ma_dsll(dest.reg, dest.reg, imm);
}

void MacroAssembler::lshift64(Register shift, Register64 dest) {
  ma_dsll(dest.reg, dest.reg, shift);
}

void MacroAssembler::rshiftPtr(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  ma_dsrl(dest, dest, imm);
}

void MacroAssembler::rshiftPtr(Register shift, Register dest) {
  ma_dsrl(dest, dest, shift);
}

void MacroAssembler::rshift64(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  ma_dsrl(dest.reg, dest.reg, imm);
}

void MacroAssembler::rshift64(Register shift, Register64 dest) {
  ma_dsrl(dest.reg, dest.reg, shift);
}

void MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  ma_dsra(dest, dest, imm);
}

void MacroAssembler::rshift64Arithmetic(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  ma_dsra(dest.reg, dest.reg, imm);
}

void MacroAssembler::rshift64Arithmetic(Register shift, Register64 dest) {
  ma_dsra(dest.reg, dest.reg, shift);
}

// ===============================================================
// Rotation functions

void MacroAssembler::rotateLeft64(Imm32 count, Register64 src, Register64 dest,
                                  Register temp) {
  MOZ_ASSERT(temp == InvalidReg);

  if (count.value) {
    ma_drol(dest.reg, src.reg, count);
  } else {
    ma_move(dest.reg, src.reg);
  }
}

void MacroAssembler::rotateLeft64(Register count, Register64 src,
                                  Register64 dest, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  ma_drol(dest.reg, src.reg, count);
}

void MacroAssembler::rotateRight64(Imm32 count, Register64 src, Register64 dest,
                                   Register temp) {
  MOZ_ASSERT(temp == InvalidReg);

  if (count.value) {
    ma_dror(dest.reg, src.reg, count);
  } else {
    ma_move(dest.reg, src.reg);
  }
}

void MacroAssembler::rotateRight64(Register count, Register64 src,
                                   Register64 dest, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  ma_dror(dest.reg, src.reg, count);
}

// ===============================================================
// Condition functions

template <typename T1, typename T2>
void MacroAssembler::cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest) {
  ma_cmp_set(dest, lhs, rhs, cond);
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

// ===============================================================
// Bit counting functions

void MacroAssembler::clz64(Register64 src, Register dest) {
  as_dclz(dest, src.reg);
}

void MacroAssembler::ctz64(Register64 src, Register dest) {
  ma_dctz(dest, src.reg);
}

void MacroAssembler::popcnt64(Register64 input, Register64 output,
                              Register tmp) {
  ma_move(output.reg, input.reg);
  ma_dsra(tmp, input.reg, Imm32(1));
  ma_li(ScratchRegister, ImmWord(0x5555555555555555UL));
  ma_and(tmp, ScratchRegister);
  ma_dsubu(output.reg, tmp);
  ma_dsra(tmp, output.reg, Imm32(2));
  ma_li(ScratchRegister, ImmWord(0x3333333333333333UL));
  ma_and(output.reg, ScratchRegister);
  ma_and(tmp, ScratchRegister);
  ma_daddu(output.reg, tmp);
  ma_dsrl(tmp, output.reg, Imm32(4));
  ma_daddu(output.reg, tmp);
  ma_li(ScratchRegister, ImmWord(0xF0F0F0F0F0F0F0FUL));
  ma_and(output.reg, ScratchRegister);
  ma_dsll(tmp, output.reg, Imm32(8));
  ma_daddu(output.reg, tmp);
  ma_dsll(tmp, output.reg, Imm32(16));
  ma_daddu(output.reg, tmp);
  ma_dsll(tmp, output.reg, Imm32(32));
  ma_daddu(output.reg, tmp);
  ma_dsra(output.reg, output.reg, Imm32(56));
}

// ===============================================================
// Branch functions

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

void MacroAssembler::branchPrivatePtr(Condition cond, const Address& lhs,
                                      Register rhs, Label* label) {
  branchPtr(cond, lhs, rhs, label);
}

template <class L>
void MacroAssembler::branchTest64(Condition cond, Register64 lhs,
                                  Register64 rhs, Register temp, L label) {
  branchTestPtr(cond, lhs.reg, rhs.reg, label);
}

void MacroAssembler::branchTestUndefined(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestUndefined(cond, scratch2, label);
}

void MacroAssembler::branchTestInt32(Condition cond, const ValueOperand& value,
                                     Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestInt32(cond, scratch2, label);
}

void MacroAssembler::branchTestInt32Truthy(bool b, const ValueOperand& value,
                                           Label* label) {
  ScratchRegisterScope scratch(*this);
  ma_dext(scratch, value.valueReg(), Imm32(0), Imm32(32));
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

void MacroAssembler::branchTestNumber(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestNumber(cond, scratch2, label);
}

void MacroAssembler::branchTestBoolean(Condition cond,
                                       const ValueOperand& value,
                                       Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestBoolean(cond, scratch2, label);
}

void MacroAssembler::branchTestBooleanTruthy(bool b, const ValueOperand& value,
                                             Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  unboxBoolean(value, scratch2);
  ma_b(scratch2, scratch2, label, b ? NonZero : Zero);
}

void MacroAssembler::branchTestString(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestString(cond, scratch2, label);
}

void MacroAssembler::branchTestStringTruthy(bool b, const ValueOperand& value,
                                            Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  unboxString(value, scratch2);
  load32(Address(scratch2, JSString::offsetOfLength()), scratch2);
  ma_b(scratch2, Imm32(0), label, b ? NotEqual : Equal);
}

void MacroAssembler::branchTestSymbol(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestSymbol(cond, scratch2, label);
}

void MacroAssembler::branchTestBigInt(Condition cond, const BaseIndex& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  computeEffectiveAddress(address, scratch2);
  splitTag(scratch2, scratch2);
  branchTestBigInt(cond, scratch2, label);
}

void MacroAssembler::branchTestBigInt(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestBigInt(cond, scratch2, label);
}

void MacroAssembler::branchTestBigIntTruthy(bool b, const ValueOperand& value,
                                            Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  unboxBigInt(value, scratch2);
  load32(Address(scratch2, BigInt::offsetOfDigitLength()), scratch2);
  ma_b(scratch2, Imm32(0), label, b ? NotEqual : Equal);
}

void MacroAssembler::branchTestNull(Condition cond, const ValueOperand& value,
                                    Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestNull(cond, scratch2, label);
}

void MacroAssembler::branchTestObject(Condition cond, const ValueOperand& value,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestObject(cond, scratch2, label);
}

void MacroAssembler::branchTestPrimitive(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  splitTag(value, scratch2);
  branchTestPrimitive(cond, scratch2, label);
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

void MacroAssembler::branchTruncateDoubleMaybeModUint32(FloatRegister src,
                                                        Register dest,
                                                        Label* fail) {
  as_truncld(ScratchDoubleReg, src);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromDouble(ScratchDoubleReg, dest);
  ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
  ma_b(ScratchRegister, Imm32(0), fail, Assembler::NotEqual);

  as_sll(dest, dest, 0);
}

void MacroAssembler::branchTruncateFloat32MaybeModUint32(FloatRegister src,
                                                         Register dest,
                                                         Label* fail) {
  as_truncls(ScratchDoubleReg, src);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromDouble(ScratchDoubleReg, dest);
  ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
  ma_b(ScratchRegister, Imm32(0), fail, Assembler::NotEqual);

  as_sll(dest, dest, 0);
}

void MacroAssembler::branchTruncateDoubleToInt32(FloatRegister src,
                                                 Register dest, Label* fail) {
  ScratchRegisterScope scratch(asMasm());
  ScratchDoubleScope fpscratch(asMasm());

  // Convert scalar to signed 64-bit fixed-point, rounding toward zero.
  // In the case of -0, the output is zero.
  // In the case of overflow, the output is:
  //   - MIPS64R2: 2^63-1
  //   - MIPS64R6: saturated
  // In the case of NaN, the output is:
  //   - MIPS64R2: 2^63-1
  //   - MIPS64R6: 0
  as_truncld(fpscratch, src);
  moveFromDouble(fpscratch, dest);

  // Fail on overflow cases, besides MIPS64R2 will also fail here on NaN cases.
  as_sll(scratch, dest, 0);
  ma_b(dest, scratch, fail, Assembler::NotEqual);
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
  mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), scratch);
  ma_xor(scratch, src.valueReg());
  ma_move(dest, scratch);
  ma_dsrl(scratch, scratch, Imm32(JSVAL_TAG_SHIFT));
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
  loadPtr(lhs, SecondScratchReg);
  cmpPtrSet(cond, SecondScratchReg, rhs, dest);
}

template <>
inline void MacroAssembler::cmpPtrSet(Assembler::Condition cond, Register lhs,
                                      Address rhs, Register dest) {
  MOZ_ASSERT(lhs != ScratchRegister);
  loadPtr(rhs, ScratchRegister);
  cmpPtrSet(cond, lhs, ScratchRegister, dest);
}

template <>
inline void MacroAssembler::cmpPtrSet(Assembler::Condition cond, Address lhs,
                                      Register rhs, Register dest) {
  MOZ_ASSERT(rhs != ScratchRegister);
  loadPtr(lhs, ScratchRegister);
  cmpPtrSet(cond, ScratchRegister, rhs, dest);
}

template <>
inline void MacroAssembler::cmp32Set(Assembler::Condition cond, Register lhs,
                                     Address rhs, Register dest) {
  MOZ_ASSERT(lhs != ScratchRegister);
  load32(rhs, ScratchRegister);
  cmp32Set(cond, lhs, ScratchRegister, dest);
}

template <>
inline void MacroAssembler::cmp32Set(Assembler::Condition cond, Address lhs,
                                     Register rhs, Register dest) {
  MOZ_ASSERT(rhs != ScratchRegister);
  load32(lhs, ScratchRegister);
  cmp32Set(cond, ScratchRegister, rhs, dest);
}

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs, Register rhs,
                                   Register src, Register dest) {
  Register scratch = ScratchRegister;
  MOZ_ASSERT(src != scratch && dest != scratch);
  cmpPtrSet(cond, lhs, rhs, scratch);
#ifdef MIPSR6
  as_selnez(src, src, scratch);
  as_seleqz(dest, dest, scratch);
  as_or(dest, dest, src);
#else
  as_movn(dest, src, scratch);
#endif
}

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs,
                                   const Address& rhs, Register src,
                                   Register dest) {
  MOZ_CRASH("NYI");
}

void MacroAssemblerMIPS64Compat::incrementInt32Value(const Address& addr) {
  asMasm().add32(Imm32(1), addr);
}

void MacroAssemblerMIPS64Compat::retn(Imm32 n) {
  // pc <- [sp]; sp += n
  loadPtr(Address(StackPointer, 0), ra);
  asMasm().addPtr(n, StackPointer);
  as_jr(ra);
  as_nop();
}

}  // namespace jit
}  // namespace js

#endif /* jit_mips64_MacroAssembler_mips64_inl_h */
