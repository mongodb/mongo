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
MacroAssembler::xor32(Imm32 imm, Register dest)
{
    ma_xor(dest, imm);
}

// ===============================================================
// Arithmetic instructions

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

//}}} check_macroassembler_style
// ===============================================================

} // namespace jit
} // namespace js

#endif /* jit_mips_shared_MacroAssembler_mips_shared_inl_h */
