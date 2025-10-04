/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_wasm32_SharedICRegisters_wasm32_h
#define jit_wasm32_SharedICRegisters_wasm32_h

#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "jit/wasm32/MacroAssembler-wasm32.h"

namespace js::jit {

static constexpr Register BaselineStackReg = StackPointer;
static constexpr Register BaselineFrameReg = FramePointer;

static constexpr ValueOperand R0 = JSReturnOperand;
static constexpr ValueOperand R1 = JSReturnOperand;
static constexpr ValueOperand R2 = JSReturnOperand;

static constexpr Register ICTailCallReg{Registers::invalid_reg};
static constexpr Register ICStubReg{Registers::invalid_reg};

static constexpr Register ExtractTemp0{Registers::invalid_reg};
static constexpr Register ExtractTemp1{Registers::invalid_reg};

static constexpr FloatRegister FloatReg0 = {FloatRegisters::invalid_reg};
static constexpr FloatRegister FloatReg1 = {FloatRegisters::invalid_reg};
static constexpr FloatRegister FloatReg2 = {FloatRegisters::invalid_reg};
static constexpr FloatRegister FloatReg3 = {FloatRegisters::invalid_reg};

}  // namespace js::jit

#endif /* jit_wasm32_SharedICRegisters_wasm32_h */
