/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_SharedICRegisters_mips32_h
#define jit_mips32_SharedICRegisters_mips32_h

#include "jit/MacroAssembler.h"

namespace js {
namespace jit {

static constexpr Register BaselineFrameReg = s5;
static constexpr Register BaselineStackReg = sp;

static constexpr ValueOperand R0(a3, a2);
static constexpr ValueOperand R1(s7, s6);
static constexpr ValueOperand R2(t7, t6);

// ICTailCallReg and ICStubReg
// These use registers that are not preserved across calls.
static constexpr Register ICTailCallReg = ra;
static constexpr Register ICStubReg = t5;

static constexpr Register ExtractTemp0 = InvalidReg;
static constexpr Register ExtractTemp1 = InvalidReg;

// Register used internally by MacroAssemblerMIPS.
static constexpr Register BaselineSecondScratchReg = SecondScratchReg;

// Note that ICTailCallReg is actually just the link register.
// In MIPS code emission, we do not clobber ICTailCallReg since we keep
// the return address for calls there.

// FloatReg0 must be equal to ReturnFloatReg.
static constexpr FloatRegister FloatReg0 = f0;
static constexpr FloatRegister FloatReg1 = f2;

} // namespace jit
} // namespace js

#endif /* jit_mips32_SharedICRegisters_mips32_h */
