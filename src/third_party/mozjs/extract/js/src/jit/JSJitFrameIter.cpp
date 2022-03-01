/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JSJitFrameIter-inl.h"

#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineIC.h"
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

#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

JSJitFrameIter::JSJitFrameIter(const JitActivation* activation)
    : JSJitFrameIter(activation, FrameType::Exit, activation->jsExitFP()) {}

JSJitFrameIter::JSJitFrameIter(const JitActivation* activation,
                               FrameType frameType, uint8_t* fp)
    : current_(fp),
      type_(frameType),
      resumePCinCurrentFrame_(nullptr),
      frameSize_(0),
      cachedSafepointIndex_(nullptr),
      activation_(activation) {
  MOZ_ASSERT(type_ == FrameType::JSJitToWasm || type_ == FrameType::Exit);
  if (activation_->bailoutData()) {
    current_ = activation_->bailoutData()->fp();
    frameSize_ = activation_->bailoutData()->topFrameSize();
    type_ = FrameType::Bailout;
  } else {
    MOZ_ASSERT(!TlsContext.get()->inUnsafeCallWithABI);
  }
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
  MOZ_ASSERT(isScripted());
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

bool JSJitFrameIter::isFunctionFrame() const {
  return CalleeTokenIsFunction(calleeToken());
}

JSScript* JSJitFrameIter::script() const {
  MOZ_ASSERT(isScripted());
  if (isBaselineJS()) {
    return baselineFrame()->script();
  }
  JSScript* script = ScriptFromCalleeToken(calleeToken());
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

Value* JSJitFrameIter::actualArgs() const { return jsFrame()->argv() + 1; }

uint8_t* JSJitFrameIter::prevFp() const {
  return current_ + current()->prevFrameLocalSize() + current()->headerSize();
}

void JSJitFrameIter::operator++() {
  MOZ_ASSERT(!isEntry());

  // Compute BaselineFrame size, the size stored in the descriptor excluding
  // VMFunction arguments pushed for VM calls.
  //
  // In debug builds this is equivalent to BaselineFrame::debugFrameSize_. This
  // is asserted at the end of this method.
  if (current()->prevType() == FrameType::BaselineJS) {
    uint32_t frameSize = prevFrameLocalSize();
    if (isExitFrame() && exitFrame()->isWrapperExit()) {
      const VMFunctionData* data = exitFrame()->footer()->function();
      frameSize -= data->explicitStackSlots() * sizeof(void*);
    }
    baselineFrameSize_ = mozilla::Some(frameSize);
  } else {
    baselineFrameSize_ = mozilla::Nothing();
  }

  frameSize_ = prevFrameLocalSize();
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
  uintptr_t* spill = spillBase();
  MachineState machine;

  for (GeneralRegisterBackwardIterator iter(reader.allGprSpills()); iter.more();
       ++iter) {
    machine.setRegisterLocation(*iter, --spill);
  }

  uint8_t* spillAlign = alignDoubleSpill(reinterpret_cast<uint8_t*>(spill));

  char* floatSpill = reinterpret_cast<char*>(spillAlign);
  FloatRegisterSet fregs = reader.allFloatSpills().set();
  fregs = fregs.reduceSetForPush();
  for (FloatRegisterBackwardIterator iter(fregs); iter.more(); ++iter) {
    floatSpill -= (*iter).size();
    for (uint32_t a = 0; a < (*iter).numAlignedAliased(); a++) {
      // Only say that registers that actually start here start here.
      // e.g. d0 should not start at s1, only at s0.
      FloatRegister ftmp = (*iter).alignedAliased(a);
      machine.setRegisterLocation(ftmp, (double*)floatSpill);
    }
  }

  return machine;
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
      fprintf(stderr, "  Frame size: %u\n",
              unsigned(current()->prevFrameLocalSize()));
      break;
    case FrameType::BaselineJS:
      dumpBaseline();
      break;
    case FrameType::BaselineStub:
      fprintf(stderr, " Baseline stub frame\n");
      fprintf(stderr, "  Frame size: %u\n",
              unsigned(current()->prevFrameLocalSize()));
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
    case FrameType::Rectifier:
      fprintf(stderr, " Rectifier frame\n");
      fprintf(stderr, "  Frame size: %u\n",
              unsigned(current()->prevFrameLocalSize()));
      break;
    case FrameType::IonICCall:
      fprintf(stderr, " Ion IC call\n");
      fprintf(stderr, "  Frame size: %u\n",
              unsigned(current()->prevFrameLocalSize()));
      break;
    case FrameType::WasmToJSJit:
      fprintf(stderr, " Fast wasm-to-JS entry frame\n");
      fprintf(stderr, "  Frame size: %u\n",
              unsigned(current()->prevFrameLocalSize()));
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

  JitcodeGlobalEntry::BytecodeLocationVector location;
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
                                                         void* pc) {
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

  // Profiler sampling must NOT be suppressed if we are here.
  MOZ_ASSERT(cx->isProfilerSamplingEnabled());

  // Try initializing with sampler pc
  if (tryInitWithPC(pc)) {
    return;
  }

  // Try initializing with sampler pc using native=>bytecode table.
  JitcodeGlobalTable* table =
      cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
  if (tryInitWithTable(table, pc, /* forLastCallSite = */ false)) {
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

  // If nothing matches, for now just assume we are at the start of the last
  // frame's baseline jit code or interpreter code.
  type_ = FrameType::BaselineJS;
  if (frameScript()->hasBaselineScript()) {
    resumePCinCurrentFrame_ = frameScript()->baselineScript()->method()->raw();
  } else {
    MOZ_ASSERT(IsBaselineInterpreterEnabled());
    resumePCinCurrentFrame_ =
        cx->runtime()->jitRuntime()->baselineInterpreter().codeRaw();
  }
}

template <typename ReturnType = CommonFrameLayout*>
static inline ReturnType GetPreviousRawFrame(CommonFrameLayout* frame) {
  size_t prevSize = frame->prevFrameLocalSize() + frame->headerSize();
  return ReturnType((uint8_t*)frame + prevSize);
}

JSJitProfilingFrameIterator::JSJitProfilingFrameIterator(
    CommonFrameLayout* fp) {
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

  MOZ_ASSERT(entry->isIon() || entry->isBaseline() ||
             entry->isBaselineInterpreter() || entry->isDummy());

  // Treat dummy lookups as an empty frame sequence.
  if (entry->isDummy()) {
    type_ = FrameType::CppToJSJit;
    fp_ = nullptr;
    resumePCinCurrentFrame_ = nullptr;
    return true;
  }

  if (entry->isIon()) {
    // If looked-up callee doesn't match frame callee, don't accept
    // lastProfilingCallSite
    if (entry->ionEntry().getScript(0) != callee) {
      return false;
    }

    type_ = FrameType::IonJS;
    resumePCinCurrentFrame_ = pc;
    return true;
  }

  if (entry->isBaseline()) {
    // If looked-up callee doesn't match frame callee, don't accept
    // lastProfilingCallSite
    if (forLastCallSite && entry->baselineEntry().script() != callee) {
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
  BaselineFrame* blFrame =
      (BaselineFrame*)(fp_ - BaselineFrame::FramePointerOffset -
                       BaselineFrame::Size());
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

void JSJitProfilingFrameIterator::moveToWasmFrame(CommonFrameLayout* frame) {
  // No previous js jit frame, this is a transition frame, used to
  // pass a wasm iterator the correct value of FP.
  resumePCinCurrentFrame_ = nullptr;
  fp_ = GetPreviousRawFrame<uint8_t*>(frame);
  type_ = FrameType::WasmToJSJit;
  MOZ_ASSERT(!done());
}

void JSJitProfilingFrameIterator::moveToCppEntryFrame() {
  // No previous frame, set to nullptr to indicate that
  // JSJitProfilingFrameIterator is done().
  resumePCinCurrentFrame_ = nullptr;
  fp_ = nullptr;
  type_ = FrameType::CppToJSJit;
}

void JSJitProfilingFrameIterator::moveToNextFrame(CommonFrameLayout* frame) {
  /*
   * fp_ points to a Baseline or Ion frame.  The possible call-stacks
   * patterns occurring between this frame and a previous Ion or Baseline
   * frame are as follows:
   *
   * <Baseline-Or-Ion>
   * ^
   * |
   * ^--- Ion
   * |
   * ^--- Baseline Stub <---- Baseline
   * |
   * ^--- WasmToJSJit <---- (other wasm frames, not handled by this iterator)
   * |
   * ^--- Argument Rectifier
   * |    ^
   * |    |
   * |    ^--- Ion
   * |    |
   * |    ^--- Baseline Stub <---- Baseline
   * |    |
   * |    ^--- WasmToJSJit <--- (other wasm frames)
   * |    |
   * |    ^--- CppToJSJit
   * |
   * ^--- Entry Frame (From C++)
   *      Exit Frame (From previous JitActivation)
   *      ^
   *      |
   *      ^--- Ion
   *      |
   *      ^--- Baseline
   *      |
   *      ^--- Baseline Stub <---- Baseline
   */
  FrameType prevType = frame->prevType();

  if (prevType == FrameType::IonJS) {
    resumePCinCurrentFrame_ = frame->returnAddress();
    fp_ = GetPreviousRawFrame<uint8_t*>(frame);
    type_ = FrameType::IonJS;
    return;
  }

  if (prevType == FrameType::BaselineJS) {
    resumePCinCurrentFrame_ = frame->returnAddress();
    fp_ = GetPreviousRawFrame<uint8_t*>(frame);
    type_ = FrameType::BaselineJS;
    return;
  }

  if (prevType == FrameType::BaselineStub) {
    BaselineStubFrameLayout* stubFrame =
        GetPreviousRawFrame<BaselineStubFrameLayout*>(frame);
    MOZ_ASSERT(stubFrame->prevType() == FrameType::BaselineJS);

    resumePCinCurrentFrame_ = stubFrame->returnAddress();
    fp_ = ((uint8_t*)stubFrame->reverseSavedFramePtr()) +
          jit::BaselineFrame::FramePointerOffset;
    type_ = FrameType::BaselineJS;
    return;
  }

  if (prevType == FrameType::Rectifier) {
    RectifierFrameLayout* rectFrame =
        GetPreviousRawFrame<RectifierFrameLayout*>(frame);
    FrameType rectPrevType = rectFrame->prevType();

    if (rectPrevType == FrameType::IonJS) {
      resumePCinCurrentFrame_ = rectFrame->returnAddress();
      fp_ = GetPreviousRawFrame<uint8_t*>(rectFrame);
      type_ = FrameType::IonJS;
      return;
    }

    if (rectPrevType == FrameType::BaselineStub) {
      BaselineStubFrameLayout* stubFrame =
          GetPreviousRawFrame<BaselineStubFrameLayout*>(rectFrame);
      resumePCinCurrentFrame_ = stubFrame->returnAddress();
      fp_ = ((uint8_t*)stubFrame->reverseSavedFramePtr()) +
            jit::BaselineFrame::FramePointerOffset;
      type_ = FrameType::BaselineJS;
      return;
    }

    if (rectPrevType == FrameType::WasmToJSJit) {
      moveToWasmFrame(rectFrame);
      return;
    }

    if (rectPrevType == FrameType::CppToJSJit) {
      moveToCppEntryFrame();
      return;
    }

    MOZ_CRASH("Bad frame type prior to rectifier frame.");
  }

  if (prevType == FrameType::IonICCall) {
    IonICCallFrameLayout* callFrame =
        GetPreviousRawFrame<IonICCallFrameLayout*>(frame);

    MOZ_ASSERT(callFrame->prevType() == FrameType::IonJS);

    resumePCinCurrentFrame_ = callFrame->returnAddress();
    fp_ = GetPreviousRawFrame<uint8_t*>(callFrame);
    type_ = FrameType::IonJS;
    return;
  }

  if (prevType == FrameType::WasmToJSJit) {
    moveToWasmFrame(frame);
    return;
  }

  if (prevType == FrameType::CppToJSJit) {
    moveToCppEntryFrame();
    return;
  }

  MOZ_CRASH("Bad frame type.");
}
