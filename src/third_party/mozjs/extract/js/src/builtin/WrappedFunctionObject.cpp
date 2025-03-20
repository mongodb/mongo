/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/WrappedFunctionObject.h"

#include <string_view>

#include "jsapi.h"

#include "builtin/ShadowRealm.h"
#include "js/CallAndConstruct.h"
#include "js/Class.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "util/StringBuffer.h"
#include "vm/Compartment.h"
#include "vm/Interpreter.h"
#include "vm/JSFunction.h"
#include "vm/ObjectOperations.h"

#include "vm/JSFunction-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Realm-inl.h"

using namespace js;
using namespace JS;

// GetWrappedValue ( callerRealm: a Realm Record, value: unknown )
bool js::GetWrappedValue(JSContext* cx, Realm* callerRealm, Handle<Value> value,
                         MutableHandle<Value> res) {
  cx->check(value);

  // Step 2. Return value (Reordered)
  if (!value.isObject()) {
    res.set(value);
    return true;
  }

  // Step 1. If Type(value) is Object, then
  //      a. If IsCallable(value) is false, throw a TypeError exception.
  Rooted<JSObject*> objectVal(cx, &value.toObject());
  if (!IsCallable(objectVal)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SHADOW_REALM_INVALID_RETURN);
    return false;
  }

  //     b. Return ? WrappedFunctionCreate(callerRealm, value).
  return WrappedFunctionCreate(cx, callerRealm, objectVal, res);
}

// [[Call]]
// https://tc39.es/proposal-shadowrealm/#sec-wrapped-function-exotic-objects-call-thisargument-argumentslist
// https://tc39.es/proposal-shadowrealm/#sec-ordinary-wrapped-function-call
static bool WrappedFunction_Call(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<JSObject*> callee(cx, &args.callee());
  MOZ_ASSERT(callee->is<WrappedFunctionObject>());

  Handle<WrappedFunctionObject*> fun = callee.as<WrappedFunctionObject>();

  // PrepareForWrappedFunctionCall is a no-op in our implementation, because
  // we've already entered the correct realm.
  MOZ_ASSERT(cx->realm() == fun->realm());

  // The next steps refer to the OrdinaryWrappedFunctionCall operation.

  // 1. Let target be F.[[WrappedTargetFunction]].
  Rooted<JSObject*> target(cx, fun->getTargetFunction());

  // 2. Assert: IsCallable(target) is true.
  MOZ_ASSERT(IsCallable(ObjectValue(*target)));

  // 3. Let callerRealm be F.[[Realm]].
  Rooted<Realm*> callerRealm(cx, fun->realm());

  // 4. NOTE: Any exception objects produced after this point are associated
  //    with callerRealm.
  //
  // Implicit in our implementation, because |callerRealm| is already the
  // current realm.

  // 5. Let targetRealm be ? GetFunctionRealm(target).
  Rooted<Realm*> targetRealm(cx, GetFunctionRealm(cx, target));
  if (!targetRealm) {
    return false;
  }

  // 6. Let wrappedArgs be a new empty List.
  InvokeArgs wrappedArgs(cx);
  if (!wrappedArgs.init(cx, args.length())) {
    return false;
  }

  // 7. For each element arg of argumentsList, do
  //     a. Let wrappedValue be ? GetWrappedValue(targetRealm, arg).
  //     b. Append wrappedValue to wrappedArgs.
  Rooted<Value> element(cx);
  for (size_t i = 0; i < args.length(); i++) {
    element = args.get(i);
    if (!GetWrappedValue(cx, targetRealm, element, &element)) {
      return false;
    }

    wrappedArgs[i].set(element);
  }

  // 8. Let wrappedThisArgument to ? GetWrappedValue(targetRealm,
  // thisArgument).
  Rooted<Value> wrappedThisArgument(cx);
  if (!GetWrappedValue(cx, targetRealm, args.thisv(), &wrappedThisArgument)) {
    return false;
  }

  // 9. Let result be the Completion Record of Call(target,
  //    wrappedThisArgument, wrappedArgs).
  Rooted<Value> targetValue(cx, ObjectValue(*target));
  Rooted<Value> result(cx);
  if (!js::Call(cx, targetValue, wrappedThisArgument, wrappedArgs, &result)) {
    // 11. Else (reordered);
    //     a. Throw a TypeError exception.
    ReportPotentiallyDetailedMessage(
        cx, JSMSG_SHADOW_REALM_WRAPPED_EXECUTION_FAILURE_DETAIL,
        JSMSG_SHADOW_REALM_WRAPPED_EXECUTION_FAILURE);
    return false;
  }

  // 10. If result.[[Type]] is normal or result.[[Type]] is return, then
  //     a. Return ? GetWrappedValue(callerRealm, result.[[Value]]).
  if (!GetWrappedValue(cx, callerRealm, result, args.rval())) {
    return false;
  }

  return true;
}

static bool CopyNameAndLength(JSContext* cx, HandleObject fun,
                              HandleObject target) {
  // 1. If argCount is undefined, then set argCount to 0 (implicit)
  constexpr int32_t argCount = 0;

  // 2. Let L be 0.
  double length = 0;

  // 3. Let targetHasLength be ? HasOwnProperty(Target, "length").
  //
  // Try to avoid invoking the resolve hook.
  // Also see ComputeLengthValue in BoundFunctionObject.cpp.
  if (target->is<JSFunction>() &&
      !target->as<JSFunction>().hasResolvedLength()) {
    uint16_t targetLen;
    if (!JSFunction::getUnresolvedLength(cx, target.as<JSFunction>(),
                                         &targetLen)) {
      return false;
    }

    length = std::max(0.0, double(targetLen) - argCount);
  } else {
    Rooted<jsid> lengthId(cx, NameToId(cx->names().length));

    bool targetHasLength;
    if (!HasOwnProperty(cx, target, lengthId, &targetHasLength)) {
      return false;
    }

    // 4. If targetHasLength is true, then
    if (targetHasLength) {
      //     a. Let targetLen be ? Get(Target, "length").
      Rooted<Value> targetLen(cx);
      if (!GetProperty(cx, target, target, lengthId, &targetLen)) {
        return false;
      }

      //     b. If Type(targetLen) is Number, then
      //         i. If targetLen is +∞𝔽, set L to +∞.
      //         ii. Else if targetLen is -∞𝔽, set L to 0.
      //         iii. Else,
      //             1. Let targetLenAsInt be ! ToIntegerOrInfinity(targetLen).
      //             2. Assert: targetLenAsInt is finite.
      //             3. Set L to max(targetLenAsInt - argCount, 0).
      if (targetLen.isNumber()) {
        length = std::max(0.0, JS::ToInteger(targetLen.toNumber()) - argCount);
      }
    }
  }

  // 5. Perform ! SetFunctionLength(F, L).
  Rooted<Value> rootedLength(cx, NumberValue(length));
  if (!DefineDataProperty(cx, fun, cx->names().length, rootedLength,
                          JSPROP_READONLY)) {
    return false;
  }

  // 6. Let targetName be ? Get(Target, "name").
  //
  // Try to avoid invoking the resolve hook.
  Rooted<Value> targetName(cx);
  if (target->is<JSFunction>() && !target->as<JSFunction>().hasResolvedName()) {
    JSFunction* targetFun = &target->as<JSFunction>();
    JSString* targetNameStr = targetFun->getUnresolvedName(cx);
    if (!targetNameStr) {
      return false;
    }
    targetName.setString(targetNameStr);
  } else {
    if (!GetProperty(cx, target, target, cx->names().name, &targetName)) {
      return false;
    }
  }

  // 7. If Type(targetName) is not String, set targetName to the empty String.
  if (!targetName.isString()) {
    targetName = StringValue(cx->runtime()->emptyString);
  }

  // 8. Perform ! SetFunctionName(F, targetName, prefix).
  return DefineDataProperty(cx, fun, cx->names().name, targetName,
                            JSPROP_READONLY);
}

static JSString* ToStringOp(JSContext* cx, JS::HandleObject obj,
                            bool isToSource) {
  // Return an unnamed native function to match the behavior of bound
  // functions.
  //
  // NOTE: The current value of the "name" property can be any value, it's not
  // necessarily a string value. It can also be an accessor property which could
  // lead to executing side-effects, which isn't allowed per the spec, cf.
  // <https://tc39.es/ecma262/#sec-function.prototype.tostring>. Even if it's a
  // data property with a string value, we'd still need to validate the string
  // can be parsed as a |PropertyName| production before using it as part of the
  // output.
  constexpr std::string_view nativeCode = "function () {\n    [native code]\n}";

  return NewStringCopy<CanGC>(cx, nativeCode);
}

static const JSClassOps classOps = {
    nullptr,               // addProperty
    nullptr,               // delProperty
    nullptr,               // enumerate
    nullptr,               // newEnumerate
    nullptr,               // resolve
    nullptr,               // mayResolve
    nullptr,               // finalize
    WrappedFunction_Call,  // call
    nullptr,               // construct
    nullptr,               // trace
};

static const ObjectOps objOps = {
    nullptr,     // lookupProperty
    nullptr,     // defineProperty
    nullptr,     // hasProperty
    nullptr,     // getProperty
    nullptr,     // setProperty
    nullptr,     // getOwnPropertyDescriptor
    nullptr,     // deleteProperty
    nullptr,     // getElements
    ToStringOp,  // funToString
};

const JSClass WrappedFunctionObject::class_ = {
    "WrappedFunctionObject",
    JSCLASS_HAS_CACHED_PROTO(
        JSProto_Function) |  // This sets the prototype to Function.prototype,
                             // Step 3 of WrappedFunctionCreate
        JSCLASS_HAS_RESERVED_SLOTS(WrappedFunctionObject::SlotCount),
    &classOps,
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    &objOps,
};

// WrappedFunctionCreate ( callerRealm: a Realm Record, Target: a function
// object)
bool js::WrappedFunctionCreate(JSContext* cx, Realm* callerRealm,
                               HandleObject target, MutableHandle<Value> res) {
  cx->check(target);

  WrappedFunctionObject* wrapped = nullptr;
  {
    // Ensure that the function object has the correct realm by allocating it
    // into that realm.
    Rooted<JSObject*> global(cx, callerRealm->maybeGlobal());
    MOZ_RELEASE_ASSERT(
        global, "global is null; executing in a realm that's being GC'd?");
    AutoRealm ar(cx, global);

    MOZ_ASSERT(target);

    // Target *could* be a function from another compartment.
    Rooted<JSObject*> maybeWrappedTarget(cx, target);
    if (!cx->compartment()->wrap(cx, &maybeWrappedTarget)) {
      return false;
    }

    // 1. Let internalSlotsList be the internal slots listed in Table 2, plus
    // [[Prototype]] and [[Extensible]].
    // 2. Let wrapped be ! MakeBasicObject(internalSlotsList).
    // 3. Set wrapped.[[Prototype]] to
    //    callerRealm.[[Intrinsics]].[[%Function.prototype%]].
    wrapped = NewBuiltinClassInstance<WrappedFunctionObject>(cx);
    if (!wrapped) {
      return false;
    }

    // 4. Set wrapped.[[Call]] as described in 2.1 (implicit in JSClass call
    // hook)
    // 5. Set wrapped.[[WrappedTargetFunction]] to Target.
    wrapped->setTargetFunction(*maybeWrappedTarget);
    // 6. Set wrapped.[[Realm]] to callerRealm. (implicitly the realm of
    //    wrapped, which we assured with the AutoRealm

    MOZ_ASSERT(wrapped->realm() == callerRealm);
  }

  // Wrap |wrapped| to the current compartment.
  RootedObject obj(cx, wrapped);
  if (!cx->compartment()->wrap(cx, &obj)) {
    return false;
  }

  // 7. Let result be CopyNameAndLength(wrapped, Target).
  if (!CopyNameAndLength(cx, obj, target)) {
    // 8. If result is an Abrupt Completion, throw a TypeError exception.
    cx->clearPendingException();

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SHADOW_REALM_WRAP_FAILURE);
    return false;
  }

  // 9. Return wrapped.
  res.set(ObjectValue(*obj));
  return true;
}
