/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Bailouts.h"
#include "jit/BaselineIC.h"
#include "vm/JSCompartment.h"

using namespace js;
using namespace js::jit;

// This file includes stubs for generating the JIT trampolines when there is no
// JIT backend, and also includes implementations for assorted random things
// which can't be implemented in headers.

void JitRuntime::generateEnterJIT(JSContext*, MacroAssembler&) { MOZ_CRASH(); }
void JitRuntime::generateInvalidator(MacroAssembler&, Label*) { MOZ_CRASH(); }
void JitRuntime::generateArgumentsRectifier(MacroAssembler&) { MOZ_CRASH(); }
JitRuntime::BailoutTable JitRuntime::generateBailoutTable(MacroAssembler&, Label*, uint32_t) { MOZ_CRASH(); }
void JitRuntime::generateBailoutHandler(MacroAssembler&, Label*) { MOZ_CRASH(); }
uint32_t JitRuntime::generatePreBarrier(JSContext*, MacroAssembler&, MIRType) { MOZ_CRASH(); }
JitCode* JitRuntime::generateDebugTrapHandler(JSContext*) { MOZ_CRASH(); }
void JitRuntime::generateExceptionTailStub(MacroAssembler&, void*, Label*) { MOZ_CRASH(); }
void JitRuntime::generateBailoutTailStub(MacroAssembler&, Label*) { MOZ_CRASH(); }
void JitRuntime::generateProfilerExitFrameTailStub(MacroAssembler&, Label*) { MOZ_CRASH(); }

bool JitRuntime::generateVMWrapper(JSContext*, MacroAssembler&, const VMFunction&) { MOZ_CRASH(); }

FrameSizeClass FrameSizeClass::FromDepth(uint32_t) { MOZ_CRASH(); }
FrameSizeClass FrameSizeClass::ClassLimit() { MOZ_CRASH(); }
uint32_t FrameSizeClass::frameSize() const { MOZ_CRASH(); }

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
