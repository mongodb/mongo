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
MacroAssembler::add64(Register64 src, Register64 dest)
{
    addPtr(src.reg, dest.reg);
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

// ===============================================================
// Shift functions

void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    Lsl(ARMRegister(dest, 64), ARMRegister(dest, 64), imm.value);
}

void
MacroAssembler::lshift64(Imm32 imm, Register64 dest)
{
    lshiftPtr(imm, dest.reg);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    Lsr(ARMRegister(dest, 64), ARMRegister(dest, 64), imm.value);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register src, Register dest)
{
    Lsr(ARMRegister(dest, 64), ARMRegister(src, 64), imm.value);
}

void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    Asr(ARMRegister(dest, 64), ARMRegister(dest, 64), imm.value);
}

void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    rshiftPtr(imm, dest.reg);
}

//}}} check_macroassembler_style
// ===============================================================

template <typename T>
void
MacroAssemblerCompat::andToStackPtr(T t)
{
    asMasm().andPtr(t, getStackPointer());
    syncStackPtr();
}

template <typename T>
void
MacroAssemblerCompat::andStackPtrTo(T t)
{
    asMasm().andPtr(getStackPointer(), t);
}

} // namespace jit
} // namespace js

#endif /* jit_arm64_MacroAssembler_arm64_inl_h */
