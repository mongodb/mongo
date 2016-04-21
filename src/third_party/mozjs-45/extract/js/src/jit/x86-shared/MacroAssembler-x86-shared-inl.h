/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_MacroAssembler_x86_shared_inl_h
#define jit_x86_shared_MacroAssembler_x86_shared_inl_h

#include "jit/x86-shared/MacroAssembler-x86-shared.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style
// ===============================================================
// Logical instructions

void
MacroAssembler::not32(Register reg)
{
    notl(reg);
}

void
MacroAssembler::and32(Register src, Register dest)
{
    andl(src, dest);
}

void
MacroAssembler::and32(Imm32 imm, Register dest)
{
    andl(imm, dest);
}

void
MacroAssembler::and32(Imm32 imm, const Address& dest)
{
    andl(imm, Operand(dest));
}

void
MacroAssembler::and32(const Address& src, Register dest)
{
    andl(Operand(src), dest);
}

void
MacroAssembler::or32(Register src, Register dest)
{
    orl(src, dest);
}

void
MacroAssembler::or32(Imm32 imm, Register dest)
{
    orl(imm, dest);
}

void
MacroAssembler::or32(Imm32 imm, const Address& dest)
{
    orl(imm, Operand(dest));
}

void
MacroAssembler::xor32(Register src, Register dest)
{
    xorl(src, dest);
}

void
MacroAssembler::xor32(Imm32 imm, Register dest)
{
    xorl(imm, dest);
}

// ===============================================================
// Arithmetic instructions

void
MacroAssembler::sub32(Register src, Register dest)
{
    subl(src, dest);
}

void
MacroAssembler::sub32(Imm32 imm, Register dest)
{
    subl(imm, dest);
}

void
MacroAssembler::sub32(const Address& src, Register dest)
{
    subl(Operand(src), dest);
}

//}}} check_macroassembler_style
// ===============================================================

} // namespace jit
} // namespace js

#endif /* jit_x86_shared_MacroAssembler_x86_shared_inl_h */
