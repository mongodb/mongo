/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Stack_inl_h
#define vm_Stack_inl_h

#include "vm/Stack.h"

#include "mozilla/PodOperations.h"

#include "jit/BaselineFrame.h"
#include "jit/RematerializedFrame.h"
#include "js/Debug.h"
#include "vm/EnvironmentObject.h"
#include "vm/GeneratorObject.h"
#include "vm/JSContext.h"
#include "vm/JSScript.h"
#include "wasm/WasmInstance.h"

#include "jit/BaselineFrame-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"

namespace js {

inline HandleObject
InterpreterFrame::environmentChain() const
{
    return HandleObject::fromMarkedLocation(&envChain_);
}

inline GlobalObject&
InterpreterFrame::global() const
{
    return environmentChain()->global();
}

inline JSObject&
InterpreterFrame::varObj() const
{
    JSObject* obj = environmentChain();
    while (!obj->isQualifiedVarObj())
        obj = obj->enclosingEnvironment();
    return *obj;
}

inline LexicalEnvironmentObject&
InterpreterFrame::extensibleLexicalEnvironment() const
{
    return NearestEnclosingExtensibleLexicalEnvironment(environmentChain());
}

inline void
InterpreterFrame::initCallFrame(InterpreterFrame* prev, jsbytecode* prevpc,
                                Value* prevsp, JSFunction& callee, JSScript* script, Value* argv,
                                uint32_t nactual, MaybeConstruct constructing)
{
    MOZ_ASSERT(callee.nonLazyScript() == script);

    /* Initialize stack frame members. */
    flags_ = 0;
    if (constructing)
        flags_ |= CONSTRUCTING;
    argv_ = argv;
    script_ = script;
    nactual_ = nactual;
    envChain_ = callee.environment();
    prev_ = prev;
    prevpc_ = prevpc;
    prevsp_ = prevsp;

    if (script->isDebuggee())
        setIsDebuggee();

    initLocals();
}

inline void
InterpreterFrame::initLocals()
{
    SetValueRangeToUndefined(slots(), script()->nfixed());
}

inline Value&
InterpreterFrame::unaliasedLocal(uint32_t i)
{
    MOZ_ASSERT(i < script()->nfixed());
    return slots()[i];
}

inline Value&
InterpreterFrame::unaliasedFormal(unsigned i, MaybeCheckAliasing checkAliasing)
{
    MOZ_ASSERT(i < numFormalArgs());
    MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals());
    MOZ_ASSERT_IF(checkAliasing, !script()->formalIsAliased(i));
    return argv()[i];
}

inline Value&
InterpreterFrame::unaliasedActual(unsigned i, MaybeCheckAliasing checkAliasing)
{
    MOZ_ASSERT(i < numActualArgs());
    MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals());
    MOZ_ASSERT_IF(checkAliasing && i < numFormalArgs(), !script()->formalIsAliased(i));
    return argv()[i];
}

template <class Op>
inline void
InterpreterFrame::unaliasedForEachActual(Op op)
{
    // Don't assert !script()->funHasAnyAliasedFormal() since this function is
    // called from ArgumentsObject::createUnexpected() which can access aliased
    // slots.

    const Value* argsEnd = argv() + numActualArgs();
    for (const Value* p = argv(); p < argsEnd; ++p)
        op(*p);
}

struct CopyTo
{
    Value* dst;
    explicit CopyTo(Value* dst) : dst(dst) {}
    void operator()(const Value& src) { *dst++ = src; }
};

struct CopyToHeap
{
    GCPtrValue* dst;
    explicit CopyToHeap(GCPtrValue* dst) : dst(dst) {}
    void operator()(const Value& src) { dst->init(src); ++dst; }
};

inline ArgumentsObject&
InterpreterFrame::argsObj() const
{
    MOZ_ASSERT(script()->needsArgsObj());
    MOZ_ASSERT(flags_ & HAS_ARGS_OBJ);
    return *argsObj_;
}

inline void
InterpreterFrame::initArgsObj(ArgumentsObject& argsobj)
{
    MOZ_ASSERT(script()->needsArgsObj());
    flags_ |= HAS_ARGS_OBJ;
    argsObj_ = &argsobj;
}

inline EnvironmentObject&
InterpreterFrame::aliasedEnvironment(EnvironmentCoordinate ec) const
{
    JSObject* env = &environmentChain()->as<EnvironmentObject>();
    for (unsigned i = ec.hops(); i; i--)
        env = &env->as<EnvironmentObject>().enclosingEnvironment();
    return env->as<EnvironmentObject>();
}

template <typename SpecificEnvironment>
inline void
InterpreterFrame::pushOnEnvironmentChain(SpecificEnvironment& env)
{
    MOZ_ASSERT(*environmentChain() == env.enclosingEnvironment());
    envChain_ = &env;
    if (IsFrameInitialEnvironment(this, env))
        flags_ |= HAS_INITIAL_ENV;
}

template <typename SpecificEnvironment>
inline void
InterpreterFrame::popOffEnvironmentChain()
{
    MOZ_ASSERT(envChain_->is<SpecificEnvironment>());
    envChain_ = &envChain_->as<SpecificEnvironment>().enclosingEnvironment();
}

inline void
InterpreterFrame::replaceInnermostEnvironment(EnvironmentObject& env)
{
    MOZ_ASSERT(env.enclosingEnvironment() ==
               envChain_->as<EnvironmentObject>().enclosingEnvironment());
    envChain_ = &env;
}

bool
InterpreterFrame::hasInitialEnvironment() const
{
    MOZ_ASSERT(script()->initialEnvironmentShape());
    return flags_ & HAS_INITIAL_ENV;
}

inline CallObject&
InterpreterFrame::callObj() const
{
    MOZ_ASSERT(callee().needsCallObject());

    JSObject* pobj = environmentChain();
    while (MOZ_UNLIKELY(!pobj->is<CallObject>()))
        pobj = pobj->enclosingEnvironment();
    return pobj->as<CallObject>();
}

inline void
InterpreterFrame::unsetIsDebuggee()
{
    MOZ_ASSERT(!script()->isDebuggee());
    flags_ &= ~DEBUGGEE;
}

/*****************************************************************************/

inline void
InterpreterStack::purge(JSRuntime* rt)
{
    rt->gc.freeUnusedLifoBlocksAfterSweeping(&allocator_);
}

uint8_t*
InterpreterStack::allocateFrame(JSContext* cx, size_t size)
{
    size_t maxFrames;
    if (cx->compartment()->principals() == cx->runtime()->trustedPrincipals())
        maxFrames = MAX_FRAMES_TRUSTED;
    else
        maxFrames = MAX_FRAMES;

    if (MOZ_UNLIKELY(frameCount_ >= maxFrames)) {
        ReportOverRecursed(cx);
        return nullptr;
    }

    uint8_t* buffer = reinterpret_cast<uint8_t*>(allocator_.alloc(size));
    if (!buffer) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    frameCount_++;
    return buffer;
}

MOZ_ALWAYS_INLINE InterpreterFrame*
InterpreterStack::getCallFrame(JSContext* cx, const CallArgs& args, HandleScript script,
                               MaybeConstruct constructing, Value** pargv)
{
    JSFunction* fun = &args.callee().as<JSFunction>();

    MOZ_ASSERT(fun->nonLazyScript() == script);
    unsigned nformal = fun->nargs();
    unsigned nvals = script->nslots();

    if (args.length() >= nformal) {
        *pargv = args.array();
        uint8_t* buffer = allocateFrame(cx, sizeof(InterpreterFrame) + nvals * sizeof(Value));
        return reinterpret_cast<InterpreterFrame*>(buffer);
    }

    // Pad any missing arguments with |undefined|.
    MOZ_ASSERT(args.length() < nformal);

    unsigned nfunctionState = 2 + constructing; // callee, |this|, |new.target|

    nvals += nformal + nfunctionState;
    uint8_t* buffer = allocateFrame(cx, sizeof(InterpreterFrame) + nvals * sizeof(Value));
    if (!buffer)
        return nullptr;

    Value* argv = reinterpret_cast<Value*>(buffer);
    unsigned nmissing = nformal - args.length();

    mozilla::PodCopy(argv, args.base(), 2 + args.length());
    SetValueRangeToUndefined(argv + 2 + args.length(), nmissing);

    if (constructing)
        argv[2 + nformal] = args.newTarget();

    *pargv = argv + 2;
    return reinterpret_cast<InterpreterFrame*>(argv + nfunctionState + nformal);
}

MOZ_ALWAYS_INLINE bool
InterpreterStack::pushInlineFrame(JSContext* cx, InterpreterRegs& regs, const CallArgs& args,
                                  HandleScript script, MaybeConstruct constructing)
{
    RootedFunction callee(cx, &args.callee().as<JSFunction>());
    MOZ_ASSERT(regs.sp == args.end());
    MOZ_ASSERT(callee->nonLazyScript() == script);

    script->ensureNonLazyCanonicalFunction();

    InterpreterFrame* prev = regs.fp();
    jsbytecode* prevpc = regs.pc;
    Value* prevsp = regs.sp;
    MOZ_ASSERT(prev);

    LifoAlloc::Mark mark = allocator_.mark();

    Value* argv;
    InterpreterFrame* fp = getCallFrame(cx, args, script, constructing, &argv);
    if (!fp)
        return false;

    fp->mark_ = mark;

    /* Initialize frame, locals, regs. */
    fp->initCallFrame(prev, prevpc, prevsp, *callee, script, argv, args.length(),
                      constructing);

    regs.prepareToRun(*fp, script);
    return true;
}

MOZ_ALWAYS_INLINE bool
InterpreterStack::resumeGeneratorCallFrame(JSContext* cx, InterpreterRegs& regs,
                                           HandleFunction callee, HandleValue newTarget,
                                           HandleObject envChain)
{
    MOZ_ASSERT(callee->isGenerator() || callee->isAsync());
    RootedScript script(cx, JSFunction::getOrCreateScript(cx, callee));
    InterpreterFrame* prev = regs.fp();
    jsbytecode* prevpc = regs.pc;
    Value* prevsp = regs.sp;
    MOZ_ASSERT(prev);

    script->ensureNonLazyCanonicalFunction();

    LifoAlloc::Mark mark = allocator_.mark();

    MaybeConstruct constructing = MaybeConstruct(newTarget.isObject());

    // Include callee, |this|, and maybe |new.target|
    unsigned nformal = callee->nargs();
    unsigned nvals = 2 + constructing + nformal + script->nslots();

    uint8_t* buffer = allocateFrame(cx, sizeof(InterpreterFrame) + nvals * sizeof(Value));
    if (!buffer)
        return false;

    Value* argv = reinterpret_cast<Value*>(buffer) + 2;
    argv[-2] = ObjectValue(*callee);
    argv[-1] = UndefinedValue();
    SetValueRangeToUndefined(argv, nformal);
    if (constructing)
        argv[nformal] = newTarget;

    InterpreterFrame* fp = reinterpret_cast<InterpreterFrame*>(argv + nformal + constructing);
    fp->mark_ = mark;
    fp->initCallFrame(prev, prevpc, prevsp, *callee, script, argv, 0, constructing);
    fp->resumeGeneratorFrame(envChain);

    regs.prepareToRun(*fp, script);
    return true;
}

MOZ_ALWAYS_INLINE void
InterpreterStack::popInlineFrame(InterpreterRegs& regs)
{
    InterpreterFrame* fp = regs.fp();
    regs.popInlineFrame();
    regs.sp[-1] = fp->returnValue();
    releaseFrame(fp);
    MOZ_ASSERT(regs.fp());
}

template <class Op>
inline void
FrameIter::unaliasedForEachActual(JSContext* cx, Op op)
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        interpFrame()->unaliasedForEachActual(op);
        return;
      case JIT:
        MOZ_ASSERT(isJSJit());
        if (jsJitFrame().isIonJS()) {
            jit::MaybeReadFallback recover(cx, activation()->asJit(), &jsJitFrame());
            ionInlineFrames_.unaliasedForEachActual(cx, op, jit::ReadFrame_Actuals, recover);
        } else if (jsJitFrame().isBailoutJS()) {
            // :TODO: (Bug 1070962) If we are introspecting the frame which is
            // being bailed, then we might be in the middle of recovering
            // instructions. Stacking computeInstructionResults implies that we
            // might be recovering result twice. In the mean time, to avoid
            // that, we just return Undefined values for instruction results
            // which are not yet recovered.
            jit::MaybeReadFallback fallback;
            ionInlineFrames_.unaliasedForEachActual(cx, op, jit::ReadFrame_Actuals, fallback);
        } else {
            MOZ_ASSERT(jsJitFrame().isBaselineJS());
            jsJitFrame().unaliasedForEachActual(op, jit::ReadFrame_Actuals);
        }
        return;
    }
    MOZ_CRASH("Unexpected state");
}

inline HandleValue
AbstractFramePtr::returnValue() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->returnValue();
    if (isWasmDebugFrame())
        return asWasmDebugFrame()->returnValue();
    return asBaselineFrame()->returnValue();
}

inline void
AbstractFramePtr::setReturnValue(const Value& rval) const
{
    if (isInterpreterFrame()) {
        asInterpreterFrame()->setReturnValue(rval);
        return;
    }
    if (isBaselineFrame()) {
        asBaselineFrame()->setReturnValue(rval);
        return;
    }
    if (isWasmDebugFrame()) {
        // TODO handle wasm function return value
        // The function is called from Debugger::slowPathOnLeaveFrame --
        // ignoring value for wasm.
        return;
    }
    asRematerializedFrame()->setReturnValue(rval);
}

inline JSObject*
AbstractFramePtr::environmentChain() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->environmentChain();
    if (isBaselineFrame())
        return asBaselineFrame()->environmentChain();
    if (isWasmDebugFrame())
        return &global()->lexicalEnvironment();
    return asRematerializedFrame()->environmentChain();
}

template <typename SpecificEnvironment>
inline void
AbstractFramePtr::pushOnEnvironmentChain(SpecificEnvironment& env)
{
    if (isInterpreterFrame()) {
        asInterpreterFrame()->pushOnEnvironmentChain(env);
        return;
    }
    if (isBaselineFrame()) {
        asBaselineFrame()->pushOnEnvironmentChain(env);
        return;
    }
    asRematerializedFrame()->pushOnEnvironmentChain(env);
}

template <typename SpecificEnvironment>
inline void
AbstractFramePtr::popOffEnvironmentChain()
{
    if (isInterpreterFrame()) {
        asInterpreterFrame()->popOffEnvironmentChain<SpecificEnvironment>();
        return;
    }
    if (isBaselineFrame()) {
        asBaselineFrame()->popOffEnvironmentChain<SpecificEnvironment>();
        return;
    }
    asRematerializedFrame()->popOffEnvironmentChain<SpecificEnvironment>();
}

inline CallObject&
AbstractFramePtr::callObj() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->callObj();
    if (isBaselineFrame())
        return asBaselineFrame()->callObj();
    return asRematerializedFrame()->callObj();
}

inline bool
AbstractFramePtr::initFunctionEnvironmentObjects(JSContext* cx)
{
    return js::InitFunctionEnvironmentObjects(cx, *this);
}

inline bool
AbstractFramePtr::pushVarEnvironment(JSContext* cx, HandleScope scope)
{
    return js::PushVarEnvironmentObject(cx, scope, *this);
}

inline JSCompartment*
AbstractFramePtr::compartment() const
{
    return environmentChain()->compartment();
}

inline unsigned
AbstractFramePtr::numActualArgs() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->numActualArgs();
    if (isBaselineFrame())
        return asBaselineFrame()->numActualArgs();
    return asRematerializedFrame()->numActualArgs();
}

inline unsigned
AbstractFramePtr::numFormalArgs() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->numFormalArgs();
    if (isBaselineFrame())
        return asBaselineFrame()->numFormalArgs();
    return asRematerializedFrame()->numFormalArgs();
}

inline Value&
AbstractFramePtr::unaliasedLocal(uint32_t i)
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->unaliasedLocal(i);
    if (isBaselineFrame())
        return asBaselineFrame()->unaliasedLocal(i);
    return asRematerializedFrame()->unaliasedLocal(i);
}

inline Value&
AbstractFramePtr::unaliasedFormal(unsigned i, MaybeCheckAliasing checkAliasing)
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->unaliasedFormal(i, checkAliasing);
    if (isBaselineFrame())
        return asBaselineFrame()->unaliasedFormal(i, checkAliasing);
    return asRematerializedFrame()->unaliasedFormal(i, checkAliasing);
}

inline Value&
AbstractFramePtr::unaliasedActual(unsigned i, MaybeCheckAliasing checkAliasing)
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->unaliasedActual(i, checkAliasing);
    if (isBaselineFrame())
        return asBaselineFrame()->unaliasedActual(i, checkAliasing);
    return asRematerializedFrame()->unaliasedActual(i, checkAliasing);
}

inline bool
AbstractFramePtr::hasInitialEnvironment() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->hasInitialEnvironment();
    if (isBaselineFrame())
        return asBaselineFrame()->hasInitialEnvironment();
    return asRematerializedFrame()->hasInitialEnvironment();
}

inline bool
AbstractFramePtr::isGlobalFrame() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->isGlobalFrame();
    if (isBaselineFrame())
        return asBaselineFrame()->isGlobalFrame();
    if (isWasmDebugFrame())
        return false;
    return asRematerializedFrame()->isGlobalFrame();
}

inline bool
AbstractFramePtr::isModuleFrame() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->isModuleFrame();
    if (isBaselineFrame())
        return asBaselineFrame()->isModuleFrame();
    if (isWasmDebugFrame())
        return false;
    return asRematerializedFrame()->isModuleFrame();
}

inline bool
AbstractFramePtr::isEvalFrame() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->isEvalFrame();
    if (isBaselineFrame())
        return asBaselineFrame()->isEvalFrame();
    if (isWasmDebugFrame())
        return false;
    MOZ_ASSERT(isRematerializedFrame());
    return false;
}

inline bool
AbstractFramePtr::isDebuggerEvalFrame() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->isDebuggerEvalFrame();
    if (isBaselineFrame())
        return asBaselineFrame()->isDebuggerEvalFrame();
    MOZ_ASSERT(isRematerializedFrame());
    return false;
}

inline bool
AbstractFramePtr::isDebuggee() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->isDebuggee();
    if (isBaselineFrame())
        return asBaselineFrame()->isDebuggee();
    if (isWasmDebugFrame())
        return asWasmDebugFrame()->isDebuggee();
    return asRematerializedFrame()->isDebuggee();
}

inline void
AbstractFramePtr::setIsDebuggee()
{
    if (isInterpreterFrame())
        asInterpreterFrame()->setIsDebuggee();
    else if (isBaselineFrame())
        asBaselineFrame()->setIsDebuggee();
    else if (isWasmDebugFrame())
        asWasmDebugFrame()->setIsDebuggee();
    else
        asRematerializedFrame()->setIsDebuggee();
}

inline void
AbstractFramePtr::unsetIsDebuggee()
{
    if (isInterpreterFrame())
        asInterpreterFrame()->unsetIsDebuggee();
    else if (isBaselineFrame())
        asBaselineFrame()->unsetIsDebuggee();
    else if (isWasmDebugFrame())
        asWasmDebugFrame()->unsetIsDebuggee();
    else
        asRematerializedFrame()->unsetIsDebuggee();
}

inline bool
AbstractFramePtr::hasArgs() const
{
    return isFunctionFrame();
}

inline bool
AbstractFramePtr::hasScript() const
{
    return !isWasmDebugFrame();
}

inline JSScript*
AbstractFramePtr::script() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->script();
    if (isBaselineFrame())
        return asBaselineFrame()->script();
    return asRematerializedFrame()->script();
}

inline wasm::Instance*
AbstractFramePtr::wasmInstance() const
{
    return asWasmDebugFrame()->instance();
}

inline GlobalObject*
AbstractFramePtr::global() const
{
    if (isWasmDebugFrame())
        return &wasmInstance()->object()->global();
    return &script()->global();
}

inline JSFunction*
AbstractFramePtr::callee() const
{
    if (isInterpreterFrame())
        return &asInterpreterFrame()->callee();
    if (isBaselineFrame())
        return asBaselineFrame()->callee();
    return asRematerializedFrame()->callee();
}

inline Value
AbstractFramePtr::calleev() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->calleev();
    if (isBaselineFrame())
        return asBaselineFrame()->calleev();
    return asRematerializedFrame()->calleev();
}

inline bool
AbstractFramePtr::isFunctionFrame() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->isFunctionFrame();
    if (isBaselineFrame())
        return asBaselineFrame()->isFunctionFrame();
    if (isWasmDebugFrame())
        return false;
    return asRematerializedFrame()->isFunctionFrame();
}

inline bool
AbstractFramePtr::isNonStrictDirectEvalFrame() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->isNonStrictDirectEvalFrame();
    if (isBaselineFrame())
        return asBaselineFrame()->isNonStrictDirectEvalFrame();
    MOZ_ASSERT(isRematerializedFrame());
    return false;
}

inline bool
AbstractFramePtr::isStrictEvalFrame() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->isStrictEvalFrame();
    if (isBaselineFrame())
        return asBaselineFrame()->isStrictEvalFrame();
    MOZ_ASSERT(isRematerializedFrame());
    return false;
}

inline Value*
AbstractFramePtr::argv() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->argv();
    if (isBaselineFrame())
        return asBaselineFrame()->argv();
    return asRematerializedFrame()->argv();
}

inline bool
AbstractFramePtr::hasArgsObj() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->hasArgsObj();
    if (isBaselineFrame())
        return asBaselineFrame()->hasArgsObj();
    return asRematerializedFrame()->hasArgsObj();
}

inline ArgumentsObject&
AbstractFramePtr::argsObj() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->argsObj();
    if (isBaselineFrame())
        return asBaselineFrame()->argsObj();
    return asRematerializedFrame()->argsObj();
}

inline void
AbstractFramePtr::initArgsObj(ArgumentsObject& argsobj) const
{
    if (isInterpreterFrame()) {
        asInterpreterFrame()->initArgsObj(argsobj);
        return;
    }
    asBaselineFrame()->initArgsObj(argsobj);
}

inline bool
AbstractFramePtr::prevUpToDate() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->prevUpToDate();
    if (isBaselineFrame())
        return asBaselineFrame()->prevUpToDate();
    if (isWasmDebugFrame())
        return asWasmDebugFrame()->prevUpToDate();
    return asRematerializedFrame()->prevUpToDate();
}

inline void
AbstractFramePtr::setPrevUpToDate() const
{
    if (isInterpreterFrame()) {
        asInterpreterFrame()->setPrevUpToDate();
        return;
    }
    if (isBaselineFrame()) {
        asBaselineFrame()->setPrevUpToDate();
        return;
    }
    if (isWasmDebugFrame()) {
        asWasmDebugFrame()->setPrevUpToDate();
        return;
    }
    asRematerializedFrame()->setPrevUpToDate();
}

inline void
AbstractFramePtr::unsetPrevUpToDate() const
{
    if (isInterpreterFrame()) {
        asInterpreterFrame()->unsetPrevUpToDate();
        return;
    }
    if (isBaselineFrame()) {
        asBaselineFrame()->unsetPrevUpToDate();
        return;
    }
    if (isWasmDebugFrame()) {
        asWasmDebugFrame()->unsetPrevUpToDate();
        return;
    }
    asRematerializedFrame()->unsetPrevUpToDate();
}

inline Value&
AbstractFramePtr::thisArgument() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->thisArgument();
    if (isBaselineFrame())
        return asBaselineFrame()->thisArgument();
    return asRematerializedFrame()->thisArgument();
}

inline Value
AbstractFramePtr::newTarget() const
{
    if (isInterpreterFrame())
        return asInterpreterFrame()->newTarget();
    if (isBaselineFrame())
        return asBaselineFrame()->newTarget();
    return asRematerializedFrame()->newTarget();
}

inline bool
AbstractFramePtr::debuggerNeedsCheckPrimitiveReturn() const
{
    if (isWasmDebugFrame())
        return false;
    return script()->isDerivedClassConstructor();
}

ActivationEntryMonitor::~ActivationEntryMonitor()
{
    if (entryMonitor_)
        entryMonitor_->Exit(cx_);

    cx_->entryMonitor = entryMonitor_;
}

Activation::Activation(JSContext* cx, Kind kind)
  : cx_(cx),
    compartment_(cx->compartment()),
    prev_(cx->activation_),
    prevProfiling_(prev_ ? prev_->mostRecentProfiling() : nullptr),
    hideScriptedCallerCount_(0),
    frameCache_(cx),
    asyncStack_(cx, cx->asyncStackForNewActivations()),
    asyncCause_(cx->asyncCauseForNewActivations),
    asyncCallIsExplicit_(cx->asyncCallIsExplicit),
    kind_(kind)
{
    cx->asyncStackForNewActivations() = nullptr;
    cx->asyncCauseForNewActivations = nullptr;
    cx->asyncCallIsExplicit = false;
    cx->activation_ = this;
}

Activation::~Activation()
{
    MOZ_ASSERT_IF(isProfiling(), this != cx_->profilingActivation_);
    MOZ_ASSERT(cx_->activation_ == this);
    MOZ_ASSERT(hideScriptedCallerCount_ == 0);
    cx_->activation_ = prev_;
    cx_->asyncCauseForNewActivations = asyncCause_;
    cx_->asyncStackForNewActivations() = asyncStack_;
    cx_->asyncCallIsExplicit = asyncCallIsExplicit_;
}

bool
Activation::isProfiling() const
{
    if (isInterpreter())
        return asInterpreter()->isProfiling();

    MOZ_ASSERT(isJit());
    return asJit()->isProfiling();
}

Activation*
Activation::mostRecentProfiling()
{
    if (isProfiling())
        return this;
    return prevProfiling_;
}

inline LiveSavedFrameCache*
Activation::getLiveSavedFrameCache(JSContext* cx) {
    if (!frameCache_.get().initialized() && !frameCache_.get().init(cx))
        return nullptr;
    return frameCache_.address();
}

InterpreterActivation::InterpreterActivation(RunState& state, JSContext* cx,
                                             InterpreterFrame* entryFrame)
  : Activation(cx, Interpreter),
    entryFrame_(entryFrame),
    opMask_(0)
#ifdef DEBUG
  , oldFrameCount_(cx->interpreterStack().frameCount_)
#endif
{
    regs_.prepareToRun(*entryFrame, state.script());
    MOZ_ASSERT(regs_.pc == state.script()->code());
    MOZ_ASSERT_IF(entryFrame_->isEvalFrame(), state.script()->isActiveEval());
}

InterpreterActivation::~InterpreterActivation()
{
    // Pop all inline frames.
    while (regs_.fp() != entryFrame_)
        popInlineFrame(regs_.fp());

    MOZ_ASSERT(oldFrameCount_ == cx_->interpreterStack().frameCount_);
    MOZ_ASSERT_IF(oldFrameCount_ == 0, cx_->interpreterStack().allocator_.used() == 0);

    if (entryFrame_)
        cx_->interpreterStack().releaseFrame(entryFrame_);
}

inline bool
InterpreterActivation::pushInlineFrame(const CallArgs& args, HandleScript script,
                                       MaybeConstruct constructing)
{
    if (!cx_->interpreterStack().pushInlineFrame(cx_, regs_, args, script, constructing))
        return false;
    MOZ_ASSERT(regs_.fp()->script()->compartment() == compartment());
    return true;
}

inline void
InterpreterActivation::popInlineFrame(InterpreterFrame* frame)
{
    (void)frame; // Quell compiler warning.
    MOZ_ASSERT(regs_.fp() == frame);
    MOZ_ASSERT(regs_.fp() != entryFrame_);

    cx_->interpreterStack().popInlineFrame(regs_);
}

inline bool
InterpreterActivation::resumeGeneratorFrame(HandleFunction callee, HandleValue newTarget,
                                            HandleObject envChain)
{
    InterpreterStack& stack = cx_->interpreterStack();
    if (!stack.resumeGeneratorCallFrame(cx_, regs_, callee, newTarget, envChain))
        return false;

    MOZ_ASSERT(regs_.fp()->script()->compartment() == compartment_);
    return true;
}

/* static */ inline Maybe<LiveSavedFrameCache::FramePtr>
LiveSavedFrameCache::FramePtr::create(const FrameIter& iter)
{
    if (iter.done())
        return mozilla::Nothing();

    if (iter.isPhysicalJitFrame())
        return mozilla::Some(FramePtr(iter.physicalJitFrame()));

    if (!iter.hasUsableAbstractFramePtr())
        return mozilla::Nothing();

    auto afp = iter.abstractFramePtr();

    if (afp.isInterpreterFrame())
        return mozilla::Some(FramePtr(afp.asInterpreterFrame()));
    if (afp.isWasmDebugFrame())
        return mozilla::Some(FramePtr(afp.asWasmDebugFrame()));
    if (afp.isRematerializedFrame())
        return mozilla::Some(FramePtr(afp.asRematerializedFrame()));

    MOZ_CRASH("unexpected frame type");
}

/* static */ inline LiveSavedFrameCache::FramePtr
LiveSavedFrameCache::FramePtr::create(AbstractFramePtr afp)
{
    MOZ_ASSERT(afp);

    if (afp.isBaselineFrame()) {
        js::jit::CommonFrameLayout *common = afp.asBaselineFrame()->framePrefix();
        return FramePtr(common);
    }
    if (afp.isInterpreterFrame())
        return FramePtr(afp.asInterpreterFrame());
    if (afp.isWasmDebugFrame())
        return FramePtr(afp.asWasmDebugFrame());
    if (afp.isRematerializedFrame())
        return FramePtr(afp.asRematerializedFrame());

    MOZ_CRASH("unexpected frame type");
}

struct LiveSavedFrameCache::FramePtr::HasCachedMatcher {
    template<typename Frame>
    bool match(Frame* f) const { return f->hasCachedSavedFrame(); }
};

inline bool
LiveSavedFrameCache::FramePtr::hasCachedSavedFrame() const {
    return ptr.match(HasCachedMatcher());
}

struct LiveSavedFrameCache::FramePtr::SetHasCachedMatcher {
    template<typename Frame>
    void match(Frame* f) { f->setHasCachedSavedFrame(); }
};

inline void
LiveSavedFrameCache::FramePtr::setHasCachedSavedFrame() {
    ptr.match(SetHasCachedMatcher());
}

} /* namespace js */

#endif /* vm_Stack_inl_h */
