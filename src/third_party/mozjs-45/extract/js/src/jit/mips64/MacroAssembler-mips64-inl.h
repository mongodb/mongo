/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_MacroAssembler_mips64_inl_h
#define jit_mips64_MacroAssembler_mips64_inl_h

#include "jit/mips64/MacroAssembler-mips64.h"

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
    ma_li(ScratchRegister, ImmWord(imm.value));
    ma_and(dest.reg, ScratchRegister);
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
    ma_or(dest.reg, src.reg);
}

void
MacroAssembler::xor64(Register64 src, Register64 dest)
{
    ma_xor(dest.reg, src.reg);
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
    addPtr(src.reg, dest.reg);
}

// ===============================================================
// Shift functions

void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    ma_dsll(dest, dest, imm);
}

void
MacroAssembler::lshift64(Imm32 imm, Register64 dest)
{
    ma_dsll(dest.reg, dest.reg, imm);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    ma_dsrl(dest, dest, imm);
}

void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    ma_dsra(dest, dest, imm);
}

void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    ma_dsrl(dest.reg, dest.reg, imm);
}

//}}} check_macroassembler_style
// ===============================================================

} // namespace jit
} // namespace js

#endif /* jit_mips64_MacroAssembler_mips64_inl_h */
