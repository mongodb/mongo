/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JSJitFrameIter-inl.h"

#include "jit/CalleeToken.h"
#include "jit/IonScript.h"
#include "jit/JitcodeMap.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "jit/MacroAssembler.h"  // js::jit::Assembler::GetPointer
#include "jit/SafepointIndex.h"
#include "jit/Safepoints.h"
#include "jit/ScriptFromCalleeToken.h"
#include "jit/VMFunctions.h"
#include "js/friend/DumpFunctions.h"  // js::DumpObject, js::DumpValue
#include "vm/JitActivation.h"

#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

JSJitFrameIter::JSJitFrameIter(const JitActivation* activation)
    : current_(activation->jsExitFP()),
      type_(FrameType::Exit),
      activation_(activation) {
  // If we're currently performing a bailout, we have to use the activation's
  // bailout data when we start iterating over the activation's frames.
  if (activation_->bailoutData()) {
    current_ = activation_->bailoutData()->fp();
    type_ = FrameType::Bailout;
  }
  MOZ_ASSERT(!TlsContext.get()->inUnsafeCallWithABI);
}

JSJitFrameIter::JSJitFrameIter(const JitActivation* activation,
                               FrameType frameType, uint8_t* fp)
    : current_(fp), type_(frameType), activation_(activation) {
  // This constructor is only used when resuming iteration after iterating Wasm
  // frames in the same JitActivation so ignore activation_->bailoutData().
  //
  // Note: FrameType::JSJitToWasm is used for JIT => Wasm calls through the Wasm
  // JIT entry trampoline. FrameType::Exit is used for direct Ion => Wasm calls.
  MOZ_ASSERT(fp > activation->jsOrWasmExitFP());
  MOZ_ASSERT(type_ == FrameType::JSJitToWasm || type_ == FrameType::Exit);
  MOZ_ASSERT(!TlsContext.get()->inUnsafeCallWithABI);
}

bool JSJitFrameIter::checkInvalidation() const {
  IonScript* dummy;
  return checkInvalidation(&dummy);
}

bool JSJitFrameIter::checkInvalidation(IonScript** ionScriptOut) const {
  JSScript* script = this->script();
  if (isBailoutJS()) {
    *ionScriptOut = activation_->bailoutData()->ionScript();
    return !script->hasIonScript() || script->ionScript() != *ionScriptOut;
  }

  uint8_t* returnAddr = resumePCinCurrentFrame();
  // N.B. the current IonScript is not the same as the frame's
  // IonScript if the frame has since been invalidated.
  bool invalidated = !script->hasIonScript() ||
                     !script->ionScript()->containsReturnAddress(returnAddr);
  if (!invalidated) {
    return false;
  }

  int32_t invalidationDataOffset = ((int32_t*)returnAddr)[-1];
  uint8_t* ionScriptDataOffset = returnAddr + invalidationDataOffset;
  IonScript* ionScript = (IonScript*)Assembler::GetPointer(ionScriptDataOffset);
  MOZ_ASSERT(ionScript->containsReturnAddress(returnAddr));
  *ionScriptOut = ionScript;
  return true;
}

CalleeToken JSJitFrameIter::calleeToken() const {
  return ((JitFrameLayout*)current_)->calleeToken();
}

JSFunction* JSJitFrameIter::callee() const {
  MOZ_ASSERT(isScripted() || isTrampolineNative());
  MOZ_ASSERT(isFunctionFrame());
  return CalleeTokenToFunction(calleeToken());
}

JSFunction* JSJitFrameIter::maybeCallee() const {
  if (isScripted() && isFunctionFrame()) {
    return callee();
  }
  return nullptr;
}

bool JSJitFrameIter::isBareExit() const {
  if (type_ != FrameType::Exit) {
    return false;
  }
  return exitFrame()->isBareExit();
}

bool JSJitFrameIter::isUnwoundJitExit() const {
  if (type_ != FrameType::Exit) {
    return false;
  }
  return exitFrame()->isUnwoundJitExit();
}

bool JSJitFrameIter::isFunctionFrame() const {
  return CalleeTokenIsFunction(calleeToken());
}

JSScript* JSJitFrameIter::script() const {
  MOZ_ASSERT(isScripted());
  JSScript* script = MaybeForwardedScriptFromCalleeToken(calleeToken());
  MOZ_ASSERT(script);
  return script;
}

JSScript* JSJitFrameIter::maybeForwardedScript() const {
  MOZ_ASSERT(isScripted());
  if (isBaselineJS()) {
    return MaybeForwardedScriptFromCalleeToken(baselineFrame()->calleeToken());
  }
  JSScript* script = MaybeForwardedScriptFromCalleeToken(calleeToken());
  MOZ_ASSERT(script);
  return script;
}

void JSJitFrameIter::baselineScriptAndPc(JSScript** scriptRes,
                                         jsbytecode** pcRes) const {
  MOZ_ASSERT(isBaselineJS());
  JSScript* script = this->script();
  if (scriptRes) {
    *scriptRes = script;
  }

  MOZ_ASSERT(pcRes);

  // The Baseline Interpreter stores the bytecode pc in the frame.
  if (baselineFrame()->runningInInterpreter()) {
    MOZ_ASSERT(baselineFrame()->interpreterScript() == script);
    *pcRes = baselineFrame()->interpreterPC();
    return;
  }

  // There must be a BaselineScript with a RetAddrEntry for the current return
  // address.
  uint8_t* retAddr = resumePCinCurrentFrame();
  const RetAddrEntry& entry =
      script->baselineScript()->retAddrEntryFromReturnAddress(retAddr);
  *pcRes = entry.pc(script);
}

Value* JSJitFrameIter::actualArgs() const { return jsFrame()->actualArgs(); }

uint8_t* JSJitFrameIter::prevFp() const { return current()->callerFramePtr(); }

// Compute the size of a Baseline frame excluding pushed VMFunction arguments or
// callee frame headers. This is used to calculate the number of Value slots in
// the frame. The caller asserts this matches BaselineFrame::debugFrameSize.
static uint32_t ComputeBaselineFrameSize(const JSJitFrameIter& frame) {
  MOZ_ASSERT(frame.prevType() == FrameType::BaselineJS);

  uint32_t frameSize = frame.current()->callerFramePtr() - frame.fp();

  if (frame.isBaselineStub()) {
    return frameSize - BaselineStubFrameLayout::Size();
  }

  // Note: an UnwoundJit exit frame is a JitFrameLayout that was turned into an
  // ExitFrameLayout by EnsureUnwoundJitExitFrame. We have to use the original
  // header size here because that's what we have on the stack.
  if (frame.isScripted() || frame.isUnwoundJitExit()) {
    return frameSize - JitFrameLayout::Size();
  }

  if (frame.isExitFrame()) {
    frameSize -= ExitFrameLayout::Size();
    if (frame.exitFrame()->isWrapperExit()) {
      VMFunctionId id = frame.exitFrame()->footer()->functionId();
      const VMFunctionData& data = GetVMFunction(id);
      frameSize -= data.explicitStackSlots() * sizeof(void*);
    }
    return frameSize;
  }

  MOZ_CRASH("Unexpected frame");
}

void JSJitFrameIter::operator++() {
  MOZ_ASSERT(!isEntry());

  // Compute BaselineFrame size. In debug builds this is equivalent to
  // BaselineFrame::debugFrameSize_. This is asserted at the end of this method.
  if (current()->prevType() == FrameType::BaselineJS) {
    uint32_t frameSize = ComputeBaselineFrameSize(*this);
    baselineFrameSize_ = mozilla::Some(frameSize);
  } else {
    baselineFrameSize_ = mozilla::Nothing();
  }

  cachedSafepointIndex_ = nullptr;

  // If the next frame is the entry frame, just exit. Don't update current_,
  // since the entry and first frames overlap.
  if (isEntry(current()->prevType())) {
    type_ = current()->prevType();
    return;
  }

  type_ = current()->prevType();
  resumePCinCurrentFrame_ = current()->returnAddress();
  current_ = prevFp();

  MOZ_ASSERT_IF(isBaselineJS(),
                baselineFrame()->debugFrameSize() == *baselineFrameSize_);
}

uintptr_t* JSJitFrameIter::spillBase() const {
  MOZ_ASSERT(isIonJS());

  // Get the base address to where safepoint registers are spilled.
  // Out-of-line calls do not unwind the extra padding space used to
  // aggregate bailout tables, so we use frameSize instead of frameLocals,
  // which would only account for local stack slots.
  return reinterpret_cast<uintptr_t*>(fp() - ionScript()->frameSize());
}

MachineState JSJitFrameIter::machineState() const {
  MOZ_ASSERT(isIonScripted());

  // The MachineState is used by GCs for tracing call-sites.
  if (MOZ_UNLIKELY(isBailoutJS())) {
    return *activation_->bailoutData()->machineState();
  }

  SafepointReader reader(ionScript(), safepoint());

  FloatRegisterSet fregs = reader.allFloatSpills().set().reduceSetForPush();
  GeneralRegisterSet regs = reader.allGprSpills().set();

  uintptr_t* spill = spillBase();
  uint8_t* spillAlign =
      alignDoubleSpill(reinterpret_cast<uint8_t*>(spill - regs.size()));
  char* floatSpill = reinterpret_cast<char*>(spillAlign);

  return MachineState::FromSafepoint(fregs, regs, floatSpill, spill);
}

JitFrameLayout* JSJitFrameIter::jsFrame() const {
  MOZ_ASSERT(isScripted());
  if (isBailoutJS()) {
    return (JitFrameLayout*)activation_->bailoutData()->fp();
  }

  return (JitFrameLayout*)fp();
}

IonScript* JSJitFrameIter::ionScript() const {
  MOZ_ASSERT(isIonScripted());
  if (isBailoutJS()) {
    return activation_->bailoutData()->ionScript();
  }

  IonScript* ionScript = nullptr;
  if (checkInvalidation(&ionScript)) {
    return ionScript;
  }
  return ionScriptFromCalleeToken();
}

IonScript* JSJitFrameIter::ionScriptFromCalleeToken() const {
  MOZ_ASSERT(isIonJS());
  MOZ_ASSERT(!checkInvalidation());
  return script()->ionScript();
}

const SafepointIndex* JSJitFrameIter::safepoint() const {
  MOZ_ASSERT(isIonJS());
  if (!cachedSafepointIndex_) {
    cachedSafepointIndex_ =
        ionScript()->getSafepointIndex(resumePCinCurrentFrame());
  }
  return cachedSafepointIndex_;
}

SnapshotOffset JSJitFrameIter::snapshotOffset() const {
  MOZ_ASSERT(isIonScripted());
  if (isBailoutJS()) {
    return activation_->bailoutData()->snapshotOffset();
  }
  return osiIndex()->snapshotOffset();
}

const OsiIndex* JSJitFrameIter::osiIndex() const {
  MOZ_ASSERT(isIonJS());
  SafepointReader reader(ionScript(), safepoint());
  return ionScript()->getOsiIndex(reader.osiReturnPointOffset());
}

bool JSJitFrameIter::isConstructing() const {
  return CalleeTokenIsConstructing(calleeToken());
}

unsigned JSJitFrameIter::numActualArgs() const {
  if (isScripted()) {
    return jsFrame()->numActualArgs();
  }

  MOZ_ASSERT(isExitFrameLayout<NativeExitFrameLayout>());
  return exitFrame()->as<NativeExitFrameLayout>()->argc();
}

void JSJitFrameIter::dumpBaseline() const {
  MOZ_ASSERT(isBaselineJS());

  fprintf(stderr, " JS Baseline frame\n");
  if (isFunctionFrame()) {
    fprintf(stderr, "  callee fun: ");
#if defined(DEBUG) || defined(JS_JITSPEW)
    DumpObject(callee());
#else
    fprintf(stderr, "?\n");
#endif
  } else {
    fprintf(stderr, "  global frame, no callee\n");
  }

  fprintf(stderr, "  file %s line %u\n", script()->filename(),
          script()->lineno());

  JSContext* cx = TlsContext.get();
  RootedScript script(cx);
  jsbytecode* pc;
  baselineScriptAndPc(script.address(), &pc);

  fprintf(stderr, "  script = %p, pc = %p (offset %u)\n", (void*)script, pc,
          uint32_t(script->pcToOffset(pc)));
  fprintf(stderr, "  current op: %s\n", CodeName(JSOp(*pc)));

  fprintf(stderr, "  actual args: %u\n", numActualArgs());

  for (unsigned i = 0; i < baselineFrameNumValueSlots(); i++) {
    fprintf(stderr, "  slot %u: ", i);
#if defined(DEBUG) || defined(JS_JITSPEW)
    Value* v = baselineFrame()->valueSlot(i);
    DumpValue(*v);
#else
    fprintf(stderr, "?\n");
#endif
  }
}

void JSJitFrameIter::dump() const {
  switch (type_) {
    case FrameType::CppToJSJit:
      fprintf(stderr, " Entry frame\n");
      fprintf(stderr, "  Caller frame ptr: %p\n", current()->callerFramePtr());
      break;
    case FrameType::BaselineJS:
      dumpBaseline();
      break;
    case FrameType::BaselineStub:
      fprintf(stderr, " Baseline stub frame\n");
      fprintf(stderr, "  Caller frame ptr: %p\n", current()->callerFramePtr());
      break;
    case FrameType::Bailout:
    case FrameType::IonJS: {
      InlineFrameIterator frames(TlsContext.get(), this);
      for (;;) {
        frames.dump();
        if (!frames.more()) {
          break;
        }
        ++frames;
      }
      break;
    }
    case FrameType::BaselineInterpreterEntry:
      fprintf(stderr, " Baseline Interpreter Entry frame\n");
      fprintf(stderr, "  Caller frame ptr: %p\n", current()->callerFramePtr());
      break;
    case FrameType::Rectifier:
      fprintf(stderr, " Rectifier frame\n");
      fprintf(stderr, "  Caller frame ptr: %p\n", current()->callerFramePtr());
      break;
    case FrameType::TrampolineNative:
      fprintf(stderr, " TrampolineNative frame\n");
      fprintf(stderr, "  Caller frame ptr: %p\n", current()->callerFramePtr());
      break;
    case FrameType::IonICCall:
      fprintf(stderr, " Ion IC call\n");
      fprintf(stderr, "  Caller frame ptr: %p\n", current()->callerFramePtr());
      break;
    case FrameType::WasmToJSJit:
      fprintf(stderr, " Fast wasm-to-JS entry frame\n");
      fprintf(stderr, "  Caller frame ptr: %p\n", current()->callerFramePtr());
      break;
    case FrameType::Exit:
      fprintf(stderr, " Exit frame\n");
      break;
    case FrameType::JSJitToWasm:
      fprintf(stderr, " Wasm exit frame\n");
      break;
  };
  fputc('\n', stderr);
}

#ifdef DEBUG
bool JSJitFrameIter::verifyReturnAddressUsingNativeToBytecodeMap() {
  MOZ_ASSERT(resumePCinCurrentFrame_ != nullptr);

  // Only handle Ion frames for now.
  if (type_ != FrameType::IonJS && type_ != FrameType::BaselineJS) {
    return true;
  }

  JSRuntime* rt = TlsContext.get()->runtime();

  // Don't verify while off thread.
  if (!CurrentThreadCanAccessRuntime(rt)) {
    return true;
  }

  // Don't verify if sampling is being suppressed.
  if (!TlsContext.get()->isProfilerSamplingEnabled()) {
    return true;
  }

  if (JS::RuntimeHeapIsMinorCollecting()) {
    return true;
  }

  JitRuntime* jitrt = rt->jitRuntime();

  // Look up and print bytecode info for the native address.
  const JitcodeGlobalEntry* entry =
      jitrt->getJitcodeGlobalTable()->lookup(resumePCinCurrentFrame_);
  if (!entry) {
    return true;
  }

  JitSpew(JitSpew_Profiling, "Found nativeToBytecode entry for %p: %p - %p",
          resumePCinCurrentFrame_, entry->nativeStartAddr(),
          entry->nativeEndAddr());

  BytecodeLocationVector location;
  uint32_t depth = UINT32_MAX;
  if (!entry->callStackAtAddr(rt, resumePCinCurrentFrame_, location, &depth)) {
    return false;
  }
  MOZ_ASSERT(depth > 0 && depth != UINT32_MAX);
  MOZ_ASSERT(location.length() == depth);

  JitSpew(JitSpew_Profiling, "Found bytecode location of depth %u:", depth);
  for (size_t i = 0; i < location.length(); i++) {
    JitSpew(JitSpew_Profiling, "   %s:%u - %zu",
            location[i].getDebugOnlyScript()->filename(),
            location[i].getDebugOnlyScript()->lineno(),
            size_t(location[i].toRawBytecode() -
                   location[i].getDebugOnlyScript()->code()));
  }

  if (type_ == FrameType::IonJS) {
    // Create an InlineFrameIterator here and verify the mapped info against the
    // iterator info.
    InlineFrameIterator inlineFrames(TlsContext.get(), this);
    for (size_t idx = 0; idx < location.length(); idx++) {
      MOZ_ASSERT(idx < location.length());
      MOZ_ASSERT_IF(idx < location.length() - 1, inlineFrames.more());

      JitSpew(JitSpew_Profiling, "Match %d: ION %s:%u(%zu) vs N2B %s:%u(%zu)",
              (int)idx, inlineFrames.script()->filename(),
              inlineFrames.script()->lineno(),
              size_t(inlineFrames.pc() - inlineFrames.script()->code()),
              location[idx].getDebugOnlyScript()->filename(),
              location[idx].getDebugOnlyScript()->lineno(),
              size_t(location[idx].toRawBytecode() -
                     location[idx].getDebugOnlyScript()->code()));

      MOZ_ASSERT(inlineFrames.script() == location[idx].getDebugOnlyScript());

      if (inlineFrames.more()) {
        ++inlineFrames;
      }
    }
  }

  return true;
}
#endif  // DEBUG

JSJitProfilingFrameIterator::JSJitProfilingFrameIterator(JSContext* cx,
                                                         void* pc, void* sp) {
  // If no profilingActivation is live, initialize directly to
  // end-of-iteration state.
  if (!cx->profilingActivation()) {
    type_ = FrameType::CppToJSJit;
    fp_ = nullptr;
    resumePCinCurrentFrame_ = nullptr;
    return;
  }

  MOZ_ASSERT(cx->profilingActivation()->isJit());

  JitActivation* act = cx->profilingActivation()->asJit();

  // If the top JitActivation has a null lastProfilingFrame, assume that
  // it's a trivially empty activation, and initialize directly
  // to end-of-iteration state.
  if (!act->lastProfilingFrame()) {
    type_ = FrameType::CppToJSJit;
    fp_ = nullptr;
    resumePCinCurrentFrame_ = nullptr;
    return;
  }

  // Get the fp from the current profilingActivation
  fp_ = (uint8_t*)act->lastProfilingFrame();

  // Use fp_ as endStackAddress_. For cases below where we know we're currently
  // executing JIT code, we use the current stack pointer instead.
  endStackAddress_ = fp_;

  // Profiler sampling must NOT be suppressed if we are here.
  MOZ_ASSERT(cx->isProfilerSamplingEnabled());

  // Try initializing with sampler pc
  if (tryInitWithPC(pc)) {
    endStackAddress_ = sp;
    return;
  }

  if (!IsPortableBaselineInterpreterEnabled()) {
    // Try initializing with sampler pc using native=>bytecode table.
    JitcodeGlobalTable* table =
        cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (tryInitWithTable(table, pc, /* forLastCallSite = */ false)) {
      endStackAddress_ = sp;
      return;
    }

    // Try initializing with lastProfilingCallSite pc
    void* lastCallSite = act->lastProfilingCallSite();
    if (lastCallSite) {
      if (tryInitWithPC(lastCallSite)) {
        return;
      }

      // Try initializing with lastProfilingCallSite pc using native=>bytecode
      // table.
      if (tryInitWithTable(table, lastCallSite, /* forLastCallSite = */ true)) {
        return;
      }
    }
  }

  // If nothing matches, for now just assume we are at the start of the last
  // frame's baseline jit code or interpreter code.
  type_ = FrameType::BaselineJS;
  if (frameScript()->hasBaselineScript()) {
    resumePCinCurrentFrame_ = frameScript()->baselineScript()->method()->raw();
  } else if (!IsPortableBaselineInterpreterEnabled()) {
    MOZ_ASSERT(IsBaselineInterpreterEnabled());
    resumePCinCurrentFrame_ =
        cx->runtime()->jitRuntime()->baselineInterpreter().codeRaw();
  } else {
    resumePCinCurrentFrame_ = nullptr;
  }
}

template <typename ReturnType = CommonFrameLayout*>
static inline ReturnType GetPreviousRawFrame(CommonFrameLayout* frame) {
  return ReturnType(frame->callerFramePtr());
}

JSJitProfilingFrameIterator::JSJitProfilingFrameIterator(
    CommonFrameLayout* fp) {
  endStackAddress_ = fp;
  moveToNextFrame(fp);
}

bool JSJitProfilingFrameIterator::tryInitWithPC(void* pc) {
  JSScript* callee = frameScript();

  // Check for Ion first, since it's more likely for hot code.
  if (callee->hasIonScript() &&
      callee->ionScript()->method()->containsNativePC(pc)) {
    type_ = FrameType::IonJS;
    resumePCinCurrentFrame_ = pc;
    return true;
  }

  // Check for containment in Baseline jitcode second.
  if (callee->hasBaselineScript() &&
      callee->baselineScript()->method()->containsNativePC(pc)) {
    type_ = FrameType::BaselineJS;
    resumePCinCurrentFrame_ = pc;
    return true;
  }

  return false;
}

bool JSJitProfilingFrameIterator::tryInitWithTable(JitcodeGlobalTable* table,
                                                   void* pc,
                                                   bool forLastCallSite) {
  if (!pc) {
    return false;
  }

  const JitcodeGlobalEntry* entry = table->lookup(pc);
  if (!entry) {
    return false;
  }

  JSScript* callee = frameScript();

  MOZ_ASSERT(entry->isIon() || entry->isIonIC() || entry->isBaseline() ||
             entry->isBaselineInterpreter() || entry->isDummy());

  // Treat dummy lookups as an empty frame sequence.
  if (entry->isDummy()) {
    type_ = FrameType::CppToJSJit;
    fp_ = nullptr;
    resumePCinCurrentFrame_ = nullptr;
    return true;
  }

  // For IonICEntry, use the corresponding IonEntry.
  if (entry->isIonIC()) {
    entry = table->lookup(entry->asIonIC().rejoinAddr());
    MOZ_ASSERT(entry);
    MOZ_RELEASE_ASSERT(entry->isIon());
  }

  if (entry->isIon()) {
    // If looked-up callee doesn't match frame callee, don't accept
    // lastProfilingCallSite
    if (entry->asIon().getScript(0) != callee) {
      return false;
    }

    type_ = FrameType::IonJS;
    resumePCinCurrentFrame_ = pc;
    return true;
  }

  if (entry->isBaseline()) {
    // If looked-up callee doesn't match frame callee, don't accept
    // lastProfilingCallSite
    if (forLastCallSite && entry->asBaseline().script() != callee) {
      return false;
    }

    type_ = FrameType::BaselineJS;
    resumePCinCurrentFrame_ = pc;
    return true;
  }

  if (entry->isBaselineInterpreter()) {
    type_ = FrameType::BaselineJS;
    resumePCinCurrentFrame_ = pc;
    return true;
  }

  return false;
}

const char* JSJitProfilingFrameIterator::baselineInterpreterLabel() const {
  MOZ_ASSERT(type_ == FrameType::BaselineJS);
  return frameScript()->jitScript()->profileString();
}

void JSJitProfilingFrameIterator::baselineInterpreterScriptPC(
    JSScript** script, jsbytecode** pc, uint64_t* realmID) const {
  MOZ_ASSERT(type_ == FrameType::BaselineJS);
  BaselineFrame* blFrame = (BaselineFrame*)(fp_ - BaselineFrame::Size());
  *script = frameScript();
  *pc = (*script)->code();

  if (blFrame->runningInInterpreter() &&
      blFrame->interpreterScript() == *script) {
    jsbytecode* interpPC = blFrame->interpreterPC();
    if ((*script)->containsPC(interpPC)) {
      *pc = interpPC;
    }

    *realmID = (*script)->realm()->creationOptions().profilerRealmID();
  }
}

void JSJitProfilingFrameIterator::operator++() {
  JitFrameLayout* frame = framePtr();
  moveToNextFrame(frame);
}

void JSJitProfilingFrameIterator::moveToNextFrame(CommonFrameLayout* frame) {
  /*
   * fp_ points to a Baseline or Ion frame.  The possible call-stacks
   * patterns occurring between this frame and a previous Ion, Baseline or Entry
   * frame are as follows:
   *
   * <Baseline-Or-Ion>
   * ^
   * |
   * ^--- Ion (or Baseline JSOp::Resume)
   * |
   * ^--- Baseline Stub <---- Baseline
   * |
   * ^--- IonICCall <---- Ion
   * |
   * ^--- WasmToJSJit <---- (other wasm frames, not handled by this iterator)
   * |
   * ^--- Entry Frame (BaselineInterpreter) (unwrapped)
   * |
   * ^--- Arguments Rectifier (unwrapped)
   * |
   * ^--- Trampoline Native (unwrapped)
   * |
   * ^--- Entry Frame (CppToJSJit)
   *
   * NOTE: Keep this in sync with JitRuntime::generateProfilerExitFrameTailStub!
   */

  while (true) {
    // Unwrap baseline interpreter entry frame.
    if (frame->prevType() == FrameType::BaselineInterpreterEntry) {
      frame = GetPreviousRawFrame<BaselineInterpreterEntryFrameLayout*>(frame);
      continue;
    }

    // Unwrap rectifier frames.
    if (frame->prevType() == FrameType::Rectifier) {
      frame = GetPreviousRawFrame<RectifierFrameLayout*>(frame);
      MOZ_ASSERT(frame->prevType() == FrameType::IonJS ||
                 frame->prevType() == FrameType::BaselineStub ||
                 frame->prevType() == FrameType::TrampolineNative ||
                 frame->prevType() == FrameType::WasmToJSJit ||
                 frame->prevType() == FrameType::CppToJSJit);
      continue;
    }

    // Unwrap TrampolineNative frames.
    if (frame->prevType() == FrameType::TrampolineNative) {
      frame = GetPreviousRawFrame<TrampolineNativeFrameLayout*>(frame);
      MOZ_ASSERT(frame->prevType() == FrameType::IonJS ||
                 frame->prevType() == FrameType::BaselineStub ||
                 frame->prevType() == FrameType::Rectifier ||
                 frame->prevType() == FrameType::WasmToJSJit ||
                 frame->prevType() == FrameType::CppToJSJit);
      continue;
    }

    break;
  }

  FrameType prevType = frame->prevType();
  switch (prevType) {
    case FrameType::IonJS:
    case FrameType::BaselineJS:
      resumePCinCurrentFrame_ = frame->returnAddress();
      fp_ = GetPreviousRawFrame<uint8_t*>(frame);
      type_ = prevType;
      return;

    case FrameType::BaselineStub:
    case FrameType::IonICCall: {
      FrameType stubPrevType = (prevType == FrameType::BaselineStub)
                                   ? FrameType::BaselineJS
                                   : FrameType::IonJS;
      auto* stubFrame = GetPreviousRawFrame<CommonFrameLayout*>(frame);
      MOZ_ASSERT(stubFrame->prevType() == stubPrevType);
      resumePCinCurrentFrame_ = stubFrame->returnAddress();
      fp_ = GetPreviousRawFrame<uint8_t*>(stubFrame);
      type_ = stubPrevType;
      return;
    }

    case FrameType::WasmToJSJit:
      // No previous JS JIT frame. Set fp_ to nullptr to indicate the
      // JSJitProfilingFrameIterator is done(). Also set wasmCallerFP_ so that
      // the caller can pass it to a Wasm frame iterator.
      resumePCinCurrentFrame_ = nullptr;
      fp_ = nullptr;
      type_ = FrameType::WasmToJSJit;
      MOZ_ASSERT(!wasmCallerFP_);
      wasmCallerFP_ = GetPreviousRawFrame<uint8_t*>(frame);
      MOZ_ASSERT(wasmCallerFP_);
      MOZ_ASSERT(done());
      return;

    case FrameType::CppToJSJit:
      // No previous JS JIT frame. Set fp_ to nullptr to indicate the
      // JSJitProfilingFrameIterator is done().
      resumePCinCurrentFrame_ = nullptr;
      fp_ = nullptr;
      type_ = FrameType::CppToJSJit;
      MOZ_ASSERT(!wasmCallerFP_);
      MOZ_ASSERT(done());
      return;

    case FrameType::BaselineInterpreterEntry:
    case FrameType::Rectifier:
    case FrameType::TrampolineNative:
    case FrameType::Exit:
    case FrameType::Bailout:
    case FrameType::JSJitToWasm:
      // Rectifier and Baseline Interpreter entry frames are handled before
      // this switch. The other frame types can't call JS functions directly.
      break;
  }

  MOZ_CRASH("Bad frame type.");
}
