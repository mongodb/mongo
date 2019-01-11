/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JavaScript bytecode interpreter.
 */

#include "vm/Interpreter-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/Sprintf.h"

#include <string.h>

#include "jsarray.h"
#include "jslibmath.h"
#include "jsnum.h"

#include "builtin/Eval.h"
#include "builtin/String.h"
#include "jit/AtomicOperations.h"
#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "jit/IonAnalysis.h"
#include "jit/Jit.h"
#include "util/StringBuffer.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BytecodeUtil.h"
#include "vm/Debugger.h"
#include "vm/GeneratorObject.h"
#include "vm/Iteration.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/Opcodes.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/Stopwatch.h"
#include "vm/StringType.h"
#include "vm/TraceLogging.h"

#include "jsboolinlines.h"

#include "jit/JitFrames-inl.h"
#include "vm/Debugger-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSFunction-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Probes-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::DebugOnly;
using mozilla::NumberEqualsInt32;

template <bool Eq>
static MOZ_ALWAYS_INLINE bool
LooseEqualityOp(JSContext* cx, InterpreterRegs& regs)
{
    HandleValue rval = regs.stackHandleAt(-1);
    HandleValue lval = regs.stackHandleAt(-2);
    bool cond;
    if (!LooselyEqual(cx, lval, rval, &cond))
        return false;
    cond = (cond == Eq);
    regs.sp--;
    regs.sp[-1].setBoolean(cond);
    return true;
}

bool
js::BoxNonStrictThis(JSContext* cx, HandleValue thisv, MutableHandleValue vp)
{
    /*
     * Check for SynthesizeFrame poisoning and fast constructors which
     * didn't check their callee properly.
     */
    MOZ_ASSERT(!thisv.isMagic());

    if (thisv.isNullOrUndefined()) {
        vp.set(cx->global()->lexicalEnvironment().thisValue());
        return true;
    }

    if (thisv.isObject()) {
        vp.set(thisv);
        return true;
    }

    JSObject* obj = PrimitiveToObject(cx, thisv);
    if (!obj)
        return false;

    vp.setObject(*obj);
    return true;
}

bool
js::GetFunctionThis(JSContext* cx, AbstractFramePtr frame, MutableHandleValue res)
{
    MOZ_ASSERT(frame.isFunctionFrame());
    MOZ_ASSERT(!frame.callee()->isArrow());

    if (frame.thisArgument().isObject() ||
        frame.callee()->strict() ||
        frame.callee()->isSelfHostedBuiltin())
    {
        res.set(frame.thisArgument());
        return true;
    }

    RootedValue thisv(cx, frame.thisArgument());

    // If there is a NSVO on environment chain, use it as basis for fallback
    // global |this|. This gives a consistent definition of global lexical
    // |this| between function and global contexts.
    //
    // NOTE: If only non-syntactic WithEnvironments are on the chain, we use the
    // global lexical |this| value. This is for compatibility with the Subscript
    // Loader.
    if (frame.script()->hasNonSyntacticScope() && thisv.isNullOrUndefined()) {
        RootedObject env(cx, frame.environmentChain());
        while (true) {
            if (IsNSVOLexicalEnvironment(env) || IsGlobalLexicalEnvironment(env)) {
                res.set(GetThisValueOfLexical(env));
                return true;
            }
            if (!env->enclosingEnvironment()) {
                // This can only happen in Debugger eval frames: in that case we
                // don't always have a global lexical env, see EvaluateInEnv.
                MOZ_ASSERT(env->is<GlobalObject>());
                res.set(GetThisValue(env));
                return true;
            }
            env = env->enclosingEnvironment();
        }
    }

    return BoxNonStrictThis(cx, thisv, res);
}

void
js::GetNonSyntacticGlobalThis(JSContext* cx, HandleObject envChain, MutableHandleValue res)
{
    RootedObject env(cx, envChain);
    while (true) {
        if (IsExtensibleLexicalEnvironment(env)) {
            res.set(GetThisValueOfLexical(env));
            return;
        }
        if (!env->enclosingEnvironment()) {
            // This can only happen in Debugger eval frames: in that case we
            // don't always have a global lexical env, see EvaluateInEnv.
            MOZ_ASSERT(env->is<GlobalObject>());
            res.set(GetThisValue(env));
            return;
        }
        env = env->enclosingEnvironment();
    }
}

bool
js::Debug_CheckSelfHosted(JSContext* cx, HandleValue fun)
{
#ifndef DEBUG
    MOZ_CRASH("self-hosted checks should only be done in Debug builds");
#endif

    RootedObject funObj(cx, UncheckedUnwrap(&fun.toObject()));
    MOZ_ASSERT(funObj->as<JSFunction>().isSelfHostedOrIntrinsic());

    // This is purely to police self-hosted code. There is no actual operation.
    return true;
}

static inline bool
GetPropertyOperation(JSContext* cx, InterpreterFrame* fp, HandleScript script, jsbytecode* pc,
                     MutableHandleValue lval, MutableHandleValue vp)
{
    JSOp op = JSOp(*pc);

    if (op == JSOP_LENGTH) {
        if (IsOptimizedArguments(fp, lval)) {
            vp.setInt32(fp->numActualArgs());
            return true;
        }

        if (GetLengthProperty(lval, vp))
            return true;
    }

    RootedPropertyName name(cx, script->getName(pc));

    if (name == cx->names().callee && IsOptimizedArguments(fp, lval)) {
        vp.setObject(fp->callee());
        return true;
    }

    // Copy lval, because it might alias vp.
    RootedValue v(cx, lval);
    return GetProperty(cx, v, name, vp);
}

static inline bool
GetNameOperation(JSContext* cx, InterpreterFrame* fp, jsbytecode* pc, MutableHandleValue vp)
{
    RootedObject envChain(cx, fp->environmentChain());
    RootedPropertyName name(cx, fp->script()->getName(pc));

    /*
     * Skip along the env chain to the enclosing global object. This is
     * used for GNAME opcodes where the bytecode emitter has determined a
     * name access must be on the global. It also insulates us from bugs
     * in the emitter: type inference will assume that GNAME opcodes are
     * accessing the global object, and the inferred behavior should match
     * the actual behavior even if the id could be found on the env chain
     * before the global object.
     */
    if (IsGlobalOp(JSOp(*pc)) && !fp->script()->hasNonSyntacticScope())
        envChain = &envChain->global().lexicalEnvironment();

    /* Kludge to allow (typeof foo == "undefined") tests. */
    JSOp op2 = JSOp(pc[JSOP_GETNAME_LENGTH]);
    if (op2 == JSOP_TYPEOF)
        return GetEnvironmentName<GetNameMode::TypeOf>(cx, envChain, name, vp);
    return GetEnvironmentName<GetNameMode::Normal>(cx, envChain, name, vp);
}

static inline bool
GetImportOperation(JSContext* cx, InterpreterFrame* fp, jsbytecode* pc, MutableHandleValue vp)
{
    RootedObject obj(cx, fp->environmentChain()), env(cx), pobj(cx);
    RootedPropertyName name(cx, fp->script()->getName(pc));
    Rooted<PropertyResult> prop(cx);

    MOZ_ALWAYS_TRUE(LookupName(cx, name, obj, &env, &pobj, &prop));
    MOZ_ASSERT(env && env->is<ModuleEnvironmentObject>());
    MOZ_ASSERT(env->as<ModuleEnvironmentObject>().hasImportBinding(name));
    return FetchName<GetNameMode::Normal>(cx, env, pobj, name, prop, vp);
}

static bool
SetPropertyOperation(JSContext* cx, JSOp op, HandleValue lval, HandleId id, HandleValue rval)
{
    MOZ_ASSERT(op == JSOP_SETPROP || op == JSOP_STRICTSETPROP);

    RootedObject obj(cx, ToObjectFromStack(cx, lval));
    if (!obj)
        return false;

    ObjectOpResult result;
    return SetProperty(cx, obj, id, rval, lval, result) &&
           result.checkStrictErrorOrWarning(cx, obj, id, op == JSOP_STRICTSETPROP);
}

JSFunction*
js::MakeDefaultConstructor(JSContext* cx, HandleScript script, jsbytecode* pc, HandleObject proto)
{
    JSOp op = JSOp(*pc);
    JSAtom* atom = script->getAtom(pc);
    bool derived = op == JSOP_DERIVEDCONSTRUCTOR;
    MOZ_ASSERT(derived == !!proto);

    jssrcnote* classNote = GetSrcNote(cx, script, pc);
    MOZ_ASSERT(classNote && SN_TYPE(classNote) == SRC_CLASS_SPAN);

    PropertyName* lookup = derived ? cx->names().DefaultDerivedClassConstructor
                                   : cx->names().DefaultBaseClassConstructor;

    RootedPropertyName selfHostedName(cx, lookup);
    RootedAtom name(cx, atom == cx->names().empty ? nullptr : atom);

    RootedFunction ctor(cx);
    if (!cx->runtime()->createLazySelfHostedFunctionClone(cx, selfHostedName, name,
                                                          /* nargs = */ !!derived,
                                                          proto, TenuredObject, &ctor))
    {
        return nullptr;
    }

    ctor->setIsConstructor();
    ctor->setIsClassConstructor();
    MOZ_ASSERT(ctor->infallibleIsDefaultClassConstructor(cx));

    // Create the script now, as the source span needs to be overridden for
    // toString. Calling toString on a class constructor must not return the
    // source for just the constructor function.
    JSScript *ctorScript = JSFunction::getOrCreateScript(cx, ctor);
    if (!ctorScript)
        return nullptr;
    uint32_t classStartOffset = GetSrcNoteOffset(classNote, 0);
    uint32_t classEndOffset = GetSrcNoteOffset(classNote, 1);
    ctorScript->setDefaultClassConstructorSpan(script->sourceObject(), classStartOffset,
                                               classEndOffset);

    return ctor;
}

bool
js::ReportIsNotFunction(JSContext* cx, HandleValue v, int numToSkip, MaybeConstruct construct)
{
    unsigned error = construct ? JSMSG_NOT_CONSTRUCTOR : JSMSG_NOT_FUNCTION;
    int spIndex = numToSkip >= 0 ? -(numToSkip + 1) : JSDVG_SEARCH_STACK;

    ReportValueError(cx, error, spIndex, v, nullptr);
    return false;
}

JSObject*
js::ValueToCallable(JSContext* cx, HandleValue v, int numToSkip, MaybeConstruct construct)
{
    if (v.isObject() && v.toObject().isCallable()) {
        return &v.toObject();
    }

    ReportIsNotFunction(cx, v, numToSkip, construct);
    return nullptr;
}

static bool
MaybeCreateThisForConstructor(JSContext* cx, JSScript* calleeScript, const CallArgs& args,
                              bool createSingleton)
{
    if (args.thisv().isObject())
        return true;

    RootedObject callee(cx, &args.callee());
    RootedObject newTarget(cx, &args.newTarget().toObject());
    NewObjectKind newKind = createSingleton ? SingletonObject : GenericObject;

    return CreateThis(cx, callee, calleeScript, newTarget, newKind, args.mutableThisv());
}

static MOZ_NEVER_INLINE bool
Interpret(JSContext* cx, RunState& state);

InterpreterFrame*
InvokeState::pushInterpreterFrame(JSContext* cx)
{
    return cx->interpreterStack().pushInvokeFrame(cx, args_, construct_);
}

InterpreterFrame*
ExecuteState::pushInterpreterFrame(JSContext* cx)
{
    return cx->interpreterStack().pushExecuteFrame(cx, script_, newTargetValue_,
                                                   envChain_, evalInFrame_);
}

InterpreterFrame*
RunState::pushInterpreterFrame(JSContext* cx)
{
    if (isInvoke())
        return asInvoke()->pushInterpreterFrame(cx);
    return asExecute()->pushInterpreterFrame(cx);
}

// MSVC with PGO inlines a lot of functions in RunScript, resulting in large
// stack frames and stack overflow issues, see bug 1167883. Turn off PGO to
// avoid this.
#ifdef _MSC_VER
# pragma optimize("g", off)
#endif
bool
js::RunScript(JSContext* cx, RunState& state)
{
    if (!CheckRecursionLimit(cx))
        return false;

    // Since any script can conceivably GC, make sure it's safe to do so.
    cx->verifyIsSafeToGC();

    MOZ_DIAGNOSTIC_ASSERT(cx->compartment()->isSystem() ||
                          cx->runtime()->allowContentJS());

    MOZ_ASSERT(!cx->enableAccessValidation ||
               cx->compartment()->isAccessValid());

    if (!Debugger::checkNoExecute(cx, state.script()))
        return false;

#if defined(MOZ_HAVE_RDTSC)
    js::AutoStopwatch stopwatch(cx);
#endif // defined(MOZ_HAVE_RDTSC)

    GeckoProfilerEntryMarker marker(cx, state.script());

    state.script()->ensureNonLazyCanonicalFunction();

    jit::EnterJitStatus status = jit::MaybeEnterJit(cx, state);
    switch (status) {
      case jit::EnterJitStatus::Error:
        return false;
      case jit::EnterJitStatus::Ok:
        return true;
      case jit::EnterJitStatus::NotEntered:
        break;
    }

    if (state.isInvoke()) {
        InvokeState& invoke = *state.asInvoke();
        TypeMonitorCall(cx, invoke.args(), invoke.constructing());
    }

    return Interpret(cx, state);
}
#ifdef _MSC_VER
# pragma optimize("", on)
#endif

/*
 * Find a function reference and its 'this' value implicit first parameter
 * under argc arguments on cx's stack, and call the function.  Push missing
 * required arguments, allocate declared local variables, and pop everything
 * when done.  Then push the return value.
 *
 * Note: This function DOES NOT call GetThisValue to munge |args.thisv()| if
 *       necessary.  The caller (usually the interpreter) must have performed
 *       this step already!
 */
bool
js::InternalCallOrConstruct(JSContext* cx, const CallArgs& args, MaybeConstruct construct)
{
    MOZ_ASSERT(args.length() <= ARGS_LENGTH_MAX);
    MOZ_ASSERT(!cx->zone()->types.activeAnalysis);

    unsigned skipForCallee = args.length() + 1 + (construct == CONSTRUCT);
    if (args.calleev().isPrimitive())
        return ReportIsNotFunction(cx, args.calleev(), skipForCallee, construct);

    /* Invoke non-functions. */
    if (MOZ_UNLIKELY(!args.callee().is<JSFunction>())) {
        MOZ_ASSERT_IF(construct, !args.callee().constructHook());
        JSNative call = args.callee().callHook();
        if (!call)
            return ReportIsNotFunction(cx, args.calleev(), skipForCallee, construct);
        return CallJSNative(cx, call, args);
    }

    /* Invoke native functions. */
    RootedFunction fun(cx, &args.callee().as<JSFunction>());
    if (construct != CONSTRUCT && fun->isClassConstructor()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CALL_CLASS_CONSTRUCTOR);
        return false;
    }

    if (fun->isNative()) {
        MOZ_ASSERT_IF(construct, !fun->isConstructor());
        JSNative native = fun->native();
        if (!construct && args.ignoresReturnValue() && fun->hasJitInfo()) {
            const JSJitInfo* jitInfo = fun->jitInfo();
            if (jitInfo->type() == JSJitInfo::IgnoresReturnValueNative)
                native = jitInfo->ignoresReturnValueMethod;
        }
        return CallJSNative(cx, native, args);
    }

    if (!JSFunction::getOrCreateScript(cx, fun))
        return false;

    /* Run function until JSOP_RETRVAL, JSOP_RETURN or error. */
    InvokeState state(cx, args, construct);

    // Check to see if createSingleton flag should be set for this frame.
    if (construct) {
        bool createSingleton = false;
        jsbytecode* pc;
        if (JSScript* script = cx->currentScript(&pc)) {
            if (ObjectGroup::useSingletonForNewObject(cx, script, pc))
                createSingleton = true;
        }

        if (!MaybeCreateThisForConstructor(cx, state.script(), args, createSingleton))
            return false;
    }

    bool ok = RunScript(cx, state);

    MOZ_ASSERT_IF(ok && construct, args.rval().isObject());
    return ok;
}

static bool
InternalCall(JSContext* cx, const AnyInvokeArgs& args)
{
    MOZ_ASSERT(args.array() + args.length() == args.end(),
               "must pass calling arguments to a calling attempt");

    if (args.thisv().isObject()) {
        // We must call the thisValue hook in case we are not called from the
        // interpreter, where a prior bytecode has computed an appropriate
        // |this| already.  But don't do that if fval is a DOM function.
        HandleValue fval = args.calleev();
        if (!fval.isObject() || !fval.toObject().is<JSFunction>() ||
            !fval.toObject().as<JSFunction>().isNative() ||
            !fval.toObject().as<JSFunction>().hasJitInfo() ||
            fval.toObject().as<JSFunction>().jitInfo()->needsOuterizedThisObject())
        {
            JSObject* thisObj = &args.thisv().toObject();
            args.mutableThisv().set(GetThisValue(thisObj));
        }
    }

    return InternalCallOrConstruct(cx, args, NO_CONSTRUCT);
}

bool
js::CallFromStack(JSContext* cx, const CallArgs& args)
{
    return InternalCall(cx, static_cast<const AnyInvokeArgs&>(args));
}

// ES7 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93 7.3.12 Call.
bool
js::Call(JSContext* cx, HandleValue fval, HandleValue thisv, const AnyInvokeArgs& args,
         MutableHandleValue rval)
{
    // Explicitly qualify these methods to bypass AnyInvokeArgs's deliberate
    // shadowing.
    args.CallArgs::setCallee(fval);
    args.CallArgs::setThis(thisv);

    if (!InternalCall(cx, args))
        return false;

    rval.set(args.rval());
    return true;
}

static bool
InternalConstruct(JSContext* cx, const AnyConstructArgs& args)
{
    MOZ_ASSERT(args.array() + args.length() + 1 == args.end(),
               "must pass constructing arguments to a construction attempt");
    MOZ_ASSERT(!JSFunction::class_.getConstruct());

    // Callers are responsible for enforcing these preconditions.
    MOZ_ASSERT(IsConstructor(args.calleev()),
               "trying to construct a value that isn't a constructor");
    MOZ_ASSERT(IsConstructor(args.CallArgs::newTarget()),
               "provided new.target value must be a constructor");

    MOZ_ASSERT(args.thisv().isMagic(JS_IS_CONSTRUCTING) || args.thisv().isObject());

    JSObject& callee = args.callee();
    if (callee.is<JSFunction>()) {
        RootedFunction fun(cx, &callee.as<JSFunction>());

        if (fun->isNative())
            return CallJSNativeConstructor(cx, fun->native(), args);

        if (!InternalCallOrConstruct(cx, args, CONSTRUCT))
            return false;

        MOZ_ASSERT(args.CallArgs::rval().isObject());
        return true;
    }

    JSNative construct = callee.constructHook();
    MOZ_ASSERT(construct != nullptr, "IsConstructor without a construct hook?");

    return CallJSNativeConstructor(cx, construct, args);
}

// Check that |callee|, the callee in a |new| expression, is a constructor.
static bool
StackCheckIsConstructorCalleeNewTarget(JSContext* cx, HandleValue callee, HandleValue newTarget)
{
    // Calls from the stack could have any old non-constructor callee.
    if (!IsConstructor(callee)) {
        ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_SEARCH_STACK, callee, nullptr);
        return false;
    }

    // The new.target has already been vetted by previous calls, or is the callee.
    // We can just assert that it's a constructor.
    MOZ_ASSERT(IsConstructor(newTarget));

    return true;
}

bool
js::ConstructFromStack(JSContext* cx, const CallArgs& args)
{
    if (!StackCheckIsConstructorCalleeNewTarget(cx, args.calleev(), args.newTarget()))
        return false;

    return InternalConstruct(cx, static_cast<const AnyConstructArgs&>(args));
}

bool
js::Construct(JSContext* cx, HandleValue fval, const AnyConstructArgs& args, HandleValue newTarget,
              MutableHandleObject objp)
{
    MOZ_ASSERT(args.thisv().isMagic(JS_IS_CONSTRUCTING));

    // Explicitly qualify to bypass AnyConstructArgs's deliberate shadowing.
    args.CallArgs::setCallee(fval);
    args.CallArgs::newTarget().set(newTarget);

    if (!InternalConstruct(cx, args))
        return false;

    MOZ_ASSERT(args.CallArgs::rval().isObject());
    objp.set(&args.CallArgs::rval().toObject());
    return true;
}

bool
js::InternalConstructWithProvidedThis(JSContext* cx, HandleValue fval, HandleValue thisv,
                                      const AnyConstructArgs& args, HandleValue newTarget,
                                      MutableHandleValue rval)
{
    args.CallArgs::setCallee(fval);

    MOZ_ASSERT(thisv.isObject());
    args.CallArgs::setThis(thisv);

    args.CallArgs::newTarget().set(newTarget);

    if (!InternalConstruct(cx, args))
        return false;

    rval.set(args.CallArgs::rval());
    return true;
}

bool
js::CallGetter(JSContext* cx, HandleValue thisv, HandleValue getter, MutableHandleValue rval)
{
    // Invoke could result in another try to get or set the same id again, see
    // bug 355497.
    if (!CheckRecursionLimit(cx))
        return false;

    FixedInvokeArgs<0> args(cx);

    return Call(cx, getter, thisv, args, rval);
}

bool
js::CallSetter(JSContext* cx, HandleValue thisv, HandleValue setter, HandleValue v)
{
    if (!CheckRecursionLimit(cx))
        return false;

    FixedInvokeArgs<1> args(cx);

    args[0].set(v);

    RootedValue ignored(cx);
    return Call(cx, setter, thisv, args, &ignored);
}

bool
js::ExecuteKernel(JSContext* cx, HandleScript script, JSObject& envChainArg,
                  const Value& newTargetValue, AbstractFramePtr evalInFrame,
                  Value* result)
{
    MOZ_ASSERT_IF(script->isGlobalCode(),
                  IsGlobalLexicalEnvironment(&envChainArg) ||
                  !IsSyntacticEnvironment(&envChainArg));
#ifdef DEBUG
    RootedObject terminatingEnv(cx, &envChainArg);
    while (IsSyntacticEnvironment(terminatingEnv))
        terminatingEnv = terminatingEnv->enclosingEnvironment();
    MOZ_ASSERT(terminatingEnv->is<GlobalObject>() ||
               script->hasNonSyntacticScope());
#endif

    if (script->treatAsRunOnce()) {
        if (script->hasRunOnce()) {
            JS_ReportErrorASCII(cx, "Trying to execute a run-once script multiple times");
            return false;
        }

        script->setHasRunOnce();
    }

    if (script->isEmpty()) {
        if (result)
            result->setUndefined();
        return true;
    }

    probes::StartExecution(script);
    ExecuteState state(cx, script, newTargetValue, envChainArg, evalInFrame, result);
    bool ok = RunScript(cx, state);
    probes::StopExecution(script);

    return ok;
}

bool
js::Execute(JSContext* cx, HandleScript script, JSObject& envChainArg, Value* rval)
{
    /* The env chain is something we control, so we know it can't
       have any outer objects on it. */
    RootedObject envChain(cx, &envChainArg);
    MOZ_ASSERT(!IsWindowProxy(envChain));

    if (script->module()) {
        MOZ_RELEASE_ASSERT(envChain == script->module()->environment(),
                           "Module scripts can only be executed in the module's environment");
    } else {
        MOZ_RELEASE_ASSERT(IsGlobalLexicalEnvironment(envChain) || script->hasNonSyntacticScope(),
                           "Only global scripts with non-syntactic envs can be executed with "
                           "interesting envchains");
    }

    /* Ensure the env chain is all same-compartment and terminates in a global. */
#ifdef DEBUG
    JSObject* s = envChain;
    do {
        assertSameCompartment(cx, s);
        MOZ_ASSERT_IF(!s->enclosingEnvironment(), s->is<GlobalObject>());
    } while ((s = s->enclosingEnvironment()));
#endif

    return ExecuteKernel(cx, script, *envChain, NullValue(),
                         NullFramePtr() /* evalInFrame */, rval);
}

/*
 * ES6 (4-25-16) 12.10.4 InstanceofOperator
 */
extern bool
js::InstanceOfOperator(JSContext* cx, HandleObject obj, HandleValue v, bool* bp)
{
    /* Step 1. is handled by caller. */

    /* Step 2. */
    RootedValue hasInstance(cx);
    RootedId id(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().hasInstance));
    if (!GetProperty(cx, obj, obj, id, &hasInstance))
        return false;

    if (!hasInstance.isNullOrUndefined()) {
        if (!IsCallable(hasInstance))
            return ReportIsNotFunction(cx, hasInstance);

        /* Step 3. */
        RootedValue rval(cx);
        if (!Call(cx, hasInstance, obj, v, &rval))
            return false;
        *bp = ToBoolean(rval);
        return true;
    }

    /* Step 4. */
    if (!obj->isCallable()) {
        RootedValue val(cx, ObjectValue(*obj));
        return ReportIsNotFunction(cx, val);
    }

    /* Step 5. */
    return OrdinaryHasInstance(cx, obj, v, bp);
}

bool
js::HasInstance(JSContext* cx, HandleObject obj, HandleValue v, bool* bp)
{
    const Class* clasp = obj->getClass();
    RootedValue local(cx, v);
    if (JSHasInstanceOp hasInstance = clasp->getHasInstance())
        return hasInstance(cx, obj, &local, bp);
    return js::InstanceOfOperator(cx, obj, local, bp);
}

static inline bool
EqualGivenSameType(JSContext* cx, HandleValue lval, HandleValue rval, bool* equal)
{
    MOZ_ASSERT(SameType(lval, rval));

    if (lval.isString())
        return EqualStrings(cx, lval.toString(), rval.toString(), equal);
    if (lval.isDouble()) {
        *equal = (lval.toDouble() == rval.toDouble());
        return true;
    }
    if (lval.isGCThing()) {  // objects or symbols
        *equal = (lval.toGCThing() == rval.toGCThing());
        return true;
    }
    *equal = lval.get().payloadAsRawUint32() == rval.get().payloadAsRawUint32();
    MOZ_ASSERT_IF(lval.isUndefined() || lval.isNull(), *equal);
    return true;
}

static inline bool
LooselyEqualBooleanAndOther(JSContext* cx, HandleValue lval, HandleValue rval, bool* result)
{
    MOZ_ASSERT(!rval.isBoolean());
    RootedValue lvalue(cx, Int32Value(lval.toBoolean() ? 1 : 0));

    // The tail-call would end up in Step 3.
    if (rval.isNumber()) {
        *result = (lvalue.toNumber() == rval.toNumber());
        return true;
    }
    // The tail-call would end up in Step 6.
    if (rval.isString()) {
        double num;
        if (!StringToNumber(cx, rval.toString(), &num))
            return false;
        *result = (lvalue.toNumber() == num);
        return true;
    }

    return LooselyEqual(cx, lvalue, rval, result);
}

// ES6 draft rev32 7.2.12 Abstract Equality Comparison
bool
js::LooselyEqual(JSContext* cx, HandleValue lval, HandleValue rval, bool* result)
{
    // Step 3.
    if (SameType(lval, rval))
        return EqualGivenSameType(cx, lval, rval, result);

    // Handle int32 x double.
    if (lval.isNumber() && rval.isNumber()) {
        *result = (lval.toNumber() == rval.toNumber());
        return true;
    }

    // Step 4. This a bit more complex, because of the undefined emulating object.
    if (lval.isNullOrUndefined()) {
        // We can return early here, because null | undefined is only equal to the same set.
        *result = rval.isNullOrUndefined() ||
                  (rval.isObject() && EmulatesUndefined(&rval.toObject()));
        return true;
    }

    // Step 5.
    if (rval.isNullOrUndefined()) {
        MOZ_ASSERT(!lval.isNullOrUndefined());
        *result = lval.isObject() && EmulatesUndefined(&lval.toObject());
        return true;
    }

    // Step 6.
    if (lval.isNumber() && rval.isString()) {
        double num;
        if (!StringToNumber(cx, rval.toString(), &num))
            return false;
        *result = (lval.toNumber() == num);
        return true;
    }

    // Step 7.
    if (lval.isString() && rval.isNumber()) {
        double num;
        if (!StringToNumber(cx, lval.toString(), &num))
            return false;
        *result = (num == rval.toNumber());
        return true;
    }

    // Step 8.
    if (lval.isBoolean())
        return LooselyEqualBooleanAndOther(cx, lval, rval, result);

    // Step 9.
    if (rval.isBoolean())
        return LooselyEqualBooleanAndOther(cx, rval, lval, result);

    // Step 10.
    if ((lval.isString() || lval.isNumber() || lval.isSymbol()) && rval.isObject()) {
        RootedValue rvalue(cx, rval);
        if (!ToPrimitive(cx, &rvalue))
            return false;
        return LooselyEqual(cx, lval, rvalue, result);
    }

    // Step 11.
    if (lval.isObject() && (rval.isString() || rval.isNumber() || rval.isSymbol())) {
        RootedValue lvalue(cx, lval);
        if (!ToPrimitive(cx, &lvalue))
            return false;
        return LooselyEqual(cx, lvalue, rval, result);
    }

    // Step 12.
    *result = false;
    return true;
}

bool
js::StrictlyEqual(JSContext* cx, HandleValue lval, HandleValue rval, bool* equal)
{
    if (SameType(lval, rval))
        return EqualGivenSameType(cx, lval, rval, equal);

    if (lval.isNumber() && rval.isNumber()) {
        *equal = (lval.toNumber() == rval.toNumber());
        return true;
    }

    *equal = false;
    return true;
}

static inline bool
IsNegativeZero(const Value& v)
{
    return v.isDouble() && mozilla::IsNegativeZero(v.toDouble());
}

static inline bool
IsNaN(const Value& v)
{
    return v.isDouble() && mozilla::IsNaN(v.toDouble());
}

bool
js::SameValue(JSContext* cx, HandleValue v1, HandleValue v2, bool* same)
{
    if (IsNegativeZero(v1)) {
        *same = IsNegativeZero(v2);
        return true;
    }
    if (IsNegativeZero(v2)) {
        *same = false;
        return true;
    }
    if (IsNaN(v1) && IsNaN(v2)) {
        *same = true;
        return true;
    }
    return StrictlyEqual(cx, v1, v2, same);
}

JSType
js::TypeOfObject(JSObject* obj)
{
    if (EmulatesUndefined(obj))
        return JSTYPE_UNDEFINED;
    if (obj->isCallable())
        return JSTYPE_FUNCTION;
    return JSTYPE_OBJECT;
}

JSType
js::TypeOfValue(const Value& v)
{
    if (v.isNumber())
        return JSTYPE_NUMBER;
    if (v.isString())
        return JSTYPE_STRING;
    if (v.isNull())
        return JSTYPE_OBJECT;
    if (v.isUndefined())
        return JSTYPE_UNDEFINED;
    if (v.isObject())
        return TypeOfObject(&v.toObject());
    if (v.isBoolean())
        return JSTYPE_BOOLEAN;
    MOZ_ASSERT(v.isSymbol());
    return JSTYPE_SYMBOL;
}

bool
js::CheckClassHeritageOperation(JSContext* cx, HandleValue heritage)
{
    if (IsConstructor(heritage))
        return true;

    if (heritage.isNull())
        return true;

    if (heritage.isObject()) {
        ReportIsNotFunction(cx, heritage, 0, CONSTRUCT);
        return false;
    }

    ReportValueError2(cx, JSMSG_BAD_HERITAGE, -1, heritage, nullptr, "not an object or null");
    return false;
}

JSObject*
js::ObjectWithProtoOperation(JSContext* cx, HandleValue val)
{
    if (!val.isObjectOrNull()) {
        ReportValueError(cx, JSMSG_NOT_OBJORNULL, -1, val, nullptr);
        return nullptr;
    }

    RootedObject proto(cx, val.toObjectOrNull());
    return NewObjectWithGivenProto<PlainObject>(cx, proto);
}

JSObject*
js::FunWithProtoOperation(JSContext* cx, HandleFunction fun, HandleObject parent,
                          HandleObject proto)
{
    return CloneFunctionObjectIfNotSingleton(cx, fun, parent, proto);
}

/*
 * Enter the new with environment using an object at sp[-1] and associate the
 * depth of the with block with sp + stackIndex.
 */
bool
js::EnterWithOperation(JSContext* cx, AbstractFramePtr frame, HandleValue val,
                       Handle<WithScope*> scope)
{
    RootedObject obj(cx);
    if (val.isObject()) {
        obj = &val.toObject();
    } else {
        obj = ToObject(cx, val);
        if (!obj)
            return false;
    }

    RootedObject envChain(cx, frame.environmentChain());
    WithEnvironmentObject* withobj = WithEnvironmentObject::create(cx, obj, envChain, scope);
    if (!withobj)
        return false;

    frame.pushOnEnvironmentChain(*withobj);
    return true;
}

static void
PopEnvironment(JSContext* cx, EnvironmentIter& ei)
{
    switch (ei.scope().kind()) {
      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
        if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
            DebugEnvironments::onPopLexical(cx, ei);
        if (ei.scope().hasEnvironment())
            ei.initialFrame().popOffEnvironmentChain<LexicalEnvironmentObject>();
        break;
      case ScopeKind::With:
        if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
            DebugEnvironments::onPopWith(ei.initialFrame());
        ei.initialFrame().popOffEnvironmentChain<WithEnvironmentObject>();
        break;
      case ScopeKind::Function:
        if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
            DebugEnvironments::onPopCall(cx, ei.initialFrame());
        if (ei.scope().hasEnvironment())
            ei.initialFrame().popOffEnvironmentChain<CallObject>();
        break;
      case ScopeKind::FunctionBodyVar:
      case ScopeKind::ParameterExpressionVar:
      case ScopeKind::StrictEval:
        if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
            DebugEnvironments::onPopVar(cx, ei);
        if (ei.scope().hasEnvironment())
            ei.initialFrame().popOffEnvironmentChain<VarEnvironmentObject>();
        break;
      case ScopeKind::Eval:
      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
      case ScopeKind::Module:
        break;
      case ScopeKind::WasmInstance:
      case ScopeKind::WasmFunction:
        MOZ_CRASH("wasm is not interpreted");
        break;
    }
}

// Unwind environment chain and iterator to match the env corresponding to
// the given bytecode position.
void
js::UnwindEnvironment(JSContext* cx, EnvironmentIter& ei, jsbytecode* pc)
{
    if (!ei.withinInitialFrame())
        return;

    RootedScope scope(cx, ei.initialFrame().script()->innermostScope(pc));

#ifdef DEBUG
    // A frame's environment chain cannot be unwound to anything enclosing the
    // body scope of a script.  This includes the parameter defaults
    // environment and the decl env object. These environments, once pushed
    // onto the environment chain, are expected to be there for the duration
    // of the frame.
    //
    // Attempting to unwind to the parameter defaults code in a script is a
    // bug; that section of code has no try-catch blocks.
    JSScript* script = ei.initialFrame().script();
    for (uint32_t i = 0; i < script->bodyScopeIndex(); i++)
        MOZ_ASSERT(scope != script->getScope(i));
#endif

    for (; ei.maybeScope() != scope; ei++)
        PopEnvironment(cx, ei);
}

// Unwind all environments. This is needed because block scopes may cover the
// first bytecode at a script's main(). e.g.,
//
//     function f() { { let i = 0; } }
//
// will have no pc location distinguishing the first block scope from the
// outermost function scope.
void
js::UnwindAllEnvironmentsInFrame(JSContext* cx, EnvironmentIter& ei)
{
    for (; ei.withinInitialFrame(); ei++)
        PopEnvironment(cx, ei);
}

// Compute the pc needed to unwind the environment to the beginning of a try
// block. We cannot unwind to *after* the JSOP_TRY, because that might be the
// first opcode of an inner scope, with the same problem as above. e.g.,
//
// try { { let x; } }
//
// will have no pc location distinguishing the try block scope from the inner
// let block scope.
jsbytecode*
js::UnwindEnvironmentToTryPc(JSScript* script, JSTryNote* tn)
{
    jsbytecode* pc = script->main() + tn->start;
    if (tn->kind == JSTRY_CATCH || tn->kind == JSTRY_FINALLY) {
        pc -= JSOP_TRY_LENGTH;
        MOZ_ASSERT(*pc == JSOP_TRY);
    } else if (tn->kind == JSTRY_DESTRUCTURING_ITERCLOSE) {
        pc -= JSOP_TRY_DESTRUCTURING_ITERCLOSE_LENGTH;
        MOZ_ASSERT(*pc == JSOP_TRY_DESTRUCTURING_ITERCLOSE);
    }
    return pc;
}

static bool
ForcedReturn(JSContext* cx, InterpreterRegs& regs)
{
    bool ok = Debugger::onLeaveFrame(cx, regs.fp(), regs.pc, true);
    // Point the frame to the end of the script, regardless of error. The
    // caller must jump to the correct continuation depending on 'ok'.
    regs.setToEndOfScript();
    return ok;
}

static void
SettleOnTryNote(JSContext* cx, JSTryNote* tn, EnvironmentIter& ei, InterpreterRegs& regs)
{
    // Unwind the environment to the beginning of the JSOP_TRY.
    UnwindEnvironment(cx, ei, UnwindEnvironmentToTryPc(regs.fp()->script(), tn));

    // Set pc to the first bytecode after the the try note to point
    // to the beginning of catch or finally.
    regs.pc = regs.fp()->script()->main() + tn->start + tn->length;
    regs.sp = regs.spForStackDepth(tn->stackDepth);
}

class InterpreterFrameStackDepthOp
{
    const InterpreterRegs& regs_;
  public:
    explicit InterpreterFrameStackDepthOp(const InterpreterRegs& regs)
      : regs_(regs)
    { }
    uint32_t operator()() { return regs_.stackDepth(); }
};

class TryNoteIterInterpreter : public TryNoteIter<InterpreterFrameStackDepthOp>
{
  public:
    TryNoteIterInterpreter(JSContext* cx, const InterpreterRegs& regs)
      : TryNoteIter(cx, regs.fp()->script(), regs.pc, InterpreterFrameStackDepthOp(regs))
    { }
};

static void
UnwindIteratorsForUncatchableException(JSContext* cx, const InterpreterRegs& regs)
{
    // c.f. the regular (catchable) TryNoteIterInterpreter loop in
    // ProcessTryNotes.
    bool inForOfIterClose = false;
    for (TryNoteIterInterpreter tni(cx, regs); !tni.done(); ++tni) {
        JSTryNote* tn = *tni;
        switch (tn->kind) {
          case JSTRY_FOR_IN: {
            // See corresponding comment in ProcessTryNotes.
            if (inForOfIterClose)
                break;

            Value* sp = regs.spForStackDepth(tn->stackDepth);
            UnwindIteratorForUncatchableException(&sp[-1].toObject());
            break;
          }

          case JSTRY_FOR_OF_ITERCLOSE:
            inForOfIterClose = true;
            break;

          case JSTRY_FOR_OF:
            inForOfIterClose = false;
            break;

          default:
            break;
        }
    }
}

enum HandleErrorContinuation
{
    SuccessfulReturnContinuation,
    ErrorReturnContinuation,
    CatchContinuation,
    FinallyContinuation
};

static HandleErrorContinuation
ProcessTryNotes(JSContext* cx, EnvironmentIter& ei, InterpreterRegs& regs)
{
    bool inForOfIterClose = false;
    for (TryNoteIterInterpreter tni(cx, regs); !tni.done(); ++tni) {
        JSTryNote* tn = *tni;

        switch (tn->kind) {
          case JSTRY_CATCH:
            /* Catch cannot intercept the closing of a generator. */
            if (cx->isClosingGenerator())
                break;

            // If IteratorClose due to abnormal completion threw inside a
            // for-of loop, it is not catchable by try statements inside of
            // the for-of loop.
            //
            // This is handled by this weirdness in the exception handler
            // instead of in bytecode because it is hard to do so in bytecode:
            //
            //   1. IteratorClose emitted due to abnormal completion (break,
            //   throw, return) are emitted inline, at the source location of
            //   the break, throw, or return statement. For example:
            //
            //     for (x of iter) {
            //       try { return; } catch (e) { }
            //     }
            //
            //   From the try-note nesting's perspective, the IteratorClose
            //   resulting from |return| is covered by the inner try, when it
            //   should not be.
            //
            //   2. Try-catch notes cannot be disjoint. That is, we can't have
            //   multiple notes with disjoint pc ranges jumping to the same
            //   catch block.
            if (inForOfIterClose)
                break;
            SettleOnTryNote(cx, tn, ei, regs);
            return CatchContinuation;

          case JSTRY_FINALLY:
            // See note above.
            if (inForOfIterClose)
                break;
            SettleOnTryNote(cx, tn, ei, regs);
            return FinallyContinuation;

          case JSTRY_FOR_IN: {
            // Don't let (extra) values pushed on the stack while closing a
            // for-of iterator confuse us into thinking we still have to close
            // an inner for-in iterator.
            if (inForOfIterClose)
                break;

            /* This is similar to JSOP_ENDITER in the interpreter loop. */
            DebugOnly<jsbytecode*> pc = regs.fp()->script()->main() + tn->start + tn->length;
            MOZ_ASSERT(JSOp(*pc) == JSOP_ENDITER);
            Value* sp = regs.spForStackDepth(tn->stackDepth);
            JSObject* obj = &sp[-1].toObject();
            CloseIterator(obj);
            break;
          }

          case JSTRY_DESTRUCTURING_ITERCLOSE: {
            // See note above.
            if (inForOfIterClose)
                break;

            // Whether the destructuring iterator is done is at the top of the
            // stack. The iterator object is second from the top.
            MOZ_ASSERT(tn->stackDepth > 1);
            Value* sp = regs.spForStackDepth(tn->stackDepth);
            RootedValue doneValue(cx, sp[-1]);
            bool done = ToBoolean(doneValue);
            if (!done) {
                RootedObject iterObject(cx, &sp[-2].toObject());
                if (!IteratorCloseForException(cx, iterObject)) {
                    SettleOnTryNote(cx, tn, ei, regs);
                    return ErrorReturnContinuation;
                }
            }
            break;
          }

          case JSTRY_FOR_OF_ITERCLOSE:
            inForOfIterClose = true;
            break;

          case JSTRY_FOR_OF:
            inForOfIterClose = false;
            break;

          case JSTRY_LOOP:
            break;

          default:
            MOZ_CRASH("Invalid try note");
        }
    }

    return SuccessfulReturnContinuation;
}

bool
js::HandleClosingGeneratorReturn(JSContext* cx, AbstractFramePtr frame, bool ok)
{
    /*
     * Propagate the exception or error to the caller unless the exception
     * is an asynchronous return from a generator.
     */
    if (cx->isClosingGenerator()) {
        cx->clearPendingException();
        ok = true;
        SetGeneratorClosed(cx, frame);
    }
    return ok;
}

static HandleErrorContinuation
HandleError(JSContext* cx, InterpreterRegs& regs)
{
    MOZ_ASSERT(regs.fp()->script()->containsPC(regs.pc));

    if (regs.fp()->script()->hasScriptCounts()) {
        PCCounts* counts = regs.fp()->script()->getThrowCounts(regs.pc);
        // If we failed to allocate, then skip the increment and continue to
        // handle the exception.
        if (counts)
            counts->numExec()++;
    }

    EnvironmentIter ei(cx, regs.fp(), regs.pc);
    bool ok = false;

  again:
    if (cx->isExceptionPending()) {
        /* Call debugger throw hooks. */
        if (!cx->isClosingGenerator()) {
            JSTrapStatus status = Debugger::onExceptionUnwind(cx, regs.fp());
            switch (status) {
              case JSTRAP_ERROR:
                goto again;

              case JSTRAP_CONTINUE:
              case JSTRAP_THROW:
                break;

              case JSTRAP_RETURN:
                UnwindIteratorsForUncatchableException(cx, regs);
                if (!ForcedReturn(cx, regs))
                    return ErrorReturnContinuation;
                return SuccessfulReturnContinuation;

              default:
                MOZ_CRASH("Bad Debugger::onExceptionUnwind status");
            }
        }

        HandleErrorContinuation res = ProcessTryNotes(cx, ei, regs);
        switch (res) {
          case SuccessfulReturnContinuation:
            break;
          case ErrorReturnContinuation:
            goto again;
          case CatchContinuation:
          case FinallyContinuation:
            // No need to increment the PCCounts number of execution here, as
            // the interpreter increments any PCCounts if present.
            MOZ_ASSERT_IF(regs.fp()->script()->hasScriptCounts(),
                          regs.fp()->script()->maybeGetPCCounts(regs.pc));
            return res;
        }

        ok = HandleClosingGeneratorReturn(cx, regs.fp(), ok);
        ok = Debugger::onLeaveFrame(cx, regs.fp(), regs.pc, ok);
    } else {
        // We may be propagating a forced return from the interrupt
        // callback, which cannot easily force a return.
        if (MOZ_UNLIKELY(cx->isPropagatingForcedReturn())) {
            cx->clearPropagatingForcedReturn();
            if (!ForcedReturn(cx, regs))
                return ErrorReturnContinuation;
            return SuccessfulReturnContinuation;
        }

        UnwindIteratorsForUncatchableException(cx, regs);
    }

    // After this point, we will pop the frame regardless. Settle the frame on
    // the end of the script.
    regs.setToEndOfScript();

    return ok ? SuccessfulReturnContinuation : ErrorReturnContinuation;
}

#define REGS                     (activation.regs())
#define PUSH_COPY(v)             do { *REGS.sp++ = (v); assertSameCompartmentDebugOnly(cx, REGS.sp[-1]); } while (0)
#define PUSH_COPY_SKIP_CHECK(v)  *REGS.sp++ = (v)
#define PUSH_NULL()              REGS.sp++->setNull()
#define PUSH_UNDEFINED()         REGS.sp++->setUndefined()
#define PUSH_BOOLEAN(b)          REGS.sp++->setBoolean(b)
#define PUSH_DOUBLE(d)           REGS.sp++->setDouble(d)
#define PUSH_INT32(i)            REGS.sp++->setInt32(i)
#define PUSH_SYMBOL(s)           REGS.sp++->setSymbol(s)
#define PUSH_STRING(s)           do { REGS.sp++->setString(s); assertSameCompartmentDebugOnly(cx, REGS.sp[-1]); } while (0)
#define PUSH_OBJECT(obj)         do { REGS.sp++->setObject(obj); assertSameCompartmentDebugOnly(cx, REGS.sp[-1]); } while (0)
#define PUSH_OBJECT_OR_NULL(obj) do { REGS.sp++->setObjectOrNull(obj); assertSameCompartmentDebugOnly(cx, REGS.sp[-1]); } while (0)
#define PUSH_MAGIC(magic)        REGS.sp++->setMagic(magic)
#define POP_COPY_TO(v)           (v) = *--REGS.sp
#define POP_RETURN_VALUE()       REGS.fp()->setReturnValue(*--REGS.sp)

#define FETCH_OBJECT(cx, n, obj)                                              \
    JS_BEGIN_MACRO                                                            \
        HandleValue val = REGS.stackHandleAt(n);                              \
        obj = ToObjectFromStack((cx), (val));                                 \
        if (!obj)                                                             \
            goto error;                                                       \
    JS_END_MACRO

/*
 * Same for JSOP_SETNAME and JSOP_SETPROP, which differ only slightly but
 * remain distinct for the decompiler.
 */
JS_STATIC_ASSERT(JSOP_SETNAME_LENGTH == JSOP_SETPROP_LENGTH);

/* See TRY_BRANCH_AFTER_COND. */
JS_STATIC_ASSERT(JSOP_IFNE_LENGTH == JSOP_IFEQ_LENGTH);
JS_STATIC_ASSERT(JSOP_IFNE == JSOP_IFEQ + 1);

/*
 * Compute the implicit |this| value used by a call expression with an
 * unqualified name reference. The environment the binding was found on is
 * passed as argument, env.
 *
 * The implicit |this| is |undefined| for all environment types except
 * WithEnvironmentObject. This is the case for |with(...) {...}| expressions or
 * if the embedding uses a non-syntactic WithEnvironmentObject.
 *
 * NOTE: A non-syntactic WithEnvironmentObject may have a corresponding
 * extensible LexicalEnviornmentObject, but it will not be considered as an
 * implicit |this|. This is for compatibility with the Gecko subscript loader.
 */
static inline Value
ComputeImplicitThis(JSObject* env)
{
    // Fast-path for GlobalObject
    if (env->is<GlobalObject>())
        return UndefinedValue();

    // WithEnvironmentObjects have an actual implicit |this|
    if (env->is<WithEnvironmentObject>())
        return GetThisValueOfWith(env);

    // Debugger environments need special casing, as despite being
    // non-syntactic, they wrap syntactic environments and should not be
    // treated like other embedding-specific non-syntactic environments.
    if (env->is<DebugEnvironmentProxy>())
        return ComputeImplicitThis(&env->as<DebugEnvironmentProxy>().environment());

    MOZ_ASSERT(env->is<EnvironmentObject>());
    return UndefinedValue();
}

static MOZ_ALWAYS_INLINE bool
AddOperation(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res)
{
    if (lhs.isInt32() && rhs.isInt32()) {
        int32_t l = lhs.toInt32(), r = rhs.toInt32();
        int32_t t;
        if (MOZ_LIKELY(SafeAdd(l, r, &t))) {
            res.setInt32(t);
            return true;
        }
    }

    if (!ToPrimitive(cx, lhs))
        return false;
    if (!ToPrimitive(cx, rhs))
        return false;

    bool lIsString, rIsString;
    if ((lIsString = lhs.isString()) | (rIsString = rhs.isString())) {
        JSString* lstr;
        if (lIsString) {
            lstr = lhs.toString();
        } else {
            lstr = ToString<CanGC>(cx, lhs);
            if (!lstr)
                return false;
        }

        JSString* rstr;
        if (rIsString) {
            rstr = rhs.toString();
        } else {
            // Save/restore lstr in case of GC activity under ToString.
            lhs.setString(lstr);
            rstr = ToString<CanGC>(cx, rhs);
            if (!rstr)
                return false;
            lstr = lhs.toString();
        }
        JSString* str = ConcatStrings<NoGC>(cx, lstr, rstr);
        if (!str) {
            RootedString nlstr(cx, lstr), nrstr(cx, rstr);
            str = ConcatStrings<CanGC>(cx, nlstr, nrstr);
            if (!str)
                return false;
        }
        res.setString(str);
    } else {
        double l, r;
        if (!ToNumber(cx, lhs, &l) || !ToNumber(cx, rhs, &r))
            return false;
        res.setNumber(l + r);
    }

    return true;
}

static MOZ_ALWAYS_INLINE bool
SubOperation(JSContext* cx, HandleValue lhs, HandleValue rhs, MutableHandleValue res)
{
    double d1, d2;
    if (!ToNumber(cx, lhs, &d1) || !ToNumber(cx, rhs, &d2))
        return false;
    res.setNumber(d1 - d2);
    return true;
}

static MOZ_ALWAYS_INLINE bool
MulOperation(JSContext* cx, HandleValue lhs, HandleValue rhs, MutableHandleValue res)
{
    double d1, d2;
    if (!ToNumber(cx, lhs, &d1) || !ToNumber(cx, rhs, &d2))
        return false;
    res.setNumber(d1 * d2);
    return true;
}

static MOZ_ALWAYS_INLINE bool
DivOperation(JSContext* cx, HandleValue lhs, HandleValue rhs, MutableHandleValue res)
{
    double d1, d2;
    if (!ToNumber(cx, lhs, &d1) || !ToNumber(cx, rhs, &d2))
        return false;
    res.setNumber(NumberDiv(d1, d2));
    return true;
}

static MOZ_ALWAYS_INLINE bool
ModOperation(JSContext* cx, HandleValue lhs, HandleValue rhs, MutableHandleValue res)
{
    int32_t l, r;
    if (lhs.isInt32() && rhs.isInt32() &&
        (l = lhs.toInt32()) >= 0 && (r = rhs.toInt32()) > 0) {
        int32_t mod = l % r;
        res.setInt32(mod);
        return true;
    }

    double d1, d2;
    if (!ToNumber(cx, lhs, &d1) || !ToNumber(cx, rhs, &d2))
        return false;

    res.setNumber(NumberMod(d1, d2));
    return true;
}

static MOZ_ALWAYS_INLINE bool
SetObjectElementOperation(JSContext* cx, HandleObject obj, HandleId id, HandleValue value,
                          HandleValue receiver, bool strict,
                          JSScript* script = nullptr, jsbytecode* pc = nullptr)
{
    // receiver != obj happens only at super[expr], where we expect to find the property
    // People probably aren't building hashtables with |super| anyway.
    TypeScript::MonitorAssign(cx, obj, id);

    if (obj->isNative() && JSID_IS_INT(id)) {
        uint32_t length = obj->as<NativeObject>().getDenseInitializedLength();
        int32_t i = JSID_TO_INT(id);
        if ((uint32_t)i >= length) {
            // Annotate script if provided with information (e.g. baseline)
            if (script && script->hasBaselineScript() && IsSetElemPC(pc))
                script->baselineScript()->noteHasDenseAdd(script->pcToOffset(pc));
        }
    }

    // Set the HadElementsAccess flag on the object if needed. This flag is
    // used to do more eager dictionary-mode conversion for objects that are
    // used as hashmaps. Set this flag only for objects with many properties,
    // to avoid unnecessary Shape changes.
    if (obj->isNative() &&
        JSID_IS_ATOM(id) &&
        !obj->as<NativeObject>().inDictionaryMode() &&
        !obj->as<NativeObject>().hadElementsAccess() &&
        obj->as<NativeObject>().slotSpan() > PropertyTree::MAX_HEIGHT_WITH_ELEMENTS_ACCESS / 3)
    {
        if (!NativeObject::setHadElementsAccess(cx, obj.as<NativeObject>()))
            return false;
    }

    ObjectOpResult result;
    return SetProperty(cx, obj, id, value, receiver, result) &&
           result.checkStrictErrorOrWarning(cx, obj, id, strict);
}

/*
 * Get the innermost enclosing function that has a 'this' binding.
 *
 * Implements ES6 12.3.5.2 GetSuperConstructor() steps 1-3, including
 * the loop in ES6 8.3.2 GetThisEnvironment(). Our implementation of
 * ES6 12.3.5.3 MakeSuperPropertyReference() also uses this code.
 */
static JSFunction&
GetSuperEnvFunction(JSContext* cx, InterpreterRegs& regs)
{
    JSObject* env = regs.fp()->environmentChain();
    Scope* scope = regs.fp()->script()->innermostScope(regs.pc);
    for (EnvironmentIter ei(cx, env, scope); ei; ei++) {
        if (ei.hasSyntacticEnvironment() && ei.scope().is<FunctionScope>()) {
            JSFunction& callee = ei.environment().as<CallObject>().callee();

            // Arrow functions don't have the information we're looking for,
            // their enclosing scopes do. Nevertheless, they might have call
            // objects. Skip them to find what we came for.
            if (callee.isArrow())
                continue;

            return callee;
        }
    }
    MOZ_CRASH("unexpected env chain for GetSuperEnvFunction");
}


/*
 * As an optimization, the interpreter creates a handful of reserved Rooted<T>
 * variables at the beginning, thus inserting them into the Rooted list once
 * upon entry. ReservedRooted "borrows" a reserved Rooted variable and uses it
 * within a local scope, resetting the value to nullptr (or the appropriate
 * equivalent for T) at scope end. This avoids inserting/removing the Rooted
 * from the rooter list, while preventing stale values from being kept alive
 * unnecessarily.
 */

template<typename T>
class ReservedRooted : public RootedBase<T, ReservedRooted<T>>
{
    Rooted<T>* savedRoot;

  public:
    ReservedRooted(Rooted<T>* root, const T& ptr) : savedRoot(root) {
        *root = ptr;
    }

    explicit ReservedRooted(Rooted<T>* root) : savedRoot(root) {
        *root = JS::GCPolicy<T>::initial();
    }

    ~ReservedRooted() {
        *savedRoot = JS::GCPolicy<T>::initial();
    }

    void set(const T& p) const { *savedRoot = p; }
    operator Handle<T>() { return *savedRoot; }
    operator Rooted<T>&() { return *savedRoot; }
    MutableHandle<T> operator&() { return &*savedRoot; }

    DECLARE_NONPOINTER_ACCESSOR_METHODS(savedRoot->get())
    DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(savedRoot->get())
    DECLARE_POINTER_CONSTREF_OPS(T)
    DECLARE_POINTER_ASSIGN_OPS(ReservedRooted, T)
};

void
js::ReportInNotObjectError(JSContext* cx, HandleValue lref, int lindex,
                           HandleValue rref, int rindex)
{
    auto uniqueCharsFromString = [](JSContext* cx, HandleValue ref) -> UniqueChars {
        static const size_t MaxStringLength = 16;
        RootedString str(cx, ref.toString());
        if (str->length() > MaxStringLength) {
            StringBuffer buf(cx);
            if (!buf.appendSubstring(str, 0, MaxStringLength))
                return nullptr;
            if (!buf.append("..."))
                return nullptr;
            str = buf.finishString();
            if (!str)
                return nullptr;
        }
        return UniqueChars(JS_EncodeString(cx, str));
    };

    if (lref.isString() && rref.isString()) {
        UniqueChars lbytes = uniqueCharsFromString(cx, lref);
        if (!lbytes)
            return;
        UniqueChars rbytes = uniqueCharsFromString(cx, rref);
        if (!rbytes)
            return;
        JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_IN_STRING,
                                   lbytes.get(), rbytes.get());
        return;
    }

    JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_IN_NOT_OBJECT,
                               InformalValueTypeName(rref));
}

static MOZ_NEVER_INLINE bool
Interpret(JSContext* cx, RunState& state)
{
/*
 * Define macros for an interpreter loop. Opcode dispatch may be either by a
 * switch statement or by indirect goto (aka a threaded interpreter), depending
 * on compiler support.
 *
 * Threaded interpretation appears to be well-supported by GCC 3 and higher.
 * IBM's C compiler when run with the right options (e.g., -qlanglvl=extended)
 * also supports threading. Ditto the SunPro C compiler.
 */
#if (defined(__GNUC__) ||                                                         \
     (__IBMC__ >= 700 && defined __IBM_COMPUTED_GOTO) ||                      \
     __SUNPRO_C >= 0x570)
// Non-standard but faster indirect-goto-based dispatch.
# define INTERPRETER_LOOP()
# define CASE(OP)                 label_##OP:
# define DEFAULT()                label_default:
# define DISPATCH_TO(OP)          goto* addresses[(OP)]

# define LABEL(X)                 (&&label_##X)

    // Use addresses instead of offsets to optimize for runtime speed over
    // load-time relocation overhead.
    static const void* const addresses[EnableInterruptsPseudoOpcode + 1] = {
# define OPCODE_LABEL(op, ...)  LABEL(op),
        FOR_EACH_OPCODE(OPCODE_LABEL)
# undef OPCODE_LABEL
# define TRAILING_LABEL(v)                                                    \
    ((v) == EnableInterruptsPseudoOpcode                                      \
     ? LABEL(EnableInterruptsPseudoOpcode)                                    \
     : LABEL(default)),
        FOR_EACH_TRAILING_UNUSED_OPCODE(TRAILING_LABEL)
# undef TRAILING_LABEL
    };
#else
// Portable switch-based dispatch.
# define INTERPRETER_LOOP()       the_switch: switch (switchOp)
# define CASE(OP)                 case OP:
# define DEFAULT()                default:
# define DISPATCH_TO(OP)                                                      \
    JS_BEGIN_MACRO                                                            \
        switchOp = (OP);                                                      \
        goto the_switch;                                                      \
    JS_END_MACRO

    // This variable is effectively a parameter to the_switch.
    jsbytecode switchOp;
#endif

    /*
     * Increment REGS.pc by N, load the opcode at that position,
     * and jump to the code to execute it.
     *
     * When Debugger puts a script in single-step mode, all js::Interpret
     * invocations that might be presently running that script must have
     * interrupts enabled. It's not practical to simply check
     * script->stepModeEnabled() at each point some callee could have changed
     * it, because there are so many places js::Interpret could possibly cause
     * JavaScript to run: each place an object might be coerced to a primitive
     * or a number, for example. So instead, we expose a simple mechanism to
     * let Debugger tweak the affected js::Interpret frames when an onStep
     * handler is added: calling activation.enableInterruptsUnconditionally()
     * will enable interrupts, and activation.opMask() is or'd with the opcode
     * to implement a simple alternate dispatch.
     */
#define ADVANCE_AND_DISPATCH(N)                                               \
    JS_BEGIN_MACRO                                                            \
        REGS.pc += (N);                                                       \
        SANITY_CHECKS();                                                      \
        DISPATCH_TO(*REGS.pc | activation.opMask());                          \
    JS_END_MACRO

   /*
    * Shorthand for the common sequence at the end of a fixed-size opcode.
    */
#define END_CASE(OP)              ADVANCE_AND_DISPATCH(OP##_LENGTH);

    /*
     * Prepare to call a user-supplied branch handler, and abort the script
     * if it returns false.
     */
#define CHECK_BRANCH()                                                        \
    JS_BEGIN_MACRO                                                            \
        if (!CheckForInterrupt(cx))                                           \
            goto error;                                                       \
    JS_END_MACRO

    /*
     * This is a simple wrapper around ADVANCE_AND_DISPATCH which also does
     * a CHECK_BRANCH() if n is not positive, which possibly indicates that it
     * is the backedge of a loop.
     */
#define BRANCH(n)                                                             \
    JS_BEGIN_MACRO                                                            \
        int32_t nlen = (n);                                                   \
        if (nlen <= 0)                                                        \
            CHECK_BRANCH();                                                   \
        ADVANCE_AND_DISPATCH(nlen);                                           \
    JS_END_MACRO

    /*
     * Initialize code coverage vectors.
     */
#define INIT_COVERAGE()                                                       \
    JS_BEGIN_MACRO                                                            \
        if (!script->hasScriptCounts()) {                                     \
            if (cx->compartment()->collectCoverageForDebug()) {               \
                if (!script->initScriptCounts(cx))                            \
                    goto error;                                               \
            }                                                                 \
        }                                                                     \
    JS_END_MACRO

    /*
     * Increment the code coverage counter associated with the given pc.
     */
#define COUNT_COVERAGE_PC(PC)                                                 \
    JS_BEGIN_MACRO                                                            \
        if (script->hasScriptCounts()) {                                      \
            PCCounts* counts = script->maybeGetPCCounts(PC);                  \
            MOZ_ASSERT(counts);                                               \
            counts->numExec()++;                                              \
        }                                                                     \
    JS_END_MACRO

#define COUNT_COVERAGE_MAIN()                                                 \
    JS_BEGIN_MACRO                                                            \
        jsbytecode* main = script->main();                                    \
        if (!BytecodeIsJumpTarget(JSOp(*main)))                               \
            COUNT_COVERAGE_PC(main);                                          \
    JS_END_MACRO

#define COUNT_COVERAGE()                                                      \
    JS_BEGIN_MACRO                                                            \
        MOZ_ASSERT(BytecodeIsJumpTarget(JSOp(*REGS.pc)));                     \
        COUNT_COVERAGE_PC(REGS.pc);                                           \
    JS_END_MACRO

#define LOAD_DOUBLE(PCOFF, dbl)                                               \
    ((dbl) = script->getConst(GET_UINT32_INDEX(REGS.pc + (PCOFF))).toDouble())

#define SET_SCRIPT(s)                                                         \
    JS_BEGIN_MACRO                                                            \
        script = (s);                                                         \
        if (script->hasAnyBreakpointsOrStepMode() || script->hasScriptCounts()) \
            activation.enableInterruptsUnconditionally();                     \
    JS_END_MACRO

#define SANITY_CHECKS()                                                       \
    JS_BEGIN_MACRO                                                            \
        js::gc::MaybeVerifyBarriers(cx);                                      \
    JS_END_MACRO

    gc::MaybeVerifyBarriers(cx, true);
    MOZ_ASSERT(!cx->zone()->types.activeAnalysis);

    InterpreterFrame* entryFrame = state.pushInterpreterFrame(cx);
    if (!entryFrame)
        return false;

    ActivationEntryMonitor entryMonitor(cx, entryFrame);
    InterpreterActivation activation(state, cx, entryFrame);

    /* The script is used frequently, so keep a local copy. */
    RootedScript script(cx);
    SET_SCRIPT(REGS.fp()->script());

    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
    TraceLoggerEvent scriptEvent(TraceLogger_Scripts, script);
    TraceLogStartEvent(logger, scriptEvent);
    TraceLogStartEvent(logger, TraceLogger_Interpreter);

    /*
     * Pool of rooters for use in this interpreter frame. References to these
     * are used for local variables within interpreter cases. This avoids
     * creating new rooters each time an interpreter case is entered, and also
     * correctness pitfalls due to incorrect compilation of destructor calls
     * around computed gotos.
     */
    RootedValue rootValue0(cx), rootValue1(cx);
    RootedString rootString0(cx), rootString1(cx);
    RootedObject rootObject0(cx), rootObject1(cx), rootObject2(cx);
    RootedNativeObject rootNativeObject0(cx);
    RootedFunction rootFunction0(cx);
    RootedPropertyName rootName0(cx);
    RootedId rootId0(cx);
    RootedShape rootShape0(cx);
    RootedScript rootScript0(cx);
    Rooted<Scope*> rootScope0(cx);
    DebugOnly<uint32_t> blockDepth;

    /* State communicated between non-local jumps: */
    bool interpReturnOK;
    bool frameHalfInitialized;

    if (!activation.entryFrame()->prologue(cx))
        goto prologue_error;

    switch (Debugger::onEnterFrame(cx, activation.entryFrame())) {
      case JSTRAP_CONTINUE:
        break;
      case JSTRAP_RETURN:
        if (!ForcedReturn(cx, REGS))
            goto error;
        goto successful_return_continuation;
      case JSTRAP_THROW:
      case JSTRAP_ERROR:
        goto error;
      default:
        MOZ_CRASH("bad Debugger::onEnterFrame status");
    }

    // Increment the coverage for the main entry point.
    INIT_COVERAGE();
    COUNT_COVERAGE_MAIN();

    // Enter the interpreter loop starting at the current pc.
    ADVANCE_AND_DISPATCH(0);

INTERPRETER_LOOP() {

CASE(EnableInterruptsPseudoOpcode)
{
    bool moreInterrupts = false;
    jsbytecode op = *REGS.pc;

    if (!script->hasScriptCounts() && cx->compartment()->collectCoverageForDebug()) {
        if (!script->initScriptCounts(cx))
            goto error;
    }

    if (script->isDebuggee()) {
        if (script->stepModeEnabled()) {
            RootedValue rval(cx);
            JSTrapStatus status = JSTRAP_CONTINUE;
            status = Debugger::onSingleStep(cx, &rval);
            switch (status) {
              case JSTRAP_ERROR:
                goto error;
              case JSTRAP_CONTINUE:
                break;
              case JSTRAP_RETURN:
                REGS.fp()->setReturnValue(rval);
                if (!ForcedReturn(cx, REGS))
                    goto error;
                goto successful_return_continuation;
              case JSTRAP_THROW:
                cx->setPendingException(rval);
                goto error;
              default:;
            }
            moreInterrupts = true;
        }

        if (script->hasAnyBreakpointsOrStepMode())
            moreInterrupts = true;

        if (script->hasBreakpointsAt(REGS.pc)) {
            RootedValue rval(cx);
            JSTrapStatus status = Debugger::onTrap(cx, &rval);
            switch (status) {
              case JSTRAP_ERROR:
                goto error;
              case JSTRAP_RETURN:
                REGS.fp()->setReturnValue(rval);
                if (!ForcedReturn(cx, REGS))
                    goto error;
                goto successful_return_continuation;
              case JSTRAP_THROW:
                cx->setPendingException(rval);
                goto error;
              default:
                break;
            }
            MOZ_ASSERT(status == JSTRAP_CONTINUE);
            MOZ_ASSERT(rval.isInt32() && rval.toInt32() == op);
        }
    }

    MOZ_ASSERT(activation.opMask() == EnableInterruptsPseudoOpcode);
    if (!moreInterrupts)
        activation.clearInterruptsMask();

    /* Commence executing the actual opcode. */
    SANITY_CHECKS();
    DISPATCH_TO(op);
}

/* Various 1-byte no-ops. */
CASE(JSOP_NOP)
CASE(JSOP_NOP_DESTRUCTURING)
CASE(JSOP_TRY_DESTRUCTURING_ITERCLOSE)
CASE(JSOP_UNUSED126)
CASE(JSOP_UNUSED206)
CASE(JSOP_UNUSED223)
CASE(JSOP_CONDSWITCH)
{
    MOZ_ASSERT(CodeSpec[*REGS.pc].length == 1);
    ADVANCE_AND_DISPATCH(1);
}

CASE(JSOP_TRY)
CASE(JSOP_JUMPTARGET)
CASE(JSOP_LOOPHEAD)
{
    MOZ_ASSERT(CodeSpec[*REGS.pc].length == 1);
    COUNT_COVERAGE();
    ADVANCE_AND_DISPATCH(1);
}

CASE(JSOP_LABEL)
END_CASE(JSOP_LABEL)

CASE(JSOP_LOOPENTRY)
    COUNT_COVERAGE();
    // Attempt on-stack replacement with Baseline code.
    if (jit::IsBaselineEnabled(cx)) {
        jit::MethodStatus status = jit::CanEnterBaselineAtBranch(cx, REGS.fp());
        if (status == jit::Method_Error)
            goto error;
        if (status == jit::Method_Compiled) {
            bool wasProfiler = REGS.fp()->hasPushedGeckoProfilerFrame();

            jit::JitExecStatus maybeOsr;
            {
                GeckoProfilerBaselineOSRMarker osr(cx, wasProfiler);
                maybeOsr = jit::EnterBaselineAtBranch(cx, REGS.fp(), REGS.pc);
            }

            // We failed to call into baseline at all, so treat as an error.
            if (maybeOsr == jit::JitExec_Aborted)
                goto error;

            interpReturnOK = (maybeOsr == jit::JitExec_Ok);

            // Pop the profiler frame pushed by the interpreter.  (The compiled
            // version of the function popped a copy of the frame pushed by the
            // OSR trampoline.)
            if (wasProfiler)
                cx->geckoProfiler().exit(script, script->functionNonDelazifying());

            if (activation.entryFrame() != REGS.fp())
                goto jit_return_pop_frame;
            goto leave_on_safe_point;
        }
    }
END_CASE(JSOP_LOOPENTRY)

CASE(JSOP_LINENO)
END_CASE(JSOP_LINENO)

CASE(JSOP_FORCEINTERPRETER)
END_CASE(JSOP_FORCEINTERPRETER)

CASE(JSOP_UNDEFINED)
    // If this ever changes, change what JSOP_GIMPLICITTHIS does too.
    PUSH_UNDEFINED();
END_CASE(JSOP_UNDEFINED)

CASE(JSOP_POP)
    REGS.sp--;
END_CASE(JSOP_POP)

CASE(JSOP_POPN)
    MOZ_ASSERT(GET_UINT16(REGS.pc) <= REGS.stackDepth());
    REGS.sp -= GET_UINT16(REGS.pc);
END_CASE(JSOP_POPN)

CASE(JSOP_DUPAT)
{
    MOZ_ASSERT(GET_UINT24(REGS.pc) < REGS.stackDepth());
    unsigned i = GET_UINT24(REGS.pc);
    const Value& rref = REGS.sp[-int(i + 1)];
    PUSH_COPY(rref);
}
END_CASE(JSOP_DUPAT)

CASE(JSOP_SETRVAL)
    POP_RETURN_VALUE();
END_CASE(JSOP_SETRVAL)

CASE(JSOP_GETRVAL)
    PUSH_COPY(REGS.fp()->returnValue());
END_CASE(JSOP_GETRVAL)

CASE(JSOP_ENTERWITH)
{
    ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
    REGS.sp--;
    ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

    if (!EnterWithOperation(cx, REGS.fp(), val, scope.as<WithScope>()))
        goto error;
}
END_CASE(JSOP_ENTERWITH)

CASE(JSOP_LEAVEWITH)
    REGS.fp()->popOffEnvironmentChain<WithEnvironmentObject>();
END_CASE(JSOP_LEAVEWITH)

CASE(JSOP_RETURN)
    POP_RETURN_VALUE();
    /* FALL THROUGH */

CASE(JSOP_RETRVAL)
{
    /*
     * When the inlined frame exits with an exception or an error, ok will be
     * false after the inline_return label.
     */
    CHECK_BRANCH();

  successful_return_continuation:
    interpReturnOK = true;

  return_continuation:
    frameHalfInitialized = false;

  prologue_return_continuation:

    if (activation.entryFrame() != REGS.fp()) {
        // Stop the engine. (No details about which engine exactly, could be
        // interpreter, Baseline or IonMonkey.)
        TraceLogStopEvent(logger, TraceLogger_Engine);
        TraceLogStopEvent(logger, TraceLogger_Scripts);

        if (MOZ_LIKELY(!frameHalfInitialized)) {
            interpReturnOK = Debugger::onLeaveFrame(cx, REGS.fp(), REGS.pc, interpReturnOK);

            REGS.fp()->epilogue(cx, REGS.pc);
        }

  jit_return_pop_frame:

        activation.popInlineFrame(REGS.fp());
        SET_SCRIPT(REGS.fp()->script());

  jit_return:

        MOZ_ASSERT(CodeSpec[*REGS.pc].format & JOF_INVOKE);

        /* Resume execution in the calling frame. */
        if (MOZ_LIKELY(interpReturnOK)) {
            TypeScript::Monitor(cx, script, REGS.pc, REGS.sp[-1]);

            ADVANCE_AND_DISPATCH(JSOP_CALL_LENGTH);
        }

        goto error;
    } else {
        MOZ_ASSERT(REGS.stackDepth() == 0);
    }
    goto exit;
}

CASE(JSOP_DEFAULT)
    REGS.sp--;
    /* FALL THROUGH */
CASE(JSOP_GOTO)
{
    BRANCH(GET_JUMP_OFFSET(REGS.pc));
}

CASE(JSOP_IFEQ)
{
    bool cond = ToBoolean(REGS.stackHandleAt(-1));
    REGS.sp--;
    if (!cond)
        BRANCH(GET_JUMP_OFFSET(REGS.pc));
}
END_CASE(JSOP_IFEQ)

CASE(JSOP_IFNE)
{
    bool cond = ToBoolean(REGS.stackHandleAt(-1));
    REGS.sp--;
    if (cond)
        BRANCH(GET_JUMP_OFFSET(REGS.pc));
}
END_CASE(JSOP_IFNE)

CASE(JSOP_OR)
{
    bool cond = ToBoolean(REGS.stackHandleAt(-1));
    if (cond)
        ADVANCE_AND_DISPATCH(GET_JUMP_OFFSET(REGS.pc));
}
END_CASE(JSOP_OR)

CASE(JSOP_AND)
{
    bool cond = ToBoolean(REGS.stackHandleAt(-1));
    if (!cond)
        ADVANCE_AND_DISPATCH(GET_JUMP_OFFSET(REGS.pc));
}
END_CASE(JSOP_AND)

#define FETCH_ELEMENT_ID(n, id)                                               \
    JS_BEGIN_MACRO                                                            \
        if (!ToPropertyKey(cx, REGS.stackHandleAt(n), &(id)))                 \
            goto error;                                                       \
    JS_END_MACRO

#define TRY_BRANCH_AFTER_COND(cond,spdec)                                     \
    JS_BEGIN_MACRO                                                            \
        MOZ_ASSERT(CodeSpec[*REGS.pc].length == 1);                           \
        unsigned diff_ = (unsigned) GET_UINT8(REGS.pc) - (unsigned) JSOP_IFEQ; \
        if (diff_ <= 1) {                                                     \
            REGS.sp -= (spdec);                                               \
            if ((cond) == (diff_ != 0)) {                                     \
                ++REGS.pc;                                                    \
                BRANCH(GET_JUMP_OFFSET(REGS.pc));                             \
            }                                                                 \
            ADVANCE_AND_DISPATCH(1 + JSOP_IFEQ_LENGTH);                       \
        }                                                                     \
    JS_END_MACRO

CASE(JSOP_IN)
{
    HandleValue rref = REGS.stackHandleAt(-1);
    if (!rref.isObject()) {
        HandleValue lref = REGS.stackHandleAt(-2);
        ReportInNotObjectError(cx, lref, -2, rref, -1);
        goto error;
    }
    bool found;
    {
        ReservedRooted<JSObject*> obj(&rootObject0, &rref.toObject());
        ReservedRooted<jsid> id(&rootId0);
        FETCH_ELEMENT_ID(-2, id);
        if (!HasProperty(cx, obj, id, &found))
            goto error;
    }
    TRY_BRANCH_AFTER_COND(found, 2);
    REGS.sp--;
    REGS.sp[-1].setBoolean(found);
}
END_CASE(JSOP_IN)

CASE(JSOP_HASOWN)
{
    HandleValue val = REGS.stackHandleAt(-1);
    HandleValue idval = REGS.stackHandleAt(-2);

    bool found;
    if (!HasOwnProperty(cx, val, idval, &found))
        goto error;

    REGS.sp--;
    REGS.sp[-1].setBoolean(found);
}
END_CASE(JSOP_HASOWN)

CASE(JSOP_ITER)
{
    MOZ_ASSERT(REGS.stackDepth() >= 1);
    HandleValue val = REGS.stackHandleAt(-1);
    JSObject* iter = ValueToIterator(cx, val);
    if (!iter)
        goto error;
    REGS.sp[-1].setObject(*iter);
}
END_CASE(JSOP_ITER)

CASE(JSOP_MOREITER)
{
    MOZ_ASSERT(REGS.stackDepth() >= 1);
    MOZ_ASSERT(REGS.sp[-1].isObject());
    PUSH_NULL();
    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());
    if (!IteratorMore(cx, obj, REGS.stackHandleAt(-1)))
        goto error;
}
END_CASE(JSOP_MOREITER)

CASE(JSOP_ISNOITER)
{
    bool b = REGS.sp[-1].isMagic(JS_NO_ITER_VALUE);
    PUSH_BOOLEAN(b);
}
END_CASE(JSOP_ISNOITER)

CASE(JSOP_ENDITER)
{
    MOZ_ASSERT(REGS.stackDepth() >= 1);
    COUNT_COVERAGE();
    CloseIterator(&REGS.sp[-1].toObject());
    REGS.sp--;
}
END_CASE(JSOP_ENDITER)

CASE(JSOP_ISGENCLOSING)
{
    bool b = REGS.sp[-1].isMagic(JS_GENERATOR_CLOSING);
    PUSH_BOOLEAN(b);
}
END_CASE(JSOP_ISGENCLOSING)

CASE(JSOP_ITERNEXT)
{
    // Ion relies on this.
    MOZ_ASSERT(REGS.sp[-1].isString());
}
END_CASE(JSOP_ITERNEXT)

CASE(JSOP_DUP)
{
    MOZ_ASSERT(REGS.stackDepth() >= 1);
    const Value& rref = REGS.sp[-1];
    PUSH_COPY(rref);
}
END_CASE(JSOP_DUP)

CASE(JSOP_DUP2)
{
    MOZ_ASSERT(REGS.stackDepth() >= 2);
    const Value& lref = REGS.sp[-2];
    const Value& rref = REGS.sp[-1];
    PUSH_COPY(lref);
    PUSH_COPY(rref);
}
END_CASE(JSOP_DUP2)

CASE(JSOP_SWAP)
{
    MOZ_ASSERT(REGS.stackDepth() >= 2);
    Value& lref = REGS.sp[-2];
    Value& rref = REGS.sp[-1];
    lref.swap(rref);
}
END_CASE(JSOP_SWAP)

CASE(JSOP_PICK)
{
    unsigned i = GET_UINT8(REGS.pc);
    MOZ_ASSERT(REGS.stackDepth() >= i + 1);
    Value lval = REGS.sp[-int(i + 1)];
    memmove(REGS.sp - (i + 1), REGS.sp - i, sizeof(Value) * i);
    REGS.sp[-1] = lval;
}
END_CASE(JSOP_PICK)

CASE(JSOP_UNPICK)
{
    int i = GET_UINT8(REGS.pc);
    MOZ_ASSERT(REGS.stackDepth() >= unsigned(i) + 1);
    Value lval = REGS.sp[-1];
    memmove(REGS.sp - i, REGS.sp - (i + 1), sizeof(Value) * i);
    REGS.sp[-(i + 1)] = lval;
}
END_CASE(JSOP_UNPICK)

CASE(JSOP_BINDGNAME)
CASE(JSOP_BINDNAME)
{
    JSOp op = JSOp(*REGS.pc);
    ReservedRooted<JSObject*> envChain(&rootObject0);
    if (op == JSOP_BINDNAME || script->hasNonSyntacticScope())
        envChain.set(REGS.fp()->environmentChain());
    else
        envChain.set(&REGS.fp()->global().lexicalEnvironment());
    ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

    /* Assigning to an undeclared name adds a property to the global object. */
    ReservedRooted<JSObject*> env(&rootObject1);
    if (!LookupNameUnqualified(cx, name, envChain, &env))
        goto error;

    PUSH_OBJECT(*env);

    static_assert(JSOP_BINDNAME_LENGTH == JSOP_BINDGNAME_LENGTH,
                  "We're sharing the END_CASE so the lengths better match");
}
END_CASE(JSOP_BINDNAME)

CASE(JSOP_BINDVAR)
{
    PUSH_OBJECT(REGS.fp()->varObj());
}
END_CASE(JSOP_BINDVAR)

#define BITWISE_OP(OP)                                                        \
    JS_BEGIN_MACRO                                                            \
        int32_t i, j;                                                         \
        if (!ToInt32(cx, REGS.stackHandleAt(-2), &i))                         \
            goto error;                                                       \
        if (!ToInt32(cx, REGS.stackHandleAt(-1), &j))                         \
            goto error;                                                       \
        i = i OP j;                                                           \
        REGS.sp--;                                                            \
        REGS.sp[-1].setInt32(i);                                              \
    JS_END_MACRO

CASE(JSOP_BITOR)
    BITWISE_OP(|);
END_CASE(JSOP_BITOR)

CASE(JSOP_BITXOR)
    BITWISE_OP(^);
END_CASE(JSOP_BITXOR)

CASE(JSOP_BITAND)
    BITWISE_OP(&);
END_CASE(JSOP_BITAND)

#undef BITWISE_OP

CASE(JSOP_EQ)
    if (!LooseEqualityOp<true>(cx, REGS))
        goto error;
END_CASE(JSOP_EQ)

CASE(JSOP_NE)
    if (!LooseEqualityOp<false>(cx, REGS))
        goto error;
END_CASE(JSOP_NE)

#define STRICT_EQUALITY_OP(OP, COND)                                          \
    JS_BEGIN_MACRO                                                            \
        HandleValue lval = REGS.stackHandleAt(-2);                            \
        HandleValue rval = REGS.stackHandleAt(-1);                            \
        bool equal;                                                           \
        if (!StrictlyEqual(cx, lval, rval, &equal))                           \
            goto error;                                                       \
        (COND) = equal OP true;                                               \
        REGS.sp--;                                                            \
    JS_END_MACRO

CASE(JSOP_STRICTEQ)
{
    bool cond;
    STRICT_EQUALITY_OP(==, cond);
    REGS.sp[-1].setBoolean(cond);
}
END_CASE(JSOP_STRICTEQ)

CASE(JSOP_STRICTNE)
{
    bool cond;
    STRICT_EQUALITY_OP(!=, cond);
    REGS.sp[-1].setBoolean(cond);
}
END_CASE(JSOP_STRICTNE)

CASE(JSOP_CASE)
{
    bool cond;
    STRICT_EQUALITY_OP(==, cond);
    if (cond) {
        REGS.sp--;
        BRANCH(GET_JUMP_OFFSET(REGS.pc));
    }
}
END_CASE(JSOP_CASE)

#undef STRICT_EQUALITY_OP

CASE(JSOP_LT)
{
    bool cond;
    MutableHandleValue lval = REGS.stackHandleAt(-2);
    MutableHandleValue rval = REGS.stackHandleAt(-1);
    if (!LessThanOperation(cx, lval, rval, &cond))
        goto error;
    TRY_BRANCH_AFTER_COND(cond, 2);
    REGS.sp[-2].setBoolean(cond);
    REGS.sp--;
}
END_CASE(JSOP_LT)

CASE(JSOP_LE)
{
    bool cond;
    MutableHandleValue lval = REGS.stackHandleAt(-2);
    MutableHandleValue rval = REGS.stackHandleAt(-1);
    if (!LessThanOrEqualOperation(cx, lval, rval, &cond))
        goto error;
    TRY_BRANCH_AFTER_COND(cond, 2);
    REGS.sp[-2].setBoolean(cond);
    REGS.sp--;
}
END_CASE(JSOP_LE)

CASE(JSOP_GT)
{
    bool cond;
    MutableHandleValue lval = REGS.stackHandleAt(-2);
    MutableHandleValue rval = REGS.stackHandleAt(-1);
    if (!GreaterThanOperation(cx, lval, rval, &cond))
        goto error;
    TRY_BRANCH_AFTER_COND(cond, 2);
    REGS.sp[-2].setBoolean(cond);
    REGS.sp--;
}
END_CASE(JSOP_GT)

CASE(JSOP_GE)
{
    bool cond;
    MutableHandleValue lval = REGS.stackHandleAt(-2);
    MutableHandleValue rval = REGS.stackHandleAt(-1);
    if (!GreaterThanOrEqualOperation(cx, lval, rval, &cond))
        goto error;
    TRY_BRANCH_AFTER_COND(cond, 2);
    REGS.sp[-2].setBoolean(cond);
    REGS.sp--;
}
END_CASE(JSOP_GE)

#define SIGNED_SHIFT_OP(OP, TYPE)                                             \
    JS_BEGIN_MACRO                                                            \
        int32_t i, j;                                                         \
        if (!ToInt32(cx, REGS.stackHandleAt(-2), &i))                         \
            goto error;                                                       \
        if (!ToInt32(cx, REGS.stackHandleAt(-1), &j))                         \
            goto error;                                                       \
        i = TYPE(i) OP (j & 31);                                              \
        REGS.sp--;                                                            \
        REGS.sp[-1].setInt32(i);                                              \
    JS_END_MACRO

CASE(JSOP_LSH)
    SIGNED_SHIFT_OP(<<, uint32_t);
END_CASE(JSOP_LSH)

CASE(JSOP_RSH)
    SIGNED_SHIFT_OP(>>, int32_t);
END_CASE(JSOP_RSH)

#undef SIGNED_SHIFT_OP

CASE(JSOP_URSH)
{
    HandleValue lval = REGS.stackHandleAt(-2);
    HandleValue rval = REGS.stackHandleAt(-1);
    MutableHandleValue res = REGS.stackHandleAt(-2);
    if (!UrshOperation(cx, lval, rval, res))
        goto error;
    REGS.sp--;
}
END_CASE(JSOP_URSH)

CASE(JSOP_ADD)
{
    MutableHandleValue lval = REGS.stackHandleAt(-2);
    MutableHandleValue rval = REGS.stackHandleAt(-1);
    MutableHandleValue res = REGS.stackHandleAt(-2);
    if (!AddOperation(cx, lval, rval, res))
        goto error;
    REGS.sp--;
}
END_CASE(JSOP_ADD)

CASE(JSOP_SUB)
{
    ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
    ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
    MutableHandleValue res = REGS.stackHandleAt(-2);
    if (!SubOperation(cx, lval, rval, res))
        goto error;
    REGS.sp--;
}
END_CASE(JSOP_SUB)

CASE(JSOP_MUL)
{
    ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
    ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
    MutableHandleValue res = REGS.stackHandleAt(-2);
    if (!MulOperation(cx, lval, rval, res))
        goto error;
    REGS.sp--;
}
END_CASE(JSOP_MUL)

CASE(JSOP_DIV)
{
    ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
    ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
    MutableHandleValue res = REGS.stackHandleAt(-2);
    if (!DivOperation(cx, lval, rval, res))
        goto error;
    REGS.sp--;
}
END_CASE(JSOP_DIV)

CASE(JSOP_MOD)
{
    ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
    ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
    MutableHandleValue res = REGS.stackHandleAt(-2);
    if (!ModOperation(cx, lval, rval, res))
        goto error;
    REGS.sp--;
}
END_CASE(JSOP_MOD)

CASE(JSOP_POW)
{
    ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
    ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
    MutableHandleValue res = REGS.stackHandleAt(-2);
    if (!math_pow_handle(cx, lval, rval, res))
        goto error;
    REGS.sp--;
}
END_CASE(JSOP_POW)

CASE(JSOP_NOT)
{
    bool cond = ToBoolean(REGS.stackHandleAt(-1));
    REGS.sp--;
    PUSH_BOOLEAN(!cond);
}
END_CASE(JSOP_NOT)

CASE(JSOP_BITNOT)
{
    int32_t i;
    HandleValue value = REGS.stackHandleAt(-1);
    if (!BitNot(cx, value, &i))
        goto error;
    REGS.sp[-1].setInt32(i);
}
END_CASE(JSOP_BITNOT)

CASE(JSOP_NEG)
{
    ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
    MutableHandleValue res = REGS.stackHandleAt(-1);
    if (!NegOperation(cx, val, res))
        goto error;
}
END_CASE(JSOP_NEG)

CASE(JSOP_POS)
    if (!ToNumber(cx, REGS.stackHandleAt(-1)))
        goto error;
END_CASE(JSOP_POS)

CASE(JSOP_DELNAME)
{
    ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
    ReservedRooted<JSObject*> envObj(&rootObject0, REGS.fp()->environmentChain());

    PUSH_BOOLEAN(true);
    MutableHandleValue res = REGS.stackHandleAt(-1);
    if (!DeleteNameOperation(cx, name, envObj, res))
        goto error;
}
END_CASE(JSOP_DELNAME)

CASE(JSOP_DELPROP)
CASE(JSOP_STRICTDELPROP)
{
    static_assert(JSOP_DELPROP_LENGTH == JSOP_STRICTDELPROP_LENGTH,
                  "delprop and strictdelprop must be the same size");
    ReservedRooted<jsid> id(&rootId0, NameToId(script->getName(REGS.pc)));
    ReservedRooted<JSObject*> obj(&rootObject0);
    FETCH_OBJECT(cx, -1, obj);

    ObjectOpResult result;
    if (!DeleteProperty(cx, obj, id, result))
        goto error;
    if (!result && JSOp(*REGS.pc) == JSOP_STRICTDELPROP) {
        result.reportError(cx, obj, id);
        goto error;
    }
    MutableHandleValue res = REGS.stackHandleAt(-1);
    res.setBoolean(result.ok());
}
END_CASE(JSOP_DELPROP)

CASE(JSOP_DELELEM)
CASE(JSOP_STRICTDELELEM)
{
    static_assert(JSOP_DELELEM_LENGTH == JSOP_STRICTDELELEM_LENGTH,
                  "delelem and strictdelelem must be the same size");
    /* Fetch the left part and resolve it to a non-null object. */
    ReservedRooted<JSObject*> obj(&rootObject0);
    FETCH_OBJECT(cx, -2, obj);

    ReservedRooted<Value> propval(&rootValue0, REGS.sp[-1]);

    ObjectOpResult result;
    ReservedRooted<jsid> id(&rootId0);
    if (!ToPropertyKey(cx, propval, &id))
        goto error;
    if (!DeleteProperty(cx, obj, id, result))
        goto error;
    if (!result && JSOp(*REGS.pc) == JSOP_STRICTDELELEM) {
        result.reportError(cx, obj, id);
        goto error;
    }

    MutableHandleValue res = REGS.stackHandleAt(-2);
    res.setBoolean(result.ok());
    REGS.sp--;
}
END_CASE(JSOP_DELELEM)

CASE(JSOP_TOID)
{
    /*
     * Increment or decrement requires use to lookup the same property twice,
     * but we need to avoid the observable stringification the second time.
     * There must be an object value below the id, which will not be popped.
     */
    ReservedRooted<Value> idval(&rootValue1, REGS.sp[-1]);
    MutableHandleValue res = REGS.stackHandleAt(-1);
    if (!ToIdOperation(cx, idval, res))
        goto error;
}
END_CASE(JSOP_TOID)

CASE(JSOP_TYPEOFEXPR)
CASE(JSOP_TYPEOF)
{
    REGS.sp[-1].setString(TypeOfOperation(REGS.sp[-1], cx->runtime()));
}
END_CASE(JSOP_TYPEOF)

CASE(JSOP_VOID)
    REGS.sp[-1].setUndefined();
END_CASE(JSOP_VOID)

CASE(JSOP_FUNCTIONTHIS)
    PUSH_NULL();
    if (!GetFunctionThis(cx, REGS.fp(), REGS.stackHandleAt(-1)))
        goto error;
END_CASE(JSOP_FUNCTIONTHIS)

CASE(JSOP_GLOBALTHIS)
{
    if (script->hasNonSyntacticScope()) {
        PUSH_NULL();
        GetNonSyntacticGlobalThis(cx, REGS.fp()->environmentChain(), REGS.stackHandleAt(-1));
    } else {
        PUSH_COPY(cx->global()->lexicalEnvironment().thisValue());
    }
}
END_CASE(JSOP_GLOBALTHIS)

CASE(JSOP_CHECKISOBJ)
{
    if (!REGS.sp[-1].isObject()) {
        MOZ_ALWAYS_FALSE(ThrowCheckIsObject(cx, CheckIsObjectKind(GET_UINT8(REGS.pc))));
        goto error;
    }
}
END_CASE(JSOP_CHECKISOBJ)

CASE(JSOP_CHECKISCALLABLE)
{
    if (!IsCallable(REGS.sp[-1])) {
        MOZ_ALWAYS_FALSE(ThrowCheckIsCallable(cx, CheckIsCallableKind(GET_UINT8(REGS.pc))));
        goto error;
    }
}
END_CASE(JSOP_CHECKISCALLABLE)

CASE(JSOP_CHECKTHIS)
{
    if (REGS.sp[-1].isMagic(JS_UNINITIALIZED_LEXICAL)) {
        MOZ_ALWAYS_FALSE(ThrowUninitializedThis(cx, REGS.fp()));
        goto error;
    }
}
END_CASE(JSOP_CHECKTHIS)

CASE(JSOP_CHECKTHISREINIT)
{
    if (!REGS.sp[-1].isMagic(JS_UNINITIALIZED_LEXICAL)) {
        MOZ_ALWAYS_FALSE(ThrowInitializedThis(cx));
        goto error;
    }
}
END_CASE(JSOP_CHECKTHISREINIT)

CASE(JSOP_CHECKRETURN)
{
    if (!REGS.fp()->checkReturn(cx, REGS.stackHandleAt(-1)))
        goto error;
    REGS.sp--;
}
END_CASE(JSOP_CHECKRETURN)

CASE(JSOP_GETPROP)
CASE(JSOP_LENGTH)
CASE(JSOP_CALLPROP)
{
    MutableHandleValue lval = REGS.stackHandleAt(-1);
    if (!GetPropertyOperation(cx, REGS.fp(), script, REGS.pc, lval, lval))
        goto error;

    TypeScript::Monitor(cx, script, REGS.pc, lval);
    assertSameCompartmentDebugOnly(cx, lval);
}
END_CASE(JSOP_GETPROP)

CASE(JSOP_GETPROP_SUPER)
{
    ReservedRooted<Value> receiver(&rootValue0, REGS.sp[-2]);
    ReservedRooted<JSObject*> obj(&rootObject1, &REGS.sp[-1].toObject());
    MutableHandleValue rref = REGS.stackHandleAt(-2);

    if (!GetProperty(cx, obj, receiver, script->getName(REGS.pc), rref))
        goto error;

    TypeScript::Monitor(cx, script, REGS.pc, rref);
    assertSameCompartmentDebugOnly(cx, rref);

    REGS.sp--;
}
END_CASE(JSOP_GETPROP_SUPER)

CASE(JSOP_GETBOUNDNAME)
{
    ReservedRooted<JSObject*> env(&rootObject0, &REGS.sp[-1].toObject());
    ReservedRooted<jsid> id(&rootId0, NameToId(script->getName(REGS.pc)));
    MutableHandleValue rval = REGS.stackHandleAt(-1);
    if (!GetNameBoundInEnvironment(cx, env, id, rval))
        goto error;

    TypeScript::Monitor(cx, script, REGS.pc, rval);
    assertSameCompartmentDebugOnly(cx, rval);
}
END_CASE(JSOP_GETBOUNDNAME)

CASE(JSOP_SETINTRINSIC)
{
    HandleValue value = REGS.stackHandleAt(-1);

    if (!SetIntrinsicOperation(cx, script, REGS.pc, value))
        goto error;
}
END_CASE(JSOP_SETINTRINSIC)

CASE(JSOP_SETGNAME)
CASE(JSOP_STRICTSETGNAME)
CASE(JSOP_SETNAME)
CASE(JSOP_STRICTSETNAME)
{
    static_assert(JSOP_SETNAME_LENGTH == JSOP_STRICTSETNAME_LENGTH,
                  "setname and strictsetname must be the same size");
    static_assert(JSOP_SETGNAME_LENGTH == JSOP_STRICTSETGNAME_LENGTH,
                  "setganem adn strictsetgname must be the same size");
    static_assert(JSOP_SETNAME_LENGTH == JSOP_SETGNAME_LENGTH,
                  "We're sharing the END_CASE so the lengths better match");

    ReservedRooted<JSObject*> env(&rootObject0, &REGS.sp[-2].toObject());
    HandleValue value = REGS.stackHandleAt(-1);

    if (!SetNameOperation(cx, script, REGS.pc, env, value))
        goto error;

    REGS.sp[-2] = REGS.sp[-1];
    REGS.sp--;
}
END_CASE(JSOP_SETNAME)

CASE(JSOP_SETPROP)
CASE(JSOP_STRICTSETPROP)
{
    static_assert(JSOP_SETPROP_LENGTH == JSOP_STRICTSETPROP_LENGTH,
                  "setprop and strictsetprop must be the same size");
    HandleValue lval = REGS.stackHandleAt(-2);
    HandleValue rval = REGS.stackHandleAt(-1);

    ReservedRooted<jsid> id(&rootId0, NameToId(script->getName(REGS.pc)));
    if (!SetPropertyOperation(cx, JSOp(*REGS.pc), lval, id, rval))
        goto error;

    REGS.sp[-2] = REGS.sp[-1];
    REGS.sp--;
}
END_CASE(JSOP_SETPROP)

CASE(JSOP_SETPROP_SUPER)
CASE(JSOP_STRICTSETPROP_SUPER)
{
    static_assert(JSOP_SETPROP_SUPER_LENGTH == JSOP_STRICTSETPROP_SUPER_LENGTH,
                  "setprop-super and strictsetprop-super must be the same size");

    ReservedRooted<Value> receiver(&rootValue0, REGS.sp[-3]);
    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());
    ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
    ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

    bool strict = JSOp(*REGS.pc) == JSOP_STRICTSETPROP_SUPER;

    if (!SetPropertySuper(cx, obj, receiver, name, rval, strict))
        goto error;

    REGS.sp[-3] = REGS.sp[-1];
    REGS.sp -= 2;
}
END_CASE(JSOP_SETPROP_SUPER)

CASE(JSOP_GETELEM)
CASE(JSOP_CALLELEM)
{
    MutableHandleValue lval = REGS.stackHandleAt(-2);
    HandleValue rval = REGS.stackHandleAt(-1);
    MutableHandleValue res = REGS.stackHandleAt(-2);

    bool done = false;
    if (!GetElemOptimizedArguments(cx, REGS.fp(), lval, rval, res, &done))
        goto error;

    if (!done) {
        if (!GetElementOperation(cx, JSOp(*REGS.pc), lval, rval, res))
            goto error;
    }

    TypeScript::Monitor(cx, script, REGS.pc, res);
    REGS.sp--;
}
END_CASE(JSOP_GETELEM)

CASE(JSOP_GETELEM_SUPER)
{
    ReservedRooted<Value> rval(&rootValue0, REGS.sp[-3]);
    ReservedRooted<Value> receiver(&rootValue1, REGS.sp[-2]);
    ReservedRooted<JSObject*> obj(&rootObject1, &REGS.sp[-1].toObject());

    MutableHandleValue res = REGS.stackHandleAt(-3);

    // Since we have asserted that obj has to be an object, it cannot be
    // either optimized arguments, or indeed any primitive. This simplifies
    // our task some.
    if (!GetObjectElementOperation(cx, JSOp(*REGS.pc), obj, receiver, rval, res))
        goto error;

    TypeScript::Monitor(cx, script, REGS.pc, res);
    REGS.sp -= 2;
}
END_CASE(JSOP_GETELEM_SUPER)

CASE(JSOP_SETELEM)
CASE(JSOP_STRICTSETELEM)
{
    static_assert(JSOP_SETELEM_LENGTH == JSOP_STRICTSETELEM_LENGTH,
                  "setelem and strictsetelem must be the same size");
    HandleValue receiver = REGS.stackHandleAt(-3);
    ReservedRooted<JSObject*> obj(&rootObject0);
    obj = ToObjectFromStack(cx, receiver);
    if (!obj)
        goto error;
    ReservedRooted<jsid> id(&rootId0);
    FETCH_ELEMENT_ID(-2, id);
    HandleValue value = REGS.stackHandleAt(-1);
    if (!SetObjectElementOperation(cx, obj, id, value, receiver, *REGS.pc == JSOP_STRICTSETELEM))
        goto error;
    REGS.sp[-3] = value;
    REGS.sp -= 2;
}
END_CASE(JSOP_SETELEM)

CASE(JSOP_SETELEM_SUPER)
CASE(JSOP_STRICTSETELEM_SUPER)
{
    static_assert(JSOP_SETELEM_SUPER_LENGTH == JSOP_STRICTSETELEM_SUPER_LENGTH,
                  "setelem-super and strictsetelem-super must be the same size");

    ReservedRooted<Value> index(&rootValue1, REGS.sp[-4]);
    ReservedRooted<Value> receiver(&rootValue0, REGS.sp[-3]);
    ReservedRooted<JSObject*> obj(&rootObject1, &REGS.sp[-2].toObject());
    HandleValue value = REGS.stackHandleAt(-1);

    bool strict = JSOp(*REGS.pc) == JSOP_STRICTSETELEM_SUPER;
    if (!SetObjectElement(cx, obj, index, value, receiver, strict))
        goto error;
    REGS.sp[-4] = value;
    REGS.sp -= 3;
}
END_CASE(JSOP_SETELEM_SUPER)

CASE(JSOP_EVAL)
CASE(JSOP_STRICTEVAL)
{
    static_assert(JSOP_EVAL_LENGTH == JSOP_STRICTEVAL_LENGTH,
                  "eval and stricteval must be the same size");

    CallArgs args = CallArgsFromSp(GET_ARGC(REGS.pc), REGS.sp);
    if (REGS.fp()->environmentChain()->global().valueIsEval(args.calleev())) {
        if (!DirectEval(cx, args.get(0), args.rval()))
            goto error;
    } else {
        if (!CallFromStack(cx, args))
            goto error;
    }

    REGS.sp = args.spAfterCall();
    TypeScript::Monitor(cx, script, REGS.pc, REGS.sp[-1]);
}
END_CASE(JSOP_EVAL)

CASE(JSOP_SPREADNEW)
CASE(JSOP_SPREADCALL)
CASE(JSOP_SPREADSUPERCALL)
    if (REGS.fp()->hasPushedGeckoProfilerFrame())
        cx->geckoProfiler().updatePC(cx, script, REGS.pc);
    /* FALL THROUGH */

CASE(JSOP_SPREADEVAL)
CASE(JSOP_STRICTSPREADEVAL)
{
    static_assert(JSOP_SPREADEVAL_LENGTH == JSOP_STRICTSPREADEVAL_LENGTH,
                  "spreadeval and strictspreadeval must be the same size");
    bool construct = JSOp(*REGS.pc) == JSOP_SPREADNEW || JSOp(*REGS.pc) == JSOP_SPREADSUPERCALL;;

    MOZ_ASSERT(REGS.stackDepth() >= 3u + construct);

    HandleValue callee = REGS.stackHandleAt(-3 - construct);
    HandleValue thisv = REGS.stackHandleAt(-2 - construct);
    HandleValue arr = REGS.stackHandleAt(-1 - construct);
    MutableHandleValue ret = REGS.stackHandleAt(-3 - construct);

    RootedValue& newTarget = rootValue0;
    if (construct)
        newTarget = REGS.sp[-1];
    else
        newTarget = NullValue();

    if (!SpreadCallOperation(cx, script, REGS.pc, thisv, callee, arr, newTarget, ret))
        goto error;

    REGS.sp -= 2 + construct;
}
END_CASE(JSOP_SPREADCALL)

CASE(JSOP_FUNAPPLY)
{
    CallArgs args = CallArgsFromSp(GET_ARGC(REGS.pc), REGS.sp);
    if (!GuardFunApplyArgumentsOptimization(cx, REGS.fp(), args))
        goto error;
    /* FALL THROUGH */
}

CASE(JSOP_NEW)
CASE(JSOP_CALL)
CASE(JSOP_CALL_IGNORES_RV)
CASE(JSOP_CALLITER)
CASE(JSOP_SUPERCALL)
CASE(JSOP_FUNCALL)
{
    if (REGS.fp()->hasPushedGeckoProfilerFrame())
        cx->geckoProfiler().updatePC(cx, script, REGS.pc);

    MaybeConstruct construct = MaybeConstruct(*REGS.pc == JSOP_NEW || *REGS.pc == JSOP_SUPERCALL);
    bool ignoresReturnValue = *REGS.pc == JSOP_CALL_IGNORES_RV;
    unsigned argStackSlots = GET_ARGC(REGS.pc) + construct;

    MOZ_ASSERT(REGS.stackDepth() >= 2u + GET_ARGC(REGS.pc));
    CallArgs args = CallArgsFromSp(argStackSlots, REGS.sp, construct, ignoresReturnValue);

    JSFunction* maybeFun;
    bool isFunction = IsFunctionObject(args.calleev(), &maybeFun);

    /* Don't bother trying to fast-path calls to scripted non-constructors. */
    if (!isFunction || !maybeFun->isInterpreted() || !maybeFun->isConstructor() ||
        (!construct && maybeFun->isClassConstructor()))
    {
        if (construct) {
            if (!ConstructFromStack(cx, args))
                goto error;
        } else {
            if (*REGS.pc == JSOP_CALLITER && args.calleev().isPrimitive()) {
                MOZ_ASSERT(args.length() == 0, "thisv must be on top of the stack");
                ReportValueError(cx, JSMSG_NOT_ITERABLE, -1, args.thisv(), nullptr);
                goto error;
            }
            if (!CallFromStack(cx, args))
                goto error;
        }
        Value* newsp = args.spAfterCall();
        TypeScript::Monitor(cx, script, REGS.pc, newsp[-1]);
        REGS.sp = newsp;
        ADVANCE_AND_DISPATCH(JSOP_CALL_LENGTH);
    }

    {
        MOZ_ASSERT(maybeFun);
        ReservedRooted<JSFunction*> fun(&rootFunction0, maybeFun);
        ReservedRooted<JSScript*> funScript(&rootScript0, JSFunction::getOrCreateScript(cx, fun));
        if (!funScript)
            goto error;

        bool createSingleton = false;
        if (construct) {
            createSingleton = ObjectGroup::useSingletonForNewObject(cx, script, REGS.pc);

            if (!MaybeCreateThisForConstructor(cx, funScript, args, createSingleton))
                goto error;
        }

        TypeMonitorCall(cx, args, construct);

        {
            InvokeState state(cx, args, construct);

            jit::EnterJitStatus status = jit::MaybeEnterJit(cx, state);
            switch (status) {
              case jit::EnterJitStatus::Error:
                goto error;
              case jit::EnterJitStatus::Ok:
                interpReturnOK = true;
                CHECK_BRANCH();
                REGS.sp = args.spAfterCall();
                goto jit_return;
              case jit::EnterJitStatus::NotEntered:
                break;
            }
        }

        funScript = fun->nonLazyScript();

        if (!activation.pushInlineFrame(args, funScript, construct))
            goto error;
    }

    SET_SCRIPT(REGS.fp()->script());

    {
        TraceLoggerEvent event(TraceLogger_Scripts, script);
        TraceLogStartEvent(logger, event);
        TraceLogStartEvent(logger, TraceLogger_Interpreter);
    }

    if (!REGS.fp()->prologue(cx))
        goto prologue_error;

    switch (Debugger::onEnterFrame(cx, REGS.fp())) {
      case JSTRAP_CONTINUE:
        break;
      case JSTRAP_RETURN:
        if (!ForcedReturn(cx, REGS))
            goto error;
        goto successful_return_continuation;
      case JSTRAP_THROW:
      case JSTRAP_ERROR:
        goto error;
      default:
        MOZ_CRASH("bad Debugger::onEnterFrame status");
    }

    // Increment the coverage for the main entry point.
    INIT_COVERAGE();
    COUNT_COVERAGE_MAIN();

    /* Load first op and dispatch it (safe since JSOP_RETRVAL). */
    ADVANCE_AND_DISPATCH(0);
}

CASE(JSOP_OPTIMIZE_SPREADCALL)
{
    ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);

    bool optimized = false;
    if (!OptimizeSpreadCall(cx, val, &optimized))
        goto error;

    PUSH_BOOLEAN(optimized);
}
END_CASE(JSOP_OPTIMIZE_SPREADCALL)

CASE(JSOP_THROWMSG)
{
    JS_ALWAYS_FALSE(ThrowMsgOperation(cx, GET_UINT16(REGS.pc)));
    goto error;
}
END_CASE(JSOP_THROWMSG)

CASE(JSOP_IMPLICITTHIS)
CASE(JSOP_GIMPLICITTHIS)
{
    JSOp op = JSOp(*REGS.pc);
    if (op == JSOP_IMPLICITTHIS || script->hasNonSyntacticScope()) {
        ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
        ReservedRooted<JSObject*> envObj(&rootObject0, REGS.fp()->environmentChain());
        ReservedRooted<JSObject*> env(&rootObject1);
        if (!LookupNameWithGlobalDefault(cx, name, envObj, &env))
            goto error;

        Value v = ComputeImplicitThis(env);
        PUSH_COPY(v);
    } else {
        // Treat it like JSOP_UNDEFINED.
        PUSH_UNDEFINED();
    }
    static_assert(JSOP_IMPLICITTHIS_LENGTH == JSOP_GIMPLICITTHIS_LENGTH,
                  "We're sharing the END_CASE so the lengths better match");
}
END_CASE(JSOP_IMPLICITTHIS)

CASE(JSOP_GETGNAME)
CASE(JSOP_GETNAME)
{
    ReservedRooted<Value> rval(&rootValue0);
    if (!GetNameOperation(cx, REGS.fp(), REGS.pc, &rval))
        goto error;

    PUSH_COPY(rval);
    TypeScript::Monitor(cx, script, REGS.pc, rval);
    static_assert(JSOP_GETNAME_LENGTH == JSOP_GETGNAME_LENGTH,
                  "We're sharing the END_CASE so the lengths better match");
}
END_CASE(JSOP_GETNAME)

CASE(JSOP_GETIMPORT)
{
    PUSH_NULL();
    MutableHandleValue rval = REGS.stackHandleAt(-1);
    if (!GetImportOperation(cx, REGS.fp(), REGS.pc, rval))
        goto error;

    TypeScript::Monitor(cx, script, REGS.pc, rval);
}
END_CASE(JSOP_GETIMPORT)

CASE(JSOP_GETINTRINSIC)
{
    ReservedRooted<Value> rval(&rootValue0);
    if (!GetIntrinsicOperation(cx, REGS.pc, &rval))
        goto error;

    PUSH_COPY(rval);
    TypeScript::Monitor(cx, script, REGS.pc, rval);
}
END_CASE(JSOP_GETINTRINSIC)

CASE(JSOP_UINT16)
    PUSH_INT32((int32_t) GET_UINT16(REGS.pc));
END_CASE(JSOP_UINT16)

CASE(JSOP_UINT24)
    PUSH_INT32((int32_t) GET_UINT24(REGS.pc));
END_CASE(JSOP_UINT24)

CASE(JSOP_INT8)
    PUSH_INT32(GET_INT8(REGS.pc));
END_CASE(JSOP_INT8)

CASE(JSOP_INT32)
    PUSH_INT32(GET_INT32(REGS.pc));
END_CASE(JSOP_INT32)

CASE(JSOP_DOUBLE)
{
    double dbl;
    LOAD_DOUBLE(0, dbl);
    PUSH_DOUBLE(dbl);
}
END_CASE(JSOP_DOUBLE)

CASE(JSOP_STRING)
    PUSH_STRING(script->getAtom(REGS.pc));
END_CASE(JSOP_STRING)

CASE(JSOP_TOSTRING)
{
    MutableHandleValue oper = REGS.stackHandleAt(-1);

    if (!oper.isString()) {
        JSString* operString = ToString<CanGC>(cx, oper);
        if (!operString)
            goto error;
        oper.setString(operString);
    }
}
END_CASE(JSOP_TOSTRING)

CASE(JSOP_SYMBOL)
    PUSH_SYMBOL(cx->wellKnownSymbols().get(GET_UINT8(REGS.pc)));
END_CASE(JSOP_SYMBOL)

CASE(JSOP_OBJECT)
{
    ReservedRooted<JSObject*> ref(&rootObject0, script->getObject(REGS.pc));
    if (cx->compartment()->creationOptions().cloneSingletons()) {
        JSObject* obj = DeepCloneObjectLiteral(cx, ref, TenuredObject);
        if (!obj)
            goto error;
        PUSH_OBJECT(*obj);
    } else {
        cx->compartment()->behaviors().setSingletonsAsValues();
        PUSH_OBJECT(*ref);
    }
}
END_CASE(JSOP_OBJECT)

CASE(JSOP_CALLSITEOBJ)
{
    ReservedRooted<JSObject*> cso(&rootObject0, script->getObject(REGS.pc));
    ReservedRooted<JSObject*> raw(&rootObject1, script->getObject(GET_UINT32_INDEX(REGS.pc) + 1));

    if (!ProcessCallSiteObjOperation(cx, cso, raw))
        goto error;

    PUSH_OBJECT(*cso);
}
END_CASE(JSOP_CALLSITEOBJ)

CASE(JSOP_REGEXP)
{
    /*
     * Push a regexp object cloned from the regexp literal object mapped by the
     * bytecode at pc.
     */
    ReservedRooted<JSObject*> re(&rootObject0, script->getRegExp(REGS.pc));
    JSObject* obj = CloneRegExpObject(cx, re.as<RegExpObject>());
    if (!obj)
        goto error;
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_REGEXP)

CASE(JSOP_ZERO)
    PUSH_INT32(0);
END_CASE(JSOP_ZERO)

CASE(JSOP_ONE)
    PUSH_INT32(1);
END_CASE(JSOP_ONE)

CASE(JSOP_NULL)
    PUSH_NULL();
END_CASE(JSOP_NULL)

CASE(JSOP_FALSE)
    PUSH_BOOLEAN(false);
END_CASE(JSOP_FALSE)

CASE(JSOP_TRUE)
    PUSH_BOOLEAN(true);
END_CASE(JSOP_TRUE)

CASE(JSOP_TABLESWITCH)
{
    jsbytecode* pc2 = REGS.pc;
    int32_t len = GET_JUMP_OFFSET(pc2);

    /*
     * ECMAv2+ forbids conversion of discriminant, so we will skip to the
     * default case if the discriminant isn't already an int jsval.  (This
     * opcode is emitted only for dense int-domain switches.)
     */
    const Value& rref = *--REGS.sp;
    int32_t i;
    if (rref.isInt32()) {
        i = rref.toInt32();
    } else {
        /* Use mozilla::NumberEqualsInt32 to treat -0 (double) as 0. */
        if (!rref.isDouble() || !NumberEqualsInt32(rref.toDouble(), &i))
            ADVANCE_AND_DISPATCH(len);
    }

    pc2 += JUMP_OFFSET_LEN;
    int32_t low = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;
    int32_t high = GET_JUMP_OFFSET(pc2);

    i = uint32_t(i) - uint32_t(low);
    if ((uint32_t)i < (uint32_t)(high - low + 1)) {
        pc2 += JUMP_OFFSET_LEN + JUMP_OFFSET_LEN * i;
        int32_t off = (int32_t) GET_JUMP_OFFSET(pc2);
        if (off)
            len = off;
    }
    ADVANCE_AND_DISPATCH(len);
}

CASE(JSOP_ARGUMENTS)
    if (!script->ensureHasAnalyzedArgsUsage(cx))
        goto error;
    if (script->needsArgsObj()) {
        ArgumentsObject* obj = ArgumentsObject::createExpected(cx, REGS.fp());
        if (!obj)
            goto error;
        PUSH_COPY(ObjectValue(*obj));
    } else {
        PUSH_COPY(MagicValue(JS_OPTIMIZED_ARGUMENTS));
    }
END_CASE(JSOP_ARGUMENTS)

CASE(JSOP_RUNONCE)
{
    if (!RunOnceScriptPrologue(cx, script))
        goto error;
}
END_CASE(JSOP_RUNONCE)

CASE(JSOP_REST)
{
    ReservedRooted<JSObject*> rest(&rootObject0, REGS.fp()->createRestParameter(cx));
    if (!rest)
        goto error;
    PUSH_COPY(ObjectValue(*rest));
}
END_CASE(JSOP_REST)

CASE(JSOP_GETALIASEDVAR)
{
    EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
    ReservedRooted<Value> val(&rootValue0, REGS.fp()->aliasedEnvironment(ec).aliasedBinding(ec));
#ifdef DEBUG
    // Only the .this slot can hold the TDZ MagicValue.
    if (IsUninitializedLexical(val)) {
        PropertyName* name = EnvironmentCoordinateName(cx->caches().envCoordinateNameCache,
                                                       script, REGS.pc);
        MOZ_ASSERT(name == cx->names().dotThis);
        JSOp next = JSOp(*GetNextPc(REGS.pc));
        MOZ_ASSERT(next == JSOP_CHECKTHIS || next == JSOP_CHECKRETURN || next == JSOP_CHECKTHISREINIT);
    }
#endif
    PUSH_COPY(val);
    TypeScript::Monitor(cx, script, REGS.pc, REGS.sp[-1]);
}
END_CASE(JSOP_GETALIASEDVAR)

CASE(JSOP_SETALIASEDVAR)
{
    EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
    EnvironmentObject& obj = REGS.fp()->aliasedEnvironment(ec);
    SetAliasedVarOperation(cx, script, REGS.pc, obj, ec, REGS.sp[-1], CheckTDZ);
}
END_CASE(JSOP_SETALIASEDVAR)

CASE(JSOP_THROWSETCONST)
CASE(JSOP_THROWSETALIASEDCONST)
CASE(JSOP_THROWSETCALLEE)
{
    ReportRuntimeConstAssignment(cx, script, REGS.pc);
    goto error;
}
END_CASE(JSOP_THROWSETCONST)

CASE(JSOP_CHECKLEXICAL)
{
    uint32_t i = GET_LOCALNO(REGS.pc);
    ReservedRooted<Value> val(&rootValue0, REGS.fp()->unaliasedLocal(i));
    if (!CheckUninitializedLexical(cx, script, REGS.pc, val))
        goto error;
}
END_CASE(JSOP_CHECKLEXICAL)

CASE(JSOP_INITLEXICAL)
{
    uint32_t i = GET_LOCALNO(REGS.pc);
    REGS.fp()->unaliasedLocal(i) = REGS.sp[-1];
}
END_CASE(JSOP_INITLEXICAL)

CASE(JSOP_CHECKALIASEDLEXICAL)
{
    EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
    ReservedRooted<Value> val(&rootValue0, REGS.fp()->aliasedEnvironment(ec).aliasedBinding(ec));
    if (!CheckUninitializedLexical(cx, script, REGS.pc, val))
        goto error;
}
END_CASE(JSOP_CHECKALIASEDLEXICAL)

CASE(JSOP_INITALIASEDLEXICAL)
{
    EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
    EnvironmentObject& obj = REGS.fp()->aliasedEnvironment(ec);
    SetAliasedVarOperation(cx, script, REGS.pc, obj, ec, REGS.sp[-1], DontCheckTDZ);
}
END_CASE(JSOP_INITALIASEDLEXICAL)

CASE(JSOP_INITGLEXICAL)
{
    LexicalEnvironmentObject* lexicalEnv;
    if (script->hasNonSyntacticScope())
        lexicalEnv = &REGS.fp()->extensibleLexicalEnvironment();
    else
        lexicalEnv = &cx->global()->lexicalEnvironment();
    HandleValue value = REGS.stackHandleAt(-1);
    InitGlobalLexicalOperation(cx, lexicalEnv, script, REGS.pc, value);
}
END_CASE(JSOP_INITGLEXICAL)

CASE(JSOP_UNINITIALIZED)
    PUSH_MAGIC(JS_UNINITIALIZED_LEXICAL);
END_CASE(JSOP_UNINITIALIZED)

CASE(JSOP_GETARG)
{
    unsigned i = GET_ARGNO(REGS.pc);
    if (script->argsObjAliasesFormals())
        PUSH_COPY(REGS.fp()->argsObj().arg(i));
    else
        PUSH_COPY(REGS.fp()->unaliasedFormal(i));
}
END_CASE(JSOP_GETARG)

CASE(JSOP_SETARG)
{
    unsigned i = GET_ARGNO(REGS.pc);
    if (script->argsObjAliasesFormals())
        REGS.fp()->argsObj().setArg(i, REGS.sp[-1]);
    else
        REGS.fp()->unaliasedFormal(i) = REGS.sp[-1];
}
END_CASE(JSOP_SETARG)

CASE(JSOP_GETLOCAL)
{
    uint32_t i = GET_LOCALNO(REGS.pc);
    PUSH_COPY_SKIP_CHECK(REGS.fp()->unaliasedLocal(i));

#ifdef DEBUG
    // Derived class constructors store the TDZ Value in the .this slot
    // before a super() call.
    if (IsUninitializedLexical(REGS.sp[-1])) {
        MOZ_ASSERT(script->isDerivedClassConstructor());
        JSOp next = JSOp(*GetNextPc(REGS.pc));
        MOZ_ASSERT(next == JSOP_CHECKTHIS || next == JSOP_CHECKRETURN || next == JSOP_CHECKTHISREINIT);
    }
#endif

    /*
     * Skip the same-compartment assertion if the local will be immediately
     * popped. We do not guarantee sync for dead locals when coming in from the
     * method JIT, and a GETLOCAL followed by POP is not considered to be
     * a use of the variable.
     */
    if (REGS.pc[JSOP_GETLOCAL_LENGTH] != JSOP_POP)
        assertSameCompartmentDebugOnly(cx, REGS.sp[-1]);
}
END_CASE(JSOP_GETLOCAL)

CASE(JSOP_SETLOCAL)
{
    uint32_t i = GET_LOCALNO(REGS.pc);

    MOZ_ASSERT(!IsUninitializedLexical(REGS.fp()->unaliasedLocal(i)));

    REGS.fp()->unaliasedLocal(i) = REGS.sp[-1];
}
END_CASE(JSOP_SETLOCAL)

CASE(JSOP_DEFVAR)
{
    /* ES5 10.5 step 8 (with subsequent errata). */
    unsigned attrs = JSPROP_ENUMERATE;
    if (!REGS.fp()->isEvalFrame())
        attrs |= JSPROP_PERMANENT;

    /* Step 8b. */
    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.fp()->varObj());
    ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

    if (!DefVarOperation(cx, obj, name, attrs))
        goto error;
}
END_CASE(JSOP_DEFVAR)

CASE(JSOP_DEFCONST)
CASE(JSOP_DEFLET)
{
    LexicalEnvironmentObject* lexicalEnv;
    JSObject* varObj;
    if (script->hasNonSyntacticScope()) {
        lexicalEnv = &REGS.fp()->extensibleLexicalEnvironment();
        varObj = &REGS.fp()->varObj();
    } else {
        lexicalEnv = &cx->global()->lexicalEnvironment();
        varObj = cx->global();
    }
    if (!DefLexicalOperation(cx, lexicalEnv, varObj, script, REGS.pc))
        goto error;
}
END_CASE(JSOP_DEFLET)

CASE(JSOP_DEFFUN)
{
    /*
     * A top-level function defined in Global or Eval code (see ECMA-262
     * Ed. 3), or else a SpiderMonkey extension: a named function statement in
     * a compound statement (not at the top statement level of global code, or
     * at the top level of a function body).
     */
    ReservedRooted<JSFunction*> fun(&rootFunction0, &REGS.sp[-1].toObject().as<JSFunction>());
    if (!DefFunOperation(cx, script, REGS.fp()->environmentChain(), fun))
        goto error;
    REGS.sp--;
}
END_CASE(JSOP_DEFFUN)

CASE(JSOP_LAMBDA)
{
    /* Load the specified function object literal. */
    ReservedRooted<JSFunction*> fun(&rootFunction0, script->getFunction(GET_UINT32_INDEX(REGS.pc)));
    JSObject* obj = Lambda(cx, fun, REGS.fp()->environmentChain());
    if (!obj)
        goto error;

    MOZ_ASSERT(obj->staticPrototype());
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_LAMBDA)

CASE(JSOP_LAMBDA_ARROW)
{
    /* Load the specified function object literal. */
    ReservedRooted<JSFunction*> fun(&rootFunction0, script->getFunction(GET_UINT32_INDEX(REGS.pc)));
    ReservedRooted<Value> newTarget(&rootValue1, REGS.sp[-1]);
    JSObject* obj = LambdaArrow(cx, fun, REGS.fp()->environmentChain(), newTarget);
    if (!obj)
        goto error;

    MOZ_ASSERT(obj->staticPrototype());
    REGS.sp[-1].setObject(*obj);
}
END_CASE(JSOP_LAMBDA_ARROW)

CASE(JSOP_TOASYNC)
{
    ReservedRooted<JSFunction*> unwrapped(&rootFunction0,
                                          &REGS.sp[-1].toObject().as<JSFunction>());
    JSObject* wrapped = WrapAsyncFunction(cx, unwrapped);
    if (!wrapped)
        goto error;

    REGS.sp[-1].setObject(*wrapped);
}
END_CASE(JSOP_TOASYNC)

CASE(JSOP_TOASYNCGEN)
{
    ReservedRooted<JSFunction*> unwrapped(&rootFunction0,
                                          &REGS.sp[-1].toObject().as<JSFunction>());
    JSObject* wrapped = WrapAsyncGenerator(cx, unwrapped);
    if (!wrapped)
        goto error;

    REGS.sp[-1].setObject(*wrapped);
}
END_CASE(JSOP_TOASYNCGEN)

CASE(JSOP_TOASYNCITER)
{
    ReservedRooted<Value> nextMethod(&rootValue0, REGS.sp[-1]);
    ReservedRooted<JSObject*> iter(&rootObject1, &REGS.sp[-2].toObject());
    JSObject* asyncIter = CreateAsyncFromSyncIterator(cx, iter, nextMethod);
    if (!asyncIter)
        goto error;

    REGS.sp--;
    REGS.sp[-1].setObject(*asyncIter);
}
END_CASE(JSOP_TOASYNCITER)

CASE(JSOP_SETFUNNAME)
{
    MOZ_ASSERT(REGS.stackDepth() >= 2);
    FunctionPrefixKind prefixKind = FunctionPrefixKind(GET_UINT8(REGS.pc));
    ReservedRooted<Value> name(&rootValue0, REGS.sp[-1]);
    ReservedRooted<JSFunction*> fun(&rootFunction0, &REGS.sp[-2].toObject().as<JSFunction>());
    if (!SetFunctionNameIfNoOwnName(cx, fun, name, prefixKind))
        goto error;

    REGS.sp--;
}
END_CASE(JSOP_SETFUNNAME)

CASE(JSOP_CALLEE)
    MOZ_ASSERT(REGS.fp()->isFunctionFrame());
    PUSH_COPY(REGS.fp()->calleev());
END_CASE(JSOP_CALLEE)

CASE(JSOP_INITPROP_GETTER)
CASE(JSOP_INITHIDDENPROP_GETTER)
CASE(JSOP_INITPROP_SETTER)
CASE(JSOP_INITHIDDENPROP_SETTER)
{
    MOZ_ASSERT(REGS.stackDepth() >= 2);

    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());
    ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
    ReservedRooted<JSObject*> val(&rootObject1, &REGS.sp[-1].toObject());

    if (!InitGetterSetterOperation(cx, REGS.pc, obj, name, val))
        goto error;

    REGS.sp--;
}
END_CASE(JSOP_INITPROP_GETTER)

CASE(JSOP_INITELEM_GETTER)
CASE(JSOP_INITHIDDENELEM_GETTER)
CASE(JSOP_INITELEM_SETTER)
CASE(JSOP_INITHIDDENELEM_SETTER)
{
    MOZ_ASSERT(REGS.stackDepth() >= 3);

    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-3].toObject());
    ReservedRooted<Value> idval(&rootValue0, REGS.sp[-2]);
    ReservedRooted<JSObject*> val(&rootObject1, &REGS.sp[-1].toObject());

    if (!InitGetterSetterOperation(cx, REGS.pc, obj, idval, val))
        goto error;

    REGS.sp -= 2;
}
END_CASE(JSOP_INITELEM_GETTER)

CASE(JSOP_HOLE)
    PUSH_MAGIC(JS_ELEMENTS_HOLE);
END_CASE(JSOP_HOLE)

CASE(JSOP_NEWINIT)
{
    uint8_t i = GET_UINT8(REGS.pc);
    MOZ_ASSERT(i == JSProto_Array || i == JSProto_Object);

    JSObject* obj;
    if (i == JSProto_Array)
        obj = NewArrayOperation(cx, script, REGS.pc, 0);
    else
        obj = NewObjectOperation(cx, script, REGS.pc);

    if (!obj)
        goto error;
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_NEWINIT)

CASE(JSOP_NEWARRAY)
{
    uint32_t length = GET_UINT32(REGS.pc);
    JSObject* obj = NewArrayOperation(cx, script, REGS.pc, length);
    if (!obj)
        goto error;
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_NEWARRAY)

CASE(JSOP_NEWARRAY_COPYONWRITE)
{
    ReservedRooted<JSObject*> baseobj(&rootObject0, ObjectGroup::getOrFixupCopyOnWriteObject(cx, script, REGS.pc));
    if (!baseobj)
        goto error;

    ReservedRooted<JSObject*> obj(&rootObject1, NewDenseCopyOnWriteArray(cx, ((RootedObject&)(baseobj)).as<ArrayObject>(), gc::DefaultHeap));
    if (!obj)
        goto error;

    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_NEWARRAY_COPYONWRITE)

CASE(JSOP_NEWOBJECT)
{
    JSObject* obj = NewObjectOperation(cx, script, REGS.pc);
    if (!obj)
        goto error;
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_NEWOBJECT)

CASE(JSOP_MUTATEPROTO)
{
    MOZ_ASSERT(REGS.stackDepth() >= 2);

    if (REGS.sp[-1].isObjectOrNull()) {
        ReservedRooted<JSObject*> newProto(&rootObject1, REGS.sp[-1].toObjectOrNull());
        ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());
        MOZ_ASSERT(obj->is<PlainObject>());

        if (!SetPrototype(cx, obj, newProto))
            goto error;
    }

    REGS.sp--;
}
END_CASE(JSOP_MUTATEPROTO)

CASE(JSOP_INITPROP)
CASE(JSOP_INITLOCKEDPROP)
CASE(JSOP_INITHIDDENPROP)
{
    static_assert(JSOP_INITPROP_LENGTH == JSOP_INITLOCKEDPROP_LENGTH,
                  "initprop and initlockedprop must be the same size");
    static_assert(JSOP_INITPROP_LENGTH == JSOP_INITHIDDENPROP_LENGTH,
                  "initprop and inithiddenprop must be the same size");
    /* Load the property's initial value into rval. */
    MOZ_ASSERT(REGS.stackDepth() >= 2);
    ReservedRooted<Value> rval(&rootValue0, REGS.sp[-1]);

    /* Load the object being initialized into lval/obj. */
    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());

    ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

    if (!InitPropertyOperation(cx, JSOp(*REGS.pc), obj, name, rval))
        goto error;

    REGS.sp--;
}
END_CASE(JSOP_INITPROP)

CASE(JSOP_INITELEM)
CASE(JSOP_INITHIDDENELEM)
{
    MOZ_ASSERT(REGS.stackDepth() >= 3);
    HandleValue val = REGS.stackHandleAt(-1);
    HandleValue id = REGS.stackHandleAt(-2);

    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-3].toObject());

    if (!InitElemOperation(cx, REGS.pc, obj, id, val))
        goto error;

    REGS.sp -= 2;
}
END_CASE(JSOP_INITELEM)

CASE(JSOP_INITELEM_ARRAY)
{
    MOZ_ASSERT(REGS.stackDepth() >= 2);
    HandleValue val = REGS.stackHandleAt(-1);

    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());

    uint32_t index = GET_UINT32(REGS.pc);
    if (!InitArrayElemOperation(cx, REGS.pc, obj, index, val))
        goto error;

    REGS.sp--;
}
END_CASE(JSOP_INITELEM_ARRAY)

CASE(JSOP_INITELEM_INC)
{
    MOZ_ASSERT(REGS.stackDepth() >= 3);
    HandleValue val = REGS.stackHandleAt(-1);

    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-3].toObject());

    uint32_t index = REGS.sp[-2].toInt32();
    if (!InitArrayElemOperation(cx, REGS.pc, obj, index, val))
        goto error;

    REGS.sp[-2].setInt32(index + 1);
    REGS.sp--;
}
END_CASE(JSOP_INITELEM_INC)

CASE(JSOP_GOSUB)
{
    PUSH_BOOLEAN(false);
    int32_t i = script->pcToOffset(REGS.pc) + JSOP_GOSUB_LENGTH;
    int32_t len = GET_JUMP_OFFSET(REGS.pc);
    PUSH_INT32(i);
    ADVANCE_AND_DISPATCH(len);
}

CASE(JSOP_RETSUB)
{
    /* Pop [exception or hole, retsub pc-index]. */
    Value rval, lval;
    POP_COPY_TO(rval);
    POP_COPY_TO(lval);
    MOZ_ASSERT(lval.isBoolean());
    if (lval.toBoolean()) {
        /*
         * Exception was pending during finally, throw it *before* we adjust
         * pc, because pc indexes into script->trynotes.  This turns out not to
         * be necessary, but it seems clearer.  And it points out a FIXME:
         * 350509, due to Igor Bukanov.
         */
        cx->setPendingException(rval);
        goto error;
    }
    MOZ_ASSERT(rval.isInt32());

    /* Increment the PC by this much. */
    int32_t len = rval.toInt32() - int32_t(script->pcToOffset(REGS.pc));
    ADVANCE_AND_DISPATCH(len);
}

CASE(JSOP_EXCEPTION)
{
    PUSH_NULL();
    MutableHandleValue res = REGS.stackHandleAt(-1);
    if (!GetAndClearException(cx, res))
        goto error;
}
END_CASE(JSOP_EXCEPTION)

CASE(JSOP_FINALLY)
    CHECK_BRANCH();
END_CASE(JSOP_FINALLY)

CASE(JSOP_THROWING)
{
    ReservedRooted<Value> v(&rootValue0);
    POP_COPY_TO(v);
    MOZ_ALWAYS_TRUE(ThrowingOperation(cx, v));
}
END_CASE(JSOP_THROWING)

CASE(JSOP_THROW)
{
    CHECK_BRANCH();
    ReservedRooted<Value> v(&rootValue0);
    POP_COPY_TO(v);
    JS_ALWAYS_FALSE(Throw(cx, v));
    /* let the code at error try to catch the exception. */
    goto error;
}

CASE(JSOP_INSTANCEOF)
{
    ReservedRooted<Value> rref(&rootValue0, REGS.sp[-1]);
    if (HandleValue(rref).isPrimitive()) {
        ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS, -1, rref, nullptr);
        goto error;
    }
    ReservedRooted<JSObject*> obj(&rootObject0, &rref.toObject());
    bool cond = false;
    if (!HasInstance(cx, obj, REGS.stackHandleAt(-2), &cond))
        goto error;
    REGS.sp--;
    REGS.sp[-1].setBoolean(cond);
}
END_CASE(JSOP_INSTANCEOF)

CASE(JSOP_DEBUGGER)
{
    RootedValue rval(cx);
    switch (Debugger::onDebuggerStatement(cx, REGS.fp())) {
      case JSTRAP_ERROR:
        goto error;
      case JSTRAP_CONTINUE:
        break;
      case JSTRAP_RETURN:
        if (!ForcedReturn(cx, REGS))
            goto error;
        goto successful_return_continuation;
      case JSTRAP_THROW:
        goto error;
      default:;
    }
}
END_CASE(JSOP_DEBUGGER)

CASE(JSOP_PUSHLEXICALENV)
{
    ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

    // Create block environment and push on scope chain.
    if (!REGS.fp()->pushLexicalEnvironment(cx, scope.as<LexicalScope>()))
        goto error;
}
END_CASE(JSOP_PUSHLEXICALENV)

CASE(JSOP_POPLEXICALENV)
{
#ifdef DEBUG
    // Pop block from scope chain.
    Scope* scope = script->lookupScope(REGS.pc);
    MOZ_ASSERT(scope);
    MOZ_ASSERT(scope->is<LexicalScope>());
    MOZ_ASSERT(scope->as<LexicalScope>().hasEnvironment());
#endif

    if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);

    // Pop block from scope chain.
    REGS.fp()->popOffEnvironmentChain<LexicalEnvironmentObject>();
}
END_CASE(JSOP_POPLEXICALENV)

CASE(JSOP_DEBUGLEAVELEXICALENV)
{
    MOZ_ASSERT(script->lookupScope(REGS.pc));
    MOZ_ASSERT(script->lookupScope(REGS.pc)->is<LexicalScope>());
    MOZ_ASSERT(!script->lookupScope(REGS.pc)->as<LexicalScope>().hasEnvironment());

    // FIXME: This opcode should not be necessary.  The debugger shouldn't need
    // help from bytecode to do its job.  See bug 927782.

    if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);
}
END_CASE(JSOP_DEBUGLEAVELEXICALENV)

CASE(JSOP_FRESHENLEXICALENV)
{
    if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);

    if (!REGS.fp()->freshenLexicalEnvironment(cx))
        goto error;
}
END_CASE(JSOP_FRESHENLEXICALENV)

CASE(JSOP_RECREATELEXICALENV)
{
    if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);

    if (!REGS.fp()->recreateLexicalEnvironment(cx))
        goto error;
}
END_CASE(JSOP_RECREATELEXICALENV)

CASE(JSOP_PUSHVARENV)
{
    ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

    if (!REGS.fp()->pushVarEnvironment(cx, scope))
        goto error;
}
END_CASE(JSOP_PUSHVARENV)

CASE(JSOP_POPVARENV)
{
#ifdef DEBUG
    Scope* scope = script->lookupScope(REGS.pc);
    MOZ_ASSERT(scope);
    MOZ_ASSERT(scope->is<VarScope>());
    MOZ_ASSERT(scope->as<VarScope>().hasEnvironment());
#endif

    if (MOZ_UNLIKELY(cx->compartment()->isDebuggee()))
        DebugEnvironments::onPopVar(cx, REGS.fp(), REGS.pc);

    REGS.fp()->popOffEnvironmentChain<VarEnvironmentObject>();
}
END_CASE(JSOP_POPVARENV)

CASE(JSOP_GENERATOR)
{
    MOZ_ASSERT(!cx->isExceptionPending());
    MOZ_ASSERT(REGS.stackDepth() == 0);
    JSObject* obj = GeneratorObject::create(cx, REGS.fp());
    if (!obj)
        goto error;
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_GENERATOR)

CASE(JSOP_INITIALYIELD)
{
    MOZ_ASSERT(!cx->isExceptionPending());
    MOZ_ASSERT(REGS.fp()->isFunctionFrame());
    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-1].toObject());
    POP_RETURN_VALUE();
    MOZ_ASSERT(REGS.stackDepth() == 0);
    if (!GeneratorObject::initialSuspend(cx, obj, REGS.fp(), REGS.pc))
        goto error;
    goto successful_return_continuation;
}

CASE(JSOP_YIELD)
CASE(JSOP_AWAIT)
{
    MOZ_ASSERT(!cx->isExceptionPending());
    MOZ_ASSERT(REGS.fp()->isFunctionFrame());
    ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-1].toObject());
    if (!GeneratorObject::normalSuspend(cx, obj, REGS.fp(), REGS.pc,
                                        REGS.spForStackDepth(0), REGS.stackDepth() - 2))
    {
        goto error;
    }

    REGS.sp--;
    POP_RETURN_VALUE();

    goto successful_return_continuation;
}

CASE(JSOP_RESUME)
{
    {
        ReservedRooted<JSObject*> gen(&rootObject0, &REGS.sp[-2].toObject());
        ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
        // popInlineFrame expects there to be an additional value on the stack
        // to pop off, so leave "gen" on the stack.

        GeneratorObject::ResumeKind resumeKind = GeneratorObject::getResumeKind(REGS.pc);
        bool ok = GeneratorObject::resume(cx, activation, gen, val, resumeKind);
        SET_SCRIPT(REGS.fp()->script());

        TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
        TraceLoggerEvent scriptEvent(TraceLogger_Scripts, script);
        TraceLogStartEvent(logger, scriptEvent);
        TraceLogStartEvent(logger, TraceLogger_Interpreter);

        if (!ok)
            goto error;
    }
    ADVANCE_AND_DISPATCH(0);
}

CASE(JSOP_DEBUGAFTERYIELD)
{
    // No-op in the interpreter, as GeneratorObject::resume takes care of
    // fixing up InterpreterFrames.
    MOZ_ASSERT_IF(REGS.fp()->script()->isDebuggee(), REGS.fp()->isDebuggee());
}
END_CASE(JSOP_DEBUGAFTERYIELD)

CASE(JSOP_FINALYIELDRVAL)
{
    ReservedRooted<JSObject*> gen(&rootObject0, &REGS.sp[-1].toObject());
    REGS.sp--;
    GeneratorObject::finalSuspend(gen);
    goto successful_return_continuation;
}

CASE(JSOP_CHECKCLASSHERITAGE)
{
    HandleValue heritage = REGS.stackHandleAt(-1);

    if (!CheckClassHeritageOperation(cx, heritage))
        goto error;
}
END_CASE(JSOP_CHECKCLASSHERITAGE)

CASE(JSOP_BUILTINPROTO)
{
    MOZ_ASSERT(GET_UINT8(REGS.pc) < JSProto_LIMIT);
    JSProtoKey key = static_cast<JSProtoKey>(GET_UINT8(REGS.pc));
    JSObject* builtin = GlobalObject::getOrCreatePrototype(cx, key);
    if (!builtin)
        goto error;
    PUSH_OBJECT(*builtin);
}
END_CASE(JSOP_BUILTINPROTO)

CASE(JSOP_FUNWITHPROTO)
{
    ReservedRooted<JSObject*> proto(&rootObject1, &REGS.sp[-1].toObject());

    /* Load the specified function object literal. */
    ReservedRooted<JSFunction*> fun(&rootFunction0, script->getFunction(GET_UINT32_INDEX(REGS.pc)));

    JSObject* obj = FunWithProtoOperation(cx, fun, REGS.fp()->environmentChain(), proto);
    if (!obj)
        goto error;

    REGS.sp[-1].setObject(*obj);
}
END_CASE(JSOP_FUNWITHPROTO)

CASE(JSOP_OBJWITHPROTO)
{
    JSObject* obj = ObjectWithProtoOperation(cx, REGS.stackHandleAt(-1));
    if (!obj)
        goto error;

    REGS.sp[-1].setObject(*obj);
}
END_CASE(JSOP_OBJWITHPROTO)

CASE(JSOP_INITHOMEOBJECT)
{
    unsigned skipOver = GET_UINT8(REGS.pc);
    MOZ_ASSERT(REGS.stackDepth() >= skipOver + 2);

    /* Load the function to be initialized */
    ReservedRooted<JSFunction*> func(&rootFunction0, &REGS.sp[-1].toObject().as<JSFunction>());
    MOZ_ASSERT(func->allowSuperProperty());

    /* Load the home object */
    ReservedRooted<JSObject*> obj(&rootObject0);
    obj = &REGS.sp[int(-2 - skipOver)].toObject();
    MOZ_ASSERT(obj->is<PlainObject>() || obj->is<UnboxedPlainObject>() || obj->is<JSFunction>());

    func->setExtendedSlot(FunctionExtended::METHOD_HOMEOBJECT_SLOT, ObjectValue(*obj));
}
END_CASE(JSOP_INITHOMEOBJECT)

CASE(JSOP_SUPERBASE)
{
    JSFunction& superEnvFunc = GetSuperEnvFunction(cx, REGS);
    MOZ_ASSERT(superEnvFunc.allowSuperProperty());
    MOZ_ASSERT(superEnvFunc.nonLazyScript()->needsHomeObject());
    const Value& homeObjVal = superEnvFunc.getExtendedSlot(FunctionExtended::METHOD_HOMEOBJECT_SLOT);

    ReservedRooted<JSObject*> homeObj(&rootObject0, &homeObjVal.toObject());
    ReservedRooted<JSObject*> superBase(&rootObject1);
    superBase = HomeObjectSuperBase(cx, homeObj);
    if (!superBase)
        goto error;

    PUSH_OBJECT(*superBase);
}
END_CASE(JSOP_SUPERBASE)

CASE(JSOP_NEWTARGET)
    PUSH_COPY(REGS.fp()->newTarget());
    MOZ_ASSERT(REGS.sp[-1].isObject() || REGS.sp[-1].isUndefined());
END_CASE(JSOP_NEWTARGET)

CASE(JSOP_SUPERFUN)
{
    ReservedRooted<JSObject*> superEnvFunc(&rootObject0, &GetSuperEnvFunction(cx, REGS));
    ReservedRooted<JSObject*> superFun(&rootObject1);
    superFun = SuperFunOperation(cx, superEnvFunc);
    if (!superFun)
        goto error;

    PUSH_OBJECT(*superFun);
}
END_CASE(JSOP_SUPERFUN)

CASE(JSOP_DERIVEDCONSTRUCTOR)
{
    MOZ_ASSERT(REGS.sp[-1].isObject());
    ReservedRooted<JSObject*> proto(&rootObject0, &REGS.sp[-1].toObject());

    JSFunction* constructor = MakeDefaultConstructor(cx, script, REGS.pc, proto);
    if (!constructor)
        goto error;

    REGS.sp[-1].setObject(*constructor);
}
END_CASE(JSOP_DERIVEDCONSTRUCTOR)

CASE(JSOP_CLASSCONSTRUCTOR)
{
    JSFunction* constructor = MakeDefaultConstructor(cx, script, REGS.pc, nullptr);
    if (!constructor)
        goto error;
    PUSH_OBJECT(*constructor);
}
END_CASE(JSOP_CLASSCONSTRUCTOR)

CASE(JSOP_CHECKOBJCOERCIBLE)
{
    ReservedRooted<Value> checkVal(&rootValue0, REGS.sp[-1]);
    if (checkVal.isNullOrUndefined() && !ToObjectFromStack(cx, checkVal))
        goto error;
}
END_CASE(JSOP_CHECKOBJCOERCIBLE)

CASE(JSOP_DEBUGCHECKSELFHOSTED)
{
#ifdef DEBUG
    ReservedRooted<Value> checkVal(&rootValue0, REGS.sp[-1]);
    if (!Debug_CheckSelfHosted(cx, checkVal))
        goto error;
#endif
}
END_CASE(JSOP_DEBUGCHECKSELFHOSTED)

CASE(JSOP_IS_CONSTRUCTING)
    PUSH_MAGIC(JS_IS_CONSTRUCTING);
END_CASE(JSOP_IS_CONSTRUCTING)

DEFAULT()
{
    char numBuf[12];
    SprintfLiteral(numBuf, "%d", *REGS.pc);
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_BYTECODE, numBuf);
    goto error;
}

} /* interpreter loop */

    MOZ_CRASH("Interpreter loop exited via fallthrough");

  error:
    switch (HandleError(cx, REGS)) {
      case SuccessfulReturnContinuation:
        goto successful_return_continuation;

      case ErrorReturnContinuation:
        interpReturnOK = false;
        goto return_continuation;

      case CatchContinuation:
        ADVANCE_AND_DISPATCH(0);

      case FinallyContinuation: {
        /*
         * Push (true, exception) pair for finally to indicate that [retsub]
         * should rethrow the exception.
         */
        ReservedRooted<Value> exception(&rootValue0);
        if (!cx->getPendingException(&exception)) {
            interpReturnOK = false;
            goto return_continuation;
        }
        PUSH_BOOLEAN(true);
        PUSH_COPY(exception);
        cx->clearPendingException();
      }
      ADVANCE_AND_DISPATCH(0);
    }

    MOZ_CRASH("Invalid HandleError continuation");

  exit:
    if (MOZ_LIKELY(!frameHalfInitialized)) {
        interpReturnOK = Debugger::onLeaveFrame(cx, REGS.fp(), REGS.pc, interpReturnOK);

        REGS.fp()->epilogue(cx, REGS.pc);
    }

    gc::MaybeVerifyBarriers(cx, true);

    TraceLogStopEvent(logger, TraceLogger_Engine);
    TraceLogStopEvent(logger, scriptEvent);

    /*
     * This path is used when it's guaranteed the method can be finished
     * inside the JIT.
     */
  leave_on_safe_point:

    if (interpReturnOK)
        state.setReturnValue(activation.entryFrame()->returnValue());

    return interpReturnOK;

  prologue_error:
    interpReturnOK = false;
    frameHalfInitialized = true;
    goto prologue_return_continuation;
}

bool
js::Throw(JSContext* cx, HandleValue v)
{
    MOZ_ASSERT(!cx->isExceptionPending());
    cx->setPendingException(v);
    return false;
}

bool
js::ThrowingOperation(JSContext* cx, HandleValue v)
{
    // Like js::Throw, but returns |true| instead of |false| to continue
    // execution instead of calling the (JIT) exception handler.

    MOZ_ASSERT(!cx->isExceptionPending());
    cx->setPendingException(v);
    return true;
}

bool
js::GetProperty(JSContext* cx, HandleValue v, HandlePropertyName name, MutableHandleValue vp)
{
    if (name == cx->names().length) {
        // Fast path for strings, arrays and arguments.
        if (GetLengthProperty(v, vp))
            return true;
    }

    // Optimize common cases like (2).toString() or "foo".valueOf() to not
    // create a wrapper object.
    if (v.isPrimitive() && !v.isNullOrUndefined()) {
        NativeObject* proto;
        if (v.isNumber()) {
            proto = GlobalObject::getOrCreateNumberPrototype(cx, cx->global());
        } else if (v.isString()) {
            proto = GlobalObject::getOrCreateStringPrototype(cx, cx->global());
        } else if (v.isBoolean()) {
            proto = GlobalObject::getOrCreateBooleanPrototype(cx, cx->global());
        } else {
            MOZ_ASSERT(v.isSymbol());
            proto = GlobalObject::getOrCreateSymbolPrototype(cx, cx->global());
        }
        if (!proto)
            return false;

        if (GetPropertyPure(cx, proto, NameToId(name), vp.address()))
            return true;
    }

    RootedValue receiver(cx, v);
    RootedObject obj(cx, ToObjectFromStack(cx, v));
    if (!obj)
        return false;

    return GetProperty(cx, obj, receiver, name, vp);
}

JSObject*
js::Lambda(JSContext* cx, HandleFunction fun, HandleObject parent)
{
    MOZ_ASSERT(!fun->isArrow());

    JSFunction* clone;
    if (fun->isNative()) {
        MOZ_ASSERT(IsAsmJSModule(fun));
        clone = CloneAsmJSModuleFunction(cx, fun);
    } else {
        clone = CloneFunctionObjectIfNotSingleton(cx, fun, parent);
    }
    if (!clone)
        return nullptr;

    MOZ_ASSERT(fun->global() == clone->global());
    return clone;
}

JSObject*
js::LambdaArrow(JSContext* cx, HandleFunction fun, HandleObject parent, HandleValue newTargetv)
{
    MOZ_ASSERT(fun->isArrow());

    JSFunction* clone = CloneFunctionObjectIfNotSingleton(cx, fun, parent);
    if (!clone)
        return nullptr;

    MOZ_ASSERT(clone->isArrow());
    clone->setExtendedSlot(0, newTargetv);

    MOZ_ASSERT(fun->global() == clone->global());
    return clone;
}

bool
js::DefFunOperation(JSContext* cx, HandleScript script, HandleObject envChain,
                    HandleFunction fun)
{
    /*
     * We define the function as a property of the variable object and not the
     * current scope chain even for the case of function expression statements
     * and functions defined by eval inside let or with blocks.
     */
    RootedObject parent(cx, envChain);
    while (!parent->isQualifiedVarObj())
        parent = parent->enclosingEnvironment();

    /* ES5 10.5 (NB: with subsequent errata). */
    RootedPropertyName name(cx, fun->explicitName()->asPropertyName());

    Rooted<PropertyResult> prop(cx);
    RootedObject pobj(cx);
    if (!LookupProperty(cx, parent, name, &pobj, &prop))
        return false;

    RootedValue rval(cx, ObjectValue(*fun));

    /*
     * ECMA requires functions defined when entering Eval code to be
     * impermanent.
     */
    unsigned attrs = script->isActiveEval()
                     ? JSPROP_ENUMERATE
                     : JSPROP_ENUMERATE | JSPROP_PERMANENT;

    /* Steps 5d, 5f. */
    if (!prop || pobj != parent) {
        if (!DefineDataProperty(cx, parent, name, rval, attrs))
            return false;

        return parent->is<GlobalObject>() ? parent->compartment()->addToVarNames(cx, name) : true;
    }

    /*
     * Step 5e.
     *
     * A DebugEnvironmentProxy is okay here, and sometimes necessary. If
     * Debugger.Frame.prototype.eval defines a function with the same name as an
     * extant variable in the frame, the DebugEnvironmentProxy takes care of storing
     * the function in the stack frame (for non-aliased variables) or on the
     * scope object (for aliased).
     */
    MOZ_ASSERT(parent->isNative() || parent->is<DebugEnvironmentProxy>());
    if (parent->is<GlobalObject>()) {
        Shape* shape = prop.shape();
        if (shape->configurable()) {
            if (!DefineDataProperty(cx, parent, name, rval, attrs))
                return false;
        } else {
            MOZ_ASSERT(shape->isDataDescriptor());
            MOZ_ASSERT(shape->writable());
            MOZ_ASSERT(shape->enumerable());
        }

        // Careful: the presence of a shape, even one appearing to derive from
        // a variable declaration, doesn't mean it's in [[VarNames]].
        if (!parent->compartment()->addToVarNames(cx, name))
            return false;
    }

    /*
     * Non-global properties, and global properties which we aren't simply
     * redefining, must be set.  First, this preserves their attributes.
     * Second, this will produce warnings and/or errors as necessary if the
     * specified Call object property is not writable (const).
     */

    /* Step 5f. */
    RootedId id(cx, NameToId(name));
    return PutProperty(cx, parent, id, rval, script->strict());
}

bool
js::ThrowMsgOperation(JSContext* cx, const unsigned errorNum)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, errorNum);
    return false;
}

bool
js::GetAndClearException(JSContext* cx, MutableHandleValue res)
{
    if (!cx->getPendingException(res))
        return false;
    cx->clearPendingException();

    // Allow interrupting deeply nested exception handling.
    return CheckForInterrupt(cx);
}

template <bool strict>
bool
js::DeletePropertyJit(JSContext* cx, HandleValue v, HandlePropertyName name, bool* bp)
{
    RootedObject obj(cx, ToObjectFromStack(cx, v));
    if (!obj)
        return false;

    RootedId id(cx, NameToId(name));
    ObjectOpResult result;
    if (!DeleteProperty(cx, obj, id, result))
        return false;

    if (strict) {
        if (!result)
            return result.reportError(cx, obj, id);
        *bp = true;
    } else {
        *bp = result.ok();
    }
    return true;
}

template bool js::DeletePropertyJit<true> (JSContext* cx, HandleValue val, HandlePropertyName name,
                                           bool* bp);
template bool js::DeletePropertyJit<false>(JSContext* cx, HandleValue val, HandlePropertyName name,
                                           bool* bp);

template <bool strict>
bool
js::DeleteElementJit(JSContext* cx, HandleValue val, HandleValue index, bool* bp)
{
    RootedObject obj(cx, ToObjectFromStack(cx, val));
    if (!obj)
        return false;

    RootedId id(cx);
    if (!ToPropertyKey(cx, index, &id))
        return false;
    ObjectOpResult result;
    if (!DeleteProperty(cx, obj, id, result))
        return false;

    if (strict) {
        if (!result)
            return result.reportError(cx, obj, id);
        *bp = true;
    } else {
        *bp = result.ok();
    }
    return true;
}

template bool js::DeleteElementJit<true> (JSContext*, HandleValue, HandleValue, bool* succeeded);
template bool js::DeleteElementJit<false>(JSContext*, HandleValue, HandleValue, bool* succeeded);

bool
js::GetElement(JSContext* cx, MutableHandleValue lref, HandleValue rref, MutableHandleValue vp)
{
    return GetElementOperation(cx, JSOP_GETELEM, lref, rref, vp);
}

bool
js::CallElement(JSContext* cx, MutableHandleValue lref, HandleValue rref, MutableHandleValue res)
{
    return GetElementOperation(cx, JSOP_CALLELEM, lref, rref, res);
}

bool
js::SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index, HandleValue value,
                     bool strict)
{
    RootedId id(cx);
    if (!ToPropertyKey(cx, index, &id))
        return false;
    RootedValue receiver(cx, ObjectValue(*obj));
    return SetObjectElementOperation(cx, obj, id, value, receiver, strict);
}

bool
js::SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index, HandleValue value,
                     bool strict, HandleScript script, jsbytecode* pc)
{
    MOZ_ASSERT(pc);
    RootedId id(cx);
    if (!ToPropertyKey(cx, index, &id))
        return false;
    RootedValue receiver(cx, ObjectValue(*obj));
    return SetObjectElementOperation(cx, obj, id, value, receiver, strict, script, pc);
}

bool
js::SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index, HandleValue value,
                     HandleValue receiver, bool strict)
{
    RootedId id(cx);
    if (!ToPropertyKey(cx, index, &id))
        return false;
    return SetObjectElementOperation(cx, obj, id, value, receiver, strict);
}

bool
js::SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index, HandleValue value,
                     HandleValue receiver, bool strict, HandleScript script, jsbytecode* pc)
{
    MOZ_ASSERT(pc);
    RootedId id(cx);
    if (!ToPropertyKey(cx, index, &id))
        return false;
    return SetObjectElementOperation(cx, obj, id, value, receiver, strict, script, pc);
}

bool
js::InitElementArray(JSContext* cx, jsbytecode* pc, HandleObject obj, uint32_t index, HandleValue value)
{
    return InitArrayElemOperation(cx, pc, obj, index, value);
}

bool
js::AddValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res)
{
    return AddOperation(cx, lhs, rhs, res);
}

bool
js::SubValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res)
{
    return SubOperation(cx, lhs, rhs, res);
}

bool
js::MulValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res)
{
    return MulOperation(cx, lhs, rhs, res);
}

bool
js::DivValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res)
{
    return DivOperation(cx, lhs, rhs, res);
}

bool
js::ModValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res)
{
    return ModOperation(cx, lhs, rhs, res);
}

bool
js::UrshValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res)
{
    return UrshOperation(cx, lhs, rhs, res);
}

bool
js::AtomicIsLockFree(JSContext* cx, HandleValue in, int* out)
{
    int i;
    if (!ToInt32(cx, in, &i))
        return false;
    *out = js::jit::AtomicOperations::isLockfreeJS(i);
    return true;
}

bool
js::DeleteNameOperation(JSContext* cx, HandlePropertyName name, HandleObject scopeObj,
                        MutableHandleValue res)
{
    RootedObject scope(cx), pobj(cx);
    Rooted<PropertyResult> prop(cx);
    if (!LookupName(cx, name, scopeObj, &scope, &pobj, &prop))
        return false;

    if (!scope) {
        // Return true for non-existent names.
        res.setBoolean(true);
        return true;
    }

    ObjectOpResult result;
    RootedId id(cx, NameToId(name));
    if (!DeleteProperty(cx, scope, id, result))
        return false;

    bool status = result.ok();
    res.setBoolean(status);

    if (status) {
        // Deleting a name from the global object removes it from [[VarNames]].
        if (pobj == scope && scope->is<GlobalObject>())
            scope->compartment()->removeFromVarNames(name);
    }

    return true;
}

bool
js::ImplicitThisOperation(JSContext* cx, HandleObject scopeObj, HandlePropertyName name,
                          MutableHandleValue res)
{
    RootedObject obj(cx);
    if (!LookupNameWithGlobalDefault(cx, name, scopeObj, &obj))
        return false;

    res.set(ComputeImplicitThis(obj));
    return true;
}

bool
js::RunOnceScriptPrologue(JSContext* cx, HandleScript script)
{
    MOZ_ASSERT(script->treatAsRunOnce());

    if (!script->hasRunOnce()) {
        script->setHasRunOnce();
        return true;
    }

    // Force instantiation of the script's function's group to ensure the flag
    // is preserved in type information.
    RootedFunction fun(cx, script->functionNonDelazifying());
    if (!JSObject::getGroup(cx, fun))
        return false;

    MarkObjectGroupFlags(cx, script->functionNonDelazifying(), OBJECT_FLAG_RUNONCE_INVALIDATED);
    return true;
}

unsigned
js::GetInitDataPropAttrs(JSOp op)
{
    switch (op) {
      case JSOP_INITPROP:
      case JSOP_INITELEM:
        return JSPROP_ENUMERATE;
      case JSOP_INITLOCKEDPROP:
        return JSPROP_PERMANENT | JSPROP_READONLY;
      case JSOP_INITHIDDENPROP:
      case JSOP_INITHIDDENELEM:
        // Non-enumerable, but writable and configurable
        return 0;
      default:;
    }
    MOZ_CRASH("Unknown data initprop");
}

bool
js::InitGetterSetterOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, HandleId id,
                              HandleObject val)
{
    MOZ_ASSERT(val->isCallable());
    GetterOp getter;
    SetterOp setter;

    JSOp op = JSOp(*pc);

    unsigned attrs = 0;
    if (!IsHiddenInitOp(op))
        attrs |= JSPROP_ENUMERATE;

    if (op == JSOP_INITPROP_GETTER || op == JSOP_INITELEM_GETTER ||
        op == JSOP_INITHIDDENPROP_GETTER || op == JSOP_INITHIDDENELEM_GETTER)
    {
        getter = CastAsGetterOp(val);
        setter = nullptr;
        attrs |= JSPROP_GETTER;
    } else {
        MOZ_ASSERT(op == JSOP_INITPROP_SETTER || op == JSOP_INITELEM_SETTER ||
                   op == JSOP_INITHIDDENPROP_SETTER || op == JSOP_INITHIDDENELEM_SETTER);
        getter = nullptr;
        setter = CastAsSetterOp(val);
        attrs |= JSPROP_SETTER;
    }

    return DefineAccessorProperty(cx, obj, id, getter, setter, attrs);
}

bool
js::InitGetterSetterOperation(JSContext* cx, jsbytecode* pc, HandleObject obj,
                              HandlePropertyName name, HandleObject val)
{
    RootedId id(cx, NameToId(name));
    return InitGetterSetterOperation(cx, pc, obj, id, val);
}

bool
js::InitGetterSetterOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, HandleValue idval,
                              HandleObject val)
{
    RootedId id(cx);
    if (!ToPropertyKey(cx, idval, &id))
        return false;

    return InitGetterSetterOperation(cx, pc, obj, id, val);
}

bool
js::SpreadCallOperation(JSContext* cx, HandleScript script, jsbytecode* pc, HandleValue thisv,
                        HandleValue callee, HandleValue arr, HandleValue newTarget, MutableHandleValue res)
{
    RootedArrayObject aobj(cx, &arr.toObject().as<ArrayObject>());
    uint32_t length = aobj->length();
    JSOp op = JSOp(*pc);
    bool constructing = op == JSOP_SPREADNEW || op == JSOP_SPREADSUPERCALL;

    // {Construct,Invoke}Args::init does this too, but this gives us a better
    // error message.
    if (length > ARGS_LENGTH_MAX) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  constructing ? JSMSG_TOO_MANY_CON_SPREADARGS
                                               : JSMSG_TOO_MANY_FUN_SPREADARGS);
        return false;
    }

    // Do our own checks for the callee being a function, as Invoke uses the
    // expression decompiler to decompile the callee stack operand based on
    // the number of arguments. Spread operations have the callee at sp - 3
    // when not constructing, and sp - 4 when constructing.
    if (callee.isPrimitive()) {
        return ReportIsNotFunction(cx, callee, 2 + constructing,
                                   constructing ? CONSTRUCT : NO_CONSTRUCT);
    }

    if (MOZ_UNLIKELY(!callee.toObject().is<JSFunction>()) && !callee.toObject().callHook()) {
        return ReportIsNotFunction(cx, callee, 2 + constructing,
                                   constructing ? CONSTRUCT : NO_CONSTRUCT);
    }

#ifdef DEBUG
    // The object must be an array with dense elements and no holes. Baseline's
    // optimized spread call stubs rely on this.
    MOZ_ASSERT(!aobj->isIndexed());
    MOZ_ASSERT(aobj->getDenseInitializedLength() == aobj->length());
    for (size_t i = 0; i < aobj->length(); i++)
        MOZ_ASSERT(!aobj->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE));
#endif

    if (constructing) {
        if (!StackCheckIsConstructorCalleeNewTarget(cx, callee, newTarget))
            return false;

        ConstructArgs cargs(cx);
        if (!cargs.init(cx, length))
            return false;

        if (!GetElements(cx, aobj, length, cargs.array()))
            return false;

        RootedObject obj(cx);
        if (!Construct(cx, callee, cargs, newTarget, &obj))
            return false;
        res.setObject(*obj);
    } else {
        InvokeArgs args(cx);
        if (!args.init(cx, length))
            return false;

        if (!GetElements(cx, aobj, length, args.array()))
            return false;

        if ((op == JSOP_SPREADEVAL || op == JSOP_STRICTSPREADEVAL) &&
            cx->global()->valueIsEval(callee))
        {
            if (!DirectEval(cx, args.get(0), res))
                return false;
        } else {
            MOZ_ASSERT(op == JSOP_SPREADCALL ||
                       op == JSOP_SPREADEVAL ||
                       op == JSOP_STRICTSPREADEVAL,
                       "bad spread opcode");

            if (!Call(cx, callee, thisv, args, res))
                return false;
        }
    }

    TypeScript::Monitor(cx, script, pc, res);
    return true;
}

bool
js::OptimizeSpreadCall(JSContext* cx, HandleValue arg, bool* optimized)
{
    // Optimize spread call by skipping spread operation when following
    // conditions are met:
    //   * the argument is an array
    //   * the array has no hole
    //   * array[@@iterator] is not modified
    //   * the array's prototype is Array.prototype
    //   * Array.prototype[@@iterator] is not modified
    //   * %ArrayIteratorPrototype%.next is not modified
    if (!arg.isObject()) {
        *optimized = false;
        return true;
    }

    RootedObject obj(cx, &arg.toObject());
    if (!IsPackedArray(obj)) {
        *optimized = false;
        return true;
    }

    ForOfPIC::Chain* stubChain = ForOfPIC::getOrCreate(cx);
    if (!stubChain)
        return false;

    return stubChain->tryOptimizeArray(cx, obj.as<ArrayObject>(), optimized);
}

JSObject*
js::NewObjectOperation(JSContext* cx, HandleScript script, jsbytecode* pc,
                       NewObjectKind newKind /* = GenericObject */)
{
    MOZ_ASSERT(newKind != SingletonObject);

    RootedObjectGroup group(cx);
    if (ObjectGroup::useSingletonForAllocationSite(script, pc, JSProto_Object)) {
        newKind = SingletonObject;
    } else {
        group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Object);
        if (!group)
            return nullptr;
        if (group->maybePreliminaryObjects()) {
            group->maybePreliminaryObjects()->maybeAnalyze(cx, group);
            if (group->maybeUnboxedLayout())
                group->maybeUnboxedLayout()->setAllocationSite(script, pc);
        }

        if (group->shouldPreTenure() || group->maybePreliminaryObjects())
            newKind = TenuredObject;

        if (group->maybeUnboxedLayout())
            return UnboxedPlainObject::create(cx, group, newKind);
    }

    RootedPlainObject obj(cx);

    if (*pc == JSOP_NEWOBJECT) {
        RootedPlainObject baseObject(cx, &script->getObject(pc)->as<PlainObject>());
        obj = CopyInitializerObject(cx, baseObject, newKind);
    } else {
        MOZ_ASSERT(*pc == JSOP_NEWINIT);
        MOZ_ASSERT(GET_UINT8(pc) == JSProto_Object);
        obj = NewBuiltinClassInstance<PlainObject>(cx, newKind);
    }

    if (!obj)
        return nullptr;

    if (newKind == SingletonObject) {
        if (!JSObject::setSingleton(cx, obj))
            return nullptr;
    } else {
        obj->setGroup(group);

        if (PreliminaryObjectArray* preliminaryObjects = group->maybePreliminaryObjects())
            preliminaryObjects->registerNewObject(obj);
    }

    return obj;
}

JSObject*
js::NewObjectOperationWithTemplate(JSContext* cx, HandleObject templateObject)
{
    // This is an optimized version of NewObjectOperation for use when the
    // object is not a singleton and has had its preliminary objects analyzed,
    // with the template object a copy of the object to create.
    MOZ_ASSERT(!templateObject->isSingleton());

    NewObjectKind newKind = templateObject->group()->shouldPreTenure() ? TenuredObject : GenericObject;

    if (templateObject->group()->maybeUnboxedLayout()) {
        RootedObjectGroup group(cx, templateObject->group());
        return UnboxedPlainObject::create(cx, group, newKind);
    }

    JSObject* obj = CopyInitializerObject(cx, templateObject.as<PlainObject>(), newKind);
    if (!obj)
        return nullptr;

    obj->setGroup(templateObject->group());
    return obj;
}

JSObject*
js::NewArrayOperation(JSContext* cx, HandleScript script, jsbytecode* pc, uint32_t length,
                      NewObjectKind newKind /* = GenericObject */)
{
    MOZ_ASSERT(newKind != SingletonObject);

    RootedObjectGroup group(cx);
    if (ObjectGroup::useSingletonForAllocationSite(script, pc, JSProto_Array)) {
        newKind = SingletonObject;
    } else {
        group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Array);
        if (!group)
            return nullptr;
        if (group->maybePreliminaryObjects())
            group->maybePreliminaryObjects()->maybeAnalyze(cx, group);

        if (group->shouldPreTenure() || group->maybePreliminaryObjects())
            newKind = TenuredObject;
    }

    ArrayObject* obj = NewDenseFullyAllocatedArray(cx, length, nullptr, newKind);
    if (!obj)
        return nullptr;

    if (newKind == SingletonObject)
        MOZ_ASSERT(obj->isSingleton());
    else
        obj->setGroup(group);

    return obj;
}

JSObject*
js::NewArrayOperationWithTemplate(JSContext* cx, HandleObject templateObject)
{
    MOZ_ASSERT(!templateObject->isSingleton());

    NewObjectKind newKind = templateObject->group()->shouldPreTenure() ? TenuredObject : GenericObject;

    ArrayObject* obj = NewDenseFullyAllocatedArray(cx, templateObject->as<ArrayObject>().length(),
                                                   nullptr, newKind);
    if (!obj)
        return nullptr;

    MOZ_ASSERT(obj->lastProperty() == templateObject->as<ArrayObject>().lastProperty());
    obj->setGroup(templateObject->group());
    return obj;
}

void
js::ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber, HandleId id)
{
    MOZ_ASSERT(errorNumber == JSMSG_UNINITIALIZED_LEXICAL ||
               errorNumber == JSMSG_BAD_CONST_ASSIGN);
    JSAutoByteString printable;
    if (ValueToPrintable(cx, IdToValue(id), &printable))
        JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, errorNumber, printable.ptr());
}

void
js::ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber, HandlePropertyName name)
{
    RootedId id(cx, NameToId(name));
    ReportRuntimeLexicalError(cx, errorNumber, id);
}

void
js::ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                              HandleScript script, jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    MOZ_ASSERT(op == JSOP_CHECKLEXICAL ||
               op == JSOP_CHECKALIASEDLEXICAL ||
               op == JSOP_THROWSETCONST ||
               op == JSOP_THROWSETALIASEDCONST ||
               op == JSOP_THROWSETCALLEE ||
               op == JSOP_GETIMPORT);

    RootedPropertyName name(cx);

    if (op == JSOP_THROWSETCALLEE) {
        name = script->functionNonDelazifying()->explicitName()->asPropertyName();
    } else if (IsLocalOp(op)) {
        name = FrameSlotName(script, pc)->asPropertyName();
    } else if (IsAtomOp(op)) {
        name = script->getName(pc);
    } else {
        MOZ_ASSERT(IsAliasedVarOp(op));
        name = EnvironmentCoordinateName(cx->caches().envCoordinateNameCache, script, pc);
    }

    ReportRuntimeLexicalError(cx, errorNumber, name);
}

void
js::ReportRuntimeRedeclaration(JSContext* cx, HandlePropertyName name, const char* redeclKind)
{
    JSAutoByteString printable;
    if (AtomToPrintableString(cx, name, &printable)) {
        JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_REDECLARED_VAR,
                                   redeclKind, printable.ptr());
    }
}

bool
js::ThrowCheckIsObject(JSContext* cx, CheckIsObjectKind kind)
{
    switch (kind) {
      case CheckIsObjectKind::IteratorNext:
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_ITER_METHOD_RETURNED_PRIMITIVE, "next");
        break;
      case CheckIsObjectKind::IteratorReturn:
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_ITER_METHOD_RETURNED_PRIMITIVE, "return");
        break;
      case CheckIsObjectKind::IteratorThrow:
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_ITER_METHOD_RETURNED_PRIMITIVE, "throw");
        break;
      case CheckIsObjectKind::GetIterator:
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_GET_ITER_RETURNED_PRIMITIVE);
        break;
      case CheckIsObjectKind::GetAsyncIterator:
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_GET_ASYNC_ITER_RETURNED_PRIMITIVE);
        break;
      default:
        MOZ_CRASH("Unknown kind");
    }
    return false;
}

bool
js::ThrowCheckIsCallable(JSContext* cx, CheckIsCallableKind kind)
{
    switch (kind) {
      case CheckIsCallableKind::IteratorReturn:
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_RETURN_NOT_CALLABLE);
        break;
      default:
        MOZ_CRASH("Unknown kind");
    }
    return false;
}

bool
js::ThrowUninitializedThis(JSContext* cx, AbstractFramePtr frame)
{
    RootedFunction fun(cx);
    if (frame.isFunctionFrame()) {
        fun = frame.callee();
    } else {
        Scope* startingScope;
        if (frame.isDebuggerEvalFrame()) {
            AbstractFramePtr evalInFramePrev = frame.asInterpreterFrame()->evalInFramePrev();
            while (evalInFramePrev.isDebuggerEvalFrame())
                evalInFramePrev = evalInFramePrev.asInterpreterFrame()->evalInFramePrev();
            startingScope = evalInFramePrev.script()->bodyScope();
        } else {
            MOZ_ASSERT(frame.isEvalFrame());
            MOZ_ASSERT(frame.script()->isDirectEvalInFunction());
            startingScope = frame.script()->enclosingScope();
        }

        for (ScopeIter si(startingScope); si; si++) {
            if (si.scope()->is<FunctionScope>()) {
                fun = si.scope()->as<FunctionScope>().canonicalFunction();
                break;
            }
        }
        MOZ_ASSERT(fun);
    }

    if (fun->isDerivedClassConstructor()) {
        const char* name = "anonymous";
        JSAutoByteString str;
        if (fun->explicitName()) {
            if (!AtomToPrintableString(cx, fun->explicitName(), &str))
                return false;
            name = str.ptr();
        }

        JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_UNINITIALIZED_THIS, name);
        return false;
    }

    MOZ_ASSERT(fun->isArrow());
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_UNINITIALIZED_THIS_ARROW);
    return false;
}

JSObject*
js::HomeObjectSuperBase(JSContext* cx, HandleObject homeObj)
{
    RootedObject superBase(cx);

    if (!GetPrototype(cx, homeObj, &superBase))
        return nullptr;

    if (!superBase) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                                  "null", "object");
        return nullptr;
    }

    return superBase;
}

JSObject*
js::SuperFunOperation(JSContext* cx, HandleObject callee)
{
    MOZ_ASSERT(callee->as<JSFunction>().isClassConstructor());
    MOZ_ASSERT(callee->as<JSFunction>().nonLazyScript()->isDerivedClassConstructor());

    RootedObject superFun(cx);

    if (!GetPrototype(cx, callee, &superFun))
        return nullptr;

    RootedValue superFunVal(cx, UndefinedValue());
    if (!superFun)
        superFunVal = NullValue();
    else if (!superFun->isConstructor())
        superFunVal = ObjectValue(*superFun);

    if (superFunVal.isObjectOrNull()) {
        ReportIsNotFunction(cx, superFunVal, JSDVG_IGNORE_STACK, CONSTRUCT);
        return nullptr;
    }

    return superFun;
}

bool
js::ThrowInitializedThis(JSContext* cx)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_REINIT_THIS);
    return false;
}

bool
js::SetPropertySuper(JSContext* cx, HandleObject obj, HandleValue receiver,
                     HandlePropertyName name, HandleValue rval, bool strict)
{
    RootedId id(cx, NameToId(name));
    ObjectOpResult result;
    if (!SetProperty(cx, obj, id, rval, receiver, result))
        return false;

    return result.checkStrictErrorOrWarning(cx, obj, id, strict);
}
