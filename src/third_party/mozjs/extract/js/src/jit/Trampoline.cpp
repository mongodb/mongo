/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <initializer_list>

#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/MacroAssembler.h"
#include "vm/JitActivation.h"
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

void JitRuntime::generateExceptionTailStub(MacroAssembler& masm,
                                           Label* profilerExitTail,
                                           Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateExceptionTailStub");

  exceptionTailOffset_ = startTrampolineCode(masm);

  uint32_t returnValueCheckOffset = 0;
  masm.bind(masm.failureLabel());
  masm.handleFailureWithHandlerTail(profilerExitTail, bailoutTail,
                                    &returnValueCheckOffset);

  exceptionTailReturnValueCheckOffset_ = returnValueCheckOffset;
}

void JitRuntime::generateProfilerExitFrameTailStub(MacroAssembler& masm,
                                                   Label* profilerExitTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateProfilerExitFrameTailStub");

  profilerExitFrameTailOffset_ = startTrampolineCode(masm);
  masm.bind(profilerExitTail);

  static constexpr size_t CallerFPOffset =
      CommonFrameLayout::offsetOfCallerFramePtr();

  // Assert the caller frame's type is one of the types we expect.
  auto emitAssertPrevFrameType = [&masm](
                                     Register framePtr, Register scratch,
                                     std::initializer_list<FrameType> types) {
#ifdef DEBUG
    masm.loadPtr(Address(framePtr, CommonFrameLayout::offsetOfDescriptor()),
                 scratch);
    masm.and32(Imm32(FRAMETYPE_MASK), scratch);

    Label checkOk;
    for (FrameType type : types) {
      masm.branch32(Assembler::Equal, scratch, Imm32(type), &checkOk);
    }
    masm.assumeUnreachable("Unexpected previous frame");
    masm.bind(&checkOk);
#else
    (void)masm;
#endif
  };

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(JSReturnOperand);
  Register scratch = regs.takeAny();

  // The code generated below expects that the current frame pointer points
  // to an Ion or Baseline frame, at the state it would be immediately before
  // the frame epilogue and ret(). Thus, after this stub's business is done, it
  // restores the frame pointer and stack pointer, then executes a ret() and
  // returns directly to the caller frame, on behalf of the callee script that
  // jumped to this code.
  //
  // Thus the expected state is:
  //
  //    [JitFrameLayout] <-- FramePointer
  //    [frame contents] <-- StackPointer
  //
  // The generated jitcode is responsible for overwriting the
  // jitActivation->lastProfilingFrame field with a pointer to the previous
  // Ion or Baseline jit-frame that was pushed before this one. It is also
  // responsible for overwriting jitActivation->lastProfilingCallSite with
  // the return address into that frame.
  //
  // So this jitcode is responsible for "walking up" the jit stack, finding
  // the previous Ion or Baseline JS frame, and storing its address and the
  // return address into the appropriate fields on the current jitActivation.
  //
  // There are a fixed number of different path types that can lead to the
  // current frame, which is either a Baseline or Ion frame:
  //
  // <Baseline-Or-Ion>
  // ^
  // |
  // ^--- Ion (or Baseline JSOp::Resume)
  // |
  // ^--- Baseline Stub <---- Baseline
  // |
  // ^--- IonICCall <---- Ion
  // |
  // ^--- Entry Frame (BaselineInterpreter) (unwrapped)
  // |
  // ^--- Arguments Rectifier (unwrapped)
  // |
  // ^--- Trampoline Native (unwrapped)
  // |
  // ^--- Entry Frame (CppToJSJit or WasmToJSJit)
  //
  // NOTE: Keep this in sync with JSJitProfilingFrameIterator::moveToNextFrame!

  Register actReg = regs.takeAny();
  masm.loadJSContext(actReg);
  masm.loadPtr(Address(actReg, offsetof(JSContext, profilingActivation_)),
               actReg);

  Address lastProfilingFrame(actReg,
                             JitActivation::offsetOfLastProfilingFrame());
  Address lastProfilingCallSite(actReg,
                                JitActivation::offsetOfLastProfilingCallSite());

#ifdef DEBUG
  // Ensure that frame we are exiting is current lastProfilingFrame
  {
    masm.loadPtr(lastProfilingFrame, scratch);
    Label checkOk;
    masm.branchPtr(Assembler::Equal, scratch, ImmWord(0), &checkOk);
    masm.branchPtr(Assembler::Equal, FramePointer, scratch, &checkOk);
    masm.assumeUnreachable(
        "Mismatch between stored lastProfilingFrame and current frame "
        "pointer.");
    masm.bind(&checkOk);
  }
#endif

  // Move FP into a scratch register and use that scratch register below, to
  // allow unwrapping rectifier frames without clobbering FP.
  Register fpScratch = regs.takeAny();
  masm.mov(FramePointer, fpScratch);

  Label again;
  masm.bind(&again);

  // Load the frame descriptor into |scratch|, figure out what to do depending
  // on its type.
  masm.loadPtr(Address(fpScratch, JitFrameLayout::offsetOfDescriptor()),
               scratch);
  masm.and32(Imm32(FRAMETYPE_MASK), scratch);

  // Handling of each case is dependent on FrameDescriptor.type
  Label handle_BaselineOrIonJS;
  Label handle_BaselineStub;
  Label handle_Rectifier;
  Label handle_TrampolineNative;
  Label handle_BaselineInterpreterEntry;
  Label handle_IonICCall;
  Label handle_Entry;

  // We check for IonJS and BaselineStub first because these are the most common
  // types. Calls from Baseline are usually from a BaselineStub frame.
  masm.branch32(Assembler::Equal, scratch, Imm32(FrameType::IonJS),
                &handle_BaselineOrIonJS);
  masm.branch32(Assembler::Equal, scratch, Imm32(FrameType::BaselineStub),
                &handle_BaselineStub);
  masm.branch32(Assembler::Equal, scratch, Imm32(FrameType::Rectifier),
                &handle_Rectifier);
  if (JitOptions.emitInterpreterEntryTrampoline) {
    masm.branch32(Assembler::Equal, scratch,
                  Imm32(FrameType::BaselineInterpreterEntry),
                  &handle_BaselineInterpreterEntry);
  }
  masm.branch32(Assembler::Equal, scratch, Imm32(FrameType::CppToJSJit),
                &handle_Entry);
  masm.branch32(Assembler::Equal, scratch, Imm32(FrameType::BaselineJS),
                &handle_BaselineOrIonJS);
  masm.branch32(Assembler::Equal, scratch, Imm32(FrameType::IonICCall),
                &handle_IonICCall);
  masm.branch32(Assembler::Equal, scratch, Imm32(FrameType::TrampolineNative),
                &handle_TrampolineNative);
  masm.branch32(Assembler::Equal, scratch, Imm32(FrameType::WasmToJSJit),
                &handle_Entry);

  masm.assumeUnreachable(
      "Invalid caller frame type when returning from a JIT frame.");

  masm.bind(&handle_BaselineOrIonJS);
  {
    // Returning directly to a Baseline or Ion frame.

    // lastProfilingCallSite := ReturnAddress
    masm.loadPtr(Address(fpScratch, JitFrameLayout::offsetOfReturnAddress()),
                 scratch);
    masm.storePtr(scratch, lastProfilingCallSite);

    // lastProfilingFrame := CallerFrame
    masm.loadPtr(Address(fpScratch, CallerFPOffset), scratch);
    masm.storePtr(scratch, lastProfilingFrame);

    masm.moveToStackPtr(FramePointer);
    masm.pop(FramePointer);
    masm.ret();
  }

  // Shared implementation for BaselineStub and IonICCall frames.
  auto emitHandleStubFrame = [&](FrameType expectedPrevType) {
    // Load pointer to stub frame and assert type of its caller frame.
    masm.loadPtr(Address(fpScratch, CallerFPOffset), fpScratch);
    emitAssertPrevFrameType(fpScratch, scratch, {expectedPrevType});

    // lastProfilingCallSite := StubFrame.ReturnAddress
    masm.loadPtr(Address(fpScratch, CommonFrameLayout::offsetOfReturnAddress()),
                 scratch);
    masm.storePtr(scratch, lastProfilingCallSite);

    // lastProfilingFrame := StubFrame.CallerFrame
    masm.loadPtr(Address(fpScratch, CallerFPOffset), scratch);
    masm.storePtr(scratch, lastProfilingFrame);

    masm.moveToStackPtr(FramePointer);
    masm.pop(FramePointer);
    masm.ret();
  };

  masm.bind(&handle_BaselineStub);
  {
    // BaselineJS => BaselineStub frame.
    emitHandleStubFrame(FrameType::BaselineJS);
  }

  masm.bind(&handle_IonICCall);
  {
    // IonJS => IonICCall frame.
    emitHandleStubFrame(FrameType::IonJS);
  }

  masm.bind(&handle_Rectifier);
  {
    // There can be multiple previous frame types so just "unwrap" the arguments
    // rectifier frame and try again.
    masm.loadPtr(Address(fpScratch, CallerFPOffset), fpScratch);
    emitAssertPrevFrameType(
        fpScratch, scratch,
        {FrameType::IonJS, FrameType::BaselineStub, FrameType::TrampolineNative,
         FrameType::CppToJSJit, FrameType::WasmToJSJit});
    masm.jump(&again);
  }

  masm.bind(&handle_TrampolineNative);
  {
    // Unwrap this frame, similar to arguments rectifier frames.
    masm.loadPtr(Address(fpScratch, CallerFPOffset), fpScratch);
    emitAssertPrevFrameType(
        fpScratch, scratch,
        {FrameType::IonJS, FrameType::BaselineStub, FrameType::Rectifier,
         FrameType::CppToJSJit, FrameType::WasmToJSJit});
    masm.jump(&again);
  }

  if (JitOptions.emitInterpreterEntryTrampoline) {
    masm.bind(&handle_BaselineInterpreterEntry);
    {
      // Unwrap the baseline interpreter entry frame and try again.
      masm.loadPtr(Address(fpScratch, CallerFPOffset), fpScratch);
      emitAssertPrevFrameType(
          fpScratch, scratch,
          {FrameType::IonJS, FrameType::BaselineJS, FrameType::BaselineStub,
           FrameType::CppToJSJit, FrameType::WasmToJSJit, FrameType::IonICCall,
           FrameType::Rectifier});
      masm.jump(&again);
    }
  }

  masm.bind(&handle_Entry);
  {
    // FrameType::CppToJSJit / FrameType::WasmToJSJit
    //
    // A fast-path wasm->jit transition frame is an entry frame from the point
    // of view of the JIT.
    // Store null into both fields.
    masm.movePtr(ImmPtr(nullptr), scratch);
    masm.storePtr(scratch, lastProfilingCallSite);
    masm.storePtr(scratch, lastProfilingFrame);

    masm.moveToStackPtr(FramePointer);
    masm.pop(FramePointer);
    masm.ret();
  }
}
