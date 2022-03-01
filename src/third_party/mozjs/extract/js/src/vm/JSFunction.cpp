/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS function support.
 */

#include "vm/JSFunction-inl.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"
#include "mozilla/Utf8.h"

#include <algorithm>
#include <iterator>
#include <string.h>

#include "jsapi.h"
#include "jstypes.h"

#include "builtin/Array.h"
#include "builtin/BigInt.h"
#include "builtin/Eval.h"
#include "builtin/Object.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/Symbol.h"
#include "frontend/BytecodeCompilation.h"
#include "frontend/BytecodeCompiler.h"
#include "frontend/TokenStream.h"
#include "gc/Marking.h"
#include "gc/Policy.h"
#include "jit/InlinableNatives.h"
#include "jit/Ion.h"
#include "js/CallNonGenericMethod.h"
#include "js/CompilationAndEvaluation.h"
#include "js/CompileOptions.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/PropertySpec.h"
#include "js/Proxy.h"
#include "js/SourceText.h"
#include "js/StableStringChars.h"
#include "js/Wrapper.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BooleanObject.h"
#include "vm/FunctionFlags.h"          // js::FunctionFlags
#include "vm/GeneratorAndAsyncKind.h"  // js::GeneratorKind, js::FunctionAsyncKind
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/NumberObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/SharedImmutableStringsCache.h"
#include "vm/StringObject.h"
#include "vm/WellKnownAtom.h"  // js_*_str
#include "vm/WrapperObject.h"
#include "vm/Xdr.h"
#include "wasm/AsmJS.h"

#include "debugger/DebugAPI-inl.h"
#include "vm/FrameIter-inl.h"  // js::FrameIter::unaliasedForEachActual
#include "vm/Interpreter-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

using mozilla::CheckedInt;
using mozilla::Maybe;
using mozilla::Some;
using mozilla::Utf8Unit;

using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::SourceOwnership;
using JS::SourceText;

static bool fun_enumerate(JSContext* cx, HandleObject obj) {
  MOZ_ASSERT(obj->is<JSFunction>());

  RootedId id(cx);
  bool found;

  if (!obj->isBoundFunction() && !obj->as<JSFunction>().isArrow()) {
    id = NameToId(cx->names().prototype);
    if (!HasOwnProperty(cx, obj, id, &found)) {
      return false;
    }
  }

  if (!obj->as<JSFunction>().hasResolvedLength()) {
    id = NameToId(cx->names().length);
    if (!HasOwnProperty(cx, obj, id, &found)) {
      return false;
    }
  }

  if (!obj->as<JSFunction>().hasResolvedName()) {
    id = NameToId(cx->names().name);
    if (!HasOwnProperty(cx, obj, id, &found)) {
      return false;
    }
  }

  return true;
}

bool IsFunction(HandleValue v) {
  return v.isObject() && v.toObject().is<JSFunction>();
}

static bool AdvanceToActiveCallLinear(JSContext* cx,
                                      NonBuiltinScriptFrameIter& iter,
                                      HandleFunction fun) {
  MOZ_ASSERT(!fun->isBuiltin());

  for (; !iter.done(); ++iter) {
    if (!iter.isFunctionFrame()) {
      continue;
    }
    if (iter.matchCallee(cx, fun)) {
      return true;
    }
  }
  return false;
}

void js::ThrowTypeErrorBehavior(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_THROW_TYPE_ERROR);
}

static bool IsSloppyNormalFunction(JSFunction* fun) {
  // FunctionDeclaration or FunctionExpression in sloppy mode.
  if (fun->kind() == FunctionFlags::NormalFunction) {
    if (fun->isBuiltin() || fun->isBoundFunction()) {
      return false;
    }

    if (fun->isGenerator() || fun->isAsync()) {
      return false;
    }

    MOZ_ASSERT(fun->isInterpreted());
    return !fun->strict();
  }

  // Or asm.js function in sloppy mode.
  if (fun->kind() == FunctionFlags::AsmJS) {
    return !IsAsmJSStrictModeModuleOrFunction(fun);
  }

  return false;
}

// Beware: this function can be invoked on *any* function! That includes
// natives, strict mode functions, bound functions, arrow functions,
// self-hosted functions and constructors, asm.js functions, functions with
// destructuring arguments and/or a rest argument, and probably a few more I
// forgot. Turn back and save yourself while you still can. It's too late for
// me.
static bool ArgumentsRestrictions(JSContext* cx, HandleFunction fun) {
  // Throw unless the function is a sloppy, normal function.
  // TODO (bug 1057208): ensure semantics are correct for all possible
  // pairings of callee/caller.
  if (!IsSloppyNormalFunction(fun)) {
    ThrowTypeErrorBehavior(cx);
    return false;
  }

  return true;
}

bool ArgumentsGetterImpl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsFunction(args.thisv()));

  RootedFunction fun(cx, &args.thisv().toObject().as<JSFunction>());
  if (!ArgumentsRestrictions(cx, fun)) {
    return false;
  }

  // Return null if this function wasn't found on the stack.
  NonBuiltinScriptFrameIter iter(cx);
  if (!AdvanceToActiveCallLinear(cx, iter, fun)) {
    args.rval().setNull();
    return true;
  }

  Rooted<ArgumentsObject*> argsobj(cx,
                                   ArgumentsObject::createUnexpected(cx, iter));
  if (!argsobj) {
    return false;
  }

#ifndef JS_CODEGEN_NONE
  // Disabling compiling of this script in IonMonkey.  IonMonkey doesn't
  // guarantee |f.arguments| can be fully recovered, so we try to mitigate
  // observing this behavior by detecting its use early.
  JSScript* script = iter.script();
  jit::ForbidCompilation(cx, script);
#endif

  args.rval().setObject(*argsobj);
  return true;
}

static bool ArgumentsGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsFunction, ArgumentsGetterImpl>(cx, args);
}

bool ArgumentsSetterImpl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsFunction(args.thisv()));

  RootedFunction fun(cx, &args.thisv().toObject().as<JSFunction>());
  if (!ArgumentsRestrictions(cx, fun)) {
    return false;
  }

  // If the function passes the gauntlet, return |undefined|.
  args.rval().setUndefined();
  return true;
}

static bool ArgumentsSetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsFunction, ArgumentsSetterImpl>(cx, args);
}

// Beware: this function can be invoked on *any* function! That includes
// natives, strict mode functions, bound functions, arrow functions,
// self-hosted functions and constructors, asm.js functions, functions with
// destructuring arguments and/or a rest argument, and probably a few more I
// forgot. Turn back and save yourself while you still can. It's too late for
// me.
static bool CallerRestrictions(JSContext* cx, HandleFunction fun) {
  // Throw unless the function is a sloppy, normal function.
  // TODO (bug 1057208): ensure semantics are correct for all possible
  // pairings of callee/caller.
  if (!IsSloppyNormalFunction(fun)) {
    ThrowTypeErrorBehavior(cx);
    return false;
  }

  return true;
}

bool CallerGetterImpl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsFunction(args.thisv()));

  // Beware!  This function can be invoked on *any* function!  It can't
  // assume it'll never be invoked on natives, strict mode functions, bound
  // functions, or anything else that ordinarily has immutable .caller
  // defined with [[ThrowTypeError]].
  RootedFunction fun(cx, &args.thisv().toObject().as<JSFunction>());
  if (!CallerRestrictions(cx, fun)) {
    return false;
  }

  // Also return null if this function wasn't found on the stack.
  NonBuiltinScriptFrameIter iter(cx);
  if (!AdvanceToActiveCallLinear(cx, iter, fun)) {
    args.rval().setNull();
    return true;
  }

  ++iter;
  while (!iter.done() && iter.isEvalFrame()) {
    ++iter;
  }

  if (iter.done() || !iter.isFunctionFrame()) {
    args.rval().setNull();
    return true;
  }

  RootedObject caller(cx, iter.callee(cx));
  if (!cx->compartment()->wrap(cx, &caller)) {
    return false;
  }

  // Censor the caller if we don't have full access to it.  If we do, but the
  // caller is a function with strict mode code, throw a TypeError per ES5.
  // If we pass these checks, we can return the computed caller.
  {
    JSObject* callerObj = CheckedUnwrapStatic(caller);
    if (!callerObj) {
      args.rval().setNull();
      return true;
    }

    if (JS_IsDeadWrapper(callerObj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return false;
    }

    JSFunction* callerFun = &callerObj->as<JSFunction>();
    MOZ_ASSERT(!callerFun->isBuiltin(),
               "non-builtin iterator returned a builtin?");

    if (callerFun->strict() || callerFun->isAsync() ||
        callerFun->isGenerator()) {
      args.rval().setNull();
      return true;
    }
  }

  args.rval().setObject(*caller);
  return true;
}

static bool CallerGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsFunction, CallerGetterImpl>(cx, args);
}

bool CallerSetterImpl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsFunction(args.thisv()));

  // We just have to return |undefined|, but first we call CallerGetterImpl
  // because we need the same strict-mode and security checks.

  if (!CallerGetterImpl(cx, args)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool CallerSetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsFunction, CallerSetterImpl>(cx, args);
}

static const JSPropertySpec function_properties[] = {
    JS_PSGS("arguments", ArgumentsGetter, ArgumentsSetter, 0),
    JS_PSGS("caller", CallerGetter, CallerSetter, 0), JS_PS_END};

static bool ResolveInterpretedFunctionPrototype(JSContext* cx,
                                                HandleFunction fun,
                                                HandleId id) {
  MOZ_ASSERT(fun->isInterpreted() || fun->isAsmJSNative());
  MOZ_ASSERT(id == NameToId(cx->names().prototype));

  // Assert that fun is not a compiler-created function object, which
  // must never leak to script or embedding code and then be mutated.
  // Also assert that fun is not bound, per the ES5 15.3.4.5 ref above.
  MOZ_ASSERT(!IsInternalFunctionObject(*fun));
  MOZ_ASSERT(!fun->isBoundFunction());

  // Make the prototype object an instance of Object with the same parent as
  // the function object itself, unless the function is an ES6 generator.  In
  // that case, per the 15 July 2013 ES6 draft, section 15.19.3, its parent is
  // the GeneratorObjectPrototype singleton.
  bool isGenerator = fun->isGenerator();
  Rooted<GlobalObject*> global(cx, &fun->global());
  RootedObject objProto(cx);
  if (isGenerator && fun->isAsync()) {
    objProto = GlobalObject::getOrCreateAsyncGeneratorPrototype(cx, global);
  } else if (isGenerator) {
    objProto = GlobalObject::getOrCreateGeneratorObjectPrototype(cx, global);
  } else {
    objProto = GlobalObject::getOrCreateObjectPrototype(cx, global);
  }
  if (!objProto) {
    return false;
  }

  RootedPlainObject proto(
      cx, NewTenuredObjectWithGivenProto<PlainObject>(cx, objProto));
  if (!proto) {
    return false;
  }

  // Per ES5 13.2 the prototype's .constructor property is configurable,
  // non-enumerable, and writable.  However, per the 15 July 2013 ES6 draft,
  // section 15.19.3, the .prototype of a generator function does not link
  // back with a .constructor.
  if (!isGenerator) {
    RootedValue objVal(cx, ObjectValue(*fun));
    if (!DefineDataProperty(cx, proto, cx->names().constructor, objVal, 0)) {
      return false;
    }
  }

  // Per ES5 15.3.5.2 a user-defined function's .prototype property is
  // initially non-configurable, non-enumerable, and writable.
  RootedValue protoVal(cx, ObjectValue(*proto));
  return DefineDataProperty(cx, fun, id, protoVal,
                            JSPROP_PERMANENT | JSPROP_RESOLVING);
}

bool JSFunction::needsPrototypeProperty() {
  /*
   * Built-in functions do not have a .prototype property per ECMA-262,
   * or (Object.prototype, Function.prototype, etc.) have that property
   * created eagerly.
   *
   * ES5 15.3.4.5: bound functions don't have a prototype property. The
   * isBuiltin() test covers this case because bound functions are self-hosted
   * (scripted) built-ins.
   *
   * ES6 9.2.8 MakeConstructor defines the .prototype property on constructors.
   * Generators are not constructors, but they have a .prototype property
   * anyway, according to errata to ES6. See bug 1191486.
   *
   * Thus all of the following don't get a .prototype property:
   * - Methods (that are not class-constructors or generators)
   * - Arrow functions
   * - Function.prototype
   * - Async functions
   */
  return !isBuiltin() && (isConstructor() || isGenerator());
}

bool JSFunction::hasNonConfigurablePrototypeDataProperty() {
  if (!isBuiltin()) {
    return needsPrototypeProperty();
  }

  if (isSelfHostedBuiltin()) {
    // Self-hosted constructors other than bound functions have a
    // non-configurable .prototype data property.
    if (!isConstructor() || isBoundFunction()) {
      return false;
    }
#ifdef DEBUG
    PropertyName* prototypeName =
        runtimeFromMainThread()->commonNames->prototype;
    Maybe<PropertyInfo> prop = lookupPure(prototypeName);
    MOZ_ASSERT(prop.isSome());
    MOZ_ASSERT(prop->isDataProperty());
    MOZ_ASSERT(!prop->configurable());
#endif
    return true;
  }

  if (!isConstructor()) {
    // We probably don't have a .prototype property. Avoid the lookup below.
    return false;
  }

  PropertyName* prototypeName = runtimeFromMainThread()->commonNames->prototype;
  Maybe<PropertyInfo> prop = lookupPure(prototypeName);
  return prop.isSome() && prop->isDataProperty() && !prop->configurable();
}

static bool fun_mayResolve(const JSAtomState& names, jsid id, JSObject*) {
  if (!id.isAtom()) {
    return false;
  }

  JSAtom* atom = id.toAtom();
  return atom == names.prototype || atom == names.length || atom == names.name;
}

static bool fun_resolve(JSContext* cx, HandleObject obj, HandleId id,
                        bool* resolvedp) {
  if (!id.isAtom()) {
    return true;
  }

  RootedFunction fun(cx, &obj->as<JSFunction>());

  if (id.isAtom(cx->names().prototype)) {
    if (!fun->needsPrototypeProperty()) {
      return true;
    }

    if (!ResolveInterpretedFunctionPrototype(cx, fun, id)) {
      return false;
    }

    *resolvedp = true;
    return true;
  }

  bool isLength = id.isAtom(cx->names().length);
  if (isLength || id.isAtom(cx->names().name)) {
    MOZ_ASSERT(!IsInternalFunctionObject(*obj));

    RootedValue v(cx);

    // Since f.length and f.name are configurable, they could be resolved
    // and then deleted:
    //     function f(x) {}
    //     assertEq(f.length, 1);
    //     delete f.length;
    //     assertEq(f.name, "f");
    //     delete f.name;
    // Afterwards, asking for f.length or f.name again will cause this
    // resolve hook to run again. Defining the property again the second
    // time through would be a bug.
    //     assertEq(f.length, 0);  // gets Function.prototype.length!
    //     assertEq(f.name, "");  // gets Function.prototype.name!
    // We use the RESOLVED_LENGTH and RESOLVED_NAME flags as a hack to prevent
    // this bug.
    if (isLength) {
      if (fun->hasResolvedLength()) {
        return true;
      }

      if (!JSFunction::getUnresolvedLength(cx, fun, &v)) {
        return false;
      }
    } else {
      if (fun->hasResolvedName()) {
        return true;
      }

      if (!JSFunction::getUnresolvedName(cx, fun, &v)) {
        return false;
      }
    }

    if (!NativeDefineDataProperty(cx, fun, id, v,
                                  JSPROP_READONLY | JSPROP_RESOLVING)) {
      return false;
    }

    if (isLength) {
      fun->setResolvedLength();
    } else {
      fun->setResolvedName();
    }

    *resolvedp = true;
    return true;
  }

  return true;
}

template <XDRMode mode>
XDRResult js::XDRInterpretedFunction(XDRState<mode>* xdr,
                                     HandleScope enclosingScope,
                                     HandleScriptSourceObject sourceObject,
                                     MutableHandleFunction objp) {
  enum FirstWordFlag {
    HasAtom = 1 << 0,
    IsGenerator = 1 << 1,
    IsAsync = 1 << 2,
    IsLazy = 1 << 3,
  };

  /* NB: Keep this in sync with CloneInnerInterpretedFunction. */

  JSContext* cx = xdr->cx();

  uint8_t xdrFlags = 0; /* bitmask of FirstWordFlag */

  uint16_t nargs = 0;
  uint16_t flags = 0;

  RootedFunction fun(cx);
  RootedAtom atom(cx);
  RootedScript script(cx);
  Rooted<BaseScript*> lazy(cx);

  if (mode == XDR_ENCODE) {
    fun = objp;
    if (!fun->isInterpreted() || fun->isBoundFunction()) {
      return xdr->fail(JS::TranscodeResult::Failure_NotInterpretedFun);
    }

    if (fun->isGenerator()) {
      xdrFlags |= IsGenerator;
    }
    if (fun->isAsync()) {
      xdrFlags |= IsAsync;
    }

    if (fun->hasBytecode()) {
      // Encode the script.
      script = fun->nonLazyScript();
    } else {
      // Encode a lazy script.
      xdrFlags |= IsLazy;
      lazy = fun->baseScript();
    }

    if (fun->displayAtom()) {
      xdrFlags |= HasAtom;
    }

    nargs = fun->nargs();
    flags = FunctionFlags::clearMutableflags(fun->flags()).toRaw();

    atom = fun->displayAtom();
  }

  MOZ_TRY(xdr->codeUint8(&xdrFlags));

  MOZ_TRY(xdr->codeUint16(&nargs));
  MOZ_TRY(xdr->codeUint16(&flags));

  if (xdrFlags & HasAtom) {
    MOZ_TRY(XDRAtom(xdr, &atom));
  }

  if (mode == XDR_DECODE) {
    GeneratorKind generatorKind = (xdrFlags & IsGenerator)
                                      ? GeneratorKind::Generator
                                      : GeneratorKind::NotGenerator;
    FunctionAsyncKind asyncKind = (xdrFlags & IsAsync)
                                      ? FunctionAsyncKind::AsyncFunction
                                      : FunctionAsyncKind::SyncFunction;

    RootedObject proto(cx);
    if (!GetFunctionPrototype(cx, generatorKind, asyncKind, &proto)) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }

    gc::AllocKind allocKind = gc::AllocKind::FUNCTION;
    if (flags & FunctionFlags::EXTENDED) {
      allocKind = gc::AllocKind::FUNCTION_EXTENDED;
    }

    // Sanity check the flags. We should have cleared the mutable flags already
    // and we do not support self-hosted-lazy, bound or wasm functions.
    constexpr uint16_t UnsupportedFlags =
        FunctionFlags::MUTABLE_FLAGS | FunctionFlags::SELFHOSTLAZY |
        FunctionFlags::BOUND_FUN | FunctionFlags::WASM_JIT_ENTRY;
    if ((flags & UnsupportedFlags) != 0) {
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
    }

    fun = NewFunctionWithProto(cx, nullptr, nargs, FunctionFlags(flags),
                               nullptr, atom, proto, allocKind, TenuredObject);
    if (!fun) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    objp.set(fun);
  }

  if (xdrFlags & IsLazy) {
    MOZ_TRY(XDRLazyScript(xdr, enclosingScope, sourceObject, fun, &lazy));
  } else {
    MOZ_TRY(XDRScript(xdr, enclosingScope, sourceObject, fun, &script));
  }

  // Verify marker at end of function to detect buffer trunction.
  MOZ_TRY(xdr->codeMarker(0x9E35CA1F));

  return Ok();
}

template XDRResult js::XDRInterpretedFunction(XDRState<XDR_ENCODE>*,
                                              HandleScope,
                                              HandleScriptSourceObject,
                                              MutableHandleFunction);

template XDRResult js::XDRInterpretedFunction(XDRState<XDR_DECODE>*,
                                              HandleScope,
                                              HandleScriptSourceObject,
                                              MutableHandleFunction);

/* ES6 (04-25-16) 19.2.3.6 Function.prototype [ @@hasInstance ] */
static bool fun_symbolHasInstance(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() < 1) {
    args.rval().setBoolean(false);
    return true;
  }

  /* Step 1. */
  HandleValue func = args.thisv();

  // Primitives are non-callable and will always return false from
  // OrdinaryHasInstance.
  if (!func.isObject()) {
    args.rval().setBoolean(false);
    return true;
  }

  RootedObject obj(cx, &func.toObject());

  /* Step 2. */
  bool result;
  if (!OrdinaryHasInstance(cx, obj, args[0], &result)) {
    return false;
  }

  args.rval().setBoolean(result);
  return true;
}

/*
 * ES6 (4-25-16) 7.3.19 OrdinaryHasInstance
 */
bool JS::OrdinaryHasInstance(JSContext* cx, HandleObject objArg, HandleValue v,
                             bool* bp) {
  AssertHeapIsIdle();
  cx->check(objArg, v);

  RootedObject obj(cx, objArg);

  /* Step 1. */
  if (!obj->isCallable()) {
    *bp = false;
    return true;
  }

  /* Step 2. */
  if (obj->is<JSFunction>() && obj->isBoundFunction()) {
    /* Steps 2a-b. */
    AutoCheckRecursionLimit recursion(cx);
    if (!recursion.check(cx)) {
      return false;
    }
    obj = obj->as<JSFunction>().getBoundFunctionTarget();
    return InstanceofOperator(cx, obj, v, bp);
  }

  /* Step 3. */
  if (!v.isObject()) {
    *bp = false;
    return true;
  }

  /* Step 4. */
  RootedValue pval(cx);
  if (!GetProperty(cx, obj, obj, cx->names().prototype, &pval)) {
    return false;
  }

  /* Step 5. */
  if (pval.isPrimitive()) {
    /*
     * Throw a runtime error if instanceof is called on a function that
     * has a non-object as its .prototype value.
     */
    RootedValue val(cx, ObjectValue(*obj));
    ReportValueError(cx, JSMSG_BAD_PROTOTYPE, -1, val, nullptr);
    return false;
  }

  /* Step 6. */
  RootedObject pobj(cx, &pval.toObject());
  bool isPrototype;
  if (!IsPrototypeOf(cx, pobj, &v.toObject(), &isPrototype)) {
    return false;
  }
  *bp = isPrototype;
  return true;
}

inline void JSFunction::trace(JSTracer* trc) {
  if (isExtended()) {
    TraceRange(trc, std::size(toExtended()->extendedSlots),
               (GCPtrValue*)toExtended()->extendedSlots, "nativeReserved");
  }

  TraceNullableEdge(trc, &atom_, "atom");

  if (isInterpreted()) {
    // Functions can be be marked as interpreted despite having no script
    // yet at some points when parsing, and can be lazy with no lazy script
    // for self-hosted code.
    if (isIncomplete()) {
      MOZ_ASSERT(u.scripted.s.script_ == nullptr);
    } else if (hasBaseScript()) {
      BaseScript* script = u.scripted.s.script_;
      TraceManuallyBarrieredEdge(trc, &script, "script");
      // Self-hosted scripts are shared with workers but are never
      // relocated. Skip unnecessary writes to prevent the possible data race.
      if (u.scripted.s.script_ != script) {
        u.scripted.s.script_ = script;
      }
    }
    // NOTE: The u.scripted.s.selfHostedLazy_ does not point to GC things.

    if (u.scripted.env_) {
      TraceManuallyBarrieredEdge(trc, &u.scripted.env_, "fun_environment");
    }
  }
}

static void fun_trace(JSTracer* trc, JSObject* obj) {
  obj->as<JSFunction>().trace(trc);
}

static JSObject* CreateFunctionConstructor(JSContext* cx, JSProtoKey key) {
  Rooted<GlobalObject*> global(cx, cx->global());
  RootedObject functionProto(
      cx, &global->getPrototype(JSProto_Function).toObject());

  RootedObject functionCtor(
      cx, NewFunctionWithProto(
              cx, Function, 1, FunctionFlags::NATIVE_CTOR, nullptr,
              HandlePropertyName(cx->names().Function), functionProto,
              gc::AllocKind::FUNCTION, TenuredObject));
  if (!functionCtor) {
    return nullptr;
  }

  return functionCtor;
}

static bool FunctionPrototype(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setUndefined();
  return true;
}

static JSObject* CreateFunctionPrototype(JSContext* cx, JSProtoKey key) {
  Rooted<GlobalObject*> self(cx, cx->global());

  RootedObject objectProto(cx, &self->getPrototype(JSProto_Object).toObject());

  return NewFunctionWithProto(
      cx, FunctionPrototype, 0, FunctionFlags::NATIVE_FUN, nullptr,
      HandlePropertyName(cx->names().empty), objectProto,
      gc::AllocKind::FUNCTION, TenuredObject);
}

JSString* js::FunctionToStringCache::lookup(BaseScript* script) const {
  for (size_t i = 0; i < NumEntries; i++) {
    if (entries_[i].script == script) {
      return entries_[i].string;
    }
  }
  return nullptr;
}

void js::FunctionToStringCache::put(BaseScript* script, JSString* string) {
  for (size_t i = NumEntries - 1; i > 0; i--) {
    entries_[i] = entries_[i - 1];
  }

  entries_[0].set(script, string);
}

JSString* js::FunctionToString(JSContext* cx, HandleFunction fun,
                               bool isToSource) {
  if (IsAsmJSModule(fun)) {
    return AsmJSModuleToString(cx, fun, isToSource);
  }
  if (IsAsmJSFunction(fun)) {
    return AsmJSFunctionToString(cx, fun);
  }

  // Self-hosted built-ins should not expose their source code.
  bool haveSource = fun->isInterpreted() && !fun->isSelfHostedBuiltin();

  // If we're in toSource mode, put parentheses around lambda functions so
  // that eval returns lambda, not function statement.
  bool addParentheses =
      haveSource && isToSource && (fun->isLambda() && !fun->isArrow());

  if (haveSource) {
    if (!ScriptSource::loadSource(cx, fun->baseScript()->scriptSource(),
                                  &haveSource)) {
      return nullptr;
    }
  }

  // Fast path for the common case, to avoid StringBuffer overhead.
  if (!addParentheses && haveSource) {
    FunctionToStringCache& cache = cx->zone()->functionToStringCache();
    if (JSString* str = cache.lookup(fun->baseScript())) {
      return str;
    }

    BaseScript* script = fun->baseScript();
    size_t start = script->toStringStart();
    size_t end = script->toStringEnd();
    JSString* str =
        (end - start <= ScriptSource::SourceDeflateLimit)
            ? script->scriptSource()->substring(cx, start, end)
            : script->scriptSource()->substringDontDeflate(cx, start, end);
    if (!str) {
      return nullptr;
    }

    cache.put(fun->baseScript(), str);
    return str;
  }

  JSStringBuilder out(cx);
  if (addParentheses) {
    if (!out.append('(')) {
      return nullptr;
    }
  }

  if (haveSource) {
    if (!fun->baseScript()->appendSourceDataForToString(cx, out)) {
      return nullptr;
    }
  } else if (!isToSource) {
    // For the toString() output the source representation must match
    // NativeFunction when no source text is available.
    //
    // NativeFunction:
    //   function PropertyName[~Yield,~Await]opt (
    //      FormalParameters[~Yield,~Await] ) { [native code] }
    //
    // Additionally, if |fun| is a well-known intrinsic object and is not
    // identified as an anonymous function, the portion of the returned
    // string that would be matched by IdentifierName must be the initial
    // value of the name property of |fun|.

    auto hasGetterOrSetterPrefix = [](JSAtom* name) {
      auto hasGetterOrSetterPrefix = [](const auto* chars) {
        return (chars[0] == 'g' || chars[0] == 's') && chars[1] == 'e' &&
               chars[2] == 't' && chars[3] == ' ';
      };

      JS::AutoCheckCannotGC nogc;
      return name->length() >= 4 &&
             (name->hasLatin1Chars()
                  ? hasGetterOrSetterPrefix(name->latin1Chars(nogc))
                  : hasGetterOrSetterPrefix(name->twoByteChars(nogc)));
    };

    if (!out.append("function")) {
      return nullptr;
    }

    // We don't want to fully parse the function's name here because of
    // performance reasons, so only append the name if we're confident it
    // can be matched as the 'PropertyName' grammar production.
    if (fun->explicitName() && !fun->isBoundFunction() &&
        (fun->kind() == FunctionFlags::NormalFunction ||
         fun->kind() == FunctionFlags::ClassConstructor)) {
      if (!out.append(' ')) {
        return nullptr;
      }

      // Built-in getters or setters are classified as normal
      // functions, strip any leading "get " or "set " if present.
      JSAtom* name = fun->explicitName();
      size_t offset = hasGetterOrSetterPrefix(name) ? 4 : 0;
      if (!out.appendSubstring(name, offset, name->length() - offset)) {
        return nullptr;
      }
    }

    if (!out.append("() {\n    [native code]\n}")) {
      return nullptr;
    }
  } else {
    if (fun->isAsync()) {
      if (!out.append("async ")) {
        return nullptr;
      }
    }

    if (!fun->isArrow()) {
      if (!out.append("function")) {
        return nullptr;
      }

      if (fun->isGenerator()) {
        if (!out.append('*')) {
          return nullptr;
        }
      }
    }

    if (fun->explicitName()) {
      if (!out.append(' ')) {
        return nullptr;
      }

      if (fun->isBoundFunction()) {
        JSLinearString* boundName = JSFunction::getBoundFunctionName(cx, fun);
        if (!boundName || !out.append(boundName)) {
          return nullptr;
        }
      } else {
        if (!out.append(fun->explicitName())) {
          return nullptr;
        }
      }
    }

    if (!out.append("() {\n    [native code]\n}")) {
      return nullptr;
    }
  }

  if (addParentheses) {
    if (!out.append(')')) {
      return nullptr;
    }
  }

  return out.finishString();
}

JSString* fun_toStringHelper(JSContext* cx, HandleObject obj, bool isToSource) {
  if (!obj->is<JSFunction>()) {
    if (JSFunToStringOp op = obj->getOpsFunToString()) {
      return op(cx, obj, isToSource);
    }

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, js_Function_str,
                              js_toString_str, "object");
    return nullptr;
  }

  return FunctionToString(cx, obj.as<JSFunction>(), isToSource);
}

bool js::fun_toString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(IsFunctionObject(args.calleev()));

  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  JSString* str = fun_toStringHelper(cx, obj, /* isToSource = */ false);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static bool fun_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(IsFunctionObject(args.calleev()));

  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  RootedString str(cx);
  if (obj->isCallable()) {
    str = fun_toStringHelper(cx, obj, /* isToSource = */ true);
  } else {
    str = ObjectToSource(cx, obj);
  }
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

bool js::fun_call(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  HandleValue func = args.thisv();

  // We don't need to do this -- Call would do it for us -- but the error
  // message is *much* better if we do this here.  (Without this,
  // JSDVG_SEARCH_STACK tries to decompile |func| as if it were |this| in
  // the scripted caller's frame -- so for example
  //
  //   Function.prototype.call.call({});
  //
  // would identify |{}| as |this| as being the result of evaluating
  // |Function.prototype.call| and would conclude, "Function.prototype.call
  // is not a function".  Grotesque.)
  if (!IsCallable(func)) {
    ReportIncompatibleMethod(cx, args, &JSFunction::class_);
    return false;
  }

  size_t argCount = args.length();
  if (argCount > 0) {
    argCount--;  // strip off provided |this|
  }

  InvokeArgs iargs(cx);
  if (!iargs.init(cx, argCount)) {
    return false;
  }

  for (size_t i = 0; i < argCount; i++) {
    iargs[i].set(args[i + 1]);
  }

  return Call(cx, func, args.get(0), iargs, args.rval());
}

// ES5 15.3.4.3
bool js::fun_apply(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  //
  // Note that we must check callability here, not at actual call time,
  // because extracting argument values from the provided arraylike might
  // have side effects or throw an exception.
  HandleValue fval = args.thisv();
  if (!IsCallable(fval)) {
    ReportIncompatibleMethod(cx, args, &JSFunction::class_);
    return false;
  }

  // Step 2.
  if (args.length() < 2 || args[1].isNullOrUndefined()) {
    return fun_call(cx, (args.length() > 0) ? 1 : 0, vp);
  }

  // Step 3.
  if (!args[1].isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_APPLY_ARGS, js_apply_str);
    return false;
  }

  // Steps 4-5 (note erratum removing steps originally numbered 5 and 7 in
  // original version of ES5).
  RootedObject aobj(cx, &args[1].toObject());
  uint64_t length;
  if (!GetLengthProperty(cx, aobj, &length)) {
    return false;
  }

  // Step 6.
  InvokeArgs args2(cx);
  if (!args2.init(cx, length)) {
    return false;
  }

  MOZ_ASSERT(length <= ARGS_LENGTH_MAX);

  // Steps 7-8.
  if (!GetElements(cx, aobj, length, args2.array())) {
    return false;
  }

  // Step 9.
  return Call(cx, fval, args[0], args2, args.rval());
}

static const JSFunctionSpec function_methods[] = {
    JS_FN(js_toSource_str, fun_toSource, 0, 0),
    JS_FN(js_toString_str, fun_toString, 0, 0),
    JS_FN(js_apply_str, fun_apply, 2, 0),
    JS_FN(js_call_str, fun_call, 1, 0),
    JS_SELF_HOSTED_FN("bind", "FunctionBind", 2, 0),
    JS_SYM_FN(hasInstance, fun_symbolHasInstance, 1,
              JSPROP_READONLY | JSPROP_PERMANENT),
    JS_FS_END};

static const JSClassOps JSFunctionClassOps = {
    nullptr,         // addProperty
    nullptr,         // delProperty
    fun_enumerate,   // enumerate
    nullptr,         // newEnumerate
    fun_resolve,     // resolve
    fun_mayResolve,  // mayResolve
    nullptr,         // finalize
    nullptr,         // call
    nullptr,         // hasInstance
    nullptr,         // construct
    fun_trace,       // trace
};

static const ClassSpec JSFunctionClassSpec = {
    CreateFunctionConstructor, CreateFunctionPrototype, nullptr, nullptr,
    function_methods,          function_properties};

const JSClass JSFunction::class_ = {js_Function_str,
                                    JSCLASS_HAS_CACHED_PROTO(JSProto_Function),
                                    &JSFunctionClassOps, &JSFunctionClassSpec};

const JSClass* const js::FunctionClassPtr = &JSFunction::class_;

bool JSFunction::isDerivedClassConstructor() const {
  bool derived = hasBaseScript() && baseScript()->isDerivedClassConstructor();
  MOZ_ASSERT_IF(derived, isClassConstructor());
  return derived;
}

bool JSFunction::isSyntheticFunction() const {
  bool synthetic = hasBaseScript() && baseScript()->isSyntheticFunction();
  MOZ_ASSERT_IF(synthetic, isMethod());
  return synthetic;
}

/* static */
bool JSFunction::getLength(JSContext* cx, HandleFunction fun,
                           uint16_t* length) {
  MOZ_ASSERT(!fun->isBoundFunction());

  if (fun->isNativeFun()) {
    *length = fun->nargs();
    return true;
  }

  JSScript* script = getOrCreateScript(cx, fun);
  if (!script) {
    return false;
  }

  *length = script->funLength();
  return true;
}

/* static */
bool JSFunction::getUnresolvedLength(JSContext* cx, HandleFunction fun,
                                     MutableHandleValue v) {
  MOZ_ASSERT(!IsInternalFunctionObject(*fun));
  MOZ_ASSERT(!fun->hasResolvedLength());

  // Bound functions' length can have values up to MAX_SAFE_INTEGER, so
  // they're handled differently from other functions.
  if (fun->isBoundFunction()) {
    constexpr auto lengthSlot = FunctionExtended::BOUND_FUNCTION_LENGTH_SLOT;
    MOZ_ASSERT(fun->getExtendedSlot(lengthSlot).isNumber());
    v.set(fun->getExtendedSlot(lengthSlot));
    return true;
  }

  uint16_t length;
  if (!JSFunction::getLength(cx, fun, &length)) {
    return false;
  }

  v.setInt32(length);
  return true;
}

JSAtom* JSFunction::infallibleGetUnresolvedName(JSContext* cx) {
  MOZ_ASSERT(!IsInternalFunctionObject(*this));
  MOZ_ASSERT(!hasResolvedName());

  if (JSAtom* name = explicitOrInferredName()) {
    return name;
  }

  return cx->names().empty;
}

/* static */
bool JSFunction::getUnresolvedName(JSContext* cx, HandleFunction fun,
                                   MutableHandleValue v) {
  if (fun->isBoundFunction()) {
    JSLinearString* name = JSFunction::getBoundFunctionName(cx, fun);
    if (!name) {
      return false;
    }

    v.setString(name);
    return true;
  }

  v.setString(fun->infallibleGetUnresolvedName(cx));
  return true;
}

/* static */
JSLinearString* JSFunction::getBoundFunctionName(JSContext* cx,
                                                 HandleFunction fun) {
  MOZ_ASSERT(fun->isBoundFunction());
  JSAtom* name = fun->explicitName();

  // Bound functions are never unnamed.
  MOZ_ASSERT(name);

  // If the bound function prefix is present, return the name as is.
  if (fun->hasBoundFunctionNamePrefix()) {
    return name;
  }

  // Otherwise return "bound " * (number of bound function targets) + name.
  size_t boundTargets = 0;
  for (JSFunction* boundFn = fun; boundFn->isBoundFunction();) {
    boundTargets++;

    JSObject* target = boundFn->getBoundFunctionTarget();
    if (!target->is<JSFunction>()) {
      break;
    }
    boundFn = &target->as<JSFunction>();
  }

  // |function /*unnamed*/ (){...}.bind()| is a common case, handle it here.
  if (name->empty() && boundTargets == 1) {
    return cx->names().boundWithSpace;
  }

  static constexpr char boundWithSpaceChars[] = "bound ";
  static constexpr size_t boundWithSpaceCharsLength =
      js_strlen(boundWithSpaceChars);
  MOZ_ASSERT(
      StringEqualsAscii(cx->names().boundWithSpace, boundWithSpaceChars));

  JSStringBuilder sb(cx);
  if (name->hasTwoByteChars() && !sb.ensureTwoByteChars()) {
    return nullptr;
  }

  CheckedInt<size_t> len(boundTargets);
  len *= boundWithSpaceCharsLength;
  len += name->length();
  if (!len.isValid()) {
    ReportAllocationOverflow(cx);
    return nullptr;
  }
  if (!sb.reserve(len.value())) {
    return nullptr;
  }

  while (boundTargets--) {
    sb.infallibleAppend(boundWithSpaceChars, boundWithSpaceCharsLength);
  }
  sb.infallibleAppendSubstring(name, 0, name->length());

  return sb.finishString();
}

static const js::Value& BoundFunctionEnvironmentSlotValue(const JSFunction* fun,
                                                          uint32_t slotIndex) {
  MOZ_ASSERT(fun->isBoundFunction());
  MOZ_ASSERT(fun->environment()->is<CallObject>());
  CallObject* callObject = &fun->environment()->as<CallObject>();
  return callObject->getSlot(slotIndex);
}

JSObject* JSFunction::getBoundFunctionTarget() const {
  js::Value targetVal =
      BoundFunctionEnvironmentSlotValue(this, BoundFunctionEnvTargetSlot);
  MOZ_ASSERT(IsCallable(targetVal));
  return &targetVal.toObject();
}

const js::Value& JSFunction::getBoundFunctionThis() const {
  return BoundFunctionEnvironmentSlotValue(this, BoundFunctionEnvThisSlot);
}

static ArrayObject* GetBoundFunctionArguments(const JSFunction* boundFun) {
  js::Value argsVal =
      BoundFunctionEnvironmentSlotValue(boundFun, BoundFunctionEnvArgsSlot);
  return &argsVal.toObject().as<ArrayObject>();
}

const js::Value& JSFunction::getBoundFunctionArgument(unsigned which) const {
  MOZ_ASSERT(which < getBoundFunctionArgumentCount());
  return GetBoundFunctionArguments(this)->getDenseElement(which);
}

size_t JSFunction::getBoundFunctionArgumentCount() const {
  return GetBoundFunctionArguments(this)->length();
}

static JSAtom* AppendBoundFunctionPrefix(JSContext* cx, JSString* str) {
  static constexpr char boundWithSpaceChars[] = "bound ";
  MOZ_ASSERT(
      StringEqualsAscii(cx->names().boundWithSpace, boundWithSpaceChars));

  StringBuffer sb(cx);
  if (!sb.append(boundWithSpaceChars) || !sb.append(str)) {
    return nullptr;
  }
  return sb.finishAtom();
}

/* static */
bool JSFunction::finishBoundFunctionInit(JSContext* cx, HandleFunction bound,
                                         HandleObject targetObj,
                                         int32_t argCount) {
  bound->setIsBoundFunction();
  MOZ_ASSERT(bound->getBoundFunctionTarget() == targetObj);

  // 9.4.1.3 BoundFunctionCreate, steps 1, 3-5, 8-12 (Already performed).

  // 9.4.1.3 BoundFunctionCreate, step 6.
  if (targetObj->isConstructor()) {
    bound->setIsConstructor();
  }

  // 9.4.1.3 BoundFunctionCreate, step 2.
  RootedObject proto(cx);
  if (!GetPrototype(cx, targetObj, &proto)) {
    return false;
  }

  // 9.4.1.3 BoundFunctionCreate, step 7.
  if (bound->staticPrototype() != proto) {
    if (!SetPrototype(cx, bound, proto)) {
      return false;
    }
  }

  double length = 0.0;

  // Try to avoid invoking the resolve hook.
  if (targetObj->is<JSFunction>() &&
      !targetObj->as<JSFunction>().hasResolvedLength()) {
    RootedValue targetLength(cx);
    if (!JSFunction::getUnresolvedLength(cx, targetObj.as<JSFunction>(),
                                         &targetLength)) {
      return false;
    }

    length = std::max(0.0, targetLength.toNumber() - argCount);
  } else {
    // 19.2.3.2 Function.prototype.bind, step 5.
    bool hasLength;
    RootedId idRoot(cx, NameToId(cx->names().length));
    if (!HasOwnProperty(cx, targetObj, idRoot, &hasLength)) {
      return false;
    }

    // 19.2.3.2 Function.prototype.bind, step 6.
    if (hasLength) {
      RootedValue targetLength(cx);
      if (!GetProperty(cx, targetObj, targetObj, idRoot, &targetLength)) {
        return false;
      }

      if (targetLength.isNumber()) {
        length =
            std::max(0.0, JS::ToInteger(targetLength.toNumber()) - argCount);
      }
    }

    // 19.2.3.2 Function.prototype.bind, step 7 (implicit).
  }

  // 19.2.3.2 Function.prototype.bind, step 8.
  bound->setExtendedSlot(FunctionExtended::BOUND_FUNCTION_LENGTH_SLOT,
                         NumberValue(length));

  MOZ_ASSERT(!bound->hasGuessedAtom());

  // Try to avoid invoking the resolve hook.
  if (targetObj->is<JSFunction>() &&
      !targetObj->as<JSFunction>().hasResolvedName()) {
    JSFunction* targetFn = &targetObj->as<JSFunction>();

    // If the target is a bound function with a prefixed name, we can't
    // lazily compute the full name in getBoundFunctionName(), therefore
    // we need to append the bound function name prefix here.
    if (targetFn->isBoundFunction() && targetFn->hasBoundFunctionNamePrefix()) {
      JSAtom* name = AppendBoundFunctionPrefix(cx, targetFn->explicitName());
      if (!name) {
        return false;
      }
      bound->setPrefixedBoundFunctionName(name);
    } else {
      JSAtom* name = targetFn->infallibleGetUnresolvedName(cx);
      MOZ_ASSERT(name);

      bound->setAtom(name);
    }
  } else {
    // 19.2.3.2 Function.prototype.bind, step 9.
    RootedValue targetName(cx);
    if (!GetProperty(cx, targetObj, targetObj, cx->names().name, &targetName)) {
      return false;
    }

    // 19.2.3.2 Function.prototype.bind, step 10.
    if (!targetName.isString()) {
      targetName.setString(cx->names().empty);
    }

    // If the target itself is a bound function (with a resolved name), we
    // can't compute the full name in getBoundFunctionName() based only on
    // the number of bound target functions, therefore we need to store
    // the complete prefixed name here.
    if (targetObj->is<JSFunction>() &&
        targetObj->as<JSFunction>().isBoundFunction()) {
      JSAtom* name = AppendBoundFunctionPrefix(cx, targetName.toString());
      if (!name) {
        return false;
      }
      bound->setPrefixedBoundFunctionName(name);
    } else {
      JSAtom* name = AtomizeString(cx, targetName.toString());
      if (!name) {
        return false;
      }
      bound->setAtom(name);
    }
  }

  return true;
}

/* static */
bool JSFunction::delazifyLazilyInterpretedFunction(JSContext* cx,
                                                   HandleFunction fun) {
  MOZ_ASSERT(fun->hasBaseScript());
  MOZ_ASSERT(cx->compartment() == fun->compartment());

  // The function must be same-compartment but might be cross-realm. Make sure
  // the script is created in the function's realm.
  AutoRealm ar(cx, fun);

  Rooted<BaseScript*> lazy(cx, fun->baseScript());
  RootedFunction canonicalFun(cx, lazy->function());

  // If this function is non-canonical, then use the canonical function first
  // to get the delazified script. This may result in calling this method
  // again on the canonical function. This ensures the canonical function is
  // always non-lazy if any of the clones are non-lazy.
  if (fun != canonicalFun) {
    JSScript* script = JSFunction::getOrCreateScript(cx, canonicalFun);
    if (!script) {
      return false;
    }

    // Delazifying the canonical function should naturally make us non-lazy
    // because we share a BaseScript with the canonical function.
    MOZ_ASSERT(fun->hasBytecode());
    return true;
  }

  // Finally, compile the script if it really doesn't exist.
  if (!frontend::DelazifyCanonicalScriptedFunction(cx, fun)) {
    // The frontend shouldn't fail after linking the function and the
    // non-lazy script together.
    MOZ_ASSERT(fun->baseScript() == lazy);
    MOZ_ASSERT(lazy->isReadyForDelazification());
    return false;
  }

  return true;
}

/* static */
bool JSFunction::delazifySelfHostedLazyFunction(JSContext* cx,
                                                js::HandleFunction fun) {
  MOZ_ASSERT(cx->compartment() == fun->compartment());

  // The function must be same-compartment but might be cross-realm. Make sure
  // the script is created in the function's realm.
  AutoRealm ar(cx, fun);

  /* Lazily cloned self-hosted script. */
  MOZ_ASSERT(fun->isSelfHostedBuiltin());
  Rooted<PropertyName*> funName(cx, GetClonedSelfHostedFunctionName(fun));
  if (!funName) {
    return false;
  }
  return cx->runtime()->cloneSelfHostedFunctionScript(cx, funName, fun);
}

void JSFunction::maybeRelazify(JSRuntime* rt) {
  MOZ_ASSERT(!isIncomplete(), "Cannot relazify incomplete functions");

  // Don't relazify functions in compartments that are active.
  Realm* realm = this->realm();
  if (!rt->allowRelazificationForTesting) {
    if (realm->compartment()->gcState.hasEnteredRealm) {
      return;
    }

    MOZ_ASSERT(!realm->hasBeenEnteredIgnoringJit());
  }

  // The caller should have checked we're not in the self-hosting zone (it's
  // shared with worker runtimes so relazifying functions in it will race).
  MOZ_ASSERT(!realm->isSelfHostingRealm());

  // Don't relazify if the realm is being debugged. The debugger side-tables
  // such as the set of active breakpoints require bytecode to exist.
  if (realm->isDebuggee()) {
    return;
  }

  // Don't relazify if we are collecting coverage so that we do not lose count
  // information.
  if (coverage::IsLCovEnabled()) {
    return;
  }

  // Check the script's eligibility.
  JSScript* script = nonLazyScript();
  if (!script->allowRelazify()) {
    return;
  }
  MOZ_ASSERT(script->isRelazifiable());

  // There must not be any JIT code attached since the relazification process
  // does not know how to discard it. In general, the GC should discard most JIT
  // code before attempting relazification.
  if (script->hasJitScript()) {
    return;
  }

  if (isSelfHostedBuiltin()) {
    gc::PreWriteBarrier(script);
    initSelfHostedLazyScript(&rt->selfHostedLazyScript.ref());
  } else {
    script->relazify(rt);
  }
}

js::GeneratorKind JSFunction::clonedSelfHostedGeneratorKind() const {
  MOZ_ASSERT(hasSelfHostedLazyScript());

  // This is a lazy clone of a self-hosted builtin. It has no BaseScript, and
  // `this->flags_` does not contain the generator kind. Consult the
  // implementation in the self-hosting realm, which has a BaseScript.
  MOZ_RELEASE_ASSERT(isExtended());
  PropertyName* name = GetClonedSelfHostedFunctionName(this);
  return runtimeFromMainThread()->getSelfHostedFunctionGeneratorKind(name);
}

// ES2018 draft rev 2aea8f3e617b49df06414eb062ab44fad87661d3
// 19.2.1.1.1 CreateDynamicFunction( constructor, newTarget, kind, args )
static bool CreateDynamicFunction(JSContext* cx, const CallArgs& args,
                                  GeneratorKind generatorKind,
                                  FunctionAsyncKind asyncKind) {
  using namespace frontend;

  // Steps 1-5.
  bool isGenerator = generatorKind == GeneratorKind::Generator;
  bool isAsync = asyncKind == FunctionAsyncKind::AsyncFunction;

  RootedScript maybeScript(cx);
  const char* filename;
  unsigned lineno;
  bool mutedErrors;
  uint32_t pcOffset;
  DescribeScriptedCallerForCompilation(cx, &maybeScript, &filename, &lineno,
                                       &pcOffset, &mutedErrors);

  const char* introductionType = "Function";
  if (isAsync) {
    if (isGenerator) {
      introductionType = "AsyncGenerator";
    } else {
      introductionType = "AsyncFunction";
    }
  } else if (isGenerator) {
    introductionType = "GeneratorFunction";
  }

  const char* introducerFilename = filename;
  if (maybeScript && maybeScript->scriptSource()->introducerFilename()) {
    introducerFilename = maybeScript->scriptSource()->introducerFilename();
  }

  CompileOptions options(cx);
  options.setMutedErrors(mutedErrors)
      .setFileAndLine(filename, 1)
      .setNoScriptRval(false)
      .setIntroductionInfo(introducerFilename, introductionType, lineno,
                           pcOffset)
      .setdeferDebugMetadata();

  JSStringBuilder sb(cx);

  if (isAsync) {
    if (!sb.append("async ")) {
      return false;
    }
  }
  if (!sb.append("function")) {
    return false;
  }
  if (isGenerator) {
    if (!sb.append('*')) {
      return false;
    }
  }

  if (!sb.append(" anonymous(")) {
    return false;
  }

  if (args.length() > 1) {
    RootedString str(cx);

    // Steps 10, 14.d.
    unsigned n = args.length() - 1;

    for (unsigned i = 0; i < n; i++) {
      // Steps 14.a-b, 14.d.i-ii.
      str = ToString<CanGC>(cx, args[i]);
      if (!str) {
        return false;
      }

      // Steps 14.b, 14.d.iii.
      if (!sb.append(str)) {
        return false;
      }

      if (i < args.length() - 2) {
        // Step 14.d.iii.
        if (!sb.append(',')) {
          return false;
        }
      }
    }
  }

  if (!sb.append('\n')) {
    return false;
  }

  // Remember the position of ")".
  Maybe<uint32_t> parameterListEnd = Some(uint32_t(sb.length()));
  MOZ_ASSERT(FunctionConstructorMedialSigils[0] == ')');

  if (!sb.append(FunctionConstructorMedialSigils)) {
    return false;
  }

  if (args.length() > 0) {
    // Steps 13, 14.e, 15.
    RootedString body(cx, ToString<CanGC>(cx, args[args.length() - 1]));
    if (!body || !sb.append(body)) {
      return false;
    }
  }

  if (!sb.append(FunctionConstructorFinalBrace)) {
    return false;
  }

  // The parser only accepts two byte strings.
  if (!sb.ensureTwoByteChars()) {
    return false;
  }

  RootedString functionText(cx, sb.finishString());
  if (!functionText) {
    return false;
  }

  // Block this call if security callbacks forbid it.
  Handle<GlobalObject*> global = cx->global();
  if (!GlobalObject::isRuntimeCodeGenEnabled(cx, functionText, global)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_FUNCTION);
    return false;
  }

  // Steps 7.a-b, 8.a-b, 9.a-b, 16-28.
  AutoStableStringChars stableChars(cx);
  if (!stableChars.initTwoByte(cx, functionText)) {
    return false;
  }

  mozilla::Range<const char16_t> chars = stableChars.twoByteRange();
  SourceOwnership ownership = stableChars.maybeGiveOwnershipToCaller()
                                  ? SourceOwnership::TakeOwnership
                                  : SourceOwnership::Borrowed;
  SourceText<char16_t> srcBuf;
  if (!srcBuf.init(cx, chars.begin().get(), chars.length(), ownership)) {
    return false;
  }

  FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::Expression;

  RootedFunction fun(cx);
  JSProtoKey protoKey;
  if (isAsync) {
    if (isGenerator) {
      fun = CompileStandaloneAsyncGenerator(cx, options, srcBuf,
                                            parameterListEnd, syntaxKind);
      protoKey = JSProto_AsyncGeneratorFunction;
    } else {
      fun = CompileStandaloneAsyncFunction(cx, options, srcBuf,
                                           parameterListEnd, syntaxKind);
      protoKey = JSProto_AsyncFunction;
    }
  } else {
    if (isGenerator) {
      fun = CompileStandaloneGenerator(cx, options, srcBuf, parameterListEnd,
                                       syntaxKind);
      protoKey = JSProto_GeneratorFunction;
    } else {
      fun = CompileStandaloneFunction(cx, options, srcBuf, parameterListEnd,
                                      syntaxKind);
      protoKey = JSProto_Function;
    }
  }
  if (!fun) {
    return false;
  }

  RootedValue undefValue(cx);
  RootedScript funScript(cx, JS_GetFunctionScript(cx, fun));
  if (funScript && !UpdateDebugMetadata(cx, funScript, options, undefValue,
                                        nullptr, maybeScript, maybeScript)) {
    return false;
  }

  if (fun->isInterpreted()) {
    fun->initEnvironment(&cx->global()->lexicalEnvironment());
  }

  // Steps 6, 29.
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, protoKey, &proto)) {
    return false;
  }

  // Steps 7.d, 8.d (implicit).
  // Call SetPrototype if an explicit prototype was given.
  if (proto && !SetPrototype(cx, fun, proto)) {
    return false;
  }

  // Step 38.
  args.rval().setObject(*fun);
  return true;
}

bool js::Function(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CreateDynamicFunction(cx, args, GeneratorKind::NotGenerator,
                               FunctionAsyncKind::SyncFunction);
}

bool js::Generator(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CreateDynamicFunction(cx, args, GeneratorKind::Generator,
                               FunctionAsyncKind::SyncFunction);
}

bool js::AsyncFunctionConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CreateDynamicFunction(cx, args, GeneratorKind::NotGenerator,
                               FunctionAsyncKind::AsyncFunction);
}

bool js::AsyncGeneratorConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CreateDynamicFunction(cx, args, GeneratorKind::Generator,
                               FunctionAsyncKind::AsyncFunction);
}

bool JSFunction::isBuiltinFunctionConstructor() {
  return maybeNative() == Function || maybeNative() == Generator;
}

bool JSFunction::needsExtraBodyVarEnvironment() const {
  if (isNativeFun()) {
    return false;
  }

  if (!nonLazyScript()->functionHasExtraBodyVarScope()) {
    return false;
  }

  return nonLazyScript()->functionExtraBodyVarScope()->hasEnvironment();
}

bool JSFunction::needsNamedLambdaEnvironment() const {
  if (!isNamedLambda()) {
    return false;
  }

  LexicalScope* scope = nonLazyScript()->maybeNamedLambdaScope();
  if (!scope) {
    return false;
  }

  return scope->hasEnvironment();
}

bool JSFunction::needsCallObject() const {
  if (isNativeFun()) {
    return false;
  }

  MOZ_ASSERT(hasBytecode());

  // Note: this should be kept in sync with
  // FunctionBox::needsCallObjectRegardlessOfBindings().
  MOZ_ASSERT_IF(
      baseScript()->funHasExtensibleScope() || isGenerator() || isAsync(),
      nonLazyScript()->bodyScope()->hasEnvironment());

  return nonLazyScript()->bodyScope()->hasEnvironment();
}

JSFunction* js::NewScriptedFunction(
    JSContext* cx, unsigned nargs, FunctionFlags flags, HandleAtom atom,
    HandleObject proto /* = nullptr */,
    gc::AllocKind allocKind /* = AllocKind::FUNCTION */,
    NewObjectKind newKind /* = GenericObject */,
    HandleObject enclosingEnvArg /* = nullptr */) {
  RootedObject enclosingEnv(cx, enclosingEnvArg);
  if (!enclosingEnv) {
    enclosingEnv = &cx->global()->lexicalEnvironment();
  }
  return NewFunctionWithProto(cx, nullptr, nargs, flags, enclosingEnv, atom,
                              proto, allocKind, newKind);
}

#ifdef DEBUG
static JSObject* SkipEnvironmentObjects(JSObject* env) {
  if (!env) {
    return nullptr;
  }
  while (env->is<EnvironmentObject>()) {
    env = &env->as<EnvironmentObject>().enclosingEnvironment();
  }
  return env;
}

static bool NewFunctionEnvironmentIsWellFormed(JSContext* cx,
                                               HandleObject env) {
  // Assert that the terminating environment is null, global, or a debug
  // scope proxy. All other cases of polluting global scope behavior are
  // handled by EnvironmentObjects (viz. non-syntactic DynamicWithObject and
  // NonSyntacticVariablesObject).
  RootedObject terminatingEnv(cx, SkipEnvironmentObjects(env));
  return !terminatingEnv || terminatingEnv == cx->global() ||
         terminatingEnv->is<DebugEnvironmentProxy>();
}
#endif

JSFunction* js::NewFunctionWithProto(
    JSContext* cx, Native native, unsigned nargs, FunctionFlags flags,
    HandleObject enclosingEnv, HandleAtom atom, HandleObject proto,
    gc::AllocKind allocKind /* = AllocKind::FUNCTION */,
    NewObjectKind newKind /* = GenericObject */) {
  MOZ_ASSERT(allocKind == gc::AllocKind::FUNCTION ||
             allocKind == gc::AllocKind::FUNCTION_EXTENDED);
  MOZ_ASSERT_IF(native, !enclosingEnv);
  MOZ_ASSERT(NewFunctionEnvironmentIsWellFormed(cx, enclosingEnv));

  // NOTE: Keep this in sync with `CreateFunctionFast` in Stencil.cpp

  JSFunction* fun =
      NewObjectWithClassProto<JSFunction>(cx, proto, allocKind, newKind);
  if (!fun) {
    return nullptr;
  }

  if (allocKind == gc::AllocKind::FUNCTION_EXTENDED) {
    flags.setIsExtended();
  }

  // Disallow flags that require special union arms to be initialized.
  MOZ_ASSERT(!flags.hasSelfHostedLazyScript());
  MOZ_ASSERT(!flags.isWasmWithJitEntry());

  /* Initialize all function members. */
  fun->setArgCount(uint16_t(nargs));
  fun->setFlags(flags);
  if (fun->isInterpreted()) {
    fun->initScript(nullptr);
    fun->initEnvironment(enclosingEnv);
  } else {
    MOZ_ASSERT(fun->isNativeFun());
    fun->initNative(native, nullptr);
  }
  if (allocKind == gc::AllocKind::FUNCTION_EXTENDED) {
    fun->initializeExtended();
  }
  fun->initAtom(atom);

  return fun;
}

bool js::GetFunctionPrototype(JSContext* cx, js::GeneratorKind generatorKind,
                              js::FunctionAsyncKind asyncKind,
                              js::MutableHandleObject proto) {
  // Self-hosted functions have null [[Prototype]]. This allows self-hosting to
  // support generators, despite this loop in the builtin object graph:
  // - %Generator%.prototype.[[Prototype]] is Iterator.prototype;
  // - Iterator.prototype has self-hosted methods (iterator helpers).
  if (cx->realm()->isSelfHostingRealm()) {
    proto.set(nullptr);
    return true;
  }

  if (generatorKind == js::GeneratorKind::NotGenerator) {
    if (asyncKind == js::FunctionAsyncKind::SyncFunction) {
      proto.set(nullptr);
      return true;
    }

    proto.set(
        GlobalObject::getOrCreateAsyncFunctionPrototype(cx, cx->global()));
  } else {
    if (asyncKind == js::FunctionAsyncKind::SyncFunction) {
      proto.set(GlobalObject::getOrCreateGeneratorFunctionPrototype(
          cx, cx->global()));
    } else {
      proto.set(GlobalObject::getOrCreateAsyncGenerator(cx, cx->global()));
    }
  }
  return !!proto;
}

bool js::CanReuseScriptForClone(JS::Realm* realm, HandleFunction fun,
                                HandleObject newEnclosingEnv) {
  MOZ_ASSERT(fun->isInterpreted());

  if (realm != fun->realm()) {
    return false;
  }

  if (newEnclosingEnv->is<GlobalObject>()) {
    return true;
  }

  // Don't need to clone the script if newEnclosingEnv is a syntactic scope,
  // since in that case we have some actual scope objects on our scope chain and
  // whatnot; whoever put them there should be responsible for setting our
  // script's flags appropriately.  We hit this case for JSOp::Lambda, for
  // example.
  if (IsSyntacticEnvironment(newEnclosingEnv)) {
    return true;
  }

  // We need to clone the script if we're not already marked as having a
  // non-syntactic scope. The HasNonSyntacticScope flag is not computed for lazy
  // scripts so fallback to checking the scope chain.
  BaseScript* script = fun->baseScript();
  return script->hasNonSyntacticScope() ||
         script->enclosingScope()->hasOnChain(ScopeKind::NonSyntactic);
}

static inline JSFunction* NewFunctionClone(JSContext* cx, HandleFunction fun,
                                           NewObjectKind newKind,
                                           gc::AllocKind allocKind,
                                           HandleObject proto) {
  RootedObject cloneProto(cx, proto);
  if (!proto) {
    if (!GetFunctionPrototype(cx, fun->generatorKind(), fun->asyncKind(),
                              &cloneProto)) {
      return nullptr;
    }
  }

  RootedFunction clone(cx);
  clone =
      NewObjectWithClassProto<JSFunction>(cx, cloneProto, allocKind, newKind);
  if (!clone) {
    return nullptr;
  }

  constexpr uint16_t NonCloneableFlags = FunctionFlags::EXTENDED |
                                         FunctionFlags::RESOLVED_LENGTH |
                                         FunctionFlags::RESOLVED_NAME;

  FunctionFlags flags = fun->flags();
  flags.clearFlags(NonCloneableFlags);

  if (allocKind == gc::AllocKind::FUNCTION_EXTENDED) {
    flags.setIsExtended();
  }

  clone->setArgCount(fun->nargs());
  clone->setFlags(flags);

  JSAtom* atom = fun->displayAtom();
  if (atom) {
    cx->markAtom(atom);
  }
  clone->initAtom(atom);

  if (allocKind == gc::AllocKind::FUNCTION_EXTENDED) {
    if (fun->isExtended() && fun->compartment() == cx->compartment()) {
      for (unsigned i = 0; i < FunctionExtended::NUM_EXTENDED_SLOTS; i++) {
        clone->initExtendedSlot(i, fun->getExtendedSlot(i));
      }
    } else {
      clone->initializeExtended();
    }
  }

  return clone;
}

JSFunction* js::CloneFunctionReuseScript(JSContext* cx, HandleFunction fun,
                                         HandleObject enclosingEnv,
                                         gc::AllocKind allocKind,
                                         HandleObject proto) {
  MOZ_ASSERT(cx->realm() == fun->realm());
  MOZ_ASSERT(NewFunctionEnvironmentIsWellFormed(cx, enclosingEnv));
  MOZ_ASSERT(fun->isInterpreted());
  MOZ_ASSERT(!fun->isBoundFunction());
  MOZ_ASSERT(CanReuseScriptForClone(cx->realm(), fun, enclosingEnv));

  NewObjectKind newKind = GenericObject;
  RootedFunction clone(cx,
                       NewFunctionClone(cx, fun, newKind, allocKind, proto));
  if (!clone) {
    return nullptr;
  }

  if (fun->hasBaseScript()) {
    BaseScript* base = fun->baseScript();
    clone->initScript(base);
    clone->initEnvironment(enclosingEnv);
  } else {
    MOZ_ASSERT(fun->hasSelfHostedLazyScript());
    SelfHostedLazyScript* lazy = fun->selfHostedLazyScript();
    clone->initSelfHostedLazyScript(lazy);
    clone->initEnvironment(enclosingEnv);
  }

  return clone;
}

JSFunction* js::CloneFunctionAndScript(JSContext* cx, HandleFunction fun,
                                       HandleObject enclosingEnv,
                                       HandleScope newScope,
                                       Handle<ScriptSourceObject*> sourceObject,
                                       gc::AllocKind allocKind,
                                       HandleObject proto /* = nullptr */) {
  MOZ_ASSERT(NewFunctionEnvironmentIsWellFormed(cx, enclosingEnv));
  MOZ_ASSERT(fun->isInterpreted());
  MOZ_ASSERT(!fun->isBoundFunction());

  JSScript::AutoDelazify funScript(cx, fun);
  if (!funScript) {
    return nullptr;
  }

  RootedFunction clone(
      cx, NewFunctionClone(cx, fun, TenuredObject, allocKind, proto));
  if (!clone) {
    return nullptr;
  }

  clone->initScript(nullptr);
  clone->initEnvironment(enclosingEnv);

  RootedScript script(cx, fun->nonLazyScript());
  MOZ_ASSERT(script->realm() == fun->realm());
  MOZ_ASSERT(cx->compartment() == clone->compartment(),
             "Otherwise we could relazify clone below!");

  RootedScript clonedScript(
      cx, CloneScriptIntoFunction(cx, newScope, clone, script, sourceObject));
  if (!clonedScript) {
    return nullptr;
  }
  DebugAPI::onNewScript(cx, clonedScript);

  return clone;
}

JSFunction* js::CloneAsmJSModuleFunction(JSContext* cx, HandleFunction fun) {
  MOZ_ASSERT(fun->isNativeFun());
  MOZ_ASSERT(IsAsmJSModule(fun));
  MOZ_ASSERT(fun->isExtended());
  MOZ_ASSERT(cx->compartment() == fun->compartment());

  JSFunction* clone =
      NewFunctionClone(cx, fun, GenericObject, gc::AllocKind::FUNCTION_EXTENDED,
                       /* proto = */ nullptr);
  if (!clone) {
    return nullptr;
  }

  MOZ_ASSERT(fun->native() == InstantiateAsmJS);
  MOZ_ASSERT(!fun->hasJitInfo());
  clone->initNative(InstantiateAsmJS, nullptr);

  return clone;
}

JSFunction* js::CloneSelfHostingIntrinsic(JSContext* cx, HandleFunction fun) {
  MOZ_ASSERT(fun->isNativeFun());
  MOZ_ASSERT(fun->realm()->isSelfHostingRealm());
  MOZ_ASSERT(!fun->isExtended());
  MOZ_ASSERT(cx->compartment() != fun->compartment());

  JSFunction* clone =
      NewFunctionClone(cx, fun, TenuredObject, gc::AllocKind::FUNCTION,
                       /* proto = */ nullptr);
  if (!clone) {
    return nullptr;
  }

  clone->initNative(fun->native(),
                    fun->hasJitInfo() ? fun->jitInfo() : nullptr);
  return clone;
}

static JSAtom* SymbolToFunctionName(JSContext* cx, JS::Symbol* symbol,
                                    FunctionPrefixKind prefixKind) {
  // Step 4.a.
  JSAtom* desc = symbol->description();

  // Step 4.b, no prefix fastpath.
  if (!desc && prefixKind == FunctionPrefixKind::None) {
    return cx->names().empty;
  }

  // Step 5 (reordered).
  StringBuffer sb(cx);
  if (prefixKind == FunctionPrefixKind::Get) {
    if (!sb.append("get ")) {
      return nullptr;
    }
  } else if (prefixKind == FunctionPrefixKind::Set) {
    if (!sb.append("set ")) {
      return nullptr;
    }
  }

  // Step 4.b.
  if (desc) {
    // Note: Private symbols are wedged in, as implementation wise they're
    // PrivateNameSymbols with a the source level name as a description
    // i.e. obj.#f desugars to obj.[PrivateNameSymbol("#f")], however
    // they don't use the symbol naming, but rather property naming.
    if (symbol->isPrivateName()) {
      if (!sb.append(desc)) {
        return nullptr;
      }
    } else {
      // Step 4.c.
      if (!sb.append('[') || !sb.append(desc) || !sb.append(']')) {
        return nullptr;
      }
    }
  }
  return sb.finishAtom();
}

static JSAtom* NameToFunctionName(JSContext* cx, HandleValue name,
                                  FunctionPrefixKind prefixKind) {
  MOZ_ASSERT(name.isString() || name.isNumeric());

  if (prefixKind == FunctionPrefixKind::None) {
    return ToAtom<CanGC>(cx, name);
  }

  JSString* nameStr = ToString(cx, name);
  if (!nameStr) {
    return nullptr;
  }

  StringBuffer sb(cx);
  if (prefixKind == FunctionPrefixKind::Get) {
    if (!sb.append("get ")) {
      return nullptr;
    }
  } else {
    if (!sb.append("set ")) {
      return nullptr;
    }
  }
  if (!sb.append(nameStr)) {
    return nullptr;
  }
  return sb.finishAtom();
}

/*
 * Return an atom for use as the name of a builtin method with the given
 * property id.
 *
 * Function names are always strings. If id is the well-known @@iterator
 * symbol, this returns "[Symbol.iterator]".  If a prefix is supplied the final
 * name is |prefix + " " + name|.
 *
 * Implements steps 3-5 of 9.2.11 SetFunctionName in ES2016.
 */
JSAtom* js::IdToFunctionName(
    JSContext* cx, HandleId id,
    FunctionPrefixKind prefixKind /* = FunctionPrefixKind::None */) {
  MOZ_ASSERT(id.isString() || id.isSymbol() || id.isInt());

  // No prefix fastpath.
  if (id.isAtom() && prefixKind == FunctionPrefixKind::None) {
    return id.toAtom();
  }

  // Step 3 (implicit).

  // Step 4.
  if (id.isSymbol()) {
    return SymbolToFunctionName(cx, id.toSymbol(), prefixKind);
  }

  // Step 5.
  RootedValue idv(cx, IdToValue(id));
  return NameToFunctionName(cx, idv, prefixKind);
}

bool js::SetFunctionName(JSContext* cx, HandleFunction fun, HandleValue name,
                         FunctionPrefixKind prefixKind) {
  MOZ_ASSERT(name.isString() || name.isSymbol() || name.isNumeric());

  // `fun` is a newly created function, so it can't already have an inferred
  // name.
  MOZ_ASSERT(!fun->hasInferredName());

  // Anonymous functions should neither have an own 'name' property nor a
  // resolved name at this point.
  MOZ_ASSERT(!fun->containsPure(cx->names().name));
  MOZ_ASSERT(!fun->hasResolvedName());

  JSAtom* funName = name.isSymbol()
                        ? SymbolToFunctionName(cx, name.toSymbol(), prefixKind)
                        : NameToFunctionName(cx, name, prefixKind);
  if (!funName) {
    return false;
  }

  fun->setInferredName(funName);

  return true;
}

JSFunction* js::DefineFunction(
    JSContext* cx, HandleObject obj, HandleId id, Native native, unsigned nargs,
    unsigned flags, gc::AllocKind allocKind /* = AllocKind::FUNCTION */) {
  RootedAtom atom(cx, IdToFunctionName(cx, id));
  if (!atom) {
    return nullptr;
  }

  MOZ_ASSERT(native);

  RootedFunction fun(cx);
  if (flags & JSFUN_CONSTRUCTOR) {
    fun = NewNativeConstructor(cx, native, nargs, atom, allocKind);
  } else {
    fun = NewNativeFunction(cx, native, nargs, atom, allocKind);
  }

  if (!fun) {
    return nullptr;
  }

  RootedValue funVal(cx, ObjectValue(*fun));
  if (!DefineDataProperty(cx, obj, id, funVal, flags & ~JSFUN_FLAGS_MASK)) {
    return nullptr;
  }

  return fun;
}

void js::ReportIncompatibleMethod(JSContext* cx, const CallArgs& args,
                                  const JSClass* clasp) {
  RootedValue thisv(cx, args.thisv());

#ifdef DEBUG
  switch (thisv.type()) {
    case ValueType::Object:
      MOZ_ASSERT(thisv.toObject().getClass() != clasp ||
                 !thisv.toObject().is<NativeObject>() ||
                 !thisv.toObject().staticPrototype() ||
                 thisv.toObject().staticPrototype()->getClass() != clasp);
      break;
    case ValueType::String:
      MOZ_ASSERT(clasp != &StringObject::class_);
      break;
    case ValueType::Double:
    case ValueType::Int32:
      MOZ_ASSERT(clasp != &NumberObject::class_);
      break;
    case ValueType::Boolean:
      MOZ_ASSERT(clasp != &BooleanObject::class_);
      break;
    case ValueType::Symbol:
      MOZ_ASSERT(clasp != &SymbolObject::class_);
      break;
    case ValueType::BigInt:
      MOZ_ASSERT(clasp != &BigIntObject::class_);
      break;
    case ValueType::Undefined:
    case ValueType::Null:
      break;
    case ValueType::Magic:
    case ValueType::PrivateGCThing:
      MOZ_CRASH("unexpected type");
  }
#endif

  if (JSFunction* fun = ReportIfNotFunction(cx, args.calleev())) {
    UniqueChars funNameBytes;
    if (const char* funName = GetFunctionNameBytes(cx, fun, &funNameBytes)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INCOMPATIBLE_PROTO, clasp->name, funName,
                               InformalValueTypeName(thisv));
    }
  }
}

void js::ReportIncompatible(JSContext* cx, const CallArgs& args) {
  if (JSFunction* fun = ReportIfNotFunction(cx, args.calleev())) {
    UniqueChars funNameBytes;
    if (const char* funName = GetFunctionNameBytes(cx, fun, &funNameBytes)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INCOMPATIBLE_METHOD, funName, "method",
                               InformalValueTypeName(args.thisv()));
    }
  }
}

namespace JS {
namespace detail {

JS_PUBLIC_API void CheckIsValidConstructible(const Value& calleev) {
  MOZ_ASSERT(calleev.toObject().isConstructor());
}

}  // namespace detail
}  // namespace JS
