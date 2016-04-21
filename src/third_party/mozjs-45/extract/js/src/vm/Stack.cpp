/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Stack-inl.h"

#include "mozilla/PodOperations.h"

#include "jscntxt.h"

#include "asmjs/AsmJSFrameIterator.h"
#include "asmjs/AsmJSModule.h"
#include "gc/Marking.h"
#include "jit/BaselineFrame.h"
#include "jit/JitcodeMap.h"
#include "jit/JitCompartment.h"
#include "js/GCAPI.h"
#include "vm/Debugger.h"
#include "vm/Opcodes.h"

#include "jit/JitFrameIterator-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/Probes-inl.h"
#include "vm/ScopeObject-inl.h"

using namespace js;

using mozilla::Maybe;
using mozilla::PodCopy;

/*****************************************************************************/

void
InterpreterFrame::initExecuteFrame(JSContext* cx, HandleScript script, AbstractFramePtr evalInFramePrev,
                                   const Value& newTargetValue, HandleObject scopeChain,
                                   ExecuteType type)
{
    /*
     * See encoding of ExecuteType. When GLOBAL isn't set, we are executing a
     * script in the context of another frame and the frame type is determined
     * by the context.
     */
    flags_ = type | HAS_SCOPECHAIN;

    JSObject* callee = nullptr;

    // newTarget = NullValue is an initial sentinel for "please fill me in from the stack".
    // It should never be passed from Ion code.
    RootedValue newTarget(cx, newTargetValue);
    if (!(flags_ & (GLOBAL | MODULE))) {
        if (evalInFramePrev) {
            MOZ_ASSERT(evalInFramePrev.isFunctionFrame() || evalInFramePrev.isGlobalFrame());
            if (evalInFramePrev.isFunctionFrame()) {
                callee = evalInFramePrev.callee();
                if (newTarget.isNull())
                    newTarget = evalInFramePrev.newTarget();
                flags_ |= FUNCTION;
            } else {
                flags_ |= GLOBAL;
            }
        } else {
            FrameIter iter(cx);
            MOZ_ASSERT(iter.isFunctionFrame() || iter.isGlobalFrame());
            MOZ_ASSERT(!iter.isAsmJS());
            if (iter.isFunctionFrame()) {
                if (newTarget.isNull())
                    newTarget = iter.newTarget();
                callee = iter.callee(cx);
                flags_ |= FUNCTION;
            } else {
                flags_ |= GLOBAL;
            }
        }
    }

    Value* dstvp = (Value*)this - 2;

    if (isFunctionFrame()) {
        dstvp[1] = ObjectValue(*callee);
        exec.fun = &callee->as<JSFunction>();
        u.evalScript = script;
    } else {
        MOZ_ASSERT(isGlobalFrame() || isModuleFrame());
        dstvp[1] = NullValue();
        exec.script = script;
#ifdef DEBUG
        u.evalScript = (JSScript*)0xbad;
#endif
    }
    dstvp[0] = newTarget;

    scopeChain_ = scopeChain.get();
    prev_ = nullptr;
    prevpc_ = nullptr;
    prevsp_ = nullptr;

    MOZ_ASSERT_IF(evalInFramePrev, isDebuggerEvalFrame());
    evalInFramePrev_ = evalInFramePrev;

    if (script->isDebuggee())
        setIsDebuggee();

#ifdef DEBUG
    Debug_SetValueRangeToCrashOnTouch(&rval_, 1);
#endif
}

bool
InterpreterFrame::isNonGlobalEvalFrame() const
{
    return isEvalFrame() &&
           script()->enclosingStaticScope()->as<StaticEvalObject>().isNonGlobal();
}

bool
InterpreterFrame::copyRawFrameSlots(AutoValueVector* vec)
{
    if (!vec->resize(numFormalArgs() + script()->nfixed()))
        return false;
    PodCopy(vec->begin(), argv(), numFormalArgs());
    PodCopy(vec->begin() + numFormalArgs(), slots(), script()->nfixed());
    return true;
}

JSObject*
InterpreterFrame::createRestParameter(JSContext* cx)
{
    MOZ_ASSERT(fun()->hasRest());
    unsigned nformal = fun()->nargs() - 1, nactual = numActualArgs();
    unsigned nrest = (nactual > nformal) ? nactual - nformal : 0;
    Value* restvp = argv() + nformal;
    return ObjectGroup::newArrayObject(cx, restvp, nrest, GenericObject,
                                       ObjectGroup::NewArrayKind::UnknownIndex);
}

static inline void
AssertDynamicScopeMatchesStaticScope(JSContext* cx, JSScript* script, JSObject* scope)
{
#ifdef DEBUG
    RootedObject originalScope(cx, scope);
    RootedObject enclosingScope(cx, script->enclosingStaticScope());
    for (StaticScopeIter<NoGC> i(enclosingScope); !i.done(); i++) {
        if (i.type() == StaticScopeIter<NoGC>::NonSyntactic) {
            while (scope->is<DynamicWithObject>() ||
                   scope->is<NonSyntacticVariablesObject>() ||
                   (scope->is<ClonedBlockObject>() &&
                    !scope->as<ClonedBlockObject>().isSyntactic()))
            {
                MOZ_ASSERT(!IsSyntacticScope(scope));
                scope = &scope->as<ScopeObject>().enclosingScope();
            }
        } else if (i.hasSyntacticDynamicScopeObject()) {
            switch (i.type()) {
              case StaticScopeIter<NoGC>::Module:
                MOZ_ASSERT(scope->as<ModuleEnvironmentObject>().module().script() == i.moduleScript());
                scope = &scope->as<ModuleEnvironmentObject>().enclosingScope();
                break;
              case StaticScopeIter<NoGC>::Function:
                MOZ_ASSERT(scope->as<CallObject>().callee().nonLazyScript() == i.funScript());
                scope = &scope->as<CallObject>().enclosingScope();
                break;
              case StaticScopeIter<NoGC>::Block:
                MOZ_ASSERT(&i.block() == scope->as<ClonedBlockObject>().staticScope());
                scope = &scope->as<ClonedBlockObject>().enclosingScope();
                break;
              case StaticScopeIter<NoGC>::With:
                MOZ_ASSERT(&i.staticWith() == scope->as<DynamicWithObject>().staticScope());
                scope = &scope->as<DynamicWithObject>().enclosingScope();
                break;
              case StaticScopeIter<NoGC>::NamedLambda:
                scope = &scope->as<DeclEnvObject>().enclosingScope();
                break;
              case StaticScopeIter<NoGC>::Eval:
                scope = &scope->as<CallObject>().enclosingScope();
                break;
              case StaticScopeIter<NoGC>::NonSyntactic:
                MOZ_CRASH("NonSyntactic should not have a syntactic scope");
                break;
            }
        }
    }

    // In the case of a non-syntactic scope chain, the immediate parent of the
    // outermost non-syntactic scope may be the global lexical scope, or, if
    // called from Debugger, a DebugScopeObject.
    //
    // In the case of a syntactic scope chain, the outermost scope is always a
    // GlobalObject.
    MOZ_ASSERT(scope->is<GlobalObject>() || IsGlobalLexicalScope(scope) ||
               scope->is<DebugScopeObject>());
#endif
}

bool
InterpreterFrame::initFunctionScopeObjects(JSContext* cx)
{
    CallObject* callobj = CallObject::createForFunction(cx, this);
    if (!callobj)
        return false;
    pushOnScopeChain(*callobj);
    flags_ |= HAS_CALL_OBJ;
    return true;
}

bool
InterpreterFrame::prologue(JSContext* cx)
{
    RootedScript script(cx, this->script());

    MOZ_ASSERT(isModuleFrame() == !!script->module());
    MOZ_ASSERT(cx->interpreterRegs().pc == script->code());

    if (isEvalFrame()) {
        if (script->strict()) {
            CallObject* callobj = CallObject::createForStrictEval(cx, this);
            if (!callobj)
                return false;
            pushOnScopeChain(*callobj);
            flags_ |= HAS_CALL_OBJ;
        } else {
            // Non-strict eval may introduce var bindings that conflict with
            // lexical bindings in an enclosing lexical scope.
            RootedObject varObjRoot(cx, &varObj());
            if (!CheckEvalDeclarationConflicts(cx, script, scopeChain(), varObjRoot))
                return false;
        }
        return probes::EnterScript(cx, script, nullptr, this);
    }

    if (isGlobalFrame()) {
        Rooted<ClonedBlockObject*> lexicalScope(cx);
        RootedObject varObjRoot(cx);
        if (script->hasNonSyntacticScope()) {
            lexicalScope = &extensibleLexicalScope();
            varObjRoot = &varObj();
        } else {
            lexicalScope = &cx->global()->lexicalScope();
            varObjRoot = cx->global();
        }
        if (!CheckGlobalDeclarationConflicts(cx, script, lexicalScope, varObjRoot))
            return false;
        return probes::EnterScript(cx, script, nullptr, this);
    }

    AssertDynamicScopeMatchesStaticScope(cx, script, scopeChain());

    if (isModuleFrame())
        return probes::EnterScript(cx, script, nullptr, this);

    MOZ_ASSERT(isNonEvalFunctionFrame());
    if (fun()->needsCallObject() && !initFunctionScopeObjects(cx))
        return false;

    if (isConstructing()) {
        if (script->isDerivedClassConstructor()) {
            MOZ_ASSERT(callee().isClassConstructor());
            thisArgument() = MagicValue(JS_UNINITIALIZED_LEXICAL);
        } else if (thisArgument().isPrimitive()) {
            RootedObject callee(cx, &this->callee());
            RootedObject newTarget(cx, &this->newTarget().toObject());
            JSObject* obj = CreateThisForFunction(cx, callee, newTarget,
                                                  createSingleton() ? SingletonObject : GenericObject);
            if (!obj)
                return false;
            thisArgument() = ObjectValue(*obj);
        }
    }

    return probes::EnterScript(cx, script, script->functionNonDelazifying(), this);
}

void
InterpreterFrame::epilogue(JSContext* cx)
{
    RootedScript script(cx, this->script());
    probes::ExitScript(cx, script, script->functionNonDelazifying(), hasPushedSPSFrame());

    if (isEvalFrame()) {
        if (isStrictEvalFrame()) {
            MOZ_ASSERT_IF(hasCallObj(), scopeChain()->as<CallObject>().isForEval());
            if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
                DebugScopes::onPopStrictEvalScope(this);
        } else if (isNonGlobalEvalFrame()) {
            MOZ_ASSERT_IF(isDebuggerEvalFrame(), !IsSyntacticScope(scopeChain()));
        }
        return;
    }

    if (isGlobalFrame()) {
        // Confusingly, global frames may run in non-global scopes (that is,
        // not directly under the GlobalObject and its lexical scope).
        //
        // Gecko often runs global scripts under custom scopes. See users of
        // CreateNonSyntacticScopeChain.
        MOZ_ASSERT(IsGlobalLexicalScope(scopeChain()) || !IsSyntacticScope(scopeChain()));
        return;
    }

    if (isModuleFrame())
        return;

    MOZ_ASSERT(isNonEvalFunctionFrame());

    if (fun()->needsCallObject()) {
        MOZ_ASSERT_IF(hasCallObj() && !fun()->isGenerator(),
                      scopeChain()->as<CallObject>().callee().nonLazyScript() == script);
    } else {
        AssertDynamicScopeMatchesStaticScope(cx, script, scopeChain());
    }

    if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
        DebugScopes::onPopCall(this, cx);

    if (!fun()->isGenerator() &&
        isConstructing() &&
        thisArgument().isObject() &&
        returnValue().isPrimitive())
    {
        setReturnValue(thisArgument());
    }
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
InterpreterFrame::pushBlock(JSContext* cx, StaticBlockObject& block)
{
    MOZ_ASSERT(block.needsClone());

    Rooted<StaticBlockObject*> blockHandle(cx, &block);
    ClonedBlockObject* clone = ClonedBlockObject::create(cx, blockHandle, this);
    if (!clone)
        return false;

    pushOnScopeChain(*clone);

    return true;
}

bool
InterpreterFrame::freshenBlock(JSContext* cx)
{
    MOZ_ASSERT(flags_ & HAS_SCOPECHAIN);
    Rooted<ClonedBlockObject*> block(cx, &scopeChain_->as<ClonedBlockObject>());
    ClonedBlockObject* fresh = ClonedBlockObject::clone(cx, block);
    if (!fresh)
        return false;

    replaceInnermostScope(*fresh);
    return true;
}

void
InterpreterFrame::popBlock(JSContext* cx)
{
    MOZ_ASSERT(scopeChain_->is<ClonedBlockObject>());
    popOffScopeChain();
}

void
InterpreterFrame::popWith(JSContext* cx)
{
    if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
        DebugScopes::onPopWith(this);

    MOZ_ASSERT(scopeChain()->is<DynamicWithObject>());
    popOffScopeChain();
}

void
InterpreterFrame::mark(JSTracer* trc)
{
    /*
     * Normally we would use MarkRoot here, except that generators also take
     * this path. However, generators use a special write barrier when the stack
     * frame is copied to the floating frame. Therefore, no barrier is needed.
     */
    if (flags_ & HAS_SCOPECHAIN)
        TraceManuallyBarrieredEdge(trc, &scopeChain_, "scope chain");
    if (flags_ & HAS_ARGS_OBJ)
        TraceManuallyBarrieredEdge(trc, &argsObj_, "arguments");
    if (isFunctionFrame()) {
        TraceManuallyBarrieredEdge(trc, &exec.fun, "fun");
        if (isEvalFrame())
            TraceManuallyBarrieredEdge(trc, &u.evalScript, "eval script");
    } else {
        TraceManuallyBarrieredEdge(trc, &exec.script, "script");
    }
    if (trc->isMarkingTracer())
        script()->compartment()->zone()->active = true;
    if (hasReturnValue())
        TraceManuallyBarrieredEdge(trc, &rval_, "rval");
}

void
InterpreterFrame::markValues(JSTracer* trc, unsigned start, unsigned end)
{
    if (start < end)
        TraceRootRange(trc, end - start, slots() + start, "vm_stack");
}

void
InterpreterFrame::markValues(JSTracer* trc, Value* sp, jsbytecode* pc)
{
    MOZ_ASSERT(sp >= slots());

    JSScript* script = this->script();
    size_t nfixed = script->nfixed();
    size_t nlivefixed = script->calculateLiveFixed(pc);

    if (nfixed == nlivefixed) {
        // All locals are live.
        markValues(trc, 0, sp - slots());
    } else {
        // Mark operand stack.
        markValues(trc, nfixed, sp - slots());

        // Clear dead block-scoped locals.
        while (nfixed > nlivefixed)
            unaliasedLocal(--nfixed).setMagic(JS_UNINITIALIZED_LEXICAL);

        // Mark live locals.
        markValues(trc, 0, nlivefixed);
    }

    if (hasArgs()) {
        // Mark callee, |this| and arguments.
        unsigned argc = Max(numActualArgs(), numFormalArgs());
        TraceRootRange(trc, argc + 2 + isConstructing(), argv_ - 2, "fp argv");
    } else {
        // Mark callee and newTarget
        TraceRootRange(trc, 2, ((Value*)this) - 2, "stack callee and newTarget");
    }
}

static void
MarkInterpreterActivation(JSTracer* trc, InterpreterActivation* act)
{
    for (InterpreterFrameIterator frames(act); !frames.done(); ++frames) {
        InterpreterFrame* fp = frames.frame();
        fp->markValues(trc, frames.sp(), frames.pc());
        fp->mark(trc);
    }
}

void
js::MarkInterpreterActivations(JSRuntime* rt, JSTracer* trc)
{
    for (ActivationIterator iter(rt); !iter.done(); ++iter) {
        Activation* act = iter.activation();
        if (act->isInterpreter())
            MarkInterpreterActivation(trc, act->asInterpreter());
    }
}

/*****************************************************************************/

// Unlike the other methods of this class, this method is defined here so that
// we don't have to #include jsautooplen.h in vm/Stack.h.
void
InterpreterRegs::setToEndOfScript()
{
    sp = fp()->base();
    pc = fp()->script()->lastPC();
}

/*****************************************************************************/

InterpreterFrame*
InterpreterStack::pushInvokeFrame(JSContext* cx, const CallArgs& args, InitialFrameFlags initial)
{
    LifoAlloc::Mark mark = allocator_.mark();

    RootedFunction fun(cx, &args.callee().as<JSFunction>());
    RootedScript script(cx, fun->nonLazyScript());

    InterpreterFrame::Flags flags = ToFrameFlags(initial);
    Value* argv;
    InterpreterFrame* fp = getCallFrame(cx, args, script, &flags, &argv);
    if (!fp)
        return nullptr;

    fp->mark_ = mark;
    fp->initCallFrame(cx, nullptr, nullptr, nullptr, *fun, script, argv, args.length(), flags);
    return fp;
}

InterpreterFrame*
InterpreterStack::pushExecuteFrame(JSContext* cx, HandleScript script, const Value& newTargetValue,
                                   HandleObject scopeChain, ExecuteType type,
                                   AbstractFramePtr evalInFrame)
{
    LifoAlloc::Mark mark = allocator_.mark();

    unsigned nvars = 2 /* callee, newTarget */ + script->nslots();
    uint8_t* buffer = allocateFrame(cx, sizeof(InterpreterFrame) + nvars * sizeof(Value));
    if (!buffer)
        return nullptr;

    InterpreterFrame* fp = reinterpret_cast<InterpreterFrame*>(buffer + 2 * sizeof(Value));
    fp->mark_ = mark;
    fp->initExecuteFrame(cx, script, evalInFrame, newTargetValue, scopeChain, type);
    fp->initLocals();

    return fp;
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
    while (true) {
        if (data_.activations_.done()) {
            data_.state_ = DONE;
            return;
        }

        Activation* activation = data_.activations_.activation();

        // If JS_SaveFrameChain was called, stop iterating here (unless
        // GO_THROUGH_SAVED is set).
        if (data_.savedOption_ == STOP_AT_SAVED && activation->hasSavedFrameChain()) {
            data_.state_ = DONE;
            return;
        }

        // Skip activations from another context if needed.
        MOZ_ASSERT(activation->cx());
        MOZ_ASSERT(data_.cx_);
        if (data_.contextOption_ == CURRENT_CONTEXT && activation->cx() != data_.cx_) {
            ++data_.activations_;
            continue;
        }

        // If the caller supplied principals, only show activations which are subsumed (of the same
        // origin or of an origin accessible) by these principals.
        if (data_.principals_) {
            JSContext* cx = data_.cx_->asJSContext();
            if (JSSubsumesOp subsumes = cx->runtime()->securityCallbacks->subsumes) {
                if (!subsumes(data_.principals_, activation->compartment()->principals())) {
                    ++data_.activations_;
                    continue;
                }
            }
        }

        if (activation->isJit()) {
            data_.jitFrames_ = jit::JitFrameIterator(data_.activations_);

            // Stop at the first scripted frame.
            while (!data_.jitFrames_.isScripted() && !data_.jitFrames_.done())
                ++data_.jitFrames_;

            // It's possible to have an JitActivation with no scripted frames,
            // for instance if we hit an over-recursion during bailout.
            if (data_.jitFrames_.done()) {
                ++data_.activations_;
                continue;
            }

            nextJitFrame();
            data_.state_ = JIT;
            return;
        }

        if (activation->isAsmJS()) {
            data_.asmJSFrames_ = AsmJSFrameIterator(*data_.activations_->asAsmJS());

            if (data_.asmJSFrames_.done()) {
                ++data_.activations_;
                continue;
            }

            data_.state_ = ASMJS;
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

FrameIter::Data::Data(JSContext* cx, SavedOption savedOption,
                      ContextOption contextOption, DebuggerEvalOption debuggerEvalOption,
                      JSPrincipals* principals)
  : cx_(cx),
    savedOption_(savedOption),
    contextOption_(contextOption),
    debuggerEvalOption_(debuggerEvalOption),
    principals_(principals),
    pc_(nullptr),
    interpFrames_(nullptr),
    activations_(cx->runtime()),
    jitFrames_(),
    ionInlineFrameNo_(0),
    asmJSFrames_()
{
}

FrameIter::Data::Data(const FrameIter::Data& other)
  : cx_(other.cx_),
    savedOption_(other.savedOption_),
    contextOption_(other.contextOption_),
    debuggerEvalOption_(other.debuggerEvalOption_),
    principals_(other.principals_),
    state_(other.state_),
    pc_(other.pc_),
    interpFrames_(other.interpFrames_),
    activations_(other.activations_),
    jitFrames_(other.jitFrames_),
    ionInlineFrameNo_(other.ionInlineFrameNo_),
    asmJSFrames_(other.asmJSFrames_)
{
}

FrameIter::FrameIter(JSContext* cx, SavedOption savedOption)
  : data_(cx, savedOption, CURRENT_CONTEXT, FOLLOW_DEBUGGER_EVAL_PREV_LINK, nullptr),
    ionInlineFrames_(cx, (js::jit::JitFrameIterator*) nullptr)
{
    // settleOnActivation can only GC if principals are given.
    JS::AutoSuppressGCAnalysis nogc;
    settleOnActivation();
}

FrameIter::FrameIter(JSContext* cx, ContextOption contextOption,
                     SavedOption savedOption, DebuggerEvalOption debuggerEvalOption)
  : data_(cx, savedOption, contextOption, debuggerEvalOption, nullptr),
    ionInlineFrames_(cx, (js::jit::JitFrameIterator*) nullptr)
{
    // settleOnActivation can only GC if principals are given.
    JS::AutoSuppressGCAnalysis nogc;
    settleOnActivation();
}

FrameIter::FrameIter(JSContext* cx, ContextOption contextOption,
                     SavedOption savedOption, DebuggerEvalOption debuggerEvalOption,
                     JSPrincipals* principals)
  : data_(cx, savedOption, contextOption, debuggerEvalOption, principals),
    ionInlineFrames_(cx, (js::jit::JitFrameIterator*) nullptr)
{
    settleOnActivation();
}

FrameIter::FrameIter(const FrameIter& other)
  : data_(other.data_),
    ionInlineFrames_(other.data_.cx_,
                     data_.jitFrames_.isIonScripted() ? &other.ionInlineFrames_ : nullptr)
{
}

FrameIter::FrameIter(const Data& data)
  : data_(data),
    ionInlineFrames_(data.cx_, data_.jitFrames_.isIonScripted() ? &data_.jitFrames_ : nullptr)
{
    MOZ_ASSERT(data.cx_);

    if (data_.jitFrames_.isIonScripted()) {
        while (ionInlineFrames_.frameNo() != data.ionInlineFrameNo_)
            ++ionInlineFrames_;
    }
}

void
FrameIter::nextJitFrame()
{
    if (data_.jitFrames_.isIonScripted()) {
        ionInlineFrames_.resetOn(&data_.jitFrames_);
        data_.pc_ = ionInlineFrames_.pc();
    } else {
        MOZ_ASSERT(data_.jitFrames_.isBaselineJS());
        data_.jitFrames_.baselineScriptAndPc(nullptr, &data_.pc_);
    }
}

void
FrameIter::popJitFrame()
{
    MOZ_ASSERT(data_.state_ == JIT);

    if (data_.jitFrames_.isIonScripted() && ionInlineFrames_.more()) {
        ++ionInlineFrames_;
        data_.pc_ = ionInlineFrames_.pc();
        return;
    }

    ++data_.jitFrames_;
    while (!data_.jitFrames_.done() && !data_.jitFrames_.isScripted())
        ++data_.jitFrames_;

    if (!data_.jitFrames_.done()) {
        nextJitFrame();
        return;
    }

    popActivation();
}

void
FrameIter::popAsmJSFrame()
{
    MOZ_ASSERT(data_.state_ == ASMJS);

    ++data_.asmJSFrames_;
    if (data_.asmJSFrames_.done())
        popActivation();
}

FrameIter&
FrameIter::operator++()
{
    switch (data_.state_) {
      case DONE:
        MOZ_CRASH("Unexpected state");
      case INTERP:
        if (interpFrame()->isDebuggerEvalFrame() &&
            interpFrame()->evalInFramePrev() &&
            data_.debuggerEvalOption_ == FOLLOW_DEBUGGER_EVAL_PREV_LINK)
        {
            AbstractFramePtr eifPrev = interpFrame()->evalInFramePrev();

            // Eval-in-frame can cross contexts and works across saved frame
            // chains.
            ContextOption prevContextOption = data_.contextOption_;
            SavedOption prevSavedOption = data_.savedOption_;
            data_.contextOption_ = ALL_CONTEXTS;
            data_.savedOption_ = GO_THROUGH_SAVED;

            popInterpreterFrame();

            while (!hasUsableAbstractFramePtr() || abstractFramePtr() != eifPrev) {
                if (data_.state_ == JIT)
                    popJitFrame();
                else
                    popInterpreterFrame();
            }

            data_.contextOption_ = prevContextOption;
            data_.savedOption_ = prevSavedOption;
            data_.cx_ = data_.activations_->cx();
            break;
        }
        popInterpreterFrame();
        break;
      case JIT:
        popJitFrame();
        break;
      case ASMJS:
        popAsmJSFrame();
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

    MOZ_ASSERT(data_.state_ != ASMJS);
    if (data && data_.jitFrames_.isIonScripted())
        data->ionInlineFrameNo_ = ionInlineFrames_.frameNo();
    // Give the copied Data the cx of the current activation, which may be
    // different than the cx that the current FrameIter was constructed
    // with. This ensures that when we instantiate another FrameIter with the
    // copied data, its cx is still alive.
    data->cx_ = activation()->cx();
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
      case ASMJS:
        return nullptr;
      case JIT:
        return data_.jitFrames_.fp();
      case INTERP:
        return interpFrame();
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
      case ASMJS:
        return data_.activations_->compartment();
    }
    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::isFunctionFrame() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        return interpFrame()->isFunctionFrame();
      case JIT:
        MOZ_ASSERT(data_.jitFrames_.isScripted());
        if (data_.jitFrames_.isBaselineJS())
            return data_.jitFrames_.isFunctionFrame();
        return ionInlineFrames_.isFunctionFrame();
      case ASMJS:
        return true;
    }
    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::isGlobalFrame() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        return interpFrame()->isGlobalFrame();
      case JIT:
        if (data_.jitFrames_.isBaselineJS())
            return data_.jitFrames_.baselineFrame()->isGlobalFrame();
        MOZ_ASSERT(!script()->isForEval());
        return !script()->functionNonDelazifying();
      case ASMJS:
        return false;
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
        if (data_.jitFrames_.isBaselineJS())
            return data_.jitFrames_.baselineFrame()->isEvalFrame();
        MOZ_ASSERT(!script()->isForEval());
        return false;
      case ASMJS:
        return false;
    }
    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::isNonEvalFunctionFrame() const
{
    MOZ_ASSERT(!done());
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
        return interpFrame()->isNonEvalFunctionFrame();
      case JIT:
        return !isEvalFrame() && isFunctionFrame();
      case ASMJS:
        return true;
    }
    MOZ_CRASH("Unexpected state");
}

JSAtom*
FrameIter::functionDisplayAtom() const
{
    MOZ_ASSERT(isNonEvalFunctionFrame());

    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        return calleeTemplate()->displayAtom();
      case ASMJS:
        return data_.asmJSFrames_.functionDisplayAtom();
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
      case ASMJS:
        return data_.activations_->asAsmJS()->module().scriptSource();
    }

    MOZ_CRASH("Unexpected state");
}

const char*
FrameIter::scriptFilename() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        return script()->filename();
      case ASMJS:
        return data_.activations_->asAsmJS()->module().scriptSource()->filename();
    }

    MOZ_CRASH("Unexpected state");
}

const char16_t*
FrameIter::scriptDisplayURL() const
{
    ScriptSource* ss = scriptSource();
    return ss->hasDisplayURL() ? ss->displayURL() : nullptr;
}

unsigned
FrameIter::computeLine(uint32_t* column) const
{
    switch (data_.state_) {
      case DONE:
        break;
      case INTERP:
      case JIT:
        return PCToLineNumber(script(), pc(), column);
      case ASMJS:
        return data_.asmJSFrames_.computeLine(column);
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
        return script()->mutedErrors();
      case ASMJS:
        return data_.activations_->asAsmJS()->module().scriptSource()->mutedErrors();
    }

    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::isConstructing() const
{
    switch (data_.state_) {
      case DONE:
      case ASMJS:
        break;
      case JIT:
        if (data_.jitFrames_.isIonScripted())
            return ionInlineFrames_.isConstructing();
        MOZ_ASSERT(data_.jitFrames_.isBaselineJS());
        return data_.jitFrames_.isConstructing();
      case INTERP:
        return interpFrame()->isConstructing();
    }

    MOZ_CRASH("Unexpected state");
}

bool
FrameIter::ensureHasRematerializedFrame(JSContext* cx)
{
    MOZ_ASSERT(isIon());
    return !!activation()->asJit()->getRematerializedFrame(cx, data_.jitFrames_);
}

bool
FrameIter::hasUsableAbstractFramePtr() const
{
    switch (data_.state_) {
      case DONE:
      case ASMJS:
        return false;
      case JIT:
        if (data_.jitFrames_.isBaselineJS())
            return true;

        MOZ_ASSERT(data_.jitFrames_.isIonScripted());
        return !!activation()->asJit()->lookupRematerializedFrame(data_.jitFrames_.fp(),
                                                                  ionInlineFrames_.frameNo());
        break;
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
      case ASMJS:
        break;
      case JIT: {
        if (data_.jitFrames_.isBaselineJS())
            return data_.jitFrames_.baselineFrame();

        MOZ_ASSERT(data_.jitFrames_.isIonScripted());
        return activation()->asJit()->lookupRematerializedFrame(data_.jitFrames_.fp(),
                                                                ionInlineFrames_.frameNo());
        break;
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
      case ASMJS:
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
        if (data_.jitFrames_.isBaselineJS()) {
            jit::BaselineFrame* frame = data_.jitFrames_.baselineFrame();
            jit::JitActivation* activation = data_.activations_->asJit();

            // ActivationIterator::jitTop_ may be invalid, so create a new
            // activation iterator.
            data_.activations_ = ActivationIterator(data_.cx_->runtime());
            while (data_.activations_.activation() != activation)
                ++data_.activations_;

            // Look for the current frame.
            data_.jitFrames_ = jit::JitFrameIterator(data_.activations_);
            while (!data_.jitFrames_.isBaselineJS() || data_.jitFrames_.baselineFrame() != frame)
                ++data_.jitFrames_;

            // Update the pc.
            MOZ_ASSERT(data_.jitFrames_.baselineFrame() == frame);
            data_.jitFrames_.baselineScriptAndPc(nullptr, &data_.pc_);
            return;
        }
        break;
    }
    MOZ_CRASH("Unexpected state");
}

JSFunction*
FrameIter::calleeTemplate() const
{
    switch (data_.state_) {
      case DONE:
      case ASMJS:
        break;
      case INTERP:
        MOZ_ASSERT(isFunctionFrame());
        return &interpFrame()->callee();
      case JIT:
        if (data_.jitFrames_.isBaselineJS())
            return data_.jitFrames_.callee();
        MOZ_ASSERT(data_.jitFrames_.isIonScripted());
        return ionInlineFrames_.calleeTemplate();
    }
    MOZ_CRASH("Unexpected state");
}

JSFunction*
FrameIter::callee(JSContext* cx) const
{
    switch (data_.state_) {
      case DONE:
      case ASMJS:
        break;
      case INTERP:
        return calleeTemplate();
      case JIT:
        if (data_.jitFrames_.isIonScripted()) {
            jit::MaybeReadFallback recover(cx, activation()->asJit(), &data_.jitFrames_);
            return ionInlineFrames_.callee(recover);
        }
        MOZ_ASSERT(data_.jitFrames_.isBaselineJS());
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
      case ASMJS:
        break;
      case INTERP:
        MOZ_ASSERT(isFunctionFrame());
        return interpFrame()->numActualArgs();
      case JIT:
        if (data_.jitFrames_.isIonScripted())
            return ionInlineFrames_.numActualArgs();

        MOZ_ASSERT(data_.jitFrames_.isBaselineJS());
        return data_.jitFrames_.numActualArgs();
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
FrameIter::scopeChain(JSContext* cx) const
{
    switch (data_.state_) {
      case DONE:
      case ASMJS:
        break;
      case JIT:
        if (data_.jitFrames_.isIonScripted()) {
            jit::MaybeReadFallback recover(cx, activation()->asJit(), &data_.jitFrames_);
            return ionInlineFrames_.scopeChain(recover);
        }
        return data_.jitFrames_.baselineFrame()->scopeChain();
      case INTERP:
        return interpFrame()->scopeChain();
    }
    MOZ_CRASH("Unexpected state");
}

CallObject&
FrameIter::callObj(JSContext* cx) const
{
    MOZ_ASSERT(calleeTemplate()->needsCallObject());

    JSObject* pobj = scopeChain(cx);
    while (!pobj->is<CallObject>())
        pobj = pobj->enclosingScope();
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
    MOZ_ASSERT(isNonEvalFunctionFrame());

    switch (data_.state_) {
      case DONE:
      case ASMJS:
        break;
      case JIT:
        if (data_.jitFrames_.isIonScripted()) {
            jit::MaybeReadFallback recover(cx, activation()->asJit(), &data_.jitFrames_);
            return ionInlineFrames_.thisArgument(recover);
        }
        return data_.jitFrames_.baselineFrame()->thisArgument();
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
      case ASMJS:
        break;
      case INTERP:
        return interpFrame()->newTarget();
      case JIT:
        MOZ_ASSERT(data_.jitFrames_.isBaselineJS());
        return data_.jitFrames_.baselineFrame()->newTarget();
    }
    MOZ_CRASH("Unexpected state");
}

Value
FrameIter::returnValue() const
{
    switch (data_.state_) {
      case DONE:
      case ASMJS:
        break;
      case JIT:
        if (data_.jitFrames_.isBaselineJS())
            return data_.jitFrames_.baselineFrame()->returnValue();
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
      case ASMJS:
        break;
      case JIT:
        if (data_.jitFrames_.isBaselineJS()) {
            data_.jitFrames_.baselineFrame()->setReturnValue(v);
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
      case ASMJS:
        break;
      case JIT: {
        if (data_.jitFrames_.isIonScripted()) {
            return ionInlineFrames_.snapshotIterator().numAllocations() -
                ionInlineFrames_.script()->nfixed();
        }
        jit::BaselineFrame* frame = data_.jitFrames_.baselineFrame();
        return frame->numValueSlots() - data_.jitFrames_.script()->nfixed();
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
      case ASMJS:
        break;
      case JIT:
        if (data_.jitFrames_.isIonScripted()) {
            jit::SnapshotIterator si(ionInlineFrames_.snapshotIterator());
            index += ionInlineFrames_.script()->nfixed();
            return si.maybeReadAllocByIndex(index);
        }

        index += data_.jitFrames_.script()->nfixed();
        return *data_.jitFrames_.baselineFrame()->valueSlot(index);
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
  : cx_(cx), entryMonitor_(cx->runtime()->entryMonitor)
{
    cx->runtime()->entryMonitor = nullptr;
}

Value
ActivationEntryMonitor::asyncStack(JSContext* cx)
{
    RootedValue stack(cx, ObjectOrNullValue(cx->runtime()->asyncStackForNewActivations));
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
        RootedString asyncCause(cx, cx->runtime()->asyncCauseForNewActivations);
        if (entryFrame->isFunctionFrame())
            entryMonitor_->Entry(cx, entryFrame->fun(), stack, asyncCause);
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
        RootedString asyncCause(cx, cx->runtime()->asyncCauseForNewActivations);
        if (jit::CalleeTokenIsFunction(entryToken))
            entryMonitor_->Entry(cx_, jit::CalleeTokenToFunction(entryToken), stack, asyncCause);
        else
            entryMonitor_->Entry(cx_, jit::CalleeTokenToScript(entryToken), stack, asyncCause);
    }
}

/*****************************************************************************/

jit::JitActivation::JitActivation(JSContext* cx, bool active)
  : Activation(cx, Jit),
    active_(active),
    isLazyLinkExitFrame_(false),
    rematerializedFrames_(nullptr),
    ionRecovery_(cx),
    bailoutData_(nullptr),
    lastProfilingFrame_(nullptr),
    lastProfilingCallSite_(nullptr)
{
    if (active) {
        prevJitTop_ = cx->runtime()->jitTop;
        prevJitJSContext_ = cx->runtime()->jitJSContext;
        prevJitActivation_ = cx->runtime()->jitActivation;
        cx->runtime()->jitJSContext = cx;
        cx->runtime()->jitActivation = this;

        registerProfiling();
    } else {
        prevJitTop_ = nullptr;
        prevJitJSContext_ = nullptr;
        prevJitActivation_ = nullptr;
    }
}

jit::JitActivation::~JitActivation()
{
    if (active_) {
        if (isProfiling())
            unregisterProfiling();

        cx_->runtime()->jitTop = prevJitTop_;
        cx_->runtime()->jitJSContext = prevJitJSContext_;
        cx_->runtime()->jitActivation = prevJitActivation_;
    }

    // All reocvered value are taken from activation during the bailout.
    MOZ_ASSERT(ionRecovery_.empty());

    // The BailoutFrameInfo should have unregistered itself from the
    // JitActivations.
    MOZ_ASSERT(!bailoutData_);

    clearRematerializedFrames();
    js_delete(rematerializedFrames_);
}

bool
jit::JitActivation::isProfiling() const
{
    // All JitActivations can be profiled.
    return true;
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

// setActive() is inlined in GenerateFFIIonExit() with explicit masm instructions so
// changes to the logic here need to be reflected in GenerateFFIIonExit() in the enable
// and disable activation instruction sequences.
void
jit::JitActivation::setActive(JSContext* cx, bool active)
{
    // Only allowed to deactivate/activate if activation is top.
    // (Not tested and will probably fail in other situations.)
    MOZ_ASSERT(cx->runtime()->activation_ == this);
    MOZ_ASSERT(active != active_);

    if (active) {
        *((volatile bool*) active_) = true;
        prevJitTop_ = cx->runtime()->jitTop;
        prevJitJSContext_ = cx->runtime()->jitJSContext;
        prevJitActivation_ = cx->runtime()->jitActivation;
        cx->runtime()->jitJSContext = cx;
        cx->runtime()->jitActivation = this;

        registerProfiling();

    } else {
        unregisterProfiling();

        cx->runtime()->jitTop = prevJitTop_;
        cx->runtime()->jitJSContext = prevJitJSContext_;
        cx->runtime()->jitActivation = prevJitActivation_;

        *((volatile bool*) active_) = false;
    }
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
jit::JitActivation::getRematerializedFrame(JSContext* cx, const JitFrameIterator& iter, size_t inlineDepth)
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
        RematerializedFrameVector empty(cx);
        if (!rematerializedFrames_->add(p, top, Move(empty))) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

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
        AutoCompartment ac(cx, compartment_);

        if (!RematerializedFrame::RematerializeInlineFrames(cx, top, inlineIter, recover,
                                                            p->value()))
        {
            return nullptr;
        }

        // See comment in unsetPrevUpToDateUntil.
        DebugScopes::unsetPrevUpToDateUntil(cx, p->value()[inlineDepth]);
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
jit::JitActivation::markRematerializedFrames(JSTracer* trc)
{
    if (!rematerializedFrames_)
        return;
    for (RematerializedFrameTable::Enum e(*rematerializedFrames_); !e.empty(); e.popFront())
        RematerializedFrame::MarkInVector(trc, e.front().value());
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
jit::JitActivation::markIonRecovery(JSTracer* trc)
{
    for (RInstructionResults* it = ionRecovery_.begin(); it != ionRecovery_.end(); it++)
        it->trace(trc);
}

AsmJSActivation::AsmJSActivation(JSContext* cx, AsmJSModule& module)
  : Activation(cx, AsmJS),
    module_(module),
    entrySP_(nullptr),
    resumePC_(nullptr),
    fp_(nullptr),
    packedExitReason_(wasm::ExitReason(wasm::ExitReason::None).pack())
{
    (void) entrySP_;  // squelch GCC warning

    prevAsmJSForModule_ = module.activation();
    module.activation() = this;

    prevAsmJS_ = cx->runtime()->asmJSActivationStack_;
    cx->runtime()->asmJSActivationStack_ = this;

    // Now that the AsmJSActivation is fully initialized, make it visible to
    // asynchronous profiling.
    registerProfiling();
}

AsmJSActivation::~AsmJSActivation()
{
    // Hide this activation from the profiler before is is destroyed.
    unregisterProfiling();

    MOZ_ASSERT(fp_ == nullptr);

    MOZ_ASSERT(module_.activation() == this);
    module_.activation() = prevAsmJSForModule_;

    JSContext* cx = cx_->asJSContext();
    MOZ_ASSERT(cx->runtime()->asmJSActivationStack_ == this);

    cx->runtime()->asmJSActivationStack_ = prevAsmJS_;
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
    cx_->runtime()->profilingActivation_ = this;
}

void
Activation::unregisterProfiling()
{
    MOZ_ASSERT(isProfiling());
    MOZ_ASSERT(cx_->runtime()->profilingActivation_ == this);

    // There may be a non-active jit activation in the linked list.  Skip past it.
    Activation* prevProfiling = prevProfiling_;
    while (prevProfiling && prevProfiling->isJit() && !prevProfiling->asJit()->isActive())
        prevProfiling = prevProfiling->prevProfiling_;

    cx_->runtime()->profilingActivation_ = prevProfiling;
}

ActivationIterator::ActivationIterator(JSRuntime* rt)
  : jitTop_(rt->jitTop),
    activation_(rt->activation_)
{
    settle();
}

ActivationIterator&
ActivationIterator::operator++()
{
    MOZ_ASSERT(activation_);
    if (activation_->isJit() && activation_->asJit()->isActive())
        jitTop_ = activation_->asJit()->prevJitTop();
    activation_ = activation_->prev();
    settle();
    return *this;
}

void
ActivationIterator::settle()
{
    // Stop at the next active activation. No need to update jitTop_, since
    // we don't iterate over an active jit activation.
    while (!done() && activation_->isJit() && !activation_->asJit()->isActive())
        activation_ = activation_->prev();
}

JS::ProfilingFrameIterator::ProfilingFrameIterator(JSRuntime* rt, const RegisterState& state,
                                                   uint32_t sampleBufferGen)
  : rt_(rt),
    sampleBufferGen_(sampleBufferGen),
    activation_(nullptr),
    savedPrevJitTop_(nullptr)
{
    if (!rt->spsProfiler.enabled())
        MOZ_CRASH("ProfilingFrameIterator called when spsProfiler not enabled for runtime.");

    if (!rt->profilingActivation())
        return;

    // If profiler sampling is not enabled, skip.
    if (!rt_->isProfilerSamplingEnabled())
        return;

    activation_ = rt->profilingActivation();

    MOZ_ASSERT(activation_->isProfiling());

    static_assert(sizeof(AsmJSProfilingFrameIterator) <= StorageSpace &&
                  sizeof(jit::JitProfilingFrameIterator) <= StorageSpace,
                  "Need to increase storage");

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
    MOZ_ASSERT(activation_->isAsmJS() || activation_->isJit());

    if (activation_->isAsmJS()) {
        ++asmJSIter();
        settle();
        return;
    }

    ++jitIter();
    settle();
}

void
JS::ProfilingFrameIterator::settle()
{
    while (iteratorDone()) {
        iteratorDestroy();
        activation_ = activation_->prevProfiling();

        // Skip past any non-active jit activations in the list.
        while (activation_ && activation_->isJit() && !activation_->asJit()->isActive())
            activation_ = activation_->prevProfiling();

        if (!activation_)
            return;
        iteratorConstruct();
    }
}

void
JS::ProfilingFrameIterator::iteratorConstruct(const RegisterState& state)
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isAsmJS() || activation_->isJit());

    if (activation_->isAsmJS()) {
        new (storage_.addr()) AsmJSProfilingFrameIterator(*activation_->asAsmJS(), state);
        // Set savedPrevJitTop_ to the actual jitTop_ from the runtime.
        savedPrevJitTop_ = activation_->cx()->runtime()->jitTop;
        return;
    }

    MOZ_ASSERT(activation_->asJit()->isActive());
    new (storage_.addr()) jit::JitProfilingFrameIterator(rt_, state);
}

void
JS::ProfilingFrameIterator::iteratorConstruct()
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isAsmJS() || activation_->isJit());

    if (activation_->isAsmJS()) {
        new (storage_.addr()) AsmJSProfilingFrameIterator(*activation_->asAsmJS());
        return;
    }

    MOZ_ASSERT(activation_->asJit()->isActive());
    MOZ_ASSERT(savedPrevJitTop_ != nullptr);
    new (storage_.addr()) jit::JitProfilingFrameIterator(savedPrevJitTop_);
}

void
JS::ProfilingFrameIterator::iteratorDestroy()
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isAsmJS() || activation_->isJit());

    if (activation_->isAsmJS()) {
        asmJSIter().~AsmJSProfilingFrameIterator();
        return;
    }

    // Save prevjitTop for later use
    savedPrevJitTop_ = activation_->asJit()->prevJitTop();
    jitIter().~JitProfilingFrameIterator();
}

bool
JS::ProfilingFrameIterator::iteratorDone()
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isAsmJS() || activation_->isJit());

    if (activation_->isAsmJS())
        return asmJSIter().done();

    return jitIter().done();
}

void*
JS::ProfilingFrameIterator::stackAddress() const
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(activation_->isAsmJS() || activation_->isJit());

    if (activation_->isAsmJS())
        return asmJSIter().stackAddress();

    return jitIter().stackAddress();
}

Maybe<JS::ProfilingFrameIterator::Frame>
JS::ProfilingFrameIterator::getPhysicalFrameAndEntry(jit::JitcodeGlobalEntry* entry) const
{
    void* stackAddr = stackAddress();

    if (isAsmJS()) {
        Frame frame;
        frame.kind = Frame_AsmJS;
        frame.stackAddress = stackAddr;
        frame.returnAddress = nullptr;
        frame.activation = activation_;
        frame.label = nullptr;
        return mozilla::Some(frame);
    }

    MOZ_ASSERT(isJit());

    // Look up an entry for the return address.
    void* returnAddr = jitIter().returnAddressToFp();
    jit::JitcodeGlobalTable* table = rt_->jitRuntime()->getJitcodeGlobalTable();
    if (hasSampleBufferGen())
        table->lookupForSampler(returnAddr, entry, rt_, sampleBufferGen_);
    else
        table->lookupInfallible(returnAddr, entry, rt_);

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

    if (isAsmJS()) {
        frames[offset] = physicalFrame.value();
        frames[offset].label = asmJSIter().label();
        return 1;
    }

    // Extract the stack for the entry.  Assume maximum inlining depth is <64
    const char* labels[64];
    uint32_t depth = entry.callStackAtAddr(rt_, jitIter().returnAddressToFp(), labels, 64);
    MOZ_ASSERT(depth < 64);
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
JS::ProfilingFrameIterator::isAsmJS() const
{
    MOZ_ASSERT(!done());
    return activation_->isAsmJS();
}

bool
JS::ProfilingFrameIterator::isJit() const
{
    return activation_->isJit();
}
