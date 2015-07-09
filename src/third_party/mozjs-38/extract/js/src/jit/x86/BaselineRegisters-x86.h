/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_BaselineRegisters_x86_h
#define jit_x86_BaselineRegisters_x86_h

#include "jit/MacroAssembler.h"

namespace js {
namespace jit {

static MOZ_CONSTEXPR_VAR Register BaselineFrameReg = ebp;
static MOZ_CONSTEXPR_VAR Register BaselineStackReg = esp;

// ValueOperands R0, R1, and R2
static MOZ_CONSTEXPR_VAR ValueOperand R0(ecx, edx);
static MOZ_CONSTEXPR_VAR ValueOperand R1(eax, ebx);
static MOZ_CONSTEXPR_VAR ValueOperand R2(esi, edi);

// BaselineTailCallReg and BaselineStubReg reuse
// registers from R2.
static MOZ_CONSTEXPR_VAR Register BaselineTailCallReg = esi;
static MOZ_CONSTEXPR_VAR Register BaselineStubReg     = edi;

static MOZ_CONSTEXPR_VAR Register ExtractTemp0        = InvalidReg;
static MOZ_CONSTEXPR_VAR Register ExtractTemp1        = InvalidReg;

// FloatReg0 must be equal to ReturnFloatReg.
static MOZ_CONSTEXPR_VAR FloatRegister FloatReg0      = xmm0;
static MOZ_CONSTEXPR_VAR FloatRegister FloatReg1      = xmm1;

} // namespace jit
} // namespace js

#endif /* jit_x86_BaselineRegisters_x86_h */
