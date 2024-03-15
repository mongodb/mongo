/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_SharedICRegisters_mips64_h
#define jit_mips64_SharedICRegisters_mips64_h

#include "jit/mips64/Assembler-mips64.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

// ValueOperands R0, R1, and R2.
// R0 == JSReturnReg, and R2 uses registers not preserved across calls. R1 value
// should be preserved across calls.
static constexpr ValueOperand R0(v1);
static constexpr ValueOperand R1(s4);
static constexpr ValueOperand R2(a6);

// ICTailCallReg and ICStubReg
// These use registers that are not preserved across calls.
static constexpr Register ICTailCallReg = ra;
static constexpr Register ICStubReg = a5;

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

#endif /* jit_mips64_SharedICRegisters_mips64_h */
