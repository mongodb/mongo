/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Stack_h
#define vm_Stack_h

#include "mozilla/HashFunctions.h"
#include "mozilla/MemoryReporting.h"

#include <algorithm>
#include <type_traits>

#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/ValueArray.h"
#include "vm/ArgumentsObject.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "wasm/WasmDebugFrame.h"  // js::wasm::DebugFrame

namespace js {

class InterpreterRegs;
class CallObject;
class FrameIter;
class ClassBodyScope;
class EnvironmentObject;
class BlockLexicalEnvironmentObject;
class ExtensibleLexicalEnvironmentObject;
class GeckoProfilerRuntime;
class InterpreterFrame;
class EnvironmentIter;
class EnvironmentCoordinate;

namespace jit {
class CommonFrameLayout;
}
namespace wasm {
class Instance;
}  // namespace wasm

// [SMDOC] VM stack layout
//
// A JSRuntime's stack consists of a linked list of activations. Every
// activation contains a number of scripted frames that are either running in
// the interpreter (InterpreterActivation) or JIT code (JitActivation). The
// frames inside a single activation are contiguous: whenever C++ calls back
// into JS, a new activation is pushed.
//
// Every activation is tied to a single JSContext and JS::Compartment. This
// means we can reconstruct a given context's stack by skipping activations
// belonging to other contexts. This happens whenever an embedding enters the JS
// engine on cx1 and then, from a native called by the JS engine, reenters the
// VM on cx2.

// Interpreter frames (InterpreterFrame)
//
// Each interpreter script activation (global or function code) is given a
// fixed-size header (js::InterpreterFrame). The frame contains bookkeeping
// information about the activation and links to the previous frame.
//
// The values after an InterpreterFrame in memory are its locals followed by its
// expression stack. InterpreterFrame::argv_ points to the frame's arguments.
// Missing formal arguments are padded with |undefined|, so the number of
// arguments is always >= the number of formals.
//
// The top of an activation's current frame's expression stack is pointed to by
// the activation's "current regs", which contains the stack pointer 'sp'. In
// the interpreter, sp is adjusted as individual values are pushed and popped
// from the stack and the InterpreterRegs struct (pointed to by the
// InterpreterActivation) is a local var of js::Interpret.

enum MaybeCheckAliasing { CHECK_ALIASING = true, DONT_CHECK_ALIASING = false };

}  // namespace js

/*****************************************************************************/

namespace js {

namespace jit {
class BaselineFrame;
class RematerializedFrame;
}  // namespace jit

/**
 * Pointer to a live JS or WASM stack frame.
 */
class AbstractFramePtr {
  friend class FrameIter;

  uintptr_t ptr_;

  enum {
    Tag_InterpreterFrame = 0x1,
    Tag_BaselineFrame = 0x2,
    Tag_RematerializedFrame = 0x3,
    Tag_WasmDebugFrame = 0x4,
    TagMask = 0x7
  };

 public:
  AbstractFramePtr() : ptr_(0) {}

  MOZ_IMPLICIT AbstractFramePtr(InterpreterFrame* fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_InterpreterFrame : 0) {
    MOZ_ASSERT_IF(fp, asInterpreterFrame() == fp);
  }

  MOZ_IMPLICIT AbstractFramePtr(jit::BaselineFrame* fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_BaselineFrame : 0) {
    MOZ_ASSERT_IF(fp, asBaselineFrame() == fp);
  }

  MOZ_IMPLICIT AbstractFramePtr(jit::RematerializedFrame* fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_RematerializedFrame : 0) {
    MOZ_ASSERT_IF(fp, asRematerializedFrame() == fp);
  }

  MOZ_IMPLICIT AbstractFramePtr(wasm::DebugFrame* fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_WasmDebugFrame : 0) {
    static_assert(wasm::DebugFrame::Alignment >= TagMask, "aligned");
    MOZ_ASSERT_IF(fp, asWasmDebugFrame() == fp);
  }

  bool isInterpreterFrame() const {
    return (ptr_ & TagMask) == Tag_InterpreterFrame;
  }
  InterpreterFrame* asInterpreterFrame() const {
    MOZ_ASSERT(isInterpreterFrame());
    InterpreterFrame* res = (InterpreterFrame*)(ptr_ & ~TagMask);
    MOZ_ASSERT(res);
    return res;
  }
  bool isBaselineFrame() const { return (ptr_ & TagMask) == Tag_BaselineFrame; }
  jit::BaselineFrame* asBaselineFrame() const {
    MOZ_ASSERT(isBaselineFrame());
    jit::BaselineFrame* res = (jit::BaselineFrame*)(ptr_ & ~TagMask);
    MOZ_ASSERT(res);
    return res;
  }
  bool isRematerializedFrame() const {
    return (ptr_ & TagMask) == Tag_RematerializedFrame;
  }
  jit::RematerializedFrame* asRematerializedFrame() const {
    MOZ_ASSERT(isRematerializedFrame());
    jit::RematerializedFrame* res =
        (jit::RematerializedFrame*)(ptr_ & ~TagMask);
    MOZ_ASSERT(res);
    return res;
  }
  bool isWasmDebugFrame() const {
    return (ptr_ & TagMask) == Tag_WasmDebugFrame;
  }
  wasm::DebugFrame* asWasmDebugFrame() const {
    MOZ_ASSERT(isWasmDebugFrame());
    wasm::DebugFrame* res = (wasm::DebugFrame*)(ptr_ & ~TagMask);
    MOZ_ASSERT(res);
    return res;
  }

  void* raw() const { return reinterpret_cast<void*>(ptr_); }

  bool operator==(const AbstractFramePtr& other) const {
    return ptr_ == other.ptr_;
  }
  bool operator!=(const AbstractFramePtr& other) const {
    return ptr_ != other.ptr_;
  }

  explicit operator bool() const { return !!ptr_; }

  inline JSObject* environmentChain() const;
  inline CallObject& callObj() const;
  inline bool initFunctionEnvironmentObjects(JSContext* cx);
  inline bool pushVarEnvironment(JSContext* cx, Handle<Scope*> scope);
  template <typename SpecificEnvironment>
  inline void pushOnEnvironmentChain(SpecificEnvironment& env);
  template <typename SpecificEnvironment>
  inline void popOffEnvironmentChain();

  inline JS::Realm* realm() const;

  inline bool hasInitialEnvironment() const;
  inline bool isGlobalFrame() const;
  inline bool isModuleFrame() const;
  inline bool isEvalFrame() const;
  inline bool isDebuggerEvalFrame() const;

  inline bool hasScript() const;
  inline JSScript* script() const;
  inline wasm::Instance* wasmInstance() const;
  inline GlobalObject* global() const;
  inline bool hasGlobal(const GlobalObject* global) const;
  inline JSFunction* callee() const;
  inline Value calleev() const;
  inline Value& thisArgument() const;

  inline bool isConstructing() const;

  inline bool debuggerNeedsCheckPrimitiveReturn() const;

  inline bool isFunctionFrame() const;
  inline bool isGeneratorFrame() const;

  inline bool saveGeneratorSlots(JSContext* cx, unsigned nslots,
                                 ArrayObject* dest) const;

  inline bool hasCachedSavedFrame() const;

  inline unsigned numActualArgs() const;
  inline unsigned numFormalArgs() const;

  inline Value* argv() const;

  inline bool hasArgs() const;
  inline bool hasArgsObj() const;
  inline ArgumentsObject& argsObj() const;
  inline void initArgsObj(ArgumentsObject& argsobj) const;

  inline Value& unaliasedLocal(uint32_t i);
  inline Value& unaliasedFormal(
      unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING);
  inline Value& unaliasedActual(
      unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING);
  template <class Op>
  inline void unaliasedForEachActual(JSContext* cx, Op op);

  inline bool prevUpToDate() const;
  inline void setPrevUpToDate() const;
  inline void unsetPrevUpToDate() const;

  inline bool isDebuggee() const;
  inline void setIsDebuggee();
  inline void unsetIsDebuggee();

  inline HandleValue returnValue() const;
  inline void setReturnValue(const Value& rval) const;

  friend void GDBTestInitAbstractFramePtr(AbstractFramePtr&, InterpreterFrame*);
  friend void GDBTestInitAbstractFramePtr(AbstractFramePtr&,
                                          jit::BaselineFrame*);
  friend void GDBTestInitAbstractFramePtr(AbstractFramePtr&,
                                          jit::RematerializedFrame*);
  friend void GDBTestInitAbstractFramePtr(AbstractFramePtr& frame,
                                          wasm::DebugFrame* ptr);
};

class NullFramePtr : public AbstractFramePtr {
 public:
  NullFramePtr() : AbstractFramePtr() {}
};

enum MaybeConstruct { NO_CONSTRUCT = false, CONSTRUCT = true };

/*****************************************************************************/

class InterpreterFrame {
  enum Flags : uint32_t {
    CONSTRUCTING = 0x1, /* frame is for a constructor invocation */

    RESUMED_GENERATOR = 0x2, /* frame is for a resumed generator invocation */

    /* Function prologue state */
    HAS_INITIAL_ENV =
        0x4,            /* callobj created for function or var env for eval */
    HAS_ARGS_OBJ = 0x8, /* ArgumentsObject created for needsArgsObj script */

    /* Lazy frame initialization */
    HAS_RVAL = 0x10, /* frame has rval_ set */

    /* Debugger state */
    PREV_UP_TO_DATE = 0x20, /* see DebugScopes::updateLiveScopes */

    /*
     * See comment above 'isDebuggee' in Realm.h for explanation of
     * invariants of debuggee compartments, scripts, and frames.
     */
    DEBUGGEE = 0x40, /* Execution is being observed by Debugger */

    /* Used in tracking calls and profiling (see vm/GeckoProfiler.cpp) */
    HAS_PUSHED_PROF_FRAME = 0x80, /* Gecko Profiler was notified of entry */

    /*
     * If set, we entered one of the JITs and ScriptFrameIter should skip
     * this frame.
     */
    RUNNING_IN_JIT = 0x100,

    /*
     * If set, this frame has been on the stack when
     * |js::SavedStacks::saveCurrentStack| was called, and so there is a
     * |js::SavedFrame| object cached for this frame.
     */
    HAS_CACHED_SAVED_FRAME = 0x200,
  };

  mutable uint32_t flags_; /* bits described by Flags */
  uint32_t nactual_;       /* number of actual arguments, for function frames */
  JSScript* script_;       /* the script we're executing */
  JSObject* envChain_;     /* current environment chain */
  Value rval_;             /* if HAS_RVAL, return value of the frame */
  ArgumentsObject* argsObj_; /* if HAS_ARGS_OBJ, the call's arguments object */

  /*
   * Previous frame and its pc and sp. Always nullptr for
   * InterpreterActivation's entry frame, always non-nullptr for inline
   * frames.
   */
  InterpreterFrame* prev_;
  jsbytecode* prevpc_;
  Value* prevsp_;

  /*
   * For an eval-in-frame DEBUGGER_EVAL frame, the frame in whose scope
   * we're evaluating code. Iteration treats this as our previous frame.
   */
  AbstractFramePtr evalInFramePrev_;

  Value* argv_;          /* If hasArgs(), points to frame's arguments. */
  LifoAlloc::Mark mark_; /* Used to release memory for this frame. */

  static void staticAsserts() {
    static_assert(offsetof(InterpreterFrame, rval_) % sizeof(Value) == 0);
    static_assert(sizeof(InterpreterFrame) % sizeof(Value) == 0);
  }

  /*
   * The utilities are private since they are not able to assert that only
   * unaliased vars/formals are accessed. Normal code should prefer the
   * InterpreterFrame::unaliased* members (or InterpreterRegs::stackDepth for
   * the usual "depth is at least" assertions).
   */
  Value* slots() const { return (Value*)(this + 1); }
  Value* base() const { return slots() + script()->nfixed(); }

  friend class FrameIter;
  friend class InterpreterRegs;
  friend class InterpreterStack;
  friend class jit::BaselineFrame;

  /*
   * Frame initialization, called by InterpreterStack operations after acquiring
   * the raw memory for the frame:
   */

  /* Used for Invoke and Interpret. */
  void initCallFrame(InterpreterFrame* prev, jsbytecode* prevpc, Value* prevsp,
                     JSFunction& callee, JSScript* script, Value* argv,
                     uint32_t nactual, MaybeConstruct constructing);

  /* Used for eval, module or global frames. */
  void initExecuteFrame(JSContext* cx, HandleScript script,
                        AbstractFramePtr prev, HandleObject envChain);

 public:
  /*
   * Frame prologue/epilogue
   *
   * Every stack frame must have 'prologue' called before executing the
   * first op and 'epilogue' called after executing the last op and before
   * popping the frame (whether the exit is exceptional or not).
   *
   * For inline JS calls/returns, it is easy to call the prologue/epilogue
   * exactly once. When calling JS from C++, Invoke/Execute push the stack
   * frame but do *not* call the prologue/epilogue. That means Interpret
   * must call the prologue/epilogue for the entry frame. This scheme
   * simplifies jit compilation.
   *
   * An important corner case is what happens when an error occurs (OOM,
   * over-recursed) after pushing the stack frame but before 'prologue' is
   * called or completes fully. To simplify usage, 'epilogue' does not assume
   * 'prologue' has completed and handles all the intermediate state details.
   */

  bool prologue(JSContext* cx);
  void epilogue(JSContext* cx, jsbytecode* pc);

  bool checkReturn(JSContext* cx, HandleValue thisv, MutableHandleValue result);

  bool initFunctionEnvironmentObjects(JSContext* cx);

  /*
   * Initialize locals of newly-pushed frame to undefined.
   */
  void initLocals();

  /*
   * Stack frame type
   *
   * A stack frame may have one of four types, which determines which
   * members of the frame may be accessed and other invariants:
   *
   *  global frame:   execution of global code
   *  function frame: execution of function code
   *  module frame:   execution of a module
   *  eval frame:     execution of eval code
   */

  bool isGlobalFrame() const { return script_->isGlobalCode(); }

  bool isModuleFrame() const { return script_->isModule(); }

  bool isEvalFrame() const { return script_->isForEval(); }

  bool isFunctionFrame() const { return script_->isFunction(); }

  /*
   * Previous frame
   *
   * A frame's 'prev' frame is either null or the previous frame pointed to
   * by cx->regs->fp when this frame was pushed. Often, given two prev-linked
   * frames, the next-frame is a function or eval that was called by the
   * prev-frame, but not always: the prev-frame may have called a native that
   * reentered the VM through JS_CallFunctionValue on the same context
   * (without calling JS_SaveFrameChain) which pushed the next-frame. Thus,
   * 'prev' has little semantic meaning and basically just tells the VM what
   * to set cx->regs->fp to when this frame is popped.
   */

  InterpreterFrame* prev() const { return prev_; }

  AbstractFramePtr evalInFramePrev() const {
    MOZ_ASSERT(isEvalFrame());
    return evalInFramePrev_;
  }

  /*
   * (Unaliased) locals and arguments
   *
   * Only non-eval function frames have arguments. The arguments pushed by
   * the caller are the 'actual' arguments. The declared arguments of the
   * callee are the 'formal' arguments. When the caller passes less actual
   * arguments, missing formal arguments are padded with |undefined|.
   *
   * When a local/formal variable is aliased (accessed by nested closures,
   * environment operations, or 'arguments'), the canonical location for
   * that value is the slot of an environment object.  Aliased locals don't
   * have stack slots assigned to them.  These functions assert that
   * accesses to stack values are unaliased.
   */

  inline Value& unaliasedLocal(uint32_t i);

  bool hasArgs() const { return isFunctionFrame(); }
  inline Value& unaliasedFormal(unsigned i,
                                MaybeCheckAliasing = CHECK_ALIASING);
  inline Value& unaliasedActual(unsigned i,
                                MaybeCheckAliasing = CHECK_ALIASING);
  template <class Op>
  inline void unaliasedForEachActual(Op op);

  unsigned numFormalArgs() const {
    MOZ_ASSERT(hasArgs());
    return callee().nargs();
  }
  unsigned numActualArgs() const {
    MOZ_ASSERT(hasArgs());
    return nactual_;
  }

  /* Watch out, this exposes a pointer to the unaliased formal arg array. */
  Value* argv() const {
    MOZ_ASSERT(hasArgs());
    return argv_;
  }

  /*
   * Arguments object
   *
   * If a non-eval function has script->needsArgsObj, an arguments object is
   * created in the prologue and stored in the local variable for the
   * 'arguments' binding (script->argumentsLocal). Since this local is
   * mutable, the arguments object can be overwritten and we can "lose" the
   * arguments object. Thus, InterpreterFrame keeps an explicit argsObj_ field
   * so that the original arguments object is always available.
   */

  ArgumentsObject& argsObj() const;
  void initArgsObj(ArgumentsObject& argsobj);

  ArrayObject* createRestParameter(JSContext* cx);

  /*
   * Environment chain
   *
   * In theory, the environment chain would contain an object for every
   * lexical scope. However, only objects that are required for dynamic
   * lookup are actually created.
   *
   * Given that an InterpreterFrame corresponds roughly to a ES Execution
   * Context (ES 10.3), GetVariablesObject corresponds to the
   * VariableEnvironment component of a Exection Context. Intuitively, the
   * variables object is where new bindings (variables and functions) are
   * stored. One might expect that this is either the Call object or
   * envChain.globalObj for function or global code, respectively, however
   * the JSAPI allows calls of Execute to specify a variables object on the
   * environment chain other than the call/global object. This allows
   * embeddings to run multiple scripts under the same global, each time
   * using a new variables object to collect and discard the script's global
   * variables.
   */

  inline HandleObject environmentChain() const;

  inline EnvironmentObject& aliasedEnvironment(EnvironmentCoordinate ec) const;
  inline EnvironmentObject& aliasedEnvironmentMaybeDebug(
      EnvironmentCoordinate ec) const;
  inline GlobalObject& global() const;
  inline CallObject& callObj() const;
  inline ExtensibleLexicalEnvironmentObject& extensibleLexicalEnvironment()
      const;

  template <typename SpecificEnvironment>
  inline void pushOnEnvironmentChain(SpecificEnvironment& env);
  template <typename SpecificEnvironment>
  inline void popOffEnvironmentChain();
  inline void replaceInnermostEnvironment(BlockLexicalEnvironmentObject& env);

  // Push a VarEnvironmentObject for function frames of functions that have
  // parameter expressions with closed over var bindings.
  bool pushVarEnvironment(JSContext* cx, Handle<Scope*> scope);

  /*
   * For lexical envs with aliased locals, these interfaces push and pop
   * entries on the environment chain.  The "freshen" operation replaces the
   * current lexical env with a fresh copy of it, to implement semantics
   * providing distinct bindings per iteration of a for(;;) loop whose head
   * has a lexical declaration.  The "recreate" operation replaces the
   * current lexical env with a copy of it containing uninitialized
   * bindings, to implement semantics providing distinct bindings per
   * iteration of a for-in/of loop.
   */

  bool pushLexicalEnvironment(JSContext* cx, Handle<LexicalScope*> scope);
  bool freshenLexicalEnvironment(JSContext* cx);
  bool recreateLexicalEnvironment(JSContext* cx);

  bool pushClassBodyEnvironment(JSContext* cx, Handle<ClassBodyScope*> scope);

  /*
   * Script
   *
   * All frames have an associated JSScript which holds the bytecode being
   * executed for the frame.
   */

  JSScript* script() const { return script_; }

  /* Return the previous frame's pc. */
  jsbytecode* prevpc() {
    MOZ_ASSERT(prev_);
    return prevpc_;
  }

  /* Return the previous frame's sp. */
  Value* prevsp() {
    MOZ_ASSERT(prev_);
    return prevsp_;
  }

  /*
   * Return the 'this' argument passed to a non-eval function frame. This is
   * not necessarily the frame's this-binding, for instance non-strict
   * functions will box primitive 'this' values and thisArgument() will
   * return the original, unboxed Value.
   */
  Value& thisArgument() const {
    MOZ_ASSERT(isFunctionFrame());
    return argv()[-1];
  }

  /*
   * Callee
   *
   * Only function frames have a true callee. An eval frame in a function has
   * the same callee as its containing function frame. An async module has to
   * create a wrapper callee to allow passing the script to generators for
   * pausing and resuming.
   */

  JSFunction& callee() const {
    MOZ_ASSERT(isFunctionFrame());
    return calleev().toObject().as<JSFunction>();
  }

  const Value& calleev() const {
    MOZ_ASSERT(isFunctionFrame());
    return argv()[-2];
  }

  /*
   * New Target
   *
   * Only non-arrow function frames have a meaningful newTarget.
   */
  Value newTarget() const {
    MOZ_ASSERT(isFunctionFrame());
    MOZ_ASSERT(!callee().isArrow());

    if (isConstructing()) {
      unsigned pushedArgs = std::max(numFormalArgs(), numActualArgs());
      return argv()[pushedArgs];
    }
    return UndefinedValue();
  }

  /* Profiler flags */

  bool hasPushedGeckoProfilerFrame() {
    return !!(flags_ & HAS_PUSHED_PROF_FRAME);
  }

  void setPushedGeckoProfilerFrame() { flags_ |= HAS_PUSHED_PROF_FRAME; }

  void unsetPushedGeckoProfilerFrame() { flags_ &= ~HAS_PUSHED_PROF_FRAME; }

  /* Return value */

  bool hasReturnValue() const { return flags_ & HAS_RVAL; }

  MutableHandleValue returnValue() {
    if (!hasReturnValue()) {
      rval_.setUndefined();
    }
    return MutableHandleValue::fromMarkedLocation(&rval_);
  }

  void markReturnValue() { flags_ |= HAS_RVAL; }

  void setReturnValue(const Value& v) {
    rval_ = v;
    markReturnValue();
  }

  // Copy values from this frame into a private Array, owned by the
  // GeneratorObject, for suspending.
  [[nodiscard]] inline bool saveGeneratorSlots(JSContext* cx, unsigned nslots,
                                               ArrayObject* dest) const;

  // Copy values from the Array into this stack frame, for resuming.
  inline void restoreGeneratorSlots(ArrayObject* src);

  void resumeGeneratorFrame(JSObject* envChain) {
    MOZ_ASSERT(script()->isGenerator() || script()->isAsync());
    MOZ_ASSERT_IF(!script()->isModule(), isFunctionFrame());
    flags_ |= HAS_INITIAL_ENV;
    envChain_ = envChain;
  }

  /*
   * Other flags
   */

  bool isConstructing() const { return !!(flags_ & CONSTRUCTING); }

  void setResumedGenerator() { flags_ |= RESUMED_GENERATOR; }
  bool isResumedGenerator() const { return !!(flags_ & RESUMED_GENERATOR); }

  /*
   * These two queries should not be used in general: the presence/absence of
   * the call/args object is determined by the static(ish) properties of the
   * JSFunction/JSScript. These queries should only be performed when probing
   * a stack frame that may be in the middle of the prologue (during which
   * time the call/args object are created).
   */

  inline bool hasInitialEnvironment() const;

  bool hasInitialEnvironmentUnchecked() const {
    return flags_ & HAS_INITIAL_ENV;
  }

  bool hasArgsObj() const {
    MOZ_ASSERT(script()->needsArgsObj());
    return flags_ & HAS_ARGS_OBJ;
  }

  /*
   * Debugger eval frames.
   *
   * - If evalInFramePrev_ is non-null, frame was created for an "eval in
   *   frame" call, which can push a successor to any live frame; so its
   *   logical "prev" frame is not necessarily the previous frame in memory.
   *   Iteration should treat evalInFramePrev_ as this frame's previous frame.
   *
   * - Don't bother to JIT it, because it's probably short-lived.
   *
   * - It is required to have a environment chain object outside the
   *   js::EnvironmentObject hierarchy: either a global object, or a
   *   DebugEnvironmentProxy.
   */
  bool isDebuggerEvalFrame() const {
    return isEvalFrame() && !!evalInFramePrev_;
  }

  bool prevUpToDate() const { return !!(flags_ & PREV_UP_TO_DATE); }

  void setPrevUpToDate() { flags_ |= PREV_UP_TO_DATE; }

  void unsetPrevUpToDate() { flags_ &= ~PREV_UP_TO_DATE; }

  bool isDebuggee() const { return !!(flags_ & DEBUGGEE); }

  void setIsDebuggee() { flags_ |= DEBUGGEE; }

  inline void unsetIsDebuggee();

  bool hasCachedSavedFrame() const { return flags_ & HAS_CACHED_SAVED_FRAME; }
  void setHasCachedSavedFrame() { flags_ |= HAS_CACHED_SAVED_FRAME; }
  void clearHasCachedSavedFrame() { flags_ &= ~HAS_CACHED_SAVED_FRAME; }

 public:
  void trace(JSTracer* trc, Value* sp, jsbytecode* pc);
  void traceValues(JSTracer* trc, unsigned start, unsigned end);

  // Entered Baseline/Ion from the interpreter.
  bool runningInJit() const { return !!(flags_ & RUNNING_IN_JIT); }
  void setRunningInJit() { flags_ |= RUNNING_IN_JIT; }
  void clearRunningInJit() { flags_ &= ~RUNNING_IN_JIT; }
};

/*****************************************************************************/

class InterpreterRegs {
 public:
  Value* sp;
  jsbytecode* pc;

 private:
  InterpreterFrame* fp_;

 public:
  InterpreterFrame* fp() const { return fp_; }

  unsigned stackDepth() const {
    MOZ_ASSERT(sp >= fp_->base());
    return sp - fp_->base();
  }

  Value* spForStackDepth(unsigned depth) const {
    MOZ_ASSERT(fp_->script()->nfixed() + depth <= fp_->script()->nslots());
    return fp_->base() + depth;
  }

  void popInlineFrame() {
    pc = fp_->prevpc();
    unsigned spForNewTarget =
        fp_->isResumedGenerator() ? 0 : fp_->isConstructing();
    // This code is called when resuming from async and generator code.
    // In the case of modules, we don't have arguments, so we can't use
    // numActualArgs, which asserts 'hasArgs'.
    unsigned nActualArgs = fp_->isModuleFrame() ? 0 : fp_->numActualArgs();
    sp = fp_->prevsp() - nActualArgs - 1 - spForNewTarget;
    fp_ = fp_->prev();
    MOZ_ASSERT(fp_);
  }
  void prepareToRun(InterpreterFrame& fp, JSScript* script) {
    pc = script->code();
    sp = fp.slots() + script->nfixed();
    fp_ = &fp;
  }

  void setToEndOfScript();

  MutableHandleValue stackHandleAt(int i) {
    return MutableHandleValue::fromMarkedLocation(&sp[i]);
  }

  HandleValue stackHandleAt(int i) const {
    return HandleValue::fromMarkedLocation(&sp[i]);
  }

  friend void GDBTestInitInterpreterRegs(InterpreterRegs&,
                                         js::InterpreterFrame*, JS::Value*,
                                         uint8_t*);
};

/*****************************************************************************/

class InterpreterStack {
  friend class InterpreterActivation;

  static const size_t DEFAULT_CHUNK_SIZE = 4 * 1024;
  LifoAlloc allocator_;

  // Number of interpreter frames on the stack, for over-recursion checks.
  static const size_t MAX_FRAMES = 50 * 1000;
  static const size_t MAX_FRAMES_TRUSTED = MAX_FRAMES + 1000;
  size_t frameCount_;

  inline uint8_t* allocateFrame(JSContext* cx, size_t size);

  inline InterpreterFrame* getCallFrame(JSContext* cx, const CallArgs& args,
                                        HandleScript script,
                                        MaybeConstruct constructing,
                                        Value** pargv);

  void releaseFrame(InterpreterFrame* fp) {
    frameCount_--;
    allocator_.release(fp->mark_);
  }

 public:
  InterpreterStack() : allocator_(DEFAULT_CHUNK_SIZE), frameCount_(0) {}

  ~InterpreterStack() { MOZ_ASSERT(frameCount_ == 0); }

  // For execution of eval, module or global code.
  InterpreterFrame* pushExecuteFrame(JSContext* cx, HandleScript script,
                                     HandleObject envChain,
                                     AbstractFramePtr evalInFrame);

  // Called to invoke a function.
  InterpreterFrame* pushInvokeFrame(JSContext* cx, const CallArgs& args,
                                    MaybeConstruct constructing);

  // The interpreter can push light-weight, "inline" frames without entering a
  // new InterpreterActivation or recursively calling Interpret.
  bool pushInlineFrame(JSContext* cx, InterpreterRegs& regs,
                       const CallArgs& args, HandleScript script,
                       MaybeConstruct constructing);

  void popInlineFrame(InterpreterRegs& regs);

  bool resumeGeneratorCallFrame(JSContext* cx, InterpreterRegs& regs,
                                HandleFunction callee, HandleObject envChain);

  inline void purge(JSRuntime* rt);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return allocator_.sizeOfExcludingThis(mallocSizeOf);
  }
};

void TraceInterpreterActivations(JSContext* cx, JSTracer* trc);

/*****************************************************************************/

/** Base class for all function call args. */
class AnyInvokeArgs : public JS::CallArgs {};

/** Base class for all function construction args. */
class AnyConstructArgs : public JS::CallArgs {
  // Only js::Construct (or internal methods that call the qualified CallArgs
  // versions) should do these things!
  void setCallee(const Value& v) = delete;
  void setThis(const Value& v) = delete;
  MutableHandleValue newTarget() const = delete;
  MutableHandleValue rval() const = delete;
};

namespace detail {

/** Function call/construct args of statically-unknown count. */
template <MaybeConstruct Construct>
class GenericArgsBase
    : public std::conditional_t<Construct, AnyConstructArgs, AnyInvokeArgs> {
 protected:
  RootedValueVector v_;

  explicit GenericArgsBase(JSContext* cx) : v_(cx) {}

 public:
  bool init(JSContext* cx, uint64_t argc) {
    if (argc > ARGS_LENGTH_MAX) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TOO_MANY_ARGUMENTS);
      return false;
    }

    // callee, this, arguments[, new.target iff constructing]
    size_t len = 2 + argc + uint32_t(Construct);
    MOZ_ASSERT(len > argc);  // no overflow
    if (!v_.resize(len)) {
      return false;
    }

    *static_cast<JS::CallArgs*>(this) = CallArgsFromVp(argc, v_.begin());
    this->constructing_ = Construct;
    if (Construct) {
      this->CallArgs::setThis(MagicValue(JS_IS_CONSTRUCTING));
    }
    return true;
  }
};

/** Function call/construct args of statically-known count. */
template <MaybeConstruct Construct, size_t N>
class FixedArgsBase
    : public std::conditional_t<Construct, AnyConstructArgs, AnyInvokeArgs> {
  // Add +1 here to avoid noisy warning on gcc when N=0 (0 <= unsigned).
  static_assert(N + 1 <= ARGS_LENGTH_MAX + 1, "o/~ too many args o/~");

 protected:
  JS::RootedValueArray<2 + N + uint32_t(Construct)> v_;

  explicit FixedArgsBase(JSContext* cx) : v_(cx) {
    *static_cast<JS::CallArgs*>(this) = CallArgsFromVp(N, v_.begin());
    this->constructing_ = Construct;
    if (Construct) {
      this->CallArgs::setThis(MagicValue(JS_IS_CONSTRUCTING));
    }
  }
};

}  // namespace detail

/** Function call args of statically-unknown count. */
class InvokeArgs : public detail::GenericArgsBase<NO_CONSTRUCT> {
  using Base = detail::GenericArgsBase<NO_CONSTRUCT>;

 public:
  explicit InvokeArgs(JSContext* cx) : Base(cx) {}
};

/** Function call args of statically-unknown count. */
class InvokeArgsMaybeIgnoresReturnValue
    : public detail::GenericArgsBase<NO_CONSTRUCT> {
  using Base = detail::GenericArgsBase<NO_CONSTRUCT>;

 public:
  explicit InvokeArgsMaybeIgnoresReturnValue(JSContext* cx) : Base(cx) {}

  bool init(JSContext* cx, unsigned argc, bool ignoresReturnValue) {
    if (!Base::init(cx, argc)) {
      return false;
    }
    this->ignoresReturnValue_ = ignoresReturnValue;
    return true;
  }
};

/** Function call args of statically-known count. */
template <size_t N>
class FixedInvokeArgs : public detail::FixedArgsBase<NO_CONSTRUCT, N> {
  using Base = detail::FixedArgsBase<NO_CONSTRUCT, N>;

 public:
  explicit FixedInvokeArgs(JSContext* cx) : Base(cx) {}
};

/** Function construct args of statically-unknown count. */
class ConstructArgs : public detail::GenericArgsBase<CONSTRUCT> {
  using Base = detail::GenericArgsBase<CONSTRUCT>;

 public:
  explicit ConstructArgs(JSContext* cx) : Base(cx) {}
};

/** Function call args of statically-known count. */
template <size_t N>
class FixedConstructArgs : public detail::FixedArgsBase<CONSTRUCT, N> {
  using Base = detail::FixedArgsBase<CONSTRUCT, N>;

 public:
  explicit FixedConstructArgs(JSContext* cx) : Base(cx) {}
};

template <class Args, class Arraylike>
inline bool FillArgumentsFromArraylike(JSContext* cx, Args& args,
                                       const Arraylike& arraylike) {
  uint32_t len = arraylike.length();
  if (!args.init(cx, len)) {
    return false;
  }

  for (uint32_t i = 0; i < len; i++) {
    args[i].set(arraylike[i]);
  }

  return true;
}

}  // namespace js

namespace mozilla {

template <>
struct DefaultHasher<js::AbstractFramePtr> {
  using Lookup = js::AbstractFramePtr;

  static js::HashNumber hash(const Lookup& key) {
    return mozilla::HashGeneric(key.raw());
  }

  static bool match(const js::AbstractFramePtr& k, const Lookup& l) {
    return k == l;
  }
};

}  // namespace mozilla

#endif  // vm_Stack_h
