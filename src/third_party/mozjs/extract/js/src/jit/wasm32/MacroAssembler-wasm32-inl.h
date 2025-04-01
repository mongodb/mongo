/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_wasm32_MacroAssembler_wasm32_inl_h
#define jit_wasm32_MacroAssembler_wasm32_inl_h

#include "jit/wasm32/MacroAssembler-wasm32.h"

namespace js::jit {

//{{{ check_macroassembler_style

void MacroAssembler::move64(Imm64 imm, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::move64(Register64 src, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::moveDoubleToGPR64(FloatRegister src, Register64 dest) {
  MOZ_CRASH();
}

void MacroAssembler::moveGPR64ToDouble(Register64 src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::move64To32(Register64 src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::move32To64ZeroExtend(Register src, Register64 dest) {
  MOZ_CRASH();
}

void MacroAssembler::move8To64SignExtend(Register src, Register64 dest) {
  MOZ_CRASH();
}

void MacroAssembler::move16To64SignExtend(Register src, Register64 dest) {
  MOZ_CRASH();
}

void MacroAssembler::move32To64SignExtend(Register src, Register64 dest) {
  MOZ_CRASH();
}

void MacroAssembler::move32SignExtendToPtr(Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::move32ZeroExtendToPtr(Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::load32SignExtendToPtr(const Address& src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::notPtr(Register reg) { MOZ_CRASH(); }

void MacroAssembler::andPtr(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::andPtr(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::and64(Imm64 imm, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::or64(Imm64 imm, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::xor64(Imm64 imm, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::orPtr(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::orPtr(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::and64(Register64 src, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::or64(Register64 src, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::xor64(Register64 src, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::xorPtr(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::xorPtr(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::byteSwap64(Register64 reg) { MOZ_CRASH(); }

void MacroAssembler::addPtr(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::addPtr(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::addPtr(ImmWord imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::add64(Register64 src, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::add64(Imm32 imm, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::add64(Imm64 imm, Register64 dest) { MOZ_CRASH(); }

CodeOffset MacroAssembler::sub32FromStackPtrWithPatch(Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::patchSub32FromStackPtr(CodeOffset offset, Imm32 imm) {
  MOZ_CRASH();
}

void MacroAssembler::subPtr(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::subPtr(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::sub64(Register64 src, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::sub64(Imm64 imm, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::mulPtr(Register rhs, Register srcDest) { MOZ_CRASH(); }

void MacroAssembler::mul64(Imm64 imm, const Register64& dest) { MOZ_CRASH(); }

void MacroAssembler::mul64(const Register64& src, const Register64& dest,
                           const Register temp) {
  MOZ_CRASH();
}

void MacroAssembler::mulBy3(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::inc64(AbsoluteAddress dest) { MOZ_CRASH(); }

void MacroAssembler::neg64(Register64 reg) { MOZ_CRASH(); }

void MacroAssembler::negPtr(Register reg) { MOZ_CRASH(); }

void MacroAssembler::lshiftPtr(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::rshiftPtr(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::lshift64(Imm32 imm, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::rshift64(Imm32 imm, Register64 dest) { MOZ_CRASH(); }

void MacroAssembler::rshift64Arithmetic(Imm32 imm, Register64 dest) {
  MOZ_CRASH();
}

void MacroAssembler::lshiftPtr(Register shift, Register srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::rshiftPtr(Register shift, Register srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::lshift64(Register shift, Register64 srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::rshift64(Register shift, Register64 srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::rshift64Arithmetic(Register shift, Register64 srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::clz64(Register64 src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::ctz64(Register64 src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::popcnt64(Register64 src, Register64 dest, Register temp) {
  MOZ_CRASH();
}

template <typename T1, typename T2>
void MacroAssembler::cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::branchToComputedAddress(const BaseIndex& address) {
  MOZ_CRASH();
}

void MacroAssembler::move8ZeroExtend(Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::move8SignExtend(Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::move16SignExtend(Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::loadAbiReturnAddress(Register dest) { MOZ_CRASH(); }

void MacroAssembler::not32(Register reg) { MOZ_CRASH(); }

void MacroAssembler::and32(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::and32(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::and32(Imm32 imm, const Address& dest) { MOZ_CRASH(); }

void MacroAssembler::and32(const Address& src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::or32(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::or32(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::or32(Imm32 imm, const Address& dest) { MOZ_CRASH(); }

void MacroAssembler::xor32(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::xor32(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::xor32(Imm32 imm, const Address& dest) { MOZ_CRASH(); }

void MacroAssembler::xor32(const Address& src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::byteSwap16SignExtend(Register reg) { MOZ_CRASH(); }

void MacroAssembler::byteSwap16ZeroExtend(Register reg) { MOZ_CRASH(); }

void MacroAssembler::byteSwap32(Register reg) { MOZ_CRASH(); }

void MacroAssembler::add32(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::add32(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::add32(Imm32 imm, Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::add32(Imm32 imm, const Address& dest) { MOZ_CRASH(); }

void MacroAssembler::addFloat32(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::addDouble(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::sub32(const Address& src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::sub32(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::sub32(Imm32 imm, Register dest) { MOZ_CRASH(); }

void MacroAssembler::subFloat32(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::subDouble(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::mul32(Register rhs, Register srcDest) { MOZ_CRASH(); }

void MacroAssembler::mul32(Imm32 imm, Register srcDest) { MOZ_CRASH(); }

void MacroAssembler::mulHighUnsigned32(Imm32 imm, Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::mulFloat32(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::mulDouble(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::divFloat32(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::divDouble(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::neg32(Register reg) { MOZ_CRASH(); }

void MacroAssembler::negateFloat(FloatRegister reg) { MOZ_CRASH(); }

void MacroAssembler::negateDouble(FloatRegister reg) { MOZ_CRASH(); }

void MacroAssembler::abs32(Register src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::absFloat32(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::absDouble(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::sqrtDouble(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::lshift32(Imm32 shift, Register srcDest) { MOZ_CRASH(); }

void MacroAssembler::rshift32(Imm32 shift, Register srcDest) { MOZ_CRASH(); }

void MacroAssembler::rshift32Arithmetic(Imm32 shift, Register srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::rshift32Arithmetic(Register shift, Register srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::lshift32(Register shift, Register srcDest) { MOZ_CRASH(); }

void MacroAssembler::rshift32(Register shift, Register srcDest) { MOZ_CRASH(); }

void MacroAssembler::memoryBarrier(MemoryBarrierBits barrier) { MOZ_CRASH(); }

void MacroAssembler::clampIntToUint8(Register reg) { MOZ_CRASH(); }

template <class L>
void MacroAssembler::branchTest32(Condition cond, Register lhs, Register rhs,
                                  L label) {
  MOZ_CRASH();
}

template <class L>
void MacroAssembler::branchTest32(Condition cond, Register lhs, Imm32 rhs,
                                  L label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTest32(Condition cond, const Address& lhs, Imm32 rhh,
                                  Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTest32(Condition cond, const AbsoluteAddress& lhs,
                                  Imm32 rhs, Label* label) {
  MOZ_CRASH();
}

template <class L>
void MacroAssembler::branchTestPtr(Condition cond, Register lhs, Register rhs,
                                   L label) {
  MOZ_CRASH();
}
void MacroAssembler::branchTestPtr(Condition cond, Register lhs, Imm32 rhs,
                                   Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestPtr(Condition cond, const Address& lhs,
                                   Imm32 rhs, Label* label) {
  MOZ_CRASH();
}

template <class L>
void MacroAssembler::branchTest64(Condition cond, Register64 lhs,
                                  Register64 rhs, Register temp, L label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestUndefined(Condition cond, Register tag,
                                         Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestInt32(Condition cond, Register tag,
                                     Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestDouble(Condition cond, Register tag,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestNumber(Condition cond, Register tag,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBoolean(Condition cond, Register tag,
                                       Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestString(Condition cond, Register tag,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestSymbol(Condition cond, Register tag,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBigInt(Condition cond, Register tag,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestNull(Condition cond, Register tag,
                                    Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestObject(Condition cond, Register tag,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestPrimitive(Condition cond, Register tag,
                                         Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestMagic(Condition cond, Register tag,
                                     Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestUndefined(Condition cond, const Address& address,
                                         Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestUndefined(Condition cond,
                                         const BaseIndex& address,
                                         Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestInt32(Condition cond, const Address& address,
                                     Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestInt32(Condition cond, const BaseIndex& address,
                                     Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestInt32(Condition cond, const ValueOperand& value,
                                     Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestDouble(Condition cond, const Address& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestDouble(Condition cond, const BaseIndex& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBoolean(Condition cond, const Address& address,
                                       Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBoolean(Condition cond, const BaseIndex& address,
                                       Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestString(Condition cond, const Address& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestString(Condition cond, const BaseIndex& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestSymbol(Condition cond, const Address& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestSymbol(Condition cond, const BaseIndex& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBigInt(Condition cond, const Address& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBigInt(Condition cond, const BaseIndex& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestNull(Condition cond, const Address& address,
                                    Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestNull(Condition cond, const BaseIndex& address,
                                    Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestNull(Condition cond, const ValueOperand& value,
                                    Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestObject(Condition cond, const Address& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestObject(Condition cond, const BaseIndex& address,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestObject(Condition cond, const ValueOperand& value,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestGCThing(Condition cond, const Address& address,
                                       Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestGCThing(Condition cond, const BaseIndex& address,
                                       Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestGCThing(Condition cond,
                                       const ValueOperand& value,
                                       Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestMagic(Condition cond, const Address& address,
                                     Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestMagic(Condition cond, const BaseIndex& address,
                                     Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestMagic(Condition cond, const Address& valaddr,
                                     JSWhyMagic why, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestValue(Condition cond, const BaseIndex& lhs,
                                     const ValueOperand& rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestDoubleTruthy(bool truthy, FloatRegister reg,
                                            Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBooleanTruthy(bool truthy,
                                             const ValueOperand& value,
                                             Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestStringTruthy(bool truthy,
                                            const ValueOperand& value,
                                            Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBigIntTruthy(bool truthy,
                                            const ValueOperand& value,
                                            Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::fallibleUnboxPtr(const ValueOperand& src, Register dest,
                                      JSValueType type, Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::fallibleUnboxPtr(const Address& src, Register dest,
                                      JSValueType type, Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::fallibleUnboxPtr(const BaseIndex& src, Register dest,
                                      JSValueType type, Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs, Register rhs,
                                   Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs,
                                   const Address& rhs, Register src,
                                   Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs,
                              Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs,
                              Imm32 rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branch32(Condition cond, const Address& lhs, Register rhs,
                              Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branch32(Condition cond, const Address& lhs, Imm32 rhs,
                              Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs,
                              Register rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branch8(Condition cond, const Address& lhs, Imm32 rhs,
                             Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branch8(Condition cond, const BaseIndex& lhs, Register rhs,
                             Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branch16(Condition cond, const Address& lhs, Imm32 rhs,
                              Label* label) {
  MOZ_CRASH();
}

template <class L>
void MacroAssembler::branch32(Condition cond, Register lhs, Register rhs,
                              L label) {
  MOZ_CRASH();
}

template <class L>
void MacroAssembler::branch32(Condition cond, Register lhs, Imm32 rhs,
                              L label) {
  MOZ_CRASH();
}

void MacroAssembler::branch32(Condition cond, wasm::SymbolicAddress lhs,
                              Imm32 rhs, Label* label) {
  MOZ_CRASH();
}

template <class L>
void MacroAssembler::branchPtr(Condition cond, Register lhs, Register rhs,
                               L label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, Imm32 rhs,
                               Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmPtr rhs,
                               Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmGCPtr rhs,
                               Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, Register lhs, ImmWord rhs,
                               Label* label) {
  MOZ_CRASH();
}

template <class L>
void MacroAssembler::branchPtr(Condition cond, const Address& lhs, Register rhs,
                               L label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmPtr rhs,
                               Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmGCPtr rhs,
                               Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmWord rhs,
                               Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs,
                               ImmWord rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs,
                               Register rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs,
                               Register rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs,
                               ImmWord rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchPtr(Condition cond, wasm::SymbolicAddress lhs,
                               Register rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchFloat(DoubleCondition cond, FloatRegister lhs,
                                 FloatRegister rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchDouble(DoubleCondition cond, FloatRegister lhs,
                                  FloatRegister rhs, Label* label) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::branchAdd32(Condition cond, T src, Register dest,
                                 Label* label) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::branchSub32(Condition cond, T src, Register dest,
                                 Label* label) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::branchMul32(Condition cond, T src, Register dest,
                                 Label* label) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::branchRshift32(Condition cond, T src, Register dest,
                                    Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchNeg32(Condition cond, Register reg, Label* label) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::branchAddPtr(Condition cond, T src, Register dest,
                                  Label* label) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::branchSubPtr(Condition cond, T src, Register dest,
                                  Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchMulPtr(Condition cond, Register src, Register dest,
                                  Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::decBranchPtr(Condition cond, Register lhs, Imm32 rhs,
                                  Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::spectreZeroRegister(Condition cond, Register scratch,
                                         Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::spectreMovePtr(Condition cond, Register src,
                                    Register dest) {
  MOZ_CRASH();
}

FaultingCodeOffset MacroAssembler::storeUncanonicalizedDouble(
    FloatRegister src, const Address& dest) {
  MOZ_CRASH();
}

FaultingCodeOffset MacroAssembler::storeUncanonicalizedDouble(
    FloatRegister src, const BaseIndex& dest) {
  MOZ_CRASH();
}

FaultingCodeOffset MacroAssembler::storeUncanonicalizedFloat32(
    FloatRegister src, const Address& dest) {
  MOZ_CRASH();
}

FaultingCodeOffset MacroAssembler::storeUncanonicalizedFloat32(
    FloatRegister src, const BaseIndex& dest) {
  MOZ_CRASH();
}

void MacroAssembler::addPtr(Imm32 imm, const Address& dest) { MOZ_CRASH(); }

void MacroAssembler::addPtr(const Address& src, Register dest) { MOZ_CRASH(); }

void MacroAssembler::subPtr(Register src, const Address& dest) { MOZ_CRASH(); }

void MacroAssembler::subPtr(const Address& addr, Register dest) { MOZ_CRASH(); }

void MacroAssembler::branchTruncateFloat32MaybeModUint32(FloatRegister src,
                                                         Register dest,
                                                         Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::branchTruncateDoubleMaybeModUint32(FloatRegister src,
                                                        Register dest,
                                                        Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::test32MovePtr(Condition cond, const Address& addr,
                                   Imm32 mask, Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::cmp32MovePtr(Condition cond, Register lhs, Imm32 rhs,
                                  Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::test32LoadPtr(Condition cond, const Address& addr,
                                   Imm32 mask, const Address& src,
                                   Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::spectreBoundsCheck32(Register index, Register length,
                                          Register maybeScratch,
                                          Label* failure) {
  MOZ_CRASH();
}

void MacroAssembler::spectreBoundsCheck32(Register index, const Address& length,
                                          Register maybeScratch,
                                          Label* failure) {
  MOZ_CRASH();
}

void MacroAssembler::spectreBoundsCheckPtr(Register index, Register length,
                                           Register maybeScratch,
                                           Label* failure) {
  MOZ_CRASH();
}

void MacroAssembler::spectreBoundsCheckPtr(Register index,
                                           const Address& length,
                                           Register maybeScratch,
                                           Label* failure) {
  MOZ_CRASH();
}

void MacroAssembler::cmp32Load32(Condition cond, Register lhs, Imm32 rhs,
                                 const Address& src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::cmp32LoadPtr(Condition cond, const Address& lhs, Imm32 rhs,
                                  const Address& src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::branchTruncateFloat32ToInt32(FloatRegister src,
                                                  Register dest, Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::branchTruncateDoubleToInt32(FloatRegister src,
                                                 Register dest, Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::mulDoublePtr(ImmPtr imm, Register temp,
                                  FloatRegister dest) {
  MOZ_CRASH();
}

template <typename T1, typename T2>
void MacroAssembler::cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::cmp64Set(Condition cond, Address lhs, Imm64 rhs,
                              Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestDouble(Condition cond, const ValueOperand& value,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestInt32Truthy(bool truthy,
                                           const ValueOperand& value,
                                           Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestNumber(Condition cond, const ValueOperand& value,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestSymbol(Condition cond, const ValueOperand& value,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBigInt(Condition cond, const ValueOperand& value,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestBoolean(Condition cond,
                                       const ValueOperand& value,
                                       Label* label) {
  MOZ_CRASH();
}

template <class L>
void MacroAssembler::branchTestMagic(Condition cond, const ValueOperand& value,
                                     L label) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestString(Condition cond, const ValueOperand& value,
                                      Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs, Imm32 rhs,
                                 Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs, Register rhs,
                                 Register src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::cmp32Move32(Condition cond, Register lhs,
                                 const Address& rhs, Register src,
                                 Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestUndefined(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchAdd64(Condition cond, Imm64 imm, Register64 dest,
                                 Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::quotient32(Register rhs, Register srcDest,
                                bool isUnsigned) {
  MOZ_CRASH();
}

void MacroAssembler::remainder32(Register rhs, Register srcDest,
                                 bool isUnsigned) {
  MOZ_CRASH();
}

void MacroAssembler::branch64(Condition cond, Register64 lhs, Imm64 val,
                              Label* success, Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::branch64(Condition cond, Register64 lhs, Register64 rhs,
                              Label* success, Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::branch64(Condition cond, const Address& lhs, Imm64 val,
                              Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branch64(Condition cond, const Address& lhs,
                              Register64 rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branch64(Condition cond, const Address& lhs,
                              const Address& rhs, Register scratch,
                              Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::minFloat32(FloatRegister other, FloatRegister srcDest,
                                bool handleNaN) {
  MOZ_CRASH();
}

void MacroAssembler::minDouble(FloatRegister other, FloatRegister srcDest,
                               bool handleNaN) {
  MOZ_CRASH();
}

void MacroAssembler::maxFloat32(FloatRegister other, FloatRegister srcDest,
                                bool handleNaN) {
  MOZ_CRASH();
}

void MacroAssembler::maxDouble(FloatRegister other, FloatRegister srcDest,
                               bool handleNaN) {
  MOZ_CRASH();
}

void MacroAssembler::rotateLeft(Imm32 count, Register input, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::rotateLeft(Register count, Register input, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::rotateLeft64(Imm32 count, Register64 input,
                                  Register64 dest, Register temp) {
  MOZ_CRASH();
}

void MacroAssembler::rotateLeft64(Register count, Register64 input,
                                  Register64 dest, Register temp) {
  MOZ_CRASH();
}

void MacroAssembler::rotateRight(Imm32 count, Register input, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::rotateRight(Register count, Register input,
                                 Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::rotateRight64(Imm32 count, Register64 input,
                                   Register64 dest, Register temp) {
  MOZ_CRASH();
}

void MacroAssembler::rotateRight64(Register count, Register64 input,
                                   Register64 dest, Register temp) {
  MOZ_CRASH();
}

void MacroAssembler::flexibleLshift32(Register shift, Register srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::flexibleRshift32(Register shift, Register srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::flexibleRshift32Arithmetic(Register shift,
                                                Register srcDest) {
  MOZ_CRASH();
}

void MacroAssembler::branchPrivatePtr(Condition cond, const Address& lhs,
                                      Register rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::clz32(Register src, Register dest, bool knownNotZero) {
  MOZ_CRASH();
}

void MacroAssembler::ctz32(Register src, Register dest, bool knownNotZero) {
  MOZ_CRASH();
}

void MacroAssembler::popcnt32(Register src, Register dest, Register temp) {
  MOZ_CRASH();
}

void MacroAssembler::moveFloat32ToGPR(FloatRegister src, Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::moveGPRToFloat32(Register src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestPrimitive(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::sqrtFloat32(FloatRegister src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::cmp16Set(Condition cond, Address lhs, Imm32 rhs,
                              Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::cmp8Set(Condition cond, Address lhs, Imm32 rhs,
                             Register dest) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::testNumberSet(Condition cond, const T& src,
                                   Register dest) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::testBooleanSet(Condition cond, const T& src,
                                    Register dest) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::testStringSet(Condition cond, const T& src,
                                   Register dest) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::testSymbolSet(Condition cond, const T& src,
                                   Register dest) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::testBigIntSet(Condition cond, const T& src,
                                   Register dest) {
  MOZ_CRASH();
}

template <typename T>
void MacroAssembler::branchTestGCThingImpl(Condition cond, const T& t,
                                           Label* label) {
  MOZ_CRASH();
}

//}}} check_macroassembler_style

}  // namespace js::jit

#endif /* jit_wasm32_MacroAssembler_wasm32_inl_h */
