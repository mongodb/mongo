/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_MacroAssembler_x64_inl_h
#define jit_x64_MacroAssembler_x64_inl_h

#include "jit/x64/MacroAssembler-x64.h"

#include "jit/x86-shared/MacroAssembler-x86-shared-inl.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style
// ===============================================================

void
MacroAssembler::move64(Imm64 imm, Register64 dest)
{
    movq(ImmWord(imm.value), dest.reg);
}

void
MacroAssembler::move64(Register64 src, Register64 dest)
{
    movq(src.reg, dest.reg);
}

void
MacroAssembler::moveDoubleToGPR64(FloatRegister src, Register64 dest)
{
    vmovq(src, dest.reg);
}

void
MacroAssembler::moveGPR64ToDouble(Register64 src, FloatRegister dest)
{
    vmovq(src.reg, dest);
}

void
MacroAssembler::move64To32(Register64 src, Register dest)
{
    movl(src.reg, dest);
}

void
MacroAssembler::move32To64ZeroExtend(Register src, Register64 dest)
{
    movl(src, dest.reg);
}

void
MacroAssembler::move8To64SignExtend(Register src, Register64 dest)
{
    movsbq(Operand(src), dest.reg);
}

void
MacroAssembler::move16To64SignExtend(Register src, Register64 dest)
{
    movswq(Operand(src), dest.reg);
}

void
MacroAssembler::move32To64SignExtend(Register src, Register64 dest)
{
    movslq(src, dest.reg);
}

void
MacroAssembler::andPtr(Register src, Register dest)
{
    andq(src, dest);
}

void
MacroAssembler::andPtr(Imm32 imm, Register dest)
{
    andq(imm, dest);
}

void
MacroAssembler::and64(Imm64 imm, Register64 dest)
{
    if (INT32_MIN <= int64_t(imm.value) && int64_t(imm.value) <= INT32_MAX) {
        andq(Imm32(imm.value), dest.reg);
    } else {
        ScratchRegisterScope scratch(*this);
        movq(ImmWord(uintptr_t(imm.value)), scratch);
        andq(scratch, dest.reg);
    }
}

void
MacroAssembler::or64(Imm64 imm, Register64 dest)
{
    if (INT32_MIN <= int64_t(imm.value) && int64_t(imm.value) <= INT32_MAX) {
        orq(Imm32(imm.value), dest.reg);
    } else {
        ScratchRegisterScope scratch(*this);
        movq(ImmWord(uintptr_t(imm.value)), scratch);
        orq(scratch, dest.reg);
    }
}

void
MacroAssembler::xor64(Imm64 imm, Register64 dest)
{
    if (INT32_MIN <= int64_t(imm.value) && int64_t(imm.value) <= INT32_MAX) {
        xorq(Imm32(imm.value), dest.reg);
    } else {
        ScratchRegisterScope scratch(*this);
        movq(ImmWord(uintptr_t(imm.value)), scratch);
        xorq(scratch, dest.reg);
    }
}

void
MacroAssembler::orPtr(Register src, Register dest)
{
    orq(src, dest);
}

void
MacroAssembler::orPtr(Imm32 imm, Register dest)
{
    orq(imm, dest);
}

void
MacroAssembler::and64(Register64 src, Register64 dest)
{
    andq(src.reg, dest.reg);
}

void
MacroAssembler::or64(Register64 src, Register64 dest)
{
    orq(src.reg, dest.reg);
}

void
MacroAssembler::xor64(Register64 src, Register64 dest)
{
    xorq(src.reg, dest.reg);
}

void
MacroAssembler::xorPtr(Register src, Register dest)
{
    xorq(src, dest);
}

void
MacroAssembler::xorPtr(Imm32 imm, Register dest)
{
    xorq(imm, dest);
}

void
MacroAssembler::and64(const Operand& src, Register64 dest)
{
    andq(src, dest.reg);
}

void
MacroAssembler::or64(const Operand& src, Register64 dest)
{
    orq(src, dest.reg);
}

void
MacroAssembler::xor64(const Operand& src, Register64 dest)
{
    xorq(src, dest.reg);
}

// ===============================================================
// Arithmetic functions

void
MacroAssembler::addPtr(Register src, Register dest)
{
    addq(src, dest);
}

void
MacroAssembler::addPtr(Imm32 imm, Register dest)
{
    addq(imm, dest);
}

void
MacroAssembler::addPtr(ImmWord imm, Register dest)
{
    ScratchRegisterScope scratch(*this);
    MOZ_ASSERT(dest != scratch);
    if ((intptr_t)imm.value <= INT32_MAX && (intptr_t)imm.value >= INT32_MIN) {
        addq(Imm32((int32_t)imm.value), dest);
    } else {
        mov(imm, scratch);
        addq(scratch, dest);
    }
}

void
MacroAssembler::addPtr(Imm32 imm, const Address& dest)
{
    addq(imm, Operand(dest));
}

void
MacroAssembler::addPtr(Imm32 imm, const AbsoluteAddress& dest)
{
    addq(imm, Operand(dest));
}

void
MacroAssembler::addPtr(const Address& src, Register dest)
{
    addq(Operand(src), dest);
}

void
MacroAssembler::add64(const Operand& src, Register64 dest)
{
    addq(src, dest.reg);
}

void
MacroAssembler::add64(Register64 src, Register64 dest)
{
    addq(src.reg, dest.reg);
}

void
MacroAssembler::add64(Imm32 imm, Register64 dest)
{
    addq(imm, dest.reg);
}

void
MacroAssembler::add64(Imm64 imm, Register64 dest)
{
    addPtr(ImmWord(imm.value), dest.reg);
}

CodeOffset
MacroAssembler::sub32FromStackPtrWithPatch(Register dest)
{
    moveStackPtrTo(dest);
    addqWithPatch(Imm32(0), dest);
    return CodeOffset(currentOffset());
}

void
MacroAssembler::patchSub32FromStackPtr(CodeOffset offset, Imm32 imm)
{
    patchAddq(offset, -imm.value);
}

void
MacroAssembler::subPtr(Register src, Register dest)
{
    subq(src, dest);
}

void
MacroAssembler::subPtr(Register src, const Address& dest)
{
    subq(src, Operand(dest));
}

void
MacroAssembler::subPtr(Imm32 imm, Register dest)
{
    subq(imm, dest);
}

void
MacroAssembler::subPtr(ImmWord imm, Register dest)
{
    ScratchRegisterScope scratch(*this);
    MOZ_ASSERT(dest != scratch);
    if ((intptr_t)imm.value <= INT32_MAX && (intptr_t)imm.value >= INT32_MIN) {
        subq(Imm32((int32_t)imm.value), dest);
    } else {
        mov(imm, scratch);
        subq(scratch, dest);
    }
}

void
MacroAssembler::subPtr(const Address& addr, Register dest)
{
    subq(Operand(addr), dest);
}

void
MacroAssembler::sub64(const Operand& src, Register64 dest)
{
    subq(src, dest.reg);
}

void
MacroAssembler::sub64(Register64 src, Register64 dest)
{
    subq(src.reg, dest.reg);
}

void
MacroAssembler::sub64(Imm64 imm, Register64 dest)
{
    subPtr(ImmWord(imm.value), dest.reg);
}

void
MacroAssembler::mul64(Imm64 imm, const Register64& dest, const Register temp)
{
    MOZ_ASSERT(temp == InvalidReg);
    mul64(imm, dest);
}

void
MacroAssembler::mul64(Imm64 imm, const Register64& dest)
{
    movq(ImmWord(uintptr_t(imm.value)), ScratchReg);
    imulq(ScratchReg, dest.reg);
}

void
MacroAssembler::mul64(const Register64& src, const Register64& dest, const Register temp)
{
    MOZ_ASSERT(temp == InvalidReg);
    mul64(Operand(src.reg), dest);
}

void
MacroAssembler::mul64(const Operand& src, const Register64& dest)
{
    imulq(src, dest.reg);
}

void
MacroAssembler::mul64(const Operand& src, const Register64& dest, const Register temp)
{
    MOZ_ASSERT(temp == InvalidReg);
    mul64(src, dest);
}

void
MacroAssembler::mulBy3(Register src, Register dest)
{
    lea(Operand(src, src, TimesTwo), dest);
}

void
MacroAssembler::mulDoublePtr(ImmPtr imm, Register temp, FloatRegister dest)
{
    movq(imm, ScratchReg);
    vmulsd(Operand(ScratchReg, 0), dest, dest);
}

void
MacroAssembler::inc64(AbsoluteAddress dest)
{
    if (X86Encoding::IsAddressImmediate(dest.addr)) {
        addPtr(Imm32(1), dest);
    } else {
        ScratchRegisterScope scratch(*this);
        mov(ImmPtr(dest.addr), scratch);
        addPtr(Imm32(1), Address(scratch, 0));
    }
}

void
MacroAssembler::neg64(Register64 reg)
{
    negq(reg.reg);
}

// ===============================================================
// Shift functions

void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    shlq(imm, dest);
}

void
MacroAssembler::lshift64(Imm32 imm, Register64 dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    lshiftPtr(imm, dest.reg);
}

void
MacroAssembler::lshift64(Register shift, Register64 srcDest)
{
    MOZ_ASSERT(shift == rcx);
    shlq_cl(srcDest.reg);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    shrq(imm, dest);
}

void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    rshiftPtr(imm, dest.reg);
}

void
MacroAssembler::rshift64(Register shift, Register64 srcDest)
{
    MOZ_ASSERT(shift == rcx);
    shrq_cl(srcDest.reg);
}

void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    sarq(imm, dest);
}

void
MacroAssembler::rshift64Arithmetic(Imm32 imm, Register64 dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    rshiftPtrArithmetic(imm, dest.reg);
}

void
MacroAssembler::rshift64Arithmetic(Register shift, Register64 srcDest)
{
    MOZ_ASSERT(shift == rcx);
    sarq_cl(srcDest.reg);
}

// ===============================================================
// Rotation functions

void
MacroAssembler::rotateLeft64(Register count, Register64 src, Register64 dest)
{
    MOZ_ASSERT(src == dest, "defineReuseInput");
    MOZ_ASSERT(count == ecx, "defineFixed(ecx)");

    rolq_cl(dest.reg);
}

void
MacroAssembler::rotateLeft64(Register count, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == InvalidReg);
    rotateLeft64(count, src, dest);
}

void
MacroAssembler::rotateRight64(Register count, Register64 src, Register64 dest)
{
    MOZ_ASSERT(src == dest, "defineReuseInput");
    MOZ_ASSERT(count == ecx, "defineFixed(ecx)");

    rorq_cl(dest.reg);
}

void
MacroAssembler::rotateRight64(Register count, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == InvalidReg);
    rotateRight64(count, src, dest);
}

void
MacroAssembler::rotateLeft64(Imm32 count, Register64 src, Register64 dest)
{
    MOZ_ASSERT(src == dest, "defineReuseInput");
    rolq(count, dest.reg);
}

void
MacroAssembler::rotateLeft64(Imm32 count, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == InvalidReg);
    rotateLeft64(count, src, dest);
}

void
MacroAssembler::rotateRight64(Imm32 count, Register64 src, Register64 dest)
{
    MOZ_ASSERT(src == dest, "defineReuseInput");
    rorq(count, dest.reg);
}

void
MacroAssembler::rotateRight64(Imm32 count, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == InvalidReg);
    rotateRight64(count, src, dest);
}

// ===============================================================
// Condition functions

template <typename T1, typename T2>
void
MacroAssembler::cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest)
{
    cmpPtr(lhs, rhs);
    emitSet(cond, dest);
}

// ===============================================================
// Bit counting functions

void
MacroAssembler::clz64(Register64 src, Register dest)
{
    // On very recent chips (Haswell and newer) there is actually an
    // LZCNT instruction that does all of this.

    Label nonzero;
    bsrq(src.reg, dest);
    j(Assembler::NonZero, &nonzero);
    movq(ImmWord(0x7F), dest);
    bind(&nonzero);
    xorq(Imm32(0x3F), dest);
}

void
MacroAssembler::ctz64(Register64 src, Register dest)
{
    Label nonzero;
    bsfq(src.reg, dest);
    j(Assembler::NonZero, &nonzero);
    movq(ImmWord(64), dest);
    bind(&nonzero);
}

void
MacroAssembler::popcnt64(Register64 src64, Register64 dest64, Register tmp)
{
    Register src = src64.reg;
    Register dest = dest64.reg;

    if (AssemblerX86Shared::HasPOPCNT()) {
        MOZ_ASSERT(tmp == InvalidReg);
        popcntq(src, dest);
        return;
    }

    if (src != dest)
        movq(src, dest);

    MOZ_ASSERT(tmp != dest);

    ScratchRegisterScope scratch(*this);

    // Equivalent to mozilla::CountPopulation32, adapted for 64 bits.
    // x -= (x >> 1) & m1;
    movq(src, tmp);
    movq(ImmWord(0x5555555555555555), scratch);
    shrq(Imm32(1), tmp);
    andq(scratch, tmp);
    subq(tmp, dest);

    // x = (x & m2) + ((x >> 2) & m2);
    movq(dest, tmp);
    movq(ImmWord(0x3333333333333333), scratch);
    andq(scratch, dest);
    shrq(Imm32(2), tmp);
    andq(scratch, tmp);
    addq(tmp, dest);

    // x = (x + (x >> 4)) & m4;
    movq(dest, tmp);
    movq(ImmWord(0x0f0f0f0f0f0f0f0f), scratch);
    shrq(Imm32(4), tmp);
    addq(tmp, dest);
    andq(scratch, dest);

    // (x * h01) >> 56
    movq(ImmWord(0x0101010101010101), scratch);
    imulq(scratch, dest);
    shrq(Imm32(56), dest);
}

// ===============================================================
// Branch functions

void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    if (X86Encoding::IsAddressImmediate(lhs.addr)) {
        branch32(cond, Operand(lhs), rhs, label);
    } else {
        ScratchRegisterScope scratch(*this);
        mov(ImmPtr(lhs.addr), scratch);
        branch32(cond, Address(scratch, 0), rhs, label);
    }
}
void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    if (X86Encoding::IsAddressImmediate(lhs.addr)) {
        branch32(cond, Operand(lhs), rhs, label);
    } else {
        ScratchRegisterScope scratch(*this);
        mov(ImmPtr(lhs.addr), scratch);
        branch32(cond, Address(scratch, 0), rhs, label);
    }
}

void
MacroAssembler::branch32(Condition cond, wasm::SymbolicAddress lhs, Imm32 rhs, Label* label)
{
    ScratchRegisterScope scratch(*this);
    mov(lhs, scratch);
    branch32(cond, Address(scratch, 0), rhs, label);
}

void
MacroAssembler::branch64(Condition cond, Register64 lhs, Imm64 val, Label* success, Label* fail)
{
    MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal ||
               cond == Assembler::LessThan || cond == Assembler::LessThanOrEqual ||
               cond == Assembler::GreaterThan || cond == Assembler::GreaterThanOrEqual ||
               cond == Assembler::Below || cond == Assembler::BelowOrEqual ||
               cond == Assembler::Above || cond == Assembler::AboveOrEqual,
               "other condition codes not supported");

    branchPtr(cond, lhs.reg, ImmWord(val.value), success);
    if (fail)
        jump(fail);
}

void
MacroAssembler::branch64(Condition cond, Register64 lhs, Register64 rhs, Label* success, Label* fail)
{
    MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal ||
               cond == Assembler::LessThan || cond == Assembler::LessThanOrEqual ||
               cond == Assembler::GreaterThan || cond == Assembler::GreaterThanOrEqual ||
               cond == Assembler::Below || cond == Assembler::BelowOrEqual ||
               cond == Assembler::Above || cond == Assembler::AboveOrEqual,
               "other condition codes not supported");

    branchPtr(cond, lhs.reg, rhs.reg, success);
    if (fail)
        jump(fail);
}

void
MacroAssembler::branch64(Condition cond, const Address& lhs, Imm64 val, Label* label)
{
    MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
               "other condition codes not supported");

    branchPtr(cond, lhs, ImmWord(val.value), label);
}

void
MacroAssembler::branch64(Condition cond, const Address& lhs, const Address& rhs, Register scratch,
                         Label* label)
{
    MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
               "other condition codes not supported");
    MOZ_ASSERT(lhs.base != scratch);
    MOZ_ASSERT(rhs.base != scratch);

    loadPtr(rhs, scratch);
    branchPtr(cond, lhs, scratch, label);
}

void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    ScratchRegisterScope scratch(*this);
    MOZ_ASSERT(rhs != scratch);
    if (X86Encoding::IsAddressImmediate(lhs.addr)) {
        branchPtrImpl(cond, Operand(lhs), rhs, label);
    } else {
        mov(ImmPtr(lhs.addr), scratch);
        branchPtrImpl(cond, Operand(scratch, 0x0), rhs, label);
    }
}

void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, ImmWord rhs, Label* label)
{
    if (X86Encoding::IsAddressImmediate(lhs.addr)) {
        branchPtrImpl(cond, Operand(lhs), rhs, label);
    } else {
        ScratchRegisterScope scratch(*this);
        mov(ImmPtr(lhs.addr), scratch);
        branchPtrImpl(cond, Operand(scratch, 0x0), rhs, label);
    }
}

void
MacroAssembler::branchPtr(Condition cond, wasm::SymbolicAddress lhs, Register rhs, Label* label)
{
    ScratchRegisterScope scratch(*this);
    MOZ_ASSERT(rhs != scratch);
    mov(lhs, scratch);
    branchPtrImpl(cond, Operand(scratch, 0x0), rhs, label);
}

void
MacroAssembler::branchPrivatePtr(Condition cond, const Address& lhs, Register rhs, Label* label)
{
    ScratchRegisterScope scratch(*this);
    if (rhs != scratch)
        movePtr(rhs, scratch);
    // Instead of unboxing lhs, box rhs and do direct comparison with lhs.
    rshiftPtr(Imm32(1), scratch);
    branchPtr(cond, lhs, scratch, label);
}

void
MacroAssembler::branchTruncateFloat32ToPtr(FloatRegister src, Register dest, Label* fail)
{
    vcvttss2sq(src, dest);

    // Same trick as for Doubles
    cmpPtr(dest, Imm32(1));
    j(Assembler::Overflow, fail);
}

void
MacroAssembler::branchTruncateFloat32MaybeModUint32(FloatRegister src, Register dest, Label* fail)
{
    branchTruncateFloat32ToPtr(src, dest, fail);
    movl(dest, dest); // Zero upper 32-bits.
}

void
MacroAssembler::branchTruncateFloat32ToInt32(FloatRegister src, Register dest, Label* fail)
{
    branchTruncateFloat32ToPtr(src, dest, fail);
    branch32(Assembler::Above, dest, Imm32(0xffffffff), fail);
}

void
MacroAssembler::branchTruncateDoubleToPtr(FloatRegister src, Register dest, Label* fail)
{
    vcvttsd2sq(src, dest);

    // vcvttsd2sq returns 0x8000000000000000 on failure. Test for it by
    // subtracting 1 and testing overflow (this avoids the need to
    // materialize that value in a register).
    cmpPtr(dest, Imm32(1));
    j(Assembler::Overflow, fail);
}

void
MacroAssembler::branchTruncateDoubleMaybeModUint32(FloatRegister src, Register dest, Label* fail)
{
    branchTruncateDoubleToPtr(src, dest, fail);
    movl(dest, dest); // Zero upper 32-bits.
}

void
MacroAssembler::branchTruncateDoubleToInt32(FloatRegister src, Register dest, Label* fail)
{
    branchTruncateDoubleToPtr(src, dest, fail);
    branch32(Assembler::Above, dest, Imm32(0xffffffff), fail);
}

void
MacroAssembler::branchTest32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    if (X86Encoding::IsAddressImmediate(lhs.addr)) {
        test32(Operand(lhs), rhs);
    } else {
        ScratchRegisterScope scratch(*this);
        mov(ImmPtr(lhs.addr), scratch);
        test32(Operand(scratch, 0), rhs);
    }
    j(cond, label);
}

template <class L>
void
MacroAssembler::branchTest64(Condition cond, Register64 lhs, Register64 rhs, Register temp,
                             L label)
{
    branchTestPtr(cond, lhs.reg, rhs.reg, label);
}

void
MacroAssembler::branchTestBooleanTruthy(bool truthy, const ValueOperand& value, Label* label)
{
    test32(value.valueReg(), value.valueReg());
    j(truthy ? NonZero : Zero, label);
}

void
MacroAssembler::branchTestMagic(Condition cond, const Address& valaddr, JSWhyMagic why, Label* label)
{
    uint64_t magic = MagicValue(why).asRawBits();
    cmpPtr(valaddr, ImmWord(magic));
    j(cond, label);
}

void
MacroAssembler::branchToComputedAddress(const BaseIndex& address)
{
    jmp(Operand(address));
}

void
MacroAssembler::cmp32MovePtr(Condition cond, Register lhs, Imm32 rhs, Register src,
                             Register dest)
{
    cmp32(lhs, rhs);
    cmovCCq(cond, Operand(src), dest);
}

void
MacroAssembler::test32LoadPtr(Condition cond, const Address& addr, Imm32 mask, const Address& src,
                              Register dest)
{
    MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);
    test32(addr, mask);
    cmovCCq(cond, Operand(src), dest);
}

void
MacroAssembler::test32MovePtr(Condition cond, const Address& addr, Imm32 mask, Register src,
                              Register dest)
{
    MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);
    test32(addr, mask);
    cmovCCq(cond, Operand(src), dest);
}

void
MacroAssembler::spectreMovePtr(Condition cond, Register src, Register dest)
{
    cmovCCq(cond, Operand(src), dest);
}

void
MacroAssembler::spectreBoundsCheck32(Register index, Register length, Register maybeScratch,
                                     Label* failure)
{
    MOZ_ASSERT(length != maybeScratch);
    MOZ_ASSERT(index != maybeScratch);

    ScratchRegisterScope scratch(*this);
    MOZ_ASSERT(index != scratch);
    MOZ_ASSERT(length != scratch);

    if (JitOptions.spectreIndexMasking)
        move32(Imm32(0), scratch);

    cmp32(index, length);
    j(Assembler::AboveOrEqual, failure);

    if (JitOptions.spectreIndexMasking)
        cmovCCl(Assembler::AboveOrEqual, scratch, index);
}

void
MacroAssembler::spectreBoundsCheck32(Register index, const Address& length, Register maybeScratch,
                                     Label* failure)
{
    MOZ_ASSERT(index != length.base);
    MOZ_ASSERT(length.base != maybeScratch);
    MOZ_ASSERT(index != maybeScratch);

    ScratchRegisterScope scratch(*this);
    MOZ_ASSERT(index != scratch);
    MOZ_ASSERT(length.base != scratch);

    if (JitOptions.spectreIndexMasking)
        move32(Imm32(0), scratch);

    cmp32(index, Operand(length));
    j(Assembler::AboveOrEqual, failure);

    if (JitOptions.spectreIndexMasking)
        cmovCCl(Assembler::AboveOrEqual, scratch, index);
}

// ========================================================================
// Truncate floating point.

void
MacroAssembler::truncateFloat32ToUInt64(Address src, Address dest, Register temp,
                                        FloatRegister floatTemp)
{
    Label done;

    loadFloat32(src, floatTemp);

    truncateFloat32ToInt64(src, dest, temp);

    // For unsigned conversion the case of [INT64, UINT64] needs to get handle seperately.
    loadPtr(dest, temp);
    branchPtr(Assembler::Condition::NotSigned, temp, Imm32(0), &done);

    // Move the value inside INT64 range.
    storeFloat32(floatTemp, dest);
    loadConstantFloat32(double(int64_t(0x8000000000000000)), floatTemp);
    vaddss(Operand(dest), floatTemp, floatTemp);
    storeFloat32(floatTemp, dest);
    truncateFloat32ToInt64(dest, dest, temp);

    loadPtr(dest, temp);
    or64(Imm64(0x8000000000000000), Register64(temp));
    storePtr(temp, dest);

    bind(&done);
}

void
MacroAssembler::truncateDoubleToUInt64(Address src, Address dest, Register temp,
                                       FloatRegister floatTemp)
{
    Label done;

    loadDouble(src, floatTemp);

    truncateDoubleToInt64(src, dest, temp);

    // For unsigned conversion the case of [INT64, UINT64] needs to get handle seperately.
    loadPtr(dest, temp);
    branchPtr(Assembler::Condition::NotSigned, temp, Imm32(0), &done);

    // Move the value inside INT64 range.
    storeDouble(floatTemp, dest);
    loadConstantDouble(double(int64_t(0x8000000000000000)), floatTemp);
    vaddsd(Operand(dest), floatTemp, floatTemp);
    storeDouble(floatTemp, dest);
    truncateDoubleToInt64(dest, dest, temp);

    loadPtr(dest, temp);
    or64(Imm64(0x8000000000000000), Register64(temp));
    storePtr(temp, dest);

    bind(&done);
}

//}}} check_macroassembler_style
// ===============================================================

void
MacroAssemblerX64::incrementInt32Value(const Address& addr)
{
    asMasm().addPtr(Imm32(1), addr);
}

void
MacroAssemblerX64::unboxValue(const ValueOperand& src, AnyRegister dest, JSValueType type)
{
    if (dest.isFloat()) {
        Label notInt32, end;
        asMasm().branchTestInt32(Assembler::NotEqual, src, &notInt32);
        convertInt32ToDouble(src.valueReg(), dest.fpu());
        jump(&end);
        bind(&notInt32);
        unboxDouble(src, dest.fpu());
        bind(&end);
    } else {
        unboxNonDouble(src, dest.gpr(), type);
    }
}

template <typename T>
void
MacroAssemblerX64::loadInt32OrDouble(const T& src, FloatRegister dest)
{
    Label notInt32, end;
    asMasm().branchTestInt32(Assembler::NotEqual, src, &notInt32);
    convertInt32ToDouble(src, dest);
    jump(&end);
    bind(&notInt32);
    loadDouble(src, dest);
    bind(&end);
}

// If source is a double, load it into dest. If source is int32,
// convert it to double. Else, branch to failure.
void
MacroAssemblerX64::ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure)
{
    Label isDouble, done;
    {
        ScratchTagScope tag(asMasm(), source);
        splitTagForTest(source, tag);
        asMasm().branchTestDouble(Assembler::Equal, tag, &isDouble);
        asMasm().branchTestInt32(Assembler::NotEqual, tag, failure);
    }

    ScratchRegisterScope scratch(asMasm());
    unboxInt32(source, scratch);
    convertInt32ToDouble(scratch, dest);
    jump(&done);

    bind(&isDouble);
    unboxDouble(source, dest);

    bind(&done);
}

} // namespace jit
} // namespace js

#endif /* jit_x64_MacroAssembler_x64_inl_h */
