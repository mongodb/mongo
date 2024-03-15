/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/WrappingOperations.h"

#include <string.h>

#include "jsapi.h"
#include "jslibmath.h"
#include "jsmath.h"
#include "jsnum.h"

#include "builtin/Array.h"
#include "builtin/Eval.h"
#include "builtin/ModuleObject.h"
#include "builtin/Object.h"
#include "builtin/Promise.h"
#include "gc/GC.h"
#include "jit/AtomicOperations.h"
#include "jit/BaselineJIT.h"
#include "jit/Jit.h"
#include "jit/JitRuntime.h"
#include "js/experimental/JitInfo.h"  // JSJitInfo
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/friend/WindowProxy.h"    // js::IsWindowProxy
#include "js/Printer.h"
#include "util/CheckedArithmetic.h"
#include "util/StringBuffer.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BigIntType.h"
#include "vm/BytecodeUtil.h"        // JSDVG_SEARCH_STACK
#include "vm/EqualityOperations.h"  // js::StrictlyEqual
#include "vm/GeneratorObject.h"
#include "vm/Iteration.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/Opcodes.h"
#include "vm/PIC.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/SharedStencil.h"  // GCThingIndex
#include "vm/StringType.h"
#include "vm/ThrowMsgKind.h"  // ThrowMsgKind
#include "vm/Time.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "vm/RecordType.h"
#  include "vm/TupleType.h"
#endif

#include "builtin/Boolean-inl.h"
#include "debugger/DebugAPI-inl.h"
#include "vm/ArgumentsObject-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"
#include "vm/PlainObject-inl.h"  // js::CopyInitializerObject, js::CreateThis
#include "vm/Probes-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

using mozilla::DebugOnly;
using mozilla::NumberEqualsInt32;

using js::jit::JitScript;

template <bool Eq>
static MOZ_ALWAYS_INLINE bool LooseEqualityOp(JSContext* cx,
                                              InterpreterRegs& regs) {
  HandleValue rval = regs.stackHandleAt(-1);
  HandleValue lval = regs.stackHandleAt(-2);
  bool cond;
  if (!LooselyEqual(cx, lval, rval, &cond)) {
    return false;
  }
  cond = (cond == Eq);
  regs.sp--;
  regs.sp[-1].setBoolean(cond);
  return true;
}

JSObject* js::BoxNonStrictThis(JSContext* cx, HandleValue thisv) {
  MOZ_ASSERT(!thisv.isMagic());

  if (thisv.isNullOrUndefined()) {
    return cx->global()->lexicalEnvironment().thisObject();
  }

  if (thisv.isObject()) {
    return &thisv.toObject();
  }

  return PrimitiveToObject(cx, thisv);
}

bool js::GetFunctionThis(JSContext* cx, AbstractFramePtr frame,
                         MutableHandleValue res) {
  MOZ_ASSERT(frame.isFunctionFrame());
  MOZ_ASSERT(!frame.callee()->isArrow());

  if (frame.thisArgument().isObject() || frame.callee()->strict()) {
    res.set(frame.thisArgument());
    return true;
  }

  MOZ_ASSERT(!frame.callee()->isSelfHostedBuiltin(),
             "Self-hosted builtins must be strict");

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
        res.setObject(*GetThisObjectOfLexical(env));
        return true;
      }
      if (!env->enclosingEnvironment()) {
        // This can only happen in Debugger eval frames: in that case we
        // don't always have a global lexical env, see EvaluateInEnv.
        MOZ_ASSERT(env->is<GlobalObject>());
        res.setObject(*GetThisObject(env));
        return true;
      }
      env = env->enclosingEnvironment();
    }
  }

  JSObject* obj = BoxNonStrictThis(cx, thisv);
  if (!obj) {
    return false;
  }

  res.setObject(*obj);
  return true;
}

void js::GetNonSyntacticGlobalThis(JSContext* cx, HandleObject envChain,
                                   MutableHandleValue res) {
  RootedObject env(cx, envChain);
  while (true) {
    if (IsExtensibleLexicalEnvironment(env)) {
      res.setObject(*GetThisObjectOfLexical(env));
      return;
    }
    if (!env->enclosingEnvironment()) {
      // This can only happen in Debugger eval frames: in that case we
      // don't always have a global lexical env, see EvaluateInEnv.
      MOZ_ASSERT(env->is<GlobalObject>());
      res.setObject(*GetThisObject(env));
      return;
    }
    env = env->enclosingEnvironment();
  }
}

#ifdef DEBUG
static bool IsSelfHostedOrKnownBuiltinCtor(JSFunction* fun, JSContext* cx) {
  if (fun->isSelfHostedOrIntrinsic()) {
    return true;
  }

  // GetBuiltinConstructor in ArrayGroupToMap
  if (fun == cx->global()->maybeGetConstructor(JSProto_Map)) {
    return true;
  }

  // GetBuiltinConstructor in intlFallbackSymbol
  if (fun == cx->global()->maybeGetConstructor(JSProto_Symbol)) {
    return true;
  }

  // ConstructorForTypedArray in MergeSortTypedArray
  if (fun == cx->global()->maybeGetConstructor(JSProto_Int8Array) ||
      fun == cx->global()->maybeGetConstructor(JSProto_Uint8Array) ||
      fun == cx->global()->maybeGetConstructor(JSProto_Int16Array) ||
      fun == cx->global()->maybeGetConstructor(JSProto_Uint16Array) ||
      fun == cx->global()->maybeGetConstructor(JSProto_Int32Array) ||
      fun == cx->global()->maybeGetConstructor(JSProto_Uint32Array) ||
      fun == cx->global()->maybeGetConstructor(JSProto_Float32Array) ||
      fun == cx->global()->maybeGetConstructor(JSProto_Float64Array) ||
      fun == cx->global()->maybeGetConstructor(JSProto_Uint8ClampedArray) ||
      fun == cx->global()->maybeGetConstructor(JSProto_BigInt64Array) ||
      fun == cx->global()->maybeGetConstructor(JSProto_BigUint64Array)) {
    return true;
  }

  return false;
}
#endif  // DEBUG

bool js::Debug_CheckSelfHosted(JSContext* cx, HandleValue funVal) {
#ifdef DEBUG
  JSFunction* fun = &UncheckedUnwrap(&funVal.toObject())->as<JSFunction>();
  MOZ_ASSERT(IsSelfHostedOrKnownBuiltinCtor(fun, cx),
             "functions directly called inside self-hosted JS must be one of "
             "selfhosted function, self-hosted intrinsic, or known built-in "
             "constructor");
#else
  MOZ_CRASH("self-hosted checks should only be done in Debug builds");
#endif

  // This is purely to police self-hosted code. There is no actual operation.
  return true;
}

static inline bool GetPropertyOperation(JSContext* cx,
                                        Handle<PropertyName*> name,
                                        HandleValue lval,
                                        MutableHandleValue vp) {
  if (name == cx->names().length && GetLengthProperty(lval, vp)) {
    return true;
  }

  return GetProperty(cx, lval, name, vp);
}

static inline bool GetNameOperation(JSContext* cx, HandleObject envChain,
                                    Handle<PropertyName*> name, JSOp nextOp,
                                    MutableHandleValue vp) {
  /* Kludge to allow (typeof foo == "undefined") tests. */
  if (nextOp == JSOp::Typeof) {
    return GetEnvironmentName<GetNameMode::TypeOf>(cx, envChain, name, vp);
  }
  return GetEnvironmentName<GetNameMode::Normal>(cx, envChain, name, vp);
}

bool js::GetImportOperation(JSContext* cx, HandleObject envChain,
                            HandleScript script, jsbytecode* pc,
                            MutableHandleValue vp) {
  RootedObject env(cx), pobj(cx);
  Rooted<PropertyName*> name(cx, script->getName(pc));
  PropertyResult prop;

  MOZ_ALWAYS_TRUE(LookupName(cx, name, envChain, &env, &pobj, &prop));
  MOZ_ASSERT(env && env->is<ModuleEnvironmentObject>());
  MOZ_ASSERT(env->as<ModuleEnvironmentObject>().hasImportBinding(name));
  return FetchName<GetNameMode::Normal>(cx, env, pobj, name, prop, vp);
}

static JSObject* SuperFunOperation(JSObject* callee) {
  MOZ_ASSERT(callee->as<JSFunction>().isClassConstructor());
  MOZ_ASSERT(
      callee->as<JSFunction>().baseScript()->isDerivedClassConstructor());

  return callee->as<JSFunction>().staticPrototype();
}

static JSObject* HomeObjectSuperBase(JSObject* homeObj) {
  MOZ_ASSERT(homeObj->is<PlainObject>() || homeObj->is<JSFunction>());

  return homeObj->staticPrototype();
}

bool js::ReportIsNotFunction(JSContext* cx, HandleValue v, int numToSkip,
                             MaybeConstruct construct) {
  unsigned error = construct ? JSMSG_NOT_CONSTRUCTOR : JSMSG_NOT_FUNCTION;
  int spIndex = numToSkip >= 0 ? -(numToSkip + 1) : JSDVG_SEARCH_STACK;

  ReportValueError(cx, error, spIndex, v, nullptr);
  return false;
}

JSObject* js::ValueToCallable(JSContext* cx, HandleValue v, int numToSkip,
                              MaybeConstruct construct) {
  if (v.isObject() && v.toObject().isCallable()) {
    return &v.toObject();
  }

  ReportIsNotFunction(cx, v, numToSkip, construct);
  return nullptr;
}

static bool MaybeCreateThisForConstructor(JSContext* cx, const CallArgs& args) {
  if (args.thisv().isObject()) {
    return true;
  }

  RootedFunction callee(cx, &args.callee().as<JSFunction>());
  RootedObject newTarget(cx, &args.newTarget().toObject());

  MOZ_ASSERT(callee->hasBytecode());

  if (!CreateThis(cx, callee, newTarget, GenericObject, args.mutableThisv())) {
    return false;
  }

  // Ensure the callee still has a non-lazy script. We normally don't relazify
  // in active compartments, but the .prototype lookup might have called the
  // relazifyFunctions testing function that doesn't have this restriction.
  return JSFunction::getOrCreateScript(cx, callee);
}

#ifdef ENABLE_RECORD_TUPLE
static bool AddRecordSpreadOperation(JSContext* cx, HandleValue recHandle,
                                     HandleValue spreadeeHandle) {
  MOZ_ASSERT(recHandle.toExtendedPrimitive().is<RecordType>());
  RecordType* rec = &recHandle.toExtendedPrimitive().as<RecordType>();

  RootedObject obj(cx, ToObjectOrGetObjectPayload(cx, spreadeeHandle));

  RootedIdVector keys(cx);
  if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY | JSITER_SYMBOLS, &keys)) {
    return false;
  }

  size_t len = keys.length();
  RootedId propKey(cx);
  RootedValue propValue(cx);
  for (size_t i = 0; i < len; i++) {
    propKey.set(keys[i]);

    // Step 4.c.ii.1.
    if (MOZ_UNLIKELY(!GetProperty(cx, obj, obj, propKey, &propValue))) {
      return false;
    }

    if (MOZ_UNLIKELY(!rec->initializeNextProperty(cx, propKey, propValue))) {
      return false;
    }
  }

  return true;
}
#endif

InterpreterFrame* InvokeState::pushInterpreterFrame(JSContext* cx) {
  return cx->interpreterStack().pushInvokeFrame(cx, args_, construct_);
}

InterpreterFrame* ExecuteState::pushInterpreterFrame(JSContext* cx) {
  return cx->interpreterStack().pushExecuteFrame(cx, script_, envChain_,
                                                 evalInFrame_);
}

InterpreterFrame* RunState::pushInterpreterFrame(JSContext* cx) {
  if (isInvoke()) {
    return asInvoke()->pushInterpreterFrame(cx);
  }
  return asExecute()->pushInterpreterFrame(cx);
}

static MOZ_ALWAYS_INLINE bool MaybeEnterInterpreterTrampoline(JSContext* cx,
                                                              RunState& state) {
#ifdef NIGHTLY_BUILD
  if (jit::JitOptions.emitInterpreterEntryTrampoline &&
      cx->runtime()->hasJitRuntime()) {
    js::jit::JitRuntime* jitRuntime = cx->runtime()->jitRuntime();
    JSScript* script = state.script();

    uint8_t* codeRaw = nullptr;
    auto p = jitRuntime->getInterpreterEntryMap()->lookup(script);
    if (p) {
      codeRaw = p->value().raw();
    } else if (js::jit::JitCode* code =
                   jitRuntime->generateEntryTrampolineForScript(cx, script)) {
      js::jit::EntryTrampoline entry(cx, code);
      if (!jitRuntime->getInterpreterEntryMap()->put(script, entry)) {
        return false;
      }
      codeRaw = code->raw();
    }

    MOZ_ASSERT(codeRaw, "Should have a valid trampoline here.");
    // The C++ entry thunk is located at the vmInterpreterEntryOffset offset.
    codeRaw += jitRuntime->vmInterpreterEntryOffset();
    return js::jit::EnterInterpreterEntryTrampoline(codeRaw, cx, &state);
  }
#endif
  return Interpret(cx, state);
}

// MSVC with PGO inlines a lot of functions in RunScript, resulting in large
// stack frames and stack overflow issues, see bug 1167883. Turn off PGO to
// avoid this.
#ifdef _MSC_VER
#  pragma optimize("g", off)
#endif
bool js::RunScript(JSContext* cx, RunState& state) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  MOZ_ASSERT_IF(cx->runtime()->hasJitRuntime(),
                !cx->runtime()->jitRuntime()->disallowArbitraryCode());

  // Since any script can conceivably GC, make sure it's safe to do so.
  cx->verifyIsSafeToGC();

  MOZ_ASSERT(cx->realm() == state.script()->realm());

  MOZ_DIAGNOSTIC_ASSERT(cx->realm()->isSystem() ||
                        cx->runtime()->allowContentJS());

  if (!DebugAPI::checkNoExecute(cx, state.script())) {
    return false;
  }

  GeckoProfilerEntryMarker marker(cx, state.script());

  bool measuringTime = !cx->isMeasuringExecutionTime();
  mozilla::TimeStamp startTime;
  if (measuringTime) {
    cx->setIsMeasuringExecutionTime(true);
    cx->setIsExecuting(true);
    startTime = mozilla::TimeStamp::Now();
  }
  auto timerEnd = mozilla::MakeScopeExit([&]() {
    if (measuringTime) {
      mozilla::TimeDuration delta = mozilla::TimeStamp::Now() - startTime;
      cx->realm()->timers.executionTime += delta;
      cx->setIsMeasuringExecutionTime(false);
      cx->setIsExecuting(false);
    }
  });

  jit::EnterJitStatus status = jit::MaybeEnterJit(cx, state);
  switch (status) {
    case jit::EnterJitStatus::Error:
      return false;
    case jit::EnterJitStatus::Ok:
      return true;
    case jit::EnterJitStatus::NotEntered:
      break;
  }

  bool ok = MaybeEnterInterpreterTrampoline(cx, state);

  return ok;
}
#ifdef _MSC_VER
#  pragma optimize("", on)
#endif

STATIC_PRECONDITION_ASSUME(ubound(args.argv_) >= argc)
MOZ_ALWAYS_INLINE bool CallJSNative(JSContext* cx, Native native,
                                    CallReason reason, const CallArgs& args) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  NativeResumeMode resumeMode = DebugAPI::onNativeCall(cx, args, reason);
  if (resumeMode != NativeResumeMode::Continue) {
    return resumeMode == NativeResumeMode::Override;
  }

#ifdef DEBUG
  bool alreadyThrowing = cx->isExceptionPending();
#endif
  cx->check(args);
  MOZ_ASSERT(!args.callee().is<ProxyObject>());

  AutoRealm ar(cx, &args.callee());
  bool ok = native(cx, args.length(), args.base());
  if (ok) {
    cx->check(args.rval());
    MOZ_ASSERT_IF(!alreadyThrowing, !cx->isExceptionPending());
  }
  return ok;
}

STATIC_PRECONDITION(ubound(args.argv_) >= argc)
MOZ_ALWAYS_INLINE bool CallJSNativeConstructor(JSContext* cx, Native native,
                                               const CallArgs& args) {
#ifdef DEBUG
  RootedObject callee(cx, &args.callee());
#endif

  MOZ_ASSERT(args.thisv().isMagic());
  if (!CallJSNative(cx, native, CallReason::Call, args)) {
    return false;
  }

  /*
   * Native constructors must return non-primitive values on success.
   * Although it is legal, if a constructor returns the callee, there is a
   * 99.9999% chance it is a bug. If any valid code actually wants the
   * constructor to return the callee, the assertion can be removed or
   * (another) conjunct can be added to the antecedent.
   *
   * Exceptions:
   * - (new Object(Object)) returns the callee.
   * - The bound function construct hook can return an arbitrary object,
   *   including the callee.
   *
   * Also allow if this may be due to a debugger hook since fuzzing may let this
   * happen.
   */
  MOZ_ASSERT(args.rval().isObject());
  MOZ_ASSERT_IF(!JS_IsNativeFunction(callee, obj_construct) &&
                    !callee->is<BoundFunctionObject>() &&
                    !cx->insideDebuggerEvaluationWithOnNativeCallHook,
                args.rval() != ObjectValue(*callee));

  return true;
}

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
bool js::InternalCallOrConstruct(JSContext* cx, const CallArgs& args,
                                 MaybeConstruct construct,
                                 CallReason reason /* = CallReason::Call */) {
  MOZ_ASSERT(args.length() <= ARGS_LENGTH_MAX);

  unsigned skipForCallee = args.length() + 1 + (construct == CONSTRUCT);
  if (args.calleev().isPrimitive()) {
    return ReportIsNotFunction(cx, args.calleev(), skipForCallee, construct);
  }

  /* Invoke non-functions. */
  if (MOZ_UNLIKELY(!args.callee().is<JSFunction>())) {
    MOZ_ASSERT_IF(construct, !args.callee().isConstructor());

    if (!args.callee().isCallable()) {
      return ReportIsNotFunction(cx, args.calleev(), skipForCallee, construct);
    }

    if (args.callee().is<ProxyObject>()) {
      RootedObject proxy(cx, &args.callee());
      return Proxy::call(cx, proxy, args);
    }

    JSNative call = args.callee().callHook();
    MOZ_ASSERT(call, "isCallable without a callHook?");

    return CallJSNative(cx, call, reason, args);
  }

  /* Invoke native functions. */
  RootedFunction fun(cx, &args.callee().as<JSFunction>());
  if (fun->isNativeFun()) {
    MOZ_ASSERT_IF(construct, !fun->isConstructor());
    JSNative native = fun->native();
    if (!construct && args.ignoresReturnValue() && fun->hasJitInfo()) {
      const JSJitInfo* jitInfo = fun->jitInfo();
      if (jitInfo->type() == JSJitInfo::IgnoresReturnValueNative) {
        native = jitInfo->ignoresReturnValueMethod;
      }
    }
    return CallJSNative(cx, native, reason, args);
  }

  // Self-hosted builtins are considered native by the onNativeCall hook.
  if (fun->isSelfHostedBuiltin()) {
    NativeResumeMode resumeMode = DebugAPI::onNativeCall(cx, args, reason);
    if (resumeMode != NativeResumeMode::Continue) {
      return resumeMode == NativeResumeMode::Override;
    }
  }

  if (!JSFunction::getOrCreateScript(cx, fun)) {
    return false;
  }

  /* Run function until JSOp::RetRval, JSOp::Return or error. */
  InvokeState state(cx, args, construct);

  // Create |this| if we're constructing. Switch to the callee's realm to
  // ensure this object has the correct realm.
  AutoRealm ar(cx, state.script());
  if (construct && !MaybeCreateThisForConstructor(cx, args)) {
    return false;
  }

  // Calling class constructors throws an error from the callee's realm.
  if (construct != CONSTRUCT && fun->isClassConstructor()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_CALL_CLASS_CONSTRUCTOR);
    return false;
  }

  bool ok = RunScript(cx, state);

  MOZ_ASSERT_IF(ok && construct, args.rval().isObject());
  return ok;
}

// Returns true if the callee needs an outerized |this| object. Outerization
// means passing the WindowProxy instead of the Window (a GlobalObject) because
// we must never expose the Window to script. This returns false only for DOM
// getters or setters.
static bool CalleeNeedsOuterizedThisObject(const Value& callee) {
  if (!callee.isObject() || !callee.toObject().is<JSFunction>()) {
    return true;
  }
  JSFunction& fun = callee.toObject().as<JSFunction>();
  if (!fun.isNativeFun() || !fun.hasJitInfo()) {
    return true;
  }
  return fun.jitInfo()->needsOuterizedThisObject();
}

static bool InternalCall(JSContext* cx, const AnyInvokeArgs& args,
                         CallReason reason) {
  MOZ_ASSERT(args.array() + args.length() == args.end(),
             "must pass calling arguments to a calling attempt");

#ifdef DEBUG
  // The caller is responsible for calling GetThisObject if needed.
  if (args.thisv().isObject()) {
    JSObject* thisObj = &args.thisv().toObject();
    MOZ_ASSERT_IF(CalleeNeedsOuterizedThisObject(args.calleev()),
                  GetThisObject(thisObj) == thisObj);
  }
#endif

  return InternalCallOrConstruct(cx, args, NO_CONSTRUCT, reason);
}

bool js::CallFromStack(JSContext* cx, const CallArgs& args,
                       CallReason reason /* = CallReason::Call */) {
  return InternalCall(cx, static_cast<const AnyInvokeArgs&>(args), reason);
}

// ES7 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 7.3.12 Call.
bool js::Call(JSContext* cx, HandleValue fval, HandleValue thisv,
              const AnyInvokeArgs& args, MutableHandleValue rval,
              CallReason reason) {
  // Explicitly qualify these methods to bypass AnyInvokeArgs's deliberate
  // shadowing.
  args.CallArgs::setCallee(fval);
  args.CallArgs::setThis(thisv);

  if (thisv.isObject()) {
    // If |this| is a global object, it might be a Window and in that case we
    // usually have to pass the WindowProxy instead.
    JSObject* thisObj = &thisv.toObject();
    if (thisObj->is<GlobalObject>()) {
      if (CalleeNeedsOuterizedThisObject(fval)) {
        args.mutableThisv().setObject(*GetThisObject(thisObj));
      }
    } else {
      // Fast path: we don't have to do anything if the object isn't a global.
      MOZ_ASSERT(GetThisObject(thisObj) == thisObj);
    }
  }

  if (!InternalCall(cx, args, reason)) {
    return false;
  }

  rval.set(args.rval());
  return true;
}

static bool InternalConstruct(JSContext* cx, const AnyConstructArgs& args,
                              CallReason reason = CallReason::Call) {
  MOZ_ASSERT(args.array() + args.length() + 1 == args.end(),
             "must pass constructing arguments to a construction attempt");
  MOZ_ASSERT(!FunctionClass.getConstruct());
  MOZ_ASSERT(!ExtendedFunctionClass.getConstruct());

  // Callers are responsible for enforcing these preconditions.
  MOZ_ASSERT(IsConstructor(args.calleev()),
             "trying to construct a value that isn't a constructor");
  MOZ_ASSERT(IsConstructor(args.CallArgs::newTarget()),
             "provided new.target value must be a constructor");

  MOZ_ASSERT(args.thisv().isMagic(JS_IS_CONSTRUCTING) ||
             args.thisv().isObject());

  JSObject& callee = args.callee();
  if (callee.is<JSFunction>()) {
    RootedFunction fun(cx, &callee.as<JSFunction>());

    if (fun->isNativeFun()) {
      return CallJSNativeConstructor(cx, fun->native(), args);
    }

    if (!InternalCallOrConstruct(cx, args, CONSTRUCT, reason)) {
      return false;
    }

    MOZ_ASSERT(args.CallArgs::rval().isObject());
    return true;
  }

  if (callee.is<ProxyObject>()) {
    RootedObject proxy(cx, &callee);
    return Proxy::construct(cx, proxy, args);
  }

  JSNative construct = callee.constructHook();
  MOZ_ASSERT(construct != nullptr, "IsConstructor without a construct hook?");

  return CallJSNativeConstructor(cx, construct, args);
}

// Check that |callee|, the callee in a |new| expression, is a constructor.
static bool StackCheckIsConstructorCalleeNewTarget(JSContext* cx,
                                                   HandleValue callee,
                                                   HandleValue newTarget) {
  // Calls from the stack could have any old non-constructor callee.
  if (!IsConstructor(callee)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_SEARCH_STACK, callee,
                     nullptr);
    return false;
  }

  // The new.target has already been vetted by previous calls, or is the callee.
  // We can just assert that it's a constructor.
  MOZ_ASSERT(IsConstructor(newTarget));

  return true;
}

bool js::ConstructFromStack(JSContext* cx, const CallArgs& args,
                            CallReason reason /* CallReason::Call */) {
  if (!StackCheckIsConstructorCalleeNewTarget(cx, args.calleev(),
                                              args.newTarget())) {
    return false;
  }

  return InternalConstruct(cx, static_cast<const AnyConstructArgs&>(args),
                           reason);
}

bool js::Construct(JSContext* cx, HandleValue fval,
                   const AnyConstructArgs& args, HandleValue newTarget,
                   MutableHandleObject objp) {
  MOZ_ASSERT(args.thisv().isMagic(JS_IS_CONSTRUCTING));

  // Explicitly qualify to bypass AnyConstructArgs's deliberate shadowing.
  args.CallArgs::setCallee(fval);
  args.CallArgs::newTarget().set(newTarget);

  if (!InternalConstruct(cx, args)) {
    return false;
  }

  MOZ_ASSERT(args.CallArgs::rval().isObject());
  objp.set(&args.CallArgs::rval().toObject());
  return true;
}

bool js::InternalConstructWithProvidedThis(JSContext* cx, HandleValue fval,
                                           HandleValue thisv,
                                           const AnyConstructArgs& args,
                                           HandleValue newTarget,
                                           MutableHandleValue rval) {
  args.CallArgs::setCallee(fval);

  MOZ_ASSERT(thisv.isObject());
  args.CallArgs::setThis(thisv);

  args.CallArgs::newTarget().set(newTarget);

  if (!InternalConstruct(cx, args)) {
    return false;
  }

  rval.set(args.CallArgs::rval());
  return true;
}

bool js::CallGetter(JSContext* cx, HandleValue thisv, HandleValue getter,
                    MutableHandleValue rval) {
  FixedInvokeArgs<0> args(cx);

  return Call(cx, getter, thisv, args, rval, CallReason::Getter);
}

bool js::CallSetter(JSContext* cx, HandleValue thisv, HandleValue setter,
                    HandleValue v) {
  FixedInvokeArgs<1> args(cx);
  args[0].set(v);

  RootedValue ignored(cx);
  return Call(cx, setter, thisv, args, &ignored, CallReason::Setter);
}

bool js::ExecuteKernel(JSContext* cx, HandleScript script,
                       HandleObject envChainArg, AbstractFramePtr evalInFrame,
                       MutableHandleValue result) {
  MOZ_ASSERT_IF(script->isGlobalCode(),
                IsGlobalLexicalEnvironment(envChainArg) ||
                    !IsSyntacticEnvironment(envChainArg));
#ifdef DEBUG
  RootedObject terminatingEnv(cx, envChainArg);
  while (IsSyntacticEnvironment(terminatingEnv)) {
    terminatingEnv = terminatingEnv->enclosingEnvironment();
  }
  MOZ_ASSERT(terminatingEnv->is<GlobalObject>() ||
             script->hasNonSyntacticScope());
#endif

  if (script->treatAsRunOnce()) {
    if (script->hasRunOnce()) {
      JS_ReportErrorASCII(cx,
                          "Trying to execute a run-once script multiple times");
      return false;
    }

    script->setHasRunOnce();
  }

  if (script->isEmpty()) {
    result.setUndefined();
    return true;
  }

  probes::StartExecution(script);
  ExecuteState state(cx, script, envChainArg, evalInFrame, result);
  bool ok = RunScript(cx, state);
  probes::StopExecution(script);

  return ok;
}

bool js::Execute(JSContext* cx, HandleScript script, HandleObject envChain,
                 MutableHandleValue rval) {
  /* The env chain is something we control, so we know it can't
     have any outer objects on it. */
  MOZ_ASSERT(!IsWindowProxy(envChain));

  if (script->isModule()) {
    MOZ_RELEASE_ASSERT(
        envChain == script->module()->environment(),
        "Module scripts can only be executed in the module's environment");
  } else {
    MOZ_RELEASE_ASSERT(
        IsGlobalLexicalEnvironment(envChain) || script->hasNonSyntacticScope(),
        "Only global scripts with non-syntactic envs can be executed with "
        "interesting envchains");
  }

  /* Ensure the env chain is all same-compartment and terminates in a global. */
#ifdef DEBUG
  JSObject* s = envChain;
  do {
    cx->check(s);
    MOZ_ASSERT_IF(!s->enclosingEnvironment(), s->is<GlobalObject>());
  } while ((s = s->enclosingEnvironment()));
#endif

  return ExecuteKernel(cx, script, envChain, NullFramePtr() /* evalInFrame */,
                       rval);
}

/*
 * ES6 (4-25-16) 12.10.4 InstanceofOperator
 */
bool js::InstanceofOperator(JSContext* cx, HandleObject obj, HandleValue v,
                            bool* bp) {
  /* Step 1. is handled by caller. */

  /* Step 2. */
  RootedValue hasInstance(cx);
  RootedId id(cx, PropertyKey::Symbol(cx->wellKnownSymbols().hasInstance));
  if (!GetProperty(cx, obj, obj, id, &hasInstance)) {
    return false;
  }

  if (!hasInstance.isNullOrUndefined()) {
    if (!IsCallable(hasInstance)) {
      return ReportIsNotFunction(cx, hasInstance);
    }

    /* Step 3. */
    RootedValue rval(cx);
    if (!Call(cx, hasInstance, obj, v, &rval)) {
      return false;
    }
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

JSType js::TypeOfObject(JSObject* obj) {
#ifdef ENABLE_RECORD_TUPLE
  MOZ_ASSERT(!js::IsExtendedPrimitive(*obj));
#endif

  AutoUnsafeCallWithABI unsafe;
  if (EmulatesUndefined(obj)) {
    return JSTYPE_UNDEFINED;
  }
  if (obj->isCallable()) {
    return JSTYPE_FUNCTION;
  }
  return JSTYPE_OBJECT;
}

#ifdef ENABLE_RECORD_TUPLE
JSType TypeOfExtendedPrimitive(JSObject* obj) {
  MOZ_ASSERT(js::IsExtendedPrimitive(*obj));

  if (obj->is<RecordType>()) {
    return JSTYPE_RECORD;
  }
  if (obj->is<TupleType>()) {
    return JSTYPE_TUPLE;
  }
  MOZ_CRASH("Unknown ExtendedPrimitive");
}
#endif

JSType js::TypeOfValue(const Value& v) {
  switch (v.type()) {
    case ValueType::Double:
    case ValueType::Int32:
      return JSTYPE_NUMBER;
    case ValueType::String:
      return JSTYPE_STRING;
    case ValueType::Null:
      return JSTYPE_OBJECT;
    case ValueType::Undefined:
      return JSTYPE_UNDEFINED;
    case ValueType::Object:
      return TypeOfObject(&v.toObject());
#ifdef ENABLE_RECORD_TUPLE
    case ValueType::ExtendedPrimitive:
      return TypeOfExtendedPrimitive(&v.toExtendedPrimitive());
#endif
    case ValueType::Boolean:
      return JSTYPE_BOOLEAN;
    case ValueType::BigInt:
      return JSTYPE_BIGINT;
    case ValueType::Symbol:
      return JSTYPE_SYMBOL;
    case ValueType::Magic:
    case ValueType::PrivateGCThing:
      break;
  }

  ReportBadValueTypeAndCrash(v);
}

bool js::CheckClassHeritageOperation(JSContext* cx, HandleValue heritage) {
  if (IsConstructor(heritage)) {
    return true;
  }

  if (heritage.isNull()) {
    return true;
  }

  if (heritage.isObject()) {
    ReportIsNotFunction(cx, heritage, 0, CONSTRUCT);
    return false;
  }

  ReportValueError(cx, JSMSG_BAD_HERITAGE, -1, heritage, nullptr,
                   "not an object or null");
  return false;
}

PlainObject* js::ObjectWithProtoOperation(JSContext* cx, HandleValue val) {
  if (!val.isObjectOrNull()) {
    ReportValueError(cx, JSMSG_NOT_OBJORNULL, -1, val, nullptr);
    return nullptr;
  }

  RootedObject proto(cx, val.toObjectOrNull());
  return NewPlainObjectWithProto(cx, proto);
}

JSObject* js::FunWithProtoOperation(JSContext* cx, HandleFunction fun,
                                    HandleObject parent, HandleObject proto) {
  return CloneFunctionReuseScript(cx, fun, parent, proto);
}

/*
 * Enter the new with environment using an object at sp[-1] and associate the
 * depth of the with block with sp + stackIndex.
 */
bool js::EnterWithOperation(JSContext* cx, AbstractFramePtr frame,
                            HandleValue val, Handle<WithScope*> scope) {
  RootedObject obj(cx);
  if (val.isObject()) {
    obj = &val.toObject();
  } else {
    obj = ToObject(cx, val);
    if (!obj) {
      return false;
    }
  }

  RootedObject envChain(cx, frame.environmentChain());
  WithEnvironmentObject* withobj =
      WithEnvironmentObject::create(cx, obj, envChain, scope);
  if (!withobj) {
    return false;
  }

  frame.pushOnEnvironmentChain(*withobj);
  return true;
}

static void PopEnvironment(JSContext* cx, EnvironmentIter& ei) {
  switch (ei.scope().kind()) {
    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
    case ScopeKind::FunctionLexical:
    case ScopeKind::ClassBody:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, ei);
      }
      if (ei.scope().hasEnvironment()) {
        ei.initialFrame()
            .popOffEnvironmentChain<ScopedLexicalEnvironmentObject>();
      }
      break;
    case ScopeKind::With:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopWith(ei.initialFrame());
      }
      ei.initialFrame().popOffEnvironmentChain<WithEnvironmentObject>();
      break;
    case ScopeKind::Function:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopCall(cx, ei.initialFrame());
      }
      if (ei.scope().hasEnvironment()) {
        ei.initialFrame().popOffEnvironmentChain<CallObject>();
      }
      break;
    case ScopeKind::FunctionBodyVar:
    case ScopeKind::StrictEval:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopVar(cx, ei);
      }
      if (ei.scope().hasEnvironment()) {
        ei.initialFrame().popOffEnvironmentChain<VarEnvironmentObject>();
      }
      break;
    case ScopeKind::Module:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopModule(cx, ei);
      }
      break;
    case ScopeKind::Eval:
    case ScopeKind::Global:
    case ScopeKind::NonSyntactic:
      break;
    case ScopeKind::WasmInstance:
    case ScopeKind::WasmFunction:
      MOZ_CRASH("wasm is not interpreted");
      break;
  }
}

// Unwind environment chain and iterator to match the env corresponding to
// the given bytecode position.
void js::UnwindEnvironment(JSContext* cx, EnvironmentIter& ei, jsbytecode* pc) {
  if (!ei.withinInitialFrame()) {
    return;
  }

  Rooted<Scope*> scope(cx, ei.initialFrame().script()->innermostScope(pc));

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
  for (uint32_t i = 0; i < script->bodyScopeIndex(); i++) {
    MOZ_ASSERT(scope != script->getScope(GCThingIndex(i)));
  }
#endif

  for (; ei.maybeScope() != scope; ei++) {
    PopEnvironment(cx, ei);
  }
}

// Unwind all environments. This is needed because block scopes may cover the
// first bytecode at a script's main(). e.g.,
//
//     function f() { { let i = 0; } }
//
// will have no pc location distinguishing the first block scope from the
// outermost function scope.
void js::UnwindAllEnvironmentsInFrame(JSContext* cx, EnvironmentIter& ei) {
  for (; ei.withinInitialFrame(); ei++) {
    PopEnvironment(cx, ei);
  }
}

// Compute the pc needed to unwind the environment to the beginning of a try
// block. We cannot unwind to *after* the JSOp::Try, because that might be the
// first opcode of an inner scope, with the same problem as above. e.g.,
//
// try { { let x; } }
//
// will have no pc location distinguishing the try block scope from the inner
// let block scope.
jsbytecode* js::UnwindEnvironmentToTryPc(JSScript* script, const TryNote* tn) {
  jsbytecode* pc = script->offsetToPC(tn->start);
  if (tn->kind() == TryNoteKind::Catch || tn->kind() == TryNoteKind::Finally) {
    pc -= JSOpLength_Try;
    MOZ_ASSERT(JSOp(*pc) == JSOp::Try);
  } else if (tn->kind() == TryNoteKind::Destructuring) {
    pc -= JSOpLength_TryDestructuring;
    MOZ_ASSERT(JSOp(*pc) == JSOp::TryDestructuring);
  }
  return pc;
}

static void SettleOnTryNote(JSContext* cx, const TryNote* tn,
                            EnvironmentIter& ei, InterpreterRegs& regs) {
  // Unwind the environment to the beginning of the JSOp::Try.
  UnwindEnvironment(cx, ei, UnwindEnvironmentToTryPc(regs.fp()->script(), tn));

  // Set pc to the first bytecode after the the try note to point
  // to the beginning of catch or finally.
  regs.pc = regs.fp()->script()->offsetToPC(tn->start + tn->length);
  regs.sp = regs.spForStackDepth(tn->stackDepth);
}

class InterpreterTryNoteFilter {
  const InterpreterRegs& regs_;

 public:
  explicit InterpreterTryNoteFilter(const InterpreterRegs& regs)
      : regs_(regs) {}
  bool operator()(const TryNote* note) {
    return note->stackDepth <= regs_.stackDepth();
  }
};

class TryNoteIterInterpreter : public TryNoteIter<InterpreterTryNoteFilter> {
 public:
  TryNoteIterInterpreter(JSContext* cx, const InterpreterRegs& regs)
      : TryNoteIter(cx, regs.fp()->script(), regs.pc,
                    InterpreterTryNoteFilter(regs)) {}
};

static void UnwindIteratorsForUncatchableException(
    JSContext* cx, const InterpreterRegs& regs) {
  // c.f. the regular (catchable) TryNoteIterInterpreter loop in
  // ProcessTryNotes.
  for (TryNoteIterInterpreter tni(cx, regs); !tni.done(); ++tni) {
    const TryNote* tn = *tni;
    switch (tn->kind()) {
      case TryNoteKind::ForIn: {
        Value* sp = regs.spForStackDepth(tn->stackDepth);
        UnwindIteratorForUncatchableException(&sp[-1].toObject());
        break;
      }
      default:
        break;
    }
  }
}

enum HandleErrorContinuation {
  SuccessfulReturnContinuation,
  ErrorReturnContinuation,
  CatchContinuation,
  FinallyContinuation
};

static HandleErrorContinuation ProcessTryNotes(JSContext* cx,
                                               EnvironmentIter& ei,
                                               InterpreterRegs& regs) {
  for (TryNoteIterInterpreter tni(cx, regs); !tni.done(); ++tni) {
    const TryNote* tn = *tni;

    switch (tn->kind()) {
      case TryNoteKind::Catch:
        /* Catch cannot intercept the closing of a generator. */
        if (cx->isClosingGenerator()) {
          break;
        }

        SettleOnTryNote(cx, tn, ei, regs);
        return CatchContinuation;

      case TryNoteKind::Finally:
        SettleOnTryNote(cx, tn, ei, regs);
        return FinallyContinuation;

      case TryNoteKind::ForIn: {
        /* This is similar to JSOp::EndIter in the interpreter loop. */
        MOZ_ASSERT(tn->stackDepth <= regs.stackDepth());
        Value* sp = regs.spForStackDepth(tn->stackDepth);
        JSObject* obj = &sp[-1].toObject();
        CloseIterator(obj);
        break;
      }

      case TryNoteKind::Destructuring: {
        // Whether the destructuring iterator is done is at the top of the
        // stack. The iterator object is second from the top.
        MOZ_ASSERT(tn->stackDepth > 1);
        Value* sp = regs.spForStackDepth(tn->stackDepth);
        RootedValue doneValue(cx, sp[-1]);
        MOZ_RELEASE_ASSERT(!doneValue.isMagic());
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

      case TryNoteKind::ForOf:
      case TryNoteKind::Loop:
        break;

      // TryNoteKind::ForOfIterClose is handled internally by the try note
      // iterator.
      default:
        MOZ_CRASH("Invalid try note");
    }
  }

  return SuccessfulReturnContinuation;
}

bool js::HandleClosingGeneratorReturn(JSContext* cx, AbstractFramePtr frame,
                                      bool ok) {
  /*
   * Propagate the exception or error to the caller unless the exception
   * is an asynchronous return from a generator.
   */
  if (cx->isClosingGenerator()) {
    cx->clearPendingException();
    ok = true;
    auto* genObj = GetGeneratorObjectForFrame(cx, frame);
    genObj->setClosed();
  }
  return ok;
}

static HandleErrorContinuation HandleError(JSContext* cx,
                                           InterpreterRegs& regs) {
  MOZ_ASSERT(regs.fp()->script()->containsPC(regs.pc));
  MOZ_ASSERT(cx->realm() == regs.fp()->script()->realm());

  if (regs.fp()->script()->hasScriptCounts()) {
    PCCounts* counts = regs.fp()->script()->getThrowCounts(regs.pc);
    // If we failed to allocate, then skip the increment and continue to
    // handle the exception.
    if (counts) {
      counts->numExec()++;
    }
  }

  EnvironmentIter ei(cx, regs.fp(), regs.pc);
  bool ok = false;

again:
  if (cx->isExceptionPending()) {
    /* Call debugger throw hooks. */
    if (!cx->isClosingGenerator()) {
      if (!DebugAPI::onExceptionUnwind(cx, regs.fp())) {
        if (!cx->isExceptionPending()) {
          goto again;
        }
      }
      // Ensure that the debugger hasn't returned 'true' while clearing the
      // exception state.
      MOZ_ASSERT(cx->isExceptionPending());
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
  } else {
    UnwindIteratorsForUncatchableException(cx, regs);

    // We may be propagating a forced return from a debugger hook function.
    if (MOZ_UNLIKELY(cx->isPropagatingForcedReturn())) {
      cx->clearPropagatingForcedReturn();
      ok = true;
    }
  }

  ok = DebugAPI::onLeaveFrame(cx, regs.fp(), regs.pc, ok);

  // After this point, we will pop the frame regardless. Settle the frame on
  // the end of the script.
  regs.setToEndOfScript();

  return ok ? SuccessfulReturnContinuation : ErrorReturnContinuation;
}

#define REGS (activation.regs())
#define PUSH_COPY(v)                 \
  do {                               \
    *REGS.sp++ = (v);                \
    cx->debugOnlyCheck(REGS.sp[-1]); \
  } while (0)
#define PUSH_COPY_SKIP_CHECK(v) *REGS.sp++ = (v)
#define PUSH_NULL() REGS.sp++->setNull()
#define PUSH_UNDEFINED() REGS.sp++->setUndefined()
#define PUSH_BOOLEAN(b) REGS.sp++->setBoolean(b)
#define PUSH_DOUBLE(d) REGS.sp++->setDouble(d)
#define PUSH_INT32(i) REGS.sp++->setInt32(i)
#define PUSH_SYMBOL(s) REGS.sp++->setSymbol(s)
#define PUSH_BIGINT(b) REGS.sp++->setBigInt(b)
#define PUSH_STRING(s)               \
  do {                               \
    REGS.sp++->setString(s);         \
    cx->debugOnlyCheck(REGS.sp[-1]); \
  } while (0)
#define PUSH_OBJECT(obj)             \
  do {                               \
    REGS.sp++->setObject(obj);       \
    cx->debugOnlyCheck(REGS.sp[-1]); \
  } while (0)
#define PUSH_OBJECT_OR_NULL(obj)     \
  do {                               \
    REGS.sp++->setObjectOrNull(obj); \
    cx->debugOnlyCheck(REGS.sp[-1]); \
  } while (0)
#ifdef ENABLE_RECORD_TUPLE
#  define PUSH_EXTENDED_PRIMITIVE(obj)      \
    do {                                    \
      REGS.sp++->setExtendedPrimitive(obj); \
      cx->debugOnlyCheck(REGS.sp[-1]);      \
    } while (0)
#endif
#define PUSH_MAGIC(magic) REGS.sp++->setMagic(magic)
#define POP_COPY_TO(v) (v) = *--REGS.sp
#define POP_RETURN_VALUE() REGS.fp()->setReturnValue(*--REGS.sp)

/*
 * Same for JSOp::SetName and JSOp::SetProp, which differ only slightly but
 * remain distinct for the decompiler.
 */
static_assert(JSOpLength_SetName == JSOpLength_SetProp);

/* See TRY_BRANCH_AFTER_COND. */
static_assert(JSOpLength_JumpIfTrue == JSOpLength_JumpIfFalse);
static_assert(uint8_t(JSOp::JumpIfTrue) == uint8_t(JSOp::JumpIfFalse) + 1);

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
static inline Value ComputeImplicitThis(JSObject* env) {
  // Fast-path for GlobalObject
  if (env->is<GlobalObject>()) {
    return UndefinedValue();
  }

  // WithEnvironmentObjects have an actual implicit |this|
  if (env->is<WithEnvironmentObject>()) {
    return ObjectValue(*GetThisObjectOfWith(env));
  }

  // Debugger environments need special casing, as despite being
  // non-syntactic, they wrap syntactic environments and should not be
  // treated like other embedding-specific non-syntactic environments.
  if (env->is<DebugEnvironmentProxy>()) {
    return ComputeImplicitThis(&env->as<DebugEnvironmentProxy>().environment());
  }

  MOZ_ASSERT(env->is<EnvironmentObject>());
  return UndefinedValue();
}

static MOZ_ALWAYS_INLINE bool AddOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (lhs.isInt32() && rhs.isInt32()) {
    int32_t l = lhs.toInt32(), r = rhs.toInt32();
    int32_t t;
    if (MOZ_LIKELY(SafeAdd(l, r, &t))) {
      res.setInt32(t);
      return true;
    }
  }

  if (!ToPrimitive(cx, lhs)) {
    return false;
  }
  if (!ToPrimitive(cx, rhs)) {
    return false;
  }

  bool lIsString = lhs.isString();
  bool rIsString = rhs.isString();
  if (lIsString || rIsString) {
    JSString* lstr;
    if (lIsString) {
      lstr = lhs.toString();
    } else {
      lstr = ToString<CanGC>(cx, lhs);
      if (!lstr) {
        return false;
      }
    }

    JSString* rstr;
    if (rIsString) {
      rstr = rhs.toString();
    } else {
      // Save/restore lstr in case of GC activity under ToString.
      lhs.setString(lstr);
      rstr = ToString<CanGC>(cx, rhs);
      if (!rstr) {
        return false;
      }
      lstr = lhs.toString();
    }
    JSString* str = ConcatStrings<NoGC>(cx, lstr, rstr);
    if (!str) {
      RootedString nlstr(cx, lstr), nrstr(cx, rstr);
      str = ConcatStrings<CanGC>(cx, nlstr, nrstr);
      if (!str) {
        return false;
      }
    }
    res.setString(str);
    return true;
  }

  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::addValue(cx, lhs, rhs, res);
  }

  res.setNumber(lhs.toNumber() + rhs.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool SubOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::subValue(cx, lhs, rhs, res);
  }

  res.setNumber(lhs.toNumber() - rhs.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool MulOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::mulValue(cx, lhs, rhs, res);
  }

  res.setNumber(lhs.toNumber() * rhs.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool DivOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::divValue(cx, lhs, rhs, res);
  }

  res.setNumber(NumberDiv(lhs.toNumber(), rhs.toNumber()));
  return true;
}

static MOZ_ALWAYS_INLINE bool ModOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  int32_t l, r;
  if (lhs.isInt32() && rhs.isInt32() && (l = lhs.toInt32()) >= 0 &&
      (r = rhs.toInt32()) > 0) {
    int32_t mod = l % r;
    res.setInt32(mod);
    return true;
  }

  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::modValue(cx, lhs, rhs, res);
  }

  res.setNumber(NumberMod(lhs.toNumber(), rhs.toNumber()));
  return true;
}

static MOZ_ALWAYS_INLINE bool PowOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::powValue(cx, lhs, rhs, res);
  }

  res.setNumber(ecmaPow(lhs.toNumber(), rhs.toNumber()));
  return true;
}

static MOZ_ALWAYS_INLINE bool BitNotOperation(JSContext* cx,
                                              MutableHandleValue in,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, in)) {
    return false;
  }

  if (in.isBigInt()) {
    return BigInt::bitNotValue(cx, in, out);
  }

  out.setInt32(~in.toInt32());
  return true;
}

static MOZ_ALWAYS_INLINE bool BitXorOperation(JSContext* cx,
                                              MutableHandleValue lhs,
                                              MutableHandleValue rhs,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::bitXorValue(cx, lhs, rhs, out);
  }

  out.setInt32(lhs.toInt32() ^ rhs.toInt32());
  return true;
}

static MOZ_ALWAYS_INLINE bool BitOrOperation(JSContext* cx,
                                             MutableHandleValue lhs,
                                             MutableHandleValue rhs,
                                             MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::bitOrValue(cx, lhs, rhs, out);
  }

  out.setInt32(lhs.toInt32() | rhs.toInt32());
  return true;
}

static MOZ_ALWAYS_INLINE bool BitAndOperation(JSContext* cx,
                                              MutableHandleValue lhs,
                                              MutableHandleValue rhs,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::bitAndValue(cx, lhs, rhs, out);
  }

  out.setInt32(lhs.toInt32() & rhs.toInt32());
  return true;
}

static MOZ_ALWAYS_INLINE bool BitLshOperation(JSContext* cx,
                                              MutableHandleValue lhs,
                                              MutableHandleValue rhs,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::lshValue(cx, lhs, rhs, out);
  }

  // Signed left-shift is undefined on overflow, so |lhs << (rhs & 31)| won't
  // work.  Instead, convert to unsigned space (where overflow is treated
  // modularly), perform the operation there, then convert back.
  uint32_t left = static_cast<uint32_t>(lhs.toInt32());
  uint8_t right = rhs.toInt32() & 31;
  out.setInt32(mozilla::WrapToSigned(left << right));
  return true;
}

static MOZ_ALWAYS_INLINE bool BitRshOperation(JSContext* cx,
                                              MutableHandleValue lhs,
                                              MutableHandleValue rhs,
                                              MutableHandleValue out) {
  if (!ToInt32OrBigInt(cx, lhs) || !ToInt32OrBigInt(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::rshValue(cx, lhs, rhs, out);
  }

  out.setInt32(lhs.toInt32() >> (rhs.toInt32() & 31));
  return true;
}

static MOZ_ALWAYS_INLINE bool UrshOperation(JSContext* cx,
                                            MutableHandleValue lhs,
                                            MutableHandleValue rhs,
                                            MutableHandleValue out) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_TO_NUMBER);
    return false;
  }

  uint32_t left;
  int32_t right;
  if (!ToUint32(cx, lhs, &left) || !ToInt32(cx, rhs, &right)) {
    return false;
  }
  left >>= right & 31;
  out.setNumber(uint32_t(left));
  return true;
}

// BigInt proposal 3.2.4 Abstract Relational Comparison
// Returns Nothing when at least one operand is a NaN, or when
// ToNumeric or StringToBigInt can't interpret a string as a numeric
// value. (These cases correspond to a NaN result in the spec.)
// Otherwise, return a boolean to indicate whether lhs is less than
// rhs. The operands must be primitives; the caller is responsible for
// evaluating them in the correct order.
static MOZ_ALWAYS_INLINE bool LessThanImpl(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           mozilla::Maybe<bool>& res) {
  // Steps 1 and 2 are performed by the caller.

  // Step 3.
  if (lhs.isString() && rhs.isString()) {
    JSString* l = lhs.toString();
    JSString* r = rhs.toString();
    int32_t result;
    if (!CompareStrings(cx, l, r, &result)) {
      return false;
    }
    res = mozilla::Some(result < 0);
    return true;
  }

  // Step 4a.
  if (lhs.isBigInt() && rhs.isString()) {
    return BigInt::lessThan(cx, lhs, rhs, res);
  }

  // Step 4b.
  if (lhs.isString() && rhs.isBigInt()) {
    return BigInt::lessThan(cx, lhs, rhs, res);
  }

  // Steps 4c and 4d.
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  // Steps 4e-j.
  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::lessThan(cx, lhs, rhs, res);
  }

  // Step 4e for Number operands.
  MOZ_ASSERT(lhs.isNumber() && rhs.isNumber());
  double lhsNum = lhs.toNumber();
  double rhsNum = rhs.toNumber();

  if (std::isnan(lhsNum) || std::isnan(rhsNum)) {
    res = mozilla::Maybe<bool>(mozilla::Nothing());
    return true;
  }

  res = mozilla::Some(lhsNum < rhsNum);
  return true;
}

static MOZ_ALWAYS_INLINE bool LessThanOperation(JSContext* cx,
                                                MutableHandleValue lhs,
                                                MutableHandleValue rhs,
                                                bool* res) {
  if (lhs.isInt32() && rhs.isInt32()) {
    *res = lhs.toInt32() < rhs.toInt32();
    return true;
  }

  if (!ToPrimitive(cx, JSTYPE_NUMBER, lhs)) {
    return false;
  }

  if (!ToPrimitive(cx, JSTYPE_NUMBER, rhs)) {
    return false;
  }

  mozilla::Maybe<bool> tmpResult;
  if (!LessThanImpl(cx, lhs, rhs, tmpResult)) {
    return false;
  }
  *res = tmpResult.valueOr(false);
  return true;
}

static MOZ_ALWAYS_INLINE bool LessThanOrEqualOperation(JSContext* cx,
                                                       MutableHandleValue lhs,
                                                       MutableHandleValue rhs,
                                                       bool* res) {
  if (lhs.isInt32() && rhs.isInt32()) {
    *res = lhs.toInt32() <= rhs.toInt32();
    return true;
  }

  if (!ToPrimitive(cx, JSTYPE_NUMBER, lhs)) {
    return false;
  }

  if (!ToPrimitive(cx, JSTYPE_NUMBER, rhs)) {
    return false;
  }

  mozilla::Maybe<bool> tmpResult;
  if (!LessThanImpl(cx, rhs, lhs, tmpResult)) {
    return false;
  }
  *res = !tmpResult.valueOr(true);
  return true;
}

static MOZ_ALWAYS_INLINE bool GreaterThanOperation(JSContext* cx,
                                                   MutableHandleValue lhs,
                                                   MutableHandleValue rhs,
                                                   bool* res) {
  if (lhs.isInt32() && rhs.isInt32()) {
    *res = lhs.toInt32() > rhs.toInt32();
    return true;
  }

  if (!ToPrimitive(cx, JSTYPE_NUMBER, lhs)) {
    return false;
  }

  if (!ToPrimitive(cx, JSTYPE_NUMBER, rhs)) {
    return false;
  }

  mozilla::Maybe<bool> tmpResult;
  if (!LessThanImpl(cx, rhs, lhs, tmpResult)) {
    return false;
  }
  *res = tmpResult.valueOr(false);
  return true;
}

static MOZ_ALWAYS_INLINE bool GreaterThanOrEqualOperation(
    JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res) {
  if (lhs.isInt32() && rhs.isInt32()) {
    *res = lhs.toInt32() >= rhs.toInt32();
    return true;
  }

  if (!ToPrimitive(cx, JSTYPE_NUMBER, lhs)) {
    return false;
  }

  if (!ToPrimitive(cx, JSTYPE_NUMBER, rhs)) {
    return false;
  }

  mozilla::Maybe<bool> tmpResult;
  if (!LessThanImpl(cx, lhs, rhs, tmpResult)) {
    return false;
  }
  *res = !tmpResult.valueOr(true);
  return true;
}

static MOZ_ALWAYS_INLINE bool SetObjectElementOperation(
    JSContext* cx, HandleObject obj, HandleId id, HandleValue value,
    HandleValue receiver, bool strict) {
  ObjectOpResult result;
  return SetProperty(cx, obj, id, value, receiver, result) &&
         result.checkStrictModeError(cx, obj, id, strict);
}

static MOZ_ALWAYS_INLINE void InitElemArrayOperation(JSContext* cx,
                                                     jsbytecode* pc,
                                                     Handle<ArrayObject*> arr,
                                                     HandleValue val) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::InitElemArray);

  // The dense elements must have been initialized up to this index. The JIT
  // implementation also depends on this.
  uint32_t index = GET_UINT32(pc);
  MOZ_ASSERT(index < arr->getDenseCapacity());
  MOZ_ASSERT(index == arr->getDenseInitializedLength());

  // Bump the initialized length even for hole values to ensure the
  // index == initLength invariant holds for later InitElemArray ops.
  arr->setDenseInitializedLength(index + 1);

  if (val.isMagic(JS_ELEMENTS_HOLE)) {
    arr->initDenseElementHole(index);
  } else {
    arr->initDenseElement(index, val);
  }
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

template <typename T>
class ReservedRooted : public RootedOperations<T, ReservedRooted<T>> {
  Rooted<T>* savedRoot;

 public:
  ReservedRooted(Rooted<T>* root, const T& ptr) : savedRoot(root) {
    *root = ptr;
  }

  explicit ReservedRooted(Rooted<T>* root) : savedRoot(root) {
    *root = JS::SafelyInitialized<T>::create();
  }

  ~ReservedRooted() { *savedRoot = JS::SafelyInitialized<T>::create(); }

  void set(const T& p) const { *savedRoot = p; }
  operator Handle<T>() { return *savedRoot; }
  operator Rooted<T>&() { return *savedRoot; }
  MutableHandle<T> operator&() { return &*savedRoot; }

  DECLARE_NONPOINTER_ACCESSOR_METHODS(savedRoot->get())
  DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(savedRoot->get())
  DECLARE_POINTER_CONSTREF_OPS(T)
  DECLARE_POINTER_ASSIGN_OPS(ReservedRooted, T)
};

void js::ReportInNotObjectError(JSContext* cx, HandleValue lref,
                                HandleValue rref) {
  auto uniqueCharsFromString = [](JSContext* cx,
                                  HandleValue ref) -> UniqueChars {
    static const size_t MaxStringLength = 16;
    RootedString str(cx, ref.toString());
    if (str->length() > MaxStringLength) {
      JSStringBuilder buf(cx);
      if (!buf.appendSubstring(str, 0, MaxStringLength)) {
        return nullptr;
      }
      if (!buf.append("...")) {
        return nullptr;
      }
      str = buf.finishString();
      if (!str) {
        return nullptr;
      }
    }
    return QuoteString(cx, str, '"');
  };

  if (lref.isString() && rref.isString()) {
    UniqueChars lbytes = uniqueCharsFromString(cx, lref);
    if (!lbytes) {
      return;
    }
    UniqueChars rbytes = uniqueCharsFromString(cx, rref);
    if (!rbytes) {
      return;
    }
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_IN_STRING,
                             lbytes.get(), rbytes.get());
    return;
  }

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_IN_NOT_OBJECT,
                            InformalValueTypeName(rref));
}

bool MOZ_NEVER_INLINE JS_HAZ_JSNATIVE_CALLER js::Interpret(JSContext* cx,
                                                           RunState& state) {
#ifndef NO_COMPUTED_GOTO
/*
 * Define macros for an interpreter loop. Opcode dispatch is done by
 * indirect goto (aka a threaded interpreter), which is technically
 * non-standard but is supported by many compilers.
 */
#define INTERPRETER_LOOP()
#define CASE_NOT_COMPUTED(OP) label_##OP:
#define CASE(OP) label_##OP:
#define DEFAULT() \
  label_default:
#define DISPATCH_TO(OP) goto* addresses[(OP)]

#define LABEL(X) (&&label_##X)

  // Use addresses instead of offsets to optimize for runtime speed over
  // load-time relocation overhead.
  static const void* const addresses[EnableInterruptsPseudoOpcode + 1] = {
#define OPCODE_LABEL(op, ...) LABEL(op),
      FOR_EACH_OPCODE(OPCODE_LABEL)
#undef OPCODE_LABEL
#define TRAILING_LABEL(v)                                                    \
  ((v) == EnableInterruptsPseudoOpcode ? LABEL(EnableInterruptsPseudoOpcode) \
                                       : LABEL(default)),
          FOR_EACH_TRAILING_UNUSED_OPCODE(TRAILING_LABEL)
#undef TRAILING_LABEL
  };
#else  // End of case where computed goto is available.
/*
 * Define macros for a switch-based interpreter loop. Using a switch statement
 * for dispatch is usually not as fast as the "threaded" approach that uses
 * indirect goto statements, but it does not require any language extensions
 * and should work on any standard-compliant C++ compiler.
 */
#define INTERPRETER_LOOP()   \
  the_switch:                \
    switch (switchOp)
#define CASE_NOT_COMPUTED(OP) case OP:
#define CASE(OP) case static_cast<jsbytecode>(JSOp::OP):
#define DEFAULT() default:
#define DISPATCH_TO(OP)   \
    JS_BEGIN_MACRO        \
      switchOp = (OP);    \
      goto the_switch;    \
    JS_END_MACRO

  // This variable is effectively a parameter to the switch.
  jsbytecode switchOp;
#endif  // End of case where computed goto is _not_ available.

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
#define ADVANCE_AND_DISPATCH(N)                  \
  JS_BEGIN_MACRO                                 \
    REGS.pc += (N);                              \
    SANITY_CHECKS();                             \
    DISPATCH_TO(*REGS.pc | activation.opMask()); \
  JS_END_MACRO

  /*
   * Shorthand for the common sequence at the end of a fixed-size opcode.
   */
#define END_CASE(OP) ADVANCE_AND_DISPATCH(JSOpLength_##OP);

  /*
   * Prepare to call a user-supplied branch handler, and abort the script
   * if it returns false.
   */
#define CHECK_BRANCH()                      \
  JS_BEGIN_MACRO                            \
    if (!CheckForInterrupt(cx)) goto error; \
  JS_END_MACRO

  /*
   * This is a simple wrapper around ADVANCE_AND_DISPATCH which also does
   * a CHECK_BRANCH() if n is not positive, which possibly indicates that it
   * is the backedge of a loop.
   */
#define BRANCH(n)                  \
  JS_BEGIN_MACRO                   \
    int32_t nlen = (n);            \
    if (nlen <= 0) CHECK_BRANCH(); \
    ADVANCE_AND_DISPATCH(nlen);    \
  JS_END_MACRO

  /*
   * Initialize code coverage vectors.
   */
#define INIT_COVERAGE()                                \
  JS_BEGIN_MACRO                                       \
    if (!script->hasScriptCounts()) {                  \
      if (cx->realm()->collectCoverageForDebug()) {    \
        if (!script->initScriptCounts(cx)) goto error; \
      }                                                \
    }                                                  \
  JS_END_MACRO

  /*
   * Increment the code coverage counter associated with the given pc.
   */
#define COUNT_COVERAGE_PC(PC)                          \
  JS_BEGIN_MACRO                                       \
    if (script->hasScriptCounts()) {                   \
      PCCounts* counts = script->maybeGetPCCounts(PC); \
      MOZ_ASSERT(counts);                              \
      counts->numExec()++;                             \
    }                                                  \
  JS_END_MACRO

#define COUNT_COVERAGE_MAIN()                                        \
  JS_BEGIN_MACRO                                                     \
    jsbytecode* main = script->main();                               \
    if (!BytecodeIsJumpTarget(JSOp(*main))) COUNT_COVERAGE_PC(main); \
  JS_END_MACRO

#define COUNT_COVERAGE()                              \
  JS_BEGIN_MACRO                                      \
    MOZ_ASSERT(BytecodeIsJumpTarget(JSOp(*REGS.pc))); \
    COUNT_COVERAGE_PC(REGS.pc);                       \
  JS_END_MACRO

#define SET_SCRIPT(s)                                    \
  JS_BEGIN_MACRO                                         \
    script = (s);                                        \
    MOZ_ASSERT(cx->realm() == script->realm());          \
    if (DebugAPI::hasAnyBreakpointsOrStepMode(script) || \
        script->hasScriptCounts())                       \
      activation.enableInterruptsUnconditionally();      \
  JS_END_MACRO

#define SANITY_CHECKS()              \
  JS_BEGIN_MACRO                     \
    js::gc::MaybeVerifyBarriers(cx); \
  JS_END_MACRO

// Verify that an uninitialized lexical is followed by a correct check op.
#ifdef DEBUG
#  define ASSERT_UNINITIALIZED_ALIASED_LEXICAL(val)                        \
    JS_BEGIN_MACRO                                                         \
      if (IsUninitializedLexical(val)) {                                   \
        JSOp next = JSOp(*GetNextPc(REGS.pc));                             \
        MOZ_ASSERT(next == JSOp::CheckThis || next == JSOp::CheckReturn || \
                   next == JSOp::CheckThisReinit ||                        \
                   next == JSOp::CheckAliasedLexical);                     \
      }                                                                    \
    JS_END_MACRO
#else
#  define ASSERT_UNINITIALIZED_ALIASED_LEXICAL(val) \
    JS_BEGIN_MACRO                                  \
    /* nothing */                                   \
    JS_END_MACRO
#endif

  gc::MaybeVerifyBarriers(cx, true);

  InterpreterFrame* entryFrame = state.pushInterpreterFrame(cx);
  if (!entryFrame) {
    return false;
  }

  ActivationEntryMonitor entryMonitor(cx, entryFrame);
  InterpreterActivation activation(state, cx, entryFrame);

  /* The script is used frequently, so keep a local copy. */
  RootedScript script(cx);
  SET_SCRIPT(REGS.fp()->script());

  /*
   * Pool of rooters for use in this interpreter frame. References to these
   * are used for local variables within interpreter cases. This avoids
   * creating new rooters each time an interpreter case is entered, and also
   * correctness pitfalls due to incorrect compilation of destructor calls
   * around computed gotos.
   */
  RootedValue rootValue0(cx), rootValue1(cx);
  RootedObject rootObject0(cx), rootObject1(cx);
  RootedFunction rootFunction0(cx);
  Rooted<JSAtom*> rootAtom0(cx);
  Rooted<PropertyName*> rootName0(cx);
  RootedId rootId0(cx);
  RootedScript rootScript0(cx);
  Rooted<Scope*> rootScope0(cx);
  DebugOnly<uint32_t> blockDepth;

  /* State communicated between non-local jumps: */
  bool interpReturnOK;
  bool frameHalfInitialized;

  if (!activation.entryFrame()->prologue(cx)) {
    goto prologue_error;
  }

  if (!DebugAPI::onEnterFrame(cx, activation.entryFrame())) {
    goto error;
  }

  // Increment the coverage for the main entry point.
  INIT_COVERAGE();
  COUNT_COVERAGE_MAIN();

  // Enter the interpreter loop starting at the current pc.
  ADVANCE_AND_DISPATCH(0);

  INTERPRETER_LOOP() {
    CASE_NOT_COMPUTED(EnableInterruptsPseudoOpcode) {
      bool moreInterrupts = false;
      jsbytecode op = *REGS.pc;

      if (!script->hasScriptCounts() &&
          cx->realm()->collectCoverageForDebug()) {
        if (!script->initScriptCounts(cx)) {
          goto error;
        }
      }

      if (script->isDebuggee()) {
        if (DebugAPI::stepModeEnabled(script)) {
          if (!DebugAPI::onSingleStep(cx)) {
            goto error;
          }
          moreInterrupts = true;
        }

        if (DebugAPI::hasAnyBreakpointsOrStepMode(script)) {
          moreInterrupts = true;
        }

        if (DebugAPI::hasBreakpointsAt(script, REGS.pc)) {
          if (!DebugAPI::onTrap(cx)) {
            goto error;
          }
        }
      }

      MOZ_ASSERT(activation.opMask() == EnableInterruptsPseudoOpcode);
      if (!moreInterrupts) {
        activation.clearInterruptsMask();
      }

      /* Commence executing the actual opcode. */
      SANITY_CHECKS();
      DISPATCH_TO(op);
    }

    /* Various 1-byte no-ops. */
    CASE(Nop)
    CASE(Try)
    CASE(NopDestructuring)
    CASE(TryDestructuring) {
      MOZ_ASSERT(GetBytecodeLength(REGS.pc) == 1);
      ADVANCE_AND_DISPATCH(1);
    }

    CASE(JumpTarget)
    COUNT_COVERAGE();
    END_CASE(JumpTarget)

    CASE(LoopHead) {
      COUNT_COVERAGE();

      // Attempt on-stack replacement into the Baseline Interpreter.
      if (jit::IsBaselineInterpreterEnabled()) {
        script->incWarmUpCounter();

        jit::MethodStatus status =
            jit::CanEnterBaselineInterpreterAtBranch(cx, REGS.fp());
        if (status == jit::Method_Error) {
          goto error;
        }
        if (status == jit::Method_Compiled) {
          bool wasProfiler = REGS.fp()->hasPushedGeckoProfilerFrame();

          jit::JitExecStatus maybeOsr;
          {
            GeckoProfilerBaselineOSRMarker osr(cx, wasProfiler);
            maybeOsr =
                jit::EnterBaselineInterpreterAtBranch(cx, REGS.fp(), REGS.pc);
          }

          // We failed to call into baseline at all, so treat as an error.
          if (maybeOsr == jit::JitExec_Aborted) {
            goto error;
          }

          interpReturnOK = (maybeOsr == jit::JitExec_Ok);

          // Pop the profiler frame pushed by the interpreter.  (The compiled
          // version of the function popped a copy of the frame pushed by the
          // OSR trampoline.)
          if (wasProfiler) {
            cx->geckoProfiler().exit(cx, script);
          }

          if (activation.entryFrame() != REGS.fp()) {
            goto jit_return_pop_frame;
          }
          goto leave_on_safe_point;
        }
      }
    }
    END_CASE(LoopHead)

    CASE(Lineno)
    END_CASE(Lineno)

    CASE(ForceInterpreter) {
      // Ensure pattern matching still works.
      MOZ_ASSERT(script->hasForceInterpreterOp());
    }
    END_CASE(ForceInterpreter)

    CASE(Undefined) { PUSH_UNDEFINED(); }
    END_CASE(Undefined)

    CASE(Pop) { REGS.sp--; }
    END_CASE(Pop)

    CASE(PopN) {
      MOZ_ASSERT(GET_UINT16(REGS.pc) <= REGS.stackDepth());
      REGS.sp -= GET_UINT16(REGS.pc);
    }
    END_CASE(PopN)

    CASE(DupAt) {
      MOZ_ASSERT(GET_UINT24(REGS.pc) < REGS.stackDepth());
      unsigned i = GET_UINT24(REGS.pc);
      const Value& rref = REGS.sp[-int(i + 1)];
      PUSH_COPY(rref);
    }
    END_CASE(DupAt)

    CASE(SetRval) { POP_RETURN_VALUE(); }
    END_CASE(SetRval)

    CASE(GetRval) { PUSH_COPY(REGS.fp()->returnValue()); }
    END_CASE(GetRval)

    CASE(EnterWith) {
      ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
      REGS.sp--;
      ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

      if (!EnterWithOperation(cx, REGS.fp(), val, scope.as<WithScope>())) {
        goto error;
      }
    }
    END_CASE(EnterWith)

    CASE(LeaveWith) {
      REGS.fp()->popOffEnvironmentChain<WithEnvironmentObject>();
    }
    END_CASE(LeaveWith)

    CASE(Return) {
      POP_RETURN_VALUE();
      /* FALL THROUGH */
    }
    CASE(RetRval) {
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
        if (MOZ_LIKELY(!frameHalfInitialized)) {
          interpReturnOK =
              DebugAPI::onLeaveFrame(cx, REGS.fp(), REGS.pc, interpReturnOK);

          REGS.fp()->epilogue(cx, REGS.pc);
        }

      jit_return_pop_frame:

        activation.popInlineFrame(REGS.fp());
        {
          JSScript* callerScript = REGS.fp()->script();
          if (cx->realm() != callerScript->realm()) {
            cx->leaveRealm(callerScript->realm());
          }
          SET_SCRIPT(callerScript);
        }

      jit_return:

        MOZ_ASSERT(IsInvokePC(REGS.pc));
        MOZ_ASSERT(cx->realm() == script->realm());

        /* Resume execution in the calling frame. */
        if (MOZ_LIKELY(interpReturnOK)) {
          if (JSOp(*REGS.pc) == JSOp::Resume) {
            ADVANCE_AND_DISPATCH(JSOpLength_Resume);
          }

          MOZ_ASSERT(GetBytecodeLength(REGS.pc) == JSOpLength_Call);
          ADVANCE_AND_DISPATCH(JSOpLength_Call);
        }

        goto error;
      } else {
        // Stack should be empty for the outer frame, unless we executed the
        // first |await| expression in an async function.
        MOZ_ASSERT(REGS.stackDepth() == 0 ||
                   (JSOp(*REGS.pc) == JSOp::Await &&
                    !REGS.fp()->isResumedGenerator()));
      }
      goto exit;
    }

    CASE(Default) {
      REGS.sp--;
      /* FALL THROUGH */
    }
    CASE(Goto) { BRANCH(GET_JUMP_OFFSET(REGS.pc)); }

    CASE(JumpIfFalse) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      REGS.sp--;
      if (!cond) {
        BRANCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(JumpIfFalse)

    CASE(JumpIfTrue) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      REGS.sp--;
      if (cond) {
        BRANCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(JumpIfTrue)

    CASE(Or) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      if (cond) {
        ADVANCE_AND_DISPATCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(Or)

    CASE(Coalesce) {
      MutableHandleValue res = REGS.stackHandleAt(-1);
      bool cond = !res.isNullOrUndefined();
      if (cond) {
        ADVANCE_AND_DISPATCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(Coalesce)

    CASE(And) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      if (!cond) {
        ADVANCE_AND_DISPATCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(And)

#define FETCH_ELEMENT_ID(n, id)                                       \
  JS_BEGIN_MACRO                                                      \
    if (!ToPropertyKey(cx, REGS.stackHandleAt(n), &(id))) goto error; \
  JS_END_MACRO

#define TRY_BRANCH_AFTER_COND(cond, spdec)                          \
  JS_BEGIN_MACRO                                                    \
    MOZ_ASSERT(GetBytecodeLength(REGS.pc) == 1);                    \
    unsigned diff_ =                                                \
        (unsigned)GET_UINT8(REGS.pc) - (unsigned)JSOp::JumpIfFalse; \
    if (diff_ <= 1) {                                               \
      REGS.sp -= (spdec);                                           \
      if ((cond) == (diff_ != 0)) {                                 \
        ++REGS.pc;                                                  \
        BRANCH(GET_JUMP_OFFSET(REGS.pc));                           \
      }                                                             \
      ADVANCE_AND_DISPATCH(1 + JSOpLength_JumpIfFalse);             \
    }                                                               \
  JS_END_MACRO

    CASE(In) {
      HandleValue rref = REGS.stackHandleAt(-1);
      if (!rref.isObject()) {
        HandleValue lref = REGS.stackHandleAt(-2);
        ReportInNotObjectError(cx, lref, rref);
        goto error;
      }
      bool found;
      {
        ReservedRooted<JSObject*> obj(&rootObject0, &rref.toObject());
        ReservedRooted<jsid> id(&rootId0);
        FETCH_ELEMENT_ID(-2, id);
        if (!HasProperty(cx, obj, id, &found)) {
          goto error;
        }
      }
      TRY_BRANCH_AFTER_COND(found, 2);
      REGS.sp--;
      REGS.sp[-1].setBoolean(found);
    }
    END_CASE(In)

    CASE(HasOwn) {
      HandleValue val = REGS.stackHandleAt(-1);
      HandleValue idval = REGS.stackHandleAt(-2);

      bool found;
      if (!HasOwnProperty(cx, val, idval, &found)) {
        goto error;
      }

      REGS.sp--;
      REGS.sp[-1].setBoolean(found);
    }
    END_CASE(HasOwn)

    CASE(CheckPrivateField) {
      /* Load the object being initialized into lval/val. */
      HandleValue val = REGS.stackHandleAt(-2);
      HandleValue idval = REGS.stackHandleAt(-1);

      bool result = false;
      if (!CheckPrivateFieldOperation(cx, REGS.pc, val, idval, &result)) {
        goto error;
      }

      PUSH_BOOLEAN(result);
    }
    END_CASE(CheckPrivateField)

    CASE(NewPrivateName) {
      ReservedRooted<JSAtom*> name(&rootAtom0, script->getAtom(REGS.pc));

      auto* symbol = NewPrivateName(cx, name);
      if (!symbol) {
        goto error;
      }

      PUSH_SYMBOL(symbol);
    }
    END_CASE(NewPrivateName)

    CASE(IsNullOrUndefined) {
      bool b = REGS.sp[-1].isNullOrUndefined();
      PUSH_BOOLEAN(b);
    }
    END_CASE(IsNullOrUndefined)

    CASE(Iter) {
      MOZ_ASSERT(REGS.stackDepth() >= 1);
      HandleValue val = REGS.stackHandleAt(-1);
      JSObject* iter = ValueToIterator(cx, val);
      if (!iter) {
        goto error;
      }
      REGS.sp[-1].setObject(*iter);
    }
    END_CASE(Iter)

    CASE(MoreIter) {
      MOZ_ASSERT(REGS.stackDepth() >= 1);
      MOZ_ASSERT(REGS.sp[-1].isObject());
      Value v = IteratorMore(&REGS.sp[-1].toObject());
      PUSH_COPY(v);
    }
    END_CASE(MoreIter)

    CASE(IsNoIter) {
      bool b = REGS.sp[-1].isMagic(JS_NO_ITER_VALUE);
      PUSH_BOOLEAN(b);
    }
    END_CASE(IsNoIter)

    CASE(EndIter) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      CloseIterator(&REGS.sp[-2].toObject());
      REGS.sp -= 2;
    }
    END_CASE(EndIter)

    CASE(CloseIter) {
      ReservedRooted<JSObject*> iter(&rootObject0, &REGS.sp[-1].toObject());
      CompletionKind kind = CompletionKind(GET_UINT8(REGS.pc));
      if (!CloseIterOperation(cx, iter, kind)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(CloseIter)

    CASE(IsGenClosing) {
      bool b = REGS.sp[-1].isMagic(JS_GENERATOR_CLOSING);
      PUSH_BOOLEAN(b);
    }
    END_CASE(IsGenClosing)

    CASE(Dup) {
      MOZ_ASSERT(REGS.stackDepth() >= 1);
      const Value& rref = REGS.sp[-1];
      PUSH_COPY(rref);
    }
    END_CASE(Dup)

    CASE(Dup2) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      const Value& lref = REGS.sp[-2];
      const Value& rref = REGS.sp[-1];
      PUSH_COPY(lref);
      PUSH_COPY(rref);
    }
    END_CASE(Dup2)

    CASE(Swap) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      Value& lref = REGS.sp[-2];
      Value& rref = REGS.sp[-1];
      lref.swap(rref);
    }
    END_CASE(Swap)

    CASE(Pick) {
      unsigned i = GET_UINT8(REGS.pc);
      MOZ_ASSERT(REGS.stackDepth() >= i + 1);
      Value lval = REGS.sp[-int(i + 1)];
      memmove(REGS.sp - (i + 1), REGS.sp - i, sizeof(Value) * i);
      REGS.sp[-1] = lval;
    }
    END_CASE(Pick)

    CASE(Unpick) {
      int i = GET_UINT8(REGS.pc);
      MOZ_ASSERT(REGS.stackDepth() >= unsigned(i) + 1);
      Value lval = REGS.sp[-1];
      memmove(REGS.sp - i, REGS.sp - (i + 1), sizeof(Value) * i);
      REGS.sp[-(i + 1)] = lval;
    }
    END_CASE(Unpick)

    CASE(BindGName)
    CASE(BindName) {
      JSOp op = JSOp(*REGS.pc);
      ReservedRooted<JSObject*> envChain(&rootObject0);
      if (op == JSOp::BindName) {
        envChain.set(REGS.fp()->environmentChain());
      } else {
        MOZ_ASSERT(!script->hasNonSyntacticScope());
        envChain.set(&REGS.fp()->global().lexicalEnvironment());
      }
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

      // Assigning to an undeclared name adds a property to the global object.
      ReservedRooted<JSObject*> env(&rootObject1);
      if (!LookupNameUnqualified(cx, name, envChain, &env)) {
        goto error;
      }

      PUSH_OBJECT(*env);

      static_assert(JSOpLength_BindName == JSOpLength_BindGName,
                    "We're sharing the END_CASE so the lengths better match");
    }
    END_CASE(BindName)

    CASE(BindVar) {
      JSObject* varObj = BindVarOperation(cx, REGS.fp()->environmentChain());
      PUSH_OBJECT(*varObj);
    }
    END_CASE(BindVar)

    CASE(BitOr) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitOrOperation(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(BitOr)

    CASE(BitXor) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitXorOperation(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(BitXor)

    CASE(BitAnd) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitAndOperation(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(BitAnd)

    CASE(Eq) {
      if (!LooseEqualityOp<true>(cx, REGS)) {
        goto error;
      }
    }
    END_CASE(Eq)

    CASE(Ne) {
      if (!LooseEqualityOp<false>(cx, REGS)) {
        goto error;
      }
    }
    END_CASE(Ne)

#define STRICT_EQUALITY_OP(OP, COND)                  \
  JS_BEGIN_MACRO                                      \
    HandleValue lval = REGS.stackHandleAt(-2);        \
    HandleValue rval = REGS.stackHandleAt(-1);        \
    bool equal;                                       \
    if (!js::StrictlyEqual(cx, lval, rval, &equal)) { \
      goto error;                                     \
    }                                                 \
    (COND) = equal OP true;                           \
    REGS.sp--;                                        \
  JS_END_MACRO

    CASE(StrictEq) {
      bool cond;
      STRICT_EQUALITY_OP(==, cond);
      REGS.sp[-1].setBoolean(cond);
    }
    END_CASE(StrictEq)

    CASE(StrictNe) {
      bool cond;
      STRICT_EQUALITY_OP(!=, cond);
      REGS.sp[-1].setBoolean(cond);
    }
    END_CASE(StrictNe)

#undef STRICT_EQUALITY_OP

    CASE(Case) {
      bool cond = REGS.sp[-1].toBoolean();
      REGS.sp--;
      if (cond) {
        REGS.sp--;
        BRANCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(Case)

    CASE(Lt) {
      bool cond;
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!LessThanOperation(cx, lval, rval, &cond)) {
        goto error;
      }
      TRY_BRANCH_AFTER_COND(cond, 2);
      REGS.sp[-2].setBoolean(cond);
      REGS.sp--;
    }
    END_CASE(Lt)

    CASE(Le) {
      bool cond;
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!LessThanOrEqualOperation(cx, lval, rval, &cond)) {
        goto error;
      }
      TRY_BRANCH_AFTER_COND(cond, 2);
      REGS.sp[-2].setBoolean(cond);
      REGS.sp--;
    }
    END_CASE(Le)

    CASE(Gt) {
      bool cond;
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!GreaterThanOperation(cx, lval, rval, &cond)) {
        goto error;
      }
      TRY_BRANCH_AFTER_COND(cond, 2);
      REGS.sp[-2].setBoolean(cond);
      REGS.sp--;
    }
    END_CASE(Gt)

    CASE(Ge) {
      bool cond;
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!GreaterThanOrEqualOperation(cx, lval, rval, &cond)) {
        goto error;
      }
      TRY_BRANCH_AFTER_COND(cond, 2);
      REGS.sp[-2].setBoolean(cond);
      REGS.sp--;
    }
    END_CASE(Ge)

    CASE(Lsh) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitLshOperation(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Lsh)

    CASE(Rsh) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitRshOperation(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Rsh)

    CASE(Ursh) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!UrshOperation(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Ursh)

    CASE(Add) {
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!AddOperation(cx, lval, rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Add)

    CASE(Sub) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!SubOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Sub)

    CASE(Mul) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!MulOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Mul)

    CASE(Div) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!DivOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Div)

    CASE(Mod) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!ModOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Mod)

    CASE(Pow) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!PowOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Pow)

    CASE(Not) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      REGS.sp--;
      PUSH_BOOLEAN(!cond);
    }
    END_CASE(Not)

    CASE(BitNot) {
      MutableHandleValue val = REGS.stackHandleAt(-1);
      if (!BitNotOperation(cx, val, val)) {
        goto error;
      }
    }
    END_CASE(BitNot)

    CASE(Neg) {
      MutableHandleValue val = REGS.stackHandleAt(-1);
      if (!NegOperation(cx, val, val)) {
        goto error;
      }
    }
    END_CASE(Neg)

    CASE(Pos) {
      if (!ToNumber(cx, REGS.stackHandleAt(-1))) {
        goto error;
      }
    }
    END_CASE(Pos)

    CASE(DelName) {
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
      ReservedRooted<JSObject*> envObj(&rootObject0,
                                       REGS.fp()->environmentChain());

      PUSH_BOOLEAN(true);
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!DeleteNameOperation(cx, name, envObj, res)) {
        goto error;
      }
    }
    END_CASE(DelName)

    CASE(DelProp)
    CASE(StrictDelProp) {
      static_assert(JSOpLength_DelProp == JSOpLength_StrictDelProp,
                    "delprop and strictdelprop must be the same size");
      HandleValue val = REGS.stackHandleAt(-1);
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
      bool res = false;
      if (JSOp(*REGS.pc) == JSOp::StrictDelProp) {
        if (!DelPropOperation<true>(cx, val, name, &res)) {
          goto error;
        }
      } else {
        if (!DelPropOperation<false>(cx, val, name, &res)) {
          goto error;
        }
      }
      REGS.sp[-1].setBoolean(res);
    }
    END_CASE(DelProp)

    CASE(DelElem)
    CASE(StrictDelElem) {
      static_assert(JSOpLength_DelElem == JSOpLength_StrictDelElem,
                    "delelem and strictdelelem must be the same size");
      HandleValue val = REGS.stackHandleAt(-2);
      HandleValue propval = REGS.stackHandleAt(-1);
      bool res = false;
      if (JSOp(*REGS.pc) == JSOp::StrictDelElem) {
        if (!DelElemOperation<true>(cx, val, propval, &res)) {
          goto error;
        }
      } else {
        if (!DelElemOperation<false>(cx, val, propval, &res)) {
          goto error;
        }
      }
      REGS.sp[-2].setBoolean(res);
      REGS.sp--;
    }
    END_CASE(DelElem)

    CASE(ToPropertyKey) {
      ReservedRooted<Value> idval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!ToPropertyKeyOperation(cx, idval, res)) {
        goto error;
      }
    }
    END_CASE(ToPropertyKey)

    CASE(TypeofExpr)
    CASE(Typeof) {
      REGS.sp[-1].setString(TypeOfOperation(REGS.sp[-1], cx->runtime()));
    }
    END_CASE(Typeof)

    CASE(Void) { REGS.sp[-1].setUndefined(); }
    END_CASE(Void)

    CASE(FunctionThis) {
      PUSH_NULL();
      if (!GetFunctionThis(cx, REGS.fp(), REGS.stackHandleAt(-1))) {
        goto error;
      }
    }
    END_CASE(FunctionThis)

    CASE(GlobalThis) {
      MOZ_ASSERT(!script->hasNonSyntacticScope());
      PUSH_OBJECT(*cx->global()->lexicalEnvironment().thisObject());
    }
    END_CASE(GlobalThis)

    CASE(NonSyntacticGlobalThis) {
      PUSH_NULL();
      GetNonSyntacticGlobalThis(cx, REGS.fp()->environmentChain(),
                                REGS.stackHandleAt(-1));
    }
    END_CASE(NonSyntacticGlobalThis)

    CASE(CheckIsObj) {
      if (!REGS.sp[-1].isObject()) {
        MOZ_ALWAYS_FALSE(
            ThrowCheckIsObject(cx, CheckIsObjectKind(GET_UINT8(REGS.pc))));
        goto error;
      }
    }
    END_CASE(CheckIsObj)

    CASE(CheckThis) {
      if (REGS.sp[-1].isMagic(JS_UNINITIALIZED_LEXICAL)) {
        MOZ_ALWAYS_FALSE(ThrowUninitializedThis(cx));
        goto error;
      }
    }
    END_CASE(CheckThis)

    CASE(CheckThisReinit) {
      if (!REGS.sp[-1].isMagic(JS_UNINITIALIZED_LEXICAL)) {
        MOZ_ALWAYS_FALSE(ThrowInitializedThis(cx));
        goto error;
      }
    }
    END_CASE(CheckThisReinit)

    CASE(CheckReturn) {
      ReservedRooted<Value> thisv(&rootValue0, REGS.sp[-1]);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!REGS.fp()->checkReturn(cx, thisv, rval)) {
        goto error;
      }
    }
    END_CASE(CheckReturn)

    CASE(GetProp) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-1);
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
      if (!GetPropertyOperation(cx, name, lval, res)) {
        goto error;
      }
      cx->debugOnlyCheck(res);
    }
    END_CASE(GetProp)

    CASE(GetPropSuper) {
      ReservedRooted<Value> receiver(&rootValue0, REGS.sp[-2]);
      HandleValue lval = REGS.stackHandleAt(-1);
      MOZ_ASSERT(lval.isObjectOrNull());
      MutableHandleValue rref = REGS.stackHandleAt(-2);
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

      ReservedRooted<JSObject*> obj(&rootObject0);
      obj = ToObjectFromStackForPropertyAccess(cx, lval, -1, name);
      if (!obj) {
        goto error;
      }

      if (!GetProperty(cx, obj, receiver, name, rref)) {
        goto error;
      }

      cx->debugOnlyCheck(rref);

      REGS.sp--;
    }
    END_CASE(GetPropSuper)

    CASE(GetBoundName) {
      ReservedRooted<JSObject*> env(&rootObject0, &REGS.sp[-1].toObject());
      ReservedRooted<jsid> id(&rootId0, NameToId(script->getName(REGS.pc)));
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!GetNameBoundInEnvironment(cx, env, id, rval)) {
        goto error;
      }
      cx->debugOnlyCheck(rval);
    }
    END_CASE(GetBoundName)

    CASE(SetIntrinsic) {
      HandleValue value = REGS.stackHandleAt(-1);

      if (!SetIntrinsicOperation(cx, script, REGS.pc, value)) {
        goto error;
      }
    }
    END_CASE(SetIntrinsic)

    CASE(SetGName)
    CASE(StrictSetGName)
    CASE(SetName)
    CASE(StrictSetName) {
      static_assert(JSOpLength_SetName == JSOpLength_StrictSetName,
                    "setname and strictsetname must be the same size");
      static_assert(JSOpLength_SetGName == JSOpLength_StrictSetGName,
                    "setgname and strictsetgname must be the same size");
      static_assert(JSOpLength_SetName == JSOpLength_SetGName,
                    "We're sharing the END_CASE so the lengths better match");

      ReservedRooted<JSObject*> env(&rootObject0, &REGS.sp[-2].toObject());
      HandleValue value = REGS.stackHandleAt(-1);

      if (!SetNameOperation(cx, script, REGS.pc, env, value)) {
        goto error;
      }

      REGS.sp[-2] = REGS.sp[-1];
      REGS.sp--;
    }
    END_CASE(SetName)

    CASE(SetProp)
    CASE(StrictSetProp) {
      static_assert(JSOpLength_SetProp == JSOpLength_StrictSetProp,
                    "setprop and strictsetprop must be the same size");
      int lvalIndex = -2;
      HandleValue lval = REGS.stackHandleAt(lvalIndex);
      HandleValue rval = REGS.stackHandleAt(-1);

      ReservedRooted<jsid> id(&rootId0, NameToId(script->getName(REGS.pc)));

      bool strict = JSOp(*REGS.pc) == JSOp::StrictSetProp;

      ReservedRooted<JSObject*> obj(&rootObject0);
      obj = ToObjectFromStackForPropertyAccess(cx, lval, lvalIndex, id);
      if (!obj) {
        goto error;
      }

      if (!SetObjectElementOperation(cx, obj, id, rval, lval, strict)) {
        goto error;
      }

      REGS.sp[-2] = REGS.sp[-1];
      REGS.sp--;
    }
    END_CASE(SetProp)

    CASE(SetPropSuper)
    CASE(StrictSetPropSuper) {
      static_assert(
          JSOpLength_SetPropSuper == JSOpLength_StrictSetPropSuper,
          "setprop-super and strictsetprop-super must be the same size");

      HandleValue receiver = REGS.stackHandleAt(-3);
      HandleValue lval = REGS.stackHandleAt(-2);
      MOZ_ASSERT(lval.isObjectOrNull());
      HandleValue rval = REGS.stackHandleAt(-1);
      ReservedRooted<jsid> id(&rootId0, NameToId(script->getName(REGS.pc)));

      bool strict = JSOp(*REGS.pc) == JSOp::StrictSetPropSuper;

      ReservedRooted<JSObject*> obj(&rootObject0);
      obj = ToObjectFromStackForPropertyAccess(cx, lval, -2, id);
      if (!obj) {
        goto error;
      }

      if (!SetObjectElementOperation(cx, obj, id, rval, receiver, strict)) {
        goto error;
      }

      REGS.sp[-3] = REGS.sp[-1];
      REGS.sp -= 2;
    }
    END_CASE(SetPropSuper)

    CASE(GetElem) {
      int lvalIndex = -2;
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[lvalIndex]);
      HandleValue rval = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);

      if (!GetElementOperationWithStackIndex(cx, lval, lvalIndex, rval, res)) {
        goto error;
      }

      REGS.sp--;
    }
    END_CASE(GetElem)

    CASE(GetElemSuper) {
      ReservedRooted<Value> receiver(&rootValue0, REGS.sp[-3]);
      HandleValue index = REGS.stackHandleAt(-2);
      HandleValue lval = REGS.stackHandleAt(-1);
      MOZ_ASSERT(lval.isObjectOrNull());

      MutableHandleValue res = REGS.stackHandleAt(-3);

      ReservedRooted<JSObject*> obj(&rootObject0);
      obj = ToObjectFromStackForPropertyAccess(cx, lval, -1, index);
      if (!obj) {
        goto error;
      }

      if (!GetObjectElementOperation(cx, JSOp(*REGS.pc), obj, receiver, index,
                                     res)) {
        goto error;
      }

      REGS.sp -= 2;
    }
    END_CASE(GetElemSuper)

    CASE(SetElem)
    CASE(StrictSetElem) {
      static_assert(JSOpLength_SetElem == JSOpLength_StrictSetElem,
                    "setelem and strictsetelem must be the same size");
      int receiverIndex = -3;
      HandleValue receiver = REGS.stackHandleAt(receiverIndex);
      HandleValue value = REGS.stackHandleAt(-1);

      ReservedRooted<JSObject*> obj(&rootObject0);
      obj = ToObjectFromStackForPropertyAccess(cx, receiver, receiverIndex,
                                               REGS.stackHandleAt(-2));
      if (!obj) {
        goto error;
      }

      ReservedRooted<jsid> id(&rootId0);
      FETCH_ELEMENT_ID(-2, id);

      if (!SetObjectElementOperation(cx, obj, id, value, receiver,
                                     JSOp(*REGS.pc) == JSOp::StrictSetElem)) {
        goto error;
      }
      REGS.sp[-3] = value;
      REGS.sp -= 2;
    }
    END_CASE(SetElem)

    CASE(SetElemSuper)
    CASE(StrictSetElemSuper) {
      static_assert(
          JSOpLength_SetElemSuper == JSOpLength_StrictSetElemSuper,
          "setelem-super and strictsetelem-super must be the same size");

      HandleValue receiver = REGS.stackHandleAt(-4);
      HandleValue lval = REGS.stackHandleAt(-2);
      MOZ_ASSERT(lval.isObjectOrNull());
      HandleValue value = REGS.stackHandleAt(-1);

      ReservedRooted<JSObject*> obj(&rootObject0);
      obj = ToObjectFromStackForPropertyAccess(cx, lval, -2,
                                               REGS.stackHandleAt(-3));
      if (!obj) {
        goto error;
      }

      ReservedRooted<jsid> id(&rootId0);
      FETCH_ELEMENT_ID(-3, id);

      bool strict = JSOp(*REGS.pc) == JSOp::StrictSetElemSuper;
      if (!SetObjectElementOperation(cx, obj, id, value, receiver, strict)) {
        goto error;
      }
      REGS.sp[-4] = value;
      REGS.sp -= 3;
    }
    END_CASE(SetElemSuper)

    CASE(Eval)
    CASE(StrictEval) {
      static_assert(JSOpLength_Eval == JSOpLength_StrictEval,
                    "eval and stricteval must be the same size");

      CallArgs args = CallArgsFromSp(GET_ARGC(REGS.pc), REGS.sp);
      if (cx->global()->valueIsEval(args.calleev())) {
        if (!DirectEval(cx, args.get(0), args.rval())) {
          goto error;
        }
      } else {
        if (!CallFromStack(cx, args, CallReason::Call)) {
          goto error;
        }
      }

      REGS.sp = args.spAfterCall();
    }
    END_CASE(Eval)

    CASE(SpreadNew)
    CASE(SpreadCall)
    CASE(SpreadSuperCall) {
      if (REGS.fp()->hasPushedGeckoProfilerFrame()) {
        cx->geckoProfiler().updatePC(cx, script, REGS.pc);
      }
      /* FALL THROUGH */
    }

    CASE(SpreadEval)
    CASE(StrictSpreadEval) {
      static_assert(JSOpLength_SpreadEval == JSOpLength_StrictSpreadEval,
                    "spreadeval and strictspreadeval must be the same size");
      bool construct = JSOp(*REGS.pc) == JSOp::SpreadNew ||
                       JSOp(*REGS.pc) == JSOp::SpreadSuperCall;

      MOZ_ASSERT(REGS.stackDepth() >= 3u + construct);

      HandleValue callee = REGS.stackHandleAt(-3 - construct);
      HandleValue thisv = REGS.stackHandleAt(-2 - construct);
      HandleValue arr = REGS.stackHandleAt(-1 - construct);
      MutableHandleValue ret = REGS.stackHandleAt(-3 - construct);

      RootedValue& newTarget = rootValue0;
      if (construct) {
        newTarget = REGS.sp[-1];
      } else {
        newTarget = NullValue();
      }

      if (!SpreadCallOperation(cx, script, REGS.pc, thisv, callee, arr,
                               newTarget, ret)) {
        goto error;
      }

      REGS.sp -= 2 + construct;
    }
    END_CASE(SpreadCall)

    CASE(New)
    CASE(NewContent)
    CASE(Call)
    CASE(CallContent)
    CASE(CallIgnoresRv)
    CASE(CallIter)
    CASE(CallContentIter)
    CASE(SuperCall) {
      static_assert(JSOpLength_Call == JSOpLength_New,
                    "call and new must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_CallContent,
                    "call and call-content must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_CallIgnoresRv,
                    "call and call-ignores-rv must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_CallIter,
                    "call and calliter must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_CallContentIter,
                    "call and call-content-iter must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_SuperCall,
                    "call and supercall must be the same size");

      if (REGS.fp()->hasPushedGeckoProfilerFrame()) {
        cx->geckoProfiler().updatePC(cx, script, REGS.pc);
      }

      JSOp op = JSOp(*REGS.pc);
      MaybeConstruct construct = MaybeConstruct(
          op == JSOp::New || op == JSOp::NewContent || op == JSOp::SuperCall);
      bool ignoresReturnValue = op == JSOp::CallIgnoresRv;
      unsigned argStackSlots = GET_ARGC(REGS.pc) + construct;

      MOZ_ASSERT(REGS.stackDepth() >= 2u + GET_ARGC(REGS.pc));
      CallArgs args =
          CallArgsFromSp(argStackSlots, REGS.sp, construct, ignoresReturnValue);

      JSFunction* maybeFun;
      bool isFunction = IsFunctionObject(args.calleev(), &maybeFun);

      // Use the slow path if the callee is not an interpreted function, if we
      // have to throw an exception, or if we might have to invoke the
      // OnNativeCall hook for a self-hosted builtin.
      if (!isFunction || !maybeFun->isInterpreted() ||
          (construct && !maybeFun->isConstructor()) ||
          (!construct && maybeFun->isClassConstructor()) ||
          cx->insideDebuggerEvaluationWithOnNativeCallHook) {
        if (construct) {
          CallReason reason = op == JSOp::NewContent ? CallReason::CallContent
                                                     : CallReason::Call;
          if (!ConstructFromStack(cx, args, reason)) {
            goto error;
          }
        } else {
          if ((op == JSOp::CallIter || op == JSOp::CallContentIter) &&
              args.calleev().isPrimitive()) {
            MOZ_ASSERT(args.length() == 0, "thisv must be on top of the stack");
            ReportValueError(cx, JSMSG_NOT_ITERABLE, -1, args.thisv(), nullptr);
            goto error;
          }

          CallReason reason =
              (op == JSOp::CallContent || op == JSOp::CallContentIter)
                  ? CallReason::CallContent
                  : CallReason::Call;
          if (!CallFromStack(cx, args, reason)) {
            goto error;
          }
        }
        Value* newsp = args.spAfterCall();
        REGS.sp = newsp;
        ADVANCE_AND_DISPATCH(JSOpLength_Call);
      }

      {
        MOZ_ASSERT(maybeFun);
        ReservedRooted<JSFunction*> fun(&rootFunction0, maybeFun);
        ReservedRooted<JSScript*> funScript(
            &rootScript0, JSFunction::getOrCreateScript(cx, fun));
        if (!funScript) {
          goto error;
        }

        // Enter the callee's realm if this is a cross-realm call. Use
        // MakeScopeExit to leave this realm on all error/JIT-return paths
        // below.
        const bool isCrossRealm = cx->realm() != funScript->realm();
        if (isCrossRealm) {
          cx->enterRealmOf(funScript);
        }
        auto leaveRealmGuard =
            mozilla::MakeScopeExit([isCrossRealm, cx, &script] {
              if (isCrossRealm) {
                cx->leaveRealm(script->realm());
              }
            });

        if (construct && !MaybeCreateThisForConstructor(cx, args)) {
          goto error;
        }

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

#ifdef NIGHTLY_BUILD
          // If entry trampolines are enabled, call back into
          // MaybeEnterInterpreterTrampoline so we can generate an
          // entry trampoline for the new frame.
          if (jit::JitOptions.emitInterpreterEntryTrampoline) {
            if (MaybeEnterInterpreterTrampoline(cx, state)) {
              interpReturnOK = true;
              CHECK_BRANCH();
              REGS.sp = args.spAfterCall();
              goto jit_return;
            }
            goto error;
          }
#endif
        }

        funScript = fun->nonLazyScript();

        if (!activation.pushInlineFrame(args, funScript, construct)) {
          goto error;
        }
        leaveRealmGuard.release();  // We leave the callee's realm when we
                                    // call popInlineFrame.
      }

      SET_SCRIPT(REGS.fp()->script());

      if (!REGS.fp()->prologue(cx)) {
        goto prologue_error;
      }

      if (!DebugAPI::onEnterFrame(cx, REGS.fp())) {
        goto error;
      }

      // Increment the coverage for the main entry point.
      INIT_COVERAGE();
      COUNT_COVERAGE_MAIN();

      /* Load first op and dispatch it (safe since JSOp::RetRval). */
      ADVANCE_AND_DISPATCH(0);
    }

    CASE(OptimizeSpreadCall) {
      ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
      MutableHandleValue rval = REGS.stackHandleAt(-1);

      if (!OptimizeSpreadCall(cx, val, rval)) {
        goto error;
      }
    }
    END_CASE(OptimizeSpreadCall)

    CASE(ThrowMsg) {
      MOZ_ALWAYS_FALSE(ThrowMsgOperation(cx, GET_UINT8(REGS.pc)));
      goto error;
    }
    END_CASE(ThrowMsg)

    CASE(ImplicitThis) {
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
      ReservedRooted<JSObject*> envObj(&rootObject0,
                                       REGS.fp()->environmentChain());
      ReservedRooted<JSObject*> env(&rootObject1);
      if (!LookupNameWithGlobalDefault(cx, name, envObj, &env)) {
        goto error;
      }

      Value v = ComputeImplicitThis(env);
      PUSH_COPY(v);
    }
    END_CASE(ImplicitThis)

    CASE(GetGName) {
      ReservedRooted<Value> rval(&rootValue0);
      ReservedRooted<JSObject*> env(&rootObject0,
                                    &cx->global()->lexicalEnvironment());
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
      MOZ_ASSERT(!script->hasNonSyntacticScope());
      if (!GetNameOperation(cx, env, name, JSOp(REGS.pc[JSOpLength_GetGName]),
                            &rval)) {
        goto error;
      }

      PUSH_COPY(rval);
    }
    END_CASE(GetGName)

    CASE(GetName) {
      ReservedRooted<Value> rval(&rootValue0);
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
      if (!GetNameOperation(cx, REGS.fp()->environmentChain(), name,
                            JSOp(REGS.pc[JSOpLength_GetName]), &rval)) {
        goto error;
      }

      PUSH_COPY(rval);
    }
    END_CASE(GetName)

    CASE(GetImport) {
      PUSH_NULL();
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      HandleObject envChain = REGS.fp()->environmentChain();
      if (!GetImportOperation(cx, envChain, script, REGS.pc, rval)) {
        goto error;
      }
    }
    END_CASE(GetImport)

    CASE(GetIntrinsic) {
      ReservedRooted<Value> rval(&rootValue0);
      if (!GetIntrinsicOperation(cx, script, REGS.pc, &rval)) {
        goto error;
      }

      PUSH_COPY(rval);
    }
    END_CASE(GetIntrinsic)

    CASE(Uint16) { PUSH_INT32((int32_t)GET_UINT16(REGS.pc)); }
    END_CASE(Uint16)

    CASE(Uint24) { PUSH_INT32((int32_t)GET_UINT24(REGS.pc)); }
    END_CASE(Uint24)

    CASE(Int8) { PUSH_INT32(GET_INT8(REGS.pc)); }
    END_CASE(Int8)

    CASE(Int32) { PUSH_INT32(GET_INT32(REGS.pc)); }
    END_CASE(Int32)

    CASE(Double) { PUSH_COPY(GET_INLINE_VALUE(REGS.pc)); }
    END_CASE(Double)

    CASE(String) { PUSH_STRING(script->getString(REGS.pc)); }
    END_CASE(String)

    CASE(ToString) {
      MutableHandleValue oper = REGS.stackHandleAt(-1);

      if (!oper.isString()) {
        JSString* operString = ToString<CanGC>(cx, oper);
        if (!operString) {
          goto error;
        }
        oper.setString(operString);
      }
    }
    END_CASE(ToString)

    CASE(Symbol) {
      PUSH_SYMBOL(cx->wellKnownSymbols().get(GET_UINT8(REGS.pc)));
    }
    END_CASE(Symbol)

    CASE(Object) {
      MOZ_ASSERT(script->treatAsRunOnce());
      PUSH_OBJECT(*script->getObject(REGS.pc));
    }
    END_CASE(Object)

    CASE(CallSiteObj) {
      JSObject* cso = script->getObject(REGS.pc);
      MOZ_ASSERT(!cso->as<ArrayObject>().isExtensible());
      MOZ_ASSERT(cso->as<ArrayObject>().containsPure(cx->names().raw));
      PUSH_OBJECT(*cso);
    }
    END_CASE(CallSiteObj)

    CASE(RegExp) {
      /*
       * Push a regexp object cloned from the regexp literal object mapped by
       * the bytecode at pc.
       */
      ReservedRooted<JSObject*> re(&rootObject0, script->getRegExp(REGS.pc));
      JSObject* obj = CloneRegExpObject(cx, re.as<RegExpObject>());
      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(RegExp)

    CASE(Zero) { PUSH_INT32(0); }
    END_CASE(Zero)

    CASE(One) { PUSH_INT32(1); }
    END_CASE(One)

    CASE(Null) { PUSH_NULL(); }
    END_CASE(Null)

    CASE(False) { PUSH_BOOLEAN(false); }
    END_CASE(False)

    CASE(True) { PUSH_BOOLEAN(true); }
    END_CASE(True)

    CASE(TableSwitch) {
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
        if (!rref.isDouble() || !NumberEqualsInt32(rref.toDouble(), &i)) {
          ADVANCE_AND_DISPATCH(len);
        }
      }

      pc2 += JUMP_OFFSET_LEN;
      int32_t low = GET_JUMP_OFFSET(pc2);
      pc2 += JUMP_OFFSET_LEN;
      int32_t high = GET_JUMP_OFFSET(pc2);

      i = uint32_t(i) - uint32_t(low);
      if (uint32_t(i) < uint32_t(high - low + 1)) {
        len = script->tableSwitchCaseOffset(REGS.pc, uint32_t(i)) -
              script->pcToOffset(REGS.pc);
      }
      ADVANCE_AND_DISPATCH(len);
    }

    CASE(Arguments) {
      MOZ_ASSERT(script->needsArgsObj());
      ArgumentsObject* obj = ArgumentsObject::createExpected(cx, REGS.fp());
      if (!obj) {
        goto error;
      }
      PUSH_COPY(ObjectValue(*obj));
    }
    END_CASE(Arguments)

    CASE(Rest) {
      ReservedRooted<JSObject*> rest(&rootObject0,
                                     REGS.fp()->createRestParameter(cx));
      if (!rest) {
        goto error;
      }
      PUSH_COPY(ObjectValue(*rest));
    }
    END_CASE(Rest)

    CASE(GetAliasedVar) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
      ReservedRooted<Value> val(
          &rootValue0, REGS.fp()->aliasedEnvironment(ec).aliasedBinding(ec));

      ASSERT_UNINITIALIZED_ALIASED_LEXICAL(val);

      PUSH_COPY(val);
    }
    END_CASE(GetAliasedVar)

    CASE(GetAliasedDebugVar) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
      ReservedRooted<Value> val(
          &rootValue0,
          REGS.fp()->aliasedEnvironmentMaybeDebug(ec).aliasedBinding(ec));

      ASSERT_UNINITIALIZED_ALIASED_LEXICAL(val);

      PUSH_COPY(val);
    }
    END_CASE(GetAliasedVar)

    CASE(SetAliasedVar) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
      EnvironmentObject& obj = REGS.fp()->aliasedEnvironment(ec);
      MOZ_ASSERT(!IsUninitializedLexical(obj.aliasedBinding(ec)));
      obj.setAliasedBinding(ec, REGS.sp[-1]);
    }
    END_CASE(SetAliasedVar)

    CASE(ThrowSetConst) {
      ReportRuntimeLexicalError(cx, JSMSG_BAD_CONST_ASSIGN, script, REGS.pc);
      goto error;
    }
    END_CASE(ThrowSetConst)

    CASE(CheckLexical) {
      if (REGS.sp[-1].isMagic(JS_UNINITIALIZED_LEXICAL)) {
        ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, script,
                                  REGS.pc);
        goto error;
      }
    }
    END_CASE(CheckLexical)

    CASE(CheckAliasedLexical) {
      if (REGS.sp[-1].isMagic(JS_UNINITIALIZED_LEXICAL)) {
        ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, script,
                                  REGS.pc);
        goto error;
      }
    }
    END_CASE(CheckAliasedLexical)

    CASE(InitLexical) {
      uint32_t i = GET_LOCALNO(REGS.pc);
      REGS.fp()->unaliasedLocal(i) = REGS.sp[-1];
    }
    END_CASE(InitLexical)

    CASE(InitAliasedLexical) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
      EnvironmentObject& obj = REGS.fp()->aliasedEnvironment(ec);
      obj.setAliasedBinding(ec, REGS.sp[-1]);
    }
    END_CASE(InitAliasedLexical)

    CASE(InitGLexical) {
      ExtensibleLexicalEnvironmentObject* lexicalEnv;
      if (script->hasNonSyntacticScope()) {
        lexicalEnv = &REGS.fp()->extensibleLexicalEnvironment();
      } else {
        lexicalEnv = &cx->global()->lexicalEnvironment();
      }
      HandleValue value = REGS.stackHandleAt(-1);
      InitGlobalLexicalOperation(cx, lexicalEnv, script, REGS.pc, value);
    }
    END_CASE(InitGLexical)

    CASE(Uninitialized) { PUSH_MAGIC(JS_UNINITIALIZED_LEXICAL); }
    END_CASE(Uninitialized)

    CASE(GetArg) {
      unsigned i = GET_ARGNO(REGS.pc);
      if (script->argsObjAliasesFormals()) {
        PUSH_COPY(REGS.fp()->argsObj().arg(i));
      } else {
        PUSH_COPY(REGS.fp()->unaliasedFormal(i));
      }
    }
    END_CASE(GetArg)

    CASE(GetFrameArg) {
      uint32_t i = GET_ARGNO(REGS.pc);
      PUSH_COPY(REGS.fp()->unaliasedFormal(i, DONT_CHECK_ALIASING));
    }
    END_CASE(GetFrameArg)

    CASE(SetArg) {
      unsigned i = GET_ARGNO(REGS.pc);
      if (script->argsObjAliasesFormals()) {
        REGS.fp()->argsObj().setArg(i, REGS.sp[-1]);
      } else {
        REGS.fp()->unaliasedFormal(i) = REGS.sp[-1];
      }
    }
    END_CASE(SetArg)

    CASE(GetLocal) {
      uint32_t i = GET_LOCALNO(REGS.pc);
      PUSH_COPY_SKIP_CHECK(REGS.fp()->unaliasedLocal(i));

#ifdef DEBUG
      if (IsUninitializedLexical(REGS.sp[-1])) {
        JSOp next = JSOp(*GetNextPc(REGS.pc));
        MOZ_ASSERT(next == JSOp::CheckThis || next == JSOp::CheckReturn ||
                   next == JSOp::CheckThisReinit || next == JSOp::CheckLexical);
      }

      /*
       * Skip the same-compartment assertion if the local will be immediately
       * popped. We do not guarantee sync for dead locals when coming in from
       * the method JIT, and a GetLocal followed by Pop is not considered to
       * be a use of the variable.
       */
      if (JSOp(REGS.pc[JSOpLength_GetLocal]) != JSOp::Pop) {
        cx->debugOnlyCheck(REGS.sp[-1]);
      }
#endif
    }
    END_CASE(GetLocal)

    CASE(SetLocal) {
      uint32_t i = GET_LOCALNO(REGS.pc);

      MOZ_ASSERT(!IsUninitializedLexical(REGS.fp()->unaliasedLocal(i)));

      REGS.fp()->unaliasedLocal(i) = REGS.sp[-1];
    }
    END_CASE(SetLocal)

    CASE(ArgumentsLength) {
      MOZ_ASSERT(!script->needsArgsObj());
      PUSH_INT32(REGS.fp()->numActualArgs());
    }
    END_CASE(ArgumentsLength)

    CASE(GetActualArg) {
      MOZ_ASSERT(!script->needsArgsObj());
      uint32_t index = REGS.sp[-1].toInt32();
      REGS.sp[-1] = REGS.fp()->unaliasedActual(index);
    }
    END_CASE(GetActualArg)

    CASE(GlobalOrEvalDeclInstantiation) {
      GCThingIndex lastFun = GET_GCTHING_INDEX(REGS.pc);
      HandleObject env = REGS.fp()->environmentChain();
      if (!GlobalOrEvalDeclInstantiation(cx, env, script, lastFun)) {
        goto error;
      }
    }
    END_CASE(GlobalOrEvalDeclInstantiation)

    CASE(Lambda) {
      /* Load the specified function object literal. */
      ReservedRooted<JSFunction*> fun(&rootFunction0,
                                      script->getFunction(REGS.pc));
      JSObject* obj = Lambda(cx, fun, REGS.fp()->environmentChain());
      if (!obj) {
        goto error;
      }

      MOZ_ASSERT(obj->staticPrototype());
      PUSH_OBJECT(*obj);
    }
    END_CASE(Lambda)

    CASE(ToAsyncIter) {
      ReservedRooted<Value> nextMethod(&rootValue0, REGS.sp[-1]);
      ReservedRooted<JSObject*> iter(&rootObject1, &REGS.sp[-2].toObject());
      JSObject* asyncIter = CreateAsyncFromSyncIterator(cx, iter, nextMethod);
      if (!asyncIter) {
        goto error;
      }

      REGS.sp--;
      REGS.sp[-1].setObject(*asyncIter);
    }
    END_CASE(ToAsyncIter)

    CASE(CanSkipAwait) {
      ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
      bool canSkip;
      if (!CanSkipAwait(cx, val, &canSkip)) {
        goto error;
      }

      PUSH_BOOLEAN(canSkip);
    }
    END_CASE(CanSkipAwait)

    CASE(MaybeExtractAwaitValue) {
      MutableHandleValue val = REGS.stackHandleAt(-2);
      ReservedRooted<Value> canSkip(&rootValue0, REGS.sp[-1]);

      if (canSkip.toBoolean()) {
        if (!ExtractAwaitValue(cx, val, val)) {
          goto error;
        }
      }
    }
    END_CASE(MaybeExtractAwaitValue)

    CASE(AsyncAwait) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      ReservedRooted<JSObject*> gen(&rootObject1, &REGS.sp[-1].toObject());
      ReservedRooted<Value> value(&rootValue0, REGS.sp[-2]);
      JSObject* promise =
          AsyncFunctionAwait(cx, gen.as<AsyncFunctionGeneratorObject>(), value);
      if (!promise) {
        goto error;
      }

      REGS.sp--;
      REGS.sp[-1].setObject(*promise);
    }
    END_CASE(AsyncAwait)

    CASE(AsyncResolve) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      auto resolveKind = AsyncFunctionResolveKind(GET_UINT8(REGS.pc));
      ReservedRooted<JSObject*> gen(&rootObject1, &REGS.sp[-1].toObject());
      ReservedRooted<Value> valueOrReason(&rootValue0, REGS.sp[-2]);
      JSObject* promise =
          AsyncFunctionResolve(cx, gen.as<AsyncFunctionGeneratorObject>(),
                               valueOrReason, resolveKind);
      if (!promise) {
        goto error;
      }

      REGS.sp--;
      REGS.sp[-1].setObject(*promise);
    }
    END_CASE(AsyncResolve)

    CASE(SetFunName) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      FunctionPrefixKind prefixKind = FunctionPrefixKind(GET_UINT8(REGS.pc));
      ReservedRooted<Value> name(&rootValue0, REGS.sp[-1]);
      ReservedRooted<JSFunction*> fun(&rootFunction0,
                                      &REGS.sp[-2].toObject().as<JSFunction>());
      if (!SetFunctionName(cx, fun, name, prefixKind)) {
        goto error;
      }

      REGS.sp--;
    }
    END_CASE(SetFunName)

    CASE(Callee) {
      MOZ_ASSERT(REGS.fp()->isFunctionFrame());
      PUSH_COPY(REGS.fp()->calleev());
    }
    END_CASE(Callee)

    CASE(InitPropGetter)
    CASE(InitHiddenPropGetter)
    CASE(InitPropSetter)
    CASE(InitHiddenPropSetter) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);

      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
      ReservedRooted<JSObject*> val(&rootObject1, &REGS.sp[-1].toObject());

      if (!InitPropGetterSetterOperation(cx, REGS.pc, obj, name, val)) {
        goto error;
      }

      REGS.sp--;
    }
    END_CASE(InitPropGetter)

    CASE(InitElemGetter)
    CASE(InitHiddenElemGetter)
    CASE(InitElemSetter)
    CASE(InitHiddenElemSetter) {
      MOZ_ASSERT(REGS.stackDepth() >= 3);

      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-3].toObject());
      ReservedRooted<Value> idval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<JSObject*> val(&rootObject1, &REGS.sp[-1].toObject());

      if (!InitElemGetterSetterOperation(cx, REGS.pc, obj, idval, val)) {
        goto error;
      }

      REGS.sp -= 2;
    }
    END_CASE(InitElemGetter)

    CASE(Hole) { PUSH_MAGIC(JS_ELEMENTS_HOLE); }
    END_CASE(Hole)

    CASE(NewInit) {
      JSObject* obj = NewObjectOperation(cx, script, REGS.pc);

      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(NewInit)

    CASE(NewArray) {
      uint32_t length = GET_UINT32(REGS.pc);
      ArrayObject* obj = NewArrayOperation(cx, length);
      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(NewArray)

    CASE(NewObject) {
      JSObject* obj = NewObjectOperation(cx, script, REGS.pc);
      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(NewObject)

    CASE(MutateProto) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);

      if (REGS.sp[-1].isObjectOrNull()) {
        ReservedRooted<JSObject*> newProto(&rootObject1,
                                           REGS.sp[-1].toObjectOrNull());
        ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());
        MOZ_ASSERT(obj->is<PlainObject>());

        if (!SetPrototype(cx, obj, newProto)) {
          goto error;
        }
      }

      REGS.sp--;
    }
    END_CASE(MutateProto)

    CASE(InitProp)
    CASE(InitLockedProp)
    CASE(InitHiddenProp) {
      static_assert(JSOpLength_InitProp == JSOpLength_InitLockedProp,
                    "initprop and initlockedprop must be the same size");
      static_assert(JSOpLength_InitProp == JSOpLength_InitHiddenProp,
                    "initprop and inithiddenprop must be the same size");
      /* Load the property's initial value into rval. */
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      ReservedRooted<Value> rval(&rootValue0, REGS.sp[-1]);

      /* Load the object being initialized into lval/obj. */
      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());

      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

      if (!InitPropertyOperation(cx, REGS.pc, obj, name, rval)) {
        goto error;
      }

      REGS.sp--;
    }
    END_CASE(InitProp)

    CASE(InitElem)
    CASE(InitHiddenElem)
    CASE(InitLockedElem) {
      MOZ_ASSERT(REGS.stackDepth() >= 3);
      HandleValue val = REGS.stackHandleAt(-1);
      HandleValue id = REGS.stackHandleAt(-2);

      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-3].toObject());

      if (!InitElemOperation(cx, REGS.pc, obj, id, val)) {
        goto error;
      }

      REGS.sp -= 2;
    }
    END_CASE(InitElem)

    CASE(InitElemArray) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      HandleValue val = REGS.stackHandleAt(-1);
      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());

      InitElemArrayOperation(cx, REGS.pc, obj.as<ArrayObject>(), val);
      REGS.sp--;
    }
    END_CASE(InitElemArray)

    CASE(InitElemInc) {
      MOZ_ASSERT(REGS.stackDepth() >= 3);
      HandleValue val = REGS.stackHandleAt(-1);

      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-3].toObject());

      uint32_t index = REGS.sp[-2].toInt32();
      if (!InitElemIncOperation(cx, obj.as<ArrayObject>(), index, val)) {
        goto error;
      }

      REGS.sp[-2].setInt32(index + 1);
      REGS.sp--;
    }
    END_CASE(InitElemInc)

#ifdef ENABLE_RECORD_TUPLE
    CASE(InitRecord) {
      uint32_t length = GET_UINT32(REGS.pc);
      RecordType* rec = RecordType::createUninitialized(cx, length);
      if (!rec) {
        goto error;
      }
      PUSH_EXTENDED_PRIMITIVE(*rec);
    }
    END_CASE(InitRecord)

    CASE(AddRecordProperty) {
      MOZ_ASSERT(REGS.stackDepth() >= 3);

      ReservedRooted<JSObject*> rec(&rootObject0,
                                    &REGS.sp[-3].toExtendedPrimitive());
      MOZ_ASSERT(rec->is<RecordType>());

      ReservedRooted<Value> key(&rootValue0, REGS.sp[-2]);
      ReservedRooted<jsid> id(&rootId0);
      if (!JS_ValueToId(cx, key, &id)) {
        goto error;
      }
      if (!rec->as<RecordType>().initializeNextProperty(
              cx, id, REGS.stackHandleAt(-1))) {
        goto error;
      }

      REGS.sp -= 2;
    }
    END_CASE(AddRecordProperty)

    CASE(AddRecordSpread) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);

      if (!AddRecordSpreadOperation(cx, REGS.stackHandleAt(-2),
                                    REGS.stackHandleAt(-1))) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(AddRecordSpread)

    CASE(FinishRecord) {
      MOZ_ASSERT(REGS.stackDepth() >= 1);
      RecordType* rec = &REGS.sp[-1].toExtendedPrimitive().as<RecordType>();
      if (!rec->finishInitialization(cx)) {
        goto error;
      }
    }
    END_CASE(FinishRecord)

    CASE(InitTuple) {
      uint32_t length = GET_UINT32(REGS.pc);
      TupleType* tup = TupleType::createUninitialized(cx, length);
      if (!tup) {
        goto error;
      }
      PUSH_EXTENDED_PRIMITIVE(*tup);
    }
    END_CASE(InitTuple)

    CASE(AddTupleElement) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);

      ReservedRooted<JSObject*> tup(&rootObject0,
                                    &REGS.sp[-2].toExtendedPrimitive());
      HandleValue val = REGS.stackHandleAt(-1);

      if (!tup->as<TupleType>().initializeNextElement(cx, val)) {
        goto error;
      }

      REGS.sp--;
    }
    END_CASE(AddTupleElement)

    CASE(FinishTuple) {
      MOZ_ASSERT(REGS.stackDepth() >= 1);
      TupleType& tup = REGS.sp[-1].toExtendedPrimitive().as<TupleType>();
      tup.finishInitialization(cx);
    }
    END_CASE(FinishTuple)
#endif

    CASE(Exception) {
      PUSH_NULL();
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!GetAndClearException(cx, res)) {
        goto error;
      }
    }
    END_CASE(Exception)

    CASE(Finally) { CHECK_BRANCH(); }
    END_CASE(Finally)

    CASE(Throw) {
      CHECK_BRANCH();
      ReservedRooted<Value> v(&rootValue0);
      POP_COPY_TO(v);
      MOZ_ALWAYS_FALSE(ThrowOperation(cx, v));
      /* let the code at error try to catch the exception. */
      goto error;
    }

    CASE(Instanceof) {
      ReservedRooted<Value> rref(&rootValue0, REGS.sp[-1]);
      if (HandleValue(rref).isPrimitive()) {
        ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS, -1, rref, nullptr);
        goto error;
      }
      ReservedRooted<JSObject*> obj(&rootObject0, &rref.toObject());
      bool cond = false;
      if (!InstanceofOperator(cx, obj, REGS.stackHandleAt(-2), &cond)) {
        goto error;
      }
      REGS.sp--;
      REGS.sp[-1].setBoolean(cond);
    }
    END_CASE(Instanceof)

    CASE(Debugger) {
      if (!DebugAPI::onDebuggerStatement(cx, REGS.fp())) {
        goto error;
      }
    }
    END_CASE(Debugger)

    CASE(PushLexicalEnv) {
      ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

      // Create block environment and push on scope chain.
      if (!REGS.fp()->pushLexicalEnvironment(cx, scope.as<LexicalScope>())) {
        goto error;
      }
    }
    END_CASE(PushLexicalEnv)

    CASE(PopLexicalEnv) {
#ifdef DEBUG
      Scope* scope = script->lookupScope(REGS.pc);
      MOZ_ASSERT(scope);
      MOZ_ASSERT(scope->is<LexicalScope>() || scope->is<ClassBodyScope>());
      MOZ_ASSERT_IF(scope->is<LexicalScope>(),
                    scope->as<LexicalScope>().hasEnvironment());
      MOZ_ASSERT_IF(scope->is<ClassBodyScope>(),
                    scope->as<ClassBodyScope>().hasEnvironment());
#endif

      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);
      }

      // Pop block from scope chain.
      REGS.fp()->popOffEnvironmentChain<LexicalEnvironmentObject>();
    }
    END_CASE(PopLexicalEnv)

    CASE(DebugLeaveLexicalEnv) {
#ifdef DEBUG
      Scope* scope = script->lookupScope(REGS.pc);
      MOZ_ASSERT(scope);
      MOZ_ASSERT(scope->is<LexicalScope>() || scope->is<ClassBodyScope>());
      MOZ_ASSERT_IF(scope->is<LexicalScope>(),
                    !scope->as<LexicalScope>().hasEnvironment());
      MOZ_ASSERT_IF(scope->is<ClassBodyScope>(),
                    !scope->as<ClassBodyScope>().hasEnvironment());
#endif
      // FIXME: This opcode should not be necessary.  The debugger shouldn't
      // need help from bytecode to do its job.  See bug 927782.

      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);
      }
    }
    END_CASE(DebugLeaveLexicalEnv)

    CASE(FreshenLexicalEnv) {
#ifdef DEBUG
      Scope* scope = script->getScope(REGS.pc);
      auto envChain = REGS.fp()->environmentChain();
      auto* envScope = &envChain->as<BlockLexicalEnvironmentObject>().scope();
      MOZ_ASSERT(scope == envScope);
#endif

      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);
      }

      if (!REGS.fp()->freshenLexicalEnvironment(cx)) {
        goto error;
      }
    }
    END_CASE(FreshenLexicalEnv)

    CASE(RecreateLexicalEnv) {
#ifdef DEBUG
      Scope* scope = script->getScope(REGS.pc);
      auto envChain = REGS.fp()->environmentChain();
      auto* envScope = &envChain->as<BlockLexicalEnvironmentObject>().scope();
      MOZ_ASSERT(scope == envScope);
#endif

      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);
      }

      if (!REGS.fp()->recreateLexicalEnvironment(cx)) {
        goto error;
      }
    }
    END_CASE(RecreateLexicalEnv)

    CASE(PushClassBodyEnv) {
      ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

      if (!REGS.fp()->pushClassBodyEnvironment(cx,
                                               scope.as<ClassBodyScope>())) {
        goto error;
      }
    }
    END_CASE(PushClassBodyEnv)

    CASE(PushVarEnv) {
      ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

      if (!REGS.fp()->pushVarEnvironment(cx, scope)) {
        goto error;
      }
    }
    END_CASE(PushVarEnv)

    CASE(Generator) {
      MOZ_ASSERT(!cx->isExceptionPending());
      MOZ_ASSERT(REGS.stackDepth() == 0);
      JSObject* obj = AbstractGeneratorObject::createFromFrame(cx, REGS.fp());
      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(Generator)

    CASE(InitialYield) {
      MOZ_ASSERT(!cx->isExceptionPending());
      MOZ_ASSERT_IF(script->isModule() && script->isAsync(),
                    REGS.fp()->isModuleFrame());
      MOZ_ASSERT_IF(!script->isModule() && script->isAsync(),
                    REGS.fp()->isFunctionFrame());
      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-1].toObject());
      POP_RETURN_VALUE();
      MOZ_ASSERT(REGS.stackDepth() == 0);
      if (!AbstractGeneratorObject::suspend(cx, obj, REGS.fp(), REGS.pc,
                                            script->nfixed())) {
        goto error;
      }
      goto successful_return_continuation;
    }

    CASE(Yield)
    CASE(Await) {
      MOZ_ASSERT(!cx->isExceptionPending());
      MOZ_ASSERT_IF(script->isModule() && script->isAsync(),
                    REGS.fp()->isModuleFrame());
      MOZ_ASSERT_IF(!script->isModule() && script->isAsync(),
                    REGS.fp()->isFunctionFrame());
      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-1].toObject());
      if (!AbstractGeneratorObject::suspend(
              cx, obj, REGS.fp(), REGS.pc,
              script->nfixed() + REGS.stackDepth() - 2)) {
        goto error;
      }

      REGS.sp--;
      POP_RETURN_VALUE();

      goto successful_return_continuation;
    }

    CASE(ResumeKind) {
      GeneratorResumeKind resumeKind = ResumeKindFromPC(REGS.pc);
      PUSH_INT32(int32_t(resumeKind));
    }
    END_CASE(ResumeKind)

    CASE(CheckResumeKind) {
      int32_t kindInt = REGS.sp[-1].toInt32();
      GeneratorResumeKind resumeKind = IntToResumeKind(kindInt);
      if (MOZ_UNLIKELY(resumeKind != GeneratorResumeKind::Next)) {
        ReservedRooted<Value> val(&rootValue0, REGS.sp[-3]);
        Rooted<AbstractGeneratorObject*> gen(
            cx, &REGS.sp[-2].toObject().as<AbstractGeneratorObject>());
        MOZ_ALWAYS_FALSE(GeneratorThrowOrReturn(cx, activation.regs().fp(), gen,
                                                val, resumeKind));
        goto error;
      }
      REGS.sp -= 2;
    }
    END_CASE(CheckResumeKind)

    CASE(Resume) {
      {
        Rooted<AbstractGeneratorObject*> gen(
            cx, &REGS.sp[-3].toObject().as<AbstractGeneratorObject>());
        ReservedRooted<Value> val(&rootValue0, REGS.sp[-2]);
        ReservedRooted<Value> resumeKindVal(&rootValue1, REGS.sp[-1]);

        // popInlineFrame expects there to be an additional value on the stack
        // to pop off, so leave "gen" on the stack.
        REGS.sp -= 1;

        if (!AbstractGeneratorObject::resume(cx, activation, gen, val,
                                             resumeKindVal)) {
          goto error;
        }

        JSScript* generatorScript = REGS.fp()->script();
        if (cx->realm() != generatorScript->realm()) {
          cx->enterRealmOf(generatorScript);
        }
        SET_SCRIPT(generatorScript);

        if (!probes::EnterScript(cx, generatorScript,
                                 generatorScript->function(), REGS.fp())) {
          goto error;
        }

        if (!DebugAPI::onResumeFrame(cx, REGS.fp())) {
          if (cx->isPropagatingForcedReturn()) {
            MOZ_ASSERT_IF(
                REGS.fp()
                    ->callee()
                    .isGenerator(),  // as opposed to an async function
                gen->isClosed());
          }
          goto error;
        }
      }
      ADVANCE_AND_DISPATCH(0);
    }

    CASE(AfterYield) {
      // AbstractGeneratorObject::resume takes care of setting the frame's
      // debuggee flag.
      MOZ_ASSERT_IF(REGS.fp()->script()->isDebuggee(), REGS.fp()->isDebuggee());
      COUNT_COVERAGE();
    }
    END_CASE(AfterYield)

    CASE(FinalYieldRval) {
      ReservedRooted<JSObject*> gen(&rootObject0, &REGS.sp[-1].toObject());
      REGS.sp--;
      AbstractGeneratorObject::finalSuspend(gen);
      goto successful_return_continuation;
    }

    CASE(CheckClassHeritage) {
      HandleValue heritage = REGS.stackHandleAt(-1);

      if (!CheckClassHeritageOperation(cx, heritage)) {
        goto error;
      }
    }
    END_CASE(CheckClassHeritage)

    CASE(BuiltinObject) {
      auto kind = BuiltinObjectKind(GET_UINT8(REGS.pc));
      JSObject* builtin = BuiltinObjectOperation(cx, kind);
      if (!builtin) {
        goto error;
      }
      PUSH_OBJECT(*builtin);
    }
    END_CASE(BuiltinObject)

    CASE(FunWithProto) {
      ReservedRooted<JSObject*> proto(&rootObject1, &REGS.sp[-1].toObject());

      /* Load the specified function object literal. */
      ReservedRooted<JSFunction*> fun(&rootFunction0,
                                      script->getFunction(REGS.pc));

      JSObject* obj =
          FunWithProtoOperation(cx, fun, REGS.fp()->environmentChain(), proto);
      if (!obj) {
        goto error;
      }

      REGS.sp[-1].setObject(*obj);
    }
    END_CASE(FunWithProto)

    CASE(ObjWithProto) {
      JSObject* obj = ObjectWithProtoOperation(cx, REGS.stackHandleAt(-1));
      if (!obj) {
        goto error;
      }

      REGS.sp[-1].setObject(*obj);
    }
    END_CASE(ObjWithProto)

    CASE(InitHomeObject) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);

      /* Load the function to be initialized */
      JSFunction* func = &REGS.sp[-2].toObject().as<JSFunction>();
      MOZ_ASSERT(func->allowSuperProperty());

      /* Load the home object */
      JSObject* obj = &REGS.sp[-1].toObject();
      MOZ_ASSERT(obj->is<PlainObject>() || obj->is<JSFunction>());

      func->setExtendedSlot(FunctionExtended::METHOD_HOMEOBJECT_SLOT,
                            ObjectValue(*obj));
      REGS.sp--;
    }
    END_CASE(InitHomeObject)

    CASE(SuperBase) {
      JSFunction& superEnvFunc = REGS.sp[-1].toObject().as<JSFunction>();
      MOZ_ASSERT(superEnvFunc.allowSuperProperty());
      MOZ_ASSERT(superEnvFunc.baseScript()->needsHomeObject());
      const Value& homeObjVal = superEnvFunc.getExtendedSlot(
          FunctionExtended::METHOD_HOMEOBJECT_SLOT);

      JSObject* homeObj = &homeObjVal.toObject();
      JSObject* superBase = HomeObjectSuperBase(homeObj);

      REGS.sp[-1].setObjectOrNull(superBase);
    }
    END_CASE(SuperBase)

    CASE(NewTarget) {
      PUSH_COPY(REGS.fp()->newTarget());
      MOZ_ASSERT(REGS.sp[-1].isObject() || REGS.sp[-1].isUndefined());
    }
    END_CASE(NewTarget)

    CASE(ImportMeta) {
      JSObject* metaObject = ImportMetaOperation(cx, script);
      if (!metaObject) {
        goto error;
      }

      PUSH_OBJECT(*metaObject);
    }
    END_CASE(ImportMeta)

    CASE(DynamicImport) {
      ReservedRooted<Value> options(&rootValue0, REGS.sp[-1]);
      REGS.sp--;

      ReservedRooted<Value> specifier(&rootValue1);
      POP_COPY_TO(specifier);

      JSObject* promise =
          StartDynamicModuleImport(cx, script, specifier, options);
      if (!promise) goto error;

      PUSH_OBJECT(*promise);
    }
    END_CASE(DynamicImport)

    CASE(EnvCallee) {
      uint8_t numHops = GET_UINT8(REGS.pc);
      JSObject* env = &REGS.fp()->environmentChain()->as<EnvironmentObject>();
      for (unsigned i = 0; i < numHops; i++) {
        env = &env->as<EnvironmentObject>().enclosingEnvironment();
      }
      PUSH_OBJECT(env->as<CallObject>().callee());
    }
    END_CASE(EnvCallee)

    CASE(SuperFun) {
      JSObject* superEnvFunc = &REGS.sp[-1].toObject();
      JSObject* superFun = SuperFunOperation(superEnvFunc);
      REGS.sp[-1].setObjectOrNull(superFun);
    }
    END_CASE(SuperFun)

    CASE(CheckObjCoercible) {
      ReservedRooted<Value> checkVal(&rootValue0, REGS.sp[-1]);
      if (checkVal.isNullOrUndefined()) {
        MOZ_ALWAYS_FALSE(ThrowObjectCoercible(cx, checkVal));
        goto error;
      }
    }
    END_CASE(CheckObjCoercible)

    CASE(DebugCheckSelfHosted) {
#ifdef DEBUG
      ReservedRooted<Value> checkVal(&rootValue0, REGS.sp[-1]);
      if (!Debug_CheckSelfHosted(cx, checkVal)) {
        goto error;
      }
#endif
    }
    END_CASE(DebugCheckSelfHosted)

    CASE(IsConstructing) { PUSH_MAGIC(JS_IS_CONSTRUCTING); }
    END_CASE(IsConstructing)

    CASE(Inc) {
      MutableHandleValue val = REGS.stackHandleAt(-1);
      if (!IncOperation(cx, val, val)) {
        goto error;
      }
    }
    END_CASE(Inc)

    CASE(Dec) {
      MutableHandleValue val = REGS.stackHandleAt(-1);
      if (!DecOperation(cx, val, val)) {
        goto error;
      }
    }
    END_CASE(Dec)

    CASE(ToNumeric) {
      if (!ToNumeric(cx, REGS.stackHandleAt(-1))) {
        goto error;
      }
    }
    END_CASE(ToNumeric)

    CASE(BigInt) { PUSH_BIGINT(script->getBigInt(REGS.pc)); }
    END_CASE(BigInt)

    DEFAULT() {
      char numBuf[12];
      SprintfLiteral(numBuf, "%d", *REGS.pc);
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_BYTECODE, numBuf);
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
       * Push (exception, true) pair for finally to indicate that we
       * should rethrow the exception.
       */
      ReservedRooted<Value> exception(&rootValue0);
      if (!cx->getPendingException(&exception)) {
        interpReturnOK = false;
        goto return_continuation;
      }
      PUSH_COPY(exception);
      PUSH_BOOLEAN(true);
      cx->clearPendingException();
    }
      ADVANCE_AND_DISPATCH(0);
  }

  MOZ_CRASH("Invalid HandleError continuation");

exit:
  if (MOZ_LIKELY(!frameHalfInitialized)) {
    interpReturnOK =
        DebugAPI::onLeaveFrame(cx, REGS.fp(), REGS.pc, interpReturnOK);

    REGS.fp()->epilogue(cx, REGS.pc);
  }

  gc::MaybeVerifyBarriers(cx, true);

  /*
   * This path is used when it's guaranteed the method can be finished
   * inside the JIT.
   */
leave_on_safe_point:

  if (interpReturnOK) {
    state.setReturnValue(activation.entryFrame()->returnValue());
  }

  return interpReturnOK;

prologue_error:
  interpReturnOK = false;
  frameHalfInitialized = true;
  goto prologue_return_continuation;
}

bool js::ThrowOperation(JSContext* cx, HandleValue v) {
  MOZ_ASSERT(!cx->isExceptionPending());
  cx->setPendingException(v, ShouldCaptureStack::Maybe);
  return false;
}

bool js::GetProperty(JSContext* cx, HandleValue v, Handle<PropertyName*> name,
                     MutableHandleValue vp) {
  if (name == cx->names().length) {
    // Fast path for strings, arrays and arguments.
    if (GetLengthProperty(v, vp)) {
      return true;
    }
  }

  // Optimize common cases like (2).toString() or "foo".valueOf() to not
  // create a wrapper object.
  if (v.isPrimitive() && !v.isNullOrUndefined()) {
    JSObject* proto;

    switch (v.type()) {
      case ValueType::Double:
      case ValueType::Int32:
        proto = GlobalObject::getOrCreateNumberPrototype(cx, cx->global());
        break;
      case ValueType::Boolean:
        proto = GlobalObject::getOrCreateBooleanPrototype(cx, cx->global());
        break;
      case ValueType::String:
        proto = GlobalObject::getOrCreateStringPrototype(cx, cx->global());
        break;
      case ValueType::Symbol:
        proto = GlobalObject::getOrCreateSymbolPrototype(cx, cx->global());
        break;
      case ValueType::BigInt:
        proto = GlobalObject::getOrCreateBigIntPrototype(cx, cx->global());
        break;
#ifdef ENABLE_RECORD_TUPLE
      case ValueType::ExtendedPrimitive: {
        RootedObject obj(cx, &v.toExtendedPrimitive());
        RootedId id(cx, NameToId(name));
        return ExtendedPrimitiveGetProperty(cx, obj, v, id, vp);
      }
#endif
      case ValueType::Undefined:
      case ValueType::Null:
      case ValueType::Magic:
      case ValueType::PrivateGCThing:
      case ValueType::Object:
        MOZ_CRASH("unexpected type");
    }

    if (!proto) {
      return false;
    }

    if (GetPropertyPure(cx, proto, NameToId(name), vp.address())) {
      return true;
    }
  }

  RootedValue receiver(cx, v);
  RootedObject obj(
      cx, ToObjectFromStackForPropertyAccess(cx, v, JSDVG_SEARCH_STACK, name));
  if (!obj) {
    return false;
  }

  return GetProperty(cx, obj, receiver, name, vp);
}

JSObject* js::Lambda(JSContext* cx, HandleFunction fun, HandleObject parent) {
  JSFunction* clone;
  if (fun->isNativeFun()) {
    MOZ_ASSERT(IsAsmJSModule(fun));
    clone = CloneAsmJSModuleFunction(cx, fun);
  } else {
    RootedObject proto(cx, fun->staticPrototype());
    clone = CloneFunctionReuseScript(cx, fun, parent, proto);
  }
  if (!clone) {
    return nullptr;
  }

  MOZ_ASSERT(fun->global() == clone->global());
  return clone;
}

JSObject* js::BindVarOperation(JSContext* cx, JSObject* envChain) {
  // Note: BindVarOperation has an unused cx argument because the JIT callVM
  // machinery requires this.
  return &GetVariablesObject(envChain);
}

JSObject* js::ImportMetaOperation(JSContext* cx, HandleScript script) {
  RootedObject module(cx, GetModuleObjectForScript(script));
  MOZ_ASSERT(module);
  return GetOrCreateModuleMetaObject(cx, module);
}

JSObject* js::BuiltinObjectOperation(JSContext* cx, BuiltinObjectKind kind) {
  return GetOrCreateBuiltinObject(cx, kind);
}

bool js::ThrowMsgOperation(JSContext* cx, const unsigned throwMsgKind) {
  auto errorNum = ThrowMsgKindToErrNum(ThrowMsgKind(throwMsgKind));
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, errorNum);
  return false;
}

bool js::GetAndClearExceptionAndStack(JSContext* cx, MutableHandleValue res,
                                      MutableHandle<SavedFrame*> stack) {
  if (!cx->getPendingException(res)) {
    return false;
  }
  stack.set(cx->getPendingExceptionStack());
  cx->clearPendingException();

  // Allow interrupting deeply nested exception handling.
  return CheckForInterrupt(cx);
}

bool js::GetAndClearException(JSContext* cx, MutableHandleValue res) {
  Rooted<SavedFrame*> stack(cx);
  return GetAndClearExceptionAndStack(cx, res, &stack);
}

template <bool strict>
bool js::DelPropOperation(JSContext* cx, HandleValue val,
                          Handle<PropertyName*> name, bool* res) {
  const int valIndex = -1;
  RootedObject obj(cx,
                   ToObjectFromStackForPropertyAccess(cx, val, valIndex, name));
  if (!obj) {
    return false;
  }

  RootedId id(cx, NameToId(name));
  ObjectOpResult result;
  if (!DeleteProperty(cx, obj, id, result)) {
    return false;
  }

  if (strict) {
    if (!result) {
      return result.reportError(cx, obj, id);
    }
    *res = true;
  } else {
    *res = result.ok();
  }
  return true;
}

template bool js::DelPropOperation<true>(JSContext* cx, HandleValue val,
                                         Handle<PropertyName*> name, bool* res);
template bool js::DelPropOperation<false>(JSContext* cx, HandleValue val,
                                          Handle<PropertyName*> name,
                                          bool* res);

template <bool strict>
bool js::DelElemOperation(JSContext* cx, HandleValue val, HandleValue index,
                          bool* res) {
  const int valIndex = -2;
  RootedObject obj(
      cx, ToObjectFromStackForPropertyAccess(cx, val, valIndex, index));
  if (!obj) {
    return false;
  }

  RootedId id(cx);
  if (!ToPropertyKey(cx, index, &id)) {
    return false;
  }
  ObjectOpResult result;
  if (!DeleteProperty(cx, obj, id, result)) {
    return false;
  }

  if (strict) {
    if (!result) {
      return result.reportError(cx, obj, id);
    }
    *res = true;
  } else {
    *res = result.ok();
  }
  return true;
}

template bool js::DelElemOperation<true>(JSContext*, HandleValue, HandleValue,
                                         bool*);
template bool js::DelElemOperation<false>(JSContext*, HandleValue, HandleValue,
                                          bool*);

bool js::SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index,
                          HandleValue value, bool strict) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, index, &id)) {
    return false;
  }
  RootedValue receiver(cx, ObjectValue(*obj));
  return SetObjectElementOperation(cx, obj, id, value, receiver, strict);
}

bool js::SetObjectElementWithReceiver(JSContext* cx, HandleObject obj,
                                      HandleValue index, HandleValue value,
                                      HandleValue receiver, bool strict) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, index, &id)) {
    return false;
  }
  return SetObjectElementOperation(cx, obj, id, value, receiver, strict);
}

bool js::AddValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return AddOperation(cx, lhs, rhs, res);
}

bool js::SubValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return SubOperation(cx, lhs, rhs, res);
}

bool js::MulValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return MulOperation(cx, lhs, rhs, res);
}

bool js::DivValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return DivOperation(cx, lhs, rhs, res);
}

bool js::ModValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return ModOperation(cx, lhs, rhs, res);
}

bool js::PowValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return PowOperation(cx, lhs, rhs, res);
}

bool js::BitNot(JSContext* cx, MutableHandleValue in, MutableHandleValue res) {
  return BitNotOperation(cx, in, res);
}

bool js::BitXor(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
                MutableHandleValue res) {
  return BitXorOperation(cx, lhs, rhs, res);
}

bool js::BitOr(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
               MutableHandleValue res) {
  return BitOrOperation(cx, lhs, rhs, res);
}

bool js::BitAnd(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
                MutableHandleValue res) {
  return BitAndOperation(cx, lhs, rhs, res);
}

bool js::BitLsh(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
                MutableHandleValue res) {
  return BitLshOperation(cx, lhs, rhs, res);
}

bool js::BitRsh(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
                MutableHandleValue res) {
  return BitRshOperation(cx, lhs, rhs, res);
}

bool js::UrshValues(JSContext* cx, MutableHandleValue lhs,
                    MutableHandleValue rhs, MutableHandleValue res) {
  return UrshOperation(cx, lhs, rhs, res);
}

bool js::LessThan(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
                  bool* res) {
  return LessThanOperation(cx, lhs, rhs, res);
}

bool js::LessThanOrEqual(JSContext* cx, MutableHandleValue lhs,
                         MutableHandleValue rhs, bool* res) {
  return LessThanOrEqualOperation(cx, lhs, rhs, res);
}

bool js::GreaterThan(JSContext* cx, MutableHandleValue lhs,
                     MutableHandleValue rhs, bool* res) {
  return GreaterThanOperation(cx, lhs, rhs, res);
}

bool js::GreaterThanOrEqual(JSContext* cx, MutableHandleValue lhs,
                            MutableHandleValue rhs, bool* res) {
  return GreaterThanOrEqualOperation(cx, lhs, rhs, res);
}

bool js::AtomicIsLockFree(JSContext* cx, HandleValue in, int* out) {
  int i;
  if (!ToInt32(cx, in, &i)) {
    return false;
  }
  *out = js::jit::AtomicOperations::isLockfreeJS(i);
  return true;
}

bool js::DeleteNameOperation(JSContext* cx, Handle<PropertyName*> name,
                             HandleObject scopeObj, MutableHandleValue res) {
  RootedObject scope(cx), pobj(cx);
  PropertyResult prop;
  if (!LookupName(cx, name, scopeObj, &scope, &pobj, &prop)) {
    return false;
  }

  if (!scope) {
    // Return true for non-existent names.
    res.setBoolean(true);
    return true;
  }

  ObjectOpResult result;
  RootedId id(cx, NameToId(name));
  if (!DeleteProperty(cx, scope, id, result)) {
    return false;
  }

  bool status = result.ok();
  res.setBoolean(status);

  if (status) {
    // Deleting a name from the global object removes it from [[VarNames]].
    if (pobj == scope && scope->is<GlobalObject>()) {
      scope->as<GlobalObject>().removeFromVarNames(name);
    }
  }

  return true;
}

bool js::ImplicitThisOperation(JSContext* cx, HandleObject scopeObj,
                               Handle<PropertyName*> name,
                               MutableHandleValue res) {
  RootedObject obj(cx);
  if (!LookupNameWithGlobalDefault(cx, name, scopeObj, &obj)) {
    return false;
  }

  res.set(ComputeImplicitThis(obj));
  return true;
}

unsigned js::GetInitDataPropAttrs(JSOp op) {
  switch (op) {
    case JSOp::InitProp:
    case JSOp::InitElem:
      return JSPROP_ENUMERATE;
    case JSOp::InitLockedProp:
    case JSOp::InitLockedElem:
      return JSPROP_PERMANENT | JSPROP_READONLY;
    case JSOp::InitHiddenProp:
    case JSOp::InitHiddenElem:
      // Non-enumerable, but writable and configurable
      return 0;
    default:;
  }
  MOZ_CRASH("Unknown data initprop");
}

static bool InitGetterSetterOperation(JSContext* cx, jsbytecode* pc,
                                      HandleObject obj, HandleId id,
                                      HandleObject val) {
  MOZ_ASSERT(val->isCallable());

  JSOp op = JSOp(*pc);

  unsigned attrs = 0;
  if (!IsHiddenInitOp(op)) {
    attrs |= JSPROP_ENUMERATE;
  }

  if (op == JSOp::InitPropGetter || op == JSOp::InitElemGetter ||
      op == JSOp::InitHiddenPropGetter || op == JSOp::InitHiddenElemGetter) {
    return DefineAccessorProperty(cx, obj, id, val, nullptr, attrs);
  }

  MOZ_ASSERT(op == JSOp::InitPropSetter || op == JSOp::InitElemSetter ||
             op == JSOp::InitHiddenPropSetter ||
             op == JSOp::InitHiddenElemSetter);
  return DefineAccessorProperty(cx, obj, id, nullptr, val, attrs);
}

bool js::InitPropGetterSetterOperation(JSContext* cx, jsbytecode* pc,
                                       HandleObject obj,
                                       Handle<PropertyName*> name,
                                       HandleObject val) {
  RootedId id(cx, NameToId(name));
  return InitGetterSetterOperation(cx, pc, obj, id, val);
}

bool js::InitElemGetterSetterOperation(JSContext* cx, jsbytecode* pc,
                                       HandleObject obj, HandleValue idval,
                                       HandleObject val) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, idval, &id)) {
    return false;
  }

  return InitGetterSetterOperation(cx, pc, obj, id, val);
}

bool js::SpreadCallOperation(JSContext* cx, HandleScript script, jsbytecode* pc,
                             HandleValue thisv, HandleValue callee,
                             HandleValue arr, HandleValue newTarget,
                             MutableHandleValue res) {
  Rooted<ArrayObject*> aobj(cx, &arr.toObject().as<ArrayObject>());
  uint32_t length = aobj->length();
  JSOp op = JSOp(*pc);
  bool constructing = op == JSOp::SpreadNew || op == JSOp::SpreadSuperCall;

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

  if (!callee.toObject().isCallable()) {
    return ReportIsNotFunction(cx, callee, 2 + constructing,
                               constructing ? CONSTRUCT : NO_CONSTRUCT);
  }

  // The object must be an array with dense elements and no holes. Baseline's
  // optimized spread call stubs rely on this.
  MOZ_ASSERT(IsPackedArray(aobj));

  if (constructing) {
    if (!StackCheckIsConstructorCalleeNewTarget(cx, callee, newTarget)) {
      return false;
    }

    ConstructArgs cargs(cx);
    if (!cargs.init(cx, length)) {
      return false;
    }

    if (!GetElements(cx, aobj, length, cargs.array())) {
      return false;
    }

    RootedObject obj(cx);
    if (!Construct(cx, callee, cargs, newTarget, &obj)) {
      return false;
    }
    res.setObject(*obj);
  } else {
    InvokeArgs args(cx);
    if (!args.init(cx, length)) {
      return false;
    }

    if (!GetElements(cx, aobj, length, args.array())) {
      return false;
    }

    if ((op == JSOp::SpreadEval || op == JSOp::StrictSpreadEval) &&
        cx->global()->valueIsEval(callee)) {
      if (!DirectEval(cx, args.get(0), res)) {
        return false;
      }
    } else {
      MOZ_ASSERT(op == JSOp::SpreadCall || op == JSOp::SpreadEval ||
                     op == JSOp::StrictSpreadEval,
                 "bad spread opcode");

      if (!Call(cx, callee, thisv, args, res)) {
        return false;
      }
    }
  }

  return true;
}

static bool OptimizeArraySpreadCall(JSContext* cx, HandleObject obj,
                                    MutableHandleValue result) {
  MOZ_ASSERT(result.isUndefined());

  // Optimize spread call by skipping spread operation when following
  // conditions are met:
  //   * the argument is an array
  //   * the array has no hole
  //   * array[@@iterator] is not modified
  //   * the array's prototype is Array.prototype
  //   * Array.prototype[@@iterator] is not modified
  //   * %ArrayIteratorPrototype%.next is not modified
  if (!IsPackedArray(obj)) {
    return true;
  }

  ForOfPIC::Chain* stubChain = ForOfPIC::getOrCreate(cx);
  if (!stubChain) {
    return false;
  }

  bool optimized;
  if (!stubChain->tryOptimizeArray(cx, obj.as<ArrayObject>(), &optimized)) {
    return false;
  }
  if (!optimized) {
    return true;
  }

  result.setObject(*obj);
  return true;
}

static bool OptimizeArgumentsSpreadCall(JSContext* cx, HandleObject obj,
                                        MutableHandleValue result) {
  MOZ_ASSERT(result.isUndefined());

  // Optimize spread call by skipping the spread operation when the following
  // conditions are met:
  //   * the argument is an arguments object
  //   * the arguments object has no deleted elements
  //   * arguments.length is not overridden
  //   * arguments[@@iterator] is not overridden
  //   * %ArrayIteratorPrototype%.next is not modified

  if (!obj->is<ArgumentsObject>()) {
    return true;
  }

  Handle<ArgumentsObject*> args = obj.as<ArgumentsObject>();
  if (args->hasOverriddenElement() || args->hasOverriddenLength() ||
      args->hasOverriddenIterator()) {
    return true;
  }

  ForOfPIC::Chain* stubChain = ForOfPIC::getOrCreate(cx);
  if (!stubChain) {
    return false;
  }

  bool optimized;
  if (!stubChain->tryOptimizeArrayIteratorNext(cx, &optimized)) {
    return false;
  }
  if (!optimized) {
    return true;
  }

  auto* array = ArrayFromArgumentsObject(cx, args);
  if (!array) {
    return false;
  }

  result.setObject(*array);
  return true;
}

bool js::OptimizeSpreadCall(JSContext* cx, HandleValue arg,
                            MutableHandleValue result) {
  // This function returns |undefined| if the spread operation can't be
  // optimized.
  result.setUndefined();

  if (!arg.isObject()) {
    return true;
  }

  RootedObject obj(cx, &arg.toObject());
  if (!OptimizeArraySpreadCall(cx, obj, result)) {
    return false;
  }
  if (result.isObject()) {
    return true;
  }
  if (!OptimizeArgumentsSpreadCall(cx, obj, result)) {
    return false;
  }
  if (result.isObject()) {
    return true;
  }

  MOZ_ASSERT(result.isUndefined());
  return true;
}

ArrayObject* js::ArrayFromArgumentsObject(JSContext* cx,
                                          Handle<ArgumentsObject*> args) {
  MOZ_ASSERT(!args->hasOverriddenLength());
  MOZ_ASSERT(!args->hasOverriddenElement());

  uint32_t length = args->initialLength();
  auto* array = NewDenseFullyAllocatedArray(cx, length);
  if (!array) {
    return nullptr;
  }
  array->setDenseInitializedLength(length);

  for (uint32_t index = 0; index < length; index++) {
    const Value& v = args->element(index);
    array->initDenseElement(index, v);
  }

  return array;
}

JSObject* js::NewObjectOperation(JSContext* cx, HandleScript script,
                                 const jsbytecode* pc) {
  if (JSOp(*pc) == JSOp::NewObject) {
    Rooted<SharedShape*> shape(cx, script->getShape(pc));
    return PlainObject::createWithShape(cx, shape);
  }

  MOZ_ASSERT(JSOp(*pc) == JSOp::NewInit);
  return NewPlainObject(cx);
}

JSObject* js::NewPlainObjectBaselineFallback(JSContext* cx,
                                             Handle<SharedShape*> shape,
                                             gc::AllocKind allocKind,
                                             gc::AllocSite* site) {
  MOZ_ASSERT(shape->getObjectClass() == &PlainObject::class_);

  mozilla::Maybe<AutoRealm> ar;
  if (cx->realm() != shape->realm()) {
    MOZ_ASSERT(cx->compartment() == shape->compartment());
    ar.emplace(cx, shape);
  }

  gc::Heap initialHeap = site->initialHeap();
  return NativeObject::create(cx, allocKind, initialHeap, shape, site);
}

JSObject* js::NewPlainObjectOptimizedFallback(JSContext* cx,
                                              Handle<SharedShape*> shape,
                                              gc::AllocKind allocKind,
                                              gc::Heap initialHeap) {
  MOZ_ASSERT(shape->getObjectClass() == &PlainObject::class_);

  mozilla::Maybe<AutoRealm> ar;
  if (cx->realm() != shape->realm()) {
    MOZ_ASSERT(cx->compartment() == shape->compartment());
    ar.emplace(cx, shape);
  }

  gc::AllocSite* site = cx->zone()->optimizedAllocSite();
  return NativeObject::create(cx, allocKind, initialHeap, shape, site);
}

ArrayObject* js::NewArrayOperation(
    JSContext* cx, uint32_t length,
    NewObjectKind newKind /* = GenericObject */) {
  return NewDenseFullyAllocatedArray(cx, length, newKind);
}

ArrayObject* js::NewArrayObjectBaselineFallback(JSContext* cx, uint32_t length,
                                                gc::AllocKind allocKind,
                                                gc::AllocSite* site) {
  NewObjectKind newKind =
      site->initialHeap() == gc::Heap::Tenured ? TenuredObject : GenericObject;
  ArrayObject* array = NewDenseFullyAllocatedArray(cx, length, newKind, site);
  // It's important that we allocate an object with the alloc kind we were
  // expecting so that a new arena gets allocated if the current arena for that
  // kind is full.
  MOZ_ASSERT_IF(array && array->isTenured(),
                array->asTenured().getAllocKind() == allocKind);
  return array;
}

ArrayObject* js::NewArrayObjectOptimizedFallback(JSContext* cx, uint32_t length,
                                                 gc::AllocKind allocKind,
                                                 NewObjectKind newKind) {
  gc::AllocSite* site = cx->zone()->optimizedAllocSite();
  ArrayObject* array = NewDenseFullyAllocatedArray(cx, length, newKind, site);
  // It's important that we allocate an object with the alloc kind we were
  // expecting so that a new arena gets allocated if the current arena for that
  // kind is full.
  MOZ_ASSERT_IF(array && array->isTenured(),
                array->asTenured().getAllocKind() == allocKind);
  return array;
}

void js::ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                                   HandleId id) {
  MOZ_ASSERT(errorNumber == JSMSG_UNINITIALIZED_LEXICAL ||
             errorNumber == JSMSG_BAD_CONST_ASSIGN);
  if (UniqueChars printable =
          IdToPrintableUTF8(cx, id, IdToPrintableBehavior::IdIsIdentifier)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber,
                             printable.get());
  }
}

void js::ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                                   Handle<PropertyName*> name) {
  RootedId id(cx, NameToId(name));
  ReportRuntimeLexicalError(cx, errorNumber, id);
}

void js::ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                                   HandleScript script, jsbytecode* pc) {
  JSOp op = JSOp(*pc);
  MOZ_ASSERT(op == JSOp::CheckLexical || op == JSOp::CheckAliasedLexical ||
             op == JSOp::ThrowSetConst || op == JSOp::GetImport);

  Rooted<PropertyName*> name(cx);
  if (IsLocalOp(op)) {
    name = FrameSlotName(script, pc)->asPropertyName();
  } else if (IsAliasedVarOp(op)) {
    name = EnvironmentCoordinateNameSlow(script, pc);
  } else {
    MOZ_ASSERT(IsAtomOp(op));
    name = script->getName(pc);
  }

  ReportRuntimeLexicalError(cx, errorNumber, name);
}

void js::ReportRuntimeRedeclaration(JSContext* cx, Handle<PropertyName*> name,
                                    const char* redeclKind) {
  if (UniqueChars printable = AtomToPrintableString(cx, name)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_REDECLARED_VAR, redeclKind,
                              printable.get());
  }
}

bool js::ThrowCheckIsObject(JSContext* cx, CheckIsObjectKind kind) {
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
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_GET_ITER_RETURNED_PRIMITIVE);
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

bool js::ThrowUninitializedThis(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_UNINITIALIZED_THIS);
  return false;
}

bool js::ThrowInitializedThis(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_REINIT_THIS);
  return false;
}

bool js::ThrowObjectCoercible(JSContext* cx, HandleValue value) {
  MOZ_ASSERT(value.isNullOrUndefined());
  ReportIsNullOrUndefinedForPropertyAccess(cx, value, JSDVG_SEARCH_STACK);
  return false;
}

bool js::SetPropertySuper(JSContext* cx, HandleValue lval, HandleValue receiver,
                          Handle<PropertyName*> name, HandleValue rval,
                          bool strict) {
  MOZ_ASSERT(lval.isObjectOrNull());

  RootedObject obj(cx, ToObjectFromStackForPropertyAccess(
                           cx, lval, JSDVG_SEARCH_STACK, name));
  if (!obj) {
    return false;
  }

  RootedId id(cx, NameToId(name));
  return SetObjectElementOperation(cx, obj, id, rval, receiver, strict);
}

bool js::SetElementSuper(JSContext* cx, HandleValue lval, HandleValue receiver,
                         HandleValue index, HandleValue rval, bool strict) {
  MOZ_ASSERT(lval.isObjectOrNull());

  RootedObject obj(cx, ToObjectFromStackForPropertyAccess(
                           cx, lval, JSDVG_SEARCH_STACK, index));
  if (!obj) {
    return false;
  }

  return SetObjectElementWithReceiver(cx, obj, index, rval, receiver, strict);
}

bool js::LoadAliasedDebugVar(JSContext* cx, JSObject* env, jsbytecode* pc,
                             MutableHandleValue result) {
  EnvironmentCoordinate ec(pc);

  for (unsigned i = ec.hops(); i; i--) {
    if (env->is<EnvironmentObject>()) {
      env = &env->as<EnvironmentObject>().enclosingEnvironment();
    } else {
      MOZ_ASSERT(env->is<DebugEnvironmentProxy>());
      env = &env->as<DebugEnvironmentProxy>().enclosingEnvironment();
    }
  }

  EnvironmentObject& finalEnv =
      env->is<EnvironmentObject>()
          ? env->as<EnvironmentObject>()
          : env->as<DebugEnvironmentProxy>().environment();

  result.set(finalEnv.aliasedBinding(ec));
  return true;
}

// https://tc39.es/ecma262/#sec-iteratorclose
bool js::CloseIterOperation(JSContext* cx, HandleObject iter,
                            CompletionKind kind) {
  // Steps 1-2 are implicit.

  // Step 3
  RootedValue returnMethod(cx);
  bool innerResult =
      GetProperty(cx, iter, iter, cx->names().return_, &returnMethod);

  // Step 4
  RootedValue result(cx);
  if (innerResult) {
    // Step 4b
    if (returnMethod.isNullOrUndefined()) {
      return true;
    }
    // Step 4c
    if (IsCallable(returnMethod)) {
      RootedValue thisVal(cx, ObjectValue(*iter));
      innerResult = Call(cx, returnMethod, thisVal, &result);
    } else {
      innerResult = ReportIsNotFunction(cx, returnMethod);
    }
  }

  // Step 5
  if (kind == CompletionKind::Throw) {
    // If we close an iterator while unwinding for an exception,
    // the initial exception takes priority over any exception thrown
    // while closing the iterator.
    if (cx->isExceptionPending()) {
      cx->clearPendingException();
    }
    return true;
  }

  // Step 6
  if (!innerResult) {
    return false;
  }

  // Step 7
  if (!result.isObject()) {
    return ThrowCheckIsObject(cx, CheckIsObjectKind::IteratorReturn);
  }

  // Step 8
  return true;
}
