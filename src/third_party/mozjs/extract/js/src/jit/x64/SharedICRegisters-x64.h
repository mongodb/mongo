/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_SharedICRegisters_x64_h
#define jit_x64_SharedICRegisters_x64_h

#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "jit/x64/Assembler-x64.h"

namespace js {
namespace jit {

static constexpr ValueOperand R0(rcx);
static constexpr ValueOperand R1(rbx);
static constexpr ValueOperand R2(rax);

static constexpr Register ICTailCallReg = rsi;
static constexpr Register ICStubReg = rdi;

// FloatReg0 must be equal to ReturnFloatReg.
static constexpr FloatRegister FloatReg0 = xmm0;
static constexpr FloatRegister FloatReg1 = xmm1;
static constexpr FloatRegister FloatReg2 = xmm2;
static constexpr FloatRegister FloatReg3 = xmm3;

}  // namespace jit
}  // namespace js

#endif /* jit_x64_SharedICRegisters_x64_h */
