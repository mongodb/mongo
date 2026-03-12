/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/ScopeExit.h"

#include "builtin/ModuleObject.h"
#include "debugger/DebugAPI.h"
#include "gc/GC.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/CalleeToken.h"
#include "jit/Invalidation.h"
#include "jit/Ion.h"
#include "jit/IonScript.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "jit/RematerializedFrame.h"
#include "jit/SharedICRegisters.h"
#include "jit/Simulator.h"
#include "js/friend/StackLimits.h"  // js::AutoCheckRecursionLimit, js::ReportOverRecursed
#include "js/Utility.h"
#include "proxy/ScriptedProxyHandler.h"
#include "util/Memory.h"
#include "vm/ArgumentsObject.h"
#include "vm/BytecodeUtil.h"
#include "vm/JitActivation.h"

#include "jit/JitFrames-inl.h"
#include "vm/JSAtomUtils-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;
using mozilla::Maybe;

// BaselineStackBuilder may reallocate its buffer if the current one is too
// small. To avoid dangling pointers, BufferPointer represents a pointer into
// this buffer as a pointer to the header and a fixed offset.
template <typename T>
class BufferPointer {
  const UniquePtr<BaselineBailoutInfo>& header_;
  size_t offset_;
  bool heap_;

 public:
  BufferPointer(const UniquePtr<BaselineBailoutInfo>& header, size_t offset,
                bool heap)
      : header_(header), offset_(offset), heap_(heap) {}

  T* get() const {
    BaselineBailoutInfo* header = header_.get();
    if (!heap_) {
      return (T*)(header->incomingStack + offset_);
    }

    uint8_t* p = header->copyStackTop - offset_;
    MOZ_ASSERT(p >= header->copyStackBottom && p < header->copyStackTop);
    return (T*)p;
  }

  void set(const T& value) { *get() = value; }

  // Note: we return a copy instead of a reference, to avoid potential memory
  // safety hazards when the underlying buffer gets resized.
  const T operator*() const { return *get(); }
  T* operator->() const { return get(); }
};

/**
 * BaselineStackBuilder helps abstract the process of rebuilding the C stack on
 * the heap. It takes a bailout iterator and keeps track of the point on the C
 * stack from which the reconstructed frames will be written.
 *
 * It exposes methods to write data into the heap memory storing the
 * reconstructed stack.  It also exposes method to easily calculate addresses.
 * This includes both the virtual address that a particular value will be at
 * when it's eventually copied onto the stack, as well as the current actual
 * address of that value (whether on the heap allocated portion being
 * constructed or the existing stack).
 *
 * The abstraction handles transparent re-allocation of the heap memory when it
 * needs to be enlarged to accommodate new data.  Similarly to the C stack, the
 * data that's written to the reconstructed stack grows from high to low in
 * memory.
 *
 * The lowest region of the allocated memory contains a BaselineBailoutInfo
 * structure that points to the start and end of the written data.
 */
class MOZ_STACK_CLASS BaselineStackBuilder {
  JSContext* cx_;
  JitFrameLayout* frame_ = nullptr;
  SnapshotIterator& iter_;
  RootedValueVector outermostFrameFormals_;

  size_t bufferTotal_ = 1024;
  size_t bufferAvail_ = 0;
  size_t bufferUsed_ = 0;
  size_t framePushed_ = 0;

  UniquePtr<BaselineBailoutInfo> header_;

  JSScript* script_;
  JSFunction* fun_;
  const ExceptionBailoutInfo* excInfo_;
  ICScript* icScript_;

  jsbytecode* pc_ = nullptr;
  JSOp op_ = JSOp::Nop;
  mozilla::Maybe<ResumeMode> resumeMode_;
  uint32_t exprStackSlots_ = 0;
  void* prevFramePtr_ = nullptr;
  Maybe<BufferPointer<BaselineFrame>> blFrame_;

  size_t frameNo_ = 0;
  JSFunction* nextCallee_ = nullptr;

  BailoutKind bailoutKind_;

  bool canUseTrialInlinedICScripts_ = true;

  // The baseline frames we will reconstruct on the heap are not
  // rooted, so GC must be suppressed.
  gc::AutoSuppressGC suppress_;

 public:
  BaselineStackBuilder(JSContext* cx, const JSJitFrameIter& frameIter,
                       SnapshotIterator& iter,
                       const ExceptionBailoutInfo* excInfo,
                       BailoutReason reason);

  [[nodiscard]] bool init() {
    MOZ_ASSERT(!header_);
    MOZ_ASSERT(bufferUsed_ == 0);

    uint8_t* bufferRaw = cx_->pod_calloc<uint8_t>(bufferTotal_);
    if (!bufferRaw) {
      return false;
    }
    bufferAvail_ = bufferTotal_ - sizeof(BaselineBailoutInfo);

    header_.reset(new (bufferRaw) BaselineBailoutInfo());
    header_->incomingStack = reinterpret_cast<uint8_t*>(frame_);
    header_->copyStackTop = bufferRaw + bufferTotal_;
    header_->copyStackBottom = header_->copyStackTop;
    return true;
  }

  [[nodiscard]] bool buildOneFrame();
  bool done();
  void nextFrame();

  JSScript* script() const { return script_; }
  size_t frameNo() const { return frameNo_; }
  bool isOutermostFrame() const { return frameNo_ == 0; }
  MutableHandleValueVector outermostFrameFormals() {
    return &outermostFrameFormals_;
  }
  BailoutKind bailoutKind() const { return bailoutKind_; }

  inline JitFrameLayout* startFrame() { return frame_; }

  BaselineBailoutInfo* info() {
    MOZ_ASSERT(header_);
    return header_.get();
  }

  BaselineBailoutInfo* takeBuffer() {
    MOZ_ASSERT(header_);
    return header_.release();
  }

 private:
  [[nodiscard]] bool initFrame();
  [[nodiscard]] bool buildBaselineFrame();
  [[nodiscard]] bool buildArguments();
  [[nodiscard]] bool buildFixedSlots();
  [[nodiscard]] bool fixUpCallerArgs(MutableHandleValueVector savedCallerArgs,
                                     bool* fixedUp);
  [[nodiscard]] bool buildFinallyException();
  [[nodiscard]] bool buildExpressionStack();
  [[nodiscard]] bool finishLastFrame();

  [[nodiscard]] bool prepareForNextFrame(HandleValueVector savedCallerArgs);
  [[nodiscard]] bool finishOuterFrame();
  [[nodiscard]] bool buildStubFrame(uint32_t frameSize,
                                    HandleValueVector savedCallerArgs);
  [[nodiscard]] bool buildRectifierFrame(uint32_t actualArgc,
                                         size_t endOfBaselineStubArgs);

#ifdef DEBUG
  [[nodiscard]] bool validateFrame();
#endif

#ifdef DEBUG
  bool envChainSlotCanBeOptimized();
#endif

  bool isPrologueBailout();
  jsbytecode* getResumePC();
  void* getStubReturnAddress();

  uint32_t exprStackSlots() const { return exprStackSlots_; }

  // Returns true if we're bailing out to a catch or finally block in this frame
  bool catchingException() const {
    return excInfo_ && excInfo_->catchingException() &&
           excInfo_->frameNo() == frameNo_;
  }

  // Returns true if we're bailing out to a finally block in this frame.
  bool resumingInFinallyBlock() const {
    return catchingException() && excInfo_->isFinally();
  }

  bool forcedReturn() const { return excInfo_ && excInfo_->forcedReturn(); }

  // Returns true if we're bailing out in place for debug mode
  bool propagatingIonExceptionForDebugMode() const {
    return excInfo_ && excInfo_->propagatingIonExceptionForDebugMode();
  }

  void* prevFramePtr() const {
    MOZ_ASSERT(prevFramePtr_);
    return prevFramePtr_;
  }
  BufferPointer<BaselineFrame>& blFrame() { return blFrame_.ref(); }

  void setNextCallee(JSFunction* nextCallee,
                     TrialInliningState trialInliningState);
  JSFunction* nextCallee() const { return nextCallee_; }

  jsbytecode* pc() const { return pc_; }
  bool resumeAfter() const {
    return !catchingException() && iter_.resumeAfter();
  }

  ResumeMode resumeMode() const { return *resumeMode_; }

  bool needToSaveCallerArgs() const {
    return resumeMode() == ResumeMode::InlinedAccessor;
  }

  [[nodiscard]] bool enlarge() {
    MOZ_ASSERT(header_ != nullptr);
    if (bufferTotal_ & mozilla::tl::MulOverflowMask<2>::value) {
      ReportOutOfMemory(cx_);
      return false;
    }

    size_t newSize = bufferTotal_ * 2;
    uint8_t* newBufferRaw = cx_->pod_calloc<uint8_t>(newSize);
    if (!newBufferRaw) {
      return false;
    }

    // Initialize the new buffer.
    //
    //   Before:
    //
    //     [ Header | .. | Payload ]
    //
    //   After:
    //
    //     [ Header | ............... | Payload ]
    //
    // Size of Payload is |bufferUsed_|.
    //
    // We need to copy from the old buffer and header to the new buffer before
    // we set header_ (this deletes the old buffer).
    //
    // We also need to update |copyStackBottom| and |copyStackTop| because these
    // fields point to the Payload's start and end, respectively.
    using BailoutInfoPtr = UniquePtr<BaselineBailoutInfo>;
    BailoutInfoPtr newHeader(new (newBufferRaw) BaselineBailoutInfo(*header_));
    newHeader->copyStackTop = newBufferRaw + newSize;
    newHeader->copyStackBottom = newHeader->copyStackTop - bufferUsed_;
    memcpy(newHeader->copyStackBottom, header_->copyStackBottom, bufferUsed_);
    bufferTotal_ = newSize;
    bufferAvail_ = newSize - (sizeof(BaselineBailoutInfo) + bufferUsed_);
    header_ = std::move(newHeader);
    return true;
  }

  void resetFramePushed() { framePushed_ = 0; }

  size_t framePushed() const { return framePushed_; }

  [[nodiscard]] bool subtract(size_t size, const char* info = nullptr) {
    // enlarge the buffer if need be.
    while (size > bufferAvail_) {
      if (!enlarge()) {
        return false;
      }
    }

    // write out element.
    header_->copyStackBottom -= size;
    bufferAvail_ -= size;
    bufferUsed_ += size;
    framePushed_ += size;
    if (info) {
      JitSpew(JitSpew_BaselineBailouts, "      SUB_%03d   %p/%p %-15s",
              (int)size, header_->copyStackBottom,
              virtualPointerAtStackOffset(0), info);
    }
    return true;
  }

  template <typename T>
  [[nodiscard]] bool write(const T& t) {
    MOZ_ASSERT(!(uintptr_t(&t) >= uintptr_t(header_->copyStackBottom) &&
                 uintptr_t(&t) < uintptr_t(header_->copyStackTop)),
               "Should not reference memory that can be freed");
    if (!subtract(sizeof(T))) {
      return false;
    }
    memcpy(header_->copyStackBottom, &t, sizeof(T));
    return true;
  }

  template <typename T>
  [[nodiscard]] bool writePtr(T* t, const char* info) {
    if (!write<T*>(t)) {
      return false;
    }
    if (info) {
      JitSpew(JitSpew_BaselineBailouts, "      WRITE_PTR %p/%p %-15s %p",
              header_->copyStackBottom, virtualPointerAtStackOffset(0), info,
              t);
    }
    return true;
  }

  [[nodiscard]] bool writeWord(size_t w, const char* info) {
    if (!write<size_t>(w)) {
      return false;
    }
    if (info) {
      if (sizeof(size_t) == 4) {
        JitSpew(JitSpew_BaselineBailouts, "      WRITE_WRD %p/%p %-15s %08zx",
                header_->copyStackBottom, virtualPointerAtStackOffset(0), info,
                w);
      } else {
        JitSpew(JitSpew_BaselineBailouts, "      WRITE_WRD %p/%p %-15s %016zx",
                header_->copyStackBottom, virtualPointerAtStackOffset(0), info,
                w);
      }
    }
    return true;
  }

  [[nodiscard]] bool writeValue(const Value& val, const char* info) {
    if (!write<Value>(val)) {
      return false;
    }
    if (info) {
      JitSpew(JitSpew_BaselineBailouts,
              "      WRITE_VAL %p/%p %-15s %016" PRIx64,
              header_->copyStackBottom, virtualPointerAtStackOffset(0), info,
              *((uint64_t*)&val));
    }
    return true;
  }

  [[nodiscard]] bool peekLastValue(Value* result) {
    if (bufferUsed_ < sizeof(Value)) {
      return false;
    }

    memcpy(result, header_->copyStackBottom, sizeof(Value));
    return true;
  }

  [[nodiscard]] bool maybeWritePadding(size_t alignment, size_t after,
                                       const char* info) {
    MOZ_ASSERT(framePushed_ % sizeof(Value) == 0);
    MOZ_ASSERT(after % sizeof(Value) == 0);
    size_t offset = ComputeByteAlignment(after, alignment);
    while (framePushed_ % alignment != offset) {
      if (!writeValue(MagicValue(JS_ARG_POISON), info)) {
        return false;
      }
    }

    return true;
  }

  void setResumeFramePtr(void* resumeFramePtr) {
    header_->resumeFramePtr = resumeFramePtr;
  }

  void setResumeAddr(void* resumeAddr) { header_->resumeAddr = resumeAddr; }

  template <typename T>
  BufferPointer<T> pointerAtStackOffset(size_t offset) {
    if (offset < bufferUsed_) {
      // Calculate offset from copyStackTop.
      offset = header_->copyStackTop - (header_->copyStackBottom + offset);
      return BufferPointer<T>(header_, offset, /* heap = */ true);
    }

    return BufferPointer<T>(header_, offset - bufferUsed_, /* heap = */ false);
  }

  BufferPointer<Value> valuePointerAtStackOffset(size_t offset) {
    return pointerAtStackOffset<Value>(offset);
  }

  inline uint8_t* virtualPointerAtStackOffset(size_t offset) {
    if (offset < bufferUsed_) {
      return reinterpret_cast<uint8_t*>(frame_) - (bufferUsed_ - offset);
    }
    return reinterpret_cast<uint8_t*>(frame_) + (offset - bufferUsed_);
  }
};

void BaselineBailoutInfo::trace(JSTracer* trc) {
  TraceRoot(trc, &tempId, "BaselineBailoutInfo::tempId");
}

BaselineStackBuilder::BaselineStackBuilder(JSContext* cx,
                                           const JSJitFrameIter& frameIter,
                                           SnapshotIterator& iter,
                                           const ExceptionBailoutInfo* excInfo,
                                           BailoutReason reason)
    : cx_(cx),
      frame_(static_cast<JitFrameLayout*>(frameIter.current())),
      iter_(iter),
      outermostFrameFormals_(cx),
      script_(frameIter.script()),
      fun_(frameIter.maybeCallee()),
      excInfo_(excInfo),
      icScript_(script_->jitScript()->icScript()),
      bailoutKind_(iter.bailoutKind()),
      suppress_(cx) {
  MOZ_ASSERT(bufferTotal_ >= sizeof(BaselineBailoutInfo));
  if (reason == BailoutReason::Invalidate) {
    bailoutKind_ = BailoutKind::OnStackInvalidation;
  }
}

bool BaselineStackBuilder::initFrame() {
  // Get the pc and ResumeMode. If we are handling an exception, resume at the
  // pc of the catch or finally block.
  if (catchingException()) {
    pc_ = excInfo_->resumePC();
    resumeMode_ = mozilla::Some(ResumeMode::ResumeAt);
  } else {
    pc_ = script_->offsetToPC(iter_.pcOffset());
    resumeMode_ = mozilla::Some(iter_.resumeMode());
  }
  op_ = JSOp(*pc_);

  // If we are catching an exception, we are bailing out to a catch or
  // finally block and this is the frame where we will resume. Usually the
  // expression stack should be empty in this case but there can be
  // iterators on the stack.
  if (catchingException()) {
    exprStackSlots_ = excInfo_->numExprSlots();
  } else {
    uint32_t totalFrameSlots = iter_.numAllocations();
    uint32_t fixedSlots = script_->nfixed();
    uint32_t argSlots = CountArgSlots(script_, fun_);
    uint32_t intermediates = NumIntermediateValues(resumeMode());
    exprStackSlots_ = totalFrameSlots - fixedSlots - argSlots - intermediates;

    // Verify that there was no underflow.
    MOZ_ASSERT(exprStackSlots_ <= totalFrameSlots);
  }

  JitSpew(JitSpew_BaselineBailouts, "      Unpacking %s:%u:%u",
          script_->filename(), script_->lineno(),
          script_->column().oneOriginValue());
  JitSpew(JitSpew_BaselineBailouts, "      [BASELINE-JS FRAME]");

  // Write the previous frame pointer value. For the outermost frame we reuse
  // the value in the JitFrameLayout already on the stack. Record the virtual
  // stack offset at this location. Later on, if we end up writing out a
  // BaselineStub frame for the next callee, we'll need to save the address.
  if (!isOutermostFrame()) {
    if (!writePtr(prevFramePtr(), "PrevFramePtr")) {
      return false;
    }
  }
  prevFramePtr_ = virtualPointerAtStackOffset(0);

  resetFramePushed();

  return true;
}

void BaselineStackBuilder::setNextCallee(
    JSFunction* nextCallee, TrialInliningState trialInliningState) {
  nextCallee_ = nextCallee;

  if (trialInliningState == TrialInliningState::Inlined &&
      !iter_.ionScript()->purgedICScripts() && canUseTrialInlinedICScripts_) {
    // Update icScript_ to point to the icScript of nextCallee
    const uint32_t pcOff = script_->pcToOffset(pc_);
    icScript_ = icScript_->findInlinedChild(pcOff);
  } else {
    // If we don't know for certain that it's TrialInliningState::Inlined,
    // just use the callee's own ICScript. We could still have the trial
    // inlined ICScript available, but we also could not if we transitioned
    // to TrialInliningState::Failure after being monomorphic inlined.
    //
    // Also use the callee's own ICScript if we purged callee ICScripts.
    icScript_ = nextCallee->nonLazyScript()->jitScript()->icScript();

    if (trialInliningState != TrialInliningState::MonomorphicInlined) {
      // Don't use specialized ICScripts for any of the callees if we had an
      // inlining failure. We're now using the generic ICScript but compilation
      // might have used the trial-inlined ICScript and these can have very
      // different inlining graphs.
      canUseTrialInlinedICScripts_ = false;
    }
  }

  // Assert the ICScript matches nextCallee.
  JSScript* calleeScript = nextCallee->nonLazyScript();
  MOZ_RELEASE_ASSERT(icScript_->numICEntries() == calleeScript->numICEntries());
  MOZ_RELEASE_ASSERT(icScript_->bytecodeSize() == calleeScript->length());
}

bool BaselineStackBuilder::done() {
  if (!iter_.moreFrames()) {
    MOZ_ASSERT(!nextCallee_);
    return true;
  }
  return catchingException();
}

void BaselineStackBuilder::nextFrame() {
  MOZ_ASSERT(nextCallee_);
  fun_ = nextCallee_;
  script_ = fun_->nonLazyScript();
  nextCallee_ = nullptr;

  // Scripts with an IonScript must also have a BaselineScript.
  MOZ_ASSERT(script_->hasBaselineScript());

  frameNo_++;
  iter_.nextInstruction();
}

// Build the BaselineFrame struct
bool BaselineStackBuilder::buildBaselineFrame() {
  if (!subtract(BaselineFrame::Size(), "BaselineFrame")) {
    return false;
  }
  blFrame_.reset();
  blFrame_.emplace(pointerAtStackOffset<BaselineFrame>(0));

  uint32_t flags = BaselineFrame::RUNNING_IN_INTERPRETER;

  // If we are bailing to a script whose execution is observed, mark the
  // baseline frame as a debuggee frame. This is to cover the case where we
  // don't rematerialize the Ion frame via the Debugger.
  if (script_->isDebuggee()) {
    flags |= BaselineFrame::DEBUGGEE;
  }

  // Get |envChain|.
  JSObject* envChain = nullptr;
  Value envChainSlot = iter_.read();
  if (envChainSlot.isObject()) {
    // The env slot has been updated from UndefinedValue. It must be the
    // complete initial environment.
    envChain = &envChainSlot.toObject();

    // Set the HAS_INITIAL_ENV flag if needed. See IsFrameInitialEnvironment.
    MOZ_ASSERT(!script_->isForEval());
    if (fun_ && fun_->needsFunctionEnvironmentObjects()) {
      MOZ_ASSERT(fun_->nonLazyScript()->initialEnvironmentShape());
      flags |= BaselineFrame::HAS_INITIAL_ENV;
    }
  } else {
    MOZ_ASSERT(envChainSlot.isUndefined() ||
               envChainSlot.isMagic(JS_OPTIMIZED_OUT));
    MOZ_ASSERT(envChainSlotCanBeOptimized());

    // The env slot has been optimized out.
    // Get it from the function or script.
    if (fun_) {
      envChain = fun_->environment();
    } else if (script_->isModule()) {
      envChain = script_->module()->environment();
    } else {
      // For global scripts without a non-syntactic env the env
      // chain is the script's global lexical environment. (We do
      // not compile scripts with a non-syntactic global scope).
      // Also note that it's invalid to resume into the prologue in
      // this case because the prologue expects the env chain in R1
      // for eval and global scripts.
      MOZ_ASSERT(!script_->isForEval());
      MOZ_ASSERT(!script_->hasNonSyntacticScope());
      envChain = &(script_->global().lexicalEnvironment());
    }
  }

  // Write |envChain|.
  MOZ_ASSERT(envChain);
  JitSpew(JitSpew_BaselineBailouts, "      EnvChain=%p", envChain);
  blFrame()->setEnvironmentChain(envChain);

  // Get |returnValue| if present.
  Value returnValue = UndefinedValue();
  if (script_->noScriptRval()) {
    // Don't use the return value (likely a JS_OPTIMIZED_OUT MagicValue) to
    // not confuse Baseline.
    iter_.skip();
  } else {
    returnValue = iter_.read();
    flags |= BaselineFrame::HAS_RVAL;
  }

  // Write |returnValue|.
  JitSpew(JitSpew_BaselineBailouts, "      ReturnValue=%016" PRIx64,
          *((uint64_t*)&returnValue));
  blFrame()->setReturnValue(returnValue);

  // Get |argsObj| if present.
  ArgumentsObject* argsObj = nullptr;
  if (script_->needsArgsObj()) {
    Value maybeArgsObj = iter_.read();
    MOZ_ASSERT(maybeArgsObj.isObject() || maybeArgsObj.isUndefined() ||
               maybeArgsObj.isMagic(JS_OPTIMIZED_OUT));
    if (maybeArgsObj.isObject()) {
      argsObj = &maybeArgsObj.toObject().as<ArgumentsObject>();
    }
  }

  // Note: we do not need to initialize the scratchValue field in BaselineFrame.

  // Write |flags|.
  blFrame()->setFlags(flags);

  // Write |icScript|.
  JitSpew(JitSpew_BaselineBailouts, "      ICScript=%p", icScript_);
  blFrame()->setICScript(icScript_);

  // initArgsObjUnchecked modifies the frame's flags, so call it after setFlags.
  if (argsObj) {
    blFrame()->initArgsObjUnchecked(*argsObj);
  }
  return true;
}

// Overwrite the pushed args present in the calling frame with
// the unpacked |thisv| and argument values.
bool BaselineStackBuilder::buildArguments() {
  Value thisv = iter_.read();
  JitSpew(JitSpew_BaselineBailouts, "      Is function!");
  JitSpew(JitSpew_BaselineBailouts, "      thisv=%016" PRIx64,
          *((uint64_t*)&thisv));

  size_t thisvOffset = framePushed() + JitFrameLayout::offsetOfThis();
  valuePointerAtStackOffset(thisvOffset).set(thisv);

  MOZ_ASSERT(iter_.numAllocations() >= CountArgSlots(script_, fun_));
  JitSpew(JitSpew_BaselineBailouts,
          "      frame slots %u, nargs %zu, nfixed %zu", iter_.numAllocations(),
          fun_->nargs(), script_->nfixed());

  bool shouldStoreOutermostFormals =
      isOutermostFrame() && !script_->argsObjAliasesFormals();
  if (shouldStoreOutermostFormals) {
    // This is the first (outermost) frame and we don't have an
    // arguments object aliasing the formals. Due to UCE and phi
    // elimination, we could store an UndefinedValue() here for
    // formals we think are unused, but locals may still reference the
    // original argument slot (MParameter/LArgument) and expect the
    // original Value. To avoid this problem, store the formals in a
    // Vector until we are done.
    MOZ_ASSERT(outermostFrameFormals().empty());
    if (!outermostFrameFormals().resize(fun_->nargs())) {
      return false;
    }
  }

  for (uint32_t i = 0; i < fun_->nargs(); i++) {
    Value arg = iter_.read();
    JitSpew(JitSpew_BaselineBailouts, "      arg %d = %016" PRIx64, (int)i,
            *((uint64_t*)&arg));
    if (!isOutermostFrame()) {
      size_t argOffset = framePushed() + JitFrameLayout::offsetOfActualArg(i);
      valuePointerAtStackOffset(argOffset).set(arg);
    } else if (shouldStoreOutermostFormals) {
      outermostFrameFormals()[i].set(arg);
    } else {
      // When the arguments object aliases the formal arguments, then
      // JSOp::SetArg mutates the argument object. In such cases, the
      // list of arguments reported by the snapshot are only aliases
      // of argument object slots which are optimized to only store
      // differences compared to arguments which are on the stack.
    }
  }
  return true;
}

bool BaselineStackBuilder::buildFixedSlots() {
  for (uint32_t i = 0; i < script_->nfixed(); i++) {
    Value slot = iter_.read();
    if (!writeValue(slot, "FixedValue")) {
      return false;
    }
  }
  return true;
}

// The caller side of inlined js::fun_call and accessors must look
// like the function wasn't inlined.
bool BaselineStackBuilder::fixUpCallerArgs(
    MutableHandleValueVector savedCallerArgs, bool* fixedUp) {
  MOZ_ASSERT(!*fixedUp);

  // Inlining of SpreadCall-like frames not currently supported.
  MOZ_ASSERT(!IsSpreadOp(op_));

  if (resumeMode() != ResumeMode::InlinedFunCall && !needToSaveCallerArgs()) {
    return true;
  }

  // Calculate how many arguments are consumed by the inlined call.
  // All calls pass |callee| and |this|.
  uint32_t inlinedArgs = 2;
  if (resumeMode() == ResumeMode::InlinedFunCall) {
    // The first argument to an inlined FunCall becomes |this|,
    // if it exists. The rest are passed normally.
    MOZ_ASSERT(IsInvokeOp(op_));
    inlinedArgs += GET_ARGC(pc_) > 0 ? GET_ARGC(pc_) - 1 : 0;
  } else {
    MOZ_ASSERT(resumeMode() == ResumeMode::InlinedAccessor);
    MOZ_ASSERT(IsIonInlinableGetterOrSetterOp(op_));
    // Setters are passed one argument. Getters are passed none.
    if (IsSetPropOp(op_)) {
      inlinedArgs++;
    }
  }

  // Calculate how many values are live on the stack across the call,
  // and push them.
  MOZ_ASSERT(inlinedArgs <= exprStackSlots());
  uint32_t liveStackSlots = exprStackSlots() - inlinedArgs;

  JitSpew(JitSpew_BaselineBailouts,
          "      pushing %u expression stack slots before fixup",
          liveStackSlots);
  for (uint32_t i = 0; i < liveStackSlots; i++) {
    Value v = iter_.read();
    if (!writeValue(v, "StackValue")) {
      return false;
    }
  }

  // When we inline js::fun_call, we bypass the native and inline the
  // target directly. When rebuilding the stack, we need to fill in
  // the right number of slots to make it look like the js_native was
  // actually called.
  if (resumeMode() == ResumeMode::InlinedFunCall) {
    // We must transform the stack from |target, this, args| to
    // |js_fun_call, target, this, args|. The value of |js_fun_call|
    // will never be observed, so we push |undefined| for it, followed
    // by the remaining arguments.
    JitSpew(JitSpew_BaselineBailouts,
            "      pushing undefined to fixup funcall");
    if (!writeValue(UndefinedValue(), "StackValue")) {
      return false;
    }
    if (GET_ARGC(pc_) > 0) {
      JitSpew(JitSpew_BaselineBailouts,
              "      pushing %u expression stack slots", inlinedArgs);
      for (uint32_t i = 0; i < inlinedArgs; i++) {
        Value arg = iter_.read();
        if (!writeValue(arg, "StackValue")) {
          return false;
        }
      }
    } else {
      // When we inline FunCall with no arguments, we push an extra
      // |undefined| value for |this|. That value should not appear
      // in the rebuilt baseline frame.
      JitSpew(JitSpew_BaselineBailouts, "      pushing target of funcall");
      Value target = iter_.read();
      if (!writeValue(target, "StackValue")) {
        return false;
      }
      // Skip |this|.
      iter_.skip();
    }
  }

  if (needToSaveCallerArgs()) {
    // Save the actual arguments. They are needed to rebuild the callee frame.
    if (!savedCallerArgs.resize(inlinedArgs)) {
      return false;
    }
    for (uint32_t i = 0; i < inlinedArgs; i++) {
      savedCallerArgs[i].set(iter_.read());
    }

    if (IsSetPropOp(op_)) {
      // The RHS argument to SetProp remains on the stack after the
      // operation and is observable, so we have to fill it in.
      Value initialArg = savedCallerArgs[inlinedArgs - 1];
      JitSpew(JitSpew_BaselineBailouts,
              "     pushing setter's initial argument");
      if (!writeValue(initialArg, "StackValue")) {
        return false;
      }
    }
  }

  *fixedUp = true;
  return true;
}

bool BaselineStackBuilder::buildExpressionStack() {
  JitSpew(JitSpew_BaselineBailouts, "      pushing %u expression stack slots",
          exprStackSlots());

  for (uint32_t i = 0; i < exprStackSlots(); i++) {
    Value v;
    // If we're at the newest frame and in the middle of throwing an exception,
    // some expression stack slots might not be available.
    //
    // For example, if we call a function that throws, and then catch the
    // exception, the return value won't be available. This isn't usually a
    // problem, because the expression stack is generally empty when we enter a
    // catch block.  However, if a catch is inside a for-of, there are
    // iterator-related values on the stack. If one of those values is defined
    // by the function that threw, then its value will be unavailable. This is
    // not possible for a user-written catch, but can happen for synthetic
    // catches generated via desugaring. See bug 1934425 for a case involving
    // `for (await using ...)`.
    //
    // Similar issues of trying to recover the result of a throwing function can
    // also occur when bailing out while propagating an exception due to debug
    // mode.
    //
    // We therefore use a fallible read here.
    if (!iter_.tryRead(&v)) {
      MOZ_ASSERT(
          !iter_.moreFrames() &&
          (catchingException() || propagatingIonExceptionForDebugMode()));
      v = MagicValue(JS_OPTIMIZED_OUT);
    }
    if (!writeValue(v, "StackValue")) {
      return false;
    }
  }

  if (resumeMode() == ResumeMode::ResumeAfterCheckProxyGetResult) {
    JitSpew(JitSpew_BaselineBailouts,
            "      Checking that the proxy's get trap result matches "
            "expectations.");
    Value returnVal;
    if (peekLastValue(&returnVal) && !returnVal.isMagic(JS_OPTIMIZED_OUT)) {
      Value idVal = iter_.read();
      Value targetVal = iter_.read();

      MOZ_RELEASE_ASSERT(!idVal.isMagic());
      MOZ_RELEASE_ASSERT(targetVal.isObject());
      RootedObject target(cx_, &targetVal.toObject());
      RootedValue rootedIdVal(cx_, idVal);
      RootedId id(cx_);
      if (!PrimitiveValueToId<CanGC>(cx_, rootedIdVal, &id)) {
        return false;
      }
      RootedValue value(cx_, returnVal);

      auto validation =
          ScriptedProxyHandler::checkGetTrapResult(cx_, target, id, value);
      if (validation != ScriptedProxyHandler::GetTrapValidationResult::OK) {
        header_->tempId = id.get();

        JitSpew(
            JitSpew_BaselineBailouts,
            "      Proxy get trap result mismatch! Overwriting bailout kind");
        if (validation == ScriptedProxyHandler::GetTrapValidationResult::
                              MustReportSameValue) {
          bailoutKind_ = BailoutKind::ThrowProxyTrapMustReportSameValue;
        } else if (validation == ScriptedProxyHandler::GetTrapValidationResult::
                                     MustReportUndefined) {
          bailoutKind_ = BailoutKind::ThrowProxyTrapMustReportUndefined;
        } else {
          return false;
        }
      }
    }

    return true;
  }

  if (resumeMode() == ResumeMode::ResumeAfterCheckIsObject) {
    JitSpew(JitSpew_BaselineBailouts,
            "      Checking that intermediate value is an object");
    Value returnVal;
    if (iter_.tryRead(&returnVal) && !returnVal.isObject()) {
      MOZ_ASSERT(!returnVal.isMagic());
      JitSpew(JitSpew_BaselineBailouts,
              "      Not an object! Overwriting bailout kind");
      bailoutKind_ = BailoutKind::ThrowCheckIsObject;
    }
  }

  return true;
}

bool BaselineStackBuilder::buildFinallyException() {
  MOZ_ASSERT(resumingInFinallyBlock());

  if (!writeValue(excInfo_->finallyException(), "Exception")) {
    return false;
  }
  if (!writeValue(excInfo_->finallyExceptionStack(), "ExceptionStack")) {
    return false;
  }
  if (!writeValue(BooleanValue(true), "throwing")) {
    return false;
  }

  return true;
}

bool BaselineStackBuilder::prepareForNextFrame(
    HandleValueVector savedCallerArgs) {
  const uint32_t frameSize = framePushed();

  // Write out descriptor and return address for the baseline frame.
  // The icEntry in question MUST have an inlinable fallback stub.
  if (!finishOuterFrame()) {
    return false;
  }

  return buildStubFrame(frameSize, savedCallerArgs);
}

bool BaselineStackBuilder::finishOuterFrame() {
  // .               .
  // |  Descr(BLJS)  |
  // +---------------+
  // |  ReturnAddr   |
  // +===============+

  const BaselineInterpreter& baselineInterp =
      cx_->runtime()->jitRuntime()->baselineInterpreter();

  blFrame()->setInterpreterFields(script_, pc_);

  // Write out descriptor of BaselineJS frame.
  size_t baselineFrameDescr = MakeFrameDescriptor(FrameType::BaselineJS);
  if (!writeWord(baselineFrameDescr, "Descriptor")) {
    return false;
  }

  uint8_t* retAddr = baselineInterp.retAddrForIC(op_);
  return writePtr(retAddr, "ReturnAddr");
}

bool BaselineStackBuilder::buildStubFrame(uint32_t frameSize,
                                          HandleValueVector savedCallerArgs) {
  // Build baseline stub frame:
  // +===============+
  // |   FramePtr    |
  // +---------------+
  // |    StubPtr    |
  // +---------------+
  // |   Padding?    |
  // +---------------+
  // |     ArgA      |
  // +---------------+
  // |     ...       |
  // +---------------+
  // |     Arg0      |
  // +---------------+
  // |     ThisV     |
  // +---------------+
  // |  CalleeToken  |
  // +---------------+
  // | Descr(BLStub) |
  // +---------------+
  // |  ReturnAddr   |
  // +===============+

  JitSpew(JitSpew_BaselineBailouts, "      [BASELINE-STUB FRAME]");

  // Write previous frame pointer (saved earlier).
  if (!writePtr(prevFramePtr(), "PrevFramePtr")) {
    return false;
  }
  prevFramePtr_ = virtualPointerAtStackOffset(0);

  // Write stub pointer.
  uint32_t pcOff = script_->pcToOffset(pc_);
  JitScript* jitScript = script_->jitScript();
  const ICEntry& icEntry = jitScript->icEntryFromPCOffset(pcOff);
  ICFallbackStub* fallback = jitScript->fallbackStubForICEntry(&icEntry);
  if (!writePtr(fallback, "StubPtr")) {
    return false;
  }

  // Write out the arguments, copied from the baseline frame. The order
  // of the arguments is reversed relative to the baseline frame's stack
  // values.
  MOZ_ASSERT(IsIonInlinableOp(op_));
  bool pushedNewTarget = IsConstructPC(pc_);
  unsigned actualArgc;
  Value callee;
  if (needToSaveCallerArgs()) {
    // For accessors, the arguments are not on the stack anymore,
    // but they are copied in a vector and are written here.
    callee = savedCallerArgs[0];
    actualArgc = IsSetPropOp(op_) ? 1 : 0;

    // Align the stack based on the number of arguments.
    size_t afterFrameSize =
        (actualArgc + 1) * sizeof(Value) + JitFrameLayout::Size();
    if (!maybeWritePadding(JitStackAlignment, afterFrameSize, "Padding")) {
      return false;
    }

    // Push arguments.
    MOZ_ASSERT(actualArgc + 2 <= exprStackSlots());
    MOZ_ASSERT(savedCallerArgs.length() == actualArgc + 2);
    for (unsigned i = 0; i < actualArgc + 1; i++) {
      size_t arg = savedCallerArgs.length() - (i + 1);
      if (!writeValue(savedCallerArgs[arg], "ArgVal")) {
        return false;
      }
    }
  } else if (resumeMode() == ResumeMode::InlinedFunCall && GET_ARGC(pc_) == 0) {
    // When calling FunCall with 0 arguments, we push |undefined|
    // for this. See BaselineCacheIRCompiler::pushFunCallArguments.
    MOZ_ASSERT(!pushedNewTarget);
    actualArgc = 0;
    // Align the stack based on pushing |this| and 0 arguments.
    size_t afterFrameSize = sizeof(Value) + JitFrameLayout::Size();
    if (!maybeWritePadding(JitStackAlignment, afterFrameSize, "Padding")) {
      return false;
    }
    // Push an undefined value for |this|.
    if (!writeValue(UndefinedValue(), "ThisValue")) {
      return false;
    }
    size_t calleeSlot = blFrame()->numValueSlots(frameSize) - 1;
    callee = *blFrame()->valueSlot(calleeSlot);

  } else {
    MOZ_ASSERT(resumeMode() == ResumeMode::InlinedStandardCall ||
               resumeMode() == ResumeMode::InlinedFunCall);
    actualArgc = GET_ARGC(pc_);
    if (resumeMode() == ResumeMode::InlinedFunCall) {
      // See BaselineCacheIRCompiler::pushFunCallArguments.
      MOZ_ASSERT(actualArgc > 0);
      actualArgc--;
    }

    // In addition to the formal arguments, we must also push |this|.
    // When calling a constructor, we must also push |newTarget|.
    uint32_t numArguments = actualArgc + 1 + pushedNewTarget;

    // Align the stack based on the number of arguments.
    size_t afterFrameSize =
        numArguments * sizeof(Value) + JitFrameLayout::Size();
    if (!maybeWritePadding(JitStackAlignment, afterFrameSize, "Padding")) {
      return false;
    }

    // Copy the arguments and |this| from the BaselineFrame, in reverse order.
    size_t valueSlot = blFrame()->numValueSlots(frameSize) - 1;
    size_t calleeSlot = valueSlot - numArguments;

    for (size_t i = valueSlot; i > calleeSlot; i--) {
      Value v = *blFrame()->valueSlot(i);
      if (!writeValue(v, "ArgVal")) {
        return false;
      }
    }

    callee = *blFrame()->valueSlot(calleeSlot);
  }

  // In case these arguments need to be copied on the stack again for a
  // rectifier frame, save the framePushed values here for later use.
  size_t endOfBaselineStubArgs = framePushed();

  // Push callee token (must be a JS Function)
  JitSpew(JitSpew_BaselineBailouts, "      Callee = %016" PRIx64,
          callee.asRawBits());

  JSFunction* calleeFun = &callee.toObject().as<JSFunction>();
  if (!writePtr(CalleeToToken(calleeFun, pushedNewTarget), "CalleeToken")) {
    return false;
  }
  const ICEntry& icScriptEntry = icScript_->icEntryFromPCOffset(pcOff);
  ICFallbackStub* icScriptFallback =
      icScript_->fallbackStubForICEntry(&icScriptEntry);
  setNextCallee(calleeFun, icScriptFallback->trialInliningState());

  // Push BaselineStub frame descriptor
  size_t baselineStubFrameDescr =
      MakeFrameDescriptorForJitCall(FrameType::BaselineStub, actualArgc);
  if (!writeWord(baselineStubFrameDescr, "Descriptor")) {
    return false;
  }

  // Push return address into ICCall_Scripted stub, immediately after the call.
  void* baselineCallReturnAddr = getStubReturnAddress();
  MOZ_ASSERT(baselineCallReturnAddr);
  if (!writePtr(baselineCallReturnAddr, "ReturnAddr")) {
    return false;
  }

  // The stack must be aligned after the callee pushes the frame pointer.
  MOZ_ASSERT((framePushed() + sizeof(void*)) % JitStackAlignment == 0);

  // Build a rectifier frame if necessary
  if (actualArgc < calleeFun->nargs() &&
      !buildRectifierFrame(actualArgc, endOfBaselineStubArgs)) {
    return false;
  }

  return true;
}

bool BaselineStackBuilder::buildRectifierFrame(uint32_t actualArgc,
                                               size_t endOfBaselineStubArgs) {
  // Push a reconstructed rectifier frame.
  // +===============+
  // |   Padding?    |
  // +---------------+
  // |  UndefinedU   |
  // +---------------+
  // |     ...       |
  // +---------------+
  // |  Undefined0   |
  // +---------------+
  // |     ArgA      |
  // +---------------+
  // |     ...       |
  // +---------------+
  // |     Arg0      |
  // +---------------+
  // |     ThisV     |
  // +---------------+
  // |  CalleeToken  |
  // +---------------+
  // |  Descr(Rect)  |
  // +---------------+
  // |  ReturnAddr   |
  // +===============+

  JitSpew(JitSpew_BaselineBailouts, "      [RECTIFIER FRAME]");
  bool pushedNewTarget = IsConstructPC(pc_);

  if (!writePtr(prevFramePtr(), "PrevFramePtr")) {
    return false;
  }
  prevFramePtr_ = virtualPointerAtStackOffset(0);

  // Align the stack based on the number of arguments.
  size_t afterFrameSize =
      (nextCallee()->nargs() + 1 + pushedNewTarget) * sizeof(Value) +
      RectifierFrameLayout::Size();
  if (!maybeWritePadding(JitStackAlignment, afterFrameSize, "Padding")) {
    return false;
  }

  // Copy new.target, if necessary.
  if (pushedNewTarget) {
    size_t newTargetOffset = (framePushed() - endOfBaselineStubArgs) +
                             (actualArgc + 1) * sizeof(Value);
    Value newTargetValue = *valuePointerAtStackOffset(newTargetOffset);
    if (!writeValue(newTargetValue, "CopiedNewTarget")) {
      return false;
    }
  }

  // Push undefined for missing arguments.
  for (unsigned i = 0; i < (nextCallee()->nargs() - actualArgc); i++) {
    if (!writeValue(UndefinedValue(), "FillerVal")) {
      return false;
    }
  }

  // Copy arguments + thisv from BaselineStub frame.
  if (!subtract((actualArgc + 1) * sizeof(Value), "CopiedArgs")) {
    return false;
  }
  BufferPointer<uint8_t> stubArgsEnd =
      pointerAtStackOffset<uint8_t>(framePushed() - endOfBaselineStubArgs);
  JitSpew(JitSpew_BaselineBailouts, "      MemCpy from %p", stubArgsEnd.get());
  memcpy(pointerAtStackOffset<uint8_t>(0).get(), stubArgsEnd.get(),
         (actualArgc + 1) * sizeof(Value));

  // Push calleeToken again.
  if (!writePtr(CalleeToToken(nextCallee(), pushedNewTarget), "CalleeToken")) {
    return false;
  }

  // Push rectifier frame descriptor
  size_t rectifierFrameDescr =
      MakeFrameDescriptorForJitCall(FrameType::Rectifier, actualArgc);
  if (!writeWord(rectifierFrameDescr, "Descriptor")) {
    return false;
  }

  // Push return address into the ArgumentsRectifier code, immediately after the
  // ioncode call.
  void* rectReturnAddr =
      cx_->runtime()->jitRuntime()->getArgumentsRectifierReturnAddr().value;
  MOZ_ASSERT(rectReturnAddr);
  if (!writePtr(rectReturnAddr, "ReturnAddr")) {
    return false;
  }

  // The stack must be aligned after the callee pushes the frame pointer.
  MOZ_ASSERT((framePushed() + sizeof(void*)) % JitStackAlignment == 0);

  return true;
}

bool BaselineStackBuilder::finishLastFrame() {
  const BaselineInterpreter& baselineInterp =
      cx_->runtime()->jitRuntime()->baselineInterpreter();

  setResumeFramePtr(prevFramePtr());

  // Compute the native address (within the Baseline Interpreter) that we will
  // resume at and initialize the frame's interpreter fields.
  uint8_t* resumeAddr;
  if (isPrologueBailout()) {
    JitSpew(JitSpew_BaselineBailouts, "      Resuming into prologue.");
    MOZ_ASSERT(pc_ == script_->code());
    blFrame()->setInterpreterFieldsForPrologue(script_);
    resumeAddr = baselineInterp.bailoutPrologueEntryAddr();
  } else if (propagatingIonExceptionForDebugMode()) {
    // When propagating an exception for debug mode, set the
    // resume pc to the throwing pc, so that Debugger hooks report
    // the correct pc offset of the throwing op instead of its
    // successor.
    jsbytecode* throwPC = script_->offsetToPC(iter_.pcOffset());
    blFrame()->setInterpreterFields(script_, throwPC);
    resumeAddr = baselineInterp.interpretOpAddr().value;
  } else {
    jsbytecode* resumePC = getResumePC();
    blFrame()->setInterpreterFields(script_, resumePC);
    resumeAddr = baselineInterp.interpretOpAddr().value;
  }
  setResumeAddr(resumeAddr);
  JitSpew(JitSpew_BaselineBailouts, "      Set resumeAddr=%p", resumeAddr);

  if (cx_->runtime()->geckoProfiler().enabled()) {
    // Register bailout with profiler.
    const char* filename = script_->filename();
    if (filename == nullptr) {
      filename = "<unknown>";
    }
    unsigned len = strlen(filename) + 200;
    UniqueChars buf(js_pod_malloc<char>(len));
    if (buf == nullptr) {
      ReportOutOfMemory(cx_);
      return false;
    }
    snprintf(buf.get(), len, "%s %s %s on line %u of %s:%u",
             BailoutKindString(bailoutKind()), resumeAfter() ? "after" : "at",
             CodeName(op_), PCToLineNumber(script_, pc_), filename,
             script_->lineno());
    cx_->runtime()->geckoProfiler().markEvent("Bailout", buf.get());
  }

  return true;
}

#ifdef DEBUG
// The |envChain| slot must not be optimized out if the currently
// active scope requires any EnvironmentObjects beyond what is
// available at body scope. This checks that scope chain does not
// require any such EnvironmentObjects.
// See also: |CompileInfo::isObservableFrameSlot|
bool BaselineStackBuilder::envChainSlotCanBeOptimized() {
  jsbytecode* pc = script_->offsetToPC(iter_.pcOffset());
  Scope* scopeIter = script_->innermostScope(pc);
  while (scopeIter != script_->bodyScope()) {
    if (!scopeIter || scopeIter->hasEnvironment()) {
      return false;
    }
    scopeIter = scopeIter->enclosing();
  }
  return true;
}

bool jit::AssertBailoutStackDepth(JSContext* cx, JSScript* script,
                                  jsbytecode* pc, ResumeMode mode,
                                  uint32_t exprStackSlots) {
  if (IsResumeAfter(mode)) {
    pc = GetNextPc(pc);
  }

  uint32_t expectedDepth;
  bool reachablePC;
  if (!ReconstructStackDepth(cx, script, pc, &expectedDepth, &reachablePC)) {
    return false;
  }
  if (!reachablePC) {
    return true;
  }

  JSOp op = JSOp(*pc);

  if (mode == ResumeMode::InlinedFunCall) {
    // For inlined fun.call(this, ...); the reconstructed stack depth will
    // include the |this|, but the exprStackSlots won't.
    // Exception: if there are no arguments, the depths do match.
    MOZ_ASSERT(IsInvokeOp(op));
    if (GET_ARGC(pc) > 0) {
      MOZ_ASSERT(expectedDepth == exprStackSlots + 1);
    } else {
      MOZ_ASSERT(expectedDepth == exprStackSlots);
    }
    return true;
  }

  if (mode == ResumeMode::InlinedAccessor) {
    // Accessors coming out of ion are inlined via a complete lie perpetrated by
    // the compiler internally. Ion just rearranges the stack, and pretends that
    // it looked like a call all along.
    // This means that the depth is actually one *more* than expected by the
    // interpreter, as there is now a JSFunction, |this| and [arg], rather than
    // the expected |this| and [arg].
    // If the inlined accessor is a GetElem operation, the numbers do match, but
    // that's just because GetElem expects one more item on the stack. Note that
    // none of that was pushed, but it's still reflected in exprStackSlots.
    MOZ_ASSERT(IsIonInlinableGetterOrSetterOp(op));
    if (IsGetElemOp(op)) {
      MOZ_ASSERT(exprStackSlots == expectedDepth);
    } else {
      MOZ_ASSERT(exprStackSlots == expectedDepth + 1);
    }
    return true;
  }

  // In all other cases, the depth must match.
  MOZ_ASSERT(exprStackSlots == expectedDepth);
  return true;
}

bool BaselineStackBuilder::validateFrame() {
  const uint32_t frameSize = framePushed();
  blFrame()->setDebugFrameSize(frameSize);
  JitSpew(JitSpew_BaselineBailouts, "      FrameSize=%u", frameSize);

  // debugNumValueSlots() is based on the frame size, do some sanity checks.
  MOZ_ASSERT(blFrame()->debugNumValueSlots() >= script_->nfixed());
  MOZ_ASSERT(blFrame()->debugNumValueSlots() <= script_->nslots());

  uint32_t expectedSlots = exprStackSlots();
  if (resumingInFinallyBlock()) {
    // If we are resuming in a finally block, we push three extra values on the
    // stack (the exception, the exception stack, and |throwing|), so the depth
    // at the resume PC should be the depth at the fault PC plus three.
    expectedSlots += 3;
  }
  return AssertBailoutStackDepth(cx_, script_, pc_, resumeMode(),
                                 expectedSlots);
}
#endif

void* BaselineStackBuilder::getStubReturnAddress() {
  const BaselineICFallbackCode& code =
      cx_->runtime()->jitRuntime()->baselineICFallbackCode();

  if (IsGetPropOp(op_)) {
    return code.bailoutReturnAddr(BailoutReturnKind::GetProp);
  }
  if (IsSetPropOp(op_)) {
    return code.bailoutReturnAddr(BailoutReturnKind::SetProp);
  }
  if (IsGetElemOp(op_)) {
    return code.bailoutReturnAddr(BailoutReturnKind::GetElem);
  }

  // This should be a call op of some kind, now.
  MOZ_ASSERT(IsInvokeOp(op_) && !IsSpreadOp(op_));
  if (IsConstructOp(op_)) {
    return code.bailoutReturnAddr(BailoutReturnKind::New);
  }
  return code.bailoutReturnAddr(BailoutReturnKind::Call);
}

static inline jsbytecode* GetNextNonLoopHeadPc(jsbytecode* pc) {
  JSOp op = JSOp(*pc);
  switch (op) {
    case JSOp::Goto:
      return pc + GET_JUMP_OFFSET(pc);

    case JSOp::LoopHead:
    case JSOp::Nop:
      return GetNextPc(pc);

    default:
      return pc;
  }
}

// Returns the pc to resume execution at in Baseline after a bailout.
jsbytecode* BaselineStackBuilder::getResumePC() {
  if (resumeAfter()) {
    return GetNextPc(pc_);
  }

  // If we are resuming at a LoopHead op, resume at the next op to avoid
  // a bailout -> enter Ion -> bailout loop with --ion-eager.
  //
  // Cycles can cause the loop below to not terminate. Empty loops are one
  // such example:
  //
  //   L: loophead
  //      goto L
  //
  // We do cycle detection below with the "tortoise and the hare" algorithm.
  jsbytecode* slowerPc = pc_;
  jsbytecode* fasterPc = pc_;
  while (true) {
    // Advance fasterPc twice as fast as slowerPc.
    slowerPc = GetNextNonLoopHeadPc(slowerPc);
    fasterPc = GetNextNonLoopHeadPc(fasterPc);
    fasterPc = GetNextNonLoopHeadPc(fasterPc);

    // Break on cycles or at the end of goto sequences.
    if (fasterPc == slowerPc) {
      break;
    }
  }

  return slowerPc;
}

bool BaselineStackBuilder::isPrologueBailout() {
  // If we are propagating an exception for debug mode, we will not resume
  // into baseline code, but instead into HandleExceptionBaseline (i.e.,
  // never before the prologue).
  return iter_.pcOffset() == 0 && !iter_.resumeAfter() &&
         !propagatingIonExceptionForDebugMode();
}

// Build a baseline stack frame.
bool BaselineStackBuilder::buildOneFrame() {
  // Build a baseline frame:
  // +===============+
  // | PrevFramePtr  | <-- initFrame()
  // +---------------+
  // |   Baseline    | <-- buildBaselineFrame()
  // |    Frame      |
  // +---------------+
  // |    Fixed0     | <-- buildFixedSlots()
  // +---------------+
  // |     ...       |
  // +---------------+
  // |    FixedF     |
  // +---------------+
  // |    Stack0     | <-- buildExpressionStack() -or- fixupCallerArgs()
  // +---------------+
  // |     ...       |
  // +---------------+     If we are building the frame in which we will
  // |    StackS     | <-- resume, we stop here.
  // +---------------+     finishLastFrame() sets up the interpreter fields.
  // .               .
  // .               .
  // .               . <-- If there are additional frames inlined into this
  // |  Descr(BLJS)  |     one, we finish this frame. We generate a stub
  // +---------------+     frame (and maybe also a rectifier frame) between
  // |  ReturnAddr   |     this frame and the inlined frame.
  // +===============+     See: prepareForNextFrame()

  if (!initFrame()) {
    return false;
  }

  if (!buildBaselineFrame()) {
    return false;
  }

  if (fun_ && !buildArguments()) {
    return false;
  }

  if (!buildFixedSlots()) {
    return false;
  }

  bool fixedUp = false;
  RootedValueVector savedCallerArgs(cx_);
  if (iter_.moreFrames() && !fixUpCallerArgs(&savedCallerArgs, &fixedUp)) {
    return false;
  }

  if (!fixedUp) {
    if (!buildExpressionStack()) {
      return false;
    }
    if (resumingInFinallyBlock() && !buildFinallyException()) {
      return false;
    }
  }

#ifdef DEBUG
  if (!validateFrame()) {
    return false;
  }
#endif

#ifdef JS_JITSPEW
  const uint32_t pcOff = script_->pcToOffset(pc());
  JitSpew(JitSpew_BaselineBailouts,
          "      Resuming %s pc offset %d (op %s) (line %u) of %s:%u:%u",
          resumeAfter() ? "after" : "at", (int)pcOff, CodeName(op_),
          PCToLineNumber(script_, pc()), script_->filename(), script_->lineno(),
          script_->column().oneOriginValue());
  JitSpew(JitSpew_BaselineBailouts, "      Bailout kind: %s",
          BailoutKindString(bailoutKind()));
#endif

  // If this was the last inline frame, or we are bailing out to a catch or
  // finally block in this frame, then unpacking is almost done.
  if (done()) {
    return finishLastFrame();
  }

  // Otherwise, this is an outer frame for an inlined call or
  // accessor. We will be building an inner frame. Before that,
  // we must create a stub frame, and potentially a rectifier frame.
  return prepareForNextFrame(savedCallerArgs);
}

bool jit::BailoutIonToBaseline(JSContext* cx, JitActivation* activation,
                               const JSJitFrameIter& iter,
                               BaselineBailoutInfo** bailoutInfo,
                               const ExceptionBailoutInfo* excInfo,
                               BailoutReason reason) {
  MOZ_ASSERT(bailoutInfo != nullptr);
  MOZ_ASSERT(*bailoutInfo == nullptr);
  MOZ_ASSERT(iter.isBailoutJS());

  // Caller should have saved the exception while we perform the bailout.
  MOZ_ASSERT(!cx->isExceptionPending());

  // Ion bailout can fail due to overrecursion and OOM. In such cases we
  // cannot honor any further Debugger hooks on the frame, and need to
  // ensure that its Debugger.Frame entry is cleaned up.
  auto guardRemoveRematerializedFramesFromDebugger =
      mozilla::MakeScopeExit([&] {
        activation->removeRematerializedFramesFromDebugger(cx, iter.fp());
      });

  // Always remove the RInstructionResults from the JitActivation, even in
  // case of failures as the stack frame is going away after the bailout.
  auto removeIonFrameRecovery = mozilla::MakeScopeExit(
      [&] { activation->removeIonFrameRecovery(iter.jsFrame()); });

  // The caller of the top frame must be one of the following:
  //      IonJS - Ion calling into Ion.
  //      BaselineStub - Baseline calling into Ion.
  //      Entry / WasmToJSJit - Interpreter or other (wasm) calling into Ion.
  //      Rectifier - Arguments rectifier calling into Ion.
  //      BaselineJS - Resume'd Baseline, then likely OSR'd into Ion.
  MOZ_ASSERT(iter.isBailoutJS());
#if defined(DEBUG) || defined(JS_JITSPEW)
  FrameType prevFrameType = iter.prevType();
  MOZ_ASSERT(JSJitFrameIter::isEntry(prevFrameType) ||
             prevFrameType == FrameType::IonJS ||
             prevFrameType == FrameType::BaselineStub ||
             prevFrameType == FrameType::Rectifier ||
             prevFrameType == FrameType::TrampolineNative ||
             prevFrameType == FrameType::IonICCall ||
             prevFrameType == FrameType::BaselineJS ||
             prevFrameType == FrameType::BaselineInterpreterEntry);
#endif

  // All incoming frames are going to look like this:
  //
  //      +---------------+
  //      |     ...       |
  //      +---------------+
  //      |     Args      |
  //      |     ...       |
  //      +---------------+
  //      |    ThisV      |
  //      +---------------+
  //      |  ActualArgC   |
  //      +---------------+
  //      |  CalleeToken  |
  //      +---------------+
  //      |  Descriptor   |
  //      +---------------+
  //      |  ReturnAddr   |
  //      +---------------+
  //      |    |||||      | <---- Overwrite starting here.
  //      |    |||||      |
  //      |    |||||      |
  //      +---------------+

  JitSpew(JitSpew_BaselineBailouts,
          "Bailing to baseline %s:%u:%u (IonScript=%p) (FrameType=%d)",
          iter.script()->filename(), iter.script()->lineno(),
          iter.script()->column().oneOriginValue(), (void*)iter.ionScript(),
          (int)prevFrameType);

  if (excInfo) {
    if (excInfo->catchingException()) {
      JitSpew(JitSpew_BaselineBailouts, "Resuming in catch or finally block");
    }
    if (excInfo->propagatingIonExceptionForDebugMode()) {
      JitSpew(JitSpew_BaselineBailouts, "Resuming in-place for debug mode");
    }
  }

  JitSpew(JitSpew_BaselineBailouts,
          "  Reading from snapshot offset %u size %zu", iter.snapshotOffset(),
          iter.ionScript()->snapshotsListSize());

  iter.script()->updateJitCodeRaw(cx->runtime());

  // Under a bailout, there is no need to invalidate the frame after
  // evaluating the recover instruction, as the invalidation is only needed in
  // cases where the frame is introspected ahead of the bailout.
  MaybeReadFallback recoverBailout(cx, activation, &iter,
                                   MaybeReadFallback::Fallback_DoNothing);

  // Ensure that all value locations are readable from the SnapshotIterator.
  // Get the RInstructionResults from the JitActivation if the frame got
  // recovered ahead of the bailout.
  SnapshotIterator snapIter(iter, activation->bailoutData()->machineState());
  if (!snapIter.initInstructionResults(recoverBailout)) {
    return false;
  }

#ifdef TRACK_SNAPSHOTS
  snapIter.spewBailingFrom();
#endif

  BaselineStackBuilder builder(cx, iter, snapIter, excInfo, reason);
  if (!builder.init()) {
    return false;
  }

  JitSpew(JitSpew_BaselineBailouts, "  Incoming frame ptr = %p",
          builder.startFrame());
  if (iter.maybeCallee()) {
    JitSpew(JitSpew_BaselineBailouts, "  Callee function (%s:%u:%u)",
            iter.script()->filename(), iter.script()->lineno(),
            iter.script()->column().oneOriginValue());
  } else {
    JitSpew(JitSpew_BaselineBailouts, "  No callee!");
  }

  if (iter.isConstructing()) {
    JitSpew(JitSpew_BaselineBailouts, "  Constructing!");
  } else {
    JitSpew(JitSpew_BaselineBailouts, "  Not constructing!");
  }

  JitSpew(JitSpew_BaselineBailouts, "  Restoring frames:");

  while (true) {
    // Skip recover instructions as they are already recovered by
    // |initInstructionResults|.
    snapIter.settleOnFrame();

    JitSpew(JitSpew_BaselineBailouts, "    FrameNo %zu", builder.frameNo());

    if (!builder.buildOneFrame()) {
      MOZ_ASSERT(cx->isExceptionPending());
      return false;
    }

    if (builder.done()) {
      break;
    }

    builder.nextFrame();
  }
  JitSpew(JitSpew_BaselineBailouts, "  Done restoring frames");

  BailoutKind bailoutKind = builder.bailoutKind();

  if (!builder.outermostFrameFormals().empty()) {
    // Set the first frame's formals, see the comment in InitFromBailout.
    Value* argv = builder.startFrame()->actualArgs();
    mozilla::PodCopy(argv, builder.outermostFrameFormals().begin(),
                     builder.outermostFrameFormals().length());
  }

  // Do stack check.
  bool overRecursed = false;
  BaselineBailoutInfo* info = builder.info();
  size_t numBytesToPush = info->copyStackTop - info->copyStackBottom;
  MOZ_ASSERT((numBytesToPush % sizeof(uintptr_t)) == 0);
  uint8_t* newsp = info->incomingStack - numBytesToPush;
#ifdef JS_SIMULATOR
  if (Simulator::Current()->overRecursed(uintptr_t(newsp))) {
    overRecursed = true;
  }
#else
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.checkWithStackPointerDontReport(cx, newsp)) {
    overRecursed = true;
  }
#endif
  if (overRecursed) {
    JitSpew(JitSpew_BaselineBailouts, "  Overrecursion check failed!");
    ReportOverRecursed(cx);
    return false;
  }

  // Take the reconstructed baseline stack so it doesn't get freed when builder
  // destructs.
  info = builder.takeBuffer();
  info->numFrames = builder.frameNo() + 1;
  info->bailoutKind.emplace(bailoutKind);
  *bailoutInfo = info;
  guardRemoveRematerializedFramesFromDebugger.release();
  return true;
}

static void InvalidateAfterBailout(JSContext* cx, HandleScript outerScript,
                                   const char* reason) {
  // In some cases, the computation of recover instruction can invalidate the
  // Ion script before we reach the end of the bailout. Thus, if the outer
  // script no longer have any Ion script attached, then we just skip the
  // invalidation.
  //
  // For example, such case can happen if the template object for an unboxed
  // objects no longer match the content of its properties (see Bug 1174547)
  if (!outerScript->hasIonScript()) {
    JitSpew(JitSpew_BaselineBailouts, "Ion script is already invalidated");
    return;
  }

  // Record a invalidation for this script in the jit hints map
  if (cx->runtime()->jitRuntime()->hasJitHintsMap()) {
    JitHintsMap* jitHints = cx->runtime()->jitRuntime()->getJitHintsMap();
    jitHints->recordInvalidation(outerScript);
  }

  MOZ_ASSERT(!outerScript->ionScript()->invalidated());

  JitSpew(JitSpew_BaselineBailouts, "Invalidating due to %s", reason);
  Invalidate(cx, outerScript);
}

static void HandleLexicalCheckFailure(JSContext* cx, HandleScript outerScript,
                                      HandleScript innerScript) {
  JitSpew(JitSpew_IonBailouts,
          "Lexical check failure %s:%u:%u, inlined into %s:%u:%u",
          innerScript->filename(), innerScript->lineno(),
          innerScript->column().oneOriginValue(), outerScript->filename(),
          outerScript->lineno(), outerScript->column().oneOriginValue());

  if (!innerScript->failedLexicalCheck()) {
    innerScript->setFailedLexicalCheck();
  }

  InvalidateAfterBailout(cx, outerScript, "lexical check failure");
  if (innerScript->hasIonScript()) {
    Invalidate(cx, innerScript);
  }
}

static bool CopyFromRematerializedFrame(JSContext* cx, JitActivation* act,
                                        uint8_t* fp, size_t inlineDepth,
                                        BaselineFrame* frame) {
  RematerializedFrame* rematFrame =
      act->lookupRematerializedFrame(fp, inlineDepth);

  // We might not have rematerialized a frame if the user never requested a
  // Debugger.Frame for it.
  if (!rematFrame) {
    return true;
  }

  MOZ_ASSERT(rematFrame->script() == frame->script());
  MOZ_ASSERT(rematFrame->numActualArgs() == frame->numActualArgs());

  frame->setEnvironmentChain(rematFrame->environmentChain());

  if (frame->isFunctionFrame()) {
    frame->thisArgument() = rematFrame->thisArgument();
  }

  for (unsigned i = 0; i < frame->numActualArgs(); i++) {
    frame->argv()[i] = rematFrame->argv()[i];
  }

  for (size_t i = 0; i < frame->script()->nfixed(); i++) {
    *frame->valueSlot(i) = rematFrame->locals()[i];
  }

  if (frame->script()->noScriptRval()) {
    frame->setReturnValue(UndefinedValue());
  } else {
    frame->setReturnValue(rematFrame->returnValue());
  }

  // Don't copy over the hasCachedSavedFrame bit. The new BaselineFrame we're
  // building has a different AbstractFramePtr, so it won't be found in the
  // LiveSavedFrameCache if we look there.

  JitSpew(JitSpew_BaselineBailouts,
          "  Copied from rematerialized frame at (%p,%zu)", fp, inlineDepth);

  // Propagate the debuggee frame flag. For the case where the Debugger did
  // not rematerialize an Ion frame, the baseline frame has its debuggee
  // flag set iff its script is considered a debuggee. See the debuggee case
  // in InitFromBailout.
  if (rematFrame->isDebuggee()) {
    frame->setIsDebuggee();
    return DebugAPI::handleIonBailout(cx, rematFrame, frame);
  }

  return true;
}

enum class BailoutAction {
  InvalidateImmediately,
  InvalidateIfFrequent,
  DisableIfFrequent,
  NoAction
};

bool jit::FinishBailoutToBaseline(BaselineBailoutInfo* bailoutInfoArg) {
  JitSpew(JitSpew_BaselineBailouts, "  Done restoring frames");

  JSContext* cx = TlsContext.get();
  // Use UniquePtr to free the bailoutInfo before we return, and root it for
  // the tempId field.
  Rooted<UniquePtr<BaselineBailoutInfo>> bailoutInfo(cx, bailoutInfoArg);
  bailoutInfoArg = nullptr;

  MOZ_DIAGNOSTIC_ASSERT(*bailoutInfo->bailoutKind != BailoutKind::Unreachable);

  // jit::Bailout(), jit::InvalidationBailout(), and jit::HandleException()
  // should have reset the counter to zero.
  MOZ_ASSERT(!cx->isInUnsafeRegion());

  BaselineFrame* topFrame = GetTopBaselineFrame(cx);

  // We have to get rid of the rematerialized frame, whether it is
  // restored or unwound.
  uint8_t* incomingStack = bailoutInfo->incomingStack;
  auto guardRemoveRematerializedFramesFromDebugger =
      mozilla::MakeScopeExit([&] {
        JitActivation* act = cx->activation()->asJit();
        act->removeRematerializedFramesFromDebugger(cx, incomingStack);
      });

  // Ensure the frame has a call object if it needs one.
  if (!EnsureHasEnvironmentObjects(cx, topFrame)) {
    return false;
  }

  // Create arguments objects for bailed out frames, to maintain the invariant
  // that script->needsArgsObj() implies frame->hasArgsObj().
  RootedScript innerScript(cx, nullptr);
  RootedScript outerScript(cx, nullptr);

  MOZ_ASSERT(cx->currentlyRunningInJit());
  JSJitFrameIter iter(cx->activation()->asJit());
  uint8_t* outerFp = nullptr;

  // Iter currently points at the exit frame.  Get the previous frame
  // (which must be a baseline frame), and set it as the last profiling
  // frame.
  if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(
          cx->runtime())) {
    MOZ_ASSERT(iter.prevType() == FrameType::BaselineJS);
    JitFrameLayout* fp = reinterpret_cast<JitFrameLayout*>(iter.prevFp());
    cx->jitActivation->setLastProfilingFrame(fp);
  }

  uint32_t numFrames = bailoutInfo->numFrames;
  MOZ_ASSERT(numFrames > 0);

  uint32_t frameno = 0;
  while (frameno < numFrames) {
    MOZ_ASSERT(!iter.isIonJS());

    if (iter.isBaselineJS()) {
      BaselineFrame* frame = iter.baselineFrame();
      MOZ_ASSERT(frame->script()->hasBaselineScript());

      // If the frame doesn't even have a env chain set yet, then it's resuming
      // into the the prologue before the env chain is initialized.  Any
      // necessary args object will also be initialized there.
      if (frame->environmentChain() && frame->script()->needsArgsObj()) {
        ArgumentsObject* argsObj;
        if (frame->hasArgsObj()) {
          argsObj = &frame->argsObj();
        } else {
          argsObj = ArgumentsObject::createExpected(cx, frame);
          if (!argsObj) {
            return false;
          }
        }

        // The arguments is a local binding and needsArgsObj does not
        // check if it is clobbered. Ensure that the local binding
        // restored during bailout before storing the arguments object
        // to the slot.
        RootedScript script(cx, frame->script());
        SetFrameArgumentsObject(cx, frame, script, argsObj);
      }

      if (frameno == 0) {
        innerScript = frame->script();
      }

      if (frameno == numFrames - 1) {
        outerScript = frame->script();
        outerFp = iter.fp();
        MOZ_ASSERT(outerFp == incomingStack);
      }

      frameno++;
    }

    ++iter;
  }

  MOZ_ASSERT(innerScript);
  MOZ_ASSERT(outerScript);
  MOZ_ASSERT(outerFp);

  // If we rematerialized Ion frames due to debug mode toggling, copy their
  // values into the baseline frame. We need to do this even when debug mode
  // is off, as we should respect the mutations made while debug mode was
  // on.
  JitActivation* act = cx->activation()->asJit();
  if (act->hasRematerializedFrame(outerFp)) {
    JSJitFrameIter iter(act);
    size_t inlineDepth = numFrames;
    bool ok = true;
    while (inlineDepth > 0) {
      if (iter.isBaselineJS()) {
        // We must attempt to copy all rematerialized frames over,
        // even if earlier ones failed, to invoke the proper frame
        // cleanup in the Debugger.
        if (!CopyFromRematerializedFrame(cx, act, outerFp, --inlineDepth,
                                         iter.baselineFrame())) {
          ok = false;
        }
      }
      ++iter;
    }

    if (!ok) {
      return false;
    }

    // After copying from all the rematerialized frames, remove them from
    // the table to keep the table up to date.
    guardRemoveRematerializedFramesFromDebugger.release();
    act->removeRematerializedFrame(outerFp);
  }

  // If we are unwinding for an exception, we need to unwind scopes.
  // See |SettleOnTryNote|
  if (bailoutInfo->faultPC) {
    EnvironmentIter ei(cx, topFrame, bailoutInfo->faultPC);
    UnwindEnvironment(cx, ei, bailoutInfo->tryPC);
  }

  BailoutKind bailoutKind = *bailoutInfo->bailoutKind;
  JitSpew(JitSpew_BaselineBailouts,
          "  Restored outerScript=(%s:%u:%u,%u) innerScript=(%s:%u:%u,%u) "
          "(bailoutKind=%u)",
          outerScript->filename(), outerScript->lineno(),
          outerScript->column().oneOriginValue(), outerScript->getWarmUpCount(),
          innerScript->filename(), innerScript->lineno(),
          innerScript->column().oneOriginValue(), innerScript->getWarmUpCount(),
          (unsigned)bailoutKind);

  BailoutAction action = BailoutAction::InvalidateImmediately;
  DebugOnly<bool> saveFailedICHash = false;
  switch (bailoutKind) {
    case BailoutKind::TranspiledCacheIR:
      // A transpiled guard failed. If this happens often enough, we will
      // invalidate and recompile.
      action = BailoutAction::InvalidateIfFrequent;
      saveFailedICHash = true;
      break;

    case BailoutKind::MonomorphicInlinedStubFolding:
      action = BailoutAction::InvalidateIfFrequent;
      saveFailedICHash = true;
      if (innerScript != outerScript) {
        // In the case where this instruction comes from a monomorphic-inlined
        // ICScript, we need to ensure that we note the connection between the
        // inner script and the outer script, so that we can properly track if
        // we add a new case to the folded stub and avoid invalidating the
        // outer script.
        cx->zone()->jitZone()->noteStubFoldingBailout(innerScript, outerScript);
      }
      break;

    case BailoutKind::SpeculativePhi:
      // A value of an unexpected type flowed into a phi.
      MOZ_ASSERT(!outerScript->hadSpeculativePhiBailout());
      if (!outerScript->hasIonScript() ||
          outerScript->ionScript()->numFixableBailouts() == 0) {
        outerScript->setHadSpeculativePhiBailout();
      }
      InvalidateAfterBailout(cx, outerScript, "phi specialization failure");
      break;

    case BailoutKind::TypePolicy:
      // A conversion inserted by a type policy failed.
      // We will invalidate and disable recompilation if this happens too often.
      action = BailoutAction::DisableIfFrequent;
      break;

    case BailoutKind::LICM:
      // LICM may cause spurious bailouts by hoisting unreachable
      // guards past branches.  To prevent bailout loops, when an
      // instruction hoisted by LICM bails out, we update the
      // IonScript and resume in baseline. If the guard would have
      // been executed anyway, then we will hit the baseline fallback,
      // and call noteBaselineFallback. If that does not happen,
      // then the next time we reach this point, we will disable LICM
      // for this script.
      MOZ_ASSERT(!outerScript->hadLICMInvalidation());
      if (outerScript->hasIonScript()) {
        switch (outerScript->ionScript()->licmState()) {
          case IonScript::LICMState::NeverBailed:
            outerScript->ionScript()->setHadLICMBailout();
            action = BailoutAction::NoAction;
            break;
          case IonScript::LICMState::Bailed:
            outerScript->setHadLICMInvalidation();
            InvalidateAfterBailout(cx, outerScript, "LICM failure");
            break;
          case IonScript::LICMState::BailedAndHitFallback:
            // This bailout is not due to LICM. Treat it like a
            // regular TranspiledCacheIR bailout.
            action = BailoutAction::InvalidateIfFrequent;
            break;
        }
      }
      break;

    case BailoutKind::InstructionReordering:
      // An instruction moved up by instruction reordering bailed out.
      outerScript->setHadReorderingBailout();
      action = BailoutAction::InvalidateIfFrequent;
      break;

    case BailoutKind::HoistBoundsCheck:
      // An instruction hoisted or generated by tryHoistBoundsCheck bailed out.
      MOZ_ASSERT(!outerScript->failedBoundsCheck());
      outerScript->setFailedBoundsCheck();
      InvalidateAfterBailout(cx, outerScript, "bounds check failure");
      break;

    case BailoutKind::EagerTruncation:
      // An eager truncation generated by range analysis bailed out.
      // To avoid bailout loops, we set a flag to avoid generating
      // eager truncations next time we recompile.
      MOZ_ASSERT(!outerScript->hadEagerTruncationBailout());
      outerScript->setHadEagerTruncationBailout();
      InvalidateAfterBailout(cx, outerScript, "eager range analysis failure");
      break;

    case BailoutKind::UnboxFolding:
      // An unbox that was hoisted to fold with a load bailed out.
      // To avoid bailout loops, we set a flag to avoid folding
      // loads with unboxes next time we recompile.
      MOZ_ASSERT(!outerScript->hadUnboxFoldingBailout());
      outerScript->setHadUnboxFoldingBailout();
      InvalidateAfterBailout(cx, outerScript, "unbox folding failure");
      break;

    case BailoutKind::TooManyArguments:
      // A funapply or spread call had more than JIT_ARGS_LENGTH_MAX arguments.
      // We will invalidate and disable recompilation if this happens too often.
      action = BailoutAction::DisableIfFrequent;
      break;

    case BailoutKind::DuringVMCall:
      if (cx->isExceptionPending()) {
        // We are bailing out to catch an exception. We will invalidate
        // and disable recompilation if this happens too often.
        action = BailoutAction::DisableIfFrequent;
      }
      break;

    case BailoutKind::Finally:
      // We are bailing out for a finally block. We will invalidate
      // and disable recompilation if this happens too often.
      action = BailoutAction::DisableIfFrequent;
      break;

    case BailoutKind::Inevitable:
    case BailoutKind::Debugger:
      // Do nothing.
      action = BailoutAction::NoAction;
      break;

    case BailoutKind::FirstExecution:
      // We reached an instruction that had not been executed yet at
      // the time we compiled. If this happens often enough, we will
      // invalidate and recompile.
      action = BailoutAction::InvalidateIfFrequent;
      saveFailedICHash = true;
      break;

    case BailoutKind::UninitializedLexical:
      HandleLexicalCheckFailure(cx, outerScript, innerScript);
      break;

    case BailoutKind::ThrowCheckIsObject:
      MOZ_ASSERT(!cx->isExceptionPending());
      return ThrowCheckIsObject(cx, CheckIsObjectKind::IteratorReturn);

    case BailoutKind::ThrowProxyTrapMustReportSameValue:
    case BailoutKind::ThrowProxyTrapMustReportUndefined: {
      MOZ_ASSERT(!cx->isExceptionPending());
      RootedId rootedId(cx, bailoutInfo->tempId);
      ScriptedProxyHandler::reportGetTrapValidationError(
          cx, rootedId,
          bailoutKind == BailoutKind::ThrowProxyTrapMustReportSameValue
              ? ScriptedProxyHandler::GetTrapValidationResult::
                    MustReportSameValue
              : ScriptedProxyHandler::GetTrapValidationResult::
                    MustReportUndefined);
      return false;
    }

    case BailoutKind::IonExceptionDebugMode:
      // Return false to resume in HandleException with reconstructed
      // baseline frame.
      return false;

    case BailoutKind::OnStackInvalidation:
      // The script has already been invalidated. There is nothing left to do.
      action = BailoutAction::NoAction;
      break;

    default:
      MOZ_CRASH("Unknown bailout kind!");
  }

#ifdef DEBUG
  if (MOZ_UNLIKELY(cx->runtime()->jitRuntime()->ionBailAfterEnabled())) {
    action = BailoutAction::NoAction;
  }
#endif

  if (outerScript->hasIonScript()) {
    IonScript* ionScript = outerScript->ionScript();
    switch (action) {
      case BailoutAction::InvalidateImmediately:
        // The IonScript should already have been invalidated.
        MOZ_ASSERT(false);
        break;
      case BailoutAction::InvalidateIfFrequent:
        ionScript->incNumFixableBailouts();
        if (ionScript->shouldInvalidate()) {
#ifdef DEBUG
          // To detect bailout loops, we save a hash of the CacheIR used to
          // compile this script, and assert that we don't recompile with the
          // exact same inputs.  Some of our bailout detection strategies, like
          // LICM and stub folding, rely on bailing out, updating some state
          // when we hit the baseline fallback, and using that information when
          // we invalidate. If the frequentBailoutThreshold is set too low, we
          // will instead invalidate the first time we bail out, so we don't
          // have the chance to make those decisions. That doesn't happen in
          // regular code, so we just skip bailout loop detection in that case.
          if (saveFailedICHash && !JitOptions.disableBailoutLoopCheck &&
              JitOptions.frequentBailoutThreshold > 1) {
            outerScript->jitScript()->setFailedICHash(ionScript->icHash());
          }
#endif
          InvalidateAfterBailout(cx, outerScript, "fixable bailouts");
        }
        break;
      case BailoutAction::DisableIfFrequent:
        ionScript->incNumUnfixableBailouts();
        if (ionScript->shouldInvalidateAndDisable()) {
          InvalidateAfterBailout(cx, outerScript, "unfixable bailouts");
          outerScript->disableIon();
        }
        break;
      case BailoutAction::NoAction:
        break;
    }
  }

  return true;
}
