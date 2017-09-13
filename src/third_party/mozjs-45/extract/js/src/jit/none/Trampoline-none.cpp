/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscompartment.h"

#include "jit/Bailouts.h"
#include "jit/BaselineIC.h"
#include "jit/IonCaches.h"

using namespace js;
using namespace js::jit;

// This file includes stubs for generating the JIT trampolines when there is no
// JIT backend, and also includes implementations for assorted random things
// which can't be implemented in headers.

JitCode* JitRuntime::generateEnterJIT(JSContext*, EnterJitType) { MOZ_CRASH(); }
JitCode* JitRuntime::generateInvalidator(JSContext*) { MOZ_CRASH(); }
JitCode* JitRuntime::generateArgumentsRectifier(JSContext*, void**) { MOZ_CRASH(); }
JitCode* JitRuntime::generateBailoutTable(JSContext*, uint32_t) { MOZ_CRASH(); }
JitCode* JitRuntime::generateBailoutHandler(JSContext*) { MOZ_CRASH(); }
JitCode* JitRuntime::generateVMWrapper(JSContext*, const VMFunction&) { MOZ_CRASH(); }
JitCode* JitRuntime::generatePreBarrier(JSContext*, MIRType) { MOZ_CRASH(); }
JitCode* JitRuntime::generateDebugTrapHandler(JSContext*) { MOZ_CRASH(); }
JitCode* JitRuntime::generateExceptionTailStub(JSContext*, void*) { MOZ_CRASH(); }
JitCode* JitRuntime::generateBailoutTailStub(JSContext*) { MOZ_CRASH(); }

FrameSizeClass FrameSizeClass::FromDepth(uint32_t) { MOZ_CRASH(); }
FrameSizeClass FrameSizeClass::ClassLimit() { MOZ_CRASH(); }
uint32_t FrameSizeClass::frameSize() const { MOZ_CRASH(); }

const Register ABIArgGenerator::NonArgReturnReg0 = { Registers::invalid_reg };
const Register ABIArgGenerator::NonArgReturnReg1 = {  Registers::invalid_reg };
const Register ABIArgGenerator::NonArg_VolatileReg = {  Registers::invalid_reg };
const Register ABIArgGenerator::NonReturn_VolatileReg0 = {  Registers::invalid_reg };
const Register ABIArgGenerator::NonReturn_VolatileReg1 = {  Registers::invalid_reg };

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& iter, BailoutStack* bailout)
{
    MOZ_CRASH();
}

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& iter, InvalidationBailoutStack* bailout)
{
    MOZ_CRASH();
}

bool ICCompare_Int32::Compiler::generateStubCode(MacroAssembler&) { MOZ_CRASH(); }
bool ICCompare_Double::Compiler::generateStubCode(MacroAssembler&) { MOZ_CRASH(); }
bool ICBinaryArith_Int32::Compiler::generateStubCode(MacroAssembler&) { MOZ_CRASH(); }
bool ICUnaryArith_Int32::Compiler::generateStubCode(MacroAssembler&) { MOZ_CRASH(); }
JitCode* JitRuntime::generateProfilerExitFrameTailStub(JSContext*) { MOZ_CRASH(); }
