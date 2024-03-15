/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_SharedICRegisters_mips32_h
#define jit_mips32_SharedICRegisters_mips32_h

#include "jit/mips32/Assembler-mips32.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

static constexpr ValueOperand R0(a3, a2);
static constexpr ValueOperand R1(s7, s6);
static constexpr ValueOperand R2(t7, t6);

// ICTailCallReg and ICStubReg
// These use registers that are not preserved across calls.
static constexpr Register ICTailCallReg = ra;
static constexpr Register ICStubReg = t5;

// Register used internally by MacroAssemblerMIPS.
static constexpr Register BaselineSecondScratchReg = SecondScratchReg;

// Note that ICTailCallReg is actually just the link register.
// In MIPS code emission, we do not clobber ICTailCallReg since we keep
// the return address for calls there.

// FloatReg0 must be equal to ReturnFloatReg.
static constexpr FloatRegister FloatReg0 = f0;
static constexpr FloatRegister FloatReg1 = f2;
static constexpr FloatRegister FloatReg2 = f4;
static constexpr FloatRegister FloatReg3 = f6;

}  // namespace jit
}  // namespace js

#endif /* jit_mips32_SharedICRegisters_mips32_h */
