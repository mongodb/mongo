/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_MacroAssembler_mips_shared_inl_h
#define jit_mips_shared_MacroAssembler_mips_shared_inl_h

#include "jit/mips-shared/MacroAssembler-mips-shared.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style

void
MacroAssembler::moveFloat32ToGPR(FloatRegister src, Register dest)
{
    moveFromFloat32(src, dest);
}

void
MacroAssembler::moveGPRToFloat32(Register src, FloatRegister dest)
{
    moveToFloat32(src, dest);
}

void
MacroAssembler::move8SignExtend(Register src, Register dest)
{
    ma_seb(dest, src);
}

void
MacroAssembler::move16SignExtend(Register src, Register dest)
{
    ma_seh(dest, src);
}

// ===============================================================
// Logical instructions

void
MacroAssembler::not32(Register reg)
{
    ma_not(reg, reg);
}

void
MacroAssembler::and32(Register src, Register dest)
{
    as_and(dest, dest, src);
}

void
MacroAssembler::and32(Imm32 imm, Register dest)
{
    ma_and(dest, imm);
}

void
MacroAssembler::and32(Imm32 imm, const Address& dest)
{
    load32(dest, SecondScratchReg);
    ma_and(SecondScratchReg, imm);
    store32(SecondScratchReg, dest);
}

void
MacroAssembler::and32(const Address& src, Register dest)
{
    load32(src, SecondScratchReg);
    ma_and(dest, SecondScratchReg);
}

void
MacroAssembler::or32(Register src, Register dest)
{
    ma_or(dest, src);
}

void
MacroAssembler::or32(Imm32 imm, Register dest)
{
    ma_or(dest, imm);
}

void
MacroAssembler::or32(Imm32 imm, const Address& dest)
{
    load32(dest, SecondScratchReg);
    ma_or(SecondScratchReg, imm);
    store32(SecondScratchReg, dest);
}

void
MacroAssembler::xor32(Register src, Register dest)
{
    ma_xor(dest, src);
}

void
MacroAssembler::xor32(Imm32 imm, Register dest)
{
    ma_xor(dest, imm);
}

// ===============================================================
// Arithmetic instructions

void
MacroAssembler::add32(Register src, Register dest)
{
    as_addu(dest, dest, src);
}

void
MacroAssembler::add32(Imm32 imm, Register dest)
{
    ma_addu(dest, dest, imm);
}

void
MacroAssembler::add32(Imm32 imm, const Address& dest)
{
    load32(dest, SecondScratchReg);
    ma_addu(SecondScratchReg, imm);
    store32(SecondScratchReg, dest);
}

void
MacroAssembler::addPtr(Imm32 imm, const Address& dest)
{
    loadPtr(dest, ScratchRegister);
    addPtr(imm, ScratchRegister);
    storePtr(ScratchRegister, dest);
}

void
MacroAssembler::addPtr(const Address& src, Register dest)
{
    loadPtr(src, ScratchRegister);
    addPtr(ScratchRegister, dest);
}

void
MacroAssembler::addDouble(FloatRegister src, FloatRegister dest)
{
    as_addd(dest, dest, src);
}

void
MacroAssembler::addFloat32(FloatRegister src, FloatRegister dest)
{
    as_adds(dest, dest, src);
}

void
MacroAssembler::sub32(Register src, Register dest)
{
    as_subu(dest, dest, src);
}

void
MacroAssembler::sub32(Imm32 imm, Register dest)
{
    ma_subu(dest, dest, imm);
}

void
MacroAssembler::sub32(const Address& src, Register dest)
{
    load32(src, SecondScratchReg);
    as_subu(dest, dest, SecondScratchReg);
}

void
MacroAssembler::subPtr(Register src, const Address& dest)
{
    loadPtr(dest, SecondScratchReg);
    subPtr(src, SecondScratchReg);
    storePtr(SecondScratchReg, dest);
}

void
MacroAssembler::subPtr(const Address& addr, Register dest)
{
    loadPtr(addr, SecondScratchReg);
    subPtr(SecondScratchReg, dest);
}

void
MacroAssembler::subDouble(FloatRegister src, FloatRegister dest)
{
    as_subd(dest, dest, src);
}

void
MacroAssembler::subFloat32(FloatRegister src, FloatRegister dest)
{
    as_subs(dest, dest, src);
}

void
MacroAssembler::mul32(Register rhs, Register srcDest)
{
    as_mul(srcDest, srcDest, rhs);
}

void
MacroAssembler::mulFloat32(FloatRegister src, FloatRegister dest)
{
    as_muls(dest, dest, src);
}

void
MacroAssembler::mulDouble(FloatRegister src, FloatRegister dest)
{
    as_muld(dest, dest, src);
}

void
MacroAssembler::mulDoublePtr(ImmPtr imm, Register temp, FloatRegister dest)
{
    movePtr(imm, ScratchRegister);
    loadDouble(Address(ScratchRegister, 0), ScratchDoubleReg);
    mulDouble(ScratchDoubleReg, dest);
}

void
MacroAssembler::quotient32(Register rhs, Register srcDest, bool isUnsigned)
{
    if (isUnsigned)
        as_divu(srcDest, rhs);
    else
        as_div(srcDest, rhs);
    as_mflo(srcDest);
}

void
MacroAssembler::remainder32(Register rhs, Register srcDest, bool isUnsigned)
{
    if (isUnsigned)
        as_divu(srcDest, rhs);
    else
        as_div(srcDest, rhs);
    as_mfhi(srcDest);
}

void
MacroAssembler::divFloat32(FloatRegister src, FloatRegister dest)
{
    as_divs(dest, dest, src);
}

void
MacroAssembler::divDouble(FloatRegister src, FloatRegister dest)
{
    as_divd(dest, dest, src);
}

void
MacroAssembler::neg32(Register reg)
{
    ma_negu(reg, reg);
}

void
MacroAssembler::negateDouble(FloatRegister reg)
{
    as_negd(reg, reg);
}

void
MacroAssembler::negateFloat(FloatRegister reg)
{
    as_negs(reg, reg);
}

void
MacroAssembler::absFloat32(FloatRegister src, FloatRegister dest)
{
    as_abss(dest, src);
}

void
MacroAssembler::absDouble(FloatRegister src, FloatRegister dest)
{
    as_absd(dest, src);
}

void
MacroAssembler::sqrtFloat32(FloatRegister src, FloatRegister dest)
{
    as_sqrts(dest, src);
}

void
MacroAssembler::sqrtDouble(FloatRegister src, FloatRegister dest)
{
    as_sqrtd(dest, src);
}

void
MacroAssembler::minFloat32(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    minMaxFloat32(srcDest, other, handleNaN, false);
}

void
MacroAssembler::minDouble(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    minMaxDouble(srcDest, other, handleNaN, false);
}

void
MacroAssembler::maxFloat32(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    minMaxFloat32(srcDest, other, handleNaN, true);
}

void
MacroAssembler::maxDouble(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    minMaxDouble(srcDest, other, handleNaN, true);
}

// ===============================================================
// Shift functions

void
MacroAssembler::lshift32(Register src, Register dest)
{
    ma_sll(dest, dest, src);
}

void
MacroAssembler::lshift32(Imm32 imm, Register dest)
{
    ma_sll(dest, dest, imm);
}

void
MacroAssembler::rshift32(Register src, Register dest)
{
    ma_srl(dest, dest, src);
}

void
MacroAssembler::rshift32(Imm32 imm, Register dest)
{
    ma_srl(dest, dest, imm);
}

void
MacroAssembler::rshift32Arithmetic(Register src, Register dest)
{
    ma_sra(dest, dest, src);
}

void
MacroAssembler::rshift32Arithmetic(Imm32 imm, Register dest)
{
    ma_sra(dest, dest, imm);
}

// ===============================================================
// Rotation functions
void
MacroAssembler::rotateLeft(Imm32 count, Register input, Register dest)
{
    if (count.value)
        ma_rol(dest, input, count);
    else
        ma_move(dest, input);
}
void
MacroAssembler::rotateLeft(Register count, Register input, Register dest)
{
    ma_rol(dest, input, count);
}
void
MacroAssembler::rotateRight(Imm32 count, Register input, Register dest)
{
    if (count.value)
        ma_ror(dest, input, count);
    else
        ma_move(dest, input);
}
void
MacroAssembler::rotateRight(Register count, Register input, Register dest)
{
    ma_ror(dest, input, count);
}

// ===============================================================
// Bit counting functions

void
MacroAssembler::clz32(Register src, Register dest, bool knownNotZero)
{
    as_clz(dest, src);
}

void
MacroAssembler::ctz32(Register src, Register dest, bool knownNotZero)
{
    ma_ctz(dest, src);
}

void
MacroAssembler::popcnt32(Register input,  Register output, Register tmp)
{
    // Equivalent to GCC output of mozilla::CountPopulation32()
    ma_move(output, input);
    ma_sra(tmp, input, Imm32(1));
    ma_and(tmp, Imm32(0x55555555));
    ma_subu(output, tmp);
    ma_sra(tmp, output, Imm32(2));
    ma_and(output, Imm32(0x33333333));
    ma_and(tmp, Imm32(0x33333333));
    ma_addu(output, tmp);
    ma_srl(tmp, output, Imm32(4));
    ma_addu(output, tmp);
    ma_and(output, Imm32(0xF0F0F0F));
    ma_sll(tmp, output, Imm32(8));
    ma_addu(output, tmp);
    ma_sll(tmp, output, Imm32(16));
    ma_addu(output, tmp);
    ma_sra(output, output, Imm32(24));
}

// ===============================================================
// Branch functions

template <class L>
void
MacroAssembler::branch32(Condition cond, Register lhs, Register rhs, L label)
{
    ma_b(lhs, rhs, label, cond);
}

template <class L>
void
MacroAssembler::branch32(Condition cond, Register lhs, Imm32 imm, L label)
{
    ma_b(lhs, imm, label, cond);
}

void
MacroAssembler::branch32(Condition cond, const Address& lhs, Register rhs, Label* label)
{
    load32(lhs, SecondScratchReg);
    ma_b(SecondScratchReg, rhs, label, cond);
}

void
MacroAssembler::branch32(Condition cond, const Address& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, SecondScratchReg);
    ma_b(SecondScratchReg, rhs, label, cond);
}

void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    load32(lhs, SecondScratchReg);
    ma_b(SecondScratchReg, rhs, label, cond);
}

void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, SecondScratchReg);
    ma_b(SecondScratchReg, rhs, label, cond);
}

void
MacroAssembler::branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, SecondScratchReg);
    ma_b(SecondScratchReg, rhs, label, cond);
}

void
MacroAssembler::branch32(Condition cond, wasm::SymbolicAddress addr, Imm32 imm, Label* label)
{
    load32(addr, SecondScratchReg);
    ma_b(SecondScratchReg, imm, label, cond);
}

template <class L>
void
MacroAssembler::branchPtr(Condition cond, Register lhs, Register rhs, L label)
{
    ma_b(lhs, rhs, label, cond);
}

void
MacroAssembler::branchPtr(Condition cond, Register lhs, Imm32 rhs, Label* label)
{
    ma_b(lhs, rhs, label, cond);
}

void
MacroAssembler::branchPtr(Condition cond, Register lhs, ImmPtr rhs, Label* label)
{
    ma_b(lhs, rhs, label, cond);
}

void
MacroAssembler::branchPtr(Condition cond, Register lhs, ImmGCPtr rhs, Label* label)
{
    ma_b(lhs, rhs, label, cond);
}

void
MacroAssembler::branchPtr(Condition cond, Register lhs, ImmWord rhs, Label* label)
{
    ma_b(lhs, rhs, label, cond);
}

template <class L>
void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, Register rhs, L label)
{
    loadPtr(lhs, SecondScratchReg);
    branchPtr(cond, SecondScratchReg, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmPtr rhs, Label* label)
{
    loadPtr(lhs, SecondScratchReg);
    branchPtr(cond, SecondScratchReg, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmGCPtr rhs, Label* label)
{
    loadPtr(lhs, SecondScratchReg);
    branchPtr(cond, SecondScratchReg, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmWord rhs, Label* label)
{
    loadPtr(lhs, SecondScratchReg);
    branchPtr(cond, SecondScratchReg, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    loadPtr(lhs, SecondScratchReg);
    branchPtr(cond, SecondScratchReg, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, ImmWord rhs, Label* label)
{
    loadPtr(lhs, SecondScratchReg);
    branchPtr(cond, SecondScratchReg, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, wasm::SymbolicAddress lhs, Register rhs, Label* label)
{
    loadPtr(lhs, SecondScratchReg);
    branchPtr(cond, SecondScratchReg, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs, ImmWord rhs, Label* label)
{
    loadPtr(lhs, SecondScratchReg);
    branchPtr(cond, SecondScratchReg, rhs, label);
}

template <typename T>
CodeOffsetJump
MacroAssembler::branchPtrWithPatch(Condition cond, Register lhs, T rhs, RepatchLabel* label)
{
    movePtr(rhs, ScratchRegister);
    Label skipJump;
    ma_b(lhs, ScratchRegister, &skipJump, InvertCondition(cond), ShortJump);
    CodeOffsetJump off = jumpWithPatch(label);
    bind(&skipJump);
    return off;
}

template <typename T>
CodeOffsetJump
MacroAssembler::branchPtrWithPatch(Condition cond, Address lhs, T rhs, RepatchLabel* label)
{
    loadPtr(lhs, SecondScratchReg);
    movePtr(rhs, ScratchRegister);
    Label skipJump;
    ma_b(SecondScratchReg, ScratchRegister, &skipJump, InvertCondition(cond), ShortJump);
    CodeOffsetJump off = jumpWithPatch(label);
    bind(&skipJump);
    return off;
}

void
MacroAssembler::branchFloat(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                            Label* label)
{
    ma_bc1s(lhs, rhs, label, cond);
}

void
MacroAssembler::branchTruncateFloat32ToInt32(FloatRegister src, Register dest, Label* fail)
{
    MOZ_CRASH();
}

void
MacroAssembler::branchDouble(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                             Label* label)
{
    ma_bc1d(lhs, rhs, label, cond);
}

void
MacroAssembler::branchTruncateDoubleToInt32(FloatRegister src, Register dest, Label* fail)
{
    MOZ_CRASH();
}

template <typename T, typename L>
void
MacroAssembler::branchAdd32(Condition cond, T src, Register dest, L overflow)
{
    switch (cond) {
      case Overflow:
        ma_addTestOverflow(dest, dest, src, overflow);
        break;
      case CarrySet:
        ma_addTestCarry(dest, dest, src, overflow);
        break;
      default:
        MOZ_CRASH("NYI");
    }
}

template <typename T>
void
MacroAssembler::branchSub32(Condition cond, T src, Register dest, Label* overflow)
{
    switch (cond) {
      case Overflow:
        ma_subTestOverflow(dest, dest, src, overflow);
        break;
      case NonZero:
      case Zero:
        ma_subu(dest, src);
        ma_b(dest, dest, overflow, cond);
        break;
      default:
        MOZ_CRASH("NYI");
    }
}

void
MacroAssembler::decBranchPtr(Condition cond, Register lhs, Imm32 rhs, Label* label)
{
    subPtr(rhs, lhs);
    branchPtr(cond, lhs, Imm32(0), label);
}

template <class L>
void
MacroAssembler::branchTest32(Condition cond, Register lhs, Register rhs, L label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    if (lhs == rhs) {
        ma_b(lhs, rhs, label, cond);
    } else {
        as_and(ScratchRegister, lhs, rhs);
        ma_b(ScratchRegister, ScratchRegister, label, cond);
    }
}

template <class L>
void
MacroAssembler::branchTest32(Condition cond, Register lhs, Imm32 rhs, L label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    ma_and(ScratchRegister, lhs, rhs);
    ma_b(ScratchRegister, ScratchRegister, label, cond);
}

void
MacroAssembler::branchTest32(Condition cond, const Address& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, SecondScratchReg);
    branchTest32(cond, SecondScratchReg, rhs, label);
}

void
MacroAssembler::branchTest32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, SecondScratchReg);
    branchTest32(cond, SecondScratchReg, rhs, label);
}

template <class L>
void
MacroAssembler::branchTestPtr(Condition cond, Register lhs, Register rhs, L label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    if (lhs == rhs) {
        ma_b(lhs, rhs, label, cond);
    } else {
        as_and(ScratchRegister, lhs, rhs);
        ma_b(ScratchRegister, ScratchRegister, label, cond);
    }
}

void
MacroAssembler::branchTestPtr(Condition cond, Register lhs, Imm32 rhs, Label* label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    ma_and(ScratchRegister, lhs, rhs);
    ma_b(ScratchRegister, ScratchRegister, label, cond);
}

void
MacroAssembler::branchTestPtr(Condition cond, const Address& lhs, Imm32 rhs, Label* label)
{
    loadPtr(lhs, SecondScratchReg);
    branchTestPtr(cond, SecondScratchReg, rhs, label);
}

void
MacroAssembler::branchTestUndefined(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_UNDEFINED), label, cond);
}

void
MacroAssembler::branchTestUndefined(Condition cond, const Address& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestUndefined(cond, scratch2, label);
}

void
MacroAssembler::branchTestUndefined(Condition cond, const BaseIndex& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestUndefined(cond, scratch2, label);
}

void
MacroAssembler::branchTestInt32(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_INT32), label, cond);
}

void
MacroAssembler::branchTestInt32(Condition cond, const Address& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestInt32(cond, scratch2, label);
}

void
MacroAssembler::branchTestInt32(Condition cond, const BaseIndex& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestInt32(cond, scratch2, label);
}

void
MacroAssembler::branchTestDouble(Condition cond, const Address& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestDouble(cond, scratch2, label);
}

void
MacroAssembler::branchTestDouble(Condition cond, const BaseIndex& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestDouble(cond, scratch2, label);
}

void
MacroAssembler::branchTestDoubleTruthy(bool b, FloatRegister value, Label* label)
{
    ma_lid(ScratchDoubleReg, 0.0);
    DoubleCondition cond = b ? DoubleNotEqual : DoubleEqualOrUnordered;
    ma_bc1d(value, ScratchDoubleReg, label, cond);
}

void
MacroAssembler::branchTestNumber(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    Condition actual = cond == Equal ? BelowOrEqual : Above;
    ma_b(tag, ImmTag(JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET), label, actual);
}

void
MacroAssembler::branchTestBoolean(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_BOOLEAN), label, cond);
}

void
MacroAssembler::branchTestBoolean(Condition cond, const Address& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestBoolean(cond, scratch2, label);
}

void
MacroAssembler::branchTestBoolean(Condition cond, const BaseIndex& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestBoolean(cond, scratch2, label);
}

void
MacroAssembler::branchTestString(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_STRING), label, cond);
}

void
MacroAssembler::branchTestString(Condition cond, const Address& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestString(cond, scratch2, label);
}

void
MacroAssembler::branchTestString(Condition cond, const BaseIndex& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestString(cond, scratch2, label);
}

void
MacroAssembler::branchTestSymbol(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_SYMBOL), label, cond);
}

void
MacroAssembler::branchTestSymbol(Condition cond, const BaseIndex& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestSymbol(cond, scratch2, label);
}

void
MacroAssembler::branchTestNull(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_NULL), label, cond);
}

void
MacroAssembler::branchTestNull(Condition cond, const Address& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestNull(cond, scratch2, label);
}

void
MacroAssembler::branchTestNull(Condition cond, const BaseIndex& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestNull(cond, scratch2, label);
}

void
MacroAssembler::branchTestObject(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_OBJECT), label, cond);
}

void
MacroAssembler::branchTestObject(Condition cond, const Address& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestObject(cond, scratch2, label);
}

void
MacroAssembler::branchTestObject(Condition cond, const BaseIndex& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestObject(cond, scratch2, label);
}

void
MacroAssembler::branchTestGCThing(Condition cond, const Address& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    ma_b(scratch2, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET), label,
         (cond == Equal) ? AboveOrEqual : Below);
}
void
MacroAssembler::branchTestGCThing(Condition cond, const BaseIndex& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    ma_b(scratch2, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET), label,
         (cond == Equal) ? AboveOrEqual : Below);
}

void
MacroAssembler::branchTestPrimitive(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET), label,
         (cond == Equal) ? Below : AboveOrEqual);
}

void
MacroAssembler::branchTestMagic(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_MAGIC), label, cond);
}

void
MacroAssembler::branchTestMagic(Condition cond, const Address& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestMagic(cond, scratch2, label);
}

void
MacroAssembler::branchTestMagic(Condition cond, const BaseIndex& address, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    extractTag(address, scratch2);
    branchTestMagic(cond, scratch2, label);
}

void
MacroAssembler::branchToComputedAddress(const BaseIndex& addr)
{
    loadPtr(addr, ScratchRegister);
    branch(ScratchRegister);
}

void
MacroAssembler::cmp32Move32(Condition cond, Register lhs, Register rhs, Register src,
                            Register dest)
{
    MOZ_CRASH();
}

void
MacroAssembler::cmp32MovePtr(Condition cond, Register lhs, Imm32 rhs, Register src,
                             Register dest)
{
    MOZ_CRASH();
}

void
MacroAssembler::cmp32Move32(Condition cond, Register lhs, const Address& rhs, Register src,
                            Register dest)
{
    MOZ_CRASH();
}

void
MacroAssembler::test32LoadPtr(Condition cond, const Address& addr, Imm32 mask, const Address& src,
                              Register dest)
{
    MOZ_RELEASE_ASSERT(!JitOptions.spectreStringMitigations);
    Label skip;
    branchTest32(Assembler::InvertCondition(cond), addr, mask, &skip);
    loadPtr(src, dest);
    bind(&skip);
}

void
MacroAssembler::test32MovePtr(Condition cond, const Address& addr, Imm32 mask, Register src,
                              Register dest)
{
    MOZ_CRASH();
}

void
MacroAssembler::spectreBoundsCheck32(Register index, Register length, Register maybeScratch,
                                     Label* failure)
{
    MOZ_RELEASE_ASSERT(!JitOptions.spectreIndexMasking);
    branch32(Assembler::BelowOrEqual, length, index, failure);
}

void
MacroAssembler::spectreBoundsCheck32(Register index, const Address& length, Register maybeScratch,
                                     Label* failure)
{
    MOZ_RELEASE_ASSERT(!JitOptions.spectreIndexMasking);
    branch32(Assembler::BelowOrEqual, length, index, failure);
}

void
MacroAssembler::spectreMovePtr(Condition cond, Register src, Register dest)
{
    MOZ_CRASH();
}

void
MacroAssembler::spectreZeroRegister(Condition cond, Register scratch, Register dest)
{
    MOZ_CRASH();
}

// ========================================================================
// Memory access primitives.
void
MacroAssembler::storeFloat32x3(FloatRegister src, const Address& dest)
{
    MOZ_CRASH("NYI");
}
void
MacroAssembler::storeFloat32x3(FloatRegister src, const BaseIndex& dest)
{
    MOZ_CRASH("NYI");
}

void
MacroAssembler::storeUncanonicalizedDouble(FloatRegister src, const Address& addr)
{
    ma_sd(src, addr);
}
void
MacroAssembler::storeUncanonicalizedDouble(FloatRegister src, const BaseIndex& addr)
{
    ma_sd(src, addr);
}

void
MacroAssembler::storeUncanonicalizedFloat32(FloatRegister src, const Address& addr)
{
    ma_ss(src, addr);
}
void
MacroAssembler::storeUncanonicalizedFloat32(FloatRegister src, const BaseIndex& addr)
{
    ma_ss(src, addr);
}

void
MacroAssembler::memoryBarrier(MemoryBarrierBits barrier)
{
    as_sync();
}

// ===============================================================
// Clamping functions.

void
MacroAssembler::clampIntToUint8(Register reg)
{
    // If reg is < 0, then we want to clamp to 0.
    as_slti(ScratchRegister, reg, 0);
    as_movn(reg, zero, ScratchRegister);

    // If reg is >= 255, then we want to clamp to 255.
    ma_li(SecondScratchReg, Imm32(255));
    as_slti(ScratchRegister, reg, 255);
    as_movz(reg, SecondScratchReg, ScratchRegister);
}

//}}} check_macroassembler_style
// ===============================================================

} // namespace jit
} // namespace js

#endif /* jit_mips_shared_MacroAssembler_mips_shared_inl_h */
