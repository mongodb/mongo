/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS function support.
 */

#include "vm/JSFunction-inl.h"

#include "mozilla/ArrayUtils.h"  // mozilla::ArrayLength
#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"

#include <algorithm>
#include <string.h>

#include "jsapi.h"
#include "jstypes.h"

#include "builtin/Array.h"
#include "builtin/BigInt.h"
#include "builtin/Object.h"
#include "builtin/Symbol.h"
#include "frontend/BytecodeCompiler.h"  // frontend::{CompileStandaloneFunction, CompileStandaloneGenerator, CompileStandaloneAsyncFunction, CompileStandaloneAsyncGenerator, DelazifyCanonicalScriptedFunction}
#include "frontend/FrontendContext.h"  // AutoReportFrontendContext, ManualReportFrontendContext
#include "frontend/Stencil.h"  // js::DumpFunctionFlagsItems
#include "jit/InlinableNatives.h"
#include "jit/Ion.h"
#include "js/CallNonGenericMethod.h"
#include "js/CompilationAndEvaluation.h"
#include "js/CompileOptions.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/Printer.h"               // js::GenericPrinter
#include "js/PropertySpec.h"
#include "js/SourceText.h"
#include "js/StableStringChars.h"
#include "js/Wrapper.h"
#include "util/DifferentialTesting.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/BooleanObject.h"
#include "vm/BoundFunctionObject.h"
#include "vm/Compartment.h"
#include "vm/FunctionFlags.h"          // js::FunctionFlags
#include "vm/GeneratorAndAsyncKind.h"  // js::GeneratorKind, js::FunctionAsyncKind
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSAtomUtils.h"  // ToAtom
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/JSONPrinter.h"  // js::JSONPrinter
#include "vm/JSScript.h"
#include "vm/NumberObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/StringObject.h"
#include "wasm/AsmJS.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "vm/RecordType.h"
#  include "vm/TupleType.h"
#endif

#include "vm/Interpreter-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;

using mozilla::CheckedInt;
using mozilla::Maybe;
using mozilla::Some;

using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::SourceOwnership;
using JS::SourceText;

static bool fun_enumerate(JSContext* cx, HandleObject obj) {
  MOZ_ASSERT(obj->is<JSFunction>());

  RootedId id(cx);
  bool found;

  if (obj->as<JSFunction>().needsPrototypeProperty()) {
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
    if (fun->isBuiltin()) {
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

  // Function.arguments isn't standard (not even Annex B), so it isn't
  // worth the effort to guarantee that we can always recover it from
  // an Ion frame. Always return null for differential fuzzing.
  if (js::SupportDifferentialTesting()) {
    args.rval().setNull();
    return true;
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
    objProto = &global->getObjectPrototype();
  }
  if (!objProto) {
    return false;
  }

  Rooted<PlainObject*> proto(
      cx, NewPlainObjectWithProto(cx, objProto, TenuredObject));
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
    // Self-hosted constructors have a non-configurable .prototype data
    // property.
    if (!isConstructor()) {
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

bool JSFunction::getExplicitName(JSContext* cx,
                                 JS::MutableHandle<JSAtom*> name) {
  if (isAccessorWithLazyName()) {
    JSAtom* accessorName = getAccessorNameForLazy(cx);
    if (!accessorName) {
      return false;
    }

    name.set(accessorName);
    return true;
  }

  name.set(maybePartialExplicitName());
  return true;
}

bool JSFunction::getDisplayAtom(JSContext* cx,
                                JS::MutableHandle<JSAtom*> name) {
  if (isAccessorWithLazyName()) {
    JSAtom* accessorName = getAccessorNameForLazy(cx);
    if (!accessorName) {
      return false;
    }

    name.set(accessorName);
    return true;
  }

  name.set(maybePartialDisplayAtom());
  return true;
}

static JSAtom* NameToPrefixedFunctionName(JSContext* cx, JSString* nameStr,
                                          FunctionPrefixKind prefixKind) {
  MOZ_ASSERT(prefixKind != FunctionPrefixKind::None);

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

  return NameToPrefixedFunctionName(cx, nameStr, prefixKind);
}

JSAtom* JSFunction::getAccessorNameForLazy(JSContext* cx) {
  MOZ_ASSERT(isAccessorWithLazyName());

  JSAtom* name = NameToPrefixedFunctionName(
      cx, rawAtom(),
      isGetter() ? FunctionPrefixKind::Get : FunctionPrefixKind::Set);
  if (!name) {
    return nullptr;
  }

  setAtom(name);
  setFlags(flags().clearLazyAccessorName());
  return name;
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

      uint16_t len = 0;
      if (!JSFunction::getUnresolvedLength(cx, fun, &len)) {
        return false;
      }
      v.setInt32(len);
    } else {
      if (fun->hasResolvedName()) {
        return true;
      }

      JSAtom* name = fun->getUnresolvedName(cx);
      if (!name) {
        return false;
      }
      v.setString(name);
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
  if (obj->is<BoundFunctionObject>()) {
    /* Steps 2a-b. */
    AutoCheckRecursionLimit recursion(cx);
    if (!recursion.check(cx)) {
      return false;
    }
    obj = obj->as<BoundFunctionObject>().getTarget();
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
  // Functions can be be marked as interpreted despite having no script yet at
  // some points when parsing, and can be lazy with no lazy script for
  // self-hosted code.
  MOZ_ASSERT(!getFixedSlot(NativeJitInfoOrInterpretedScriptSlot).isGCThing());
  if (isInterpreted() && hasBaseScript()) {
    if (BaseScript* script = baseScript()) {
      TraceManuallyBarrieredEdge(trc, &script, "JSFunction script");
      // Self-hosted scripts are shared with workers but are never relocated.
      // Skip unnecessary writes to prevent the possible data race.
      if (baseScript() != script) {
        HeapSlot& slot = getFixedSlotRef(NativeJitInfoOrInterpretedScriptSlot);
        slot.unbarrieredSet(JS::PrivateValue(script));
      }
    }
  }
  // wasm/asm.js exported functions need to keep WasmInstantObject alive,
  // access it via WASM_INSTANCE_SLOT extended slot.
  if (isAsmJSNative() || isWasm()) {
    const Value& v = getExtendedSlot(FunctionExtended::WASM_INSTANCE_SLOT);
    if (!v.isUndefined()) {
      auto* instance = static_cast<wasm::Instance*>(v.toPrivate());
      wasm::TraceInstanceEdge(trc, instance, "JSFunction instance");
    }
  }
}

static void fun_trace(JSTracer* trc, JSObject* obj) {
  obj->as<JSFunction>().trace(trc);
}

static JSObject* CreateFunctionConstructor(JSContext* cx, JSProtoKey key) {
  Rooted<GlobalObject*> global(cx, cx->global());
  RootedObject functionProto(cx, &global->getPrototype(JSProto_Function));

  RootedObject functionCtor(
      cx, NewFunctionWithProto(
              cx, Function, 1, FunctionFlags::NATIVE_CTOR, nullptr,
              Handle<PropertyName*>(cx->names().Function), functionProto,
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

  RootedObject objectProto(cx, &self->getPrototype(JSProto_Object));

  return NewFunctionWithProto(
      cx, FunctionPrototype, 0, FunctionFlags::NATIVE_FUN, nullptr,
      Handle<PropertyName*>(cx->names().empty_), objectProto,
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
    if (fun->maybePartialExplicitName() &&
        (fun->kind() == FunctionFlags::NormalFunction ||
         (fun->isBuiltinNative() && (fun->kind() == FunctionFlags::Getter ||
                                     fun->kind() == FunctionFlags::Setter)) ||
         fun->kind() == FunctionFlags::Wasm ||
         fun->kind() == FunctionFlags::ClassConstructor)) {
      if (!out.append(' ')) {
        return nullptr;
      }

      // Built-in getters or setters are classified as one of
      // NormalFunction, Getter, or Setter. Strip any leading "get " or "set "
      // if present.
      JSAtom* name = fun->maybePartialExplicitName();
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

    JS::Rooted<JSAtom*> name(cx);
    if (!fun->getExplicitName(cx, &name)) {
      return nullptr;
    }

    if (name) {
      if (!out.append(' ')) {
        return nullptr;
      }
      if (!out.append(name)) {
        return nullptr;
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
                              JSMSG_INCOMPATIBLE_PROTO, "Function", "toString",
                              "object");
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
    ReportIncompatibleMethod(cx, args, &FunctionClass);
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

  return Call(cx, func, args.get(0), iargs, args.rval(), CallReason::FunCall);
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
    ReportIncompatibleMethod(cx, args, &FunctionClass);
    return false;
  }

  // Step 2.
  if (args.length() < 2 || args[1].isNullOrUndefined()) {
    return fun_call(cx, (args.length() > 0) ? 1 : 0, vp);
  }

  // Step 3.
  if (!args[1].isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_APPLY_ARGS, "apply");
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
  return Call(cx, fval, args[0], args2, args.rval(), CallReason::FunCall);
}

static const JSFunctionSpec function_methods[] = {
    JS_FN("toSource", fun_toSource, 0, 0),
    JS_FN("toString", fun_toString, 0, 0),
    JS_FN("apply", fun_apply, 2, 0),
    JS_FN("call", fun_call, 1, 0),
    JS_INLINABLE_FN("bind", BoundFunctionObject::functionBind, 1, 0,
                    FunctionBind),
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
    nullptr,         // construct
    fun_trace,       // trace
};

static const ClassSpec JSFunctionClassSpec = {
    CreateFunctionConstructor, CreateFunctionPrototype, nullptr, nullptr,
    function_methods,          function_properties};

const JSClass js::FunctionClass = {
    "Function",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Function) |
        JSCLASS_HAS_RESERVED_SLOTS(JSFunction::SlotCount),
    &JSFunctionClassOps, &JSFunctionClassSpec};

const JSClass js::ExtendedFunctionClass = {
    "Function",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Function) |
        JSCLASS_HAS_RESERVED_SLOTS(FunctionExtended::SlotCount),
    &JSFunctionClassOps, &JSFunctionClassSpec};

const JSClass* const js::FunctionClassPtr = &FunctionClass;
const JSClass* const js::FunctionExtendedClassPtr = &ExtendedFunctionClass;

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
  AutoReportFrontendContext fc(cx);
  if (!frontend::DelazifyCanonicalScriptedFunction(cx, &fc, fun)) {
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
  return cx->runtime()->delazifySelfHostedFunction(cx, funName, fun);
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
  uint32_t lineno;
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
      .setDeferDebugMetadata();

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
  static_assert(FunctionConstructorMedialSigils[0] == ')');

  if (!sb.append(FunctionConstructorMedialSigils.data(),
                 FunctionConstructorMedialSigils.length())) {
    return false;
  }

  if (args.length() > 0) {
    // Steps 13, 14.e, 15.
    RootedString body(cx, ToString<CanGC>(cx, args[args.length() - 1]));
    if (!body || !sb.append(body)) {
      return false;
    }
  }

  if (!sb.append(FunctionConstructorFinalBrace.data(),
                 FunctionConstructorFinalBrace.length())) {
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
  if (!cx->isRuntimeCodeGenEnabled(JS::RuntimeCode::JS, functionText)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_FUNCTION);
    return false;
  }

  // Steps 7.a-b, 8.a-b, 9.a-b, 16-28.
  AutoStableStringChars linearChars(cx);
  if (!linearChars.initTwoByte(cx, functionText)) {
    return false;
  }

  SourceText<char16_t> srcBuf;
  if (!srcBuf.initMaybeBorrowed(cx, linearChars)) {
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
  JS::InstantiateOptions instantiateOptions(options);
  if (funScript &&
      !UpdateDebugMetadata(cx, funScript, instantiateOptions, undefValue,
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

#if defined(DEBUG) || defined(JS_JITSPEW)
void JSFunction::dumpOwnFields(js::JSONPrinter& json) const {
  if (maybePartialDisplayAtom()) {
    js::GenericPrinter& out =
        json.beginStringProperty("maybePartialDisplayAtom");
    maybePartialDisplayAtom()->dumpPropertyName(out);
    json.endStringProperty();
  }

  if (hasBaseScript()) {
    js::GenericPrinter& out = json.beginStringProperty("baseScript");
    baseScript()->dumpStringContent(out);
    json.endStringProperty();
  }

  json.property("nargs", nargs());

  json.beginInlineListProperty("flags");
  DumpFunctionFlagsItems(json, flags());
  json.endInlineList();

  if (isNativeFun()) {
    json.formatProperty("native", "0x%p", native());
    if (hasJitInfo()) {
      json.formatProperty("jitInfo", "0x%p", jitInfo());
    }
  }
}

void JSFunction::dumpOwnStringContent(js::GenericPrinter& out) const {
  if (maybePartialDisplayAtom() && maybePartialDisplayAtom()->length() > 0) {
    maybePartialDisplayAtom()->dumpPropertyName(out);
  } else {
    out.put("(anonymous)");
  }

  if (hasBaseScript()) {
    out.put(" (");
    baseScript()->dumpStringContent(out);
    out.put(")");
  }
}
#endif

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

static inline const JSClass* FunctionClassForAllocKind(
    gc::AllocKind allocKind) {
  return (allocKind == gc::AllocKind::FUNCTION) ? FunctionClassPtr
                                                : FunctionExtendedClassPtr;
}

static void AssertClassMatchesAllocKind(const JSClass* clasp,
                                        gc::AllocKind kind) {
#ifdef DEBUG
  if (kind == gc::AllocKind::FUNCTION_EXTENDED) {
    MOZ_ASSERT(clasp == FunctionExtendedClassPtr);
  } else {
    MOZ_ASSERT(kind == gc::AllocKind::FUNCTION);
    MOZ_ASSERT(clasp == FunctionClassPtr);
  }
#endif
}

static SharedShape* GetFunctionShape(JSContext* cx, const JSClass* clasp,
                                     JSObject* proto, gc::AllocKind allocKind) {
  AssertClassMatchesAllocKind(clasp, allocKind);

  size_t nfixed = GetGCKindSlots(allocKind);
  return SharedShape::getInitialShape(
      cx, clasp, cx->realm(), TaggedProto(proto), nfixed, ObjectFlags());
}

SharedShape* GlobalObject::createFunctionShapeWithDefaultProto(JSContext* cx,
                                                               bool extended) {
  GlobalObjectData& data = cx->global()->data();
  HeapPtr<SharedShape*>& shapeRef =
      extended ? data.extendedFunctionShapeWithDefaultProto
               : data.functionShapeWithDefaultProto;
  MOZ_ASSERT(!shapeRef);

  RootedObject proto(cx,
                     GlobalObject::getOrCreatePrototype(cx, JSProto_Function));
  if (!proto) {
    return nullptr;
  }

  // Creating %Function.prototype% can end up initializing the shape.
  if (shapeRef) {
    return shapeRef;
  }

  gc::AllocKind allocKind =
      extended ? gc::AllocKind::FUNCTION_EXTENDED : gc::AllocKind::FUNCTION;
  const JSClass* clasp = FunctionClassForAllocKind(allocKind);

  SharedShape* shape = GetFunctionShape(cx, clasp, proto, allocKind);
  if (!shape) {
    return nullptr;
  }

  shapeRef.init(shape);
  return shape;
}

JSFunction* js::NewFunctionWithProto(
    JSContext* cx, Native native, unsigned nargs, FunctionFlags flags,
    HandleObject enclosingEnv, Handle<JSAtom*> atom, HandleObject proto,
    gc::AllocKind allocKind /* = AllocKind::FUNCTION */,
    NewObjectKind newKind /* = GenericObject */) {
  MOZ_ASSERT(allocKind == gc::AllocKind::FUNCTION ||
             allocKind == gc::AllocKind::FUNCTION_EXTENDED);
  MOZ_ASSERT_IF(native, !enclosingEnv);
  MOZ_ASSERT(NewFunctionEnvironmentIsWellFormed(cx, enclosingEnv));

  // NOTE: Keep this in sync with `CreateFunctionFast` in Stencil.cpp

  const JSClass* clasp = FunctionClassForAllocKind(allocKind);

  Rooted<SharedShape*> shape(cx);
  if (!proto) {
    bool extended = (allocKind == gc::AllocKind::FUNCTION_EXTENDED);
    shape = GlobalObject::getFunctionShapeWithDefaultProto(cx, extended);
  } else {
    shape = GetFunctionShape(cx, clasp, proto, allocKind);
  }
  if (!shape) {
    return nullptr;
  }

  gc::Heap heap = GetInitialHeap(newKind, clasp);
  JSFunction* fun = JSFunction::create(cx, allocKind, heap, shape);
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
  fun->initAtom(atom);

#ifdef DEBUG
  fun->assertFunctionKindIntegrity();
#endif

  return fun;
}

bool js::GetFunctionPrototype(JSContext* cx, js::GeneratorKind generatorKind,
                              js::FunctionAsyncKind asyncKind,
                              js::MutableHandleObject proto) {
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

#ifdef DEBUG
static bool CanReuseScriptForClone(JS::Realm* realm, HandleFunction fun,
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
#endif

static inline JSFunction* NewFunctionClone(JSContext* cx, HandleFunction fun,
                                           HandleObject proto) {
  MOZ_ASSERT(cx->realm() == fun->realm());
  MOZ_ASSERT(proto);

  const JSClass* clasp = fun->getClass();
  gc::AllocKind allocKind = fun->getAllocKind();
  AssertClassMatchesAllocKind(clasp, allocKind);

  // If |fun| also has |proto| as prototype (the common case) we can reuse its
  // shape for the clone. This works because |fun| isn't exposed to script.
  Rooted<SharedShape*> shape(cx);
  if (fun->staticPrototype() == proto) {
    shape = fun->sharedShape();
    MOZ_ASSERT(shape->propMapLength() == 0);
    MOZ_ASSERT(shape->objectFlags().isEmpty());
    MOZ_ASSERT(shape->realm() == cx->realm());
  } else {
    shape = GetFunctionShape(cx, clasp, proto, allocKind);
    if (!shape) {
      return nullptr;
    }
  }

  JSFunction* clone =
      JSFunction::create(cx, allocKind, gc::Heap::Default, shape);
  if (!clone) {
    return nullptr;
  }

  constexpr uint16_t NonCloneableFlags =
      FunctionFlags::RESOLVED_LENGTH | FunctionFlags::RESOLVED_NAME;

  FunctionFlags flags = fun->flags();
  flags.clearFlags(NonCloneableFlags);

  clone->setArgCount(fun->nargs());
  clone->setFlags(flags);

  // Note: |clone| and |fun| are same-zone so we don't need to call markAtom.
  clone->initAtom(fun->maybePartialDisplayAtom());

#ifdef DEBUG
  clone->assertFunctionKindIntegrity();
#endif

  return clone;
}

JSFunction* js::CloneFunctionReuseScript(JSContext* cx, HandleFunction fun,
                                         HandleObject enclosingEnv,
                                         HandleObject proto) {
  MOZ_ASSERT(cx->realm() == fun->realm());
  MOZ_ASSERT(NewFunctionEnvironmentIsWellFormed(cx, enclosingEnv));
  MOZ_ASSERT(fun->isInterpreted());
  MOZ_ASSERT(fun->hasBaseScript());
  MOZ_ASSERT(CanReuseScriptForClone(cx->realm(), fun, enclosingEnv));

  JSFunction* clone = NewFunctionClone(cx, fun, proto);
  if (!clone) {
    return nullptr;
  }

  BaseScript* base = fun->baseScript();
  clone->initScript(base);
  clone->initEnvironment(enclosingEnv);

#ifdef DEBUG
  // Assert extended slots don't need to be copied.
  if (fun->isExtended()) {
    for (unsigned i = 0; i < FunctionExtended::NUM_EXTENDED_SLOTS; i++) {
      MOZ_ASSERT(fun->getExtendedSlot(i).isUndefined());
      MOZ_ASSERT(clone->getExtendedSlot(i).isUndefined());
    }
  }
#endif

  return clone;
}

JSFunction* js::CloneAsmJSModuleFunction(JSContext* cx, HandleFunction fun) {
  MOZ_ASSERT(fun->isNativeFun());
  MOZ_ASSERT(IsAsmJSModule(fun));
  MOZ_ASSERT(fun->isExtended());
  MOZ_ASSERT(cx->compartment() == fun->compartment());

  RootedObject proto(cx, fun->staticPrototype());
  JSFunction* clone = NewFunctionClone(cx, fun, proto);
  if (!clone) {
    return nullptr;
  }

  MOZ_ASSERT(fun->native() == InstantiateAsmJS);
  MOZ_ASSERT(!fun->hasJitInfo());
  clone->initNative(InstantiateAsmJS, nullptr);

  JSObject* moduleObj =
      &fun->getExtendedSlot(FunctionExtended::ASMJS_MODULE_SLOT).toObject();
  clone->initExtendedSlot(FunctionExtended::ASMJS_MODULE_SLOT,
                          ObjectValue(*moduleObj));

  return clone;
}

static JSAtom* SymbolToFunctionName(JSContext* cx, JS::Symbol* symbol,
                                    FunctionPrefixKind prefixKind) {
  // Step 4.a.
  JSAtom* desc = symbol->description();

  // Step 4.b, no prefix fastpath.
  if (!desc && prefixKind == FunctionPrefixKind::None) {
    return cx->names().empty_;
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
  Rooted<JSAtom*> atom(cx, IdToFunctionName(cx, id));
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
#  ifdef ENABLE_RECORD_TUPLE
    case ValueType::ExtendedPrimitive:
      MOZ_CRASH("ExtendedPrimitive is not supported yet");
      break;
#  endif
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
