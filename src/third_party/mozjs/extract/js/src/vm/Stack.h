/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Stack_h
#define vm_Stack_h

#include "mozilla/Atomics.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Maybe.h"
#include "mozilla/MaybeOneOf.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Variant.h"

#include "jsutil.h"

#include "gc/Rooting.h"
#ifdef CHECK_OSIPOINT_REGISTERS
#include "jit/Registers.h" // for RegisterDump
#endif
#include "jit/JSJitFrameIter.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "vm/ArgumentsObject.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/SavedFrame.h"
#include "wasm/WasmFrameIter.h"
#include "wasm/WasmTypes.h"

namespace JS {
namespace dbg {
#ifdef JS_BROKEN_GCC_ATTRIBUTE_WARNING
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif // JS_BROKEN_GCC_ATTRIBUTE_WARNING

class JS_PUBLIC_API(AutoEntryMonitor);

#ifdef JS_BROKEN_GCC_ATTRIBUTE_WARNING
#pragma GCC diagnostic pop
#endif // JS_BROKEN_GCC_ATTRIBUTE_WARNING
} // namespace dbg
} // namespace JS

namespace js {

class InterpreterRegs;
class CallObject;
class FrameIter;
class EnvironmentObject;
class ScriptFrameIter;
class GeckoProfilerRuntime;
class InterpreterFrame;
class LexicalEnvironmentObject;
class EnvironmentIter;
class EnvironmentCoordinate;

class SavedFrame;

namespace jit {
class CommonFrameLayout;
}
namespace wasm {
class DebugFrame;
class Instance;
}

// VM stack layout
//
// A JSRuntime's stack consists of a linked list of activations. Every activation
// contains a number of scripted frames that are either running in the interpreter
// (InterpreterActivation) or JIT code (JitActivation). The frames inside a single
// activation are contiguous: whenever C++ calls back into JS, a new activation is
// pushed.
//
// Every activation is tied to a single JSContext and JSCompartment. This means we
// can reconstruct a given context's stack by skipping activations belonging to other
// contexts. This happens whenever an embedding enters the JS engine on cx1 and
// then, from a native called by the JS engine, reenters the VM on cx2.

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
enum MaybeCheckTDZ { CheckTDZ = true, DontCheckTDZ = false };

/*****************************************************************************/

namespace jit {
    class BaselineFrame;
    class RematerializedFrame;
} // namespace jit

/*
 * Pointer to either a ScriptFrameIter::Data, an InterpreterFrame, or a Baseline
 * JIT frame.
 *
 * The Debugger may cache ScriptFrameIter::Data as a bookmark to reconstruct a
 * ScriptFrameIter without doing a full stack walk.
 *
 * There is no way to directly create such an AbstractFramePtr. To do so, the
 * user must call ScriptFrameIter::copyDataAsAbstractFramePtr().
 *
 * ScriptFrameIter::abstractFramePtr() will never return an AbstractFramePtr
 * that is in fact a ScriptFrameIter::Data.
 *
 * To recover a ScriptFrameIter settled at the location pointed to by an
 * AbstractFramePtr, use the THIS_FRAME_ITER macro in Debugger.cpp. As an
 * aside, no asScriptFrameIterData() is provided because C++ is stupid and
 * cannot forward declare inner classes.
 */

class AbstractFramePtr
{
    friend class FrameIter;

    uintptr_t ptr_;

    enum {
        Tag_ScriptFrameIterData = 0x0,
        Tag_InterpreterFrame = 0x1,
        Tag_BaselineFrame = 0x2,
        Tag_RematerializedFrame = 0x3,
        Tag_WasmDebugFrame = 0x4,
        TagMask = 0x7
    };

  public:
    AbstractFramePtr()
      : ptr_(0)
    {}

    MOZ_IMPLICIT AbstractFramePtr(InterpreterFrame* fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_InterpreterFrame : 0)
    {
        MOZ_ASSERT_IF(fp, asInterpreterFrame() == fp);
    }

    MOZ_IMPLICIT AbstractFramePtr(jit::BaselineFrame* fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_BaselineFrame : 0)
    {
        MOZ_ASSERT_IF(fp, asBaselineFrame() == fp);
    }

    MOZ_IMPLICIT AbstractFramePtr(jit::RematerializedFrame* fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_RematerializedFrame : 0)
    {
        MOZ_ASSERT_IF(fp, asRematerializedFrame() == fp);
    }

    MOZ_IMPLICIT AbstractFramePtr(wasm::DebugFrame* fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_WasmDebugFrame : 0)
    {
        static_assert(wasm::DebugFrame::Alignment >= TagMask, "aligned");
        MOZ_ASSERT_IF(fp, asWasmDebugFrame() == fp);
    }

    static AbstractFramePtr FromRaw(void* raw) {
        AbstractFramePtr frame;
        frame.ptr_ = uintptr_t(raw);
        return frame;
    }

    bool isScriptFrameIterData() const {
        return !!ptr_ && (ptr_ & TagMask) == Tag_ScriptFrameIterData;
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
    bool isBaselineFrame() const {
        return (ptr_ & TagMask) == Tag_BaselineFrame;
    }
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
        jit::RematerializedFrame* res = (jit::RematerializedFrame*)(ptr_ & ~TagMask);
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

    bool operator ==(const AbstractFramePtr& other) const { return ptr_ == other.ptr_; }
    bool operator !=(const AbstractFramePtr& other) const { return ptr_ != other.ptr_; }

    explicit operator bool() const { return !!ptr_; }

    inline JSObject* environmentChain() const;
    inline CallObject& callObj() const;
    inline bool initFunctionEnvironmentObjects(JSContext* cx);
    inline bool pushVarEnvironment(JSContext* cx, HandleScope scope);
    template <typename SpecificEnvironment>
    inline void pushOnEnvironmentChain(SpecificEnvironment& env);
    template <typename SpecificEnvironment>
    inline void popOffEnvironmentChain();

    inline JSCompartment* compartment() const;

    inline bool hasInitialEnvironment() const;
    inline bool isGlobalFrame() const;
    inline bool isModuleFrame() const;
    inline bool isEvalFrame() const;
    inline bool isDebuggerEvalFrame() const;

    inline bool hasScript() const;
    inline JSScript* script() const;
    inline wasm::Instance* wasmInstance() const;
    inline GlobalObject* global() const;
    inline JSFunction* callee() const;
    inline Value calleev() const;
    inline Value& thisArgument() const;

    inline Value newTarget() const;

    inline bool debuggerNeedsCheckPrimitiveReturn() const;

    inline bool isFunctionFrame() const;
    inline bool isNonStrictDirectEvalFrame() const;
    inline bool isStrictEvalFrame() const;

    inline unsigned numActualArgs() const;
    inline unsigned numFormalArgs() const;

    inline Value* argv() const;

    inline bool hasArgs() const;
    inline bool hasArgsObj() const;
    inline ArgumentsObject& argsObj() const;
    inline void initArgsObj(ArgumentsObject& argsobj) const;

    inline Value& unaliasedLocal(uint32_t i);
    inline Value& unaliasedFormal(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING);
    inline Value& unaliasedActual(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING);
    template <class Op> inline void unaliasedForEachActual(JSContext* cx, Op op);

    inline bool prevUpToDate() const;
    inline void setPrevUpToDate() const;
    inline void unsetPrevUpToDate() const;

    inline bool isDebuggee() const;
    inline void setIsDebuggee();
    inline void unsetIsDebuggee();

    inline HandleValue returnValue() const;
    inline void setReturnValue(const Value& rval) const;

    friend void GDBTestInitAbstractFramePtr(AbstractFramePtr&, void*);
    friend void GDBTestInitAbstractFramePtr(AbstractFramePtr&, InterpreterFrame*);
    friend void GDBTestInitAbstractFramePtr(AbstractFramePtr&, jit::BaselineFrame*);
    friend void GDBTestInitAbstractFramePtr(AbstractFramePtr&, jit::RematerializedFrame*);
    friend void GDBTestInitAbstractFramePtr(AbstractFramePtr& frame, wasm::DebugFrame* ptr);
};

class NullFramePtr : public AbstractFramePtr
{
  public:
    NullFramePtr()
      : AbstractFramePtr()
    { }
};

enum MaybeConstruct { NO_CONSTRUCT = false, CONSTRUCT = true };

/*****************************************************************************/

class InterpreterFrame
{
    enum Flags : uint32_t {
        CONSTRUCTING           =        0x1,  /* frame is for a constructor invocation */

        RESUMED_GENERATOR      =        0x2,  /* frame is for a resumed generator invocation */

        /* Function prologue state */
        HAS_INITIAL_ENV        =        0x4,  /* callobj created for function or var env for eval */
        HAS_ARGS_OBJ           =        0x8,  /* ArgumentsObject created for needsArgsObj script */

        /* Lazy frame initialization */
        HAS_RVAL               =       0x10,  /* frame has rval_ set */

        /* Debugger state */
        PREV_UP_TO_DATE        =       0x20,  /* see DebugScopes::updateLiveScopes */

        /*
         * See comment above 'isDebuggee' in JSCompartment.h for explanation of
         * invariants of debuggee compartments, scripts, and frames.
         */
        DEBUGGEE               =       0x40,  /* Execution is being observed by Debugger */

        /* Used in tracking calls and profiling (see vm/GeckoProfiler.cpp) */
        HAS_PUSHED_PROF_FRAME  =       0x80,  /* Gecko Profiler was notified of entry */

        /*
         * If set, we entered one of the JITs and ScriptFrameIter should skip
         * this frame.
         */
        RUNNING_IN_JIT         =      0x100,

        /*
         * If set, this frame has been on the stack when
         * |js::SavedStacks::saveCurrentStack| was called, and so there is a
         * |js::SavedFrame| object cached for this frame.
         */
        HAS_CACHED_SAVED_FRAME =      0x200,
    };

    mutable uint32_t    flags_;         /* bits described by Flags */
    uint32_t            nactual_;       /* number of actual arguments, for function frames */
    JSScript*           script_;        /* the script we're executing */
    JSObject*           envChain_;      /* current environment chain */
    Value               rval_;          /* if HAS_RVAL, return value of the frame */
    ArgumentsObject*    argsObj_;       /* if HAS_ARGS_OBJ, the call's arguments object */

    /*
     * Previous frame and its pc and sp. Always nullptr for
     * InterpreterActivation's entry frame, always non-nullptr for inline
     * frames.
     */
    InterpreterFrame*   prev_;
    jsbytecode*         prevpc_;
    Value*              prevsp_;

    void*               unused;

    /*
     * For an eval-in-frame DEBUGGER_EVAL frame, the frame in whose scope
     * we're evaluating code. Iteration treats this as our previous frame.
     */
    AbstractFramePtr    evalInFramePrev_;

    Value*              argv_;         /* If hasArgs(), points to frame's arguments. */
    LifoAlloc::Mark     mark_;          /* Used to release memory for this frame. */

    static void staticAsserts() {
        JS_STATIC_ASSERT(offsetof(InterpreterFrame, rval_) % sizeof(Value) == 0);
        JS_STATIC_ASSERT(sizeof(InterpreterFrame) % sizeof(Value) == 0);
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
                       JSFunction& callee, JSScript* script, Value* argv, uint32_t nactual,
                       MaybeConstruct constructing);

    /* Used for global and eval frames. */
    void initExecuteFrame(JSContext* cx, HandleScript script, AbstractFramePtr prev,
                          const Value& newTargetValue, HandleObject envChain);

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

    bool checkReturn(JSContext* cx, HandleValue thisv);

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

    bool isGlobalFrame() const {
        return script_->isGlobalCode();
    }

    bool isModuleFrame() const {
        return script_->module();
    }

    bool isEvalFrame() const {
        return script_->isForEval();
    }

    bool isFunctionFrame() const {
        return script_->functionNonDelazifying();
    }

    inline bool isStrictEvalFrame() const {
        return isEvalFrame() && script()->strict();
    }

    bool isNonStrictEvalFrame() const {
        return isEvalFrame() && !script()->strict();
    }

    bool isNonGlobalEvalFrame() const;

    bool isNonStrictDirectEvalFrame() const {
        return isNonStrictEvalFrame() && isNonGlobalEvalFrame();
    }

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

    InterpreterFrame* prev() const {
        return prev_;
    }

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
    inline Value& unaliasedFormal(unsigned i, MaybeCheckAliasing = CHECK_ALIASING);
    inline Value& unaliasedActual(unsigned i, MaybeCheckAliasing = CHECK_ALIASING);
    template <class Op> inline void unaliasedForEachActual(Op op);

    unsigned numFormalArgs() const { MOZ_ASSERT(hasArgs()); return callee().nargs(); }
    unsigned numActualArgs() const { MOZ_ASSERT(hasArgs()); return nactual_; }

    /* Watch out, this exposes a pointer to the unaliased formal arg array. */
    Value* argv() const { MOZ_ASSERT(hasArgs()); return argv_; }

    /*
     * Arguments object
     *
     * If a non-eval function has script->needsArgsObj, an arguments object is
     * created in the prologue and stored in the local variable for the
     * 'arguments' binding (script->argumentsLocal). Since this local is
     * mutable, the arguments object can be overwritten and we can "lose" the
     * arguments object. Thus, InterpreterFrame keeps an explicit argsObj_ field so
     * that the original arguments object is always available.
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
     * Context (ES 10.3), InterpreterFrame::varObj corresponds to the
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
    inline GlobalObject& global() const;
    inline CallObject& callObj() const;
    inline JSObject& varObj() const;
    inline LexicalEnvironmentObject& extensibleLexicalEnvironment() const;

    template <typename SpecificEnvironment>
    inline void pushOnEnvironmentChain(SpecificEnvironment& env);
    template <typename SpecificEnvironment>
    inline void popOffEnvironmentChain();
    inline void replaceInnermostEnvironment(EnvironmentObject& env);

    // Push a VarEnvironmentObject for function frames of functions that have
    // parameter expressions with closed over var bindings.
    bool pushVarEnvironment(JSContext* cx, HandleScope scope);

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

    /*
     * Script
     *
     * All frames have an associated JSScript which holds the bytecode being
     * executed for the frame.
     */

    JSScript* script() const {
        return script_;
    }

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
     * Only function frames have a callee. An eval frame in a function has the
     * same callee as its containing function frame.
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
     * Only function frames have a meaningful newTarget. An eval frame in a
     * function will have a copy of the newTarget of the enclosing function
     * frame.
     */
    Value newTarget() const {
        if (isEvalFrame())
            return ((Value*)this)[-1];

        MOZ_ASSERT(isFunctionFrame());

        if (callee().isArrow())
            return callee().getExtendedSlot(FunctionExtended::ARROW_NEWTARGET_SLOT);

        if (isConstructing()) {
            unsigned pushedArgs = Max(numFormalArgs(), numActualArgs());
            return argv()[pushedArgs];
        }
        return UndefinedValue();
    }

    /* Profiler flags */

    bool hasPushedGeckoProfilerFrame() {
        return !!(flags_ & HAS_PUSHED_PROF_FRAME);
    }

    void setPushedGeckoProfilerFrame() {
        flags_ |= HAS_PUSHED_PROF_FRAME;
    }

    void unsetPushedGeckoProfilerFrame() {
        flags_ &= ~HAS_PUSHED_PROF_FRAME;
    }

    /* Return value */

    bool hasReturnValue() const {
        return flags_ & HAS_RVAL;
    }

    MutableHandleValue returnValue() {
        if (!hasReturnValue())
            rval_.setUndefined();
        return MutableHandleValue::fromMarkedLocation(&rval_);
    }

    void markReturnValue() {
        flags_ |= HAS_RVAL;
    }

    void setReturnValue(const Value& v) {
        rval_ = v;
        markReturnValue();
    }

    void clearReturnValue() {
        rval_.setUndefined();
        markReturnValue();
    }

    void resumeGeneratorFrame(JSObject* envChain) {
        MOZ_ASSERT(script()->isGenerator() || script()->isAsync());
        MOZ_ASSERT(isFunctionFrame());
        flags_ |= HAS_INITIAL_ENV;
        envChain_ = envChain;
    }

    /*
     * Other flags
     */

    bool isConstructing() const {
        return !!(flags_ & CONSTRUCTING);
    }

    void setResumedGenerator() {
        flags_ |= RESUMED_GENERATOR;
    }
    bool isResumedGenerator() const {
        return !!(flags_ & RESUMED_GENERATOR);
    }

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

    bool prevUpToDate() const {
        return !!(flags_ & PREV_UP_TO_DATE);
    }

    void setPrevUpToDate() {
        flags_ |= PREV_UP_TO_DATE;
    }

    void unsetPrevUpToDate() {
        flags_ &= ~PREV_UP_TO_DATE;
    }

    bool isDebuggee() const {
        return !!(flags_ & DEBUGGEE);
    }

    void setIsDebuggee() {
        flags_ |= DEBUGGEE;
    }

    inline void unsetIsDebuggee();

    bool hasCachedSavedFrame() const {
        return flags_ & HAS_CACHED_SAVED_FRAME;
    }
    void setHasCachedSavedFrame() {
        flags_ |= HAS_CACHED_SAVED_FRAME;
    }

  public:
    void trace(JSTracer* trc, Value* sp, jsbytecode* pc);
    void traceValues(JSTracer* trc, unsigned start, unsigned end);

    // Entered Baseline/Ion from the interpreter.
    bool runningInJit() const {
        return !!(flags_ & RUNNING_IN_JIT);
    }
    void setRunningInJit() {
        flags_ |= RUNNING_IN_JIT;
    }
    void clearRunningInJit() {
        flags_ &= ~RUNNING_IN_JIT;
    }
};

/*****************************************************************************/

class InterpreterRegs
{
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

    /* For generators. */
    void rebaseFromTo(const InterpreterRegs& from, InterpreterFrame& to) {
        fp_ = &to;
        sp = to.slots() + (from.sp - from.fp_->slots());
        pc = from.pc;
        MOZ_ASSERT(fp_);
    }

    void popInlineFrame() {
        pc = fp_->prevpc();
        unsigned spForNewTarget = fp_->isResumedGenerator() ? 0 : fp_->isConstructing();
        sp = fp_->prevsp() - fp_->numActualArgs() - 1 - spForNewTarget;
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

    friend void GDBTestInitInterpreterRegs(InterpreterRegs&, js::InterpreterFrame*,
                                           JS::Value*, uint8_t*);
};

/*****************************************************************************/

class InterpreterStack
{
    friend class InterpreterActivation;

    static const size_t DEFAULT_CHUNK_SIZE = 4 * 1024;
    LifoAlloc allocator_;

    // Number of interpreter frames on the stack, for over-recursion checks.
    static const size_t MAX_FRAMES = 50 * 1000;
    static const size_t MAX_FRAMES_TRUSTED = MAX_FRAMES + 1000;
    size_t frameCount_;

    inline uint8_t* allocateFrame(JSContext* cx, size_t size);

    inline InterpreterFrame*
    getCallFrame(JSContext* cx, const CallArgs& args, HandleScript script,
                 MaybeConstruct constructing, Value** pargv);

    void releaseFrame(InterpreterFrame* fp) {
        frameCount_--;
        allocator_.release(fp->mark_);
    }

  public:
    InterpreterStack()
      : allocator_(DEFAULT_CHUNK_SIZE),
        frameCount_(0)
    { }

    ~InterpreterStack() {
        MOZ_ASSERT(frameCount_ == 0);
    }

    // For execution of eval or global code.
    InterpreterFrame* pushExecuteFrame(JSContext* cx, HandleScript script,
                                       const Value& newTargetValue, HandleObject envChain,
                                       AbstractFramePtr evalInFrame);

    // Called to invoke a function.
    InterpreterFrame* pushInvokeFrame(JSContext* cx, const CallArgs& args,
                                      MaybeConstruct constructing);

    // The interpreter can push light-weight, "inline" frames without entering a
    // new InterpreterActivation or recursively calling Interpret.
    bool pushInlineFrame(JSContext* cx, InterpreterRegs& regs, const CallArgs& args,
                         HandleScript script, MaybeConstruct constructing);

    void popInlineFrame(InterpreterRegs& regs);

    bool resumeGeneratorCallFrame(JSContext* cx, InterpreterRegs& regs,
                                  HandleFunction callee, HandleValue newTarget,
                                  HandleObject envChain);

    inline void purge(JSRuntime* rt);

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return allocator_.sizeOfExcludingThis(mallocSizeOf);
    }
};

// CooperatingContext is a wrapper for a JSContext that is participating in
// cooperative scheduling and may be different from the current thread. It is
// in place to make it clearer when we might be operating on another thread,
// and harder to accidentally pass in another thread's context to an API that
// expects the current thread's context.
class CooperatingContext
{
    JSContext* cx;

  public:
    explicit CooperatingContext(JSContext* cx) : cx(cx) {}
    JSContext* context() const { return cx; }

    // For &cx. The address should not be taken for other CooperatingContexts.
    friend class ZoneGroup;
};

void TraceInterpreterActivations(JSContext* cx, const CooperatingContext& target, JSTracer* trc);

/*****************************************************************************/

/** Base class for all function call args. */
class AnyInvokeArgs : public JS::CallArgs
{
};

/** Base class for all function construction args. */
class AnyConstructArgs : public JS::CallArgs
{
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
  : public mozilla::Conditional<Construct, AnyConstructArgs, AnyInvokeArgs>::Type
{
  protected:
    AutoValueVector v_;

    explicit GenericArgsBase(JSContext* cx) : v_(cx) {}

  public:
    bool init(JSContext* cx, unsigned argc) {
        if (argc > ARGS_LENGTH_MAX) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TOO_MANY_ARGUMENTS);
            return false;
        }

        // callee, this, arguments[, new.target iff constructing]
        size_t len = 2 + argc + uint32_t(Construct);
        MOZ_ASSERT(len > argc);  // no overflow
        if (!v_.resize(len))
            return false;

        *static_cast<JS::CallArgs*>(this) = CallArgsFromVp(argc, v_.begin());
        this->constructing_ = Construct;
        if (Construct)
            this->CallArgs::setThis(MagicValue(JS_IS_CONSTRUCTING));
        return true;
    }
};

/** Function call/construct args of statically-known count. */
template <MaybeConstruct Construct, size_t N>
class FixedArgsBase
  : public mozilla::Conditional<Construct, AnyConstructArgs, AnyInvokeArgs>::Type
{
    static_assert(N <= ARGS_LENGTH_MAX, "o/~ too many args o/~");

  protected:
    JS::AutoValueArray<2 + N + uint32_t(Construct)> v_;

    explicit FixedArgsBase(JSContext* cx) : v_(cx) {
        *static_cast<JS::CallArgs*>(this) = CallArgsFromVp(N, v_.begin());
        this->constructing_ = Construct;
        if (Construct)
            this->CallArgs::setThis(MagicValue(JS_IS_CONSTRUCTING));
    }
};

} // namespace detail

/** Function call args of statically-unknown count. */
class InvokeArgs : public detail::GenericArgsBase<NO_CONSTRUCT>
{
    using Base = detail::GenericArgsBase<NO_CONSTRUCT>;

  public:
    explicit InvokeArgs(JSContext* cx) : Base(cx) {}
};

/** Function call args of statically-unknown count. */
class InvokeArgsMaybeIgnoresReturnValue : public detail::GenericArgsBase<NO_CONSTRUCT>
{
    using Base = detail::GenericArgsBase<NO_CONSTRUCT>;

  public:
    explicit InvokeArgsMaybeIgnoresReturnValue(JSContext* cx, bool ignoresReturnValue) : Base(cx) {
        this->ignoresReturnValue_ = ignoresReturnValue;
    }
};

/** Function call args of statically-known count. */
template <size_t N>
class FixedInvokeArgs : public detail::FixedArgsBase<NO_CONSTRUCT, N>
{
    using Base = detail::FixedArgsBase<NO_CONSTRUCT, N>;

  public:
    explicit FixedInvokeArgs(JSContext* cx) : Base(cx) {}
};

/** Function construct args of statically-unknown count. */
class ConstructArgs : public detail::GenericArgsBase<CONSTRUCT>
{
    using Base = detail::GenericArgsBase<CONSTRUCT>;

  public:
    explicit ConstructArgs(JSContext* cx) : Base(cx) {}
};

/** Function call args of statically-known count. */
template <size_t N>
class FixedConstructArgs : public detail::FixedArgsBase<CONSTRUCT, N>
{
    using Base = detail::FixedArgsBase<CONSTRUCT, N>;

  public:
    explicit FixedConstructArgs(JSContext* cx) : Base(cx) {}
};

template <class Args, class Arraylike>
inline bool
FillArgumentsFromArraylike(JSContext* cx, Args& args, const Arraylike& arraylike)
{
    uint32_t len = arraylike.length();
    if (!args.init(cx, len))
        return false;

    for (uint32_t i = 0; i < len; i++)
        args[i].set(arraylike[i]);

    return true;
}

template <>
struct DefaultHasher<AbstractFramePtr> {
    typedef AbstractFramePtr Lookup;

    static js::HashNumber hash(const Lookup& key) {
        return mozilla::HashGeneric(key.raw());
    }

    static bool match(const AbstractFramePtr& k, const Lookup& l) {
        return k == l;
    }
};

/*****************************************************************************/

// SavedFrame caching to minimize stack walking.
//
// Since each SavedFrame object includes a 'parent' pointer to the SavedFrame
// for its caller, if we could easily find the right SavedFrame for a given
// stack frame, we wouldn't need to walk the rest of the stack. Traversing deep
// stacks can be expensive, and when we're profiling or instrumenting code, we
// may want to capture JavaScript stacks frequently, so such cases would benefit
// if we could avoid walking the entire stack.
//
// We could have a cache mapping frame addresses to their SavedFrame objects,
// but invalidating its entries would be a challenge. Popping a stack frame is
// extremely performance-sensitive, and SpiderMonkey stack frames can be OSR'd,
// thrown, rematerialized, and perhaps meet other fates; we would rather our
// cache not depend on handling so many tricky cases.
//
// It turns out that we can keep the cache accurate by reserving a single bit in
// the stack frame, which must be clear on any newly pushed frame. When we
// insert an entry into the cache mapping a given frame address to its
// SavedFrame, we set the bit in the frame. Then, we take care to probe the
// cache only for frames whose bit is set; the bit tells us that the frame has
// never left the stack, so its cache entry must be accurate, at least about
// which function the frame is executing (the line may have changed; more about
// that below). The code refers to this bit as the 'hasCachedSavedFrame' flag.
//
// We could manage such a cache replacing least-recently used entries, but we
// can do better than that: the cache can be a stack, of which we need examine
// only entries from the top.
//
// First, observe that stacks are walked from the youngest frame to the oldest,
// but SavedFrame chains are built from oldest to youngest, to ensure common
// tails are shared. This means that capturing a stack is necessarily a
// two-phase process: walk the stack, and then build the SavedFrames.
//
// Naturally, the first time we capture the stack, the cache is empty, and we
// must traverse the entire stack. As we build each SavedFrame, we push an entry
// associating the frame's address to its SavedFrame on the cache, and set the
// frame's bit. At the end, every frame has its bit set and an entry in the
// cache.
//
// Then the program runs some more. Some, none, or all of the frames are popped.
// Any new frames are pushed with their bit clear. Any frame with its bit set
// has never left the stack. The cache is left untouched.
//
// For the next capture, we walk the stack up to the first frame with its bit
// set, if there is one. Call it F; it must have a cache entry. We pop entries
// from the cache - all invalid, because they are above F's entry, and hence
// younger - until we find the entry matching F's address. Since F's bit is set,
// we know it never left the stack, and hence that no younger frame could have
// had a colliding address. And since the frame's bit was set when we pushed the
// cache entry, we know the entry is still valid.
//
// F's cache entry's SavedFrame covers the rest of the stack, so we don't need
// to walk the stack any further. Now we begin building SavedFrame objects for
// the new frames, pushing cache entries, and setting bits on the frames. By the
// end, the cache again covers the full stack, and every frame's bit is set.
//
// If we walk the stack to the end, and find no frame with its bit set, then the
// entire cache is invalid. At this point, it must be emptied, so that the new
// entries we are about to push are the only frames in the cache.
//
// For example, suppose we have the following stack (let 'A > B' mean "A called
// B", so the frames are listed oldest first):
//
//     P  > Q  > R  > S          Initial stack, bits not set.
//     P* > Q* > R* > S*         Capture a SavedFrame stack, set bits.
//                               The cache now holds: P > Q > R > S.
//     P* > Q* > R*              Return from S.
//     P* > Q*                   Return from R.
//     P* > Q* > T  > U          Call T and U. New frames have clear bits.
//
// If we capture the stack now, the cache still holds:
//
//     P  > Q  > R  > S
//
// As we traverse the stack, we'll cross U and T, and then find Q with its bit
// set. We pop entries from the cache until we find the entry for Q; this
// removes entries R and S, which were indeed invalid. In Q's cache entry, we
// find the SavedFrame representing the stack P > Q. Now we build SavedFrames
// for the new portion of the stack, pushing an entry for T and setting the bit
// on the frame, and then doing the same for U. In the end, the call stack again
// has bits set on all its frames:
//
//     P* > Q* > T* > U*         All frames are now in the cache.
//
// And the cache again holds entries for the entire stack:
//
//     P  > Q  > T  > U
//
// Some details:
//
// - When we find a cache entry whose frame address matches our frame F, we know
//   that F has never left the stack, but it may certainly be the case that
//   execution took place in that frame, and that the current source position
//   within F's function has changed. This means that the entry's SavedFrame,
//   which records the source line and column as well as the function, is not
//   correct. To detect this case, when we push a cache entry, we record the
//   frame's pc. When consulting the cache, if a frame's address matches but its
//   pc does not, then we pop the cache entry and continue walking the stack.
//   The next stack frame will definitely hit: since its callee frame never left
//   the stack, the calling frame never got the chance to execute.
//
// - Generators, at least conceptually, have long-lived stack frames that
//   disappear from the stack when the generator yields, and reappear on the
//   stack when the generator's 'next' method is called. When a generator's
//   frame is placed again atop the stack, its bit must be cleared - for the
//   purposes of the cache, treating the frame as a new frame - to respect the
//   invariants we used to justify the algorithm above. Async function
//   activations usually appear atop empty stacks, since they are invoked as a
//   promise callback, but the same rule applies.
//
// - SpiderMonkey has many types of stack frames, and not all have a place to
//   store a bit indicating a cached SavedFrame. But as long as we don't create
//   cache entries for frames we can't mark, simply omitting them from the cache
//   is harmless. Uncacheable frame types include inlined Ion frames and
//   non-Debug wasm frames. The LiveSavedFrameCache::FramePtr type represents
//   only pointers to frames that can be cached, so if you have a FramePtr, you
//   don't need to further check the frame for cachability. FramePtr provides
//   access to the hasCachedSavedFrame bit.
//
// - We actually break up the cache into one cache per Activation. Popping an
//   activation invalidates all its cache entries, simply by freeing the cache
//   altogether.
//
// - The entire chain of SavedFrames for a given stack capture is created in the
//   compartment of the code that requested the capture, *not* in that of the
//   frames it represents, so in general, different compartments may have
//   different SavedFrame objects representing the same actual stack frame. The
//   LiveSavedFrameCache simply records whichever SavedFrames were created most
//   recently. When we find a cache hit, we check the entry's SavedFrame's
//   compartment against the current compartment; if they do not match, we flush
//   the entire cache. This means that it is not always true that, if a frame's
//   bit it set, it must have an entry in the cache. But we can still assert
//   that, if a frame's bit is set and the cache is not completely empty, the
//   frame will have an entry. When the cache is flushed, it will be repopulated
//   immediately with the new capture's frames.
//
// - When the Debugger API evaluates an expression in some frame (the 'target
//   frame'), it's SpiderMonkey's convention that the target frame be treated as
//   the parent of the eval frame. In reality, of course, the eval frame is
//   pushed on the top of the stack like any other frame, but stack captures
//   simply jump straight over the intervening frames, so that the '.parent'
//   property of a SavedFrame for the eval is the SavedFrame for the target.
//   This is arranged by giving the eval frame an 'evalInFramePrev` link
//   pointing to the target, which an ordinary FrameIter will notice and
//   respect.
//
//   If the LiveSavedFrameCache were presented with stack traversals that
//   skipped frames in this way, it would cause havoc. First, with no debugger
//   eval frames present, capture the stack, populating the cache. Then push a
//   debugger eval frame and capture again; the skipped frames to appear to be
//   absent from the stack. Now pop the debugger eval frame, and capture a third
//   time: the no-longer-skipped frames seem to reappear on the stack, with
//   their cached bits still set.
//
//   The LiveSavedFrameCache assumes that the stack it sees is used in a
//   stack-like fashion: if a frame has its bit set, it has never left the
//   stack. To support this assumption, when the cache is in use, we do not skip
//   the frames between a debugger eval frame an its target; we always traverse
//   the entire stack, invalidating and populating the cache in the usual way.
//   Instead, when we construct a SavedFrame for a debugger eval frame, we
//   select the appropriate parent at that point: rather than the next-older
//   frame, we find the SavedFrame for the eval's target frame. The skip appears
//   in the SavedFrame chains, even as the traversal covers all the frames.
class LiveSavedFrameCache
{
  public:
    // The address of a live frame for which we can cache SavedFrames: it has a
    // 'hasCachedSavedFrame' bit we can examine and set, and can be converted to
    // a Key to index the cache.
    class FramePtr {
        // We use jit::CommonFrameLayout for both Baseline frames and Ion
        // physical frames.
        using Ptr = mozilla::Variant<InterpreterFrame*,
                                     jit::CommonFrameLayout*,
                                     jit::RematerializedFrame*,
                                     wasm::DebugFrame*>;

        Ptr ptr;

        template<typename Frame>
        explicit FramePtr(Frame ptr) : ptr(ptr) { }

        struct HasCachedMatcher;
        struct SetHasCachedMatcher;

      public:
        // If iter's frame is of a type that can be cached, construct a FramePtr
        // for its frame. Otherwise, return Nothing.
        static inline mozilla::Maybe<FramePtr> create(const FrameIter& iter);

        // Construct a FramePtr from an AbstractFramePtr. This always succeeds.
        static inline FramePtr create(AbstractFramePtr abstractFramePtr);

        inline bool hasCachedSavedFrame() const;
        inline void setHasCachedSavedFrame();

        // Return true if this FramePtr refers to an interpreter frame.
        inline bool isInterpreterFrame() const { return ptr.is<InterpreterFrame*>(); }

        // If this FramePtr is an interpreter frame, return a pointer to it.
        inline InterpreterFrame& asInterpreterFrame() const { return *ptr.as<InterpreterFrame*>(); }

        bool operator==(const FramePtr& rhs) const { return rhs.ptr == this->ptr; }
        bool operator!=(const FramePtr& rhs) const { return !(rhs == *this); }
    };

  private:
    // A key in the cache: the address of a frame, live or dead, for which we
    // can cache SavedFrames. Since the pointer may not be live, the only
    // operation this type permits is comparison.
    class Key {
        FramePtr framePtr;

      public:
        MOZ_IMPLICIT Key(const FramePtr& framePtr) : framePtr(framePtr) { }

        bool operator==(const Key& rhs) const { return rhs.framePtr == this->framePtr; }
        bool operator!=(const Key& rhs) const { return !(rhs == *this); }
    };

    struct Entry
    {
        const Key            key;
        const jsbytecode*    pc;
        HeapPtr<SavedFrame*> savedFrame;

        Entry(const Key& key, const jsbytecode* pc, SavedFrame* savedFrame)
          : key(key)
          , pc(pc)
          , savedFrame(savedFrame)
        { }
    };

    using EntryVector = Vector<Entry, 0, SystemAllocPolicy>;
    EntryVector* frames;

    LiveSavedFrameCache(const LiveSavedFrameCache&) = delete;
    LiveSavedFrameCache& operator=(const LiveSavedFrameCache&) = delete;

  public:
    explicit LiveSavedFrameCache() : frames(nullptr) { }

    LiveSavedFrameCache(LiveSavedFrameCache&& rhs)
        : frames(rhs.frames)
    {
        MOZ_ASSERT(this != &rhs, "self-move disallowed");
        rhs.frames = nullptr;
    }

    ~LiveSavedFrameCache() {
        if (frames) {
            js_delete(frames);
            frames = nullptr;
        }
    }

    bool initialized() const { return !!frames; }
    bool init(JSContext* cx) {
        frames = js_new<EntryVector>();
        if (!frames) {
            JS_ReportOutOfMemory(cx);
            return false;
        }
        return true;
    }

    void trace(JSTracer* trc);

    // Set |frame| to the cached SavedFrame corresponding to |framePtr| at |pc|.
    // |framePtr|'s hasCachedSavedFrame bit must be set. Remove all cache
    // entries for frames younger than that one.
    //
    // This may set |frame| to nullptr if |pc| is different from the pc supplied
    // when the cache entry was inserted. In this case, the cached SavedFrame
    // (probably) has the wrong source position. Entries for younger frames are
    // still removed. The next frame, if any, will be a cache hit.
    //
    // This may also set |frame| to nullptr if the cache was populated with
    // SavedFrame objects for a different compartment than cx's current
    // compartment. In this case, the entire cache is flushed.
    void find(JSContext* cx, FramePtr& framePtr, const jsbytecode* pc,
              MutableHandleSavedFrame frame) const;

    // Search the cache for a frame matching |framePtr|, without removing any
    // entries. Return the matching saved frame, or nullptr if none is found.
    // This is used for resolving |evalInFramePrev| links.
    void findWithoutInvalidation(const FramePtr& framePtr, MutableHandleSavedFrame frame) const;

    // Push a cache entry mapping |framePtr| and |pc| to |savedFrame| on the top
    // of the cache's stack. You must insert entries for frames from oldest to
    // youngest. They must all be younger than the frame that the |find| method
    // found a hit for; or you must have cleared the entire cache with the
    // |clear| method.
    bool insert(JSContext* cx, FramePtr& framePtr, const jsbytecode* pc,
                HandleSavedFrame savedFrame);

    // Remove all entries from the cache.
    void clear() { if (frames) frames->clear(); }
};

static_assert(sizeof(LiveSavedFrameCache) == sizeof(uintptr_t),
              "Every js::Activation has a LiveSavedFrameCache, so we need to be pretty careful "
              "about avoiding bloat. If you're adding members to LiveSavedFrameCache, maybe you "
              "should consider figuring out a way to make js::Activation have a "
              "LiveSavedFrameCache* instead of a Rooted<LiveSavedFrameCache>.");

/*****************************************************************************/

class InterpreterActivation;

namespace jit {
    class JitActivation;
} // namespace jit

// This class is separate from Activation, because it calls JSCompartment::wrap()
// which can GC and walk the stack. It's not safe to do that within the
// JitActivation constructor.
class MOZ_RAII ActivationEntryMonitor
{
    JSContext* cx_;

    // The entry point monitor that was set on cx_->runtime() when this
    // ActivationEntryMonitor was created.
    JS::dbg::AutoEntryMonitor* entryMonitor_;

    explicit ActivationEntryMonitor(JSContext* cx);

    ActivationEntryMonitor(const ActivationEntryMonitor& other) = delete;
    void operator=(const ActivationEntryMonitor& other) = delete;

    Value asyncStack(JSContext* cx);

  public:
    ActivationEntryMonitor(JSContext* cx, InterpreterFrame* entryFrame);
    ActivationEntryMonitor(JSContext* cx, jit::CalleeToken entryToken);
    inline ~ActivationEntryMonitor();
};

class Activation
{
  protected:
    JSContext* cx_;
    JSCompartment* compartment_;
    Activation* prev_;
    Activation* prevProfiling_;

    // Counter incremented by JS::HideScriptedCaller and decremented by
    // JS::UnhideScriptedCaller. If > 0 for the top activation,
    // DescribeScriptedCaller will return null instead of querying that
    // activation, which should prompt the caller to consult embedding-specific
    // data structures instead.
    size_t hideScriptedCallerCount_;

    // The cache of SavedFrame objects we have already captured when walking
    // this activation's stack.
    Rooted<LiveSavedFrameCache> frameCache_;

    // Youngest saved frame of an async stack that will be iterated during stack
    // capture in place of the actual stack of previous activations. Note that
    // the stack of this activation is captured entirely before this is used.
    //
    // Usually this is nullptr, meaning that normal stack capture will occur.
    // When this is set, the stack of any previous activation is ignored.
    Rooted<SavedFrame*> asyncStack_;

    // Value of asyncCause to be attached to asyncStack_.
    const char* asyncCause_;

    // True if the async call was explicitly requested, e.g. via
    // callFunctionWithAsyncStack.
    bool asyncCallIsExplicit_;

    enum Kind { Interpreter, Jit };
    Kind kind_;

    inline Activation(JSContext* cx, Kind kind);
    inline ~Activation();

  public:
    JSContext* cx() const {
        return cx_;
    }
    JSCompartment* compartment() const {
        return compartment_;
    }
    Activation* prev() const {
        return prev_;
    }
    Activation* prevProfiling() const { return prevProfiling_; }
    inline Activation* mostRecentProfiling();

    bool isInterpreter() const {
        return kind_ == Interpreter;
    }
    bool isJit() const {
        return kind_ == Jit;
    }
    inline bool hasWasmExitFP() const;

    inline bool isProfiling() const;
    void registerProfiling();
    void unregisterProfiling();

    InterpreterActivation* asInterpreter() const {
        MOZ_ASSERT(isInterpreter());
        return (InterpreterActivation*)this;
    }
    jit::JitActivation* asJit() const {
        MOZ_ASSERT(isJit());
        return (jit::JitActivation*)this;
    }

    void hideScriptedCaller() {
        hideScriptedCallerCount_++;
    }
    void unhideScriptedCaller() {
        MOZ_ASSERT(hideScriptedCallerCount_ > 0);
        hideScriptedCallerCount_--;
    }
    bool scriptedCallerIsHidden() const {
        return hideScriptedCallerCount_ > 0;
    }

    static size_t offsetOfPrev() {
        return offsetof(Activation, prev_);
    }
    static size_t offsetOfPrevProfiling() {
        return offsetof(Activation, prevProfiling_);
    }

    SavedFrame* asyncStack() {
        return asyncStack_;
    }

    const char* asyncCause() const {
        return asyncCause_;
    }

    bool asyncCallIsExplicit() const {
        return asyncCallIsExplicit_;
    }

    inline LiveSavedFrameCache* getLiveSavedFrameCache(JSContext* cx);
    void clearLiveSavedFrameCache() { frameCache_.get().clear(); }

  private:
    Activation(const Activation& other) = delete;
    void operator=(const Activation& other) = delete;
};

// This variable holds a special opcode value which is greater than all normal
// opcodes, and is chosen such that the bitwise or of this value with any
// opcode is this value.
static const jsbytecode EnableInterruptsPseudoOpcode = -1;

static_assert(EnableInterruptsPseudoOpcode >= JSOP_LIMIT,
              "EnableInterruptsPseudoOpcode must be greater than any opcode");
static_assert(EnableInterruptsPseudoOpcode == jsbytecode(-1),
              "EnableInterruptsPseudoOpcode must be the maximum jsbytecode value");

class InterpreterFrameIterator;
class RunState;

class InterpreterActivation : public Activation
{
    friend class js::InterpreterFrameIterator;

    InterpreterRegs regs_;
    InterpreterFrame* entryFrame_;
    size_t opMask_; // For debugger interrupts, see js::Interpret.

#ifdef DEBUG
    size_t oldFrameCount_;
#endif

  public:
    inline InterpreterActivation(RunState& state, JSContext* cx, InterpreterFrame* entryFrame);
    inline ~InterpreterActivation();

    inline bool pushInlineFrame(const CallArgs& args, HandleScript script,
                                MaybeConstruct constructing);
    inline void popInlineFrame(InterpreterFrame* frame);

    inline bool resumeGeneratorFrame(HandleFunction callee, HandleValue newTarget,
                                     HandleObject envChain);

    InterpreterFrame* current() const {
        return regs_.fp();
    }
    InterpreterRegs& regs() {
        return regs_;
    }
    InterpreterFrame* entryFrame() const {
        return entryFrame_;
    }
    size_t opMask() const {
        return opMask_;
    }

    bool isProfiling() const {
        return false;
    }

    // If this js::Interpret frame is running |script|, enable interrupts.
    void enableInterruptsIfRunning(JSScript* script) {
        if (regs_.fp()->script() == script)
            enableInterruptsUnconditionally();
    }
    void enableInterruptsUnconditionally() {
        opMask_ = EnableInterruptsPseudoOpcode;
    }
    void clearInterruptsMask() {
        opMask_ = 0;
    }
};

// Iterates over a thread's activation list.
class ActivationIterator
{
  protected:
    Activation* activation_;

  public:
    explicit ActivationIterator(JSContext* cx);

    // ActivationIterator can be used to iterate over a different thread's
    // activations, for use by the GC, invalidation, and other operations that
    // don't have a user-visible effect on the target thread's JS behavior.
    ActivationIterator(JSContext* cx, const CooperatingContext& target);

    ActivationIterator& operator++();

    Activation* operator->() const {
        return activation_;
    }
    Activation* activation() const {
        return activation_;
    }
    bool done() const {
        return activation_ == nullptr;
    }
};

namespace jit {

class BailoutFrameInfo;

// A JitActivation is used for frames running in Baseline or Ion.
class JitActivation : public Activation
{
  public:
    static const uintptr_t ExitFpWasmBit = 0x1;

  private:
    // If Baseline, Ion or Wasm code is on the stack, and has called into C++,
    // this will be aligned to an ExitFrame. The last bit indicates if it's a
    // wasm frame (bit set to ExitFpWasmBit) or not (bit set to !ExitFpWasmBit).
    uint8_t* packedExitFP_;

    // When hasWasmExitFP(), encodedWasmExitReason_ holds ExitReason.
    uint32_t encodedWasmExitReason_;

    JitActivation* prevJitActivation_;

    // Rematerialized Ion frames which has info copied out of snapshots. Maps
    // frame pointers (i.e. packedExitFP_) to a vector of rematerializations of all
    // inline frames associated with that frame.
    //
    // This table is lazily initialized by calling getRematerializedFrame.
    typedef GCVector<RematerializedFrame*> RematerializedFrameVector;
    typedef HashMap<uint8_t*, RematerializedFrameVector> RematerializedFrameTable;
    RematerializedFrameTable* rematerializedFrames_;

    // This vector is used to remember the outcome of the evaluation of recover
    // instructions.
    //
    // RInstructionResults are appended into this vector when Snapshot values
    // have to be read, or when the evaluation has to run before some mutating
    // code.  Each RInstructionResults belongs to one frame which has to bailout
    // as soon as we get back to it.
    typedef Vector<RInstructionResults, 1> IonRecoveryMap;
    IonRecoveryMap ionRecovery_;

    // If we are bailing out from Ion, then this field should be a non-null
    // pointer which references the BailoutFrameInfo used to walk the inner
    // frames. This field is used for all newly constructed JSJitFrameIters to
    // read the innermost frame information from this bailout data instead of
    // reading it from the stack.
    BailoutFrameInfo* bailoutData_;

    // When profiling is enabled, these fields will be updated to reflect the
    // last pushed frame for this activation, and if that frame has been
    // left for a call, the native code site of the call.
    mozilla::Atomic<void*, mozilla::Relaxed> lastProfilingFrame_;
    mozilla::Atomic<void*, mozilla::Relaxed> lastProfilingCallSite_;
    static_assert(sizeof(mozilla::Atomic<void*, mozilla::Relaxed>) == sizeof(void*),
                  "Atomic should have same memory format as underlying type.");

    void clearRematerializedFrames();

#ifdef CHECK_OSIPOINT_REGISTERS
  protected:
    // Used to verify that live registers don't change between a VM call and
    // the OsiPoint that follows it. Protected to silence Clang warning.
    uint32_t checkRegs_;
    RegisterDump regs_;
#endif

  public:
    explicit JitActivation(JSContext* cx);
    ~JitActivation();

    bool isProfiling() const {
        // All JitActivations can be profiled.
        return true;
    }

    JitActivation* prevJitActivation() const {
        return prevJitActivation_;
    }
    static size_t offsetOfPrevJitActivation() {
        return offsetof(JitActivation, prevJitActivation_);
    }

    bool hasExitFP() const {
        return !!packedExitFP_;
    }
    static size_t offsetOfPackedExitFP() {
        return offsetof(JitActivation, packedExitFP_);
    }

    bool hasJSExitFP() const {
        return !(uintptr_t(packedExitFP_) & ExitFpWasmBit);
    }
    uint8_t* jsExitFP() const {
        MOZ_ASSERT(hasJSExitFP());
        return packedExitFP_;
    }
    void setJSExitFP(uint8_t* fp) {
        packedExitFP_ = fp;
    }

#ifdef CHECK_OSIPOINT_REGISTERS
    void setCheckRegs(bool check) {
        checkRegs_ = check;
    }
    static size_t offsetOfCheckRegs() {
        return offsetof(JitActivation, checkRegs_);
    }
    static size_t offsetOfRegs() {
        return offsetof(JitActivation, regs_);
    }
#endif

    // Look up a rematerialized frame keyed by the fp, rematerializing the
    // frame if one doesn't already exist. A frame can only be rematerialized
    // if an IonFrameIterator pointing to the nearest uninlined frame can be
    // provided, as values need to be read out of snapshots.
    //
    // The inlineDepth must be within bounds of the frame pointed to by iter.
    RematerializedFrame* getRematerializedFrame(JSContext* cx, const JSJitFrameIter& iter,
                                                size_t inlineDepth = 0);

    // Look up a rematerialized frame by the fp. If inlineDepth is out of
    // bounds of what has been rematerialized, nullptr is returned.
    RematerializedFrame* lookupRematerializedFrame(uint8_t* top, size_t inlineDepth = 0);

    // Remove all rematerialized frames associated with the fp top from the
    // Debugger.
    void removeRematerializedFramesFromDebugger(JSContext* cx, uint8_t* top);

    bool hasRematerializedFrame(uint8_t* top, size_t inlineDepth = 0) {
        return !!lookupRematerializedFrame(top, inlineDepth);
    }

    // Remove a previous rematerialization by fp.
    void removeRematerializedFrame(uint8_t* top);

    void traceRematerializedFrames(JSTracer* trc);

    // Register the results of on Ion frame recovery.
    bool registerIonFrameRecovery(RInstructionResults&& results);

    // Return the pointer to the Ion frame recovery, if it is already registered.
    RInstructionResults* maybeIonFrameRecovery(JitFrameLayout* fp);

    // If an Ion frame recovery exists for the |fp| frame exists, then remove it
    // from the activation.
    void removeIonFrameRecovery(JitFrameLayout* fp);

    void traceIonRecovery(JSTracer* trc);

    // Return the bailout information if it is registered.
    const BailoutFrameInfo* bailoutData() const { return bailoutData_; }

    // Register the bailout data when it is constructed.
    void setBailoutData(BailoutFrameInfo* bailoutData);

    // Unregister the bailout data when the frame is reconstructed.
    void cleanBailoutData();

    static size_t offsetOfLastProfilingFrame() {
        return offsetof(JitActivation, lastProfilingFrame_);
    }
    void* lastProfilingFrame() {
        return lastProfilingFrame_;
    }
    void setLastProfilingFrame(void* ptr) {
        lastProfilingFrame_ = ptr;
    }

    static size_t offsetOfLastProfilingCallSite() {
        return offsetof(JitActivation, lastProfilingCallSite_);
    }
    void* lastProfilingCallSite() {
        return lastProfilingCallSite_;
    }
    void setLastProfilingCallSite(void* ptr) {
        lastProfilingCallSite_ = ptr;
    }

    // WebAssembly specific attributes.
    bool hasWasmExitFP() const {
        return uintptr_t(packedExitFP_) & ExitFpWasmBit;
    }
    wasm::Frame* wasmExitFP() const {
        MOZ_ASSERT(hasWasmExitFP());
        return (wasm::Frame*)(uintptr_t(packedExitFP_) & ~ExitFpWasmBit);
    }
    void setWasmExitFP(const wasm::Frame* fp) {
        if (fp) {
            MOZ_ASSERT(!(uintptr_t(fp) & ExitFpWasmBit));
            packedExitFP_ = (uint8_t*)(uintptr_t(fp) | ExitFpWasmBit);
            MOZ_ASSERT(hasWasmExitFP());
        } else {
            packedExitFP_ = nullptr;
        }
    }
    wasm::ExitReason wasmExitReason() const {
        MOZ_ASSERT(hasWasmExitFP());
        return wasm::ExitReason::Decode(encodedWasmExitReason_);
    }
    static size_t offsetOfEncodedWasmExitReason() {
        return offsetof(JitActivation, encodedWasmExitReason_);
    }

    // Interrupts are started from the interrupt signal handler (or the ARM
    // simulator) and cleared by WasmHandleExecutionInterrupt or WasmHandleThrow
    // when the interrupt is handled.

    // Returns true iff we've entered interrupted state.
    bool startWasmInterrupt(const wasm::RegisterState& state);
    void finishWasmInterrupt();
    bool isWasmInterrupted() const;
    void* wasmInterruptUnwindPC() const;
    void* wasmInterruptResumePC() const;

    void startWasmTrap(wasm::Trap trap, uint32_t bytecodeOffset, const wasm::RegisterState& state);
    void finishWasmTrap();
    bool isWasmTrapping() const;
    void* wasmTrapPC() const;
    uint32_t wasmTrapBytecodeOffset() const;
};

// A filtering of the ActivationIterator to only stop at JitActivations.
class JitActivationIterator : public ActivationIterator
{
    void settle() {
        while (!done() && !activation_->isJit())
            ActivationIterator::operator++();
    }

  public:
    explicit JitActivationIterator(JSContext* cx)
      : ActivationIterator(cx)
    {
        settle();
    }

    JitActivationIterator(JSContext* cx, const CooperatingContext& target)
      : ActivationIterator(cx, target)
    {
        settle();
    }

    JitActivationIterator& operator++() {
        ActivationIterator::operator++();
        settle();
        return *this;
    }
};

} // namespace jit

inline bool
Activation::hasWasmExitFP() const
{
    return isJit() && asJit()->hasWasmExitFP();
}

// Iterates over the frames of a single InterpreterActivation.
class InterpreterFrameIterator
{
    InterpreterActivation* activation_;
    InterpreterFrame* fp_;
    jsbytecode* pc_;
    Value* sp_;

  public:
    explicit InterpreterFrameIterator(InterpreterActivation* activation)
      : activation_(activation),
        fp_(nullptr),
        pc_(nullptr),
        sp_(nullptr)
    {
        if (activation) {
            fp_ = activation->current();
            pc_ = activation->regs().pc;
            sp_ = activation->regs().sp;
        }
    }

    InterpreterFrame* frame() const {
        MOZ_ASSERT(!done());
        return fp_;
    }
    jsbytecode* pc() const {
        MOZ_ASSERT(!done());
        return pc_;
    }
    Value* sp() const {
        MOZ_ASSERT(!done());
        return sp_;
    }

    InterpreterFrameIterator& operator++();

    bool done() const {
        return fp_ == nullptr;
    }
};

// A JitFrameIter can iterate over all kind of frames emitted by our code
// generators, be they composed of JS jit frames or wasm frames, interleaved or
// not, in any order.
//
// In the following class:
// - code generated for JS is referred to as JSJit.
// - code generated for wasm is referred to as Wasm.
// Also, Jit refers to any one of them.
//
// JitFrameIter uses JSJitFrameIter to iterate over JSJit code or a
// WasmFrameIter to iterate over wasm code; only one of them is active at the
// time. When a sub-iterator is done, the JitFrameIter knows how to stop, move
// onto the next activation or move onto another kind of Jit code.
//
// For ease of use, there is also OnlyJSJitFrameIter, which skips all the
// non-JSJit frames.
//
// Note it is allowed to get a handle to the internal frame iterator via
// asJSJit() and asWasm(), but the user has to be careful not to have those be
// used after JitFrameIter leaves the scope or the operator++ is called.
//
// TODO(bug 1360211) In particular, this can handle the transition from wasm to
// ion and from ion to wasm, since these will be interleaved in the same
// JitActivation.
class JitFrameIter
{
  protected:
    jit::JitActivation* act_;
    mozilla::MaybeOneOf<jit::JSJitFrameIter, wasm::WasmFrameIter> iter_;
    bool mustUnwindActivation_;

    void settle();

  public:
    JitFrameIter() : act_(nullptr), iter_(), mustUnwindActivation_(false) {}
    explicit JitFrameIter(jit::JitActivation* activation, bool mustUnwindActivation = false);

    explicit JitFrameIter(const JitFrameIter& another);
    JitFrameIter& operator=(const JitFrameIter& another);

    bool isSome() const { return !iter_.empty(); }
    void reset() { MOZ_ASSERT(isSome()); iter_.destroy(); }

    bool isJSJit() const { return isSome() && iter_.constructed<jit::JSJitFrameIter>(); }
    jit::JSJitFrameIter& asJSJit() { return iter_.ref<jit::JSJitFrameIter>(); }
    const jit::JSJitFrameIter& asJSJit() const { return iter_.ref<jit::JSJitFrameIter>(); }

    bool isWasm() const { return isSome() && iter_.constructed<wasm::WasmFrameIter>(); }
    wasm::WasmFrameIter& asWasm() { return iter_.ref<wasm::WasmFrameIter>(); }
    const wasm::WasmFrameIter& asWasm() const { return iter_.ref<wasm::WasmFrameIter>(); }

    // Operations common to all frame iterators.
    const jit::JitActivation* activation() const { return act_; }
    bool done() const;
    void operator++();

    // Operations which have an effect only on JIT frames.
    void skipNonScriptedJSFrames();
};

// A JitFrameIter that skips all the non-JSJit frames, skipping interleaved
// frames of any another kind.

class OnlyJSJitFrameIter : public JitFrameIter
{
    void settle() {
        while (!done() && !isJSJit())
            JitFrameIter::operator++();
    }

  public:
    explicit OnlyJSJitFrameIter(jit::JitActivation* act);
    explicit OnlyJSJitFrameIter(JSContext* cx);
    explicit OnlyJSJitFrameIter(const ActivationIterator& cx);

    void operator++() {
        JitFrameIter::operator++();
        settle();
    }

    const jit::JSJitFrameIter& frame() const {
        return asJSJit();
    }
};

// A FrameIter walks over a context's stack of JS script activations,
// abstracting over whether the JS scripts were running in the interpreter or
// different modes of compiled code.
//
// FrameIter is parameterized by what it includes in the stack iteration:
//  - When provided, the optional JSPrincipal argument will cause FrameIter to
//    only show frames in globals whose JSPrincipals are subsumed (via
//    JSSecurityCallbacks::subsume) by the given JSPrincipal.
//
// Additionally, there are derived FrameIter types that automatically skip
// certain frames:
//  - ScriptFrameIter only shows frames that have an associated JSScript
//    (currently everything other than wasm stack frames). When !hasScript(),
//    clients must stick to the portion of the
//    interface marked below.
//  - NonBuiltinScriptFrameIter additionally filters out builtin (self-hosted)
//    scripts.
class FrameIter
{
  public:
    enum DebuggerEvalOption { FOLLOW_DEBUGGER_EVAL_PREV_LINK,
                              IGNORE_DEBUGGER_EVAL_PREV_LINK };

    enum State {
        DONE,      // when there are no more frames nor activations to unwind.
        INTERP,    // interpreter activation on the stack
        JIT        // jit or wasm activations on the stack
    };

    // Unlike ScriptFrameIter itself, ScriptFrameIter::Data can be allocated on
    // the heap, so this structure should not contain any GC things.
    struct Data
    {
        JSContext* cx_;
        DebuggerEvalOption  debuggerEvalOption_;
        JSPrincipals*       principals_;

        State               state_;

        jsbytecode*         pc_;

        InterpreterFrameIterator interpFrames_;
        ActivationIterator activations_;

        JitFrameIter jitFrames_;
        unsigned ionInlineFrameNo_;

        Data(JSContext* cx, DebuggerEvalOption debuggerEvalOption, JSPrincipals* principals);
        Data(JSContext* cx, const CooperatingContext& target, DebuggerEvalOption debuggerEvalOption);
        Data(const Data& other);
    };

    explicit FrameIter(JSContext* cx,
                       DebuggerEvalOption = FOLLOW_DEBUGGER_EVAL_PREV_LINK);
    FrameIter(JSContext* cx, const CooperatingContext&, DebuggerEvalOption);
    FrameIter(JSContext* cx, DebuggerEvalOption, JSPrincipals*);
    FrameIter(const FrameIter& iter);
    MOZ_IMPLICIT FrameIter(const Data& data);
    MOZ_IMPLICIT FrameIter(AbstractFramePtr frame);

    bool done() const { return data_.state_ == DONE; }

    // -------------------------------------------------------
    // The following functions can only be called when !done()
    // -------------------------------------------------------

    FrameIter& operator++();

    JSCompartment* compartment() const;
    Activation* activation() const { return data_.activations_.activation(); }

    bool isInterp() const {
        MOZ_ASSERT(!done());
        return data_.state_ == INTERP;
    }
    bool isJSJit() const {
        MOZ_ASSERT(!done());
        return data_.state_ == JIT && data_.jitFrames_.isJSJit();
    }
    bool isWasm() const {
        MOZ_ASSERT(!done());
        return data_.state_ == JIT && data_.jitFrames_.isWasm();
    }

    inline bool isIon() const;
    inline bool isBaseline() const;
    inline bool isPhysicalJitFrame() const;

    bool isEvalFrame() const;
    bool isFunctionFrame() const;
    bool hasArgs() const { return isFunctionFrame(); }

    ScriptSource* scriptSource() const;
    const char* filename() const;
    const char16_t* displayURL() const;
    unsigned computeLine(uint32_t* column = nullptr) const;
    JSAtom* functionDisplayAtom() const;
    bool mutedErrors() const;

    bool hasScript() const { return !isWasm(); }

    // -----------------------------------------------------------
    //  The following functions can only be called when isWasm()
    // -----------------------------------------------------------

    inline bool wasmDebugEnabled() const;
    inline wasm::Instance* wasmInstance() const;
    inline unsigned wasmBytecodeOffset() const;
    void wasmUpdateBytecodeOffset();

    // -----------------------------------------------------------
    // The following functions can only be called when hasScript()
    // -----------------------------------------------------------

    inline JSScript* script() const;

    bool        isConstructing() const;
    jsbytecode* pc() const { MOZ_ASSERT(!done()); return data_.pc_; }
    void        updatePcQuadratic();

    // The function |calleeTemplate()| returns either the function from which
    // the current |callee| was cloned or the |callee| if it can be read. As
    // long as we do not have to investigate the environment chain or build a
    // new frame, we should prefer to use |calleeTemplate| instead of
    // |callee|, as requesting the |callee| might cause the invalidation of
    // the frame. (see js::Lambda)
    JSFunction* calleeTemplate() const;
    JSFunction* callee(JSContext* cx) const;

    JSFunction* maybeCallee(JSContext* cx) const {
        return isFunctionFrame() ? callee(cx) : nullptr;
    }

    bool        matchCallee(JSContext* cx, HandleFunction fun) const;

    unsigned    numActualArgs() const;
    unsigned    numFormalArgs() const;
    Value       unaliasedActual(unsigned i, MaybeCheckAliasing = CHECK_ALIASING) const;
    template <class Op> inline void unaliasedForEachActual(JSContext* cx, Op op);

    JSObject*  environmentChain(JSContext* cx) const;
    CallObject& callObj(JSContext* cx) const;

    bool        hasArgsObj() const;
    ArgumentsObject& argsObj() const;

    // Get the original |this| value passed to this function. May not be the
    // actual this-binding (for instance, derived class constructors will
    // change their this-value later and non-strict functions will box
    // primitives).
    Value       thisArgument(JSContext* cx) const;

    Value       newTarget() const;

    Value       returnValue() const;
    void        setReturnValue(const Value& v);

    // These are only valid for the top frame.
    size_t      numFrameSlots() const;
    Value       frameSlotValue(size_t index) const;

    // Ensures that we have rematerialized the top frame and its associated
    // inline frames. Can only be called when isIon().
    bool ensureHasRematerializedFrame(JSContext* cx);

    // True when isInterp() or isBaseline(). True when isIon() if it
    // has a rematerialized frame. False otherwise false otherwise.
    bool hasUsableAbstractFramePtr() const;

    // -----------------------------------------------------------
    // The following functions can only be called when isInterp(),
    // isBaseline(), isWasm() or isIon(). Further, abstractFramePtr() can
    // only be called when hasUsableAbstractFramePtr().
    // -----------------------------------------------------------

    AbstractFramePtr abstractFramePtr() const;
    AbstractFramePtr copyDataAsAbstractFramePtr() const;
    Data* copyData() const;

    // This can only be called when isInterp():
    inline InterpreterFrame* interpFrame() const;

    // This can only be called when isPhysicalJitFrame():
    inline jit::CommonFrameLayout* physicalJitFrame() const;

    // This is used to provide a raw interface for debugging.
    void* rawFramePtr() const;

  private:
    Data data_;
    jit::InlineFrameIterator ionInlineFrames_;

    const jit::JSJitFrameIter& jsJitFrame() const { return data_.jitFrames_.asJSJit(); }
    const wasm::WasmFrameIter& wasmFrame() const { return data_.jitFrames_.asWasm(); }

    jit::JSJitFrameIter& jsJitFrame() { return data_.jitFrames_.asJSJit(); }
    wasm::WasmFrameIter& wasmFrame() { return data_.jitFrames_.asWasm(); }

    bool isIonScripted() const { return isJSJit() && jsJitFrame().isIonScripted(); }

    void popActivation();
    void popInterpreterFrame();
    void nextJitFrame();
    void popJitFrame();
    void settleOnActivation();
};

class ScriptFrameIter : public FrameIter
{
    void settle() {
        while (!done() && !hasScript())
            FrameIter::operator++();
    }

  public:
    explicit ScriptFrameIter(JSContext* cx,
                             DebuggerEvalOption debuggerEvalOption = FOLLOW_DEBUGGER_EVAL_PREV_LINK)
      : FrameIter(cx, debuggerEvalOption)
    {
        settle();
    }

    ScriptFrameIter(JSContext* cx,
                     const CooperatingContext& target,
                     DebuggerEvalOption debuggerEvalOption)
       : FrameIter(cx, target, debuggerEvalOption)
    {
        settle();
    }

    ScriptFrameIter(JSContext* cx,
                    DebuggerEvalOption debuggerEvalOption,
                    JSPrincipals* prin)
      : FrameIter(cx, debuggerEvalOption, prin)
    {
        settle();
    }

    ScriptFrameIter(const ScriptFrameIter& iter) : FrameIter(iter) { settle(); }
    explicit ScriptFrameIter(const FrameIter::Data& data) : FrameIter(data) { settle(); }
    explicit ScriptFrameIter(AbstractFramePtr frame) : FrameIter(frame) { settle(); }

    ScriptFrameIter& operator++() {
        FrameIter::operator++();
        settle();
        return *this;
    }
};

#ifdef DEBUG
bool SelfHostedFramesVisible();
#else
static inline bool
SelfHostedFramesVisible()
{
    return false;
}
#endif

/* A filtering of the FrameIter to only stop at non-self-hosted scripts. */
class NonBuiltinFrameIter : public FrameIter
{
    void settle();

  public:
    explicit NonBuiltinFrameIter(JSContext* cx,
                                 FrameIter::DebuggerEvalOption debuggerEvalOption =
                                 FrameIter::FOLLOW_DEBUGGER_EVAL_PREV_LINK)
      : FrameIter(cx, debuggerEvalOption)
    {
        settle();
    }

    NonBuiltinFrameIter(JSContext* cx,
                        FrameIter::DebuggerEvalOption debuggerEvalOption,
                        JSPrincipals* principals)
      : FrameIter(cx, debuggerEvalOption, principals)
    {
        settle();
    }

    NonBuiltinFrameIter(JSContext* cx, JSPrincipals* principals)
      : FrameIter(cx, FrameIter::FOLLOW_DEBUGGER_EVAL_PREV_LINK, principals)
    {
        settle();
    }

    explicit NonBuiltinFrameIter(const FrameIter::Data& data)
      : FrameIter(data)
    {}

    NonBuiltinFrameIter& operator++() {
        FrameIter::operator++();
        settle();
        return *this;
    }
};

/* A filtering of the ScriptFrameIter to only stop at non-self-hosted scripts. */
class NonBuiltinScriptFrameIter : public ScriptFrameIter
{
    void settle();

  public:
    explicit NonBuiltinScriptFrameIter(JSContext* cx,
                                       ScriptFrameIter::DebuggerEvalOption debuggerEvalOption =
                                       ScriptFrameIter::FOLLOW_DEBUGGER_EVAL_PREV_LINK)
      : ScriptFrameIter(cx, debuggerEvalOption)
    {
        settle();
    }

    NonBuiltinScriptFrameIter(JSContext* cx,
                              ScriptFrameIter::DebuggerEvalOption debuggerEvalOption,
                              JSPrincipals* principals)
      : ScriptFrameIter(cx, debuggerEvalOption, principals)
    {
        settle();
    }

    explicit NonBuiltinScriptFrameIter(const ScriptFrameIter::Data& data)
      : ScriptFrameIter(data)
    {}

    NonBuiltinScriptFrameIter& operator++() {
        ScriptFrameIter::operator++();
        settle();
        return *this;
    }
};

/*
 * Blindly iterate over all frames in the current thread's stack. These frames
 * can be from different contexts and compartments, so beware.
 */
class AllFramesIter : public FrameIter
{
  public:
    explicit AllFramesIter(JSContext* cx)
      : FrameIter(cx, ScriptFrameIter::IGNORE_DEBUGGER_EVAL_PREV_LINK)
    {}
};

/* Iterates over all script frame in the current thread's stack.
 * See also AllFramesIter and ScriptFrameIter.
 */
class AllScriptFramesIter : public ScriptFrameIter
{
  public:
    explicit AllScriptFramesIter(JSContext* cx)
      : ScriptFrameIter(cx, ScriptFrameIter::IGNORE_DEBUGGER_EVAL_PREV_LINK)
    {}

    explicit AllScriptFramesIter(JSContext* cx, const CooperatingContext& target)
      : ScriptFrameIter(cx, target, ScriptFrameIter::IGNORE_DEBUGGER_EVAL_PREV_LINK)
    {}
};

/* Popular inline definitions. */

inline JSScript*
FrameIter::script() const
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(hasScript());
    if (data_.state_ == INTERP)
        return interpFrame()->script();
    if (jsJitFrame().isIonJS())
        return ionInlineFrames_.script();
    return jsJitFrame().script();
}

inline bool
FrameIter::wasmDebugEnabled() const
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(isWasm());
    return wasmFrame().debugEnabled();
}

inline wasm::Instance*
FrameIter::wasmInstance() const
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(isWasm() && wasmDebugEnabled());
    return wasmFrame().instance();
}

inline unsigned
FrameIter::wasmBytecodeOffset() const
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(isWasm());
    return wasmFrame().lineOrBytecode();
}

inline bool
FrameIter::isIon() const
{
    return isJSJit() && jsJitFrame().isIonJS();
}

inline bool
FrameIter::isBaseline() const
{
    return isJSJit() && jsJitFrame().isBaselineJS();
}

inline InterpreterFrame*
FrameIter::interpFrame() const
{
    MOZ_ASSERT(data_.state_ == INTERP);
    return data_.interpFrames_.frame();
}

inline bool
FrameIter::isPhysicalJitFrame() const
{
    if (!isJSJit())
        return false;

    auto& jitFrame = jsJitFrame();

    if (jitFrame.isBaselineJS())
        return true;

    if (jitFrame.isIonScripted()) {
        // Only the bottom of a group of inlined Ion frames is a physical frame.
        return ionInlineFrames_.frameNo() == 0;
    }

    return false;
}

inline jit::CommonFrameLayout*
FrameIter::physicalJitFrame() const
{
    MOZ_ASSERT(isPhysicalJitFrame());
    return jsJitFrame().current();
}

}  /* namespace js */
#endif /* vm_Stack_h */
