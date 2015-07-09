/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Stack_h
#define vm_Stack_h

#include "mozilla/Atomics.h"
#include "mozilla/MemoryReporting.h"

#include "jsfun.h"
#include "jsscript.h"

#include "asmjs/AsmJSFrameIterator.h"
#include "jit/JitFrameIterator.h"
#ifdef CHECK_OSIPOINT_REGISTERS
#include "jit/Registers.h" // for RegisterDump
#endif

struct JSCompartment;

namespace js {

class ArgumentsObject;
class AsmJSModule;
class InterpreterRegs;
class CallObject;
class ScopeObject;
class ScriptFrameIter;
class SPSProfiler;
class InterpreterFrame;
class StaticBlockObject;

class ScopeCoordinate;

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
enum MaybeCheckLexical { CheckLexical = true, DontCheckLexical = false };

/*****************************************************************************/

namespace jit {
    class BaselineFrame;
    class RematerializedFrame;
}

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
        TagMask = 0x3
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

    void* raw() const { return reinterpret_cast<void*>(ptr_); }

    bool operator ==(const AbstractFramePtr& other) const { return ptr_ == other.ptr_; }
    bool operator !=(const AbstractFramePtr& other) const { return ptr_ != other.ptr_; }

    operator bool() const { return !!ptr_; }

    inline JSObject* scopeChain() const;
    inline CallObject& callObj() const;
    inline bool initFunctionScopeObjects(JSContext* cx);
    inline void pushOnScopeChain(ScopeObject& scope);

    inline JSCompartment* compartment() const;

    inline bool hasCallObj() const;
    inline bool isFunctionFrame() const;
    inline bool isGlobalFrame() const;
    inline bool isEvalFrame() const;
    inline bool isDebuggerEvalFrame() const;

    inline JSScript* script() const;
    inline JSFunction* fun() const;
    inline JSFunction* maybeFun() const;
    inline JSFunction* callee() const;
    inline Value calleev() const;
    inline Value& thisValue() const;

    inline bool isNonEvalFunctionFrame() const;
    inline bool isNonStrictDirectEvalFrame() const;
    inline bool isStrictEvalFrame() const;

    inline unsigned numActualArgs() const;
    inline unsigned numFormalArgs() const;

    inline Value* argv() const;

    inline bool hasArgs() const;
    inline bool hasArgsObj() const;
    inline ArgumentsObject& argsObj() const;
    inline void initArgsObj(ArgumentsObject& argsobj) const;
    inline bool createSingleton() const;

    inline bool copyRawFrameSlots(AutoValueVector* vec) const;

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

    JSObject* evalPrevScopeChain(JSContext* cx) const;

    inline HandleValue returnValue() const;
    inline void setReturnValue(const Value& rval) const;

    bool hasPushedSPSFrame() const;

    inline void popBlock(JSContext* cx) const;
    inline void popWith(JSContext* cx) const;
};

class NullFramePtr : public AbstractFramePtr
{
  public:
    NullFramePtr()
      : AbstractFramePtr()
    { }
};

/*****************************************************************************/

/* Flags specified for a frame as it is constructed. */
enum InitialFrameFlags {
    INITIAL_NONE           =          0,
    INITIAL_CONSTRUCT      =       0x10, /* == InterpreterFrame::CONSTRUCTING, asserted below */
};

enum ExecuteType {
    EXECUTE_GLOBAL         =        0x1, /* == InterpreterFrame::GLOBAL */
    EXECUTE_DIRECT_EVAL    =        0x4, /* == InterpreterFrame::EVAL */
    EXECUTE_INDIRECT_EVAL  =        0x5, /* == InterpreterFrame::GLOBAL | EVAL */
    EXECUTE_DEBUG          =        0xc, /* == InterpreterFrame::EVAL | DEBUGGER */
    EXECUTE_DEBUG_GLOBAL   =        0xd  /* == InterpreterFrame::EVAL | DEBUGGER | GLOBAL */
};

/*****************************************************************************/

class InterpreterFrame
{
  public:
    enum Flags {
        /* Primary frame type */
        GLOBAL             =        0x1,  /* frame pushed for a global script */
        FUNCTION           =        0x2,  /* frame pushed for a scripted call */

        /* Frame subtypes */
        EVAL               =        0x4,  /* frame pushed for eval() or debugger eval */


        /*
         * Frame pushed for debugger eval.
         * - Don't bother to JIT it, because it's probably short-lived.
         * - It is required to have a scope chain object outside the
         *   js::ScopeObject hierarchy: either a global object, or a
         *   DebugScopeObject (not a ScopeObject, despite the name)
         * - If evalInFramePrev_ is set, then this frame was created for an
         *   "eval in frame" call, which can push a successor to any live
         *   frame; so its logical "prev" frame is not necessarily the
         *   previous frame in memory. Iteration should treat
         *   evalInFramePrev_ as this frame's previous frame.
         */
        DEBUGGER_EVAL      =        0x8,

        CONSTRUCTING       =       0x10,  /* frame is for a constructor invocation */

        /* (0x20, 0x40 and 0x80 are unused) */

        /* Function prologue state */
        HAS_CALL_OBJ       =      0x100,  /* CallObject created for heavyweight fun */
        HAS_ARGS_OBJ       =      0x200,  /* ArgumentsObject created for needsArgsObj script */

        /* Lazy frame initialization */
        HAS_RVAL           =      0x800,  /* frame has rval_ set */
        HAS_SCOPECHAIN     =     0x1000,  /* frame has scopeChain_ set */

        /* Debugger state */
        PREV_UP_TO_DATE    =     0x4000,  /* see DebugScopes::updateLiveScopes */

        /*
         * See comment above 'debugMode' in jscompartment.h for explanation of
         * invariants of debuggee compartments, scripts, and frames.
         */
        DEBUGGEE           =     0x8000,  /* Execution is being observed by Debugger */

        /* Used in tracking calls and profiling (see vm/SPSProfiler.cpp) */
        HAS_PUSHED_SPS_FRAME =   0x10000, /* SPS was notified of enty */

        /*
         * If set, we entered one of the JITs and ScriptFrameIter should skip
         * this frame.
         */
        RUNNING_IN_JIT     =    0x20000,

        /* Miscellaneous state. */
        CREATE_SINGLETON   =    0x40000   /* Constructed |this| object should be singleton. */
    };

  private:
    mutable uint32_t    flags_;         /* bits described by Flags */
    union {                             /* describes what code is executing in a */
        JSScript*       script;        /*   global frame */
        JSFunction*     fun;           /*   function frame, pre GetScopeChain */
    } exec;
    union {                             /* describes the arguments of a function */
        unsigned        nactual;        /*   for non-eval frames */
        JSScript*       evalScript;    /*   the script of an eval-in-function */
    } u;
    mutable JSObject*   scopeChain_;   /* if HAS_SCOPECHAIN, current scope chain */
    Value               rval_;          /* if HAS_RVAL, return value of the frame */
    ArgumentsObject*    argsObj_;      /* if HAS_ARGS_OBJ, the call's arguments object */

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

    void writeBarrierPost();

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
    void initCallFrame(JSContext* cx, InterpreterFrame* prev, jsbytecode* prevpc, Value* prevsp,
                       JSFunction& callee, JSScript* script, Value* argv, uint32_t nactual,
                       InterpreterFrame::Flags flags);

    /* Used for global and eval frames. */
    void initExecuteFrame(JSContext* cx, HandleScript script, AbstractFramePtr prev,
                          const Value& thisv, HandleObject scopeChain, ExecuteType type);

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
    void epilogue(JSContext* cx);

    bool initFunctionScopeObjects(JSContext* cx);

    /*
     * Initialize local variables of newly-pushed frame. 'var' bindings are
     * initialized to undefined and lexical bindings are initialized to
     * JS_UNINITIALIZED_LEXICAL.
     */
    void initLocals();

    /*
     * Stack frame type
     *
     * A stack frame may have one of three types, which determines which
     * members of the frame may be accessed and other invariants:
     *
     *  global frame:   execution of global code or an eval in global code
     *  function frame: execution of function code or an eval in a function
     */

    bool isFunctionFrame() const {
        return !!(flags_ & FUNCTION);
    }

    bool isGlobalFrame() const {
        return !!(flags_ & GLOBAL);
    }

    /*
     * Eval frames
     *
     * As noted above, global and function frames may optionally be 'eval
     * frames'. Eval code shares its parent's arguments which means that the
     * arg-access members of InterpreterFrame may not be used for eval frames.
     * Search for 'hasArgs' below for more details.
     *
     * A further sub-classification of eval frames is whether the frame was
     * pushed for an ES5 strict-mode eval().
     */

    bool isEvalFrame() const {
        return flags_ & EVAL;
    }

    bool isEvalInFunction() const {
        return (flags_ & (EVAL | FUNCTION)) == (EVAL | FUNCTION);
    }

    bool isNonEvalFunctionFrame() const {
        return (flags_ & (FUNCTION | EVAL)) == FUNCTION;
    }

    inline bool isStrictEvalFrame() const {
        return isEvalFrame() && script()->strict();
    }

    bool isNonStrictEvalFrame() const {
        return isEvalFrame() && !script()->strict();
    }

    bool isDirectEvalFrame() const {
        return isEvalFrame() && script()->staticLevel() > 0;
    }

    bool isNonStrictDirectEvalFrame() const {
        return isNonStrictEvalFrame() && isDirectEvalFrame();
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
     * When a local/formal variable is "aliased" (accessed by nested closures,
     * dynamic scope operations, or 'arguments), the canonical location for
     * that value is the slot of an activation object (scope or arguments).
     * Currently, aliased locals don't have stack slots assigned to them, but
     * all formals are given slots in *both* the stack frame and heap objects,
     * even though, as just described, only one should ever be accessed. Thus,
     * it is up to the code performing an access to access the correct value.
     * These functions assert that accesses to stack values are unaliased.
     */

    inline Value& unaliasedLocal(uint32_t i);

    bool hasArgs() const { return isNonEvalFunctionFrame(); }
    inline Value& unaliasedFormal(unsigned i, MaybeCheckAliasing = CHECK_ALIASING);
    inline Value& unaliasedActual(unsigned i, MaybeCheckAliasing = CHECK_ALIASING);
    template <class Op> inline void unaliasedForEachActual(Op op);

    bool copyRawFrameSlots(AutoValueVector* v);

    unsigned numFormalArgs() const { MOZ_ASSERT(hasArgs()); return fun()->nargs(); }
    unsigned numActualArgs() const { MOZ_ASSERT(hasArgs()); return u.nactual; }

    /* Watch out, this exposes a pointer to the unaliased formal arg array. */
    Value* argv() const { return argv_; }

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

    JSObject* createRestParameter(JSContext* cx);

    /*
     * Scope chain
     *
     * In theory, the scope chain would contain an object for every lexical
     * scope. However, only objects that are required for dynamic lookup are
     * actually created.
     *
     * Given that an InterpreterFrame corresponds roughly to a ES5 Execution Context
     * (ES5 10.3), InterpreterFrame::varObj corresponds to the VariableEnvironment
     * component of a Exection Context. Intuitively, the variables object is
     * where new bindings (variables and functions) are stored. One might
     * expect that this is either the Call object or scopeChain.globalObj for
     * function or global code, respectively, however the JSAPI allows calls of
     * Execute to specify a variables object on the scope chain other than the
     * call/global object. This allows embeddings to run multiple scripts under
     * the same global, each time using a new variables object to collect and
     * discard the script's global variables.
     */

    inline HandleObject scopeChain() const;

    inline ScopeObject& aliasedVarScope(ScopeCoordinate sc) const;
    inline GlobalObject& global() const;
    inline CallObject& callObj() const;
    inline JSObject& varObj();

    inline void pushOnScopeChain(ScopeObject& scope);
    inline void popOffScopeChain();

    /*
     * For blocks with aliased locals, these interfaces push and pop entries on
     * the scope chain.
     */

    bool pushBlock(JSContext* cx, StaticBlockObject& block);
    void popBlock(JSContext* cx);

    /*
     * With
     *
     * Entering/leaving a |with| block pushes/pops an object on the scope chain.
     * Pushing uses pushOnScopeChain, popping should use popWith.
     */

    void popWith(JSContext* cx);

    /*
     * Script
     *
     * All function and global frames have an associated JSScript which holds
     * the bytecode being executed for the frame. This script/bytecode does
     * not reflect any inlining that has been performed by the method JIT.
     * If other frames were inlined into this one, the script/pc reflect the
     * point of the outermost call. Inlined frame invariants:
     *
     * - Inlined frames have the same scope chain as the outer frame.
     * - Inlined frames have the same strictness as the outer frame.
     * - Inlined frames can only make calls to other JIT frames associated with
     *   the same VMFrame. Other calls force expansion of the inlined frames.
     */

    JSScript* script() const {
        return isFunctionFrame()
               ? isEvalFrame()
                 ? u.evalScript
                 : fun()->nonLazyScript()
               : exec.script;
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
     * Function
     *
     * All function frames have an associated interpreted JSFunction. The
     * function returned by fun() and maybeFun() is not necessarily the
     * original canonical function which the frame's script was compiled
     * against.
     */

    JSFunction* fun() const {
        MOZ_ASSERT(isFunctionFrame());
        return exec.fun;
    }

    JSFunction* maybeFun() const {
        return isFunctionFrame() ? fun() : nullptr;
    }

    /*
     * This value
     *
     * Every frame has a this value although, until 'this' is computed, the
     * value may not be the semantically-correct 'this' value.
     *
     * The 'this' value is stored before the formal arguments for function
     * frames and directly before the frame for global frames. The *Args
     * members assert !isEvalFrame(), so we implement specialized inline
     * methods for accessing 'this'. When the caller has static knowledge that
     * a frame is a function, 'functionThis' allows more efficient access.
     */

    Value& functionThis() const {
        MOZ_ASSERT(isFunctionFrame());
        if (isEvalFrame())
            return ((Value*)this)[-1];
        return argv()[-1];
    }

    JSObject& constructorThis() const {
        MOZ_ASSERT(hasArgs());
        return argv()[-1].toObject();
    }

    Value& thisValue() const {
        if (flags_ & (EVAL | GLOBAL))
            return ((Value*)this)[-1];
        return argv()[-1];
    }

    /*
     * Callee
     *
     * Only function frames have a callee. An eval frame in a function has the
     * same callee as its containing function frame. maybeCalleev can be used
     * to return a value that is either the callee object (for function frames) or
     * null (for global frames).
     */

    JSFunction& callee() const {
        MOZ_ASSERT(isFunctionFrame());
        return calleev().toObject().as<JSFunction>();
    }

    const Value& calleev() const {
        MOZ_ASSERT(isFunctionFrame());
        return mutableCalleev();
    }

    const Value& maybeCalleev() const {
        Value& calleev = flags_ & (EVAL | GLOBAL)
                         ? ((Value*)this)[-2]
                         : argv()[-2];
        MOZ_ASSERT(calleev.isObjectOrNull());
        return calleev;
    }

    Value& mutableCalleev() const {
        MOZ_ASSERT(isFunctionFrame());
        if (isEvalFrame())
            return ((Value*)this)[-2];
        return argv()[-2];
    }

    CallReceiver callReceiver() const {
        return CallReceiverFromArgv(argv());
    }

    /*
     * Frame compartment
     *
     * A stack frame's compartment is the frame's containing context's
     * compartment when the frame was pushed.
     */

    inline JSCompartment* compartment() const;

    /* Profiler flags */

    bool hasPushedSPSFrame() {
        return !!(flags_ & HAS_PUSHED_SPS_FRAME);
    }

    void setPushedSPSFrame() {
        flags_ |= HAS_PUSHED_SPS_FRAME;
    }

    void unsetPushedSPSFrame() {
        flags_ &= ~HAS_PUSHED_SPS_FRAME;
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

    void resumeGeneratorFrame(JSObject* scopeChain) {
        MOZ_ASSERT(script()->isGenerator());
        MOZ_ASSERT(isNonEvalFunctionFrame());
        flags_ |= HAS_CALL_OBJ | HAS_SCOPECHAIN;
        scopeChain_ = scopeChain;
    }

    /*
     * Other flags
     */

    InitialFrameFlags initialFlags() const {
        JS_STATIC_ASSERT((int)INITIAL_NONE == 0);
        JS_STATIC_ASSERT((int)INITIAL_CONSTRUCT == (int)CONSTRUCTING);
        uint32_t mask = CONSTRUCTING;
        MOZ_ASSERT((flags_ & mask) != mask);
        return InitialFrameFlags(flags_ & mask);
    }

    void setConstructing() {
        flags_ |= CONSTRUCTING;
    }

    bool isConstructing() const {
        return !!(flags_ & CONSTRUCTING);
    }

    /*
     * These two queries should not be used in general: the presence/absence of
     * the call/args object is determined by the static(ish) properties of the
     * JSFunction/JSScript. These queries should only be performed when probing
     * a stack frame that may be in the middle of the prologue (during which
     * time the call/args object are created).
     */

    inline bool hasCallObj() const;

    bool hasCallObjUnchecked() const {
        return flags_ & HAS_CALL_OBJ;
    }

    bool hasArgsObj() const {
        MOZ_ASSERT(script()->needsArgsObj());
        return flags_ & HAS_ARGS_OBJ;
    }

    void setCreateSingleton() {
        MOZ_ASSERT(isConstructing());
        flags_ |= CREATE_SINGLETON;
    }
    bool createSingleton() const {
        MOZ_ASSERT(isConstructing());
        return flags_ & CREATE_SINGLETON;
    }

    bool isDebuggerEvalFrame() const {
        return !!(flags_ & DEBUGGER_EVAL);
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

  public:
    void mark(JSTracer* trc);
    void markValues(JSTracer* trc, unsigned start, unsigned end);
    void markValues(JSTracer* trc, Value* sp, jsbytecode* pc);

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

static const size_t VALUES_PER_STACK_FRAME = sizeof(InterpreterFrame) / sizeof(Value);

static inline InterpreterFrame::Flags
ToFrameFlags(InitialFrameFlags initial)
{
    return InterpreterFrame::Flags(initial);
}

static inline InitialFrameFlags
InitialFrameFlagsFromConstructing(bool b)
{
    return b ? INITIAL_CONSTRUCT : INITIAL_NONE;
}

static inline bool
InitialFrameFlagsAreConstructing(InitialFrameFlags initial)
{
    return !!(initial & INITIAL_CONSTRUCT);
}

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
        sp = fp_->prevsp() - fp_->numActualArgs() - 1;
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
                 InterpreterFrame::Flags* pflags, Value** pargv);

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
    InterpreterFrame* pushExecuteFrame(JSContext* cx, HandleScript script, const Value& thisv,
                                 HandleObject scopeChain, ExecuteType type,
                                 AbstractFramePtr evalInFrame);

    // Called to invoke a function.
    InterpreterFrame* pushInvokeFrame(JSContext* cx, const CallArgs& args,
                                      InitialFrameFlags initial);

    // The interpreter can push light-weight, "inline" frames without entering a
    // new InterpreterActivation or recursively calling Interpret.
    bool pushInlineFrame(JSContext* cx, InterpreterRegs& regs, const CallArgs& args,
                         HandleScript script, InitialFrameFlags initial);

    void popInlineFrame(InterpreterRegs& regs);

    bool resumeGeneratorCallFrame(JSContext* cx, InterpreterRegs& regs,
                                  HandleFunction callee, HandleValue thisv,
                                  HandleObject scopeChain);

    inline void purge(JSRuntime* rt);

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return allocator_.sizeOfExcludingThis(mallocSizeOf);
    }
};

void MarkInterpreterActivations(JSRuntime* rt, JSTracer* trc);

/*****************************************************************************/

class InvokeArgs : public JS::CallArgs
{
    AutoValueVector v_;

  public:
    explicit InvokeArgs(JSContext* cx) : v_(cx) {}

    bool init(unsigned argc) {
        if (!v_.resize(2 + argc))
            return false;
        ImplicitCast<CallArgs>(*this) = CallArgsFromVp(argc, v_.begin());
        return true;
    }
};

template <>
struct DefaultHasher<AbstractFramePtr> {
    typedef AbstractFramePtr Lookup;

    static js::HashNumber hash(const Lookup& key) {
        return size_t(key.raw());
    }

    static bool match(const AbstractFramePtr& k, const Lookup& l) {
        return k == l;
    }
};

/*****************************************************************************/

class InterpreterActivation;
class AsmJSActivation;

namespace jit {
    class JitActivation;
};

class Activation
{
  protected:
    JSContext* cx_;
    JSCompartment* compartment_;
    Activation* prev_;
    Activation* prevProfiling_;

    // Counter incremented by JS_SaveFrameChain on the top-most activation and
    // decremented by JS_RestoreFrameChain. If > 0, ScriptFrameIter should stop
    // iterating when it reaches this activation (if GO_THROUGH_SAVED is not
    // set).
    size_t savedFrameChain_;

    // Counter incremented by JS::HideScriptedCaller and decremented by
    // JS::UnhideScriptedCaller. If > 0 for the top activation,
    // DescribeScriptedCaller will return null instead of querying that
    // activation, which should prompt the caller to consult embedding-specific
    // data structures instead.
    size_t hideScriptedCallerCount_;

    enum Kind { Interpreter, Jit, AsmJS };
    Kind kind_;

    inline Activation(JSContext* cx, Kind kind_);
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
    bool isAsmJS() const {
        return kind_ == AsmJS;
    }

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
    AsmJSActivation* asAsmJS() const {
        MOZ_ASSERT(isAsmJS());
        return (AsmJSActivation*)this;
    }

    void saveFrameChain() {
        savedFrameChain_++;
    }
    void restoreFrameChain() {
        MOZ_ASSERT(savedFrameChain_ > 0);
        savedFrameChain_--;
    }
    bool hasSavedFrameChain() const {
        return savedFrameChain_ > 0;
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

    static size_t offsetOfPrevProfiling() {
        return offsetof(Activation, prevProfiling_);
    }

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
                                InitialFrameFlags initial);
    inline void popInlineFrame(InterpreterFrame* frame);

    inline bool resumeGeneratorFrame(HandleFunction callee, HandleValue thisv,
                                     HandleObject scopeChain);

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

// Iterates over a thread's activation list. If given a runtime, iterate over
// the runtime's main thread's activation list.
class ActivationIterator
{
    uint8_t* jitTop_;

  protected:
    Activation* activation_;

  private:
    void settle();

  public:
    explicit ActivationIterator(JSRuntime* rt);

    ActivationIterator& operator++();

    Activation* operator->() const {
        return activation_;
    }
    Activation* activation() const {
        return activation_;
    }
    uint8_t* jitTop() const {
        MOZ_ASSERT(activation_->isJit());
        return jitTop_;
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
    uint8_t* prevJitTop_;
    JitActivation* prevJitActivation_;
    JSContext* prevJitJSContext_;
    bool active_;

    // Rematerialized Ion frames which has info copied out of snapshots. Maps
    // frame pointers (i.e. jitTop) to a vector of rematerializations of all
    // inline frames associated with that frame.
    //
    // This table is lazily initialized by calling getRematerializedFrame.
    typedef Vector<RematerializedFrame*> RematerializedFrameVector;
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
    // frames. This field is used for all newly constructed JitFrameIterators to
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
    explicit JitActivation(JSContext* cx, bool active = true);
    ~JitActivation();

    bool isActive() const {
        return active_;
    }
    void setActive(JSContext* cx, bool active = true);

    bool isProfiling() const;

    uint8_t* prevJitTop() const {
        return prevJitTop_;
    }
    static size_t offsetOfPrevJitTop() {
        return offsetof(JitActivation, prevJitTop_);
    }
    static size_t offsetOfPrevJitJSContext() {
        return offsetof(JitActivation, prevJitJSContext_);
    }
    static size_t offsetOfPrevJitActivation() {
        return offsetof(JitActivation, prevJitActivation_);
    }
    static size_t offsetOfActiveUint8() {
        MOZ_ASSERT(sizeof(bool) == 1);
        return offsetof(JitActivation, active_);
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
    RematerializedFrame* getRematerializedFrame(JSContext* cx, const JitFrameIterator& iter,
                                                size_t inlineDepth = 0);

    // Look up a rematerialized frame by the fp. If inlineDepth is out of
    // bounds of what has been rematerialized, nullptr is returned.
    RematerializedFrame* lookupRematerializedFrame(uint8_t* top, size_t inlineDepth = 0);

    bool hasRematerializedFrame(uint8_t* top, size_t inlineDepth = 0) {
        return !!lookupRematerializedFrame(top, inlineDepth);
    }

    // Remove a previous rematerialization by fp.
    void removeRematerializedFrame(uint8_t* top);

    void markRematerializedFrames(JSTracer* trc);


    // Register the results of on Ion frame recovery.
    bool registerIonFrameRecovery(RInstructionResults&& results);

    // Return the pointer to the Ion frame recovery, if it is already registered.
    RInstructionResults* maybeIonFrameRecovery(JitFrameLayout* fp);

    // If an Ion frame recovery exists for the |fp| frame exists, then remove it
    // from the activation.
    void removeIonFrameRecovery(JitFrameLayout* fp);

    void markIonRecovery(JSTracer* trc);

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
};

// A filtering of the ActivationIterator to only stop at JitActivations.
class JitActivationIterator : public ActivationIterator
{
    void settle() {
        while (!done() && !activation_->isJit())
            ActivationIterator::operator++();
    }

  public:
    explicit JitActivationIterator(JSRuntime* rt)
      : ActivationIterator(rt)
    {
        settle();
    }

    JitActivationIterator& operator++() {
        ActivationIterator::operator++();
        settle();
        return *this;
    }

    // Returns the bottom and top addresses of the current JitActivation.
    void jitStackRange(uintptr_t*& min, uintptr_t*& end);
};

} // namespace jit

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

// An AsmJSActivation is part of two activation linked lists:
//  - the normal Activation list used by FrameIter
//  - a list of only AsmJSActivations that is signal-safe since it is accessed
//    from the profiler at arbitrary points
//
// An eventual goal is to remove AsmJSActivation and to run asm.js code in a
// JitActivation interleaved with Ion/Baseline jit code. This would allow
// efficient calls back and forth but requires that we can walk the stack for
// all kinds of jit code.
class AsmJSActivation : public Activation
{
    AsmJSModule& module_;
    AsmJSActivation* prevAsmJS_;
    AsmJSActivation* prevAsmJSForModule_;
    void* entrySP_;
    SPSProfiler* profiler_;
    void* resumePC_;
    uint8_t* fp_;
    AsmJSExit::Reason exitReason_;

  public:
    AsmJSActivation(JSContext* cx, AsmJSModule& module);
    ~AsmJSActivation();

    inline JSContext* cx();
    AsmJSModule& module() const { return module_; }
    AsmJSActivation* prevAsmJS() const { return prevAsmJS_; }

    bool isProfiling() const {
        return true;
    }

    // Returns a pointer to the base of the innermost stack frame of asm.js code
    // in this activation.
    uint8_t* fp() const { return fp_; }

    // Returns the reason why asm.js code called out of asm.js code.
    AsmJSExit::Reason exitReason() const { return exitReason_; }

    // Read by JIT code:
    static unsigned offsetOfContext() { return offsetof(AsmJSActivation, cx_); }
    static unsigned offsetOfResumePC() { return offsetof(AsmJSActivation, resumePC_); }

    // Written by JIT code:
    static unsigned offsetOfEntrySP() { return offsetof(AsmJSActivation, entrySP_); }
    static unsigned offsetOfFP() { return offsetof(AsmJSActivation, fp_); }
    static unsigned offsetOfExitReason() { return offsetof(AsmJSActivation, exitReason_); }

    // Read/written from SIGSEGV handler:
    void setResumePC(void* pc) { resumePC_ = pc; }
    void* resumePC() const { return resumePC_; }
};

// A FrameIter walks over the runtime's stack of JS script activations,
// abstracting over whether the JS scripts were running in the interpreter or
// different modes of compiled code.
//
// FrameIter is parameterized by what it includes in the stack iteration:
//  - The SavedOption controls whether FrameIter stops when it finds an
//    activation that was set aside via JS_SaveFrameChain (and not yet retored
//    by JS_RestoreFrameChain). (Hopefully this will go away.)
//  - The ContextOption determines whether the iteration will view frames from
//    all JSContexts or just the given JSContext. (Hopefully this will go away.)
//  - When provided, the optional JSPrincipal argument will cause FrameIter to
//    only show frames in globals whose JSPrincipals are subsumed (via
//    JSSecurityCallbacks::subsume) by the given JSPrincipal.
//
// Additionally, there are derived FrameIter types that automatically skip
// certain frames:
//  - ScriptFrameIter only shows frames that have an associated JSScript
//    (currently everything other than asm.js stack frames). When !hasScript(),
//    clients must stick to the portion of the
//    interface marked below.
//  - NonBuiltinScriptFrameIter additionally filters out builtin (self-hosted)
//    scripts.
class FrameIter
{
  public:
    enum SavedOption { STOP_AT_SAVED, GO_THROUGH_SAVED };
    enum ContextOption { CURRENT_CONTEXT, ALL_CONTEXTS };
    enum State { DONE, INTERP, JIT, ASMJS };

    // Unlike ScriptFrameIter itself, ScriptFrameIter::Data can be allocated on
    // the heap, so this structure should not contain any GC things.
    struct Data
    {
        JSContext * cx_;
        SavedOption         savedOption_;
        ContextOption       contextOption_;
        JSPrincipals *      principals_;

        State               state_;

        jsbytecode *        pc_;

        InterpreterFrameIterator interpFrames_;
        ActivationIterator activations_;

        jit::JitFrameIterator jitFrames_;
        unsigned ionInlineFrameNo_;
        AsmJSFrameIterator asmJSFrames_;

        Data(JSContext* cx, SavedOption savedOption, ContextOption contextOption,
             JSPrincipals* principals);
        Data(const Data& other);
    };

    MOZ_IMPLICIT FrameIter(JSContext* cx, SavedOption = STOP_AT_SAVED);
    FrameIter(JSContext* cx, ContextOption, SavedOption);
    FrameIter(JSContext* cx, ContextOption, SavedOption, JSPrincipals*);
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

    bool isInterp() const { MOZ_ASSERT(!done()); return data_.state_ == INTERP;  }
    bool isJit() const { MOZ_ASSERT(!done()); return data_.state_ == JIT; }
    bool isAsmJS() const { MOZ_ASSERT(!done()); return data_.state_ == ASMJS; }
    inline bool isIon() const;
    inline bool isBaseline() const;

    bool isFunctionFrame() const;
    bool isGlobalFrame() const;
    bool isEvalFrame() const;
    bool isNonEvalFunctionFrame() const;
    bool hasArgs() const { return isNonEvalFunctionFrame(); }

    ScriptSource* scriptSource() const;
    const char* scriptFilename() const;
    const char16_t* scriptDisplayURL() const;
    unsigned computeLine(uint32_t* column = nullptr) const;
    JSAtom* functionDisplayAtom() const;
    bool mutedErrors() const;

    bool hasScript() const { return !isAsmJS(); }

    // -----------------------------------------------------------
    // The following functions can only be called when hasScript()
    // -----------------------------------------------------------

    inline JSScript* script() const;

    bool        isConstructing() const;
    jsbytecode* pc() const { MOZ_ASSERT(!done()); return data_.pc_; }
    void        updatePcQuadratic();

    // The function |calleeTemplate()| returns either the function from which
    // the current |callee| was cloned or the |callee| if it can be read. As
    // long as we do not have to investigate the scope chain or build a new
    // frame, we should prefer to use |calleeTemplate| instead of |callee|, as
    // requesting the |callee| might cause the invalidation of the frame. (see
    // js::Lambda)
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

    JSObject*  scopeChain(JSContext* cx) const;
    CallObject& callObj(JSContext* cx) const;

    bool        hasArgsObj() const;
    ArgumentsObject& argsObj() const;

    // Ensure that computedThisValue is correct, see ComputeThis.
    bool        computeThis(JSContext* cx) const;
    // thisv() may not always be correct, even after computeThis. In case when
    // the frame is an Ion frame, the computed this value cannot be saved to
    // the Ion frame but is instead saved in the RematerializedFrame for use
    // by Debugger.
    //
    // Both methods exist because of speed. thisv() will never rematerialize
    // an Ion frame, whereas computedThisValue() will.
    Value       computedThisValue() const;
    Value       thisv(JSContext* cx) const;

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
    // isBaseline(), or isIon(). Further, abstractFramePtr() can
    // only be called when hasUsableAbstractFramePtr().
    // -----------------------------------------------------------

    AbstractFramePtr abstractFramePtr() const;
    AbstractFramePtr copyDataAsAbstractFramePtr() const;
    Data* copyData() const;

    // This can only be called when isInterp():
    inline InterpreterFrame* interpFrame() const;

  private:
    Data data_;
    jit::InlineFrameIterator ionInlineFrames_;

    void popActivation();
    void popInterpreterFrame();
    void nextJitFrame();
    void popJitFrame();
    void popAsmJSFrame();
    void settleOnActivation();
};

class ScriptFrameIter : public FrameIter
{
    void settle() {
        while (!done() && !hasScript())
            FrameIter::operator++();
    }

  public:
    explicit ScriptFrameIter(JSContext* cx, SavedOption savedOption = STOP_AT_SAVED)
      : FrameIter(cx, savedOption)
    {
        settle();
    }

    ScriptFrameIter(JSContext* cx,
                    ContextOption cxOption,
                    SavedOption savedOption)
      : FrameIter(cx, cxOption, savedOption)
    {
        settle();
    }

    ScriptFrameIter(JSContext* cx,
                    ContextOption cxOption,
                    SavedOption savedOption,
                    JSPrincipals* prin)
      : FrameIter(cx, cxOption, savedOption, prin)
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
                                 FrameIter::SavedOption opt = FrameIter::STOP_AT_SAVED)
      : FrameIter(cx, opt)
    {
        settle();
    }

    NonBuiltinFrameIter(JSContext* cx,
                        FrameIter::ContextOption contextOption,
                        FrameIter::SavedOption savedOption)
      : FrameIter(cx, contextOption, savedOption)
    {
        settle();
    }

    NonBuiltinFrameIter(JSContext* cx,
                        FrameIter::ContextOption contextOption,
                        FrameIter::SavedOption savedOption,
                        JSPrincipals* principals)
      : FrameIter(cx, contextOption, savedOption, principals)
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
                                       ScriptFrameIter::SavedOption opt =
                                       ScriptFrameIter::STOP_AT_SAVED)
      : ScriptFrameIter(cx, opt)
    {
        settle();
    }

    NonBuiltinScriptFrameIter(JSContext* cx,
                              ScriptFrameIter::ContextOption contextOption,
                              ScriptFrameIter::SavedOption savedOption)
      : ScriptFrameIter(cx, contextOption, savedOption)
    {
        settle();
    }

    NonBuiltinScriptFrameIter(JSContext* cx,
                              ScriptFrameIter::ContextOption contextOption,
                              ScriptFrameIter::SavedOption savedOption,
                              JSPrincipals* principals)
      : ScriptFrameIter(cx, contextOption, savedOption, principals)
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
class AllFramesIter : public ScriptFrameIter
{
  public:
    explicit AllFramesIter(JSContext* cx)
      : ScriptFrameIter(cx, ScriptFrameIter::ALL_CONTEXTS, ScriptFrameIter::GO_THROUGH_SAVED)
    {}
};

/* Popular inline definitions. */

inline JSScript*
FrameIter::script() const
{
    MOZ_ASSERT(!done());
    if (data_.state_ == INTERP)
        return interpFrame()->script();
    MOZ_ASSERT(data_.state_ == JIT);
    if (data_.jitFrames_.isIonJS())
        return ionInlineFrames_.script();
    return data_.jitFrames_.script();
}

inline bool
FrameIter::isIon() const
{
    return isJit() && data_.jitFrames_.isIonJS();
}

inline bool
FrameIter::isBaseline() const
{
    return isJit() && data_.jitFrames_.isBaselineJS();
}

inline InterpreterFrame*
FrameIter::interpFrame() const
{
    MOZ_ASSERT(data_.state_ == INTERP);
    return data_.interpFrames_.frame();
}

}  /* namespace js */
#endif /* vm_Stack_h */
