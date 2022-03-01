/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_MacroAssembler_x86_shared_inl_h
#define jit_x86_shared_MacroAssembler_x86_shared_inl_h

#include "jit/x86-shared/MacroAssembler-x86-shared.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style
// ===============================================================
// Move instructions

void MacroAssembler::moveFloat32ToGPR(FloatRegister src, Register dest) {
  vmovd(src, dest);
}

void MacroAssembler::moveGPRToFloat32(Register src, FloatRegister dest) {
  vmovd(src, dest);
}

void MacroAssembler::move8SignExtend(Register src, Register dest) {
  movsbl(src, dest);
}

void MacroAssembler::move16SignExtend(Register src, Register dest) {
  movswl(src, dest);
}

void MacroAssembler::loadAbiReturnAddress(Register dest) {
  loadPtr(Address(getStackPointer(), 0), dest);
}

// ===============================================================
// Logical instructions

void MacroAssembler::not32(Register reg) { notl(reg); }

void MacroAssembler::and32(Register src, Register dest) { andl(src, dest); }

void MacroAssembler::and32(Imm32 imm, Register dest) { andl(imm, dest); }

void MacroAssembler::and32(Imm32 imm, const Address& dest) {
  andl(imm, Operand(dest));
}

void MacroAssembler::and32(const Address& src, Register dest) {
  andl(Operand(src), dest);
}

void MacroAssembler::or32(Register src, Register dest) { orl(src, dest); }

void MacroAssembler::or32(Imm32 imm, Register dest) { orl(imm, dest); }

void MacroAssembler::or32(Imm32 imm, const Address& dest) {
  orl(imm, Operand(dest));
}

void MacroAssembler::xor32(Register src, Register dest) { xorl(src, dest); }

void MacroAssembler::xor32(Imm32 imm, Register dest) { xorl(imm, dest); }

void MacroAssembler::xor32(Imm32 imm, const Address& dest) {
  xorl(imm, Operand(dest));
}

void MacroAssembler::xor32(const Address& src, Register dest) {
  xorl(Operand(src), dest);
}

void MacroAssembler::clz32(Register src, Register dest, bool knownNotZero) {
  if (AssemblerX86Shared::HasLZCNT()) {
    lzcntl(src, dest);
    return;
  }

  bsrl(src, dest);
  if (!knownNotZero) {
    // If the source is zero then bsrl leaves garbage in the destination.
    Label nonzero;
    j(Assembler::NonZero, &nonzero);
    movl(Imm32(0x3F), dest);
    bind(&nonzero);
  }
  xorl(Imm32(0x1F), dest);
}

void MacroAssembler::ctz32(Register src, Register dest, bool knownNotZero) {
  if (AssemblerX86Shared::HasBMI1()) {
    tzcntl(src, dest);
    return;
  }

  bsfl(src, dest);
  if (!knownNotZero) {
    Label nonzero;
    j(Assembler::NonZero, &nonzero);
    movl(Imm32(32), dest);
    bind(&nonzero);
  }
}

void MacroAssembler::popcnt32(Register input, Register output, Register tmp) {
  if (AssemblerX86Shared::HasPOPCNT()) {
    popcntl(input, output);
    return;
  }

  MOZ_ASSERT(tmp != InvalidReg);

  // Equivalent to mozilla::CountPopulation32()

  movl(input, tmp);
  if (input != output) {
    movl(input, output);
  }
  shrl(Imm32(1), output);
  andl(Imm32(0x55555555), output);
  subl(output, tmp);
  movl(tmp, output);
  andl(Imm32(0x33333333), output);
  shrl(Imm32(2), tmp);
  andl(Imm32(0x33333333), tmp);
  addl(output, tmp);
  movl(tmp, output);
  shrl(Imm32(4), output);
  addl(tmp, output);
  andl(Imm32(0xF0F0F0F), output);
  imull(Imm32(0x1010101), output, output);
  shrl(Imm32(24), output);
}

// ===============================================================
// Swap instructions

void MacroAssembler::byteSwap16SignExtend(Register reg) {
  rolw(Imm32(8), reg);
  movswl(reg, reg);
}

void MacroAssembler::byteSwap16ZeroExtend(Register reg) {
  rolw(Imm32(8), reg);
  movzwl(reg, reg);
}

void MacroAssembler::byteSwap32(Register reg) { bswapl(reg); }

// ===============================================================
// Arithmetic instructions

void MacroAssembler::add32(Register src, Register dest) { addl(src, dest); }

void MacroAssembler::add32(Imm32 imm, Register dest) { addl(imm, dest); }

void MacroAssembler::add32(Imm32 imm, const Address& dest) {
  addl(imm, Operand(dest));
}

void MacroAssembler::add32(Imm32 imm, const AbsoluteAddress& dest) {
  addl(imm, Operand(dest));
}

void MacroAssembler::addFloat32(FloatRegister src, FloatRegister dest) {
  vaddss(src, dest, dest);
}

void MacroAssembler::addDouble(FloatRegister src, FloatRegister dest) {
  vaddsd(src, dest, dest);
}

void MacroAssembler::sub32(Register src, Register dest) { subl(src, dest); }

void MacroAssembler::sub32(Imm32 imm, Register dest) { subl(imm, dest); }

void MacroAssembler::sub32(const Address& src, Register dest) {
  subl(Operand(src), dest);
}

void MacroAssembler::subDouble(FloatRegister src, FloatRegister dest) {
  vsubsd(src, dest, dest);
}

void MacroAssembler::subFloat32(FloatRegister src, FloatRegister dest) {
  vsubss(src, dest, dest);
}

void MacroAssembler::mul32(Register rhs, Register srcDest) {
  imull(rhs, srcDest);
}

void MacroAssembler::mulFloat32(FloatRegister src, FloatRegister dest) {
  vmulss(src, dest, dest);
}

void MacroAssembler::mulDouble(FloatRegister src, FloatRegister dest) {
  vmulsd(src, dest, dest);
}

void MacroAssembler::quotient32(Register rhs, Register srcDest,
                                bool isUnsigned) {
  MOZ_ASSERT(srcDest == eax);

  // Sign extend eax into edx to make (edx:eax): idiv/udiv are 64-bit.
  if (isUnsigned) {
    mov(ImmWord(0), edx);
    udiv(rhs);
  } else {
    cdq();
    idiv(rhs);
  }
}

void MacroAssembler::remainder32(Register rhs, Register srcDest,
                                 bool isUnsigned) {
  MOZ_ASSERT(srcDest == eax);

  // Sign extend eax into edx to make (edx:eax): idiv/udiv are 64-bit.
  if (isUnsigned) {
    mov(ImmWord(0), edx);
    udiv(rhs);
  } else {
    cdq();
    idiv(rhs);
  }
  mov(edx, eax);
}

void MacroAssembler::divFloat32(FloatRegister src, FloatRegister dest) {
  vdivss(src, dest, dest);
}

void MacroAssembler::divDouble(FloatRegister src, FloatRegister dest) {
  vdivsd(src, dest, dest);
}

void MacroAssembler::neg32(Register reg) { negl(reg); }

void MacroAssembler::negateFloat(FloatRegister reg) {
  ScratchFloat32Scope scratch(*this);
  vpcmpeqw(Operand(scratch), scratch, scratch);
  vpsllq(Imm32(31), scratch, scratch);

  // XOR the float in a float register with -0.0.
  vxorps(scratch, reg, reg);  // s ^ 0x80000000
}

void MacroAssembler::negateDouble(FloatRegister reg) {
  // From MacroAssemblerX86Shared::maybeInlineDouble
  ScratchDoubleScope scratch(*this);
  vpcmpeqw(Operand(scratch), scratch, scratch);
  vpsllq(Imm32(63), scratch, scratch);

  // XOR the float in a float register with -0.0.
  vxorpd(scratch, reg, reg);  // s ^ 0x80000000000000
}

void MacroAssembler::abs32(Register src, Register dest) {
  if (src != dest) {
    move32(src, dest);
  }
  Label positive;
  branchTest32(Assembler::NotSigned, dest, dest, &positive);
  neg32(dest);
  bind(&positive);
}

void MacroAssembler::absFloat32(FloatRegister src, FloatRegister dest) {
  ScratchFloat32Scope scratch(*this);
  loadConstantFloat32(mozilla::SpecificNaN<float>(
                          0, mozilla::FloatingPoint<float>::kSignificandBits),
                      scratch);
  vandps(scratch, src, dest);
}

void MacroAssembler::absDouble(FloatRegister src, FloatRegister dest) {
  ScratchDoubleScope scratch(*this);
  loadConstantDouble(mozilla::SpecificNaN<double>(
                         0, mozilla::FloatingPoint<double>::kSignificandBits),
                     scratch);
  vandpd(scratch, src, dest);
}

void MacroAssembler::sqrtFloat32(FloatRegister src, FloatRegister dest) {
  vsqrtss(src, dest, dest);
}

void MacroAssembler::sqrtDouble(FloatRegister src, FloatRegister dest) {
  vsqrtsd(src, dest, dest);
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
// Rotation instructions
void MacroAssembler::rotateLeft(Imm32 count, Register input, Register dest) {
  MOZ_ASSERT(input == dest, "defineReuseInput");
  count.value &= 0x1f;
  if (count.value) {
    roll(count, input);
  }
}

void MacroAssembler::rotateLeft(Register count, Register input, Register dest) {
  MOZ_ASSERT(input == dest, "defineReuseInput");
  MOZ_ASSERT(count == ecx, "defineFixed(ecx)");
  roll_cl(input);
}

void MacroAssembler::rotateRight(Imm32 count, Register input, Register dest) {
  MOZ_ASSERT(input == dest, "defineReuseInput");
  count.value &= 0x1f;
  if (count.value) {
    rorl(count, input);
  }
}

void MacroAssembler::rotateRight(Register count, Register input,
                                 Register dest) {
  MOZ_ASSERT(input == dest, "defineReuseInput");
  MOZ_ASSERT(count == ecx, "defineFixed(ecx)");
  rorl_cl(input);
}

// ===============================================================
// Shift instructions

void MacroAssembler::lshift32(Register shift, Register srcDest) {
  if (HasBMI2()) {
    shlxl(srcDest, shift, srcDest);
    return;
  }
  MOZ_ASSERT(shift == ecx);
  shll_cl(srcDest);
}

void MacroAssembler::flexibleLshift32(Register shift, Register srcDest) {
  if (HasBMI2()) {
    shlxl(srcDest, shift, srcDest);
    return;
  }
  if (shift == ecx) {
    shll_cl(srcDest);
  } else {
    // Shift amount must be in ecx.
    xchg(shift, ecx);
    shll_cl(shift == srcDest ? ecx : srcDest == ecx ? shift : srcDest);
    xchg(shift, ecx);
  }
}

void MacroAssembler::rshift32(Register shift, Register srcDest) {
  if (HasBMI2()) {
    shrxl(srcDest, shift, srcDest);
    return;
  }
  MOZ_ASSERT(shift == ecx);
  shrl_cl(srcDest);
}

void MacroAssembler::flexibleRshift32(Register shift, Register srcDest) {
  if (HasBMI2()) {
    shrxl(srcDest, shift, srcDest);
    return;
  }
  if (shift == ecx) {
    shrl_cl(srcDest);
  } else {
    // Shift amount must be in ecx.
    xchg(shift, ecx);
    shrl_cl(shift == srcDest ? ecx : srcDest == ecx ? shift : srcDest);
    xchg(shift, ecx);
  }
}

void MacroAssembler::rshift32Arithmetic(Register shift, Register srcDest) {
  if (HasBMI2()) {
    sarxl(srcDest, shift, srcDest);
    return;
  }
  MOZ_ASSERT(shift == ecx);
  sarl_cl(srcDest);
}

void MacroAssembler::flexibleRshift32Arithmetic(Register shift,
                                                Register srcDest) {
  if (HasBMI2()) {
    sarxl(srcDest, shift, srcDest);
    return;
  }
  if (shift == ecx) {
    sarl_cl(srcDest);
  } else {
    // Shift amount must be in ecx.
    xchg(shift, ecx);
    sarl_cl(shift == srcDest ? ecx : srcDest == ecx ? shift : srcDest);
    xchg(shift, ecx);
  }
}

void MacroAssembler::lshift32(Imm32 shift, Register srcDest) {
  shll(shift, srcDest);
}

void MacroAssembler::rshift32(Imm32 shift, Register srcDest) {
  shrl(shift, srcDest);
}

void MacroAssembler::rshift32Arithmetic(Imm32 shift, Register srcDest) {
  sarl(shift, srcDest);
}

// ===============================================================
// Condition functions

template <typename T1, typename T2>
void MacroAssembler::cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest) {
  cmp32(lhs, rhs);
  emitSet(cond, dest);
}

// ===============================================================
// Branch instructions

template <class L>
void MacroAssembler::branch32(Condition cond, Register lhs, Register rhs,
                              L label) {
  cmp32(lhs, rhs);
  j(cond, label);
}

template <class L>
void MacroAssembler::branch32(Condition cond, Register lhs, Imm32 rhs,
                              L label) {
  cmp32(lhs, rhs);
  j(cond, label);
}

void MacroAssembler::branch32(Condition cond, const Address& lhs, Register rhs,
                              Label* label) {
  cmp32(Operand(lhs), rhs);
  j(cond, label);
}

void MacroAssembler::branch32(Condition cond, const Address& lhs, Imm32 rhs,
                              Label* label) {
  cmp32(Operand(lhs), rhs);
  j(cond, label);
}

void MacroAssembler::branch32(Condition cond, const BaseIndex& lhs,
                              Register rhs, Label* label) {
  cmp32(Operand(lhs), rhs);
  j(cond, label);
}

void MacroAssembler::branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs,
                              Label* label) {
  cmp32(Operand(lhs), rhs);
  j(cond, label);
}

void MacroAssembler::branch32(Condition cond, const Operand& lhs, Register rhs,
                              Label* label) {
  cmp32(lhs, rhs);
  j(cond, label);
}

void MacroAssembler::branch32(Condition cond, const Operand& lhs, Imm32 rhs,
                              Label* label) {
  cmp32(lhs, rhs);
  j(cond, label);
}

template <class L>
void MacroAssembler::branchPtr(Condition cond, Register lhs, Register rhs,
                               L label) {
  cmpPtr(lhs, rhs);
  j(cond, label);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, Imm32 rhs,
                               Label* label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmPtr rhs,
                               Label* label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmGCPtr rhs,
                               Label* label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmWord rhs,
                               Label* label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

template <class L>
void MacroAssembler::branchPtr(Condition cond, const Address& lhs, Register rhs,
                               L label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmPtr rhs,
                               Label* label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmGCPtr rhs,
                               Label* label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmWord rhs,
                               Label* label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs,
                               ImmWord rhs, Label* label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

void MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs,
                               Register rhs, Label* label) {
  branchPtrImpl(cond, lhs, rhs, label);
}

template <typename T, typename S, typename L>
void MacroAssembler::branchPtrImpl(Condition cond, const T& lhs, const S& rhs,
                                   L label) {
  cmpPtr(Operand(lhs), rhs);
  j(cond, label);
}

void MacroAssembler::branchFloat(DoubleCondition cond, FloatRegister lhs,
                                 FloatRegister rhs, Label* label) {
  compareFloat(cond, lhs, rhs);

  if (cond == DoubleEqual) {
    Label unordered;
    j(Parity, &unordered);
    j(Equal, label);
    bind(&unordered);
    return;
  }

  if (cond == DoubleNotEqualOrUnordered) {
    j(NotEqual, label);
    j(Parity, label);
    return;
  }

  MOZ_ASSERT(!(cond & DoubleConditionBitSpecial));
  j(ConditionFromDoubleCondition(cond), label);
}

void MacroAssembler::branchDouble(DoubleCondition cond, FloatRegister lhs,
                                  FloatRegister rhs, Label* label) {
  compareDouble(cond, lhs, rhs);

  if (cond == DoubleEqual) {
    Label unordered;
    j(Parity, &unordered);
    j(Equal, label);
    bind(&unordered);
    return;
  }
  if (cond == DoubleNotEqualOrUnordered) {
    j(NotEqual, label);
    j(Parity, label);
    return;
  }

  MOZ_ASSERT(!(cond & DoubleConditionBitSpecial));
  j(ConditionFromDoubleCondition(cond), label);
}

template <typename T>
void MacroAssembler::branchAdd32(Condition cond, T src, Register dest,
                                 Label* label) {
  addl(src, dest);
  j(cond, label);
}

template <typename T>
void MacroAssembler::branchSub32(Condition cond, T src, Register dest,
                                 Label* label) {
  subl(src, dest);
  j(cond, label);
}

template <typename T>
void MacroAssembler::branchMul32(Condition cond, T src, Register dest,
                                 Label* label) {
  mul32(src, dest);
  j(cond, label);
}

template <typename T>
void MacroAssembler::branchRshift32(Condition cond, T src, Register dest,
                                    Label* label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero);
  rshift32(src, dest);
  j(cond, label);
}

void MacroAssembler::branchNeg32(Condition cond, Register reg, Label* label) {
  MOZ_ASSERT(cond == Overflow);
  neg32(reg);
  j(cond, label);
}

template <typename T>
void MacroAssembler::branchAddPtr(Condition cond, T src, Register dest,
                                  Label* label) {
  addPtr(src, dest);
  j(cond, label);
}

template <typename T>
void MacroAssembler::branchSubPtr(Condition cond, T src, Register dest,
                                  Label* label) {
  subPtr(src, dest);
  j(cond, label);
}

void MacroAssembler::branchMulPtr(Condition cond, Register src, Register dest,
                                  Label* label) {
  mulPtr(src, dest);
  j(cond, label);
}

void MacroAssembler::decBranchPtr(Condition cond, Register lhs, Imm32 rhs,
                                  Label* label) {
  subPtr(rhs, lhs);
  j(cond, label);
}

template <class L>
void MacroAssembler::branchTest32(Condition cond, Register lhs, Register rhs,
                                  L label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  test32(lhs, rhs);
  j(cond, label);
}

template <class L>
void MacroAssembler::branchTest32(Condition cond, Register lhs, Imm32 rhs,
                                  L label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  test32(lhs, rhs);
  j(cond, label);
}

void MacroAssembler::branchTest32(Condition cond, const Address& lhs, Imm32 rhs,
                                  Label* label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed ||
             cond == NotSigned);
  test32(Operand(lhs), rhs);
  j(cond, label);
}

template <class L>
void MacroAssembler::branchTestPtr(Condition cond, Register lhs, Register rhs,
                                   L label) {
  testPtr(lhs, rhs);
  j(cond, label);
}

void MacroAssembler::branchTestPtr(Condition cond, Register lhs, Imm32 rhs,
                                   Label* label) {
  testPtr(lhs, rhs);
  j(cond, label);
}

void MacroAssembler::branchTestPtr(Condition cond, const Address& lhs,
                                   Imm32 rhs, Label* label) {
  testPtr(Operand(lhs), rhs);
  j(cond, label);
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
  cond = testUndefined(cond, t);
  j(cond, label);
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
  cond = testInt32(cond, t);
  j(cond, label);
}

void MacroAssembler::branchTestInt32Truthy(bool truthy,
                                           const ValueOperand& value,
                                           Label* label) {
  Condition cond = testInt32Truthy(truthy, value);
  j(cond, label);
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
  cond = testDouble(cond, t);
  j(cond, label);
}

void MacroAssembler::branchTestDoubleTruthy(bool truthy, FloatRegister reg,
                                            Label* label) {
  Condition cond = testDoubleTruthy(truthy, reg);
  j(cond, label);
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
  j(cond, label);
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
  cond = testBoolean(cond, t);
  j(cond, label);
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
  cond = testString(cond, t);
  j(cond, label);
}

void MacroAssembler::branchTestStringTruthy(bool truthy,
                                            const ValueOperand& value,
                                            Label* label) {
  Condition cond = testStringTruthy(truthy, value);
  j(cond, label);
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
  cond = testSymbol(cond, t);
  j(cond, label);
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
  cond = testBigInt(cond, t);
  j(cond, label);
}

void MacroAssembler::branchTestBigIntTruthy(bool truthy,
                                            const ValueOperand& value,
                                            Label* label) {
  Condition cond = testBigIntTruthy(truthy, value);
  j(cond, label);
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
  cond = testNull(cond, t);
  j(cond, label);
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
  cond = testObject(cond, t);
  j(cond, label);
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
  cond = testGCThing(cond, t);
  j(cond, label);
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
  cond = testPrimitive(cond, t);
  j(cond, label);
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
  j(cond, label);
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs, Register rhs,
                                 Register src, Register dest) {
  cmp32(lhs, rhs);
  cmovCCl(cond, src, dest);
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs,
                                 const Address& rhs, Register src,
                                 Register dest) {
  cmp32(lhs, Operand(rhs));
  cmovCCl(cond, src, dest);
}

void MacroAssembler::cmp32Load32(Condition cond, Register lhs,
                                 const Address& rhs, const Address& src,
                                 Register dest) {
  cmp32(lhs, Operand(rhs));
  cmovCCl(cond, Operand(src), dest);
}

void MacroAssembler::cmp32Load32(Condition cond, Register lhs, Register rhs,
                                 const Address& src, Register dest) {
  cmp32(lhs, rhs);
  cmovCCl(cond, Operand(src), dest);
}

void MacroAssembler::spectreZeroRegister(Condition cond, Register scratch,
                                         Register dest) {
  // Note: use movl instead of move32/xorl to ensure flags are not clobbered.
  movl(Imm32(0), scratch);
  spectreMovePtr(cond, scratch, dest);
}

// ========================================================================
// Memory access primitives.
void MacroAssembler::storeUncanonicalizedDouble(FloatRegister src,
                                                const Address& dest) {
  vmovsd(src, dest);
}
void MacroAssembler::storeUncanonicalizedDouble(FloatRegister src,
                                                const BaseIndex& dest) {
  vmovsd(src, dest);
}
void MacroAssembler::storeUncanonicalizedDouble(FloatRegister src,
                                                const Operand& dest) {
  switch (dest.kind()) {
    case Operand::MEM_REG_DISP:
      storeUncanonicalizedDouble(src, dest.toAddress());
      break;
    case Operand::MEM_SCALE:
      storeUncanonicalizedDouble(src, dest.toBaseIndex());
      break;
    default:
      MOZ_CRASH("unexpected operand kind");
  }
}

template void MacroAssembler::storeDouble(FloatRegister src,
                                          const Operand& dest);

void MacroAssembler::storeUncanonicalizedFloat32(FloatRegister src,
                                                 const Address& dest) {
  vmovss(src, dest);
}
void MacroAssembler::storeUncanonicalizedFloat32(FloatRegister src,
                                                 const BaseIndex& dest) {
  vmovss(src, dest);
}
void MacroAssembler::storeUncanonicalizedFloat32(FloatRegister src,
                                                 const Operand& dest) {
  switch (dest.kind()) {
    case Operand::MEM_REG_DISP:
      storeUncanonicalizedFloat32(src, dest.toAddress());
      break;
    case Operand::MEM_SCALE:
      storeUncanonicalizedFloat32(src, dest.toBaseIndex());
      break;
    default:
      MOZ_CRASH("unexpected operand kind");
  }
}

template void MacroAssembler::storeFloat32(FloatRegister src,
                                           const Operand& dest);

void MacroAssembler::memoryBarrier(MemoryBarrierBits barrier) {
  if (barrier & MembarStoreLoad) {
    storeLoadFence();
  }
}

// ========================================================================
// Wasm SIMD
//
// Some parts of the masm API are currently agnostic as to the data's
// interpretation as int or float, despite the Intel architecture having
// separate functional units and sometimes penalizing type-specific instructions
// that operate on data in the "wrong" unit.
//
// For the time being, we always choose the integer interpretation when we are
// forced to choose blind, but whether that is right or wrong depends on the
// application.  This applies to moveSimd128, zeroSimd128, loadConstantSimd128,
// loadUnalignedSimd128, and storeUnalignedSimd128, at least.
//
// SSE4.1 or better is assumed.
//
// The order of operations here follows the header file.

// Moves.  See comments above regarding integer operation.

void MacroAssembler::moveSimd128(FloatRegister src, FloatRegister dest) {
  MacroAssemblerX86Shared::moveSimd128Int(src, dest);
}

// Constants.  See comments above regarding integer operation.

void MacroAssembler::zeroSimd128(FloatRegister dest) {
  MacroAssemblerX86Shared::zeroSimd128Int(dest);
}

void MacroAssembler::loadConstantSimd128(const SimdConstant& v,
                                         FloatRegister dest) {
  if (v.isFloatingType()) {
    loadConstantSimd128Float(v, dest);
  } else {
    loadConstantSimd128Int(v, dest);
  }
}

// Splat

void MacroAssembler::splatX16(Register src, FloatRegister dest) {
  MacroAssemblerX86Shared::splatX16(src, dest);
}

void MacroAssembler::splatX8(Register src, FloatRegister dest) {
  MacroAssemblerX86Shared::splatX8(src, dest);
}

void MacroAssembler::splatX4(Register src, FloatRegister dest) {
  MacroAssemblerX86Shared::splatX4(src, dest);
}

void MacroAssembler::splatX4(FloatRegister src, FloatRegister dest) {
  MacroAssemblerX86Shared::splatX4(src, dest);
}

void MacroAssembler::splatX2(FloatRegister src, FloatRegister dest) {
  MacroAssemblerX86Shared::splatX2(src, dest);
}

// Extract lane as scalar

void MacroAssembler::extractLaneInt8x16(uint32_t lane, FloatRegister src,
                                        Register dest) {
  MacroAssemblerX86Shared::extractLaneInt8x16(src, dest, lane,
                                              SimdSign::Signed);
}

void MacroAssembler::unsignedExtractLaneInt8x16(uint32_t lane,
                                                FloatRegister src,
                                                Register dest) {
  MacroAssemblerX86Shared::extractLaneInt8x16(src, dest, lane,
                                              SimdSign::Unsigned);
}

void MacroAssembler::extractLaneInt16x8(uint32_t lane, FloatRegister src,
                                        Register dest) {
  MacroAssemblerX86Shared::extractLaneInt16x8(src, dest, lane,
                                              SimdSign::Signed);
}

void MacroAssembler::unsignedExtractLaneInt16x8(uint32_t lane,
                                                FloatRegister src,
                                                Register dest) {
  MacroAssemblerX86Shared::extractLaneInt16x8(src, dest, lane,
                                              SimdSign::Unsigned);
}

void MacroAssembler::extractLaneInt32x4(uint32_t lane, FloatRegister src,
                                        Register dest) {
  MacroAssemblerX86Shared::extractLaneInt32x4(src, dest, lane);
}

void MacroAssembler::extractLaneFloat32x4(uint32_t lane, FloatRegister src,
                                          FloatRegister dest) {
  MacroAssemblerX86Shared::extractLaneFloat32x4(src, dest, lane);
}

void MacroAssembler::extractLaneFloat64x2(uint32_t lane, FloatRegister src,
                                          FloatRegister dest) {
  MacroAssemblerX86Shared::extractLaneFloat64x2(src, dest, lane);
}

// Replace lane value

void MacroAssembler::replaceLaneInt8x16(unsigned lane, Register rhs,
                                        FloatRegister lhsDest) {
  vpinsrb(lane, Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::replaceLaneInt16x8(unsigned lane, Register rhs,
                                        FloatRegister lhsDest) {
  vpinsrw(lane, Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::replaceLaneInt32x4(unsigned lane, Register rhs,
                                        FloatRegister lhsDest) {
  vpinsrd(lane, rhs, lhsDest, lhsDest);
}

void MacroAssembler::replaceLaneFloat32x4(unsigned lane, FloatRegister rhs,
                                          FloatRegister lhsDest) {
  MacroAssemblerX86Shared::replaceLaneFloat32x4(rhs, lhsDest, lane);
}

void MacroAssembler::replaceLaneFloat64x2(unsigned lane, FloatRegister rhs,
                                          FloatRegister lhsDest) {
  MacroAssemblerX86Shared::replaceLaneFloat64x2(rhs, lhsDest, lane);
}

// Shuffle - permute with immediate indices

void MacroAssembler::shuffleInt8x16(const uint8_t lanes[16], FloatRegister rhs,
                                    FloatRegister lhsDest) {
  MacroAssemblerX86Shared::shuffleInt8x16(lhsDest, rhs, lhsDest, lanes);
}

void MacroAssembler::blendInt8x16(const uint8_t lanes[16], FloatRegister lhs,
                                  FloatRegister rhs, FloatRegister dest,
                                  FloatRegister temp) {
  MacroAssemblerX86Shared::blendInt8x16(lhs, rhs, dest, temp, lanes);
}

void MacroAssembler::blendInt16x8(const uint16_t lanes[8], FloatRegister lhs,
                                  FloatRegister rhs, FloatRegister dest) {
  MacroAssemblerX86Shared::blendInt16x8(lhs, rhs, dest, lanes);
}

void MacroAssembler::interleaveHighInt16x8(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpunpckhwd(rhs, lhsDest, lhsDest);
}

void MacroAssembler::interleaveHighInt32x4(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpunpckhdq(rhs, lhsDest, lhsDest);
}

void MacroAssembler::interleaveHighInt64x2(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpunpckhqdq(rhs, lhsDest, lhsDest);
}

void MacroAssembler::interleaveHighInt8x16(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpunpckhbw(rhs, lhsDest, lhsDest);
}

void MacroAssembler::interleaveLowInt16x8(FloatRegister rhs,
                                          FloatRegister lhsDest) {
  vpunpcklwd(rhs, lhsDest, lhsDest);
}

void MacroAssembler::interleaveLowInt32x4(FloatRegister rhs,
                                          FloatRegister lhsDest) {
  vpunpckldq(rhs, lhsDest, lhsDest);
}

void MacroAssembler::interleaveLowInt64x2(FloatRegister rhs,
                                          FloatRegister lhsDest) {
  vpunpcklqdq(rhs, lhsDest, lhsDest);
}

void MacroAssembler::interleaveLowInt8x16(FloatRegister rhs,
                                          FloatRegister lhsDest) {
  vpunpcklbw(rhs, lhsDest, lhsDest);
}

void MacroAssembler::permuteInt8x16(const uint8_t lanes[16], FloatRegister src,
                                    FloatRegister dest) {
  moveSimd128Int(src, dest);
  vpshufbSimd128(SimdConstant::CreateX16((const int8_t*)lanes), dest);
}

void MacroAssembler::permuteLowInt16x8(const uint16_t lanes[4],
                                       FloatRegister src, FloatRegister dest) {
  MOZ_ASSERT(lanes[0] < 4 && lanes[1] < 4 && lanes[2] < 4 && lanes[3] < 4);
  vpshuflw(ComputeShuffleMask(lanes[0], lanes[1], lanes[2], lanes[3]), src,
           dest);
}

void MacroAssembler::permuteHighInt16x8(const uint16_t lanes[4],
                                        FloatRegister src, FloatRegister dest) {
  MOZ_ASSERT(lanes[0] < 4 && lanes[1] < 4 && lanes[2] < 4 && lanes[3] < 4);
  vpshufhw(ComputeShuffleMask(lanes[0], lanes[1], lanes[2], lanes[3]), src,
           dest);
}

void MacroAssembler::permuteInt32x4(const uint32_t lanes[4], FloatRegister src,
                                    FloatRegister dest) {
  vpshufd(ComputeShuffleMask(lanes[0], lanes[1], lanes[2], lanes[3]), src,
          dest);
}

void MacroAssembler::concatAndRightShiftSimd128(FloatRegister rhs,
                                                FloatRegister lhsDest,
                                                uint32_t shift) {
  vpalignr(Operand(rhs), lhsDest, shift);
}

void MacroAssembler::leftShiftSimd128(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  moveSimd128(src, dest);
  vpslldq(count, dest, dest);
}

void MacroAssembler::rightShiftSimd128(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  moveSimd128(src, dest);
  vpsrldq(count, dest, dest);
}

// All lanes true

void MacroAssembler::allTrueInt8x16(FloatRegister src, Register dest) {
  ScratchSimd128Scope xtmp(*this);
  // xtmp is all-00h
  vpxor(xtmp, xtmp, xtmp);
  // Set FFh if byte==0 otherwise 00h
  // Operand ordering constraint: lhs==output
  vpcmpeqb(Operand(src), xtmp, xtmp);
  // Get all bytes' high bits
  vpmovmskb(xtmp, dest);
  // Now set dest to 1 if it is zero, otherwise to zero.
  testl(dest, dest);
  emitSetRegisterIfZero(dest);
}

void MacroAssembler::allTrueInt16x8(FloatRegister src, Register dest) {
  ScratchSimd128Scope xtmp(*this);
  // xtmp is all-00h
  vpxor(xtmp, xtmp, xtmp);
  // Set FFFFh if word==0 otherwise 0000h
  // Operand ordering constraint: lhs==output
  vpcmpeqw(Operand(src), xtmp, xtmp);
  // Get all bytes' high bits
  vpmovmskb(xtmp, dest);
  // Now set dest to 1 if it is zero, otherwise to zero.
  testl(dest, dest);
  emitSetRegisterIfZero(dest);
}

void MacroAssembler::allTrueInt32x4(FloatRegister src, Register dest) {
  ScratchSimd128Scope xtmp(*this);
  // xtmp is all-00h
  vpxor(xtmp, xtmp, xtmp);
  // Set FFFFFFFFh if doubleword==0 otherwise 00000000h
  // Operand ordering constraint: lhs==output
  vpcmpeqd(Operand(src), xtmp, xtmp);
  // Get all bytes' high bits
  vpmovmskb(xtmp, dest);
  // Now set dest to 1 if it is zero, otherwise to zero.
  testl(dest, dest);
  emitSetRegisterIfZero(dest);
}

void MacroAssembler::allTrueInt64x2(FloatRegister src, Register dest) {
  ScratchSimd128Scope xtmp(*this);
  // xtmp is all-00h
  vpxor(xtmp, xtmp, xtmp);
  // Set FFFFFFFFFFFFFFFFh if quadword==0 otherwise 0000000000000000h
  // Operand ordering constraint: lhs==output
  vpcmpeqq(Operand(src), xtmp, xtmp);
  // Get all bytes' high bits
  vpmovmskb(xtmp, dest);
  // Now set dest to 1 if it is zero, otherwise to zero.
  testl(dest, dest);
  emitSetRegisterIfZero(dest);
}

// Bitmask

void MacroAssembler::bitmaskInt8x16(FloatRegister src, Register dest) {
  vpmovmskb(src, dest);
}

void MacroAssembler::bitmaskInt16x8(FloatRegister src, Register dest) {
  ScratchSimd128Scope scratch(*this);
  // A three-instruction sequence is possible by using scratch as a don't-care
  // input and shifting rather than masking at the end, but creates a false
  // dependency on the old value of scratch.  The better fix is to allow src to
  // be clobbered.
  moveSimd128(src, scratch);
  vpacksswb(Operand(scratch), scratch, scratch);
  vpmovmskb(scratch, dest);
  andl(Imm32(0xFF), dest);
}

void MacroAssembler::bitmaskInt32x4(FloatRegister src, Register dest) {
  vmovmskps(src, dest);
}

void MacroAssembler::bitmaskInt64x2(FloatRegister src, Register dest) {
  vmovmskpd(src, dest);
}

// Swizzle - permute with variable indices

void MacroAssembler::swizzleInt8x16(FloatRegister rhs, FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  moveSimd128Int(rhs, scratch);
  vpcmpgtbSimd128(SimdConstant::SplatX16(15), scratch);  // set high bit
  vpor(Operand(rhs), scratch, scratch);                  //   for values > 15
  vpshufb(scratch, lhsDest, lhsDest);                    // permute
}

// Integer Add

void MacroAssembler::addInt8x16(FloatRegister rhs, FloatRegister lhsDest) {
  vpaddb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::addInt8x16(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpaddb,
                &MacroAssembler::vpaddbSimd128);
}

void MacroAssembler::addInt16x8(FloatRegister rhs, FloatRegister lhsDest) {
  vpaddw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::addInt16x8(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpaddw,
                &MacroAssembler::vpaddwSimd128);
}

void MacroAssembler::addInt32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vpaddd(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::addInt32x4(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpaddd,
                &MacroAssembler::vpadddSimd128);
}

void MacroAssembler::addInt64x2(FloatRegister rhs, FloatRegister lhsDest) {
  vpaddq(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::addInt64x2(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpaddq,
                &MacroAssembler::vpaddqSimd128);
}

// Integer subtract

void MacroAssembler::subInt8x16(FloatRegister rhs, FloatRegister lhsDest) {
  vpsubb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::subInt8x16(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpsubb,
                &MacroAssembler::vpsubbSimd128);
}

void MacroAssembler::subInt16x8(FloatRegister rhs, FloatRegister lhsDest) {
  vpsubw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::subInt16x8(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpsubw,
                &MacroAssembler::vpsubwSimd128);
}

void MacroAssembler::subInt32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vpsubd(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::subInt32x4(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpsubd,
                &MacroAssembler::vpsubdSimd128);
}

void MacroAssembler::subInt64x2(FloatRegister rhs, FloatRegister lhsDest) {
  vpsubq(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::subInt64x2(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpsubq,
                &MacroAssembler::vpsubqSimd128);
}

// Integer multiply

void MacroAssembler::mulInt16x8(FloatRegister rhs, FloatRegister lhsDest) {
  vpmullw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::mulInt16x8(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpmullw,
                &MacroAssembler::vpmullwSimd128);
}

void MacroAssembler::mulInt32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vpmulld(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::mulInt32x4(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpmulld,
                &MacroAssembler::vpmulldSimd128);
}

void MacroAssembler::mulInt64x2(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest, FloatRegister temp) {
  MOZ_ASSERT(lhs == dest);
  ScratchSimd128Scope temp2(*this);
  // lhs = <D C> <B A>
  // rhs = <H G> <F E>
  // result = <(DG+CH)_low+CG_high CG_low> <(BE+AF)_low+AE_high AE_low>
  moveSimd128(lhs, temp);               // temp  = <D C> <B A>
  vpsrlq(Imm32(32), temp, temp);        // temp  = <0 D> <0 B>
  vpmuludq(rhs, temp, temp);            // temp  = <DG> <BE>
  moveSimd128(rhs, temp2);              // temp2 = <H G> <F E>
  vpsrlq(Imm32(32), temp2, temp2);      // temp2 = <0 H> <0 F>
  vpmuludq(lhs, temp2, temp2);          // temp2 = <CH> <AF>
  vpaddq(Operand(temp), temp2, temp2);  // temp2 = <DG+CH> <BE+AF>
  vpsllq(Imm32(32), temp2, temp2);      // temp2 = <(DG+CH)_low 0>
                                        //         <(BE+AF)_low 0>
  vpmuludq(rhs, dest, dest);            // dest = <CG_high CG_low>
                                        //        <AE_high AE_low>
  vpaddq(Operand(temp2), dest, dest);   // dest =
                                        //    <(DG+CH)_low+CG_high CG_low>
                                        //    <(BE+AF)_low+AE_high AE_low>
}

// Code generation from the PR: https://github.com/WebAssembly/simd/pull/376.
// The double PSHUFD for the 32->64 case is not great, and there's some
// discussion on the PR (scroll down far enough) on how to avoid one of them,
// but we need benchmarking + correctness proofs.

void MacroAssembler::extMulLowInt8x16(FloatRegister rhs,
                                      FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  widenLowInt8x16(rhs, scratch);
  widenLowInt8x16(lhsDest, lhsDest);
  mulInt16x8(scratch, lhsDest);
}

void MacroAssembler::extMulHighInt8x16(FloatRegister rhs,
                                       FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  widenHighInt8x16(rhs, scratch);
  widenHighInt8x16(lhsDest, lhsDest);
  mulInt16x8(scratch, lhsDest);
}

void MacroAssembler::unsignedExtMulLowInt8x16(FloatRegister rhs,
                                              FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  unsignedWidenLowInt8x16(rhs, scratch);
  unsignedWidenLowInt8x16(lhsDest, lhsDest);
  mulInt16x8(scratch, lhsDest);
}

void MacroAssembler::unsignedExtMulHighInt8x16(FloatRegister rhs,
                                               FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  unsignedWidenHighInt8x16(rhs, scratch);
  unsignedWidenHighInt8x16(lhsDest, lhsDest);
  mulInt16x8(scratch, lhsDest);
}

void MacroAssembler::extMulLowInt16x8(FloatRegister rhs,
                                      FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  vmovdqa(lhsDest, scratch);
  vpmullw(Operand(rhs), lhsDest, lhsDest);
  vpmulhw(Operand(rhs), scratch, scratch);
  vpunpcklwd(scratch, lhsDest, lhsDest);
}

void MacroAssembler::extMulHighInt16x8(FloatRegister rhs,
                                       FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  vmovdqa(lhsDest, scratch);
  vpmullw(Operand(rhs), lhsDest, lhsDest);
  vpmulhw(Operand(rhs), scratch, scratch);
  vpunpckhwd(scratch, lhsDest, lhsDest);
}

void MacroAssembler::unsignedExtMulLowInt16x8(FloatRegister rhs,
                                              FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  vmovdqa(lhsDest, scratch);
  vpmullw(Operand(rhs), lhsDest, lhsDest);
  vpmulhuw(Operand(rhs), scratch, scratch);
  vpunpcklwd(scratch, lhsDest, lhsDest);
}

void MacroAssembler::unsignedExtMulHighInt16x8(FloatRegister rhs,
                                               FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  vmovdqa(lhsDest, scratch);
  vpmullw(Operand(rhs), lhsDest, lhsDest);
  vpmulhuw(Operand(rhs), scratch, scratch);
  vpunpckhwd(scratch, lhsDest, lhsDest);
}

void MacroAssembler::extMulLowInt32x4(FloatRegister rhs,
                                      FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  vpshufd(ComputeShuffleMask(0, 0, 1, 0), lhsDest, scratch);
  vpshufd(ComputeShuffleMask(0, 0, 1, 0), rhs, lhsDest);
  vpmuldq(scratch, lhsDest, lhsDest);
}

void MacroAssembler::extMulHighInt32x4(FloatRegister rhs,
                                       FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  vpshufd(ComputeShuffleMask(2, 0, 3, 0), lhsDest, scratch);
  vpshufd(ComputeShuffleMask(2, 0, 3, 0), rhs, lhsDest);
  vpmuldq(scratch, lhsDest, lhsDest);
}

void MacroAssembler::unsignedExtMulLowInt32x4(FloatRegister rhs,
                                              FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  vpshufd(ComputeShuffleMask(0, 0, 1, 0), lhsDest, scratch);
  vpshufd(ComputeShuffleMask(0, 0, 1, 0), rhs, lhsDest);
  vpmuludq(Operand(scratch), lhsDest, lhsDest);
}

void MacroAssembler::unsignedExtMulHighInt32x4(FloatRegister rhs,
                                               FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  vpshufd(ComputeShuffleMask(2, 0, 3, 0), lhsDest, scratch);
  vpshufd(ComputeShuffleMask(2, 0, 3, 0), rhs, lhsDest);
  vpmuludq(Operand(scratch), lhsDest, lhsDest);
}

void MacroAssembler::q15MulrSatInt16x8(FloatRegister rhs,
                                       FloatRegister lhsDest) {
  ScratchSimd128Scope scratch(*this);
  vpmulhrsw(Operand(rhs), lhsDest, lhsDest);
  vmovdqa(lhsDest, scratch);
  vpcmpeqwSimd128(SimdConstant::SplatX8(0x8000), scratch);
  vpxor(scratch, lhsDest, lhsDest);
}

// Integer negate

void MacroAssembler::negInt8x16(FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  if (src == dest) {
    moveSimd128Int(src, scratch);
    src = scratch;
  }
  vpxor(Operand(dest), dest, dest);
  vpsubb(Operand(src), dest, dest);
}

void MacroAssembler::negInt16x8(FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  if (src == dest) {
    moveSimd128Int(src, scratch);
    src = scratch;
  }
  vpxor(Operand(dest), dest, dest);
  vpsubw(Operand(src), dest, dest);
}

void MacroAssembler::negInt32x4(FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  if (src == dest) {
    moveSimd128Int(src, scratch);
    src = scratch;
  }
  vpxor(Operand(dest), dest, dest);
  vpsubd(Operand(src), dest, dest);
}

void MacroAssembler::negInt64x2(FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  if (src == dest) {
    moveSimd128Int(src, scratch);
    src = scratch;
  }
  vpxor(Operand(dest), dest, dest);
  vpsubq(Operand(src), dest, dest);
}

// Saturating integer add

void MacroAssembler::addSatInt8x16(FloatRegister rhs, FloatRegister lhsDest) {
  vpaddsb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::addSatInt8x16(const SimdConstant& rhs,
                                   FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpaddsb,
                &MacroAssembler::vpaddsbSimd128);
}

void MacroAssembler::unsignedAddSatInt8x16(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpaddusb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedAddSatInt8x16(const SimdConstant& rhs,
                                           FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpaddusb,
                &MacroAssembler::vpaddusbSimd128);
}

void MacroAssembler::addSatInt16x8(FloatRegister rhs, FloatRegister lhsDest) {
  vpaddsw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::addSatInt16x8(const SimdConstant& rhs,
                                   FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpaddsw,
                &MacroAssembler::vpaddswSimd128);
}

void MacroAssembler::unsignedAddSatInt16x8(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpaddusw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedAddSatInt16x8(const SimdConstant& rhs,
                                           FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpaddusw,
                &MacroAssembler::vpadduswSimd128);
}

// Saturating integer subtract

void MacroAssembler::subSatInt8x16(FloatRegister rhs, FloatRegister lhsDest) {
  vpsubsb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::subSatInt8x16(const SimdConstant& rhs,
                                   FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpsubsb,
                &MacroAssembler::vpsubsbSimd128);
}

void MacroAssembler::unsignedSubSatInt8x16(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpsubusb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedSubSatInt8x16(const SimdConstant& rhs,
                                           FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpsubusb,
                &MacroAssembler::vpsubusbSimd128);
}

void MacroAssembler::subSatInt16x8(FloatRegister rhs, FloatRegister lhsDest) {
  vpsubsw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::subSatInt16x8(const SimdConstant& rhs,
                                   FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpsubsw,
                &MacroAssembler::vpsubswSimd128);
}

void MacroAssembler::unsignedSubSatInt16x8(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpsubusw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedSubSatInt16x8(const SimdConstant& rhs,
                                           FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpsubusw,
                &MacroAssembler::vpsubuswSimd128);
}

// Lane-wise integer minimum

void MacroAssembler::minInt8x16(FloatRegister rhs, FloatRegister lhsDest) {
  vpminsb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::minInt8x16(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpminsb,
                &MacroAssembler::vpminsbSimd128);
}

void MacroAssembler::unsignedMinInt8x16(FloatRegister rhs,
                                        FloatRegister lhsDest) {
  vpminub(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedMinInt8x16(const SimdConstant& rhs,
                                        FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpminub,
                &MacroAssembler::vpminubSimd128);
}

void MacroAssembler::minInt16x8(FloatRegister rhs, FloatRegister lhsDest) {
  vpminsw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::minInt16x8(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpminsw,
                &MacroAssembler::vpminswSimd128);
}

void MacroAssembler::unsignedMinInt16x8(FloatRegister rhs,
                                        FloatRegister lhsDest) {
  vpminuw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedMinInt16x8(const SimdConstant& rhs,
                                        FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpminuw,
                &MacroAssembler::vpminuwSimd128);
}

void MacroAssembler::minInt32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vpminsd(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::minInt32x4(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpminsd,
                &MacroAssembler::vpminsdSimd128);
}

void MacroAssembler::unsignedMinInt32x4(FloatRegister rhs,
                                        FloatRegister lhsDest) {
  vpminud(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedMinInt32x4(const SimdConstant& rhs,
                                        FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpminud,
                &MacroAssembler::vpminudSimd128);
}

// Lane-wise integer maximum

void MacroAssembler::maxInt8x16(FloatRegister rhs, FloatRegister lhsDest) {
  vpmaxsb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::maxInt8x16(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpmaxsb,
                &MacroAssembler::vpmaxsbSimd128);
}

void MacroAssembler::unsignedMaxInt8x16(FloatRegister rhs,
                                        FloatRegister lhsDest) {
  vpmaxub(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedMaxInt8x16(const SimdConstant& rhs,
                                        FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpmaxub,
                &MacroAssembler::vpmaxubSimd128);
}

void MacroAssembler::maxInt16x8(FloatRegister rhs, FloatRegister lhsDest) {
  vpmaxsw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::maxInt16x8(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpmaxsw,
                &MacroAssembler::vpmaxswSimd128);
}

void MacroAssembler::unsignedMaxInt16x8(FloatRegister rhs,
                                        FloatRegister lhsDest) {
  vpmaxuw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedMaxInt16x8(const SimdConstant& rhs,
                                        FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpmaxuw,
                &MacroAssembler::vpmaxuwSimd128);
}

void MacroAssembler::maxInt32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vpmaxsd(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::maxInt32x4(const SimdConstant& rhs,
                                FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpmaxsd,
                &MacroAssembler::vpmaxsdSimd128);
}

void MacroAssembler::unsignedMaxInt32x4(FloatRegister rhs,
                                        FloatRegister lhsDest) {
  vpmaxud(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedMaxInt32x4(const SimdConstant& rhs,
                                        FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpmaxud,
                &MacroAssembler::vpmaxudSimd128);
}

// Lane-wise integer rounding average

void MacroAssembler::unsignedAverageInt8x16(FloatRegister rhs,
                                            FloatRegister lhsDest) {
  vpavgb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedAverageInt16x8(FloatRegister rhs,
                                            FloatRegister lhsDest) {
  vpavgw(Operand(rhs), lhsDest, lhsDest);
}

// Lane-wise integer absolute value

void MacroAssembler::absInt8x16(FloatRegister src, FloatRegister dest) {
  vpabsb(Operand(src), dest);
}

void MacroAssembler::absInt16x8(FloatRegister src, FloatRegister dest) {
  vpabsw(Operand(src), dest);
}

void MacroAssembler::absInt32x4(FloatRegister src, FloatRegister dest) {
  vpabsd(Operand(src), dest);
}

void MacroAssembler::absInt64x2(FloatRegister src, FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  moveSimd128(src, dest);
  signReplicationInt64x2(src, scratch);
  vpxor(Operand(scratch), dest, dest);
  vpsubq(Operand(scratch), dest, dest);
}

// Left shift by scalar

void MacroAssembler::leftShiftInt8x16(Register rhs, FloatRegister lhsDest,
                                      Register temp1, FloatRegister temp2) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt8x16(lhsDest, rhs, temp1,
                                                          temp2, lhsDest);
}

void MacroAssembler::leftShiftInt8x16(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt8x16(count, src, dest);
}

void MacroAssembler::leftShiftInt16x8(Register rhs, FloatRegister lhsDest,
                                      Register temp) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt16x8(lhsDest, rhs, temp,
                                                          lhsDest);
}

void MacroAssembler::leftShiftInt16x8(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  moveSimd128(src, dest);
  vpsllw(count, src, dest);
}

void MacroAssembler::leftShiftInt32x4(Register rhs, FloatRegister lhsDest,
                                      Register temp) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt32x4(lhsDest, rhs, temp,
                                                          lhsDest);
}

void MacroAssembler::leftShiftInt32x4(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  moveSimd128(src, dest);
  vpslld(count, src, dest);
}

void MacroAssembler::leftShiftInt64x2(Register rhs, FloatRegister lhsDest,
                                      Register temp) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt64x2(lhsDest, rhs, temp,
                                                          lhsDest);
}

void MacroAssembler::leftShiftInt64x2(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  moveSimd128(src, dest);
  vpsllq(count, src, dest);
}

// Right shift by scalar

void MacroAssembler::rightShiftInt8x16(Register rhs, FloatRegister lhsDest,
                                       Register temp1, FloatRegister temp2) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt8x16(lhsDest, rhs, temp1,
                                                           temp2, lhsDest);
}

void MacroAssembler::rightShiftInt8x16(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt8x16(count, src, dest);
}

void MacroAssembler::unsignedRightShiftInt8x16(Register rhs,
                                               FloatRegister lhsDest,
                                               Register temp1,
                                               FloatRegister temp2) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt8x16(
      lhsDest, rhs, temp1, temp2, lhsDest);
}

void MacroAssembler::unsignedRightShiftInt8x16(Imm32 count, FloatRegister src,
                                               FloatRegister dest) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt8x16(count, src,
                                                                   dest);
}

void MacroAssembler::rightShiftInt16x8(Register rhs, FloatRegister lhsDest,
                                       Register temp) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt16x8(lhsDest, rhs, temp,
                                                           lhsDest);
}

void MacroAssembler::rightShiftInt16x8(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  moveSimd128(src, dest);
  vpsraw(count, src, dest);
}

void MacroAssembler::unsignedRightShiftInt16x8(Register rhs,
                                               FloatRegister lhsDest,
                                               Register temp) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt16x8(
      lhsDest, rhs, temp, lhsDest);
}

void MacroAssembler::unsignedRightShiftInt16x8(Imm32 count, FloatRegister src,
                                               FloatRegister dest) {
  moveSimd128(src, dest);
  vpsrlw(count, src, dest);
}

void MacroAssembler::rightShiftInt32x4(Register rhs, FloatRegister lhsDest,
                                       Register temp) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt32x4(lhsDest, rhs, temp,
                                                           lhsDest);
}

void MacroAssembler::rightShiftInt32x4(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  moveSimd128(src, dest);
  vpsrad(count, src, dest);
}

void MacroAssembler::unsignedRightShiftInt32x4(Register rhs,
                                               FloatRegister lhsDest,
                                               Register temp) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt32x4(
      lhsDest, rhs, temp, lhsDest);
}

void MacroAssembler::unsignedRightShiftInt32x4(Imm32 count, FloatRegister src,
                                               FloatRegister dest) {
  moveSimd128(src, dest);
  vpsrld(count, src, dest);
}

void MacroAssembler::rightShiftInt64x2(Register rhs, FloatRegister lhsDest,
                                       Register temp1, FloatRegister temp2) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt64x2(lhsDest, rhs, temp1,
                                                           temp2, lhsDest);
}

void MacroAssembler::rightShiftInt64x2(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt64x2(count, src, dest);
}

void MacroAssembler::unsignedRightShiftInt64x2(Register rhs,
                                               FloatRegister lhsDest,
                                               Register temp) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt64x2(
      lhsDest, rhs, temp, lhsDest);
}

void MacroAssembler::unsignedRightShiftInt64x2(Imm32 count, FloatRegister src,
                                               FloatRegister dest) {
  moveSimd128(src, dest);
  vpsrlq(count, src, dest);
}

// Sign replication operation

void MacroAssembler::signReplicationInt8x16(FloatRegister src,
                                            FloatRegister dest) {
  MOZ_ASSERT(src != dest);
  vpxor(Operand(dest), dest, dest);
  vpcmpgtb(Operand(src), dest, dest);
}

void MacroAssembler::signReplicationInt16x8(FloatRegister src,
                                            FloatRegister dest) {
  moveSimd128(src, dest);
  vpsraw(Imm32(15), dest, dest);
}

void MacroAssembler::signReplicationInt32x4(FloatRegister src,
                                            FloatRegister dest) {
  moveSimd128(src, dest);
  vpsrad(Imm32(31), dest, dest);
}

void MacroAssembler::signReplicationInt64x2(FloatRegister src,
                                            FloatRegister dest) {
  vpshufd(ComputeShuffleMask(1, 1, 3, 3), src, dest);
  vpsrad(Imm32(31), dest, dest);
}

// Bitwise and, or, xor, not

void MacroAssembler::bitwiseAndSimd128(FloatRegister rhs,
                                       FloatRegister lhsDest) {
  vpand(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::bitwiseAndSimd128(const SimdConstant& rhs,
                                       FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpand,
                &MacroAssembler::vpandSimd128);
}

void MacroAssembler::bitwiseOrSimd128(FloatRegister rhs,
                                      FloatRegister lhsDest) {
  vpor(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::bitwiseOrSimd128(const SimdConstant& rhs,
                                      FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpor,
                &MacroAssembler::vporSimd128);
}

void MacroAssembler::bitwiseXorSimd128(FloatRegister rhs,
                                       FloatRegister lhsDest) {
  vpxor(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::bitwiseXorSimd128(const SimdConstant& rhs,
                                       FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpxor,
                &MacroAssembler::vpxorSimd128);
}

void MacroAssembler::bitwiseNotSimd128(FloatRegister src, FloatRegister dest) {
  moveSimd128(src, dest);
  bitwiseXorSimd128(SimdConstant::SplatX16(-1), dest);
}

// Bitwise and-not

void MacroAssembler::bitwiseNotAndSimd128(FloatRegister rhs,
                                          FloatRegister lhsDest) {
  vpandn(Operand(rhs), lhsDest, lhsDest);
}

// Bitwise select

void MacroAssembler::bitwiseSelectSimd128(FloatRegister mask,
                                          FloatRegister onTrue,
                                          FloatRegister onFalse,
                                          FloatRegister dest,
                                          FloatRegister temp) {
  MacroAssemblerX86Shared::selectSimd128(mask, onTrue, onFalse, temp, dest);
}

// Population count

void MacroAssembler::popcntInt8x16(FloatRegister src, FloatRegister dest,
                                   FloatRegister temp) {
  MacroAssemblerX86Shared::popcntInt8x16(src, temp, dest);
}

// Comparisons (integer and floating-point)

void MacroAssembler::compareInt8x16(Assembler::Condition cond,
                                    FloatRegister rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::compareInt8x16(lhsDest, Operand(rhs), cond, lhsDest);
}

void MacroAssembler::compareInt8x16(Assembler::Condition cond,
                                    const SimdConstant& rhs,
                                    FloatRegister lhsDest) {
  MOZ_ASSERT(cond != Assembler::Condition::LessThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareInt8x16(cond, rhs, lhsDest);
}

void MacroAssembler::compareInt16x8(Assembler::Condition cond,
                                    FloatRegister rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::compareInt16x8(lhsDest, Operand(rhs), cond, lhsDest);
}

void MacroAssembler::compareInt16x8(Assembler::Condition cond,
                                    const SimdConstant& rhs,
                                    FloatRegister lhsDest) {
  MOZ_ASSERT(cond != Assembler::Condition::LessThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareInt16x8(cond, rhs, lhsDest);
}

void MacroAssembler::compareInt32x4(Assembler::Condition cond,
                                    FloatRegister rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::compareInt32x4(lhsDest, Operand(rhs), cond, lhsDest);
}

void MacroAssembler::compareInt32x4(Assembler::Condition cond,
                                    const SimdConstant& rhs,
                                    FloatRegister lhsDest) {
  MOZ_ASSERT(cond != Assembler::Condition::LessThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareInt32x4(cond, rhs, lhsDest);
}

void MacroAssembler::compareForEqualityInt64x2(Assembler::Condition cond,
                                               FloatRegister rhs,
                                               FloatRegister lhsDest) {
  MacroAssemblerX86Shared::compareForEqualityInt64x2(lhsDest, Operand(rhs),
                                                     cond, lhsDest);
}

void MacroAssembler::compareForOrderingInt64x2(Assembler::Condition cond,
                                               FloatRegister rhs,
                                               FloatRegister lhsDest,
                                               FloatRegister temp1,
                                               FloatRegister temp2) {
  MacroAssemblerX86Shared::compareForOrderingInt64x2(
      lhsDest, Operand(rhs), cond, temp1, temp2, lhsDest);
}

void MacroAssembler::compareFloat32x4(Assembler::Condition cond,
                                      FloatRegister rhs,
                                      FloatRegister lhsDest) {
  // Code in the SIMD implementation allows operands to be reversed like this,
  // this benefits the baseline compiler.  Ion takes care of the reversing
  // itself and never generates GT/GE.
  if (cond == Assembler::GreaterThan) {
    MacroAssemblerX86Shared::compareFloat32x4(rhs, Operand(lhsDest),
                                              Assembler::LessThan, lhsDest);
  } else if (cond == Assembler::GreaterThanOrEqual) {
    MacroAssemblerX86Shared::compareFloat32x4(
        rhs, Operand(lhsDest), Assembler::LessThanOrEqual, lhsDest);
  } else {
    MacroAssemblerX86Shared::compareFloat32x4(lhsDest, Operand(rhs), cond,
                                              lhsDest);
  }
}

void MacroAssembler::compareFloat32x4(Assembler::Condition cond,
                                      const SimdConstant& rhs,
                                      FloatRegister lhsDest) {
  MOZ_ASSERT(cond != Assembler::Condition::GreaterThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareFloat32x4(cond, rhs, lhsDest);
}

void MacroAssembler::compareFloat64x2(Assembler::Condition cond,
                                      FloatRegister rhs,
                                      FloatRegister lhsDest) {
  // Code in the SIMD implementation allows operands to be reversed like this,
  // this benefits the baseline compiler.  Ion takes care of the reversing
  // itself and never generates GT/GE.
  if (cond == Assembler::GreaterThan) {
    MacroAssemblerX86Shared::compareFloat64x2(rhs, Operand(lhsDest),
                                              Assembler::LessThan, lhsDest);
  } else if (cond == Assembler::GreaterThanOrEqual) {
    MacroAssemblerX86Shared::compareFloat64x2(
        rhs, Operand(lhsDest), Assembler::LessThanOrEqual, lhsDest);
  } else {
    MacroAssemblerX86Shared::compareFloat64x2(lhsDest, Operand(rhs), cond,
                                              lhsDest);
  }
}

void MacroAssembler::compareFloat64x2(Assembler::Condition cond,
                                      const SimdConstant& rhs,
                                      FloatRegister lhsDest) {
  MOZ_ASSERT(cond != Assembler::Condition::GreaterThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareFloat64x2(cond, rhs, lhsDest);
}

// Load.  See comments above regarding integer operation.

void MacroAssembler::loadUnalignedSimd128(const Operand& src,
                                          FloatRegister dest) {
  loadUnalignedSimd128Int(src, dest);
}

void MacroAssembler::loadUnalignedSimd128(const Address& src,
                                          FloatRegister dest) {
  loadUnalignedSimd128Int(src, dest);
}

void MacroAssembler::loadUnalignedSimd128(const BaseIndex& src,
                                          FloatRegister dest) {
  loadUnalignedSimd128Int(src, dest);
}

// Store.  See comments above regarding integer operation.

void MacroAssembler::storeUnalignedSimd128(FloatRegister src,
                                           const Address& dest) {
  storeUnalignedSimd128Int(src, dest);
}

void MacroAssembler::storeUnalignedSimd128(FloatRegister src,
                                           const BaseIndex& dest) {
  storeUnalignedSimd128Int(src, dest);
}

// Floating point negation

void MacroAssembler::negFloat32x4(FloatRegister src, FloatRegister dest) {
  moveSimd128(src, dest);
  bitwiseXorSimd128(SimdConstant::SplatX4(-0.f), dest);
}

void MacroAssembler::negFloat64x2(FloatRegister src, FloatRegister dest) {
  moveSimd128(src, dest);
  bitwiseXorSimd128(SimdConstant::SplatX2(-0.0), dest);
}

// Floating point absolute value

void MacroAssembler::absFloat32x4(FloatRegister src, FloatRegister dest) {
  moveSimd128(src, dest);
  bitwiseAndSimd128(SimdConstant::SplatX4(0x7FFFFFFF), dest);
}

void MacroAssembler::absFloat64x2(FloatRegister src, FloatRegister dest) {
  moveSimd128(src, dest);
  bitwiseAndSimd128(SimdConstant::SplatX2(int64_t(0x7FFFFFFFFFFFFFFFll)), dest);
}

// NaN-propagating minimum

void MacroAssembler::minFloat32x4(FloatRegister rhs, FloatRegister lhsDest,
                                  FloatRegister temp1, FloatRegister temp2) {
  MacroAssemblerX86Shared::minFloat32x4(lhsDest, Operand(rhs), temp1, temp2,
                                        lhsDest);
}

void MacroAssembler::minFloat64x2(FloatRegister rhs, FloatRegister lhsDest,
                                  FloatRegister temp1, FloatRegister temp2) {
  MacroAssemblerX86Shared::minFloat64x2(lhsDest, Operand(rhs), temp1, temp2,
                                        lhsDest);
}

// NaN-propagating maximum

void MacroAssembler::maxFloat32x4(FloatRegister rhs, FloatRegister lhsDest,
                                  FloatRegister temp1, FloatRegister temp2) {
  MacroAssemblerX86Shared::maxFloat32x4(lhsDest, Operand(rhs), temp1, temp2,
                                        lhsDest);
}

void MacroAssembler::maxFloat64x2(FloatRegister rhs, FloatRegister lhsDest,
                                  FloatRegister temp1, FloatRegister temp2) {
  MacroAssemblerX86Shared::maxFloat64x2(lhsDest, Operand(rhs), temp1, temp2,
                                        lhsDest);
}

// Compare-based minimum

void MacroAssembler::pseudoMinFloat32x4(FloatRegister rhsOrRhsDest,
                                        FloatRegister lhsOrLhsDest) {
  // Shut up the linter by using the same names as in the declaration, then
  // aliasing here.
  FloatRegister rhsDest = rhsOrRhsDest;
  FloatRegister lhs = lhsOrLhsDest;
  vminps(Operand(lhs), rhsDest, rhsDest);
}

void MacroAssembler::pseudoMinFloat64x2(FloatRegister rhsOrRhsDest,
                                        FloatRegister lhsOrLhsDest) {
  FloatRegister rhsDest = rhsOrRhsDest;
  FloatRegister lhs = lhsOrLhsDest;
  vminpd(Operand(lhs), rhsDest, rhsDest);
}

// Compare-based maximum

void MacroAssembler::pseudoMaxFloat32x4(FloatRegister rhsOrRhsDest,
                                        FloatRegister lhsOrLhsDest) {
  FloatRegister rhsDest = rhsOrRhsDest;
  FloatRegister lhs = lhsOrLhsDest;
  vmaxps(Operand(lhs), rhsDest, rhsDest);
}

void MacroAssembler::pseudoMaxFloat64x2(FloatRegister rhsOrRhsDest,
                                        FloatRegister lhsOrLhsDest) {
  FloatRegister rhsDest = rhsOrRhsDest;
  FloatRegister lhs = lhsOrLhsDest;
  vmaxpd(Operand(lhs), rhsDest, rhsDest);
}

// Widening/pairwise integer dot product

void MacroAssembler::widenDotInt16x8(FloatRegister rhs, FloatRegister lhsDest) {
  vpmaddwd(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::widenDotInt16x8(const SimdConstant& rhs,
                                     FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpmaddwd,
                &MacroAssembler::vpmaddwdSimd128);
}

// Rounding

void MacroAssembler::ceilFloat32x4(FloatRegister src, FloatRegister dest) {
  vroundps(Assembler::SSERoundingMode::Ceil, Operand(src), dest);
}

void MacroAssembler::ceilFloat64x2(FloatRegister src, FloatRegister dest) {
  vroundpd(Assembler::SSERoundingMode::Ceil, Operand(src), dest);
}

void MacroAssembler::floorFloat32x4(FloatRegister src, FloatRegister dest) {
  vroundps(Assembler::SSERoundingMode::Floor, Operand(src), dest);
}

void MacroAssembler::floorFloat64x2(FloatRegister src, FloatRegister dest) {
  vroundpd(Assembler::SSERoundingMode::Floor, Operand(src), dest);
}

void MacroAssembler::truncFloat32x4(FloatRegister src, FloatRegister dest) {
  vroundps(Assembler::SSERoundingMode::Trunc, Operand(src), dest);
}

void MacroAssembler::truncFloat64x2(FloatRegister src, FloatRegister dest) {
  vroundpd(Assembler::SSERoundingMode::Trunc, Operand(src), dest);
}

void MacroAssembler::nearestFloat32x4(FloatRegister src, FloatRegister dest) {
  vroundps(Assembler::SSERoundingMode::Nearest, Operand(src), dest);
}

void MacroAssembler::nearestFloat64x2(FloatRegister src, FloatRegister dest) {
  vroundpd(Assembler::SSERoundingMode::Nearest, Operand(src), dest);
}

// Floating add

void MacroAssembler::addFloat32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vaddps(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::addFloat32x4(const SimdConstant& rhs,
                                  FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vaddps,
                &MacroAssembler::vaddpsSimd128);
}

void MacroAssembler::addFloat64x2(FloatRegister rhs, FloatRegister lhsDest) {
  vaddpd(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::addFloat64x2(const SimdConstant& rhs,
                                  FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vaddpd,
                &MacroAssembler::vaddpdSimd128);
}

// Floating subtract

void MacroAssembler::subFloat32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vsubps(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::subFloat32x4(const SimdConstant& rhs,
                                  FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vsubps,
                &MacroAssembler::vsubpsSimd128);
}

void MacroAssembler::subFloat64x2(FloatRegister rhs, FloatRegister lhsDest) {
  AssemblerX86Shared::vsubpd(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::subFloat64x2(const SimdConstant& rhs,
                                  FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vsubpd,
                &MacroAssembler::vsubpdSimd128);
}

// Floating division

void MacroAssembler::divFloat32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vdivps(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::divFloat32x4(const SimdConstant& rhs,
                                  FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vdivps,
                &MacroAssembler::vdivpsSimd128);
}

void MacroAssembler::divFloat64x2(FloatRegister rhs, FloatRegister lhsDest) {
  vdivpd(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::divFloat64x2(const SimdConstant& rhs,
                                  FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vdivpd,
                &MacroAssembler::vdivpdSimd128);
}

// Floating Multiply

void MacroAssembler::mulFloat32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vmulps(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::mulFloat32x4(const SimdConstant& rhs,
                                  FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vmulps,
                &MacroAssembler::vmulpsSimd128);
}

void MacroAssembler::mulFloat64x2(FloatRegister rhs, FloatRegister lhsDest) {
  vmulpd(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::mulFloat64x2(const SimdConstant& rhs,
                                  FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vmulpd,
                &MacroAssembler::vmulpdSimd128);
}

// Pairwise add

void MacroAssembler::extAddPairwiseInt8x16(FloatRegister src,
                                           FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  if (dest == src) {
    moveSimd128(src, scratch);
    src = scratch;
  }
  loadConstantSimd128Int(SimdConstant::SplatX16(1), dest);
  vpmaddubsw(src, dest, dest);
}

void MacroAssembler::unsignedExtAddPairwiseInt8x16(FloatRegister src,
                                                   FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  moveSimd128(src, dest);
  loadConstantSimd128Int(SimdConstant::SplatX16(1), scratch);
  vpmaddubsw(scratch, dest, dest);
}

void MacroAssembler::extAddPairwiseInt16x8(FloatRegister src,
                                           FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  moveSimd128(src, dest);
  loadConstantSimd128Int(SimdConstant::SplatX8(1), scratch);
  vpmaddwd(Operand(scratch), dest, dest);
}

void MacroAssembler::unsignedExtAddPairwiseInt16x8(FloatRegister src,
                                                   FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  moveSimd128(src, dest);
  loadConstantSimd128Int(SimdConstant::SplatX8(0x8000), scratch);
  vpxor(scratch, dest, dest);
  loadConstantSimd128Int(SimdConstant::SplatX8(1), scratch);
  vpmaddwd(Operand(scratch), dest, dest);
  loadConstantSimd128Int(SimdConstant::SplatX4(0x00010000), scratch);
  vpaddd(Operand(scratch), dest, dest);
}

// Floating square root

void MacroAssembler::sqrtFloat32x4(FloatRegister src, FloatRegister dest) {
  vsqrtps(Operand(src), dest);
}

void MacroAssembler::sqrtFloat64x2(FloatRegister src, FloatRegister dest) {
  vsqrtpd(Operand(src), dest);
}

// Integer to floating point with rounding

void MacroAssembler::convertInt32x4ToFloat32x4(FloatRegister src,
                                               FloatRegister dest) {
  vcvtdq2ps(src, dest);
}

void MacroAssembler::unsignedConvertInt32x4ToFloat32x4(FloatRegister src,
                                                       FloatRegister dest) {
  MacroAssemblerX86Shared::unsignedConvertInt32x4ToFloat32x4(src, dest);
}

void MacroAssembler::convertInt32x4ToFloat64x2(FloatRegister src,
                                               FloatRegister dest) {
  vcvtdq2pd(src, dest);
}

void MacroAssembler::unsignedConvertInt32x4ToFloat64x2(FloatRegister src,
                                                       FloatRegister dest) {
  MacroAssemblerX86Shared::unsignedConvertInt32x4ToFloat64x2(src, dest);
}

// Floating point to integer with saturation

void MacroAssembler::truncSatFloat32x4ToInt32x4(FloatRegister src,
                                                FloatRegister dest) {
  MacroAssemblerX86Shared::truncSatFloat32x4ToInt32x4(src, dest);
}

void MacroAssembler::unsignedTruncSatFloat32x4ToInt32x4(FloatRegister src,
                                                        FloatRegister dest,
                                                        FloatRegister temp) {
  MacroAssemblerX86Shared::unsignedTruncSatFloat32x4ToInt32x4(src, temp, dest);
}

void MacroAssembler::truncSatFloat64x2ToInt32x4(FloatRegister src,
                                                FloatRegister dest,
                                                FloatRegister temp) {
  MacroAssemblerX86Shared::truncSatFloat64x2ToInt32x4(src, temp, dest);
}

void MacroAssembler::unsignedTruncSatFloat64x2ToInt32x4(FloatRegister src,
                                                        FloatRegister dest,
                                                        FloatRegister temp) {
  MacroAssemblerX86Shared::unsignedTruncSatFloat64x2ToInt32x4(src, temp, dest);
}

// Floating point widening

void MacroAssembler::convertFloat64x2ToFloat32x4(FloatRegister src,
                                                 FloatRegister dest) {
  vcvtpd2ps(src, dest);
}

void MacroAssembler::convertFloat32x4ToFloat64x2(FloatRegister src,
                                                 FloatRegister dest) {
  vcvtps2pd(src, dest);
}

// Integer to integer narrowing

void MacroAssembler::narrowInt16x8(FloatRegister rhs, FloatRegister lhsDest) {
  vpacksswb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::narrowInt16x8(const SimdConstant& rhs,
                                   FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpacksswb,
                &MacroAssembler::vpacksswbSimd128);
}

void MacroAssembler::unsignedNarrowInt16x8(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpackuswb(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedNarrowInt16x8(const SimdConstant& rhs,
                                           FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpackuswb,
                &MacroAssembler::vpackuswbSimd128);
}

void MacroAssembler::narrowInt32x4(FloatRegister rhs, FloatRegister lhsDest) {
  vpackssdw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::narrowInt32x4(const SimdConstant& rhs,
                                   FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpackssdw,
                &MacroAssembler::vpackssdwSimd128);
}

void MacroAssembler::unsignedNarrowInt32x4(FloatRegister rhs,
                                           FloatRegister lhsDest) {
  vpackusdw(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::unsignedNarrowInt32x4(const SimdConstant& rhs,
                                           FloatRegister lhsDest) {
  binarySimd128(rhs, lhsDest, &MacroAssembler::vpackusdw,
                &MacroAssembler::vpackusdwSimd128);
}

// Integer to integer widening

void MacroAssembler::widenLowInt8x16(FloatRegister src, FloatRegister dest) {
  vpmovsxbw(Operand(src), dest);
}

void MacroAssembler::widenHighInt8x16(FloatRegister src, FloatRegister dest) {
  vpalignr(Operand(src), dest, 8);
  vpmovsxbw(Operand(dest), dest);
}

void MacroAssembler::unsignedWidenLowInt8x16(FloatRegister src,
                                             FloatRegister dest) {
  vpmovzxbw(Operand(src), dest);
}

void MacroAssembler::unsignedWidenHighInt8x16(FloatRegister src,
                                              FloatRegister dest) {
  vpalignr(Operand(src), dest, 8);
  vpmovzxbw(Operand(dest), dest);
}

void MacroAssembler::widenLowInt16x8(FloatRegister src, FloatRegister dest) {
  vpmovsxwd(Operand(src), dest);
}

void MacroAssembler::widenHighInt16x8(FloatRegister src, FloatRegister dest) {
  vpalignr(Operand(src), dest, 8);
  vpmovsxwd(Operand(dest), dest);
}

void MacroAssembler::unsignedWidenLowInt16x8(FloatRegister src,
                                             FloatRegister dest) {
  vpmovzxwd(Operand(src), dest);
}

void MacroAssembler::unsignedWidenHighInt16x8(FloatRegister src,
                                              FloatRegister dest) {
  vpalignr(Operand(src), dest, 8);
  vpmovzxwd(Operand(dest), dest);
}

void MacroAssembler::widenLowInt32x4(FloatRegister src, FloatRegister dest) {
  vpmovsxdq(Operand(src), dest);
}

void MacroAssembler::unsignedWidenLowInt32x4(FloatRegister src,
                                             FloatRegister dest) {
  vpmovzxdq(Operand(src), dest);
}

void MacroAssembler::widenHighInt32x4(FloatRegister src, FloatRegister dest) {
  vpshufd(ComputeShuffleMask(2, 3, 2, 3), src, dest);
  vpmovsxdq(Operand(dest), dest);
}

void MacroAssembler::unsignedWidenHighInt32x4(FloatRegister src,
                                              FloatRegister dest) {
  vpshufd(ComputeShuffleMask(2, 3, 2, 3), src, dest);
  vpmovzxdq(Operand(dest), dest);
}

// ========================================================================
// Truncate floating point.

void MacroAssembler::truncateFloat32ToInt64(Address src, Address dest,
                                            Register temp) {
  if (Assembler::HasSSE3()) {
    fld32(Operand(src));
    fisttp(Operand(dest));
    return;
  }

  if (src.base == esp) {
    src.offset += 2 * sizeof(int32_t);
  }
  if (dest.base == esp) {
    dest.offset += 2 * sizeof(int32_t);
  }

  reserveStack(2 * sizeof(int32_t));

  // Set conversion to truncation.
  fnstcw(Operand(esp, 0));
  load32(Operand(esp, 0), temp);
  andl(Imm32(~0xFF00), temp);
  orl(Imm32(0xCFF), temp);
  store32(temp, Address(esp, sizeof(int32_t)));
  fldcw(Operand(esp, sizeof(int32_t)));

  // Load double on fp stack, convert and load regular stack.
  fld32(Operand(src));
  fistp(Operand(dest));

  // Reset the conversion flag.
  fldcw(Operand(esp, 0));

  freeStack(2 * sizeof(int32_t));
}
void MacroAssembler::truncateDoubleToInt64(Address src, Address dest,
                                           Register temp) {
  if (Assembler::HasSSE3()) {
    fld(Operand(src));
    fisttp(Operand(dest));
    return;
  }

  if (src.base == esp) {
    src.offset += 2 * sizeof(int32_t);
  }
  if (dest.base == esp) {
    dest.offset += 2 * sizeof(int32_t);
  }

  reserveStack(2 * sizeof(int32_t));

  // Set conversion to truncation.
  fnstcw(Operand(esp, 0));
  load32(Operand(esp, 0), temp);
  andl(Imm32(~0xFF00), temp);
  orl(Imm32(0xCFF), temp);
  store32(temp, Address(esp, 1 * sizeof(int32_t)));
  fldcw(Operand(esp, 1 * sizeof(int32_t)));

  // Load double on fp stack, convert and load regular stack.
  fld(Operand(src));
  fistp(Operand(dest));

  // Reset the conversion flag.
  fldcw(Operand(esp, 0));

  freeStack(2 * sizeof(int32_t));
}

// ===============================================================
// Clamping functions.

void MacroAssembler::clampIntToUint8(Register reg) {
  Label inRange;
  branchTest32(Assembler::Zero, reg, Imm32(0xffffff00), &inRange);
  {
    sarl(Imm32(31), reg);
    notl(reg);
    andl(Imm32(255), reg);
  }
  bind(&inRange);
}

//}}} check_macroassembler_style
// ===============================================================

}  // namespace jit
}  // namespace js

#endif /* jit_x86_shared_MacroAssembler_x86_shared_inl_h */
