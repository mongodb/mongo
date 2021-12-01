/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_SharedICRegisters_x64_h
#define jit_x64_SharedICRegisters_x64_h

#include "jit/MacroAssembler.h"

namespace js {
namespace jit {

static constexpr Register BaselineFrameReg    = rbp;
static constexpr Register BaselineStackReg    = rsp;

static constexpr ValueOperand R0(rcx);
static constexpr ValueOperand R1(rbx);
static constexpr ValueOperand R2(rax);

static constexpr Register ICTailCallReg       = rsi;
static constexpr Register ICStubReg           = rdi;

static constexpr Register ExtractTemp0        = r14;
static constexpr Register ExtractTemp1        = r15;

// FloatReg0 must be equal to ReturnFloatReg.
static constexpr FloatRegister FloatReg0      = xmm0;
static constexpr FloatRegister FloatReg1      = xmm1;

} // namespace jit
} // namespace js

#endif /* jit_x64_SharedICRegisters_x64_h */
