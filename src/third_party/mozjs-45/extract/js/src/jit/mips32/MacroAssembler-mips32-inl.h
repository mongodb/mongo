/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_MacroAssembler_mips32_inl_h
#define jit_mips32_MacroAssembler_mips32_inl_h

#include "jit/mips32/MacroAssembler-mips32.h"

#include "jit/mips-shared/MacroAssembler-mips-shared-inl.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style
// ===============================================================
// Logical instructions

void
MacroAssembler::andPtr(Register src, Register dest)
{
    ma_and(dest, src);
}

void
MacroAssembler::andPtr(Imm32 imm, Register dest)
{
    ma_and(dest, imm);
}

void
MacroAssembler::and64(Imm64 imm, Register64 dest)
{
    and32(Imm32(imm.value & LOW_32_MASK), dest.low);
    and32(Imm32((imm.value >> 32) & LOW_32_MASK), dest.high);
}

void
MacroAssembler::orPtr(Register src, Register dest)
{
    ma_or(dest, src);
}

void
MacroAssembler::orPtr(Imm32 imm, Register dest)
{
    ma_or(dest, imm);
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
    ma_xor(dest.low, src.low);
    ma_xor(dest.high, src.high);
}

void
MacroAssembler::xorPtr(Register src, Register dest)
{
    ma_xor(dest, src);
}

void
MacroAssembler::xorPtr(Imm32 imm, Register dest)
{
    ma_xor(dest, imm);
}

// ===============================================================
// Arithmetic functions

void
MacroAssembler::add64(Register64 src, Register64 dest)
{
    as_addu(dest.low, dest.low, src.low);
    as_sltu(ScratchRegister, dest.low, src.low);
    as_addu(dest.high, dest.high, src.high);
    as_addu(dest.high, dest.high, ScratchRegister);
}

// ===============================================================
// Shift functions

void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    ma_sll(dest, dest, imm);
}

void
MacroAssembler::lshift64(Imm32 imm, Register64 dest)
{
    ScratchRegisterScope scratch(*this);
    as_sll(dest.high, dest.high, imm.value);
    as_srl(scratch, dest.low, 32 - imm.value);
    as_or(dest.high, dest.high, scratch);
    as_sll(dest.low, dest.low, imm.value);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    ma_srl(dest, dest, imm);
}

void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    ma_sra(dest, dest, imm);
}

void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    ScratchRegisterScope scratch(*this);
    as_srl(dest.low, dest.low, imm.value);
    as_sll(scratch, dest.high, 32 - imm.value);
    as_or(dest.low, dest.low, scratch);
    as_srl(dest.high, dest.high, imm.value);
}

//}}} check_macroassembler_style
// ===============================================================

} // namespace jit
} // namespace js

#endif /* jit_mips32_MacroAssembler_mips32_inl_h */
