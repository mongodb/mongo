/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Stack-inl.h"

#include "gc/Marking.h"
#include "jit/BaselineFrame.h"
#include "jit/JitcodeMap.h"
#include "jit/JitCompartment.h"
#include "vm/Debugger.h"
#include "vm/JSContext.h"
#include "vm/Opcodes.h"

#include "jit/JSJitFrameIter-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/Probes-inl.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::Maybe;

/*****************************************************************************/

void
InterpreterFrame::initExecuteFrame(JSContext* cx, HandleScript script,
                                   AbstractFramePtr evalInFramePrev,
                                   const Value& newTargetValue, HandleObject envChain)
{
    flags_ = 0;
    script_ = script;

    // newTarget = NullValue is an initial sentinel for "please fill me in from the stack".
    // It should never be passed from Ion code.
    RootedValue newTarget(cx, newTargetValue);
    if (script->isDirectEvalInFunction()) {
        FrameIter iter(cx);
        if (newTarget.isNull() &&
            iter.hasScript() &&
            iter.script()->bodyScope()->hasOnChain(ScopeKind::Function))
        {
            newTarget = iter.newTarget();
        }
    } else if (evalInFramePrev) {
        if (newTarget.isNull() &&
            evalInFramePrev.hasScript() &&
            evalInFramePrev.script()->bodyScope()->hasOnChain(ScopeKind::Function))
        {
            newTarget = evalInFramePrev.newTarget();
        }
    }

    Value* dstvp = (Value*)this - 1;
    dstvp[0] = newTarget;

    envChain_ = envChain.get();
    prev_ = nullptr;
    prevpc_ = nullptr;
    prevsp_ = nullptr;

    evalInFramePrev_ = evalInFramePrev;
    MOZ_ASSERT_IF(evalInFramePrev, isDebuggerEvalFrame());

    if (script->isDebuggee())
        setIsDebuggee();

#ifdef DEBUG
    Debug_SetValueRangeToCrashOnTouch(&rval_, 1);
#endif
}

bool
InterpreterFrame::isNonGlobalEvalFrame() const
{
    return isEvalFrame() && script()->bodyScope()->as<EvalScope>().isNonGlobal();
}

ArrayObject*
InterpreterFrame::createRestParameter(JSContext* cx)
{
    MOZ_ASSERT(script()->hasRest());
    unsigned nformal = callee().nargs() - 1, nactual = numActualArgs();
    unsigned nrest = (nactual > nformal) ? nactual - nformal : 0;
    Value* restvp = argv() + nformal;
    return ObjectGroup::newArrayObject(cx, restvp, nrest, GenericObject,
                                       ObjectGroup::NewArrayKind::UnknownIndex);
}

static inline void
AssertScopeMatchesEnvironment(Scope* scope, JSObject* originalEnv)
{
#ifdef DEBUG
    JSObject* env = originalEnv;
    for (ScopeIter si(scope); si; si++) {
        if (si.kind() == ScopeKind::NonSyntactic) {
            while (env->is<WithEnvironmentObject>() ||
                   env->is<NonSyntacticVariablesObject>() ||
                   (env->is<LexicalEnvironmentObject>() &&
                    !env->as<LexicalEnvironmentObject>().isSyntactic()))
            {
                MOZ_ASSERT(!IsSyntacticEnvironment(env));
                env = &env->as<EnvironmentObject>().enclosingEnvironment();
            }
        } else if (si.hasSyntacticEnvironment()) {
            switch (si.kind()) {
              case ScopeKind::Function:
                MOZ_ASSERT(env->as<CallObject>().callee().existingScriptNonDelazifying() ==
                           si.scope()->as<FunctionScope>().script());
                env = &env->as<CallObject>().enclosingEnvironment();
                break;

              case ScopeKind::FunctionBodyVar:
              case ScopeKind::ParameterExpressionVar:
                MOZ_ASSERT(&env->as<VarEnvironmentObject>().scope() == si.scope());
                env = &env->as<VarEnvironmentObject>().enclosingEnvironment();
                break;

              case ScopeKind::Lexical:
              case ScopeKind::SimpleCatch:
              case ScopeKind::Catch:
              case ScopeKind::NamedLambda:
              case ScopeKind::StrictNamedLambda:
                MOZ_ASSERT(&env->as<LexicalEnvironmentObject>().scope() == si.scope());
                env = &env->as<LexicalEnvironmentObject>().enclosingEnvironment();
                break;

              case ScopeKind::With:
                MOZ_ASSERT(&env->as<WithEnvironmentObject>().scope() == si.scope());
                env = &env->as<WithEnvironmentObject>().enclosingEnvironment();
                break;

              case ScopeKind::Eval:
              case ScopeKind::StrictEval:
                env = &env->as<VarEnvironmentObject>().enclosingEnvironment();
                break;

              case ScopeKind::Global:
                MOZ_ASSERT(env->as<LexicalEnvironmentObject>().isGlobal());
                env = &env->as<LexicalEnvironmentObject>().enclosingEnvironment();
                MOZ_ASSERT(env->is<GlobalObject>());
                break;

              case ScopeKind::NonSyntactic:
                MOZ_CRASH("NonSyntactic should not have a syntactic environment");
                break;

              case ScopeKind::Module:
                MOZ_ASSERT(env->as<ModuleEnvironmentObject>().module().script() ==
                           si.scope()->as<ModuleScope>().script());
                env = &env->as<ModuleEnvironmentObject>().enclosingEnvironment();
                break;

              case ScopeKind::WasmInstance:
                env = &env->as<WasmInstanceEnvironmentObject>().enclosingEnvironment();
                break;

              case ScopeKind::WasmFunction:
                env = &env->as<WasmFunctionCallObject>().enclosingEnvironment();
                break;
            }
        }
    }

    // In the case of a non-syntactic env chain, the immediate parent of the
    // outermost non-syntactic env may be the global lexical env, or, if
    // called from Debugger, a DebugEnvironmentProxy.
    //
    // In the case of a syntactic env chain, the outermost env is always a
    // GlobalObject.
    MOZ_ASSERT(env->is<GlobalObject>() || IsGlobalLexicalEnvironment(env) ||
               env->is<DebugEnvironmentProxy>());
#endif
}

static inline void
AssertScopeMatchesEnvironment(InterpreterFrame* fp, jsbytecode* pc)
{
#ifdef DEBUG
    // If we OOMed before fully initializing the environment chain, the scope
    // and environment will definitely mismatch.
    if (fp->script()->initialEnvironmentShape() && fp->hasInitialEnvironment())
        AssertScopeMatchesEnvironment(fp->script()->innermostScope(pc), fp->environmentChain());
#endif
}

bool
InterpreterFrame::initFunctionEnvironmentObjects(JSContext* cx)
{
    return js::InitFunctionEnvironmentObjects(cx, this);
}

bool
InterpreterFrame::prologue(JSContext* cx)
{
    RootedScript script(cx, this->script());

    MOZ_ASSERT(cx->interpreterRegs().pc == script->code());

    if (isEvalFrame()) {
        if (!script->bodyScope()->hasEnvironment()) {
            MOZ_ASSERT(!script->strict());
            // Non-strict eval may introduce var bindings that conflict with
            // lexical bindings in an enclosing lexical scope.
            RootedObject varObjRoot(cx, &varObj());
            if (!CheckEvalDeclarationConflicts(cx, script, environmentChain(), varObjRoot))
                return false;
        }
        return probes::EnterScript(cx, script, nullptr, this);
    }

    if (isGlobalFrame()) {
        Rooted<LexicalEnvironmentObject*> lexicalEnv(cx);
        RootedObject varObjRoot(cx);
        if (script->hasNonSyntacticScope()) {
            lexicalEnv = &extensibleLexicalEnvironment();
            varObjRoot = &varObj();
        } else {
            lexicalEnv = &cx->global()->lexicalEnvironment();
            varObjRoot = cx->global();
        }
        if (!CheckGlobalDeclarationConflicts(cx, script, lexicalEnv, varObjRoot))
            return false;
        return probes::EnterScript(cx, script, nullptr, this);
    }

    if (isModuleFrame())
        return probes::EnterScript(cx, script, nullptr, this);

    // At this point, we've yet to push any environments. Check that they
    // match the enclosing scope.
    AssertScopeMatchesEnvironment(script->enclosingScope(), environmentChain());

    MOZ_ASSERT(isFunctionFrame());
    if (callee().needsFunctionEnvironmentObjects() && !initFunctionEnvironmentObjects(cx))
        return false;

    MOZ_ASSERT_IF(isConstructing(),
                  thisArgument().isObject() || thisArgument().isMagic(JS_UNINITIALIZED_LEXICAL));

    return probes::EnterScript(cx, script, script->functionNonDelazifying(), this);
}

void
InterpreterFrame::epilogue(JSContext* cx, jsbytecode* pc)
{
    RootedScript script(cx, this->script());
    probes::ExitScript(cx, script, script->functionNonDelazifying(), hasPushedGeckoProfilerFrame());

    // Check that the scope matches the environment at the point of leaving
    // the frame.
    AssertScopeMatchesEnvironment(this, pc);

    EnvironmentIter ei(cx, this, pc);
    UnwindAllEnvironmentsInFrame(cx, ei);

    if (isFunctionFrame()) {
        if (!callee().isGenerator() &&
            !callee().isAsync() &&
            isConstructing() &&
            thisArgument().isObject() &&
            returnValue().isPrimitive())
        {
            setReturnValue(thisArgument());
        }

        return;
    }

    MOZ_ASSERT(isEvalFrame() || isGlobalFrame() || isModuleFrame());
}

bool
InterpreterFrame::checkReturn(JSContext* cx, HandleValue thisv)
{
    MOZ_ASSERT(script()->isDerivedClassConstructor());
    MOZ_ASSERT(isFunctionFrame());
    MOZ_ASSERT(callee().isClassConstructor());

    HandleValue retVal = returnValue();
    if (retVal.isObject())
        return true;

    if (!retVal.isUndefined()) {
        ReportValueError(cx, JSMSG_BAD_DERIVED_RETURN, JSDVG_IGNORE_STACK, retVal, nullptr);
        return false;
    }

    if (thisv.isMagic(JS_UNINITIALIZED_LEXICAL))
        return ThrowUninitializedThis(cx, this);

    setReturnValue(thisv);
    return true;
}

bool
InterpreterFrame::pushVarEnvironment(JSContext* cx, HandleScope scope)
{
    return js::PushVarEnvironmentObject(cx, scope, this);
}

bool
InterpreterFrame::pushLexicalEnvironment(JSContext* cx, Handle<LexicalScope*> scope)
{
    LexicalEnvironmentObject* env = LexicalEnvironmentObject::create(cx, scope, this);
    if (!env)
        return false;

    pushOnEnvironmentChain(*env);
    return true;
}

bool
InterpreterFrame::freshenLexicalEnvironment(JSContext* cx)
{
    Rooted<LexicalEnvironmentObject*> env(cx, &envChain_->as<LexicalEnvironmentObject>());
    LexicalEnvironmentObject* fresh = LexicalEnvironmentObject::clone(cx, env);
    if (!fresh)
        return false;

    replaceInnermostEnvironment(*fresh);
    return true;
}

bool
InterpreterFrame::recreateLexicalEnvironment(JSContext* cx)
{
    Rooted<LexicalEnvironmentObject*> env(cx, &envChain_->as<LexicalEnvironmentObject>());
    LexicalEnvironmentObject* fresh = LexicalEnvironmentObject::recreate(cx, env);
    if (!fresh)
        return false;

    replaceInnermostEnvironment(*fresh);
    return true;
}

void
InterpreterFrame::trace(JSTracer* trc, Value* sp, jsbytecode* pc)
{
    TraceRoot(trc, &envChain_, "env chain");
    TraceRoot(trc, &script_, "script");

    if (flags_ & HAS_ARGS_OBJ)
        TraceRoot(trc, &argsObj_, "arguments");

    if (hasReturnValue())
        TraceRoot(trc, &rval_, "rval");

    MOZ_ASSERT(sp >= slots());

    if (hasArgs()) {
        // Trace the callee and |this|. When we're doing a moving GC, we
        // need to fix up the callee pointer before we use it below, under
        // numFormalArgs() and script().
        TraceRootRange(trc, 2, argv_ - 2, "fp callee and this");

        // Trace arguments.
        unsigned argc = Max(numActualArgs(), numFormalArgs());
        TraceRootRange(trc, argc + isConstructing(), argv_, "fp argv");
    } else {
        // Trace newTarget.
        TraceRoot(trc, ((Value*)this) - 1, "stack newTarget");
    }

    JSScript* script = this->script();
    size_t nfixed = script->nfixed();
    size_t nlivefixed = script->calculateLiveFixed(pc);

    if (nfixed == nlivefixed) {
        // All locals are live.
        traceValues(trc, 0, sp - slots());
    } else {
        // Trace operand stack.
        traceValues(trc, nfixed, sp - slots());

        // Clear dead block-scoped locals.
        while (nfixed > nlivefixed)
            unaliasedLocal(--nfixed).setUndefined();

        // Trace live locals.
        traceValues(trc, 0, nlivefixed);
    }

    if (script->compartment()->debugEnvs)
        script->compartment()->debugEnvs->traceLiveFrame(trc, this);
}

void
InterpreterFrame::traceValues(JSTracer* trc, unsigned start, unsigned end)
{
    if (start < end)
        TraceRootRange(trc, end - start, slots() + start, "vm_stack");
}

static void
TraceInterpreterActivation(JSTracer* trc, InterpreterActivation* act)
{
    for (InterpreterFrameIterator frames(act); !frames.done(); ++frames) {
        InterpreterFrame* fp = frames.frame();
        fp->trace(trc, frames.sp(), frames.pc());
    }
}

void
js::TraceInterpreterActivations(JSContext* cx, const CooperatingContext& target, JSTracer* trc)
{
    for (ActivationIterator iter(cx, target); !iter.done(); ++iter) {
        Activation* act = iter.activation();
        if (act->isInterpreter())
            TraceInterpreterActivation(trc, act->asInterpreter());
    }
}

/*****************************************************************************/

// Unlike the other methods of this class, this method is defined here so that
// we don't have to #include jsautooplen.h in vm/Stack.h.
void
InterpreterRegs::setToEndOfScript()
{
    sp = fp()->base();
}

/*****************************************************************************/

InterpreterFrame*
InterpreterStack::pushInvokeFrame(JSContext* cx, const CallArgs& args, MaybeConstruct constructing)
{
    LifoAlloc::Mark mark = allocator_.mark();

    RootedFunction fun(cx, &args.callee().as<JSFunction>());
    RootedScript script(cx, fun->nonLazyScript());

    Value* argv;
    InterpreterFrame* fp = getCallFrame(cx, args, script, constructing, &argv);
    if (!fp)
        return nullptr;

    fp->mark_ = mark;
    fp->initCallFrame(nullptr, nullptr, nullptr, *fun, script, argv, args.length(),
                      constructing);
    return fp;
}

InterpreterFrame*
InterpreterStack::pushExecuteFrame(JSContext* cx, HandleScript script, const Value& newTargetValue,
                                   HandleObject envChain, AbstractFramePtr evalInFrame)
{
    LifoAlloc::Mark mark = allocator_.mark();

    unsigned nvars = 1 /* newTarget */ + script->nslots();
    uint8_t* buffer = allocateFrame(cx, sizeof(InterpreterFrame) + nvars * sizeof(Value));
    if (!buffer)
        return nullptr;

    InterpreterFrame* fp = reinterpret_cast<InterpreterFrame*>(buffer + 1 * sizeof(Value));
    fp->mark_ = mark;
    fp->initExecuteFrame(cx, script, evalInFrame, newTargetValue, envChain);
    fp->initLocals();

    return fp;
}

/*****************************************************************************/

JitFrameIter::JitFrameIter(const JitFrameIter& another)
{
    *this = another;
}

JitFrameIter&
JitFrameIter::operator=(const JitFrameIter& another)
{
    MOZ_ASSERT(this != &another);

    act_ = another.act_;
    mustUnwindActivation_ = another.mustUnwindActivation_;

    if (isSome())
        iter_.destroy();
    if (!another.isSome())
        return *this;

    if (another.isJSJit()) {
        iter_.construct<jit::JSJitFrameIter>(another.asJSJit());
    } else {
        MOZ_ASSERT(another.isWasm());
        iter_.construct<wasm::WasmFrameIter>(another.asWasm());
    }

    return *this;
}

JitFrameIter::JitFrameIter(jit::JitActivation* act, bool mustUnwindActivation)
{
    act_ = act;
    mustUnwindActivation_ = mustUnwindActivation;
    MOZ_ASSERT(act->hasExitFP(), "packedExitFP is used to determine if JSJit or wasm");
    if (act->hasJSExitFP()) {
        iter_.construct<jit::JSJitFrameIter>(act);
    } else {
        MOZ_ASSERT(act->hasWasmExitFP());
        iter_.construct<wasm::WasmFrameIter>(act);
    }
    settle();
}

void
JitFrameIter::skipNonScriptedJSFrames()
{
    if (isJSJit()) {
        // Stop at the first scripted frame.
        jit::JSJitFrameIter& frames = asJSJit();
        while (!frames.isScripted() && !frames.done())
            ++frames;
        settle();
    }
}

bool
JitFrameIter::done() const
{
    if (!isSome())
        return true;
    if (isJSJit())
        return asJSJit().done();
    if (isWasm())
        return asWasm().done();
    MOZ_CRASH("unhandled case");
}

void
JitFrameIter::settle()
{
    if (isJSJit()) {
        const jit::JSJitFrameIter& jitFrame = asJSJit();
        if (jitFrame.type() != jit::JitFrame_WasmToJSJit)
            return;

        // Transition from js jit frames to wasm frames: we're on the
        // wasm-to-jit fast path. The current stack layout is as follows:
        // (stack grows downward)
        //
        // [--------------------]
        // [WASM FUNC           ]
        // [WASM JIT EXIT FRAME ]
        // [JIT WASM ENTRY FRAME] <-- we're here.
        //
        // So prevFP points to the wasm jit exit FP, maintaing the invariant in
        // WasmFrameIter that the first frame is an exit frame and can be
        // popped.

        wasm::Frame* prevFP = (wasm::Frame*) jitFrame.prevFp();

        if (mustUnwindActivation_)
            act_->setWasmExitFP(prevFP);

        iter_.destroy();
        iter_.construct<wasm::WasmFrameIter>(act_, prevFP);
        MOZ_ASSERT(!asWasm().done());
        return;
    }

    if (isWasm()) {
        const wasm::WasmFrameIter& wasmFrame = asWasm();
        if (!wasmFrame.unwoundIonCallerFP())
            return;

        // Transition from wasm frames to jit frames: we're on the
        // jit-to-wasm fast path. The current stack layout is as follows:
        // (stack grows downward)
        //
        // [--------------------]
        // [JIT FRAME           ]
        // [WASM JIT ENTRY FRAME] <-- we're here
        //
        // The wasm iterator has saved the previous jit frame pointer for us.

        MOZ_ASSERT(wasmFrame.done());
        uint8_t* prevFP = wasmFrame.unwoundIonCallerFP();

        if (mustUnwindActivation_)
            act_->setJSExitFP(prevFP);

        iter_.destroy();
        iter_.construct<jit::JSJitFrameIter>(act_, prevFP);
        MOZ_ASSERT(!asJSJit().done());
        return;
    }
}

void
JitFrameIter::operator++()
{
    MOZ_ASSERT(isSome());
    if (isJSJit()) {
        const jit::JSJitFrameIter& jitFrame = asJSJit();

        jit::JitFrameLayout* prevFrame = nullptr;
        if (mustUnwindActivation_ && jitFrame.isScripted())
            prevFrame = jitFrame.jsFrame();

        ++asJSJit();

        if (prevFrame) {
            // Unwind the frame by updating packedExitFP. This is necessary
            // so that (1) debugger exception unwind and leave frame hooks
            // don't see this frame when they use ScriptFrameIter, and (2)
            // ScriptFrameIter does not crash when accessing an IonScript
            // that's destroyed by the ionScript->decref call.
            EnsureBareExitFrame(act_, prevFrame);
        }
    } else if (isWasm()) {
        ++asWasm();
    } else {
        MOZ_CRASH("unhandled case");
    }
    settle();
}

OnlyJSJitFrameIter::OnlyJSJitFrameIter(jit::JitActivation* act)
  : JitFrameIter(act)
{
    settle();
}

OnlyJSJitFrameIter::OnlyJSJitFrameIter(JSContext* cx)
  : OnlyJSJitFrameIter(cx->activation()->asJit())
{
}

OnlyJSJitFrameIter::OnlyJSJitFrameIter(const ActivationIterator& iter)
  : OnlyJSJitFrameIter(iter->asJit())
{
}

/*****************************************************************************/

void
FrameIter::popActivation()
{
    ++data_.activations_;
    settleOnActivation();
}

void
FrameIter::popInterpreterFrame()
{
    MOZ_ASSERT(data_.state_ == INTERP);

    ++data_.interpFrames_;

    if (data_.interpFrames_.done())
        popActivation();
    else
        data_.pc_ = data_.interpFrames_.pc();
}

void
FrameIter::settleOnActivation()
{
    MOZ_ASSERT(!data_.cx_->inUnsafeCallWithABI);

    while (true) {
        if (data_.activations_.done()) {
            data_.state_ = DONE;
            return;
        }

        Activation* activation = data_.activations_.activation();

        // If the caller supplied principals, only show activations which are subsumed (of the same
        // origin or of an origin accessible) by these principals.
        if (data_.principals_) {
            JSContext* cx = data_.cx_;
            if (JSSubsumesOp subsumes = cx->runtime()->securityCallbacks->subsumes) {
                if (!subsumes(data_.principals_, activation->compartment()->principals())) {
                    ++data_.activations_;
                    continue;
                }
            }
        }

        if (activation->isJit()) {
            data_.jitFrames_ = JitFrameIter(activation->asJit());
            data_.jitFrames_.skipNonScriptedJSFrames();
            if (data_.jitFrames_.done()) {
                // It's possible to have an JitActivation with no scripted
                // frames, for instance if we hit an over-recursion during
                // bailout.
                ++data_.activations_;
                continue;
            }
            data_.state_ = JIT;
            nextJitFrame();
            return;
        }

        MOZ_ASSERT(activation->isInterpreter());

        InterpreterActivation* interpAct = activation->asInterpreter();
        data_.interpFrames_ = InterpreterFrameIterator(interpAct);

        // If we OSR'ed into JIT code, skip the interpreter frame so that
        // the same frame is not reported twice.
        if (data_.interpFrames_.frame()->runningInJit()) {
            ++data_.interpFrames_;
            if (data_.interpFrames_.done()) {
                ++data_.activations_;
                continue;
            }
        }

        MOZ_ASSERT(!data_.interpFrames_.frame()->runningInJit());
        data_.pc_ = data_.interpFrames_.pc();
        data_.state_ = INTERP;
        return;
    }
}

FrameIter::Data::Data(JSContext* cx, DebuggerEvalOption debuggerEvalOption,
                      JSPrincipals* principals)
  : cx_(cx),
    debuggerEvalOption_(debuggerEvalOption),
    principals_(principals),
    state_(DONE),
    pc_(nullptr),
    interpFrames_(nullptr),
    activations_(cx),
    ionInlineFrameNo_(0)
{
}

FrameIter::Data::Data(JSContext* cx, const CooperatingContext& target,
                      DebuggerEvalOption debuggerEvalOption)
  : cx_(cx),
    debuggerEvalOption_(debuggerEvalOption),
    principals_(nullptr),
    state_(DONE),
    pc_(nullptr),
    interpFrames_(nullptr),
    activations_(cx, target),
    ionInlineFrameNo_(0)
{
}

FrameIter::Data::Data(const FrameIter::Data& other)
  : cx_(other.cx_),
    debuggerEvalOption_(other.debuggerEvalOption_),
    principals_(other.principals_),
    state_(other.state_),
    pc_(other.pc_),
    interpFrames_(other.interpFrames_),
    activations_(other.activations_),
    jitFrames_(other.jitFrames_),
    ionInlineFrameNo_(other.ionInlineFrameNo_)
{
}

FrameIter::FrameIter(JSContext* cx, const CooperatingContext& target,
                     DebuggerEvalOption debuggerEvalOption)
  : data_(cx, target, debuggerEvalOption),
    ionInlineFrames_(cx, (js::jit::JSJitFrameIter*) nullptr)
{
    // settleOnActivation can only GC if principals are given.
    JS::AutoSuppressGCAnalysis nogc;
    settleOnActivation();
}

FrameIter::FrameIter(JSContext* cx, DebuggerEvalOption debuggerEvalOption)
  : data_(cx, debuggerEvalOption, nullptr),
    ionInlineFrames_(cx, (js::jit::JSJitFrameIter*) nullptr)
{
    // settleOnActivation can only GC if principals are given.
    JS::AutoSuppressGCAnalysis nogc;
    settleOnActivation();
}

FrameIter::FrameIter(JSContext* cx, DebuggerEvalOption debuggerEvalOption,
                     JSPrincipals* principals)
  : data_(cx, debuggerEvalOption, principals),
    ionInlineFrames_(cx, (js::jit::JSJitFrameIter*) nullptr)
{
    settleOnActivation();
}

FrameIter::FrameIter(const FrameIter& other)
  : data_(other.data_),
    ionInlineFrames_(other.data_.cx_, isIonScripted() ? &other.ionInlineFrames_ : nullptr)
{
}

FrameIter::FrameIter(const Data& data)
  : data_(data),
    ionInlineFrames_(data.cx_, isIonScripted() ? &jsJitFrame() : nullptr)
{
    MOZ_ASSERT(data.cx_);
    if (isIonScripted()) {
        while (ionInlineFrames_.frameNo() != data.ionInlineFrameNo_)
            ++ionInlineFrames_;
    }
}

void
FrameIter::nextJitFrame()
{
    MOZ_ASSERT(data_.jitFrames_.isSome());

    if (isJSJit()) {
        if (jsJitFrame().isIonScripted()) {
            ionInlineFrames_.resetOn(&jsJitFrame());
            data_.pc_ = ionInlineFrames_.pc();
        } else {
            MOZ_ASSERT(jsJitFrame().isBaselineJS());
            jsJitFrame().baselineScriptAndPc(nullptr, &data_.pc_);
        }
        return;
    }

    MOZ_ASSERT(isWasm());
    data_.pc_ = nullptr;
}

void
FrameIter::popJitFrame()
{
    MOZ_ASSERT(data_.state_ == JIT);
    MOZ_ASSERT(data_.jitFrames_.isSome());

    if (isJSJit() && jsJitFrame().isIonScripted() && ionInlineFrames_.more()) {
        ++ionInlineFrames_;
        data_.pc_ = ionInlineFrames_.pc();
        return;
    }

    ++data_.jitFrames_;
    data_.jitFrames_.skipNonScriptedJSFrames();

    if (!data_.jitFrames_.done()) {
        nextJitFrame();
    } else {
        data_.jitFrames_.reset();
        popActivation();
    }
}

FrameIter&
FrameIter::operator++()
{
    switch (data_.state_) {
      case DONE:
        MOZ_CRASH("Unexpected state");
      case INTERP:
        if (interpFrame()->isDebuggerEvalFrame() &&
            data_.debuggerEvalOption_ == FOLLOW_DEBUGGER_EVAL_PREV_LINK)
        {
            AbstractFramePtr eifPrev = interpFrame()->evalInFramePrev();

            popInterpreterFrame();

            while (!hasUsableAbstractFramePtr() || abstractFramePtr() != eifPrev) {
                if (data_.state_ == JIT)
                    popJitFrame();
                else
                    popInterpreterFrame();
            }

            break;
        }
        popInterpreterFrame();
        break;
      case JIT:
        popJitFrame();
        break;
    }
    return *this;
}

FrameIter::Data*
FrameIter::copyData() const
{
    Data* data = data_.cx_->new_<Data>(data_);
    if (!data)
        return nullptr;

    if (data && isIonScripted())
        data->ionInlineFrameNo_ = ionInlineFrames_.frameNo();
    return data;
}

AbstractFramePtr
FrameIter::copyDataAsAbstractFramePtr() const
{
    AbstractFramePtr frame;
    if (Data* data = copyData())
        frame.ptr_ = uintptr_t(data);
    return frame;
}

void*
FrameIter::rawFramePtr() const
{
    switch (data_.state_) {
      case DONE:
        return nullptr;
      case INTERP:
        return interpFrame();
      case JIT:
        if (isJSJit())
            return jsJitFrame().fp();
        MOZ_ASSERT(isWasm());
        return nullptr;
    }
    MOZ_CRASH("Unexpected state");
}

JSCompartment*
FrameIter::compartment() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        return data_.activations_->compartment();
    }
    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::isEvalFrame() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        return interpFrame()->isEvalFrame();
      case JIT:
        if (isJSJit()) {
            if (jsJitFrame().isBaselineJS())
                return jsJitFrame().baselineFrame()->isEvalFrame();
            MOZ_ASSERT(!script()->isForEval());
            return false;
        }
        MOZ_ASSERT(isWasm());
        return false;
    }
    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::isFunctionFrame() const
{
    MOZ_ASSERT(!done());
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        return interpFrame()->isFunctionFrame();
      case JIT:
        if (isJSJit()) {
            if (jsJitFrame().isBaselineJS())
                return jsJitFrame().baselineFrame()->isFunctionFrame();
            return script()->functionNonDelazifying();
        }
        MOZ_ASSERT(isWasm());
        return false;
    }
    MOZ_CRASH("Unexpected state");
}

JSAtom*
FrameIter::functionDisplayAtom() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        if (isWasm())
            return wasmFrame().functionDisplayAtom();
        MOZ_ASSERT(isFunctionFrame());
        return calleeTemplate()->displayAtom();
    }

    MOZ_CRASH("Unexpected state");
}

ScriptSource*
FrameIter::scriptSource() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        return script()->scriptSource();
    }

    MOZ_CRASH("Unexpected state");
}

const char*
FrameIter::filename() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        if (isWasm())
            return wasmFrame().filename();
        return script()->filename();
    }

    MOZ_CRASH("Unexpected state");
}

const char16_t*
FrameIter::displayURL() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        if (isWasm())
            return wasmFrame().displayURL();
        ScriptSource* ss = script()->scriptSource();
        return ss->hasDisplayURL() ? ss->displayURL() : nullptr;
    }
    MOZ_CRASH("Unexpected state");
}

unsigned
FrameIter::computeLine(uint32_t* column) const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        if (isWasm()) {
            if (column)
                *column = 0;
            return wasmFrame().lineOrBytecode();
        }
        return PCToLineNumber(script(), pc(), column);
    }

    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::mutedErrors() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        if (isWasm())
            return wasmFrame().mutedErrors();
        return script()->mutedErrors();
    }
    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::isConstructing() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case JIT:
        MOZ_ASSERT(isJSJit());
        if (jsJitFrame().isIonScripted())
            return ionInlineFrames_.isConstructing();
        MOZ_ASSERT(jsJitFrame().isBaselineJS());
        return jsJitFrame().isConstructing();
      case INTERP:
        return interpFrame()->isConstructing();
    }

    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::ensureHasRematerializedFrame(JSContext* cx)
{
    MOZ_ASSERT(isIon());
    return !!activation()->asJit()->getRematerializedFrame(cx, jsJitFrame());
}

bool
FrameIter::hasUsableAbstractFramePtr() const
{
    switch (data_.state_) {
      case DONE:
        return false;
      case JIT:
        if (isJSJit()) {
            if (jsJitFrame().isBaselineJS())
                return true;

            MOZ_ASSERT(jsJitFrame().isIonScripted());
            return !!activation()->asJit()->lookupRematerializedFrame(jsJitFrame().fp(),
                                                                      ionInlineFrames_.frameNo());
        }
        MOZ_ASSERT(isWasm());
        return wasmFrame().debugEnabled();
      case INTERP:
        return true;
    }
    MOZ_CRASH("Unexpected state");
}

AbstractFramePtr
FrameIter::abstractFramePtr() const
{
    MOZ_ASSERT(hasUsableAbstractFramePtr());
    switch (data_.state_) {
      case DONE:
        break;
      case JIT: {
        if (isJSJit()) {
            if (jsJitFrame().isBaselineJS())
                return jsJitFrame().baselineFrame();
            MOZ_ASSERT(isIonScripted());
            return activation()->asJit()->lookupRematerializedFrame(jsJitFrame().fp(),
                                                                    ionInlineFrames_.frameNo());
        }
        MOZ_ASSERT(isWasm());
        MOZ_ASSERT(wasmFrame().debugEnabled());
        return wasmFrame().debugFrame();
      }
      case INTERP:
        MOZ_ASSERT(interpFrame());
        return AbstractFramePtr(interpFrame());
    }
    MOZ_CRASH("Unexpected state");
}

void
FrameIter::updatePcQuadratic()
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP: {
        InterpreterFrame* frame = interpFrame();
        InterpreterActivation* activation = data_.activations_->asInterpreter();

        // Look for the current frame.
        data_.interpFrames_ = InterpreterFrameIterator(activation);
        while (data_.interpFrames_.frame() != frame)
            ++data_.interpFrames_;

        // Update the pc.
        MOZ_ASSERT(data_.interpFrames_.frame() == frame);
        data_.pc_ = data_.interpFrames_.pc();
        return;
      }
      case JIT:
        if (jsJitFrame().isBaselineJS()) {
            jit::BaselineFrame* frame = jsJitFrame().baselineFrame();
            jit::JitActivation* activation = data_.activations_->asJit();

            // activation's exitFP may be invalid, so create a new
            // activation iterator.
            data_.activations_ = ActivationIterator(data_.cx_);
            while (data_.activations_.activation() != activation)
                ++data_.activations_;

            // Look for the current frame.
            data_.jitFrames_ = JitFrameIter(data_.activations_->asJit());
            while (!jsJitFrame().isBaselineJS() || jsJitFrame().baselineFrame() != frame)
                ++data_.jitFrames_;

            // Update the pc.
            MOZ_ASSERT(jsJitFrame().baselineFrame() == frame);
            jsJitFrame().baselineScriptAndPc(nullptr, &data_.pc_);
            return;
        }
        break;
    }
    MOZ_CRASH("Unexpected state");
}

void
FrameIter::wasmUpdateBytecodeOffset()
{
    MOZ_RELEASE_ASSERT(isWasm(), "Unexpected state");

    wasm::DebugFrame* frame = wasmFrame().debugFrame();

    // Relookup the current frame, updating the bytecode offset in the process.
    data_.jitFrames_ = JitFrameIter(data_.activations_->asJit());
    while (wasmFrame().debugFrame() != frame)
        ++data_.jitFrames_;

    MOZ_ASSERT(wasmFrame().debugFrame() == frame);
}

JSFunction*
FrameIter::calleeTemplate() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        MOZ_ASSERT(isFunctionFrame());
        return &interpFrame()->callee();
      case JIT:
        if (jsJitFrame().isBaselineJS())
            return jsJitFrame().callee();
        MOZ_ASSERT(jsJitFrame().isIonScripted());
        return ionInlineFrames_.calleeTemplate();
    }
    MOZ_CRASH("Unexpected state");
}

JSFunction*
FrameIter::callee(JSContext* cx) const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        return calleeTemplate();
      case JIT:
        if (isIonScripted()) {
            jit::MaybeReadFallback recover(cx, activation()->asJit(), &jsJitFrame());
            return ionInlineFrames_.callee(recover);
        }
        MOZ_ASSERT(jsJitFrame().isBaselineJS());
        return calleeTemplate();
    }
    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::matchCallee(JSContext* cx, HandleFunction fun) const
{
    RootedFunction currentCallee(cx, calleeTemplate());

    // As we do not know if the calleeTemplate is the real function, or the
    // template from which it would be cloned, we compare properties which are
    // stable across the cloning of JSFunctions.
    if (((currentCallee->flags() ^ fun->flags()) & JSFunction::STABLE_ACROSS_CLONES) != 0 ||
        currentCallee->nargs() != fun->nargs())
    {
        return false;
    }

    // Use the same condition as |js::CloneFunctionObject|, to know if we should
    // expect both functions to have the same JSScript. If so, and if they are
    // different, then they cannot be equal.
    RootedObject global(cx, &fun->global());
    bool useSameScript = CanReuseScriptForClone(fun->compartment(), currentCallee, global);
    if (useSameScript &&
        (currentCallee->hasScript() != fun->hasScript() ||
         currentCallee->nonLazyScript() != fun->nonLazyScript()))
    {
        return false;
    }

    // If none of the previous filters worked, then take the risk of
    // invalidating the frame to identify the JSFunction.
    return callee(cx) == fun;
}

unsigned
FrameIter::numActualArgs() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        MOZ_ASSERT(isFunctionFrame());
        return interpFrame()->numActualArgs();
      case JIT:
        if (isIonScripted())
            return ionInlineFrames_.numActualArgs();
        MOZ_ASSERT(jsJitFrame().isBaselineJS());
        return jsJitFrame().numActualArgs();
    }
    MOZ_CRASH("Unexpected state");
}

unsigned
FrameIter::numFormalArgs() const
{
    return script()->functionNonDelazifying()->nargs();
}

Value
FrameIter::unaliasedActual(unsigned i, MaybeCheckAliasing checkAliasing) const
{
    return abstractFramePtr().unaliasedActual(i, checkAliasing);
}

JSObject*
FrameIter::environmentChain(JSContext* cx) const
{
    switch (data_.state_) {
      case DONE:
        break;
      case JIT:
        if (isJSJit()) {
            if (isIonScripted()) {
                jit::MaybeReadFallback recover(cx, activation()->asJit(), &jsJitFrame());
                return ionInlineFrames_.environmentChain(recover);
            }
            return jsJitFrame().baselineFrame()->environmentChain();
        }
        MOZ_ASSERT(isWasm());
        return wasmFrame().debugFrame()->environmentChain();
      case INTERP:
        return interpFrame()->environmentChain();
    }
    MOZ_CRASH("Unexpected state");
}

CallObject&
FrameIter::callObj(JSContext* cx) const
{
    MOZ_ASSERT(calleeTemplate()->needsCallObject());

    JSObject* pobj = environmentChain(cx);
    while (!pobj->is<CallObject>())
        pobj = pobj->enclosingEnvironment();
    return pobj->as<CallObject>();
}

bool
FrameIter::hasArgsObj() const
{
    return abstractFramePtr().hasArgsObj();
}

ArgumentsObject&
FrameIter::argsObj() const
{
    MOZ_ASSERT(hasArgsObj());
    return abstractFramePtr().argsObj();
}

Value
FrameIter::thisArgument(JSContext* cx) const
{
    MOZ_ASSERT(isFunctionFrame());

    switch (data_.state_) {
      case DONE:
        break;
      case JIT:
        if (isIonScripted()) {
            jit::MaybeReadFallback recover(cx, activation()->asJit(), &jsJitFrame());
            return ionInlineFrames_.thisArgument(recover);
        }
        return jsJitFrame().baselineFrame()->thisArgument();
      case INTERP:
        return interpFrame()->thisArgument();
    }
    MOZ_CRASH("Unexpected state");
}

Value
FrameIter::newTarget() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        return interpFrame()->newTarget();
      case JIT:
        MOZ_ASSERT(jsJitFrame().isBaselineJS());
        return jsJitFrame().baselineFrame()->newTarget();
    }
    MOZ_CRASH("Unexpected state");
}

Value
FrameIter::returnValue() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case JIT:
        if (jsJitFrame().isBaselineJS())
            return jsJitFrame().baselineFrame()->returnValue();
        break;
      case INTERP:
        return interpFrame()->returnValue();
    }
    MOZ_CRASH("Unexpected state");
}

void
FrameIter::setReturnValue(const Value& v)
{
    switch (data_.state_) {
      case DONE:
        break;
      case JIT:
        if (jsJitFrame().isBaselineJS()) {
            jsJitFrame().baselineFrame()->setReturnValue(v);
            return;
        }
        break;
      case INTERP:
        interpFrame()->setReturnValue(v);
        return;
    }
    MOZ_CRASH("Unexpected state");
}

size_t
FrameIter::numFrameSlots() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case JIT: {
        if (isIonScripted()) {
            return ionInlineFrames_.snapshotIterator().numAllocations() -
                   ionInlineFrames_.script()->nfixed();
        }
        jit::BaselineFrame* frame = jsJitFrame().baselineFrame();
        return frame->numValueSlots() - jsJitFrame().script()->nfixed();
      }
      case INTERP:
        MOZ_ASSERT(data_.interpFrames_.sp() >= interpFrame()->base());
        return data_.interpFrames_.sp() - interpFrame()->base();
    }
    MOZ_CRASH("Unexpected state");
}

Value
FrameIter::frameSlotValue(size_t index) const
{
    switch (data_.state_) {
      case DONE:
        break;
      case JIT:
        if (isIonScripted()) {
            jit::SnapshotIterator si(ionInlineFrames_.snapshotIterator());
            index += ionInlineFrames_.script()->nfixed();
            return si.maybeReadAllocByIndex(index);
        }
        index += jsJitFrame().script()->nfixed();
        return *jsJitFrame().baselineFrame()->valueSlot(index);
      case INTERP:
          return interpFrame()->base()[index];
    }
    MOZ_CRASH("Unexpected state");
}

#ifdef DEBUG
bool
js::SelfHostedFramesVisible()
{
    static bool checked = false;
    static bool visible = false;
    if (!checked) {
        checked = true;
        char* env = getenv("MOZ_SHOW_ALL_JS_FRAMES");
        visible = !!env;
    }
    return visible;
}
#endif

void
NonBuiltinFrameIter::settle()
{
    if (!SelfHostedFramesVisible()) {
        while (!done() && hasScript() && script()->selfHosted())
            FrameIter::operator++();
    }
}

void
NonBuiltinScriptFrameIter::settle()
{
    if (!SelfHostedFramesVisible()) {
        while (!done() && script()->selfHosted())
            ScriptFrameIter::operator++();
    }
}

ActivationEntryMonitor::ActivationEntryMonitor(JSContext* cx)
  : cx_(cx), entryMonitor_(cx->entryMonitor)
{
    cx->entryMonitor = nullptr;
}

Value
ActivationEntryMonitor::asyncStack(JSContext* cx)
{
    RootedValue stack(cx, ObjectOrNullValue(cx->asyncStackForNewActivations()));
    if (!cx->compartment()->wrap(cx, &stack)) {
        cx->clearPendingException();
        return UndefinedValue();
    }
    return stack;
}

ActivationEntryMonitor::ActivationEntryMonitor(JSContext* cx, InterpreterFrame* entryFrame)
  : ActivationEntryMonitor(cx)
{
    if (entryMonitor_) {
        // The InterpreterFrame is not yet part of an Activation, so it won't
        // be traced if we trigger GC here. Suppress GC to avoid this.
        gc::AutoSuppressGC suppressGC(cx);
        RootedValue stack(cx, asyncStack(cx));
        const char* asyncCause = cx->asyncCauseForNewActivations;
        if (entryFrame->isFunctionFrame())
            entryMonitor_->Entry(cx, &entryFrame->callee(), stack, asyncCause);
        else
            entryMonitor_->Entry(cx, entryFrame->script(), stack, asyncCause);
    }
}

ActivationEntryMonitor::ActivationEntryMonitor(JSContext* cx, jit::CalleeToken entryToken)
  : ActivationEntryMonitor(cx)
{
    if (entryMonitor_) {
        // The CalleeToken is not traced at this point and we also don't want
        // a GC to discard the code we're about to enter, so we suppress GC.
        gc::AutoSuppressGC suppressGC(cx);
        RootedValue stack(cx, asyncStack(cx));
        const char* asyncCause = cx->asyncCauseForNewActivations;
        if (jit::CalleeTokenIsFunction(entryToken))
            entryMonitor_->Entry(cx_, jit::CalleeTokenToFunction(entryToken), stack, asyncCause);
        else
            entryMonitor_->Entry(cx_, jit::CalleeTokenToScript(entryToken), stack, asyncCause);
    }
}

/*****************************************************************************/

jit::JitActivation::JitActivation(JSContext* cx)
  : Activation(cx, Jit),
    packedExitFP_(nullptr),
    encodedWasmExitReason_(0),
    prevJitActivation_(cx->jitActivation),
    rematerializedFrames_(nullptr),
    ionRecovery_(cx),
    bailoutData_(nullptr),
    lastProfilingFrame_(nullptr),
    lastProfilingCallSite_(nullptr)
{
    cx->jitActivation = this;
    registerProfiling();
}

jit::JitActivation::~JitActivation()
{
    if (isProfiling())
        unregisterProfiling();
    cx_->jitActivation = prevJitActivation_;

    // All reocvered value are taken from activation during the bailout.
    MOZ_ASSERT(ionRecovery_.empty());

    // The BailoutFrameInfo should have unregistered itself from the
    // JitActivations.
    MOZ_ASSERT(!bailoutData_);

    MOZ_ASSERT(!isWasmInterrupted());
    MOZ_ASSERT(!isWasmTrapping());

    clearRematerializedFrames();
    js_delete(rematerializedFrames_);
}

void
jit::JitActivation::setBailoutData(jit::BailoutFrameInfo* bailoutData)
{
    MOZ_ASSERT(!bailoutData_);
    bailoutData_ = bailoutData;
}

void
jit::JitActivation::cleanBailoutData()
{
    MOZ_ASSERT(bailoutData_);
    bailoutData_ = nullptr;
}

void
jit::JitActivation::removeRematerializedFrame(uint8_t* top)
{
    if (!rematerializedFrames_)
        return;

    if (RematerializedFrameTable::Ptr p = rematerializedFrames_->lookup(top)) {
        RematerializedFrame::FreeInVector(p->value());
        rematerializedFrames_->remove(p);
    }
}

void
jit::JitActivation::clearRematerializedFrames()
{
    if (!rematerializedFrames_)
        return;

    for (RematerializedFrameTable::Enum e(*rematerializedFrames_); !e.empty(); e.popFront()) {
        RematerializedFrame::FreeInVector(e.front().value());
        e.removeFront();
    }
}

jit::RematerializedFrame*
jit::JitActivation::getRematerializedFrame(JSContext* cx, const JSJitFrameIter& iter,
                                           size_t inlineDepth)
{
    MOZ_ASSERT(iter.activation() == this);
    MOZ_ASSERT(iter.isIonScripted());

    if (!rematerializedFrames_) {
        rematerializedFrames_ = cx->new_<RematerializedFrameTable>(cx);
        if (!rematerializedFrames_)
            return nullptr;
        if (!rematerializedFrames_->init()) {
            rematerializedFrames_ = nullptr;
            ReportOutOfMemory(cx);
            return nullptr;
        }
    }

    uint8_t* top = iter.fp();
    RematerializedFrameTable::AddPtr p = rematerializedFrames_->lookupForAdd(top);
    if (!p) {
        RematerializedFrameVector frames(cx);

        // The unit of rematerialization is an uninlined frame and its inlined
        // frames. Since inlined frames do not exist outside of snapshots, it
        // is impossible to synchronize their rematerialized copies to
        // preserve identity. Therefore, we always rematerialize an uninlined
        // frame and all its inlined frames at once.
        InlineFrameIterator inlineIter(cx, &iter);
        MaybeReadFallback recover(cx, this, &iter);

        // Frames are often rematerialized with the cx inside a Debugger's
        // compartment. To recover slots and to create CallObjects, we need to
        // be in the activation's compartment.
        AutoCompartmentUnchecked ac(cx, compartment_);

        if (!RematerializedFrame::RematerializeInlineFrames(cx, top, inlineIter, recover, frames))
            return nullptr;

        if (!rematerializedFrames_->add(p, top, Move(frames))) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

        // See comment in unsetPrevUpToDateUntil.
        DebugEnvironments::unsetPrevUpToDateUntil(cx, p->value()[inlineDepth]);
    }

    return p->value()[inlineDepth];
}

jit::RematerializedFrame*
jit::JitActivation::lookupRematerializedFrame(uint8_t* top, size_t inlineDepth)
{
    if (!rematerializedFrames_)
        return nullptr;
    if (RematerializedFrameTable::Ptr p = rematerializedFrames_->lookup(top))
        return inlineDepth < p->value().length() ? p->value()[inlineDepth] : nullptr;
    return nullptr;
}

void
jit::JitActivation::removeRematerializedFramesFromDebugger(JSContext* cx, uint8_t* top)
{
    // Ion bailout can fail due to overrecursion and OOM. In such cases we
    // cannot honor any further Debugger hooks on the frame, and need to
    // ensure that its Debugger.Frame entry is cleaned up.
    if (!cx->compartment()->isDebuggee() || !rematerializedFrames_)
        return;
    if (RematerializedFrameTable::Ptr p = rematerializedFrames_->lookup(top)) {
        for (uint32_t i = 0; i < p->value().length(); i++)
            Debugger::handleUnrecoverableIonBailoutError(cx, p->value()[i]);
    }
}

void
jit::JitActivation::traceRematerializedFrames(JSTracer* trc)
{
    if (!rematerializedFrames_)
        return;
    for (RematerializedFrameTable::Enum e(*rematerializedFrames_); !e.empty(); e.popFront())
        e.front().value().trace(trc);
}

bool
jit::JitActivation::registerIonFrameRecovery(RInstructionResults&& results)
{
    // Check that there is no entry in the vector yet.
    MOZ_ASSERT(!maybeIonFrameRecovery(results.frame()));
    if (!ionRecovery_.append(mozilla::Move(results)))
        return false;

    return true;
}

jit::RInstructionResults*
jit::JitActivation::maybeIonFrameRecovery(JitFrameLayout* fp)
{
    for (RInstructionResults* it = ionRecovery_.begin(); it != ionRecovery_.end(); ) {
        if (it->frame() == fp)
            return it;
    }

    return nullptr;
}

void
jit::JitActivation::removeIonFrameRecovery(JitFrameLayout* fp)
{
    RInstructionResults* elem = maybeIonFrameRecovery(fp);
    if (!elem)
        return;

    ionRecovery_.erase(elem);
}

void
jit::JitActivation::traceIonRecovery(JSTracer* trc)
{
    for (RInstructionResults* it = ionRecovery_.begin(); it != ionRecovery_.end(); it++)
        it->trace(trc);
}

bool
jit::JitActivation::startWasmInterrupt(const JS::ProfilingFrameIterator::RegisterState& state)
{
    // fp may be null when first entering wasm code from an interpreter entry
    // stub.
    if (!state.fp)
        return false;

    MOZ_ASSERT(state.pc);

    // Execution can only be interrupted in function code. Afterwards, control
    // flow does not reenter function code and thus there can be no
    // interrupt-during-interrupt.

    bool unwound;
    wasm::UnwindState unwindState;
    MOZ_ALWAYS_TRUE(wasm::StartUnwinding(state, &unwindState, &unwound));

    void* pc = unwindState.pc;

    if (unwound) {
        // In the prologue/epilogue, FP might have been fixed up to the
        // caller's FP, and the caller could be the jit entry. Ignore this
        // interrupt, in this case, because FP points to a jit frame and not a
        // wasm one.
        if (!wasm::LookupCode(pc)->lookupFuncRange(pc))
            return false;
    }

    cx_->runtime()->wasmUnwindData.ref().construct<wasm::InterruptData>(pc, state.pc);
    setWasmExitFP(unwindState.fp);

    MOZ_ASSERT(compartment() == unwindState.fp->tls->instance->compartment());
    MOZ_ASSERT(isWasmInterrupted());
    return true;
}

void
jit::JitActivation::finishWasmInterrupt()
{
    MOZ_ASSERT(isWasmInterrupted());

    cx_->runtime()->wasmUnwindData.ref().destroy();
    packedExitFP_ = nullptr;
}

bool
jit::JitActivation::isWasmInterrupted() const
{
    JSRuntime* rt = cx_->runtime();
    if (!rt->wasmUnwindData.ref().constructed<wasm::InterruptData>())
        return false;

    Activation* act = cx_->activation();
    while (act && !act->hasWasmExitFP())
        act = act->prev();

    if (act != this)
        return false;

    DebugOnly<const wasm::Frame*> fp = wasmExitFP();
    DebugOnly<void*> unwindPC = rt->wasmInterruptData().unwindPC;
    MOZ_ASSERT(fp->instance()->code().containsCodePC(unwindPC));
    return true;
}

void*
jit::JitActivation::wasmInterruptUnwindPC() const
{
    MOZ_ASSERT(isWasmInterrupted());
    return cx_->runtime()->wasmInterruptData().unwindPC;
}

void*
jit::JitActivation::wasmInterruptResumePC() const
{
    MOZ_ASSERT(isWasmInterrupted());
    return cx_->runtime()->wasmInterruptData().resumePC;
}

void
jit::JitActivation::startWasmTrap(wasm::Trap trap, uint32_t bytecodeOffset,
                                  const wasm::RegisterState& state)
{
    bool unwound;
    wasm::UnwindState unwindState;
    MOZ_ALWAYS_TRUE(wasm::StartUnwinding(state, &unwindState, &unwound));
    MOZ_ASSERT(unwound == (trap == wasm::Trap::IndirectCallBadSig));

    void* pc = unwindState.pc;
    wasm::Frame* fp = unwindState.fp;

    const wasm::Code& code = fp->tls->instance->code();
    MOZ_RELEASE_ASSERT(&code == wasm::LookupCode(pc));

    // If the frame was unwound, the bytecodeOffset must be recovered from the
    // callsite so that it is accurate.
    if (unwound)
        bytecodeOffset = code.lookupCallSite(pc)->lineOrBytecode();

    cx_->runtime()->wasmUnwindData.ref().construct<wasm::TrapData>(pc, trap, bytecodeOffset);
    setWasmExitFP(fp);
}

void
jit::JitActivation::finishWasmTrap()
{
    MOZ_ASSERT(isWasmTrapping());

    cx_->runtime()->wasmUnwindData.ref().destroy();
    packedExitFP_ = nullptr;
}

bool
jit::JitActivation::isWasmTrapping() const
{
    JSRuntime* rt = cx_->runtime();
    if (!rt->wasmUnwindData.ref().constructed<wasm::TrapData>())
        return false;

    Activation* act = cx_->activation();
    while (act && !act->hasWasmExitFP())
        act = act->prev();

    if (act != this)
        return false;

    DebugOnly<const wasm::Frame*> fp = wasmExitFP();
    DebugOnly<void*> unwindPC = rt->wasmTrapData().pc;
    MOZ_ASSERT(fp->instance()->code().containsCodePC(unwindPC));
    return true;
}

void*
jit::JitActivation::wasmTrapPC() const
{
    MOZ_ASSERT(isWasmTrapping());
    return cx_->runtime()->wasmTrapData().pc;
}

uint32_t
jit::JitActivation::wasmTrapBytecodeOffset() const
{
    MOZ_ASSERT(isWasmTrapping());
    return cx_->runtime()->wasmTrapData().bytecodeOffset;
}

InterpreterFrameIterator&
InterpreterFrameIterator::operator++()
{
    MOZ_ASSERT(!done());
    if (fp_ != activation_->entryFrame_) {
        pc_ = fp_->prevpc();
        sp_ = fp_->prevsp();
        fp_ = fp_->prev();
    } else {
        pc_ = nullptr;
        sp_ = nullptr;
        fp_ = nullptr;
    }
    return *this;
}

void
Activation::registerProfiling()
{
    MOZ_ASSERT(isProfiling());
    cx_->profilingActivation_ = this;
}

void
Activation::unregisterProfiling()
{
    MOZ_ASSERT(isProfiling());
    MOZ_ASSERT(cx_->profilingActivation_ == this);
    cx_->profilingActivation_ = prevProfiling_;
}

ActivationIterator::ActivationIterator(JSContext* cx)
  : activation_(cx->activation_)
{
    MOZ_ASSERT(cx == TlsContext.get());
}

ActivationIterator::ActivationIterator(JSContext* cx, const CooperatingContext& target)
{
    MOZ_ASSERT(cx == TlsContext.get());

    // If target was specified --- even if it is the same as cx itself --- then
    // we must be in a scope where changes of the active context are prohibited.
    // Otherwise our state would be corrupted if the target thread resumed
    // execution while we are iterating over its state.
    MOZ_ASSERT(cx->runtime()->activeContextChangeProhibited() ||
               !cx->runtime()->gc.canChangeActiveContext(cx));

    // Tolerate a null target context, in case we are iterating over the
    // activations for a zone group that is not in use by any thread.
    activation_ = target.context() ? target.context()->activation_.ref() : nullptr;
}

ActivationIterator&
ActivationIterator::operator++()
{
    MOZ_ASSERT(activation_);
    activation_ = activation_->prev();
    return *this;
}

JS::ProfilingFrameIterator::ProfilingFrameIterator(JSContext* cx, const RegisterState& state,
                                                   const Maybe<uint64_t>& samplePositionInProfilerBuffer)
  : cx_(cx),
    samplePositionInProfilerBuffer_(samplePositionInProfilerBuffer),
    activation_(nullptr)
{
    if (!cx->runtime()->geckoProfiler().enabled())
        MOZ_CRASH("ProfilingFrameIterator called when geckoProfiler not enabled for runtime.");

    if (!cx->profilingActivation())
        return;

    // If profiler sampling is not enabled, skip.
    if (!cx->isProfilerSamplingEnabled())
        return;

    activation_ = cx->profilingActivation();

    MOZ_ASSERT(activation_->isProfiling());

    static_assert(sizeof(wasm::ProfilingFrameIterator) <= StorageSpace &&
                  sizeof(jit::JSJitProfilingFrameIterator) <= StorageSpace,
                  "ProfilingFrameIterator::storage_ is too small");
    static_assert(alignof(void*) >= alignof(wasm::ProfilingFrameIterator) &&
                  alignof(void*) >= alignof(jit::JSJitProfilingFrameIterator),
                  "ProfilingFrameIterator::storage_ is too weakly aligned");

    iteratorConstruct(state);
    settle();
}

JS::ProfilingFrameIterator::~ProfilingFrameIterator()
{
    if (!done()) {
        MOZ_ASSERT(activation_->isProfiling());
        iteratorDestroy();
    }
}

void
JS::ProfilingFrameIterator::operator++()
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isJit());
    if (isWasm())
        ++wasmIter();
    else
        ++jsJitIter();
    settle();
}

void
JS::ProfilingFrameIterator::settleFrames()
{
    // Handle transition frames (see comment in JitFrameIter::operator++).
    if (isJSJit() && !jsJitIter().done() && jsJitIter().frameType() == jit::JitFrame_WasmToJSJit) {
        wasm::Frame* fp = (wasm::Frame*) jsJitIter().fp();
        iteratorDestroy();
        new (storage()) wasm::ProfilingFrameIterator(*activation_->asJit(), fp);
        kind_ = Kind::Wasm;
        MOZ_ASSERT(!wasmIter().done());
        return;
    }

    if (isWasm() && wasmIter().done() && wasmIter().unwoundIonCallerFP()) {
        uint8_t* fp = wasmIter().unwoundIonCallerFP();
        iteratorDestroy();
        // Using this ctor will skip the first ion->wasm frame, which is
        // needed because the profiling iterator doesn't know how to unwind
        // when the callee has no script.
        new (storage()) jit::JSJitProfilingFrameIterator((jit::CommonFrameLayout*)fp);
        kind_ = Kind::JSJit;
        MOZ_ASSERT(!jsJitIter().done());
        return;
    }
}

void
JS::ProfilingFrameIterator::settle()
{
    settleFrames();
    while (iteratorDone()) {
        iteratorDestroy();
        activation_ = activation_->prevProfiling();
        if (!activation_)
            return;
        iteratorConstruct();
        settleFrames();
    }
}

void
JS::ProfilingFrameIterator::iteratorConstruct(const RegisterState& state)
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isJit());

    jit::JitActivation* activation = activation_->asJit();

    // We want to know if we should start with a wasm profiling frame iterator
    // or not. To determine this, there are three possibilities:
    // - we've exited to C++ from wasm, in which case the activation
    //   exitFP low bit is tagged and we can test hasWasmExitFP().
    // - we're in wasm code, so we can do a lookup on PC.
    // - in all the other cases, we're not in wasm or we haven't exited from
    //   wasm.
    if (activation->hasWasmExitFP() || wasm::InCompiledCode(state.pc)) {
        new (storage()) wasm::ProfilingFrameIterator(*activation, state);
        kind_ = Kind::Wasm;
        return;
    }

    new (storage()) jit::JSJitProfilingFrameIterator(cx_, state.pc);
    kind_ = Kind::JSJit;
}

void
JS::ProfilingFrameIterator::iteratorConstruct()
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isJit());

    jit::JitActivation* activation = activation_->asJit();

    // The same reasoning as in the above iteratorConstruct variant applies
    // here, except that it's even simpler: since this activation is higher up
    // on the stack, it can only have exited to C++, through wasm or ion.
    if (activation->hasWasmExitFP()) {
        new (storage()) wasm::ProfilingFrameIterator(*activation);
        kind_ = Kind::Wasm;
        return;
    }

    auto* fp = (jit::ExitFrameLayout*) activation->jsExitFP();
    new (storage()) jit::JSJitProfilingFrameIterator(fp);
    kind_ = Kind::JSJit;
}

void
JS::ProfilingFrameIterator::iteratorDestroy()
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isJit());

    if (isWasm()) {
        wasmIter().~ProfilingFrameIterator();
        return;
    }

    jsJitIter().~JSJitProfilingFrameIterator();
}

bool
JS::ProfilingFrameIterator::iteratorDone()
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isJit());

    if (isWasm())
        return wasmIter().done();

    return jsJitIter().done();
}

void*
JS::ProfilingFrameIterator::stackAddress() const
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isJit());

    if (isWasm())
        return wasmIter().stackAddress();

    return jsJitIter().stackAddress();
}

Maybe<JS::ProfilingFrameIterator::Frame>
JS::ProfilingFrameIterator::getPhysicalFrameAndEntry(jit::JitcodeGlobalEntry* entry) const
{
    void* stackAddr = stackAddress();

    if (isWasm()) {
        Frame frame;
        frame.kind = Frame_Wasm;
        frame.stackAddress = stackAddr;
        frame.returnAddress = nullptr;
        frame.activation = activation_;
        frame.label = nullptr;
        return mozilla::Some(frame);
    }

    MOZ_ASSERT(isJSJit());

    // Look up an entry for the return address.
    void* returnAddr = jsJitIter().returnAddressToFp();
    jit::JitcodeGlobalTable* table = cx_->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (samplePositionInProfilerBuffer_)
        *entry = table->lookupForSamplerInfallible(returnAddr, cx_->runtime(),
                                                   *samplePositionInProfilerBuffer_);
    else
        *entry = table->lookupInfallible(returnAddr);

    MOZ_ASSERT(entry->isIon() || entry->isIonCache() || entry->isBaseline() || entry->isDummy());

    // Dummy frames produce no stack frames.
    if (entry->isDummy())
        return mozilla::Nothing();

    Frame frame;
    frame.kind = entry->isBaseline() ? Frame_Baseline : Frame_Ion;
    frame.stackAddress = stackAddr;
    frame.returnAddress = returnAddr;
    frame.activation = activation_;
    frame.label = nullptr;
    return mozilla::Some(frame);
}

uint32_t
JS::ProfilingFrameIterator::extractStack(Frame* frames, uint32_t offset, uint32_t end) const
{
    if (offset >= end)
        return 0;

    jit::JitcodeGlobalEntry entry;
    Maybe<Frame> physicalFrame = getPhysicalFrameAndEntry(&entry);

    // Dummy frames produce no stack frames.
    if (physicalFrame.isNothing())
        return 0;

    if (isWasm()) {
        frames[offset] = physicalFrame.value();
        frames[offset].label = wasmIter().label();
        return 1;
    }

    // Extract the stack for the entry.  Assume maximum inlining depth is <64
    const char* labels[64];
    uint32_t depth = entry.callStackAtAddr(cx_->runtime(), jsJitIter().returnAddressToFp(),
                                           labels, ArrayLength(labels));
    MOZ_ASSERT(depth < ArrayLength(labels));
    for (uint32_t i = 0; i < depth; i++) {
        if (offset + i >= end)
            return i;
        frames[offset + i] = physicalFrame.value();
        frames[offset + i].label = labels[i];
    }

    return depth;
}

Maybe<JS::ProfilingFrameIterator::Frame>
JS::ProfilingFrameIterator::getPhysicalFrameWithoutLabel() const
{
    jit::JitcodeGlobalEntry unused;
    return getPhysicalFrameAndEntry(&unused);
}

bool
JS::ProfilingFrameIterator::isWasm() const
{
    MOZ_ASSERT(!done());
    return kind_ == Kind::Wasm;
}

bool
JS::ProfilingFrameIterator::isJSJit() const
{
    return kind_ == Kind::JSJit;
}
