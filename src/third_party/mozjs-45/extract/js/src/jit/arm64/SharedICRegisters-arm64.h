/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_SharedICRegisters_arm64_h
#define jit_arm64_SharedICRegisters_arm64_h

#include "jit/MacroAssembler.h"

namespace js {
namespace jit {

// Must be a callee-saved register for preservation around generateEnterJIT().
static constexpr Register BaselineFrameReg = r23;
static constexpr ARMRegister BaselineFrameReg64 = { BaselineFrameReg, 64 };

// BaselineStackReg is intentionally undefined on ARM64.
// Refer to the comment next to the definition of RealStackPointer.

// ValueOperands R0, R1, and R2.
// R0 == JSReturnReg, and R2 uses registers not preserved across calls.
// R1 value should be preserved across calls.
static constexpr Register R0_ = r2;
static constexpr Register R1_ = r19;
static constexpr Register R2_ = r0;

static constexpr ValueOperand R0(R0_);
static constexpr ValueOperand R1(R1_);
static constexpr ValueOperand R2(R2_);

// ICTailCallReg and ICStubReg use registers that are not preserved across calls.
static constexpr Register ICTailCallReg = r30;
static constexpr Register ICStubReg = r9;

// ExtractTemps must be callee-save registers:
// ICSetProp_Native::Compiler::generateStubCode() stores the object
// in ExtractTemp0, but then calls callTypeUpdateIC(), which clobbers
// caller-save registers.
// They should also not be the scratch registers ip0 or ip1,
// since those get clobbered all the time.
static constexpr Register ExtractTemp0 = r24;
static constexpr Register ExtractTemp1 = r25;

// R7 - R9 are generally available for use within stubcode.

// Note that BaselineTailCallReg is actually just the link
// register.  In ARM code emission, we do not clobber BaselineTailCallReg
// since we keep the return address for calls there.

static constexpr FloatRegister FloatReg0 = { FloatRegisters::d0, FloatRegisters::Double };
static constexpr FloatRegister FloatReg1 = { FloatRegisters::d1, FloatRegisters::Double };

} // namespace jit
} // namespace js

#endif // jit_arm64_SharedICRegisters_arm64_h
