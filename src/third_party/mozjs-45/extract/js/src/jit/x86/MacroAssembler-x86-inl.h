/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_MacroAssembler_x86_inl_h
#define jit_x86_MacroAssembler_x86_inl_h

#include "jit/x86/MacroAssembler-x86.h"

#include "jit/x86-shared/MacroAssembler-x86-shared-inl.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style
// ===============================================================

void
MacroAssembler::andPtr(Register src, Register dest)
{
    andl(src, dest);
}

void
MacroAssembler::andPtr(Imm32 imm, Register dest)
{
    andl(imm, dest);
}

void
MacroAssembler::and64(Imm64 imm, Register64 dest)
{
    andl(Imm32(imm.value & 0xFFFFFFFFL), dest.low);
    andl(Imm32((imm.value >> 32) & 0xFFFFFFFFL), dest.high);
}

void
MacroAssembler::orPtr(Register src, Register dest)
{
    orl(src, dest);
}

void
MacroAssembler::orPtr(Imm32 imm, Register dest)
{
    orl(imm, dest);
}

void
MacroAssembler::or64(Register64 src, Register64 dest)
{
    orl(src.low, dest.low);
    orl(src.high, dest.high);
}

void
MacroAssembler::xor64(Register64 src, Register64 dest)
{
    xorl(src.low, dest.low);
    xorl(src.high, dest.high);
}

void
MacroAssembler::xorPtr(Register src, Register dest)
{
    xorl(src, dest);
}

void
MacroAssembler::xorPtr(Imm32 imm, Register dest)
{
    xorl(imm, dest);
}

// ===============================================================
// Arithmetic functions

void
MacroAssembler::add64(Register64 src, Register64 dest)
{
    addl(src.low, dest.low);
    adcl(src.high, dest.high);
}

// ===============================================================
// Shift functions

void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    shll(imm, dest);
}

void
MacroAssembler::lshift64(Imm32 imm, Register64 dest)
{
    shldl(imm, dest.low, dest.high);
    shll(imm, dest.low);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    shrl(imm, dest);
}

void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    sarl(imm, dest);
}

void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    shrdl(imm, dest.high, dest.low);
    shrl(imm, dest.high);
}

//}}} check_macroassembler_style
// ===============================================================

} // namespace jit
} // namespace js

#endif /* jit_x86_MacroAssembler_x86_inl_h */
