/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_MacroAssembler_arm_inl_h
#define jit_arm_MacroAssembler_arm_inl_h

#include "jit/arm/MacroAssembler-arm.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style
// ===============================================================
// Logical instructions

void
MacroAssembler::not32(Register reg)
{
    ma_mvn(reg, reg);
}

void
MacroAssembler::and32(Register src, Register dest)
{
    ma_and(src, dest, SetCC);
}

void
MacroAssembler::and32(Imm32 imm, Register dest)
{
    ma_and(imm, dest, SetCC);
}

void
MacroAssembler::and32(Imm32 imm, const Address& dest)
{
    ScratchRegisterScope scratch(*this);
    load32(dest, scratch);
    ma_and(imm, scratch);
    store32(scratch, dest);
}

void
MacroAssembler::and32(const Address& src, Register dest)
{
    ScratchRegisterScope scratch(*this);
    load32(src, scratch);
    ma_and(scratch, dest, SetCC);
}

void
MacroAssembler::andPtr(Register src, Register dest)
{
    ma_and(src, dest);
}

void
MacroAssembler::andPtr(Imm32 imm, Register dest)
{
    ma_and(imm, dest);
}

void
MacroAssembler::and64(Imm64 imm, Register64 dest)
{
    and32(Imm32(imm.value & 0xFFFFFFFFL), dest.low);
    and32(Imm32((imm.value >> 32) & 0xFFFFFFFFL), dest.high);
}

void
MacroAssembler::or32(Register src, Register dest)
{
    ma_orr(src, dest);
}

void
MacroAssembler::or32(Imm32 imm, Register dest)
{
    ma_orr(imm, dest);
}

void
MacroAssembler::or32(Imm32 imm, const Address& dest)
{
    ScratchRegisterScope scratch(*this);
    load32(dest, scratch);
    ma_orr(imm, scratch);
    store32(scratch, dest);
}

void
MacroAssembler::orPtr(Register src, Register dest)
{
    ma_orr(src, dest);
}

void
MacroAssembler::orPtr(Imm32 imm, Register dest)
{
    ma_orr(imm, dest);
}

void
MacroAssembler::or64(Register64 src, Register64 dest)
{
    or32(src.low, dest.low);
    or32(src.high, dest.high);
}

void
MacroAssembler::xor64(Register64 src, Register64 dest)
{
    ma_eor(src.low, dest.low);
    ma_eor(src.high, dest.high);
}

void
MacroAssembler::xor32(Imm32 imm, Register dest)
{
    ma_eor(imm, dest, SetCC);
}

void
MacroAssembler::xorPtr(Register src, Register dest)
{
    ma_eor(src, dest);
}

void
MacroAssembler::xorPtr(Imm32 imm, Register dest)
{
    ma_eor(imm, dest);
}

// ===============================================================
// Arithmetic functions

void
MacroAssembler::add64(Register64 src, Register64 dest)
{
    ma_add(src.low, dest.low, SetCC);
    ma_adc(src.high, dest.high);
}

void
MacroAssembler::sub32(Register src, Register dest)
{
    ma_sub(src, dest, SetCC);
}

void
MacroAssembler::sub32(Imm32 imm, Register dest)
{
    ma_sub(imm, dest, SetCC);
}

void
MacroAssembler::sub32(const Address& src, Register dest)
{
    ScratchRegisterScope scratch(*this);
    load32(src, scratch);
    ma_sub(scratch, dest, SetCC);
}

// ===============================================================
// Shift functions

void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    ma_lsl(imm, dest, dest);
}

void
MacroAssembler::lshift64(Imm32 imm, Register64 dest)
{
    as_mov(dest.high, lsl(dest.high, imm.value));
    as_orr(dest.high, dest.high, lsr(dest.low, 32 - imm.value));
    as_mov(dest.low, lsl(dest.low, imm.value));
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    ma_lsr(imm, dest, dest);
}

void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    ma_asr(imm, dest, dest);
}

void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    as_mov(dest.low, lsr(dest.low, imm.value));
    as_orr(dest.low, dest.low, lsl(dest.high, 32 - imm.value));
    as_mov(dest.high, lsr(dest.high, imm.value));
}

//}}} check_macroassembler_style
// ===============================================================

} // namespace jit
} // namespace js

#endif /* jit_arm_MacroAssembler_arm_inl_h */
