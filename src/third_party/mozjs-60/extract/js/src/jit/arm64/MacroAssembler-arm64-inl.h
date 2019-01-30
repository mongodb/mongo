/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_MacroAssembler_arm64_inl_h
#define jit_arm64_MacroAssembler_arm64_inl_h

#include "jit/arm64/MacroAssembler-arm64.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style

void
MacroAssembler::move64(Register64 src, Register64 dest)
{
    Mov(ARMRegister(dest.reg, 64), ARMRegister(src.reg, 64));
}

void
MacroAssembler::move64(Imm64 imm, Register64 dest)
{
    Mov(ARMRegister(dest.reg, 64), imm.value);
}

void
MacroAssembler::moveFloat32ToGPR(FloatRegister src, Register dest)
{
    Fmov(ARMRegister(dest, 32), ARMFPRegister(src, 32));
}

void
MacroAssembler::moveGPRToFloat32(Register src, FloatRegister dest)
{
    Fmov(ARMFPRegister(dest, 32), ARMRegister(src, 32));
}

void
MacroAssembler::move8SignExtend(Register src, Register dest)
{
    Sxtb(ARMRegister(dest, 32), ARMRegister(src, 32));
}

void
MacroAssembler::move16SignExtend(Register src, Register dest)
{
    Sxth(ARMRegister(dest, 32), ARMRegister(src, 32));
}

void
MacroAssembler::moveDoubleToGPR64(FloatRegister src, Register64 dest)
{
    Fmov(ARMRegister(dest.reg, 64), ARMFPRegister(src, 64));
}

void
MacroAssembler::moveGPR64ToDouble(Register64 src, FloatRegister dest)
{
    Fmov(ARMFPRegister(dest, 64), ARMRegister(src.reg, 64));
}

void
MacroAssembler::move64To32(Register64 src, Register dest)
{
    Mov(ARMRegister(dest, 32), ARMRegister(src.reg, 32));
}

void
MacroAssembler::move32To64ZeroExtend(Register src, Register64 dest)
{
    Mov(ARMRegister(dest.reg, 32), ARMRegister(src, 32));
}

void
MacroAssembler::move8To64SignExtend(Register src, Register64 dest)
{
    Sxtb(ARMRegister(dest.reg, 64), ARMRegister(src, 32));
}

void
MacroAssembler::move16To64SignExtend(Register src, Register64 dest)
{
    Sxth(ARMRegister(dest.reg, 64), ARMRegister(src, 32));
}

void
MacroAssembler::move32To64SignExtend(Register src, Register64 dest)
{
    Sxtw(ARMRegister(dest.reg, 64), ARMRegister(src, 32));
}

// ===============================================================
// Logical instructions

void
MacroAssembler::not32(Register reg)
{
    Orn(ARMRegister(reg, 32), vixl::wzr, ARMRegister(reg, 32));
}

void
MacroAssembler::and32(Register src, Register dest)
{
    And(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(ARMRegister(src, 32)));
}

void
MacroAssembler::and32(Imm32 imm, Register dest)
{
    And(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(imm.value));
}

void
MacroAssembler::and32(Imm32 imm, Register src, Register dest)
{
    And(ARMRegister(dest, 32), ARMRegister(src, 32), Operand(imm.value));
}

void
MacroAssembler::and32(Imm32 imm, const Address& dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != dest.base);
    load32(dest, scratch32.asUnsized());
    And(scratch32, scratch32, Operand(imm.value));
    store32(scratch32.asUnsized(), dest);
}

void
MacroAssembler::and32(const Address& src, Register dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != src.base);
    load32(src, scratch32.asUnsized());
    And(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(scratch32));
}

void
MacroAssembler::andPtr(Register src, Register dest)
{
    And(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(ARMRegister(src, 64)));
}

void
MacroAssembler::andPtr(Imm32 imm, Register dest)
{
    And(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(imm.value));
}

void
MacroAssembler::and64(Imm64 imm, Register64 dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    mov(ImmWord(imm.value), scratch);
    andPtr(scratch, dest.reg);
}

void
MacroAssembler::and64(Register64 src, Register64 dest)
{
    And(ARMRegister(dest.reg, 64), ARMRegister(dest.reg, 64), ARMRegister(src.reg, 64));
}

void
MacroAssembler::or64(Imm64 imm, Register64 dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    mov(ImmWord(imm.value), scratch);
    orPtr(scratch, dest.reg);
}

void
MacroAssembler::xor64(Imm64 imm, Register64 dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    mov(ImmWord(imm.value), scratch);
    xorPtr(scratch, dest.reg);
}

void
MacroAssembler::or32(Imm32 imm, Register dest)
{
    Orr(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(imm.value));
}

void
MacroAssembler::or32(Register src, Register dest)
{
    Orr(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(ARMRegister(src, 32)));
}

void
MacroAssembler::or32(Imm32 imm, const Address& dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != dest.base);
    load32(dest, scratch32.asUnsized());
    Orr(scratch32, scratch32, Operand(imm.value));
    store32(scratch32.asUnsized(), dest);
}

void
MacroAssembler::orPtr(Register src, Register dest)
{
    Orr(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(ARMRegister(src, 64)));
}

void
MacroAssembler::orPtr(Imm32 imm, Register dest)
{
    Orr(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(imm.value));
}

void
MacroAssembler::or64(Register64 src, Register64 dest)
{
    orPtr(src.reg, dest.reg);
}

void
MacroAssembler::xor64(Register64 src, Register64 dest)
{
    xorPtr(src.reg, dest.reg);
}

void
MacroAssembler::xor32(Register src, Register dest)
{
    Eor(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(ARMRegister(src, 32)));
}

void
MacroAssembler::xor32(Imm32 imm, Register dest)
{
    Eor(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(imm.value));
}

void
MacroAssembler::xorPtr(Register src, Register dest)
{
    Eor(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(ARMRegister(src, 64)));
}

void
MacroAssembler::xorPtr(Imm32 imm, Register dest)
{
    Eor(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(imm.value));
}

// ===============================================================
// Arithmetic functions

void
MacroAssembler::add32(Register src, Register dest)
{
    Add(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(ARMRegister(src, 32)));
}

void
MacroAssembler::add32(Imm32 imm, Register dest)
{
    Add(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(imm.value));
}

void
MacroAssembler::add32(Imm32 imm, const Address& dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != dest.base);

    Ldr(scratch32, toMemOperand(dest));
    Add(scratch32, scratch32, Operand(imm.value));
    Str(scratch32, toMemOperand(dest));
}

void
MacroAssembler::addPtr(Register src, Register dest)
{
    addPtr(src, dest, dest);
}

void
MacroAssembler::addPtr(Register src1, Register src2, Register dest)
{
    Add(ARMRegister(dest, 64), ARMRegister(src1, 64), Operand(ARMRegister(src2, 64)));
}

void
MacroAssembler::addPtr(Imm32 imm, Register dest)
{
    addPtr(imm, dest, dest);
}

void
MacroAssembler::addPtr(Imm32 imm, Register src, Register dest)
{
    Add(ARMRegister(dest, 64), ARMRegister(src, 64), Operand(imm.value));
}

void
MacroAssembler::addPtr(ImmWord imm, Register dest)
{
    Add(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(imm.value));
}

void
MacroAssembler::addPtr(Imm32 imm, const Address& dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != dest.base);

    Ldr(scratch64, toMemOperand(dest));
    Add(scratch64, scratch64, Operand(imm.value));
    Str(scratch64, toMemOperand(dest));
}

void
MacroAssembler::addPtr(const Address& src, Register dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != src.base);

    Ldr(scratch64, toMemOperand(src));
    Add(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(scratch64));
}

void
MacroAssembler::add64(Register64 src, Register64 dest)
{
    addPtr(src.reg, dest.reg);
}

void
MacroAssembler::add64(Imm32 imm, Register64 dest)
{
    Add(ARMRegister(dest.reg, 64), ARMRegister(dest.reg, 64), Operand(imm.value));
}

void
MacroAssembler::add64(Imm64 imm, Register64 dest)
{
    Add(ARMRegister(dest.reg, 64), ARMRegister(dest.reg, 64), Operand(imm.value));
}

CodeOffset
MacroAssembler::sub32FromStackPtrWithPatch(Register dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch = temps.AcquireX();
    CodeOffset offs = CodeOffset(currentOffset());
    movz(scratch, 0, 0);
    movk(scratch, 0, 16);
    Sub(ARMRegister(dest, 64), sp, scratch);
    return offs;
}

void
MacroAssembler::patchSub32FromStackPtr(CodeOffset offset, Imm32 imm)
{
    Instruction* i1 = getInstructionAt(BufferOffset(offset.offset()));
    MOZ_ASSERT(i1->IsMovz());
    i1->SetInstructionBits(i1->InstructionBits() | ImmMoveWide(uint16_t(imm.value)));

    Instruction* i2 = getInstructionAt(BufferOffset(offset.offset() + 4));
    MOZ_ASSERT(i2->IsMovk());
    i2->SetInstructionBits(i2->InstructionBits() | ImmMoveWide(uint16_t(imm.value >> 16)));
}

void
MacroAssembler::addDouble(FloatRegister src, FloatRegister dest)
{
    fadd(ARMFPRegister(dest, 64), ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
}

void
MacroAssembler::addFloat32(FloatRegister src, FloatRegister dest)
{
    fadd(ARMFPRegister(dest, 32), ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
}

void
MacroAssembler::sub32(Imm32 imm, Register dest)
{
    Sub(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(imm.value));
}

void
MacroAssembler::sub32(Register src, Register dest)
{
    Sub(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(ARMRegister(src, 32)));
}

void
MacroAssembler::sub32(const Address& src, Register dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != src.base);
    load32(src, scratch32.asUnsized());
    Sub(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(scratch32));
}

void
MacroAssembler::subPtr(Register src, Register dest)
{
    Sub(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(ARMRegister(src, 64)));
}

void
MacroAssembler::subPtr(Register src, const Address& dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != dest.base);

    Ldr(scratch64, toMemOperand(dest));
    Sub(scratch64, scratch64, Operand(ARMRegister(src, 64)));
    Str(scratch64, toMemOperand(dest));
}

void
MacroAssembler::subPtr(Imm32 imm, Register dest)
{
    Sub(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(imm.value));
}

void
MacroAssembler::subPtr(const Address& addr, Register dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != addr.base);

    Ldr(scratch64, toMemOperand(addr));
    Sub(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(scratch64));
}

void
MacroAssembler::sub64(Register64 src, Register64 dest)
{
    Sub(ARMRegister(dest.reg, 64), ARMRegister(dest.reg, 64), ARMRegister(src.reg, 64));
}

void
MacroAssembler::sub64(Imm64 imm, Register64 dest)
{
    Sub(ARMRegister(dest.reg, 64), ARMRegister(dest.reg, 64), Operand(imm.value));
}

void
MacroAssembler::subDouble(FloatRegister src, FloatRegister dest)
{
    fsub(ARMFPRegister(dest, 64), ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
}

void
MacroAssembler::subFloat32(FloatRegister src, FloatRegister dest)
{
    fsub(ARMFPRegister(dest, 32), ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
}

void
MacroAssembler::mul32(Register rhs, Register srcDest)
{
    mul32(srcDest, rhs, srcDest, nullptr, nullptr);
}

void
MacroAssembler::mul32(Register src1, Register src2, Register dest, Label* onOver, Label* onZero)
{
    Smull(ARMRegister(dest, 64), ARMRegister(src1, 32), ARMRegister(src2, 32));
    if (onOver) {
        Cmp(ARMRegister(dest, 64), Operand(ARMRegister(dest, 32), vixl::SXTW));
        B(onOver, NotEqual);
    }
    if (onZero)
        Cbz(ARMRegister(dest, 32), onZero);

    // Clear upper 32 bits.
    Mov(ARMRegister(dest, 32), ARMRegister(dest, 32));
}

void
MacroAssembler::mul64(Imm64 imm, const Register64& dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(dest.reg != scratch64.asUnsized());
    mov(ImmWord(imm.value), scratch64.asUnsized());
    Mul(ARMRegister(dest.reg, 64), ARMRegister(dest.reg, 64), scratch64);
}

void
MacroAssembler::mul64(const Register64& src, const Register64& dest, const Register temp)
{
    MOZ_ASSERT(temp == Register::Invalid());
    Mul(ARMRegister(dest.reg, 64), ARMRegister(dest.reg, 64), ARMRegister(src.reg, 64));
}

void
MacroAssembler::mulBy3(Register src, Register dest)
{
    ARMRegister xdest(dest, 64);
    ARMRegister xsrc(src, 64);
    Add(xdest, xsrc, Operand(xsrc, vixl::LSL, 1));
}

void
MacroAssembler::mulFloat32(FloatRegister src, FloatRegister dest)
{
    fmul(ARMFPRegister(dest, 32), ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
}

void
MacroAssembler::mulDouble(FloatRegister src, FloatRegister dest)
{
    fmul(ARMFPRegister(dest, 64), ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
}

void
MacroAssembler::mulDoublePtr(ImmPtr imm, Register temp, FloatRegister dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(temp != scratch);
    movePtr(imm, scratch);
    const ARMFPRegister scratchDouble = temps.AcquireD();
    Ldr(scratchDouble, MemOperand(Address(scratch, 0)));
    fmul(ARMFPRegister(dest, 64), ARMFPRegister(dest, 64), scratchDouble);
}

void
MacroAssembler::quotient32(Register rhs, Register srcDest, bool isUnsigned)
{
    if (isUnsigned)
        Udiv(ARMRegister(srcDest, 32), ARMRegister(srcDest, 32), ARMRegister(rhs, 32));
    else
        Sdiv(ARMRegister(srcDest, 32), ARMRegister(srcDest, 32), ARMRegister(rhs, 32));
}

// This does not deal with x % 0 or INT_MIN % -1, the caller needs to filter
// those cases when they may occur.

void
MacroAssembler::remainder32(Register rhs, Register srcDest, bool isUnsigned)
{
    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch = temps.AcquireW();
    if (isUnsigned)
        Udiv(scratch, ARMRegister(srcDest, 32), ARMRegister(rhs, 32));
    else
        Sdiv(scratch, ARMRegister(srcDest, 32), ARMRegister(rhs, 32));
    Mul(scratch, scratch, ARMRegister(rhs, 32));
    Sub(ARMRegister(srcDest, 32), ARMRegister(srcDest, 32), scratch);
}

void
MacroAssembler::divFloat32(FloatRegister src, FloatRegister dest)
{
    fdiv(ARMFPRegister(dest, 32), ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
}

void
MacroAssembler::divDouble(FloatRegister src, FloatRegister dest)
{
    fdiv(ARMFPRegister(dest, 64), ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
}

void
MacroAssembler::inc64(AbsoluteAddress dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratchAddr64 = temps.AcquireX();
    const ARMRegister scratch64 = temps.AcquireX();

    Mov(scratchAddr64, uint64_t(dest.addr));
    Ldr(scratch64, MemOperand(scratchAddr64, 0));
    Add(scratch64, scratch64, Operand(1));
    Str(scratch64, MemOperand(scratchAddr64, 0));
}

void
MacroAssembler::neg32(Register reg)
{
    Negs(ARMRegister(reg, 32), Operand(ARMRegister(reg, 32)));
}

void
MacroAssembler::negateFloat(FloatRegister reg)
{
    fneg(ARMFPRegister(reg, 32), ARMFPRegister(reg, 32));
}

void
MacroAssembler::negateDouble(FloatRegister reg)
{
    fneg(ARMFPRegister(reg, 64), ARMFPRegister(reg, 64));
}

void
MacroAssembler::absFloat32(FloatRegister src, FloatRegister dest)
{
    fabs(ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
}

void
MacroAssembler::absDouble(FloatRegister src, FloatRegister dest)
{
    fabs(ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
}

void
MacroAssembler::sqrtFloat32(FloatRegister src, FloatRegister dest)
{
    fsqrt(ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
}

void
MacroAssembler::sqrtDouble(FloatRegister src, FloatRegister dest)
{
    fsqrt(ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
}

void
MacroAssembler::minFloat32(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    MOZ_ASSERT(handleNaN);      // Always true for wasm
    fmin(ARMFPRegister(srcDest, 32), ARMFPRegister(srcDest, 32), ARMFPRegister(other, 32));
}

void
MacroAssembler::minDouble(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    MOZ_ASSERT(handleNaN);      // Always true for wasm
    fmin(ARMFPRegister(srcDest, 64), ARMFPRegister(srcDest, 64), ARMFPRegister(other, 64));
}

void
MacroAssembler::maxFloat32(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    MOZ_ASSERT(handleNaN);      // Always true for wasm
    fmax(ARMFPRegister(srcDest, 32), ARMFPRegister(srcDest, 32), ARMFPRegister(other, 32));
}

void
MacroAssembler::maxDouble(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    MOZ_ASSERT(handleNaN);      // Always true for wasm
    fmax(ARMFPRegister(srcDest, 64), ARMFPRegister(srcDest, 64), ARMFPRegister(other, 64));
}

// ===============================================================
// Shift functions

void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    Lsl(ARMRegister(dest, 64), ARMRegister(dest, 64), imm.value);
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
    Lsl(ARMRegister(srcDest.reg, 64), ARMRegister(srcDest.reg, 64), ARMRegister(shift, 64));
}

void
MacroAssembler::lshift32(Register shift, Register dest)
{
    Lsl(ARMRegister(dest, 32), ARMRegister(dest, 32), ARMRegister(shift, 32));
}

void
MacroAssembler::lshift32(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 32);
    Lsl(ARMRegister(dest, 32), ARMRegister(dest, 32), imm.value);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    Lsr(ARMRegister(dest, 64), ARMRegister(dest, 64), imm.value);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register src, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    Lsr(ARMRegister(dest, 64), ARMRegister(src, 64), imm.value);
}

void
MacroAssembler::rshift32(Register shift, Register dest)
{
    Lsr(ARMRegister(dest, 32), ARMRegister(dest, 32), ARMRegister(shift, 32));
}

void
MacroAssembler::rshift32(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 32);
    Lsr(ARMRegister(dest, 32), ARMRegister(dest, 32), imm.value);
}

void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    Asr(ARMRegister(dest, 64), ARMRegister(dest, 64), imm.value);
}

void
MacroAssembler::rshift32Arithmetic(Register shift, Register dest)
{
    Asr(ARMRegister(dest, 32), ARMRegister(dest, 32), ARMRegister(shift, 32));
}

void
MacroAssembler::rshift32Arithmetic(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 32);
    Asr(ARMRegister(dest, 32), ARMRegister(dest, 32), imm.value);
}

void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    rshiftPtr(imm, dest.reg);
}

void
MacroAssembler::rshift64(Register shift, Register64 srcDest)
{
    Lsr(ARMRegister(srcDest.reg, 64), ARMRegister(srcDest.reg, 64), ARMRegister(shift, 64));
}

void
MacroAssembler::rshift64Arithmetic(Imm32 imm, Register64 dest)
{
    Asr(ARMRegister(dest.reg, 64), ARMRegister(dest.reg, 64), imm.value);
}

void
MacroAssembler::rshift64Arithmetic(Register shift, Register64 srcDest)
{
    Asr(ARMRegister(srcDest.reg, 64), ARMRegister(srcDest.reg, 64), ARMRegister(shift, 64));
}

// ===============================================================
// Condition functions

template <typename T1, typename T2>
void
MacroAssembler::cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest)
{
    cmp32(lhs, rhs);
    emitSet(cond, dest);
}

template <typename T1, typename T2>
void
MacroAssembler::cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest)
{
    cmpPtr(lhs, rhs);
    emitSet(cond, dest);
}

// ===============================================================
// Rotation functions

void
MacroAssembler::rotateLeft(Imm32 count, Register input, Register dest)
{
    Ror(ARMRegister(dest, 32), ARMRegister(input, 32), (32 - count.value) & 31);
}

void
MacroAssembler::rotateLeft(Register count, Register input, Register dest)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch = temps.AcquireW();
    // Really 32 - count, but the upper bits of the result are ignored.
    Neg(scratch, ARMRegister(count, 32));
    Ror(ARMRegister(dest, 32), ARMRegister(input, 32), scratch);
}

void
MacroAssembler::rotateRight(Imm32 count, Register input, Register dest)
{
    Ror(ARMRegister(dest, 32), ARMRegister(input, 32), count.value & 31);
}

void
MacroAssembler::rotateRight(Register count, Register input, Register dest)
{
    Ror(ARMRegister(dest, 32), ARMRegister(input, 32), ARMRegister(count, 32));
}

void
MacroAssembler::rotateLeft64(Register count, Register64 input, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == Register::Invalid());

    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch = temps.AcquireX();
    // Really 64 - count, but the upper bits of the result are ignored.
    Neg(scratch, ARMRegister(count, 64));
    Ror(ARMRegister(dest.reg, 64), ARMRegister(input.reg, 64), scratch);
}

void
MacroAssembler::rotateLeft64(Imm32 count, Register64 input, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == Register::Invalid());

    Ror(ARMRegister(dest.reg, 64), ARMRegister(input.reg, 64), (64 - count.value) & 63);
}

void
MacroAssembler::rotateRight64(Register count, Register64 input, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == Register::Invalid());

    Ror(ARMRegister(dest.reg, 64), ARMRegister(input.reg, 64), ARMRegister(count, 64));
}

void
MacroAssembler::rotateRight64(Imm32 count, Register64 input, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == Register::Invalid());

    Ror(ARMRegister(dest.reg, 64), ARMRegister(input.reg, 64), count.value & 63);
}

// ===============================================================
// Bit counting functions

void
MacroAssembler::clz32(Register src, Register dest, bool knownNotZero)
{
    Clz(ARMRegister(dest, 32), ARMRegister(src, 32));
}

void
MacroAssembler::ctz32(Register src, Register dest, bool knownNotZero)
{
    Rbit(ARMRegister(dest, 32), ARMRegister(src, 32));
    Clz(ARMRegister(dest, 32), ARMRegister(dest, 32));
}

void
MacroAssembler::clz64(Register64 src, Register dest)
{
    Clz(ARMRegister(dest, 64), ARMRegister(src.reg, 64));
}

void
MacroAssembler::ctz64(Register64 src, Register dest)
{
    Rbit(ARMRegister(dest, 64), ARMRegister(src.reg, 64));
    Clz(ARMRegister(dest, 64), ARMRegister(dest, 64));
}

void
MacroAssembler::popcnt32(Register src_, Register dest_, Register tmp_)
{
    MOZ_ASSERT(tmp_ != Register::Invalid());

    // Equivalent to mozilla::CountPopulation32().

    ARMRegister src(src_, 32);
    ARMRegister dest(dest_, 32);
    ARMRegister tmp(tmp_, 32);

    Mov(tmp, src);
    if (src_ != dest_)
        Mov(dest, src);
    Lsr(dest, dest, 1);
    And(dest, dest, 0x55555555);
    Sub(dest, tmp, dest);
    Lsr(tmp, dest, 2);
    And(tmp, tmp, 0x33333333);
    And(dest, dest, 0x33333333);
    Add(dest, tmp, dest);
    Add(dest, dest, Operand(dest, vixl::LSR, 4));
    And(dest, dest, 0x0F0F0F0F);
    Add(dest, dest, Operand(dest, vixl::LSL, 8));
    Add(dest, dest, Operand(dest, vixl::LSL, 16));
    Lsr(dest, dest, 24);
}

void
MacroAssembler::popcnt64(Register64 src_, Register64 dest_, Register tmp_)
{
    MOZ_ASSERT(tmp_ != Register::Invalid());

    // Equivalent to mozilla::CountPopulation64(), though likely more efficient.

    ARMRegister src(src_.reg, 64);
    ARMRegister dest(dest_.reg, 64);
    ARMRegister tmp(tmp_, 64);

    Mov(tmp, src);
    if (src_ != dest_)
        Mov(dest, src);
    Lsr(dest, dest, 1);
    And(dest, dest, 0x5555555555555555);
    Sub(dest, tmp, dest);
    Lsr(tmp, dest, 2);
    And(tmp, tmp, 0x3333333333333333);
    And(dest, dest, 0x3333333333333333);
    Add(dest, tmp, dest);
    Add(dest, dest, Operand(dest, vixl::LSR, 4));
    And(dest, dest, 0x0F0F0F0F0F0F0F0F);
    Add(dest, dest, Operand(dest, vixl::LSL, 8));
    Add(dest, dest, Operand(dest, vixl::LSL, 16));
    Add(dest, dest, Operand(dest, vixl::LSL, 32));
    Lsr(dest, dest, 56);
}

// ===============================================================
// Branch functions

template <class L>
void
MacroAssembler::branch32(Condition cond, Register lhs, Register rhs, L label)
{
    cmp32(lhs, rhs);
    B(label, cond);
}

template <class L>
void
MacroAssembler::branch32(Condition cond, Register lhs, Imm32 imm, L label)
{
    cmp32(lhs, imm);
    B(label, cond);
}

void
MacroAssembler::branch32(Condition cond, const Address& lhs, Register rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs.base);
    MOZ_ASSERT(scratch != rhs);
    load32(lhs, scratch);
    branch32(cond, scratch, rhs, label);
}

void
MacroAssembler::branch32(Condition cond, const Address& lhs, Imm32 imm, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs.base);
    load32(lhs, scratch);
    branch32(cond, scratch, imm, label);
}

void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    movePtr(ImmPtr(lhs.addr), scratch);
    branch32(cond, Address(scratch, 0), rhs, label);
}

void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    movePtr(ImmPtr(lhs.addr), scratch);
    branch32(cond, Address(scratch, 0), rhs, label);
}

void
MacroAssembler::branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != lhs.base);
    MOZ_ASSERT(scratch32.asUnsized() != lhs.index);
    doBaseIndex(scratch32, lhs, vixl::LDR_w);
    branch32(cond, scratch32.asUnsized(), rhs, label);
}

void
MacroAssembler::branch32(Condition cond, wasm::SymbolicAddress lhs, Imm32 rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    movePtr(lhs, scratch);
    branch32(cond, Address(scratch, 0), rhs, label);
}

void
MacroAssembler::branch64(Condition cond, Register64 lhs, Imm64 val, Label* success, Label* fail)
{
    Cmp(ARMRegister(lhs.reg, 64), val.value);
    B(success, cond);
    if (fail)
        B(fail);
}

void
MacroAssembler::branch64(Condition cond, Register64 lhs, Register64 rhs, Label* success, Label* fail)
{
    Cmp(ARMRegister(lhs.reg, 64), ARMRegister(rhs.reg, 64));
    B(success, cond);
    if (fail)
        B(fail);
}

void
MacroAssembler::branch64(Condition cond, const Address& lhs, Imm64 val, Label* label)
{
    MOZ_ASSERT(cond == Assembler::NotEqual,
               "other condition codes not supported");

    branchPtr(cond, lhs, ImmWord(val.value), label);
}

void
MacroAssembler::branch64(Condition cond, const Address& lhs, const Address& rhs, Register scratch,
                         Label* label)
{
    MOZ_ASSERT(cond == Assembler::NotEqual,
               "other condition codes not supported");
    MOZ_ASSERT(lhs.base != scratch);
    MOZ_ASSERT(rhs.base != scratch);

    loadPtr(rhs, scratch);
    branchPtr(cond, lhs, scratch, label);
}

template <class L>
void
MacroAssembler::branchPtr(Condition cond, Register lhs, Register rhs, L label)
{
    Cmp(ARMRegister(lhs, 64), ARMRegister(rhs, 64));
    B(label, cond);
}

void
MacroAssembler::branchPtr(Condition cond, Register lhs, Imm32 rhs, Label* label)
{
    cmpPtr(lhs, rhs);
    B(label, cond);
}

void
MacroAssembler::branchPtr(Condition cond, Register lhs, ImmPtr rhs, Label* label)
{
    cmpPtr(lhs, rhs);
    B(label, cond);
}

void
MacroAssembler::branchPtr(Condition cond, Register lhs, ImmGCPtr rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs);
    movePtr(rhs, scratch);
    branchPtr(cond, lhs, scratch, label);
}

void
MacroAssembler::branchPtr(Condition cond, Register lhs, ImmWord rhs, Label* label)
{
    cmpPtr(lhs, rhs);
    B(label, cond);
}

template <class L>
void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, Register rhs, L label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs.base);
    MOZ_ASSERT(scratch != rhs);
    loadPtr(lhs, scratch);
    branchPtr(cond, scratch, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmPtr rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs.base);
    loadPtr(lhs, scratch);
    branchPtr(cond, scratch, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmGCPtr rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch1_64 = temps.AcquireX();
    const ARMRegister scratch2_64 = temps.AcquireX();
    MOZ_ASSERT(scratch1_64.asUnsized() != lhs.base);
    MOZ_ASSERT(scratch2_64.asUnsized() != lhs.base);

    movePtr(rhs, scratch1_64.asUnsized());
    loadPtr(lhs, scratch2_64.asUnsized());
    branchPtr(cond, scratch2_64.asUnsized(), scratch1_64.asUnsized(), label);
}

void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmWord rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs.base);
    loadPtr(lhs, scratch);
    branchPtr(cond, scratch, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != rhs);
    loadPtr(lhs, scratch);
    branchPtr(cond, scratch, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, ImmWord rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    loadPtr(lhs, scratch);
    branchPtr(cond, scratch, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, wasm::SymbolicAddress lhs, Register rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != rhs);
    loadPtr(lhs, scratch);
    branchPtr(cond, scratch, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const BaseIndex& lhs, ImmWord rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs.base);
    MOZ_ASSERT(scratch != lhs.index);
    loadPtr(lhs, scratch);
    branchPtr(cond, scratch, rhs, label);
}

template <typename T>
CodeOffsetJump
MacroAssembler::branchPtrWithPatch(Condition cond, Register lhs, T rhs, RepatchLabel* label)
{
    cmpPtr(lhs, rhs);
    return jumpWithPatch(label, cond);
}

template <typename T>
CodeOffsetJump
MacroAssembler::branchPtrWithPatch(Condition cond, Address lhs, T rhs, RepatchLabel* label)
{
    // The scratch register is unused after the condition codes are set.
    {
        vixl::UseScratchRegisterScope temps(this);
        const Register scratch = temps.AcquireX().asUnsized();
        MOZ_ASSERT(scratch != lhs.base);
        loadPtr(lhs, scratch);
        cmpPtr(scratch, rhs);
    }
    return jumpWithPatch(label, cond);
}

void
MacroAssembler::branchPrivatePtr(Condition cond, const Address& lhs, Register rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    if (rhs != scratch)
        movePtr(rhs, scratch);
    // Instead of unboxing lhs, box rhs and do direct comparison with lhs.
    rshiftPtr(Imm32(1), scratch);
    branchPtr(cond, lhs, scratch, label);
}

void
MacroAssembler::branchFloat(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                            Label* label)
{
    compareFloat(cond, lhs, rhs);
    switch (cond) {
      case DoubleNotEqual: {
        Label unordered;
        // not equal *and* ordered
        branch(Overflow, &unordered);
        branch(NotEqual, label);
        bind(&unordered);
        break;
      }
      case DoubleEqualOrUnordered:
        branch(Overflow, label);
        branch(Equal, label);
        break;
      default:
        branch(Condition(cond), label);
    }
}

void
MacroAssembler::branchTruncateFloat32MaybeModUint32(FloatRegister src, Register dest, Label* fail)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();

    ARMFPRegister src32(src, 32);
    ARMRegister dest64(dest, 64);

    MOZ_ASSERT(!scratch64.Is(dest64));

    Fcvtzs(dest64, src32);
    Add(scratch64, dest64, Operand(0x7fffffffffffffff));
    Cmn(scratch64, 3);
    B(fail, Assembler::Above);
    And(dest64, dest64, Operand(0xffffffff));
}

void
MacroAssembler::branchTruncateFloat32ToInt32(FloatRegister src, Register dest, Label* fail)
{
    convertFloat32ToInt32(src, dest, fail);
}

void
MacroAssembler::branchDouble(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                             Label* label)
{
    compareDouble(cond, lhs, rhs);
    switch (cond) {
      case DoubleNotEqual: {
        Label unordered;
        // not equal *and* ordered
        branch(Overflow, &unordered);
        branch(NotEqual, label);
        bind(&unordered);
        break;
      }
      case DoubleEqualOrUnordered:
        branch(Overflow, label);
        branch(Equal, label);
        break;
      default:
        branch(Condition(cond), label);
    }
}

void
MacroAssembler::branchTruncateDoubleMaybeModUint32(FloatRegister src, Register dest, Label* fail)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();

    // An out of range integer will be saturated to the destination size.
    ARMFPRegister src64(src, 64);
    ARMRegister dest64(dest, 64);

    MOZ_ASSERT(!scratch64.Is(dest64));

    Fcvtzs(dest64, src64);
    Add(scratch64, dest64, Operand(0x7fffffffffffffff));
    Cmn(scratch64, 3);
    B(fail, Assembler::Above);
    And(dest64, dest64, Operand(0xffffffff));
}

void
MacroAssembler::branchTruncateDoubleToInt32(FloatRegister src, Register dest, Label* fail)
{
    convertDoubleToInt32(src, dest, fail);
}

template <typename T, typename L>
void
MacroAssembler::branchAdd32(Condition cond, T src, Register dest, L label)
{
    adds32(src, dest);
    B(label, cond);
}

template <typename T>
void
MacroAssembler::branchSub32(Condition cond, T src, Register dest, Label* label)
{
    subs32(src, dest);
    branch(cond, label);
}

void
MacroAssembler::decBranchPtr(Condition cond, Register lhs, Imm32 rhs, Label* label)
{
    Subs(ARMRegister(lhs, 64), ARMRegister(lhs, 64), Operand(rhs.value));
    B(cond, label);
}

template <class L>
void
MacroAssembler::branchTest32(Condition cond, Register lhs, Register rhs, L label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    // x86 prefers |test foo, foo| to |cmp foo, #0|.
    // Convert the former to the latter for ARM.
    if (lhs == rhs && (cond == Zero || cond == NonZero))
        cmp32(lhs, Imm32(0));
    else
        test32(lhs, rhs);
    B(label, cond);
}

template <class L>
void
MacroAssembler::branchTest32(Condition cond, Register lhs, Imm32 rhs, L label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    test32(lhs, rhs);
    B(label, cond);
}

void
MacroAssembler::branchTest32(Condition cond, const Address& lhs, Imm32 rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs.base);
    load32(lhs, scratch);
    branchTest32(cond, scratch, rhs, label);
}

void
MacroAssembler::branchTest32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    load32(lhs, scratch);
    branchTest32(cond, scratch, rhs, label);
}

template <class L>
void
MacroAssembler::branchTestPtr(Condition cond, Register lhs, Register rhs, L label)
{
    Tst(ARMRegister(lhs, 64), Operand(ARMRegister(rhs, 64)));
    B(label, cond);
}

void
MacroAssembler::branchTestPtr(Condition cond, Register lhs, Imm32 rhs, Label* label)
{
    Tst(ARMRegister(lhs, 64), Operand(rhs.value));
    B(label, cond);
}

void
MacroAssembler::branchTestPtr(Condition cond, const Address& lhs, Imm32 rhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs.base);
    loadPtr(lhs, scratch);
    branchTestPtr(cond, scratch, rhs, label);
}

template <class L>
void
MacroAssembler::branchTest64(Condition cond, Register64 lhs, Register64 rhs, Register temp,
                             L label)
{
    branchTestPtr(cond, lhs.reg, rhs.reg, label);
}

void
MacroAssembler::branchTestUndefined(Condition cond, Register tag, Label* label)
{
    branchTestUndefinedImpl(cond, tag, label);
}

void
MacroAssembler::branchTestUndefined(Condition cond, const Address& address, Label* label)
{
    branchTestUndefinedImpl(cond, address, label);
}

void
MacroAssembler::branchTestUndefined(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestUndefinedImpl(cond, address, label);
}

void
MacroAssembler::branchTestUndefined(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestUndefinedImpl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestUndefinedImpl(Condition cond, const T& t, Label* label)
{
    Condition c = testUndefined(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestInt32(Condition cond, Register tag, Label* label)
{
    branchTestInt32Impl(cond, tag, label);
}

void
MacroAssembler::branchTestInt32(Condition cond, const Address& address, Label* label)
{
    branchTestInt32Impl(cond, address, label);
}

void
MacroAssembler::branchTestInt32(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestInt32Impl(cond, address, label);
}

void
MacroAssembler::branchTestInt32(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestInt32Impl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestInt32Impl(Condition cond, const T& t, Label* label)
{
    Condition c = testInt32(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestInt32Truthy(bool truthy, const ValueOperand& value, Label* label)
{
    Condition c = testInt32Truthy(truthy, value);
    B(label, c);
}

void
MacroAssembler::branchTestDouble(Condition cond, Register tag, Label* label)
{
    branchTestDoubleImpl(cond, tag, label);
}

void
MacroAssembler::branchTestDouble(Condition cond, const Address& address, Label* label)
{
    branchTestDoubleImpl(cond, address, label);
}

void
MacroAssembler::branchTestDouble(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestDoubleImpl(cond, address, label);
}

void
MacroAssembler::branchTestDouble(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestDoubleImpl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestDoubleImpl(Condition cond, const T& t, Label* label)
{
    Condition c = testDouble(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestDoubleTruthy(bool truthy, FloatRegister reg, Label* label)
{
    Fcmp(ARMFPRegister(reg, 64), 0.0);
    if (!truthy) {
        // falsy values are zero, and NaN.
        branch(Zero, label);
        branch(Overflow, label);
    } else {
        // truthy values are non-zero and not nan.
        // If it is overflow
        Label onFalse;
        branch(Zero, &onFalse);
        branch(Overflow, &onFalse);
        B(label);
        bind(&onFalse);
    }
}

void
MacroAssembler::branchTestNumber(Condition cond, Register tag, Label* label)
{
    branchTestNumberImpl(cond, tag, label);
}

void
MacroAssembler::branchTestNumber(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestNumberImpl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestNumberImpl(Condition cond, const T& t, Label* label)
{
    Condition c = testNumber(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestBoolean(Condition cond, Register tag, Label* label)
{
    branchTestBooleanImpl(cond, tag, label);
}

void
MacroAssembler::branchTestBoolean(Condition cond, const Address& address, Label* label)
{
    branchTestBooleanImpl(cond, address, label);
}

void
MacroAssembler::branchTestBoolean(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestBooleanImpl(cond, address, label);
}

void
MacroAssembler::branchTestBoolean(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestBooleanImpl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestBooleanImpl(Condition cond, const T& tag, Label* label)
{
    Condition c = testBoolean(cond, tag);
    B(label, c);
}

void
MacroAssembler::branchTestBooleanTruthy(bool truthy, const ValueOperand& value, Label* label)
{
    Condition c = testBooleanTruthy(truthy, value);
    B(label, c);
}

void
MacroAssembler::branchTestString(Condition cond, Register tag, Label* label)
{
    branchTestStringImpl(cond, tag, label);
}

void
MacroAssembler::branchTestString(Condition cond, const Address& address, Label* label)
{
    branchTestStringImpl(cond, address, label);
}

void
MacroAssembler::branchTestString(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestStringImpl(cond, address, label);
}

void
MacroAssembler::branchTestString(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestStringImpl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestStringImpl(Condition cond, const T& t, Label* label)
{
    Condition c = testString(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestStringTruthy(bool truthy, const ValueOperand& value, Label* label)
{
    Condition c = testStringTruthy(truthy, value);
    B(label, c);
}

void
MacroAssembler::branchTestSymbol(Condition cond, Register tag, Label* label)
{
    branchTestSymbolImpl(cond, tag, label);
}

void
MacroAssembler::branchTestSymbol(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestSymbolImpl(cond, address, label);
}

void
MacroAssembler::branchTestSymbol(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestSymbolImpl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestSymbolImpl(Condition cond, const T& t, Label* label)
{
    Condition c = testSymbol(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestNull(Condition cond, Register tag, Label* label)
{
    branchTestNullImpl(cond, tag, label);
}

void
MacroAssembler::branchTestNull(Condition cond, const Address& address, Label* label)
{
    branchTestNullImpl(cond, address, label);
}

void
MacroAssembler::branchTestNull(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestNullImpl(cond, address, label);
}

void
MacroAssembler::branchTestNull(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestNullImpl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestNullImpl(Condition cond, const T& t, Label* label)
{
    Condition c = testNull(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestObject(Condition cond, Register tag, Label* label)
{
    branchTestObjectImpl(cond, tag, label);
}

void
MacroAssembler::branchTestObject(Condition cond, const Address& address, Label* label)
{
    branchTestObjectImpl(cond, address, label);
}

void
MacroAssembler::branchTestObject(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestObjectImpl(cond, address, label);
}

void
MacroAssembler::branchTestObject(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestObjectImpl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestObjectImpl(Condition cond, const T& t, Label* label)
{
    Condition c = testObject(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestGCThing(Condition cond, const Address& address, Label* label)
{
    branchTestGCThingImpl(cond, address, label);
}

void
MacroAssembler::branchTestGCThing(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestGCThingImpl(cond, address, label);
}

template <typename T>
void
MacroAssembler::branchTestGCThingImpl(Condition cond, const T& src, Label* label)
{
    Condition c = testGCThing(cond, src);
    B(label, c);
}

void
MacroAssembler::branchTestPrimitive(Condition cond, Register tag, Label* label)
{
    branchTestPrimitiveImpl(cond, tag, label);
}

void
MacroAssembler::branchTestPrimitive(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestPrimitiveImpl(cond, value, label);
}

template <typename T>
void
MacroAssembler::branchTestPrimitiveImpl(Condition cond, const T& t, Label* label)
{
    Condition c = testPrimitive(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestMagic(Condition cond, Register tag, Label* label)
{
    branchTestMagicImpl(cond, tag, label);
}

void
MacroAssembler::branchTestMagic(Condition cond, const Address& address, Label* label)
{
    branchTestMagicImpl(cond, address, label);
}

void
MacroAssembler::branchTestMagic(Condition cond, const BaseIndex& address, Label* label)
{
    branchTestMagicImpl(cond, address, label);
}

template <class L>
void
MacroAssembler::branchTestMagic(Condition cond, const ValueOperand& value, L label)
{
    branchTestMagicImpl(cond, value, label);
}

template <typename T, class L>
void
MacroAssembler::branchTestMagicImpl(Condition cond, const T& t, L label)
{
    Condition c = testMagic(cond, t);
    B(label, c);
}

void
MacroAssembler::branchTestMagic(Condition cond, const Address& valaddr, JSWhyMagic why, Label* label)
{
    uint64_t magic = MagicValue(why).asRawBits();
    cmpPtr(valaddr, ImmWord(magic));
    B(label, cond);
}

void
MacroAssembler::branchToComputedAddress(const BaseIndex& addr)
{
    // Not used by Rabaldr.
    MOZ_CRASH("NYI - branchToComputedAddress");
}

void
MacroAssembler::cmp32Move32(Condition cond, Register lhs, Register rhs, Register src,
                            Register dest)
{
    cmp32(lhs, rhs);
    Csel(ARMRegister(dest, 32), ARMRegister(src, 32), ARMRegister(dest, 32), cond);
}

void
MacroAssembler::cmp32Move32(Condition cond, Register lhs, const Address& rhs, Register src,
                            Register dest)
{
    cmp32(lhs, rhs);
    Csel(ARMRegister(dest, 32), ARMRegister(src, 32), ARMRegister(dest, 32), cond);
}

void
MacroAssembler::cmp32MovePtr(Condition cond, Register lhs, Imm32 rhs, Register src,
                             Register dest)
{
    cmp32(lhs, rhs);
    Csel(ARMRegister(dest, 64), ARMRegister(src, 64), ARMRegister(dest, 64), cond);
}

void
MacroAssembler::test32LoadPtr(Condition cond, const Address& addr, Imm32 mask, const Address& src,
                              Register dest)
{
    MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);

    // ARM64 does not support conditional loads, so we use a branch with a CSel
    // (to prevent Spectre attacks).
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    Label done;
    branchTest32(Assembler::InvertCondition(cond), addr, mask, &done);
    loadPtr(src, scratch64.asUnsized());
    Csel(ARMRegister(dest, 64), scratch64, ARMRegister(dest, 64), cond);
    bind(&done);
}

void
MacroAssembler::test32MovePtr(Condition cond, const Address& addr, Imm32 mask, Register src,
                              Register dest)
{
    MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);
    test32(addr, mask);
    Csel(ARMRegister(dest, 64), ARMRegister(src, 64), ARMRegister(dest, 64), cond);
}

void
MacroAssembler::spectreMovePtr(Condition cond, Register src, Register dest)
{
    Csel(ARMRegister(dest, 64), ARMRegister(src, 64), ARMRegister(dest, 64), cond);
}

void
MacroAssembler::spectreZeroRegister(Condition cond, Register, Register dest)
{
    Csel(ARMRegister(dest, 64), ARMRegister(dest, 64), vixl::xzr,
         Assembler::InvertCondition(cond));
}

void
MacroAssembler::spectreBoundsCheck32(Register index, Register length, Register maybeScratch,
                                     Label* failure)
{
    MOZ_ASSERT(length != maybeScratch);
    MOZ_ASSERT(index != maybeScratch);

    branch32(Assembler::BelowOrEqual, length, index, failure);

    if (JitOptions.spectreIndexMasking)
        Csel(ARMRegister(index, 32), ARMRegister(index, 32), vixl::wzr, Assembler::Above);
}

void
MacroAssembler::spectreBoundsCheck32(Register index, const Address& length, Register maybeScratch,
                                     Label* failure)
{
    MOZ_ASSERT(index != length.base);
    MOZ_ASSERT(length.base != maybeScratch);
    MOZ_ASSERT(index != maybeScratch);

    branch32(Assembler::BelowOrEqual, length, index, failure);

    if (JitOptions.spectreIndexMasking)
        Csel(ARMRegister(index, 32), ARMRegister(index, 32), vixl::wzr, Assembler::Above);
}

// ========================================================================
// Memory access primitives.
void
MacroAssembler::storeUncanonicalizedDouble(FloatRegister src, const Address& dest)
{
    Str(ARMFPRegister(src, 64), toMemOperand(dest));
}
void
MacroAssembler::storeUncanonicalizedDouble(FloatRegister src, const BaseIndex& dest)
{
    doBaseIndex(ARMFPRegister(src, 64), dest, vixl::STR_d);
}

void
MacroAssembler::storeUncanonicalizedFloat32(FloatRegister src, const Address& addr)
{
    Str(ARMFPRegister(src, 32), toMemOperand(addr));
}
void
MacroAssembler::storeUncanonicalizedFloat32(FloatRegister src, const BaseIndex& addr)
{
    doBaseIndex(ARMFPRegister(src, 32), addr, vixl::STR_s);
}

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
MacroAssembler::memoryBarrier(MemoryBarrierBits barrier)
{
    if (barrier == MembarStoreStore)
        Dmb(vixl::InnerShareable, vixl::BarrierWrites);
    else if (barrier == MembarLoadLoad)
        Dmb(vixl::InnerShareable, vixl::BarrierReads);
    else if (barrier)
        Dmb(vixl::InnerShareable, vixl::BarrierAll);
}

// ===============================================================
// Clamping functions.

void
MacroAssembler::clampIntToUint8(Register reg)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    const ARMRegister reg32(reg, 32);
    MOZ_ASSERT(!scratch32.Is(reg32));

    Cmp(reg32, Operand(reg32, vixl::UXTB));
    Csel(reg32, reg32, vixl::wzr, Assembler::GreaterThanOrEqual);
    Mov(scratch32, Operand(0xff));
    Csel(reg32, reg32, scratch32, Assembler::LessThanOrEqual);
}

// ========================================================================
// wasm support

template <class L>
void
MacroAssembler::wasmBoundsCheck(Condition cond, Register index, Register boundsCheckLimit, L label)
{
    // Not used on ARM64, we rely on signal handling instead
    MOZ_CRASH("NYI - wasmBoundsCheck");
}

template <class L>
void
MacroAssembler::wasmBoundsCheck(Condition cond, Register index, Address boundsCheckLimit, L label)
{
    // Not used on ARM64, we rely on signal handling instead
    MOZ_CRASH("NYI - wasmBoundsCheck");
}

//}}} check_macroassembler_style
// ===============================================================

void
MacroAssemblerCompat::addToStackPtr(Register src)
{
    Add(GetStackPointer64(), GetStackPointer64(), ARMRegister(src, 64));
}

void
MacroAssemblerCompat::addToStackPtr(Imm32 imm)
{
    Add(GetStackPointer64(), GetStackPointer64(), Operand(imm.value));
}

void
MacroAssemblerCompat::addToStackPtr(const Address& src)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch = temps.AcquireX();
    Ldr(scratch, toMemOperand(src));
    Add(GetStackPointer64(), GetStackPointer64(), scratch);
}

void
MacroAssemblerCompat::addStackPtrTo(Register dest)
{
    Add(ARMRegister(dest, 64), ARMRegister(dest, 64), GetStackPointer64());
}

void
MacroAssemblerCompat::subFromStackPtr(Register src)
{
    Sub(GetStackPointer64(), GetStackPointer64(), ARMRegister(src, 64));
    syncStackPtr();
}

void
MacroAssemblerCompat::subFromStackPtr(Imm32 imm)
{
    Sub(GetStackPointer64(), GetStackPointer64(), Operand(imm.value));
    syncStackPtr();
}

void
MacroAssemblerCompat::subStackPtrFrom(Register dest)
{
    Sub(ARMRegister(dest, 64), ARMRegister(dest, 64), GetStackPointer64());
}

void
MacroAssemblerCompat::andToStackPtr(Imm32 imm)
{
    if (sp.Is(GetStackPointer64())) {
        vixl::UseScratchRegisterScope temps(this);
        const ARMRegister scratch = temps.AcquireX();
        Mov(scratch, sp);
        And(sp, scratch, Operand(imm.value));
        // syncStackPtr() not needed since our SP is the real SP.
    } else {
        And(GetStackPointer64(), GetStackPointer64(), Operand(imm.value));
        syncStackPtr();
    }
}

void
MacroAssemblerCompat::andStackPtrTo(Register dest)
{
    And(ARMRegister(dest, 64), ARMRegister(dest, 64), GetStackPointer64());
}

void
MacroAssemblerCompat::moveToStackPtr(Register src)
{
    Mov(GetStackPointer64(), ARMRegister(src, 64));
    syncStackPtr();
}

void
MacroAssemblerCompat::moveStackPtrTo(Register dest)
{
    Mov(ARMRegister(dest, 64), GetStackPointer64());
}

void
MacroAssemblerCompat::loadStackPtr(const Address& src)
{
    if (sp.Is(GetStackPointer64())) {
        vixl::UseScratchRegisterScope temps(this);
        const ARMRegister scratch = temps.AcquireX();
        Ldr(scratch, toMemOperand(src));
        Mov(sp, scratch);
        // syncStackPtr() not needed since our SP is the real SP.
    } else {
        Ldr(GetStackPointer64(), toMemOperand(src));
        syncStackPtr();
    }
}

void
MacroAssemblerCompat::storeStackPtr(const Address& dest)
{
    if (sp.Is(GetStackPointer64())) {
        vixl::UseScratchRegisterScope temps(this);
        const ARMRegister scratch = temps.AcquireX();
        Mov(scratch, sp);
        Str(scratch, toMemOperand(dest));
    } else {
        Str(GetStackPointer64(), toMemOperand(dest));
    }
}

void
MacroAssemblerCompat::branchTestStackPtr(Condition cond, Imm32 rhs, Label* label)
{
    if (sp.Is(GetStackPointer64())) {
        vixl::UseScratchRegisterScope temps(this);
        const ARMRegister scratch = temps.AcquireX();
        Mov(scratch, sp);
        Tst(scratch, Operand(rhs.value));
    } else {
        Tst(GetStackPointer64(), Operand(rhs.value));
    }
    B(label, cond);
}

void
MacroAssemblerCompat::branchStackPtr(Condition cond, Register rhs_, Label* label)
{
    ARMRegister rhs(rhs_, 64);
    if (sp.Is(GetStackPointer64())) {
        vixl::UseScratchRegisterScope temps(this);
        const ARMRegister scratch = temps.AcquireX();
        Mov(scratch, sp);
        Cmp(scratch, rhs);
    } else {
        Cmp(GetStackPointer64(), rhs);
    }
    B(label, cond);
}

void
MacroAssemblerCompat::branchStackPtrRhs(Condition cond, Address lhs, Label* label)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch = temps.AcquireX();
    Ldr(scratch, toMemOperand(lhs));
    // Cmp disallows SP as the rhs, so flip the operands and invert the
    // condition.
    Cmp(GetStackPointer64(), scratch);
    B(label, Assembler::InvertCondition(cond));
}

// If source is a double, load into dest.
// If source is int32, convert to double and store in dest.
// Else, branch to failure.
void
MacroAssemblerCompat::ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure)
{
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

void
MacroAssemblerCompat::unboxValue(const ValueOperand& src, AnyRegister dest, JSValueType type)
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

} // namespace jit
} // namespace js

#endif /* jit_arm64_MacroAssembler_arm64_inl_h */
