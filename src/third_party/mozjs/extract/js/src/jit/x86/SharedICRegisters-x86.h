/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_SharedICRegisters_x86_h
#define jit_x86_SharedICRegisters_x86_h

#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "jit/x86/Assembler-x86.h"

namespace js {
namespace jit {

// ValueOperands R0, R1, and R2
static constexpr ValueOperand R0(ecx, edx);
static constexpr ValueOperand R1(eax, ebx);
static constexpr ValueOperand R2(esi, edi);

// ICTailCallReg and ICStubReg reuse
// registers from R2.
static constexpr Register ICTailCallReg = esi;
static constexpr Register ICStubReg = edi;

// FloatReg0 must be equal to ReturnFloatReg.
static constexpr FloatRegister FloatReg0 = xmm0;
static constexpr FloatRegister FloatReg1 = xmm1;
static constexpr FloatRegister FloatReg2 = xmm2;
static constexpr FloatRegister FloatReg3 = xmm3;

}  // namespace jit
}  // namespace js

#endif /* jit_x86_SharedICRegisters_x86_h */
