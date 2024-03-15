/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_MacroAssembler_x86_shared_inl_h
#define jit_x86_shared_MacroAssembler_x86_shared_inl_h

#include "jit/x86-shared/MacroAssembler-x86-shared.h"

#include "mozilla/MathAlgorithms.h"

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

void MacroAssembler::mul32(Imm32 imm, Register srcDest) { imull(imm, srcDest); }

void MacroAssembler::mulFloat32(FloatRegister src, FloatRegister dest) {
  vmulss(src, dest, dest);
}

void MacroAssembler::mulDouble(FloatRegister src, FloatRegister dest) {
  vmulsd(src, dest, dest);
}

void MacroAssembler::quotient32(Register rhs, Register srcDest,
                                Register tempEdx, bool isUnsigned) {
  MOZ_ASSERT(srcDest == eax && tempEdx == edx);

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
                                 Register tempEdx, bool isUnsigned) {
  MOZ_ASSERT(srcDest == eax && tempEdx == edx);

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

void MacroAssembler::cmp8Set(Condition cond, Address lhs, Imm32 rhs,
                             Register dest) {
  cmp8(lhs, rhs);
  emitSet(cond, dest);
}

void MacroAssembler::cmp16Set(Condition cond, Address lhs, Imm32 rhs,
                              Register dest) {
  cmp16(lhs, rhs);
  emitSet(cond, dest);
}

template <typename T1, typename T2>
void MacroAssembler::cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest) {
  cmp32(lhs, rhs);
  emitSet(cond, dest);
}

// ===============================================================
// Branch instructions

void MacroAssembler::branch8(Condition cond, const Address& lhs, Imm32 rhs,
                             Label* label) {
  cmp8(lhs, rhs);
  j(cond, label);
}

void MacroAssembler::branch8(Condition cond, const BaseIndex& lhs, Register rhs,
                             Label* label) {
  cmp8(Operand(lhs), rhs);
  j(cond, label);
}

void MacroAssembler::branch16(Condition cond, const Address& lhs, Imm32 rhs,
                              Label* label) {
  cmp16(lhs, rhs);
  j(cond, label);
}

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
// application.  This applies to moveSimd128, loadConstantSimd128,
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

void MacroAssembler::replaceLaneInt8x16(unsigned lane, FloatRegister lhs,
                                        Register rhs, FloatRegister dest) {
  vpinsrb(lane, Operand(rhs), lhs, dest);
}

void MacroAssembler::replaceLaneInt8x16(unsigned lane, Register rhs,
                                        FloatRegister lhsDest) {
  vpinsrb(lane, Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::replaceLaneInt16x8(unsigned lane, FloatRegister lhs,
                                        Register rhs, FloatRegister dest) {
  vpinsrw(lane, Operand(rhs), lhs, dest);
}

void MacroAssembler::replaceLaneInt16x8(unsigned lane, Register rhs,
                                        FloatRegister lhsDest) {
  vpinsrw(lane, Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::replaceLaneInt32x4(unsigned lane, FloatRegister lhs,
                                        Register rhs, FloatRegister dest) {
  vpinsrd(lane, rhs, lhs, dest);
}

void MacroAssembler::replaceLaneInt32x4(unsigned lane, Register rhs,
                                        FloatRegister lhsDest) {
  vpinsrd(lane, rhs, lhsDest, lhsDest);
}

void MacroAssembler::replaceLaneFloat32x4(unsigned lane, FloatRegister lhs,
                                          FloatRegister rhs,
                                          FloatRegister dest) {
  MacroAssemblerX86Shared::replaceLaneFloat32x4(lane, lhs, rhs, dest);
}

void MacroAssembler::replaceLaneFloat32x4(unsigned lane, FloatRegister rhs,
                                          FloatRegister lhsDest) {
  MacroAssemblerX86Shared::replaceLaneFloat32x4(lane, lhsDest, rhs, lhsDest);
}

void MacroAssembler::replaceLaneFloat64x2(unsigned lane, FloatRegister lhs,
                                          FloatRegister rhs,
                                          FloatRegister dest) {
  MacroAssemblerX86Shared::replaceLaneFloat64x2(lane, lhs, rhs, dest);
}

void MacroAssembler::replaceLaneFloat64x2(unsigned lane, FloatRegister rhs,
                                          FloatRegister lhsDest) {
  MacroAssemblerX86Shared::replaceLaneFloat64x2(lane, lhsDest, rhs, lhsDest);
}

// Shuffle - permute with immediate indices

void MacroAssembler::shuffleInt8x16(const uint8_t lanes[16], FloatRegister rhs,
                                    FloatRegister lhsDest) {
  MacroAssemblerX86Shared::shuffleInt8x16(lhsDest, rhs, lhsDest, lanes);
}

void MacroAssembler::shuffleInt8x16(const uint8_t lanes[16], FloatRegister lhs,
                                    FloatRegister rhs, FloatRegister dest) {
  MacroAssemblerX86Shared::shuffleInt8x16(lhs, rhs, dest, lanes);
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

void MacroAssembler::laneSelectSimd128(FloatRegister mask, FloatRegister lhs,
                                       FloatRegister rhs, FloatRegister dest) {
  MacroAssemblerX86Shared::laneSelectSimd128(mask, lhs, rhs, dest);
}

void MacroAssembler::interleaveHighInt16x8(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpunpckhwd(rhs, lhs, dest);
}

void MacroAssembler::interleaveHighInt32x4(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpunpckhdq(rhs, lhs, dest);
}

void MacroAssembler::interleaveHighInt64x2(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpunpckhqdq(rhs, lhs, dest);
}

void MacroAssembler::interleaveHighInt8x16(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpunpckhbw(rhs, lhs, dest);
}

void MacroAssembler::interleaveLowInt16x8(FloatRegister lhs, FloatRegister rhs,
                                          FloatRegister dest) {
  vpunpcklwd(rhs, lhs, dest);
}

void MacroAssembler::interleaveLowInt32x4(FloatRegister lhs, FloatRegister rhs,
                                          FloatRegister dest) {
  vpunpckldq(rhs, lhs, dest);
}

void MacroAssembler::interleaveLowInt64x2(FloatRegister lhs, FloatRegister rhs,
                                          FloatRegister dest) {
  vpunpcklqdq(rhs, lhs, dest);
}

void MacroAssembler::interleaveLowInt8x16(FloatRegister lhs, FloatRegister rhs,
                                          FloatRegister dest) {
  vpunpcklbw(rhs, lhs, dest);
}

void MacroAssembler::permuteInt8x16(const uint8_t lanes[16], FloatRegister src,
                                    FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpshufbSimd128(SimdConstant::CreateX16((const int8_t*)lanes), src, dest);
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

void MacroAssembler::concatAndRightShiftSimd128(FloatRegister lhs,
                                                FloatRegister rhs,
                                                FloatRegister dest,
                                                uint32_t shift) {
  vpalignr(Operand(rhs), lhs, dest, shift);
}

void MacroAssembler::leftShiftSimd128(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpslldq(count, src, dest);
}

void MacroAssembler::rightShiftSimd128(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsrldq(count, src, dest);
}

// Reverse bytes in lanes.

void MacroAssembler::reverseInt16x8(FloatRegister src, FloatRegister dest) {
  // Byteswap is MOV + PSLLW + PSRLW + POR, a small win over PSHUFB.
  ScratchSimd128Scope scratch(*this);
  FloatRegister srcForScratch = moveSimd128IntIfNotAVX(src, scratch);
  vpsrlw(Imm32(8), srcForScratch, scratch);
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsllw(Imm32(8), src, dest);
  vpor(scratch, dest, dest);
}

void MacroAssembler::reverseInt32x4(FloatRegister src, FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  int8_t lanes[] = {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12};
  vpshufbSimd128(SimdConstant::CreateX16((const int8_t*)lanes), src, dest);
}

void MacroAssembler::reverseInt64x2(FloatRegister src, FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  int8_t lanes[] = {7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8};
  vpshufbSimd128(SimdConstant::CreateX16((const int8_t*)lanes), src, dest);
}

// Any lane true, ie any bit set

void MacroAssembler::anyTrueSimd128(FloatRegister src, Register dest) {
  vptest(src, src);
  emitSetRegisterIf(Condition::NonZero, dest);
}

// All lanes true

void MacroAssembler::allTrueInt8x16(FloatRegister src, Register dest) {
  ScratchSimd128Scope xtmp(*this);
  // xtmp is all-00h
  vpxor(xtmp, xtmp, xtmp);
  // Set FFh if byte==0 otherwise 00h
  // Operand ordering constraint: lhs==output
  vpcmpeqb(Operand(src), xtmp, xtmp);
  // Check if xtmp is 0.
  vptest(xtmp, xtmp);
  emitSetRegisterIf(Condition::Zero, dest);
}

void MacroAssembler::allTrueInt16x8(FloatRegister src, Register dest) {
  ScratchSimd128Scope xtmp(*this);
  // xtmp is all-00h
  vpxor(xtmp, xtmp, xtmp);
  // Set FFFFh if word==0 otherwise 0000h
  // Operand ordering constraint: lhs==output
  vpcmpeqw(Operand(src), xtmp, xtmp);
  // Check if xtmp is 0.
  vptest(xtmp, xtmp);
  emitSetRegisterIf(Condition::Zero, dest);
}

void MacroAssembler::allTrueInt32x4(FloatRegister src, Register dest) {
  ScratchSimd128Scope xtmp(*this);
  // xtmp is all-00h
  vpxor(xtmp, xtmp, xtmp);
  // Set FFFFFFFFh if doubleword==0 otherwise 00000000h
  // Operand ordering constraint: lhs==output
  vpcmpeqd(Operand(src), xtmp, xtmp);
  // Check if xtmp is 0.
  vptest(xtmp, xtmp);
  emitSetRegisterIf(Condition::Zero, dest);
}

void MacroAssembler::allTrueInt64x2(FloatRegister src, Register dest) {
  ScratchSimd128Scope xtmp(*this);
  // xtmp is all-00h
  vpxor(xtmp, xtmp, xtmp);
  // Set FFFFFFFFFFFFFFFFh if quadword==0 otherwise 0000000000000000h
  // Operand ordering constraint: lhs==output
  vpcmpeqq(Operand(src), xtmp, xtmp);
  // Check if xtmp is 0.
  vptest(xtmp, xtmp);
  emitSetRegisterIf(Condition::Zero, dest);
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
  src = moveSimd128IntIfNotAVX(src, scratch);
  vpacksswb(Operand(src), src, scratch);
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

void MacroAssembler::swizzleInt8x16(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  rhs = moveSimd128IntIfNotAVX(rhs, scratch);
  // Set high bit to 1 for values > 15 via adding with saturation.
  vpaddusbSimd128(SimdConstant::SplatX16(0x70), rhs, scratch);
  vpshufb(scratch, lhs, dest);  // permute
}

void MacroAssembler::swizzleInt8x16Relaxed(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpshufb(rhs, lhs, dest);
}

// Integer Add

void MacroAssembler::addInt8x16(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpaddb(Operand(rhs), lhs, dest);
}

void MacroAssembler::addInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpaddb,
                &MacroAssembler::vpaddbSimd128);
}

void MacroAssembler::addInt16x8(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpaddw(Operand(rhs), lhs, dest);
}

void MacroAssembler::addInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpaddw,
                &MacroAssembler::vpaddwSimd128);
}

void MacroAssembler::addInt32x4(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpaddd(Operand(rhs), lhs, dest);
}

void MacroAssembler::addInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpaddd,
                &MacroAssembler::vpadddSimd128);
}

void MacroAssembler::addInt64x2(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpaddq(Operand(rhs), lhs, dest);
}

void MacroAssembler::addInt64x2(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpaddq,
                &MacroAssembler::vpaddqSimd128);
}

// Integer subtract

void MacroAssembler::subInt8x16(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpsubb(Operand(rhs), lhs, dest);
}

void MacroAssembler::subInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpsubb,
                &MacroAssembler::vpsubbSimd128);
}

void MacroAssembler::subInt16x8(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpsubw(Operand(rhs), lhs, dest);
}

void MacroAssembler::subInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpsubw,
                &MacroAssembler::vpsubwSimd128);
}

void MacroAssembler::subInt32x4(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpsubd(Operand(rhs), lhs, dest);
}

void MacroAssembler::subInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpsubd,
                &MacroAssembler::vpsubdSimd128);
}

void MacroAssembler::subInt64x2(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpsubq(Operand(rhs), lhs, dest);
}

void MacroAssembler::subInt64x2(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpsubq,
                &MacroAssembler::vpsubqSimd128);
}

// Integer multiply

void MacroAssembler::mulInt16x8(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpmullw(Operand(rhs), lhs, dest);
}

void MacroAssembler::mulInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpmullw,
                &MacroAssembler::vpmullwSimd128);
}

void MacroAssembler::mulInt32x4(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpmulld(Operand(rhs), lhs, dest);
}

void MacroAssembler::mulInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpmulld,
                &MacroAssembler::vpmulldSimd128);
}

void MacroAssembler::mulInt64x2(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest, FloatRegister temp) {
  ScratchSimd128Scope temp2(*this);
  // lhs = <D C> <B A>
  // rhs = <H G> <F E>
  // result = <(DG+CH)_low+CG_high CG_low> <(BE+AF)_low+AE_high AE_low>
  FloatRegister lhsForTemp =
      moveSimd128IntIfNotAVX(lhs, temp);  // temp  = <D C> <B A>
  vpsrlq(Imm32(32), lhsForTemp, temp);    // temp  = <0 D> <0 B>
  vpmuludq(rhs, temp, temp);              // temp  = <DG> <BE>
  FloatRegister rhsForTemp =
      moveSimd128IntIfNotAVX(rhs, temp2);  // temp2 = <H G> <F E>
  vpsrlq(Imm32(32), rhsForTemp, temp2);    // temp2 = <0 H> <0 F>
  vpmuludq(lhs, temp2, temp2);             // temp2 = <CH> <AF>
  vpaddq(Operand(temp), temp2, temp2);     // temp2 = <DG+CH> <BE+AF>
  vpsllq(Imm32(32), temp2, temp2);         // temp2 = <(DG+CH)_low 0>
                                           //         <(BE+AF)_low 0>
  vpmuludq(rhs, lhs, dest);                // dest = <CG_high CG_low>
                                           //        <AE_high AE_low>
  vpaddq(Operand(temp2), dest, dest);      // dest =
                                           //    <(DG+CH)_low+CG_high CG_low>
                                           //    <(BE+AF)_low+AE_high AE_low>
}

void MacroAssembler::mulInt64x2(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest, FloatRegister temp) {
  // Check if we can specialize that to less than eight instructions
  // (in comparison with the above mulInt64x2 version).
  const int64_t* c = static_cast<const int64_t*>(rhs.bytes());
  const int64_t val = c[0];
  if (val == c[1]) {
    switch (mozilla::CountPopulation64(val)) {
      case 0:  // val == 0
        vpxor(Operand(dest), dest, dest);
        return;
      case 64:  // val == -1
        negInt64x2(lhs, dest);
        return;
      case 1:  // val == power of 2
        if (val == 1) {
          moveSimd128Int(lhs, dest);
        } else {
          lhs = moveSimd128IntIfNotAVX(lhs, dest);
          vpsllq(Imm32(mozilla::CountTrailingZeroes64(val)), lhs, dest);
        }
        return;
      case 2: {
        // Constants with 2 bits set, such as 3, 5, 10, etc.
        int i0 = mozilla::CountTrailingZeroes64(val);
        int i1 = mozilla::CountTrailingZeroes64(val & (val - 1));
        FloatRegister lhsForTemp = moveSimd128IntIfNotAVX(lhs, temp);
        vpsllq(Imm32(i1), lhsForTemp, temp);
        lhs = moveSimd128IntIfNotAVX(lhs, dest);
        if (i0 > 0) {
          vpsllq(Imm32(i0), lhs, dest);
          lhs = dest;
        }
        vpaddq(Operand(temp), lhs, dest);
        return;
      }
      case 63: {
        // Some constants with 1 bit unset, such as -2, -3, -5, etc.
        FloatRegister lhsForTemp = moveSimd128IntIfNotAVX(lhs, temp);
        vpsllq(Imm32(mozilla::CountTrailingZeroes64(~val)), lhsForTemp, temp);
        negInt64x2(lhs, dest);
        vpsubq(Operand(temp), dest, dest);
        return;
      }
    }
  }

  // lhs = <D C> <B A>
  // rhs = <H G> <F E>
  // result = <(DG+CH)_low+CG_high CG_low> <(BE+AF)_low+AE_high AE_low>

  if ((c[0] >> 32) == 0 && (c[1] >> 32) == 0) {
    // If the H and F == 0, simplify calculations:
    //   result = <DG_low+CG_high CG_low> <BE_low+AE_high AE_low>
    const int64_t rhsShifted[2] = {c[0] << 32, c[1] << 32};
    FloatRegister lhsForTemp = moveSimd128IntIfNotAVX(lhs, temp);
    vpmulldSimd128(SimdConstant::CreateSimd128(rhsShifted), lhsForTemp, temp);
    vpmuludqSimd128(rhs, lhs, dest);
    vpaddq(Operand(temp), dest, dest);
    return;
  }

  const int64_t rhsSwapped[2] = {
      static_cast<int64_t>(static_cast<uint64_t>(c[0]) >> 32) | (c[0] << 32),
      static_cast<int64_t>(static_cast<uint64_t>(c[1]) >> 32) | (c[1] << 32),
  };  // rhsSwapped = <G H> <E F>
  FloatRegister lhsForTemp = moveSimd128IntIfNotAVX(lhs, temp);
  vpmulldSimd128(SimdConstant::CreateSimd128(rhsSwapped), lhsForTemp,
                 temp);                // temp = <DG CH> <BE AF>
  vphaddd(Operand(temp), temp, temp);  // temp = <xx xx> <DG+CH BE+AF>
  vpmovzxdq(Operand(temp), temp);      // temp = <0 DG+CG> <0 BE+AF>
  vpmuludqSimd128(rhs, lhs, dest);     // dest = <CG_high CG_low>
                                       //        <AE_high AE_low>
  vpsllq(Imm32(32), temp, temp);       // temp = <(DG+CH)_low 0>
                                       //        <(BE+AF)_low 0>
  vpaddq(Operand(temp), dest, dest);
}

// Code generation from the PR: https://github.com/WebAssembly/simd/pull/376.
// The double PSHUFD for the 32->64 case is not great, and there's some
// discussion on the PR (scroll down far enough) on how to avoid one of them,
// but we need benchmarking + correctness proofs.

void MacroAssembler::extMulLowInt8x16(FloatRegister lhs, FloatRegister rhs,
                                      FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  widenLowInt8x16(rhs, scratch);
  widenLowInt8x16(lhs, dest);
  mulInt16x8(dest, scratch, dest);
}

void MacroAssembler::extMulHighInt8x16(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  widenHighInt8x16(rhs, scratch);
  widenHighInt8x16(lhs, dest);
  mulInt16x8(dest, scratch, dest);
}

void MacroAssembler::unsignedExtMulLowInt8x16(FloatRegister lhs,
                                              FloatRegister rhs,
                                              FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  unsignedWidenLowInt8x16(rhs, scratch);
  unsignedWidenLowInt8x16(lhs, dest);
  mulInt16x8(dest, scratch, dest);
}

void MacroAssembler::unsignedExtMulHighInt8x16(FloatRegister lhs,
                                               FloatRegister rhs,
                                               FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  unsignedWidenHighInt8x16(rhs, scratch);
  unsignedWidenHighInt8x16(lhs, dest);
  mulInt16x8(dest, scratch, dest);
}

void MacroAssembler::extMulLowInt16x8(FloatRegister lhs, FloatRegister rhs,
                                      FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  FloatRegister lhsCopy = moveSimd128IntIfNotAVX(lhs, scratch);
  vpmulhw(Operand(rhs), lhsCopy, scratch);
  vpmullw(Operand(rhs), lhs, dest);
  vpunpcklwd(scratch, dest, dest);
}

void MacroAssembler::extMulHighInt16x8(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  FloatRegister lhsCopy = moveSimd128IntIfNotAVX(lhs, scratch);
  vpmulhw(Operand(rhs), lhsCopy, scratch);
  vpmullw(Operand(rhs), lhs, dest);
  vpunpckhwd(scratch, dest, dest);
}

void MacroAssembler::unsignedExtMulLowInt16x8(FloatRegister lhs,
                                              FloatRegister rhs,
                                              FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  FloatRegister lhsCopy = moveSimd128IntIfNotAVX(lhs, scratch);
  vpmulhuw(Operand(rhs), lhsCopy, scratch);
  vpmullw(Operand(rhs), lhs, dest);
  vpunpcklwd(scratch, dest, dest);
}

void MacroAssembler::unsignedExtMulHighInt16x8(FloatRegister lhs,
                                               FloatRegister rhs,
                                               FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  FloatRegister lhsCopy = moveSimd128IntIfNotAVX(lhs, scratch);
  vpmulhuw(Operand(rhs), lhsCopy, scratch);
  vpmullw(Operand(rhs), lhs, dest);
  vpunpckhwd(scratch, dest, dest);
}

void MacroAssembler::extMulLowInt32x4(FloatRegister lhs, FloatRegister rhs,
                                      FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  vpshufd(ComputeShuffleMask(0, 0, 1, 0), lhs, scratch);
  vpshufd(ComputeShuffleMask(0, 0, 1, 0), rhs, dest);
  vpmuldq(scratch, dest, dest);
}

void MacroAssembler::extMulHighInt32x4(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  vpshufd(ComputeShuffleMask(2, 0, 3, 0), lhs, scratch);
  vpshufd(ComputeShuffleMask(2, 0, 3, 0), rhs, dest);
  vpmuldq(scratch, dest, dest);
}

void MacroAssembler::unsignedExtMulLowInt32x4(FloatRegister lhs,
                                              FloatRegister rhs,
                                              FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  vpshufd(ComputeShuffleMask(0, 0, 1, 0), lhs, scratch);
  vpshufd(ComputeShuffleMask(0, 0, 1, 0), rhs, dest);
  vpmuludq(Operand(scratch), dest, dest);
}

void MacroAssembler::unsignedExtMulHighInt32x4(FloatRegister lhs,
                                               FloatRegister rhs,
                                               FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  vpshufd(ComputeShuffleMask(2, 0, 3, 0), lhs, scratch);
  vpshufd(ComputeShuffleMask(2, 0, 3, 0), rhs, dest);
  vpmuludq(Operand(scratch), dest, dest);
}

void MacroAssembler::q15MulrSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  vpmulhrsw(Operand(rhs), lhs, dest);
  FloatRegister destCopy = moveSimd128IntIfNotAVX(dest, scratch);
  vpcmpeqwSimd128(SimdConstant::SplatX8(0x8000), destCopy, scratch);
  vpxor(scratch, dest, dest);
}

void MacroAssembler::q15MulrInt16x8Relaxed(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpmulhrsw(Operand(rhs), lhs, dest);
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

void MacroAssembler::addSatInt8x16(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest) {
  vpaddsb(Operand(rhs), lhs, dest);
}

void MacroAssembler::addSatInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                   FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpaddsb,
                &MacroAssembler::vpaddsbSimd128);
}

void MacroAssembler::unsignedAddSatInt8x16(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpaddusb(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedAddSatInt8x16(FloatRegister lhs,
                                           const SimdConstant& rhs,
                                           FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpaddusb,
                &MacroAssembler::vpaddusbSimd128);
}

void MacroAssembler::addSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest) {
  vpaddsw(Operand(rhs), lhs, dest);
}

void MacroAssembler::addSatInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                   FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpaddsw,
                &MacroAssembler::vpaddswSimd128);
}

void MacroAssembler::unsignedAddSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpaddusw(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedAddSatInt16x8(FloatRegister lhs,
                                           const SimdConstant& rhs,
                                           FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpaddusw,
                &MacroAssembler::vpadduswSimd128);
}

// Saturating integer subtract

void MacroAssembler::subSatInt8x16(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest) {
  vpsubsb(Operand(rhs), lhs, dest);
}

void MacroAssembler::subSatInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                   FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpsubsb,
                &MacroAssembler::vpsubsbSimd128);
}

void MacroAssembler::unsignedSubSatInt8x16(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpsubusb(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedSubSatInt8x16(FloatRegister lhs,
                                           const SimdConstant& rhs,
                                           FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpsubusb,
                &MacroAssembler::vpsubusbSimd128);
}

void MacroAssembler::subSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest) {
  vpsubsw(Operand(rhs), lhs, dest);
}

void MacroAssembler::subSatInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                   FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpsubsw,
                &MacroAssembler::vpsubswSimd128);
}

void MacroAssembler::unsignedSubSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpsubusw(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedSubSatInt16x8(FloatRegister lhs,
                                           const SimdConstant& rhs,
                                           FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpsubusw,
                &MacroAssembler::vpsubuswSimd128);
}

// Lane-wise integer minimum

void MacroAssembler::minInt8x16(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpminsb(Operand(rhs), lhs, dest);
}

void MacroAssembler::minInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpminsb,
                &MacroAssembler::vpminsbSimd128);
}

void MacroAssembler::unsignedMinInt8x16(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vpminub(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedMinInt8x16(FloatRegister lhs,
                                        const SimdConstant& rhs,
                                        FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpminub,
                &MacroAssembler::vpminubSimd128);
}

void MacroAssembler::minInt16x8(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpminsw(Operand(rhs), lhs, dest);
}

void MacroAssembler::minInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpminsw,
                &MacroAssembler::vpminswSimd128);
}

void MacroAssembler::unsignedMinInt16x8(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vpminuw(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedMinInt16x8(FloatRegister lhs,
                                        const SimdConstant& rhs,
                                        FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpminuw,
                &MacroAssembler::vpminuwSimd128);
}

void MacroAssembler::minInt32x4(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpminsd(Operand(rhs), lhs, dest);
}

void MacroAssembler::minInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpminsd,
                &MacroAssembler::vpminsdSimd128);
}

void MacroAssembler::unsignedMinInt32x4(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vpminud(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedMinInt32x4(FloatRegister lhs,
                                        const SimdConstant& rhs,
                                        FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpminud,
                &MacroAssembler::vpminudSimd128);
}

// Lane-wise integer maximum

void MacroAssembler::maxInt8x16(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpmaxsb(Operand(rhs), lhs, dest);
}

void MacroAssembler::maxInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpmaxsb,
                &MacroAssembler::vpmaxsbSimd128);
}

void MacroAssembler::unsignedMaxInt8x16(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vpmaxub(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedMaxInt8x16(FloatRegister lhs,
                                        const SimdConstant& rhs,
                                        FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpmaxub,
                &MacroAssembler::vpmaxubSimd128);
}

void MacroAssembler::maxInt16x8(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpmaxsw(Operand(rhs), lhs, dest);
}

void MacroAssembler::maxInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpmaxsw,
                &MacroAssembler::vpmaxswSimd128);
}

void MacroAssembler::unsignedMaxInt16x8(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vpmaxuw(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedMaxInt16x8(FloatRegister lhs,
                                        const SimdConstant& rhs,
                                        FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpmaxuw,
                &MacroAssembler::vpmaxuwSimd128);
}

void MacroAssembler::maxInt32x4(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest) {
  vpmaxsd(Operand(rhs), lhs, dest);
}

void MacroAssembler::maxInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpmaxsd,
                &MacroAssembler::vpmaxsdSimd128);
}

void MacroAssembler::unsignedMaxInt32x4(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vpmaxud(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedMaxInt32x4(FloatRegister lhs,
                                        const SimdConstant& rhs,
                                        FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpmaxud,
                &MacroAssembler::vpmaxudSimd128);
}

// Lane-wise integer rounding average

void MacroAssembler::unsignedAverageInt8x16(FloatRegister lhs,
                                            FloatRegister rhs,
                                            FloatRegister dest) {
  vpavgb(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedAverageInt16x8(FloatRegister lhs,
                                            FloatRegister rhs,
                                            FloatRegister dest) {
  vpavgw(Operand(rhs), lhs, dest);
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
  signReplicationInt64x2(src, scratch);
  src = moveSimd128IntIfNotAVX(src, dest);
  vpxor(Operand(scratch), src, dest);
  vpsubq(Operand(scratch), dest, dest);
}

// Left shift by scalar

void MacroAssembler::leftShiftInt8x16(Register rhs, FloatRegister lhsDest,
                                      FloatRegister temp) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt8x16(lhsDest, rhs, temp,
                                                          lhsDest);
}

void MacroAssembler::leftShiftInt8x16(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt8x16(count, src, dest);
}

void MacroAssembler::leftShiftInt16x8(Register rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt16x8(lhsDest, rhs,
                                                          lhsDest);
}

void MacroAssembler::leftShiftInt16x8(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsllw(count, src, dest);
}

void MacroAssembler::leftShiftInt32x4(Register rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt32x4(lhsDest, rhs,
                                                          lhsDest);
}

void MacroAssembler::leftShiftInt32x4(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpslld(count, src, dest);
}

void MacroAssembler::leftShiftInt64x2(Register rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::packedLeftShiftByScalarInt64x2(lhsDest, rhs,
                                                          lhsDest);
}

void MacroAssembler::leftShiftInt64x2(Imm32 count, FloatRegister src,
                                      FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsllq(count, src, dest);
}

// Right shift by scalar

void MacroAssembler::rightShiftInt8x16(Register rhs, FloatRegister lhsDest,
                                       FloatRegister temp) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt8x16(lhsDest, rhs, temp,
                                                           lhsDest);
}

void MacroAssembler::rightShiftInt8x16(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt8x16(count, src, dest);
}

void MacroAssembler::unsignedRightShiftInt8x16(Register rhs,
                                               FloatRegister lhsDest,
                                               FloatRegister temp) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt8x16(
      lhsDest, rhs, temp, lhsDest);
}

void MacroAssembler::unsignedRightShiftInt8x16(Imm32 count, FloatRegister src,
                                               FloatRegister dest) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt8x16(count, src,
                                                                   dest);
}

void MacroAssembler::rightShiftInt16x8(Register rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt16x8(lhsDest, rhs,
                                                           lhsDest);
}

void MacroAssembler::rightShiftInt16x8(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsraw(count, src, dest);
}

void MacroAssembler::unsignedRightShiftInt16x8(Register rhs,
                                               FloatRegister lhsDest) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt16x8(lhsDest, rhs,
                                                                   lhsDest);
}

void MacroAssembler::unsignedRightShiftInt16x8(Imm32 count, FloatRegister src,
                                               FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsrlw(count, src, dest);
}

void MacroAssembler::rightShiftInt32x4(Register rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt32x4(lhsDest, rhs,
                                                           lhsDest);
}

void MacroAssembler::rightShiftInt32x4(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsrad(count, src, dest);
}

void MacroAssembler::unsignedRightShiftInt32x4(Register rhs,
                                               FloatRegister lhsDest) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt32x4(lhsDest, rhs,
                                                                   lhsDest);
}

void MacroAssembler::unsignedRightShiftInt32x4(Imm32 count, FloatRegister src,
                                               FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsrld(count, src, dest);
}

void MacroAssembler::rightShiftInt64x2(Register rhs, FloatRegister lhsDest,
                                       FloatRegister temp) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt64x2(lhsDest, rhs, temp,
                                                           lhsDest);
}

void MacroAssembler::rightShiftInt64x2(Imm32 count, FloatRegister src,
                                       FloatRegister dest) {
  MacroAssemblerX86Shared::packedRightShiftByScalarInt64x2(count, src, dest);
}

void MacroAssembler::unsignedRightShiftInt64x2(Register rhs,
                                               FloatRegister lhsDest) {
  MacroAssemblerX86Shared::packedUnsignedRightShiftByScalarInt64x2(lhsDest, rhs,
                                                                   lhsDest);
}

void MacroAssembler::unsignedRightShiftInt64x2(Imm32 count, FloatRegister src,
                                               FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
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
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsraw(Imm32(15), src, dest);
}

void MacroAssembler::signReplicationInt32x4(FloatRegister src,
                                            FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpsrad(Imm32(31), src, dest);
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

void MacroAssembler::bitwiseAndSimd128(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest) {
  vpand(Operand(rhs), lhs, dest);
}

void MacroAssembler::bitwiseAndSimd128(FloatRegister lhs,
                                       const SimdConstant& rhs,
                                       FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpand,
                &MacroAssembler::vpandSimd128);
}

void MacroAssembler::bitwiseOrSimd128(FloatRegister rhs,
                                      FloatRegister lhsDest) {
  vpor(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::bitwiseOrSimd128(FloatRegister lhs, FloatRegister rhs,
                                      FloatRegister dest) {
  vpor(Operand(rhs), lhs, dest);
}

void MacroAssembler::bitwiseOrSimd128(FloatRegister lhs,
                                      const SimdConstant& rhs,
                                      FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpor,
                &MacroAssembler::vporSimd128);
}

void MacroAssembler::bitwiseXorSimd128(FloatRegister rhs,
                                       FloatRegister lhsDest) {
  vpxor(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::bitwiseXorSimd128(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest) {
  vpxor(Operand(rhs), lhs, dest);
}

void MacroAssembler::bitwiseXorSimd128(FloatRegister lhs,
                                       const SimdConstant& rhs,
                                       FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpxor,
                &MacroAssembler::vpxorSimd128);
}

void MacroAssembler::bitwiseNotSimd128(FloatRegister src, FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  bitwiseXorSimd128(src, SimdConstant::SplatX16(-1), dest);
}

// Bitwise and-not

void MacroAssembler::bitwiseNotAndSimd128(FloatRegister rhs,
                                          FloatRegister lhsDest) {
  vpandn(Operand(rhs), lhsDest, lhsDest);
}

void MacroAssembler::bitwiseNotAndSimd128(FloatRegister lhs, FloatRegister rhs,
                                          FloatRegister dest) {
  vpandn(Operand(rhs), lhs, dest);
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
                                    FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest) {
  MacroAssemblerX86Shared::compareInt8x16(lhs, Operand(rhs), cond, dest);
}

void MacroAssembler::compareInt8x16(Assembler::Condition cond,
                                    FloatRegister lhs, const SimdConstant& rhs,
                                    FloatRegister dest) {
  MOZ_ASSERT(cond != Assembler::Condition::LessThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareInt8x16(cond, lhs, rhs, dest);
}

void MacroAssembler::compareInt16x8(Assembler::Condition cond,
                                    FloatRegister rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::compareInt16x8(lhsDest, Operand(rhs), cond, lhsDest);
}

void MacroAssembler::compareInt16x8(Assembler::Condition cond,
                                    FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest) {
  MacroAssemblerX86Shared::compareInt16x8(lhs, Operand(rhs), cond, dest);
}

void MacroAssembler::compareInt16x8(Assembler::Condition cond,
                                    FloatRegister lhs, const SimdConstant& rhs,
                                    FloatRegister dest) {
  MOZ_ASSERT(cond != Assembler::Condition::LessThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareInt16x8(cond, lhs, rhs, dest);
}

void MacroAssembler::compareInt32x4(Assembler::Condition cond,
                                    FloatRegister rhs, FloatRegister lhsDest) {
  MacroAssemblerX86Shared::compareInt32x4(lhsDest, Operand(rhs), cond, lhsDest);
}

void MacroAssembler::compareInt32x4(Assembler::Condition cond,
                                    FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest) {
  MacroAssemblerX86Shared::compareInt32x4(lhs, Operand(rhs), cond, dest);
}

void MacroAssembler::compareInt32x4(Assembler::Condition cond,
                                    FloatRegister lhs, const SimdConstant& rhs,
                                    FloatRegister dest) {
  MOZ_ASSERT(cond != Assembler::Condition::LessThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareInt32x4(cond, lhs, rhs, dest);
}

void MacroAssembler::compareForEqualityInt64x2(Assembler::Condition cond,
                                               FloatRegister lhs,
                                               FloatRegister rhs,
                                               FloatRegister dest) {
  MacroAssemblerX86Shared::compareForEqualityInt64x2(lhs, Operand(rhs), cond,
                                                     dest);
}

void MacroAssembler::compareForOrderingInt64x2(
    Assembler::Condition cond, FloatRegister lhs, FloatRegister rhs,
    FloatRegister dest, FloatRegister temp1, FloatRegister temp2) {
  if (HasAVX() && HasSSE42()) {
    MacroAssemblerX86Shared::compareForOrderingInt64x2AVX(lhs, rhs, cond, dest);
  } else {
    MacroAssemblerX86Shared::compareForOrderingInt64x2(lhs, Operand(rhs), cond,
                                                       temp1, temp2, dest);
  }
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
                                      FloatRegister lhs, FloatRegister rhs,
                                      FloatRegister dest) {
  MacroAssemblerX86Shared::compareFloat32x4(lhs, Operand(rhs), cond, dest);
}

void MacroAssembler::compareFloat32x4(Assembler::Condition cond,
                                      FloatRegister lhs,
                                      const SimdConstant& rhs,
                                      FloatRegister dest) {
  MOZ_ASSERT(cond != Assembler::Condition::GreaterThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareFloat32x4(cond, lhs, rhs, dest);
}

void MacroAssembler::compareFloat64x2(Assembler::Condition cond,
                                      FloatRegister rhs,
                                      FloatRegister lhsDest) {
  compareFloat64x2(cond, lhsDest, rhs, lhsDest);
}

void MacroAssembler::compareFloat64x2(Assembler::Condition cond,
                                      FloatRegister lhs, FloatRegister rhs,
                                      FloatRegister dest) {
  // Code in the SIMD implementation allows operands to be reversed like this,
  // this benefits the baseline compiler.  Ion takes care of the reversing
  // itself and never generates GT/GE.
  if (cond == Assembler::GreaterThan) {
    MacroAssemblerX86Shared::compareFloat64x2(rhs, Operand(lhs),
                                              Assembler::LessThan, dest);
  } else if (cond == Assembler::GreaterThanOrEqual) {
    MacroAssemblerX86Shared::compareFloat64x2(rhs, Operand(lhs),
                                              Assembler::LessThanOrEqual, dest);
  } else {
    MacroAssemblerX86Shared::compareFloat64x2(lhs, Operand(rhs), cond, dest);
  }
}

void MacroAssembler::compareFloat64x2(Assembler::Condition cond,
                                      FloatRegister lhs,
                                      const SimdConstant& rhs,
                                      FloatRegister dest) {
  MOZ_ASSERT(cond != Assembler::Condition::GreaterThan &&
             cond != Assembler::Condition::GreaterThanOrEqual);
  MacroAssemblerX86Shared::compareFloat64x2(cond, lhs, rhs, dest);
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
  src = moveSimd128FloatIfNotAVX(src, dest);
  bitwiseXorSimd128(src, SimdConstant::SplatX4(-0.f), dest);
}

void MacroAssembler::negFloat64x2(FloatRegister src, FloatRegister dest) {
  src = moveSimd128FloatIfNotAVX(src, dest);
  bitwiseXorSimd128(src, SimdConstant::SplatX2(-0.0), dest);
}

// Floating point absolute value

void MacroAssembler::absFloat32x4(FloatRegister src, FloatRegister dest) {
  src = moveSimd128FloatIfNotAVX(src, dest);
  bitwiseAndSimd128(src, SimdConstant::SplatX4(0x7FFFFFFF), dest);
}

void MacroAssembler::absFloat64x2(FloatRegister src, FloatRegister dest) {
  src = moveSimd128FloatIfNotAVX(src, dest);
  bitwiseAndSimd128(src, SimdConstant::SplatX2(int64_t(0x7FFFFFFFFFFFFFFFll)),
                    dest);
}

// NaN-propagating minimum

void MacroAssembler::minFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest, FloatRegister temp1,
                                  FloatRegister temp2) {
  MacroAssemblerX86Shared::minFloat32x4(lhs, rhs, temp1, temp2, dest);
}

void MacroAssembler::minFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest, FloatRegister temp1,
                                  FloatRegister temp2) {
  MacroAssemblerX86Shared::minFloat64x2(lhs, rhs, temp1, temp2, dest);
}

// NaN-propagating maximum

void MacroAssembler::maxFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest, FloatRegister temp1,
                                  FloatRegister temp2) {
  MacroAssemblerX86Shared::maxFloat32x4(lhs, rhs, temp1, temp2, dest);
}

void MacroAssembler::maxFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest, FloatRegister temp1,
                                  FloatRegister temp2) {
  MacroAssemblerX86Shared::maxFloat64x2(lhs, rhs, temp1, temp2, dest);
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

void MacroAssembler::pseudoMinFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vminps(Operand(rhs), lhs, dest);
}

void MacroAssembler::pseudoMinFloat64x2(FloatRegister rhsOrRhsDest,
                                        FloatRegister lhsOrLhsDest) {
  FloatRegister rhsDest = rhsOrRhsDest;
  FloatRegister lhs = lhsOrLhsDest;
  vminpd(Operand(lhs), rhsDest, rhsDest);
}

void MacroAssembler::pseudoMinFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vminpd(Operand(rhs), lhs, dest);
}

// Compare-based maximum

void MacroAssembler::pseudoMaxFloat32x4(FloatRegister rhsOrRhsDest,
                                        FloatRegister lhsOrLhsDest) {
  FloatRegister rhsDest = rhsOrRhsDest;
  FloatRegister lhs = lhsOrLhsDest;
  vmaxps(Operand(lhs), rhsDest, rhsDest);
}

void MacroAssembler::pseudoMaxFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vmaxps(Operand(rhs), lhs, dest);
}

void MacroAssembler::pseudoMaxFloat64x2(FloatRegister rhsOrRhsDest,
                                        FloatRegister lhsOrLhsDest) {
  FloatRegister rhsDest = rhsOrRhsDest;
  FloatRegister lhs = lhsOrLhsDest;
  vmaxpd(Operand(lhs), rhsDest, rhsDest);
}

void MacroAssembler::pseudoMaxFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest) {
  vmaxpd(Operand(rhs), lhs, dest);
}

// Widening/pairwise integer dot product

void MacroAssembler::widenDotInt16x8(FloatRegister lhs, FloatRegister rhs,
                                     FloatRegister dest) {
  vpmaddwd(Operand(rhs), lhs, dest);
}

void MacroAssembler::widenDotInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                     FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpmaddwd,
                &MacroAssembler::vpmaddwdSimd128);
}

void MacroAssembler::dotInt8x16Int7x16(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  if (lhs == dest && !HasAVX()) {
    moveSimd128Int(lhs, scratch);
    lhs = scratch;
  }
  rhs = moveSimd128IntIfNotAVX(rhs, dest);
  vpmaddubsw(lhs, rhs, dest);
}

void MacroAssembler::dotInt8x16Int7x16ThenAdd(FloatRegister lhs,
                                              FloatRegister rhs,
                                              FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  rhs = moveSimd128IntIfNotAVX(rhs, scratch);
  vpmaddubsw(lhs, rhs, scratch);
  vpmaddwdSimd128(SimdConstant::SplatX8(1), scratch, scratch);
  vpaddd(Operand(scratch), dest, dest);
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

void MacroAssembler::addFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest) {
  vaddps(Operand(rhs), lhs, dest);
}

void MacroAssembler::addFloat32x4(FloatRegister lhs, const SimdConstant& rhs,
                                  FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vaddps,
                &MacroAssembler::vaddpsSimd128);
}

void MacroAssembler::addFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest) {
  vaddpd(Operand(rhs), lhs, dest);
}

void MacroAssembler::addFloat64x2(FloatRegister lhs, const SimdConstant& rhs,
                                  FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vaddpd,
                &MacroAssembler::vaddpdSimd128);
}

// Floating subtract

void MacroAssembler::subFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest) {
  vsubps(Operand(rhs), lhs, dest);
}

void MacroAssembler::subFloat32x4(FloatRegister lhs, const SimdConstant& rhs,
                                  FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vsubps,
                &MacroAssembler::vsubpsSimd128);
}

void MacroAssembler::subFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest) {
  AssemblerX86Shared::vsubpd(Operand(rhs), lhs, dest);
}

void MacroAssembler::subFloat64x2(FloatRegister lhs, const SimdConstant& rhs,
                                  FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vsubpd,
                &MacroAssembler::vsubpdSimd128);
}

// Floating division

void MacroAssembler::divFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest) {
  vdivps(Operand(rhs), lhs, dest);
}

void MacroAssembler::divFloat32x4(FloatRegister lhs, const SimdConstant& rhs,
                                  FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vdivps,
                &MacroAssembler::vdivpsSimd128);
}

void MacroAssembler::divFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest) {
  vdivpd(Operand(rhs), lhs, dest);
}

void MacroAssembler::divFloat64x2(FloatRegister lhs, const SimdConstant& rhs,
                                  FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vdivpd,
                &MacroAssembler::vdivpdSimd128);
}

// Floating Multiply

void MacroAssembler::mulFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest) {
  vmulps(Operand(rhs), lhs, dest);
}

void MacroAssembler::mulFloat32x4(FloatRegister lhs, const SimdConstant& rhs,
                                  FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vmulps,
                &MacroAssembler::vmulpsSimd128);
}

void MacroAssembler::mulFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest) {
  vmulpd(Operand(rhs), lhs, dest);
}

void MacroAssembler::mulFloat64x2(FloatRegister lhs, const SimdConstant& rhs,
                                  FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vmulpd,
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
  src = moveSimd128IntIfNotAVX(src, dest);
  vpmaddubswSimd128(SimdConstant::SplatX16(1), src, dest);
}

void MacroAssembler::extAddPairwiseInt16x8(FloatRegister src,
                                           FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpmaddwdSimd128(SimdConstant::SplatX8(1), src, dest);
}

void MacroAssembler::unsignedExtAddPairwiseInt16x8(FloatRegister src,
                                                   FloatRegister dest) {
  src = moveSimd128IntIfNotAVX(src, dest);
  vpxorSimd128(SimdConstant::SplatX8(-0x8000), src, dest);
  vpmaddwdSimd128(SimdConstant::SplatX8(1), dest, dest);
  vpadddSimd128(SimdConstant::SplatX4(0x00010000), dest, dest);
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

void MacroAssembler::truncFloat32x4ToInt32x4Relaxed(FloatRegister src,
                                                    FloatRegister dest) {
  vcvttps2dq(src, dest);
}

void MacroAssembler::unsignedTruncFloat32x4ToInt32x4Relaxed(
    FloatRegister src, FloatRegister dest) {
  MacroAssemblerX86Shared::unsignedTruncFloat32x4ToInt32x4Relaxed(src, dest);
}

void MacroAssembler::truncFloat64x2ToInt32x4Relaxed(FloatRegister src,
                                                    FloatRegister dest) {
  vcvttpd2dq(src, dest);
}

void MacroAssembler::unsignedTruncFloat64x2ToInt32x4Relaxed(
    FloatRegister src, FloatRegister dest) {
  MacroAssemblerX86Shared::unsignedTruncFloat64x2ToInt32x4Relaxed(src, dest);
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

void MacroAssembler::narrowInt16x8(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest) {
  vpacksswb(Operand(rhs), lhs, dest);
}

void MacroAssembler::narrowInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                   FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpacksswb,
                &MacroAssembler::vpacksswbSimd128);
}

void MacroAssembler::unsignedNarrowInt16x8(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpackuswb(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedNarrowInt16x8(FloatRegister lhs,
                                           const SimdConstant& rhs,
                                           FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpackuswb,
                &MacroAssembler::vpackuswbSimd128);
}

void MacroAssembler::narrowInt32x4(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest) {
  vpackssdw(Operand(rhs), lhs, dest);
}

void MacroAssembler::narrowInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                                   FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpackssdw,
                &MacroAssembler::vpackssdwSimd128);
}

void MacroAssembler::unsignedNarrowInt32x4(FloatRegister lhs, FloatRegister rhs,
                                           FloatRegister dest) {
  vpackusdw(Operand(rhs), lhs, dest);
}

void MacroAssembler::unsignedNarrowInt32x4(FloatRegister lhs,
                                           const SimdConstant& rhs,
                                           FloatRegister dest) {
  binarySimd128(lhs, rhs, dest, &MacroAssembler::vpackusdw,
                &MacroAssembler::vpackusdwSimd128);
}

// Integer to integer widening

void MacroAssembler::widenLowInt8x16(FloatRegister src, FloatRegister dest) {
  vpmovsxbw(Operand(src), dest);
}

void MacroAssembler::widenHighInt8x16(FloatRegister src, FloatRegister dest) {
  vpalignr(Operand(src), dest, dest, 8);
  vpmovsxbw(Operand(dest), dest);
}

void MacroAssembler::unsignedWidenLowInt8x16(FloatRegister src,
                                             FloatRegister dest) {
  vpmovzxbw(Operand(src), dest);
}

void MacroAssembler::unsignedWidenHighInt8x16(FloatRegister src,
                                              FloatRegister dest) {
  vpalignr(Operand(src), dest, dest, 8);
  vpmovzxbw(Operand(dest), dest);
}

void MacroAssembler::widenLowInt16x8(FloatRegister src, FloatRegister dest) {
  vpmovsxwd(Operand(src), dest);
}

void MacroAssembler::widenHighInt16x8(FloatRegister src, FloatRegister dest) {
  vpalignr(Operand(src), dest, dest, 8);
  vpmovsxwd(Operand(dest), dest);
}

void MacroAssembler::unsignedWidenLowInt16x8(FloatRegister src,
                                             FloatRegister dest) {
  vpmovzxwd(Operand(src), dest);
}

void MacroAssembler::unsignedWidenHighInt16x8(FloatRegister src,
                                              FloatRegister dest) {
  vpalignr(Operand(src), dest, dest, 8);
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
  if (src == dest || HasAVX()) {
    vmovhlps(src, src, dest);
  } else {
    vpshufd(ComputeShuffleMask(2, 3, 2, 3), src, dest);
  }
  vpmovsxdq(Operand(dest), dest);
}

void MacroAssembler::unsignedWidenHighInt32x4(FloatRegister src,
                                              FloatRegister dest) {
  ScratchSimd128Scope scratch(*this);
  src = moveSimd128IntIfNotAVX(src, dest);
  vpxor(scratch, scratch, scratch);
  vpunpckhdq(scratch, src, dest);
}

// Floating multiply-accumulate: srcDest [+-]= src1 * src2
// The Intel FMA feature is some AVX* special sauce, no support yet.

void MacroAssembler::fmaFloat32x4(FloatRegister src1, FloatRegister src2,
                                  FloatRegister srcDest) {
  if (HasFMA()) {
    vfmadd231ps(src2, src1, srcDest);
    return;
  }
  ScratchSimd128Scope scratch(*this);
  src1 = moveSimd128FloatIfNotAVX(src1, scratch);
  mulFloat32x4(src1, src2, scratch);
  addFloat32x4(srcDest, scratch, srcDest);
}

void MacroAssembler::fnmaFloat32x4(FloatRegister src1, FloatRegister src2,
                                   FloatRegister srcDest) {
  if (HasFMA()) {
    vfnmadd231ps(src2, src1, srcDest);
    return;
  }
  ScratchSimd128Scope scratch(*this);
  src1 = moveSimd128FloatIfNotAVX(src1, scratch);
  mulFloat32x4(src1, src2, scratch);
  subFloat32x4(srcDest, scratch, srcDest);
}

void MacroAssembler::fmaFloat64x2(FloatRegister src1, FloatRegister src2,
                                  FloatRegister srcDest) {
  if (HasFMA()) {
    vfmadd231pd(src2, src1, srcDest);
    return;
  }
  ScratchSimd128Scope scratch(*this);
  src1 = moveSimd128FloatIfNotAVX(src1, scratch);
  mulFloat64x2(src1, src2, scratch);
  addFloat64x2(srcDest, scratch, srcDest);
}

void MacroAssembler::fnmaFloat64x2(FloatRegister src1, FloatRegister src2,
                                   FloatRegister srcDest) {
  if (HasFMA()) {
    vfnmadd231pd(src2, src1, srcDest);
    return;
  }
  ScratchSimd128Scope scratch(*this);
  src1 = moveSimd128FloatIfNotAVX(src1, scratch);
  mulFloat64x2(src1, src2, scratch);
  subFloat64x2(srcDest, scratch, srcDest);
}

void MacroAssembler::minFloat32x4Relaxed(FloatRegister src,
                                         FloatRegister srcDest) {
  vminps(Operand(src), srcDest, srcDest);
}

void MacroAssembler::minFloat32x4Relaxed(FloatRegister lhs, FloatRegister rhs,
                                         FloatRegister dest) {
  vminps(Operand(rhs), lhs, dest);
}

void MacroAssembler::maxFloat32x4Relaxed(FloatRegister src,
                                         FloatRegister srcDest) {
  vmaxps(Operand(src), srcDest, srcDest);
}

void MacroAssembler::maxFloat32x4Relaxed(FloatRegister lhs, FloatRegister rhs,
                                         FloatRegister dest) {
  vmaxps(Operand(rhs), lhs, dest);
}

void MacroAssembler::minFloat64x2Relaxed(FloatRegister src,
                                         FloatRegister srcDest) {
  vminpd(Operand(src), srcDest, srcDest);
}

void MacroAssembler::minFloat64x2Relaxed(FloatRegister lhs, FloatRegister rhs,
                                         FloatRegister dest) {
  vminpd(Operand(rhs), lhs, dest);
}

void MacroAssembler::maxFloat64x2Relaxed(FloatRegister src,
                                         FloatRegister srcDest) {
  vmaxpd(Operand(src), srcDest, srcDest);
}

void MacroAssembler::maxFloat64x2Relaxed(FloatRegister lhs, FloatRegister rhs,
                                         FloatRegister dest) {
  vmaxpd(Operand(rhs), lhs, dest);
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
