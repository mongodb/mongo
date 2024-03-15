/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Bailouts.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/ScopeExit.h"

#include "gc/GC.h"
#include "jit/Assembler.h"  // jit::FramePointer
#include "jit/BaselineJIT.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JSJitFrameIter.h"
#include "jit/SafepointIndex.h"
#include "jit/ScriptFromCalleeToken.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/Stack.h"

#include "vm/JSScript-inl.h"
#include "vm/Probes-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::IsInRange;

#if defined(_WIN32)
#  pragma pack(push, 1)
#endif
class js::jit::BailoutStack {
  RegisterDump::FPUArray fpregs_;
  RegisterDump::GPRArray regs_;
  uintptr_t frameSize_;
  uintptr_t snapshotOffset_;

 public:
  MachineState machineState() {
    return MachineState::FromBailout(regs_, fpregs_);
  }
  uint32_t snapshotOffset() const { return snapshotOffset_; }
  uint32_t frameSize() const { return frameSize_; }
  uint8_t* parentStackPointer() {
    return (uint8_t*)this + sizeof(BailoutStack);
  }
};
#if defined(_WIN32)
#  pragma pack(pop)
#endif

// Make sure the compiler doesn't add extra padding on 32-bit platforms.
static_assert((sizeof(BailoutStack) % 8) == 0,
              "BailoutStack should be 8-byte aligned.");

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& activations,
                                   BailoutStack* bailout)
    : machine_(bailout->machineState()), activation_(nullptr) {
  uint8_t* sp = bailout->parentStackPointer();
  framePointer_ = sp + bailout->frameSize();
  MOZ_RELEASE_ASSERT(uintptr_t(framePointer_) == machine_.read(FramePointer));

  JSScript* script =
      ScriptFromCalleeToken(((JitFrameLayout*)framePointer_)->calleeToken());
  topIonScript_ = script->ionScript();

  attachOnJitActivation(activations);
  snapshotOffset_ = bailout->snapshotOffset();
}

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& activations,
                                   InvalidationBailoutStack* bailout)
    : machine_(bailout->machine()), activation_(nullptr) {
  framePointer_ = (uint8_t*)bailout->fp();
  MOZ_RELEASE_ASSERT(uintptr_t(framePointer_) == machine_.read(FramePointer));

  topIonScript_ = bailout->ionScript();
  attachOnJitActivation(activations);

  uint8_t* returnAddressToFp_ = bailout->osiPointReturnAddress();
  const OsiIndex* osiIndex = topIonScript_->getOsiIndex(returnAddressToFp_);
  snapshotOffset_ = osiIndex->snapshotOffset();
}

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& activations,
                                   const JSJitFrameIter& frame)
    : machine_(frame.machineState()) {
  framePointer_ = (uint8_t*)frame.fp();
  topIonScript_ = frame.ionScript();
  attachOnJitActivation(activations);

  const OsiIndex* osiIndex = frame.osiIndex();
  snapshotOffset_ = osiIndex->snapshotOffset();
}

// This address is a magic number made to cause crashes while indicating that we
// are making an attempt to mark the stack during a bailout.
static constexpr uint32_t FAKE_EXITFP_FOR_BAILOUT_ADDR = 0xba2;
static uint8_t* const FAKE_EXITFP_FOR_BAILOUT =
    reinterpret_cast<uint8_t*>(FAKE_EXITFP_FOR_BAILOUT_ADDR);

static_assert(!(FAKE_EXITFP_FOR_BAILOUT_ADDR & wasm::ExitFPTag),
              "FAKE_EXITFP_FOR_BAILOUT could be mistaken as a low-bit tagged "
              "wasm exit fp");

bool jit::Bailout(BailoutStack* sp, BaselineBailoutInfo** bailoutInfo) {
  JSContext* cx = TlsContext.get();
  MOZ_ASSERT(bailoutInfo);

  // We don't have an exit frame.
  MOZ_ASSERT(IsInRange(FAKE_EXITFP_FOR_BAILOUT, 0, 0x1000) &&
                 IsInRange(FAKE_EXITFP_FOR_BAILOUT + sizeof(CommonFrameLayout),
                           0, 0x1000),
             "Fake exitfp pointer should be within the first page.");

#ifdef DEBUG
  // Reset the counter when we bailed after MDebugEnterGCUnsafeRegion, but
  // before the matching MDebugLeaveGCUnsafeRegion.
  //
  // NOTE: EnterJit ensures the counter is zero when we enter JIT code.
  cx->resetInUnsafeRegion();
#endif

  cx->activation()->asJit()->setJSExitFP(FAKE_EXITFP_FOR_BAILOUT);

  JitActivationIterator jitActivations(cx);
  BailoutFrameInfo bailoutData(jitActivations, sp);
  JSJitFrameIter frame(jitActivations->asJit());
  MOZ_ASSERT(!frame.ionScript()->invalidated());
  JitFrameLayout* currentFramePtr = frame.jsFrame();

  JitSpew(JitSpew_IonBailouts, "Took bailout! Snapshot offset: %u",
          frame.snapshotOffset());

  MOZ_ASSERT(IsBaselineJitEnabled(cx));

  *bailoutInfo = nullptr;
  bool success =
      BailoutIonToBaseline(cx, bailoutData.activation(), frame, bailoutInfo,
                           /*exceptionInfo=*/nullptr, BailoutReason::Normal);
  MOZ_ASSERT_IF(success, *bailoutInfo != nullptr);

  if (!success) {
    MOZ_ASSERT(cx->isExceptionPending());
    JSScript* script = frame.script();
    probes::ExitScript(cx, script, script->function(),
                       /* popProfilerFrame = */ false);
  }

  // This condition was wrong when we entered this bailout function, but it
  // might be true now. A GC might have reclaimed all the Jit code and
  // invalidated all frames which are currently on the stack. As we are
  // already in a bailout, we could not switch to an invalidation
  // bailout. When the code of an IonScript which is on the stack is
  // invalidated (see InvalidateActivation), we remove references to it and
  // increment the reference counter for each activation that appear on the
  // stack. As the bailed frame is one of them, we have to decrement it now.
  if (frame.ionScript()->invalidated()) {
    frame.ionScript()->decrementInvalidationCount(cx->gcContext());
  }

  // NB: Commentary on how |lastProfilingFrame| is set from bailouts.
  //
  // Once we return to jitcode, any following frames might get clobbered,
  // but the current frame will not (as it will be clobbered "in-place"
  // with a baseline frame that will share the same frame prefix).
  // However, there may be multiple baseline frames unpacked from this
  // single Ion frame, which means we will need to once again reset
  // |lastProfilingFrame| to point to the correct unpacked last frame
  // in |FinishBailoutToBaseline|.
  //
  // In the case of error, the jitcode will jump immediately to an
  // exception handler, which will unwind the frames and properly set
  // the |lastProfilingFrame| to point to the frame being resumed into
  // (see |AutoResetLastProfilerFrameOnReturnFromException|).
  //
  // In both cases, we want to temporarily set the |lastProfilingFrame|
  // to the current frame being bailed out, and then fix it up later.
  if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(
          cx->runtime())) {
    cx->jitActivation->setLastProfilingFrame(currentFramePtr);
  }

  return success;
}

bool jit::InvalidationBailout(InvalidationBailoutStack* sp,
                              BaselineBailoutInfo** bailoutInfo) {
  sp->checkInvariants();

  JSContext* cx = TlsContext.get();

#ifdef DEBUG
  // Reset the counter when we bailed after MDebugEnterGCUnsafeRegion, but
  // before the matching MDebugLeaveGCUnsafeRegion.
  //
  // NOTE: EnterJit ensures the counter is zero when we enter JIT code.
  cx->resetInUnsafeRegion();
#endif

  // We don't have an exit frame.
  cx->activation()->asJit()->setJSExitFP(FAKE_EXITFP_FOR_BAILOUT);

  JitActivationIterator jitActivations(cx);
  BailoutFrameInfo bailoutData(jitActivations, sp);
  JSJitFrameIter frame(jitActivations->asJit());
  JitFrameLayout* currentFramePtr = frame.jsFrame();

  JitSpew(JitSpew_IonBailouts, "Took invalidation bailout! Snapshot offset: %u",
          frame.snapshotOffset());

  MOZ_ASSERT(IsBaselineJitEnabled(cx));

  *bailoutInfo = nullptr;
  bool success = BailoutIonToBaseline(cx, bailoutData.activation(), frame,
                                      bailoutInfo, /*exceptionInfo=*/nullptr,
                                      BailoutReason::Invalidate);
  MOZ_ASSERT_IF(success, *bailoutInfo != nullptr);

  if (!success) {
    MOZ_ASSERT(cx->isExceptionPending());

    // If the bailout failed, then bailout trampoline will pop the
    // current frame and jump straight to exception handling code when
    // this function returns.  Any Gecko Profiler entry pushed for this
    // frame will be silently forgotten.
    //
    // We call ExitScript here to ensure that if the ionScript had Gecko
    // Profiler instrumentation, then the entry for it is popped.
    //
    // However, if the bailout was during argument check, then a
    // pseudostack frame would not have been pushed in the first
    // place, so don't pop anything in that case.
    JSScript* script = frame.script();
    probes::ExitScript(cx, script, script->function(),
                       /* popProfilerFrame = */ false);

#ifdef JS_JITSPEW
    JitFrameLayout* layout = frame.jsFrame();
    JitSpew(JitSpew_IonInvalidate, "Bailout failed (Fatal Error)");
    JitSpew(JitSpew_IonInvalidate, "   calleeToken %p",
            (void*)layout->calleeToken());
    JitSpew(JitSpew_IonInvalidate, "   callerFramePtr %p",
            layout->callerFramePtr());
    JitSpew(JitSpew_IonInvalidate, "   ra %p", (void*)layout->returnAddress());
#endif
  }

  frame.ionScript()->decrementInvalidationCount(cx->gcContext());

  // Make the frame being bailed out the top profiled frame.
  if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(
          cx->runtime())) {
    cx->jitActivation->setLastProfilingFrame(currentFramePtr);
  }

  return success;
}

bool jit::ExceptionHandlerBailout(JSContext* cx,
                                  const InlineFrameIterator& frame,
                                  ResumeFromException* rfe,
                                  const ExceptionBailoutInfo& excInfo) {
  // If we are resuming in a finally block, the exception has already
  // been captured.
  // We can also be propagating debug mode exceptions without there being an
  // actual exception pending. For instance, when we return false from an
  // operation callback like a timeout handler.
  MOZ_ASSERT_IF(
      !cx->isExceptionPending(),
      excInfo.isFinally() || excInfo.propagatingIonExceptionForDebugMode());

  JS::AutoSaveExceptionState savedExc(cx);

  JitActivation* act = cx->activation()->asJit();
  uint8_t* prevExitFP = act->jsExitFP();
  auto restoreExitFP =
      mozilla::MakeScopeExit([&]() { act->setJSExitFP(prevExitFP); });
  act->setJSExitFP(FAKE_EXITFP_FOR_BAILOUT);

  gc::AutoSuppressGC suppress(cx);

  JitActivationIterator jitActivations(cx);
  BailoutFrameInfo bailoutData(jitActivations, frame.frame());
  JSJitFrameIter frameView(jitActivations->asJit());
  JitFrameLayout* currentFramePtr = frameView.jsFrame();

  BaselineBailoutInfo* bailoutInfo = nullptr;
  bool success = BailoutIonToBaseline(cx, bailoutData.activation(), frameView,
                                      &bailoutInfo, &excInfo,
                                      BailoutReason::ExceptionHandler);
  if (success) {
    MOZ_ASSERT(bailoutInfo);

    // Overwrite the kind so HandleException after the bailout returns
    // false, jumping directly to the exception tail.
    if (excInfo.propagatingIonExceptionForDebugMode()) {
      bailoutInfo->bailoutKind =
          mozilla::Some(BailoutKind::IonExceptionDebugMode);
    } else if (excInfo.isFinally()) {
      bailoutInfo->bailoutKind = mozilla::Some(BailoutKind::Finally);
    }

    rfe->kind = ExceptionResumeKind::Bailout;
    rfe->stackPointer = bailoutInfo->incomingStack;
    rfe->bailoutInfo = bailoutInfo;
  } else {
    // Drop the exception that triggered the bailout and instead propagate the
    // failure caused by processing the bailout (eg. OOM).
    savedExc.drop();
    MOZ_ASSERT(!bailoutInfo);
    MOZ_ASSERT(cx->isExceptionPending());
  }

  // Make the frame being bailed out the top profiled frame.
  if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(
          cx->runtime())) {
    cx->jitActivation->setLastProfilingFrame(currentFramePtr);
  }

  return success;
}

// Initialize the NamedLambdaObject and CallObject of the current frame if
// needed.
bool jit::EnsureHasEnvironmentObjects(JSContext* cx, AbstractFramePtr fp) {
  // Ion does not compile eval scripts.
  MOZ_ASSERT(!fp.isEvalFrame());

  if (fp.isFunctionFrame() && !fp.hasInitialEnvironment() &&
      fp.callee()->needsFunctionEnvironmentObjects()) {
    if (!fp.initFunctionEnvironmentObjects(cx)) {
      return false;
    }
  }

  return true;
}

void BailoutFrameInfo::attachOnJitActivation(
    const JitActivationIterator& jitActivations) {
  MOZ_ASSERT(jitActivations->asJit()->jsExitFP() == FAKE_EXITFP_FOR_BAILOUT);
  activation_ = jitActivations->asJit();
  activation_->setBailoutData(this);
}

BailoutFrameInfo::~BailoutFrameInfo() { activation_->cleanBailoutData(); }
