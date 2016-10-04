/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/shared/BaselineCompiler-shared.h"

#include "jit/BaselineIC.h"
#include "jit/VMFunctions.h"

#include "jsscriptinlines.h"
#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

BaselineCompilerShared::BaselineCompilerShared(JSContext* cx, TempAllocator& alloc, JSScript* script)
  : cx(cx),
    script(script),
    pc(script->code()),
    ionCompileable_(jit::IsIonEnabled(cx) && CanIonCompileScript(cx, script, false)),
    ionOSRCompileable_(jit::IsIonEnabled(cx) && CanIonCompileScript(cx, script, true)),
    compileDebugInstrumentation_(script->isDebuggee()),
    alloc_(alloc),
    analysis_(alloc, script),
    frame(script, masm),
    stubSpace_(),
    icEntries_(),
    pcMappingEntries_(),
    icLoadLabels_(),
    pushedBeforeCall_(0),
    inCall_(false),
    spsPushToggleOffset_(),
    profilerEnterFrameToggleOffset_(),
    profilerExitFrameToggleOffset_(),
    traceLoggerEnterToggleOffset_(),
    traceLoggerExitToggleOffset_(),
    traceLoggerScriptTextIdOffset_()
{ }

void
BaselineCompilerShared::prepareVMCall()
{
    pushedBeforeCall_ = masm.framePushed();
    inCall_ = true;

    // Ensure everything is synced.
    frame.syncStack(0);

    // Save the frame pointer.
    masm.Push(BaselineFrameReg);
}

bool
BaselineCompilerShared::callVM(const VMFunction& fun, CallVMPhase phase)
{
    JitCode* code = cx->runtime()->jitRuntime()->getVMWrapper(fun);
    if (!code)
        return false;

#ifdef DEBUG
    // Assert prepareVMCall() has been called.
    MOZ_ASSERT(inCall_);
    inCall_ = false;
#endif

#ifdef DEBUG
    // Assert the frame does not have an override pc when we're executing JIT code.
    {
        Label ok;
        masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                          Imm32(BaselineFrame::HAS_OVERRIDE_PC), &ok);
        masm.assumeUnreachable("BaselineFrame shouldn't override pc when executing JIT code");
        masm.bind(&ok);
    }
#endif

    // Compute argument size. Note that this include the size of the frame pointer
    // pushed by prepareVMCall.
    uint32_t argSize = fun.explicitStackSlots() * sizeof(void*) + sizeof(void*);

    // Assert all arguments were pushed.
    MOZ_ASSERT(masm.framePushed() - pushedBeforeCall_ == argSize);

    Address frameSizeAddress(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize());
    uint32_t frameVals = frame.nlocals() + frame.stackDepth();
    uint32_t frameBaseSize = BaselineFrame::FramePointerOffset + BaselineFrame::Size();
    uint32_t frameFullSize = frameBaseSize + (frameVals * sizeof(Value));
    if (phase == POST_INITIALIZE) {
        masm.store32(Imm32(frameFullSize), frameSizeAddress);
        uint32_t descriptor = MakeFrameDescriptor(frameFullSize + argSize, JitFrame_BaselineJS);
        masm.push(Imm32(descriptor));

    } else if (phase == PRE_INITIALIZE) {
        masm.store32(Imm32(frameBaseSize), frameSizeAddress);
        uint32_t descriptor = MakeFrameDescriptor(frameBaseSize + argSize, JitFrame_BaselineJS);
        masm.push(Imm32(descriptor));

    } else {
        MOZ_ASSERT(phase == CHECK_OVER_RECURSED);
        Label afterWrite;
        Label writePostInitialize;

        // If OVER_RECURSED is set, then frame locals haven't been pushed yet.
        masm.branchTest32(Assembler::Zero,
                          frame.addressOfFlags(),
                          Imm32(BaselineFrame::OVER_RECURSED),
                          &writePostInitialize);

        masm.move32(Imm32(frameBaseSize), ICTailCallReg);
        masm.jump(&afterWrite);

        masm.bind(&writePostInitialize);
        masm.move32(Imm32(frameFullSize), ICTailCallReg);

        masm.bind(&afterWrite);
        masm.store32(ICTailCallReg, frameSizeAddress);
        masm.add32(Imm32(argSize), ICTailCallReg);
        masm.makeFrameDescriptor(ICTailCallReg, JitFrame_BaselineJS);
        masm.push(ICTailCallReg);
    }
    MOZ_ASSERT(fun.expectTailCall == NonTailCall);
    // Perform the call.
    masm.call(code);
    uint32_t callOffset = masm.currentOffset();
    masm.pop(BaselineFrameReg);

#ifdef DEBUG
    // Assert the frame does not have an override pc when we're executing JIT code.
    {
        Label ok;
        masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                          Imm32(BaselineFrame::HAS_OVERRIDE_PC), &ok);
        masm.assumeUnreachable("BaselineFrame shouldn't override pc after VM call");
        masm.bind(&ok);
    }
#endif

    // Add a fake ICEntry (without stubs), so that the return offset to
    // pc mapping works.
    return appendICEntry(ICEntry::Kind_CallVM, callOffset);
}
