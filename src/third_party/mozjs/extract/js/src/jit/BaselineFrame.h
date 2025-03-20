/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineFrame_h
#define jit_BaselineFrame_h

#include <algorithm>

#include "jit/CalleeToken.h"
#include "jit/JitFrames.h"
#include "jit/ScriptFromCalleeToken.h"
#include "vm/Stack.h"

namespace js {
namespace jit {

class ICEntry;
class ICScript;
class JSJitFrameIter;

// The stack looks like this, fp is the frame pointer:
//
// fp+y   arguments
// fp  => JitFrameLayout (frame header)
// fp-x   BaselineFrame
//        locals
//        stack values

class BaselineFrame {
 public:
  enum Flags : uint32_t {
    // The frame has a valid return value. See also InterpreterFrame::HAS_RVAL.
    HAS_RVAL = 1 << 0,

    // The frame is running in the Baseline interpreter instead of JIT.
    RUNNING_IN_INTERPRETER = 1 << 1,

    // An initial environment has been pushed on the environment chain for
    // function frames that need a CallObject or eval frames that need a
    // VarEnvironmentObject.
    HAS_INITIAL_ENV = 1 << 2,

    // Frame has an arguments object, argsObj_.
    HAS_ARGS_OBJ = 1 << 4,

    // See InterpreterFrame::PREV_UP_TO_DATE.
    PREV_UP_TO_DATE = 1 << 5,

    // Frame has execution observed by a Debugger.
    //
    // See comment above 'isDebuggee' in vm/Realm.h for explanation
    // of invariants of debuggee compartments, scripts, and frames.
    DEBUGGEE = 1 << 6,
  };

 protected:  // Silence Clang warning about unused private fields.
  // The fields below are only valid if RUNNING_IN_INTERPRETER.
  JSScript* interpreterScript_;
  jsbytecode* interpreterPC_;
  ICEntry* interpreterICEntry_;

  JSObject* envChain_;        // Environment chain (always initialized).
  ICScript* icScript_;        // IC script (initialized if Warp is enabled).
  ArgumentsObject* argsObj_;  // If HAS_ARGS_OBJ, the arguments object.

  // We need to split the Value into 2 fields of 32 bits, otherwise the C++
  // compiler may add some padding between the fields.
  uint32_t loScratchValue_;
  uint32_t hiScratchValue_;
  uint32_t flags_;
#ifdef DEBUG
  // Size of the frame. Stored in DEBUG builds when calling into C++. This is
  // BaselineFrame::Size() + the size of the local and expression stack Values.
  //
  // We don't store this in release builds because it's redundant with the frame
  // size computed from the frame pointers. In debug builds it's still useful
  // for assertions.
  uint32_t debugFrameSize_;
#else
  uint32_t unused_;
#endif
  uint32_t loReturnValue_;  // If HAS_RVAL, the frame's return value.
  uint32_t hiReturnValue_;

 public:
  [[nodiscard]] bool initForOsr(InterpreterFrame* fp, uint32_t numStackValues);

#ifdef DEBUG
  uint32_t debugFrameSize() const { return debugFrameSize_; }
  void setDebugFrameSize(uint32_t frameSize) { debugFrameSize_ = frameSize; }
#endif

  JSObject* environmentChain() const { return envChain_; }
  void setEnvironmentChain(JSObject* envChain) { envChain_ = envChain; }

  template <typename SpecificEnvironment>
  inline void pushOnEnvironmentChain(SpecificEnvironment& env);
  template <typename SpecificEnvironment>
  inline void popOffEnvironmentChain();
  inline void replaceInnermostEnvironment(EnvironmentObject& env);

  CalleeToken calleeToken() const { return framePrefix()->calleeToken(); }
  void replaceCalleeToken(CalleeToken token) {
    framePrefix()->replaceCalleeToken(token);
  }
  bool isConstructing() const {
    return CalleeTokenIsConstructing(calleeToken());
  }
  JSScript* script() const {
    return MaybeForwardedScriptFromCalleeToken(calleeToken());
  }
  JSFunction* callee() const { return CalleeTokenToFunction(calleeToken()); }
  Value calleev() const { return ObjectValue(*callee()); }

  size_t numValueSlots(size_t frameSize) const {
    MOZ_ASSERT(frameSize == debugFrameSize());

    MOZ_ASSERT(frameSize >= BaselineFrame::Size());
    frameSize -= BaselineFrame::Size();

    MOZ_ASSERT((frameSize % sizeof(Value)) == 0);
    return frameSize / sizeof(Value);
  }

  Value newTarget() const {
    MOZ_ASSERT(isFunctionFrame());
    MOZ_ASSERT(!callee()->isArrow());

    if (isConstructing()) {
      unsigned pushedArgs = std::max(numFormalArgs(), numActualArgs());
      return argv()[pushedArgs];
    }
    return UndefinedValue();
  }

#ifdef DEBUG
  size_t debugNumValueSlots() const { return numValueSlots(debugFrameSize()); }
#endif

  Value* valueSlot(size_t slot) const {
#ifndef ENABLE_PORTABLE_BASELINE_INTERP
    // Assert that we're within the frame, but only if the "debug
    // frame size" has been set. Ordinarily if we are in C++ code
    // looking upward at a baseline frame, it will be, because it is
    // set for the *previous* frame when we push an exit frame and
    // call back into C++ from generated baseline code. However, the
    // portable baseline interpreter uses accessors on BaselineFrame
    // directly within the active frame and so the "debug frame size"
    // hasn't been set (and it would be expensive to constantly update
    // it). Because this is only used for assertions, and is not
    // needed for correctness, we can disable this check below when
    // PBL is enabled.
    MOZ_ASSERT(slot < debugNumValueSlots());
#endif
    return (Value*)this - (slot + 1);
  }

  static size_t frameSizeForNumValueSlots(size_t numValueSlots) {
    return BaselineFrame::Size() + numValueSlots * sizeof(Value);
  }

  Value& unaliasedFormal(
      unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING) const {
    MOZ_ASSERT(i < numFormalArgs());
    MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals() &&
                                     !script()->formalIsAliased(i));
    return argv()[i];
  }

  Value& unaliasedActual(
      unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING) const {
    MOZ_ASSERT(i < numActualArgs());
    MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals());
    MOZ_ASSERT_IF(checkAliasing && i < numFormalArgs(),
                  !script()->formalIsAliased(i));
    return argv()[i];
  }

  Value& unaliasedLocal(uint32_t i) const {
    MOZ_ASSERT(i < script()->nfixed());
    return *valueSlot(i);
  }

  unsigned numActualArgs() const { return framePrefix()->numActualArgs(); }
  unsigned numFormalArgs() const { return script()->function()->nargs(); }
  Value& thisArgument() const {
    MOZ_ASSERT(isFunctionFrame());
    return framePrefix()->thisv();
  }
  Value* argv() const { return framePrefix()->actualArgs(); }

  [[nodiscard]] bool saveGeneratorSlots(JSContext* cx, unsigned nslots,
                                        ArrayObject* dest) const;

 public:
  void prepareForBaselineInterpreterToJitOSR() {
    // Clearing the RUNNING_IN_INTERPRETER flag is sufficient, but we also null
    // out the interpreter fields to ensure we don't use stale values.
    flags_ &= ~RUNNING_IN_INTERPRETER;
    interpreterScript_ = nullptr;
    interpreterPC_ = nullptr;
  }

 private:
  bool uninlineIsProfilerSamplingEnabled(JSContext* cx);

 public:
  // Switch a JIT frame on the stack to Interpreter mode. The caller is
  // responsible for patching the return address into this frame to a location
  // in the interpreter code. Also assert profiler sampling has been suppressed
  // so the sampler thread doesn't see an inconsistent state while we are
  // patching frames.
  void switchFromJitToInterpreter(JSContext* cx, jsbytecode* pc) {
    MOZ_ASSERT(!uninlineIsProfilerSamplingEnabled(cx));
    MOZ_ASSERT(!runningInInterpreter());
    flags_ |= RUNNING_IN_INTERPRETER;
    setInterpreterFields(pc);
  }
  void switchFromJitToInterpreterAtPrologue(JSContext* cx) {
    MOZ_ASSERT(!uninlineIsProfilerSamplingEnabled(cx));
    MOZ_ASSERT(!runningInInterpreter());
    flags_ |= RUNNING_IN_INTERPRETER;
    setInterpreterFieldsForPrologue(script());
  }

  // Like switchFromJitToInterpreter, but set the interpreterICEntry_ field to
  // nullptr. Initializing this field requires a binary search on the
  // JitScript's ICEntry list but the exception handler never returns to this
  // pc anyway so we can avoid the overhead.
  void switchFromJitToInterpreterForExceptionHandler(JSContext* cx,
                                                     jsbytecode* pc) {
    MOZ_ASSERT(!uninlineIsProfilerSamplingEnabled(cx));
    MOZ_ASSERT(!runningInInterpreter());
    flags_ |= RUNNING_IN_INTERPRETER;
    interpreterScript_ = script();
    interpreterPC_ = pc;
    interpreterICEntry_ = nullptr;
  }

  bool runningInInterpreter() const { return flags_ & RUNNING_IN_INTERPRETER; }

  JSScript* interpreterScript() const {
    MOZ_ASSERT(runningInInterpreter());
    return interpreterScript_;
  }

  jsbytecode* interpreterPC() const {
    MOZ_ASSERT(runningInInterpreter());
    return interpreterPC_;
  }
  jsbytecode*& interpreterPC() {
    MOZ_ASSERT(runningInInterpreter());
    return interpreterPC_;
  }

  ICEntry* interpreterICEntry() const {
    MOZ_ASSERT(runningInInterpreter());
    return interpreterICEntry_;
  }
  ICEntry*& interpreterICEntry() {
    MOZ_ASSERT(runningInInterpreter());
    return interpreterICEntry_;
  }

  void setInterpreterFields(JSScript* script, jsbytecode* pc);

  void setInterpreterFields(jsbytecode* pc) {
    setInterpreterFields(script(), pc);
  }

  // Initialize interpreter fields for resuming in the prologue (before the
  // argument type check ICs).
  void setInterpreterFieldsForPrologue(JSScript* script);

  ICScript* icScript() const { return icScript_; }
  void setICScript(ICScript* icScript) { icScript_ = icScript; }

  // The script that owns the current ICScript.
  JSScript* outerScript() const;

  bool hasReturnValue() const { return flags_ & HAS_RVAL; }
  MutableHandleValue returnValue() {
    if (!hasReturnValue()) {
      addressOfReturnValue()->setUndefined();
    }
    return MutableHandleValue::fromMarkedLocation(addressOfReturnValue());
  }
  void setReturnValue(const Value& v) {
    returnValue().set(v);
    flags_ |= HAS_RVAL;
  }
  inline Value* addressOfReturnValue() {
    return reinterpret_cast<Value*>(&loReturnValue_);
  }

  bool hasInitialEnvironment() const { return flags_ & HAS_INITIAL_ENV; }

  inline CallObject& callObj() const;

  void setFlag(uint32_t flag) { flags_ |= flag; }
  void setFlags(uint32_t flags) { flags_ = flags; }

  [[nodiscard]] inline bool pushLexicalEnvironment(JSContext* cx,
                                                   Handle<LexicalScope*> scope);
  template <bool IsDebuggee>
  [[nodiscard]] inline bool freshenLexicalEnvironment(
      JSContext* cx, const jsbytecode* pc = nullptr);
  template <bool IsDebuggee>
  [[nodiscard]] inline bool recreateLexicalEnvironment(
      JSContext* cx, const jsbytecode* pc = nullptr);

  [[nodiscard]] bool initFunctionEnvironmentObjects(JSContext* cx);
  [[nodiscard]] bool pushClassBodyEnvironment(JSContext* cx,
                                              Handle<ClassBodyScope*> scope);
  [[nodiscard]] bool pushVarEnvironment(JSContext* cx, Handle<Scope*> scope);

  void initArgsObjUnchecked(ArgumentsObject& argsobj) {
    flags_ |= HAS_ARGS_OBJ;
    argsObj_ = &argsobj;
  }
  void initArgsObj(ArgumentsObject& argsobj) {
    MOZ_ASSERT(script()->needsArgsObj());
    initArgsObjUnchecked(argsobj);
  }
  bool hasArgsObj() const { return flags_ & HAS_ARGS_OBJ; }
  ArgumentsObject& argsObj() const {
    MOZ_ASSERT(hasArgsObj());
    MOZ_ASSERT(script()->needsArgsObj());
    return *argsObj_;
  }

  bool prevUpToDate() const { return flags_ & PREV_UP_TO_DATE; }
  void setPrevUpToDate() { flags_ |= PREV_UP_TO_DATE; }
  void unsetPrevUpToDate() { flags_ &= ~PREV_UP_TO_DATE; }

  bool isDebuggee() const { return flags_ & DEBUGGEE; }
  void setIsDebuggee() { flags_ |= DEBUGGEE; }
  inline void unsetIsDebuggee();

  void trace(JSTracer* trc, const JSJitFrameIter& frame);

  bool isGlobalFrame() const { return script()->isGlobalCode(); }
  bool isModuleFrame() const { return script()->isModule(); }
  bool isEvalFrame() const { return script()->isForEval(); }
  bool isFunctionFrame() const {
    return CalleeTokenIsFunction(calleeToken()) && !isModuleFrame();
  }
  bool isDebuggerEvalFrame() const { return false; }

  JitFrameLayout* framePrefix() const {
    uint8_t* fp = (uint8_t*)this + Size();
    return (JitFrameLayout*)fp;
  }

  static size_t Size() { return sizeof(BaselineFrame); }

  // The reverseOffsetOf methods below compute the offset relative to the
  // frame's base pointer. Since the stack grows down, these offsets are
  // negative.

#ifdef DEBUG
  static int reverseOffsetOfDebugFrameSize() {
    return -int(Size()) + offsetof(BaselineFrame, debugFrameSize_);
  }
#endif

  // The scratch value slot can either be used as a Value slot or as two
  // separate 32-bit integer slots.
  static int reverseOffsetOfScratchValueLow32() {
    return -int(Size()) + offsetof(BaselineFrame, loScratchValue_);
  }
  static int reverseOffsetOfScratchValueHigh32() {
    return -int(Size()) + offsetof(BaselineFrame, hiScratchValue_);
  }
  static int reverseOffsetOfScratchValue() {
    return reverseOffsetOfScratchValueLow32();
  }

  static int reverseOffsetOfEnvironmentChain() {
    return -int(Size()) + offsetof(BaselineFrame, envChain_);
  }
  static int reverseOffsetOfArgsObj() {
    return -int(Size()) + offsetof(BaselineFrame, argsObj_);
  }
  static int reverseOffsetOfFlags() {
    return -int(Size()) + offsetof(BaselineFrame, flags_);
  }
  static int reverseOffsetOfReturnValue() {
    return -int(Size()) + offsetof(BaselineFrame, loReturnValue_);
  }
  static int reverseOffsetOfInterpreterScript() {
    return -int(Size()) + offsetof(BaselineFrame, interpreterScript_);
  }
  static int reverseOffsetOfInterpreterPC() {
    return -int(Size()) + offsetof(BaselineFrame, interpreterPC_);
  }
  static int reverseOffsetOfInterpreterICEntry() {
    return -int(Size()) + offsetof(BaselineFrame, interpreterICEntry_);
  }
  static int reverseOffsetOfICScript() {
    return -int(Size()) + offsetof(BaselineFrame, icScript_);
  }
  static int reverseOffsetOfLocal(size_t index) {
    return -int(Size()) - (index + 1) * sizeof(Value);
  }
};

// Ensure the frame is 8-byte aligned (required on ARM).
static_assert((sizeof(BaselineFrame) % 8) == 0, "frame must be 8-byte aligned");

}  // namespace jit
}  // namespace js

#endif /* jit_BaselineFrame_h */
