/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_none_SharedICRegisters_none_h
#define jit_none_SharedICRegisters_none_h

#include "jit/none/MacroAssembler-none.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

static constexpr ValueOperand R0 = JSReturnOperand;
static constexpr ValueOperand R1 = JSReturnOperand;
static constexpr ValueOperand R2 = JSReturnOperand;

static constexpr Register ICTailCallReg{Registers::invalid_reg};
static constexpr Register ICStubReg{Registers::invalid_reg};

static constexpr FloatRegister FloatReg0 = {FloatRegisters::invalid_reg};
static constexpr FloatRegister FloatReg1 = {FloatRegisters::invalid_reg};
static constexpr FloatRegister FloatReg2 = {FloatRegisters::invalid_reg};
static constexpr FloatRegister FloatReg3 = {FloatRegisters::invalid_reg};

}  // namespace jit
}  // namespace js

#endif /* jit_none_SharedICRegisters_none_h */
