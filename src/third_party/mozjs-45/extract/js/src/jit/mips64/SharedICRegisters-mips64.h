/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_SharedICRegisters_mips64_h
#define jit_mips64_SharedICRegisters_mips64_h

#include "jit/MacroAssembler.h"

namespace js {
namespace jit {

static MOZ_CONSTEXPR_VAR Register BaselineFrameReg = s5;
static MOZ_CONSTEXPR_VAR Register BaselineStackReg = sp;

// ValueOperands R0, R1, and R2.
// R0 == JSReturnReg, and R2 uses registers not preserved across calls. R1 value
// should be preserved across calls.
static MOZ_CONSTEXPR_VAR ValueOperand R0(v1);
static MOZ_CONSTEXPR_VAR ValueOperand R1(s4);
static MOZ_CONSTEXPR_VAR ValueOperand R2(a6);

// ICTailCallReg and ICStubReg
// These use registers that are not preserved across calls.
static MOZ_CONSTEXPR_VAR Register ICTailCallReg = ra;
static MOZ_CONSTEXPR_VAR Register ICStubReg = a5;

static MOZ_CONSTEXPR_VAR Register ExtractTemp0 = s6;
static MOZ_CONSTEXPR_VAR Register ExtractTemp1 = s7;

// Register used internally by MacroAssemblerMIPS.
static MOZ_CONSTEXPR_VAR Register BaselineSecondScratchReg = SecondScratchReg;

// Note that ICTailCallReg is actually just the link register.
// In MIPS code emission, we do not clobber ICTailCallReg since we keep
// the return address for calls there.

// FloatReg0 must be equal to ReturnFloatReg.
static MOZ_CONSTEXPR_VAR FloatRegister FloatReg0 = f0;
static MOZ_CONSTEXPR_VAR FloatRegister FloatReg1 = f2;

} // namespace jit
} // namespace js

#endif /* jit_mips64_SharedICRegisters_mips64_h */
