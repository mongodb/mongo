/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_SharedICRegisters_x86_h
#define jit_x86_SharedICRegisters_x86_h

#include "jit/MacroAssembler.h"

namespace js {
namespace jit {

static constexpr Register BaselineFrameReg = ebp;
static constexpr Register BaselineStackReg = esp;

// ValueOperands R0, R1, and R2
static constexpr ValueOperand R0(ecx, edx);
static constexpr ValueOperand R1(eax, ebx);
static constexpr ValueOperand R2(esi, edi);

// ICTailCallReg and ICStubReg reuse
// registers from R2.
static constexpr Register ICTailCallReg       = esi;
static constexpr Register ICStubReg           = edi;

static constexpr Register ExtractTemp0        = InvalidReg;
static constexpr Register ExtractTemp1        = InvalidReg;

// FloatReg0 must be equal to ReturnFloatReg.
static constexpr FloatRegister FloatReg0      = xmm0;
static constexpr FloatRegister FloatReg1      = xmm1;

} // namespace jit
} // namespace js

#endif /* jit_x86_SharedICRegisters_x86_h */
