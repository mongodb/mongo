/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_SharedICRegisters_arm_h
#define jit_arm_SharedICRegisters_arm_h

#include "jit/arm/Assembler-arm.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

// r15 = program-counter
// r14 = link-register
// r13 = stack-pointer
// r11 = frame-pointer

// ValueOperands R0, R1, and R2.
// R0 == JSReturnReg, and R2 uses registers not preserved across calls. R1 value
// should be preserved across calls.
static constexpr ValueOperand R0(r3, r2);
static constexpr ValueOperand R1(r5, r4);
static constexpr ValueOperand R2(r1, r0);

// ICTailCallReg and ICStubReg
// These use registers that are not preserved across calls.
static constexpr Register ICTailCallReg = r14;
static constexpr Register ICStubReg = r9;

// Register used internally by MacroAssemblerARM.
static constexpr Register BaselineSecondScratchReg = r6;

// R7 - R9 are generally available for use within stubcode.

// Note that ICTailCallReg is actually just the link register. In ARM code
// emission, we do not clobber ICTailCallReg since we keep the return
// address for calls there.

// FloatReg0 must be equal to ReturnFloatReg.
static constexpr FloatRegister FloatReg0 = d0;
static constexpr FloatRegister FloatReg1 = d1;
static constexpr FloatRegister FloatReg2 = d2;
static constexpr FloatRegister FloatReg3 = d3;

}  // namespace jit
}  // namespace js

#endif /* jit_arm_SharedICRegisters_arm_h */
