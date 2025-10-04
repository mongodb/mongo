/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_riscv64_Register_riscv64_h
#define jit_riscv64_Register_riscv64_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "jit/Registers.h"
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

static constexpr Register zero{Registers::zero};
static constexpr Register ra{Registers::ra};
static constexpr Register tp{Registers::tp};
static constexpr Register sp{Registers::sp};
static constexpr Register gp{Registers::gp};
static constexpr Register a0{Registers::a0};
static constexpr Register a1{Registers::a1};
static constexpr Register a2{Registers::a2};
static constexpr Register a3{Registers::a3};
static constexpr Register a4{Registers::a4};
static constexpr Register a5{Registers::a5};
static constexpr Register a6{Registers::a6};
static constexpr Register a7{Registers::a7};
static constexpr Register t0{Registers::t0};
static constexpr Register t1{Registers::t1};
static constexpr Register t2{Registers::t2};
static constexpr Register t3{Registers::t3};
static constexpr Register t4{Registers::t4};
static constexpr Register t5{Registers::t5};
static constexpr Register t6{Registers::t6};
static constexpr Register fp{Registers::fp};
static constexpr Register s1{Registers::s1};
static constexpr Register s2{Registers::s2};
static constexpr Register s3{Registers::s3};
static constexpr Register s4{Registers::s4};
static constexpr Register s5{Registers::s5};
static constexpr Register s6{Registers::s6};
static constexpr Register s7{Registers::s7};
static constexpr Register s8{Registers::s8};
static constexpr Register s9{Registers::s9};
static constexpr Register s10{Registers::s10};
static constexpr Register s11{Registers::s11};

static constexpr FloatRegister ft0{FloatRegisters::f0};
static constexpr FloatRegister ft1{FloatRegisters::f1};
static constexpr FloatRegister ft2{FloatRegisters::f2};
static constexpr FloatRegister ft3{FloatRegisters::f3};
static constexpr FloatRegister ft4{FloatRegisters::f4};
static constexpr FloatRegister ft5{FloatRegisters::f5};
static constexpr FloatRegister ft6{FloatRegisters::f6};
static constexpr FloatRegister ft7{FloatRegisters::f7};
static constexpr FloatRegister fs0{FloatRegisters::f8};
static constexpr FloatRegister fs1{FloatRegisters::f9};
static constexpr FloatRegister fa0{FloatRegisters::f10};
static constexpr FloatRegister fa1{FloatRegisters::f11};
static constexpr FloatRegister fa2{FloatRegisters::f12};
static constexpr FloatRegister fa3{FloatRegisters::f13};
static constexpr FloatRegister fa4{FloatRegisters::f14};
static constexpr FloatRegister fa5{FloatRegisters::f15};
static constexpr FloatRegister fa6{FloatRegisters::f16};
static constexpr FloatRegister fa7{FloatRegisters::f17};
static constexpr FloatRegister fs2{FloatRegisters::f18};
static constexpr FloatRegister fs3{FloatRegisters::f19};
static constexpr FloatRegister fs4{FloatRegisters::f20};
static constexpr FloatRegister fs5{FloatRegisters::f21};
static constexpr FloatRegister fs6{FloatRegisters::f22};
static constexpr FloatRegister fs7{FloatRegisters::f23};
static constexpr FloatRegister fs8{FloatRegisters::f24};
static constexpr FloatRegister fs9{FloatRegisters::f25};
static constexpr FloatRegister fs10{FloatRegisters::f26};
static constexpr FloatRegister fs11{FloatRegisters::f27};
static constexpr FloatRegister ft8{FloatRegisters::f28};
static constexpr FloatRegister ft9{FloatRegisters::f29};
static constexpr FloatRegister ft10{FloatRegisters::f30};
static constexpr FloatRegister ft11{FloatRegisters::f31};

static constexpr Register StackPointer{Registers::sp};
static constexpr Register FramePointer{Registers::fp};
static constexpr Register ReturnReg{Registers::a0};
static constexpr Register ScratchRegister{Registers::s11};
static constexpr Register64 ReturnReg64(ReturnReg);

static constexpr FloatRegister ReturnFloat32Reg{FloatRegisters::fa0};
static constexpr FloatRegister ReturnDoubleReg{FloatRegisters::fa0};
#ifdef ENABLE_WASM_SIMD
static constexpr FloatRegister ReturnSimd128Reg{FloatRegisters::invalid_reg};
static constexpr FloatRegister ScratchSimd128Reg{FloatRegisters::invalid_reg};
#endif
static constexpr FloatRegister InvalidFloatReg{};

static constexpr FloatRegister ScratchFloat32Reg{FloatRegisters::ft10};
static constexpr FloatRegister ScratchDoubleReg{FloatRegisters::ft10};
static constexpr FloatRegister ScratchDoubleReg2{FloatRegisters::fs11};

static constexpr Register OsrFrameReg{Registers::a3};
static constexpr Register PreBarrierReg{Registers::a1};
static constexpr Register InterpreterPCReg{Registers::t0};
static constexpr Register CallTempReg0{Registers::t0};
static constexpr Register CallTempReg1{Registers::t1};
static constexpr Register CallTempReg2{Registers::t2};
static constexpr Register CallTempReg3{Registers::t3};
static constexpr Register CallTempReg4{Registers::a6};
static constexpr Register CallTempReg5{Registers::a7};
static constexpr Register InvalidReg{Registers::invalid_reg};
static constexpr Register CallTempNonArgRegs[] = {t0, t1, t2, t3};
static const uint32_t NumCallTempNonArgRegs = std::size(CallTempNonArgRegs);

static constexpr Register IntArgReg0{Registers::a0};
static constexpr Register IntArgReg1{Registers::a1};
static constexpr Register IntArgReg2{Registers::a2};
static constexpr Register IntArgReg3{Registers::a3};
static constexpr Register IntArgReg4{Registers::a4};
static constexpr Register IntArgReg5{Registers::a5};
static constexpr Register IntArgReg6{Registers::a6};
static constexpr Register IntArgReg7{Registers::a7};
static constexpr Register HeapReg{Registers::s7};

// Registers used by RegExpMatcher and RegExpExecMatch stubs (do not use
// JSReturnOperand).
static constexpr Register RegExpMatcherRegExpReg = CallTempReg0;
static constexpr Register RegExpMatcherStringReg = CallTempReg1;
static constexpr Register RegExpMatcherLastIndexReg = CallTempReg2;

// Registers used by RegExpExecTest stub (do not use ReturnReg).
static constexpr Register RegExpExecTestRegExpReg = CallTempReg0;
static constexpr Register RegExpExecTestStringReg = CallTempReg1;

// Registers used by RegExpSearcher stub (do not use ReturnReg).
static constexpr Register RegExpSearcherRegExpReg = CallTempReg0;
static constexpr Register RegExpSearcherStringReg = CallTempReg1;
static constexpr Register RegExpSearcherLastIndexReg = CallTempReg2;

static constexpr Register JSReturnReg_Type{Registers::a3};
static constexpr Register JSReturnReg_Data{Registers::s2};
static constexpr Register JSReturnReg{Registers::a2};
static constexpr ValueOperand JSReturnOperand = ValueOperand(JSReturnReg);

// These registers may be volatile or nonvolatile.
static constexpr Register ABINonArgReg0{Registers::t0};
static constexpr Register ABINonArgReg1{Registers::t1};
static constexpr Register ABINonArgReg2{Registers::t2};
static constexpr Register ABINonArgReg3{Registers::t3};

// These registers may be volatile or nonvolatile.
// Note: these three registers are all guaranteed to be different
static constexpr Register ABINonArgReturnReg0{Registers::t0};
static constexpr Register ABINonArgReturnReg1{Registers::t1};
static constexpr Register ABINonVolatileReg{Registers::s1};

// This register is guaranteed to be clobberable during the prologue and
// epilogue of an ABI call which must preserve both ABI argument, return
// and non-volatile registers.
static constexpr Register ABINonArgReturnVolatileReg{Registers::t0};

// This register may be volatile or nonvolatile.
// Avoid ft11 which is the scratch register.
static constexpr FloatRegister ABINonArgDoubleReg{FloatRegisters::ft11};

static constexpr Register WasmTableCallScratchReg0{ABINonArgReg0};
static constexpr Register WasmTableCallScratchReg1{ABINonArgReg1};
static constexpr Register WasmTableCallSigReg{ABINonArgReg2};
static constexpr Register WasmTableCallIndexReg{ABINonArgReg3};

// Instance pointer argument register for WebAssembly functions. This must not
// alias any other register used for passing function arguments or return
// values. Preserved by WebAssembly functions. Must be nonvolatile.
static constexpr Register InstanceReg{Registers::s4};

static constexpr Register WasmJitEntryReturnScratch{Registers::t1};

static constexpr Register WasmCallRefCallScratchReg0{ABINonArgReg0};
static constexpr Register WasmCallRefCallScratchReg1{ABINonArgReg1};
static constexpr Register WasmCallRefReg{ABINonArgReg3};

static constexpr Register WasmTailCallInstanceScratchReg{ABINonArgReg1};
static constexpr Register WasmTailCallRAScratchReg{ra};
static constexpr Register WasmTailCallFPScratchReg{ABINonArgReg3};

}  // namespace jit
}  // namespace js

#endif  // jit_riscv64_Register_riscv64_h
