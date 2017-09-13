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
    movq(ImmWord(uintptr_t(imm.value)), ScratchReg);
    andq(ScratchReg, dest.reg);
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

// ===============================================================
// Arithmetic functions

void
MacroAssembler::add64(Register64 src, Register64 dest)
{
    addq(src.reg, dest.reg);
}

// ===============================================================
// Shift functions

void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    shlq(imm, dest);
}

void
MacroAssembler::lshift64(Imm32 imm, Register64 dest)
{
    shlq(imm, dest.reg);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    shrq(imm, dest);
}

void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    sarq(imm, dest);
}

void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    shrq(imm, dest.reg);
}

//}}} check_macroassembler_style
// ===============================================================

} // namespace jit
} // namespace js

#endif /* jit_x64_MacroAssembler_x64_inl_h */
