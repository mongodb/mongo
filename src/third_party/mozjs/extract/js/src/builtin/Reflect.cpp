/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Reflect.h"

#include "jsapi.h"

#include "builtin/Object.h"
#include "jit/InlinableNatives.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_NOT_EXPECTED_TYPE
#include "js/PropertySpec.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"

#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;

/*** Reflect methods ********************************************************/

/* ES6 26.1.4 Reflect.deleteProperty (target, propertyKey) */
static bool Reflect_deleteProperty(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject target(
      cx,
      RequireObjectArg(cx, "`target`", "Reflect.deleteProperty", args.get(0)));
  if (!target) {
    return false;
  }

  // Steps 2-3.
  RootedValue propertyKey(cx, args.get(1));
  RootedId key(cx);
  if (!ToPropertyKey(cx, propertyKey, &key)) {
    return false;
  }

  // Step 4.
  ObjectOpResult result;
  if (!DeleteProperty(cx, target, key, result)) {
    return false;
  }
  args.rval().setBoolean(result.ok());
  return true;
}

/* ES6 26.1.8 Reflect.getPrototypeOf(target) */
bool js::Reflect_getPrototypeOf(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject target(
      cx,
      RequireObjectArg(cx, "`target`", "Reflect.getPrototypeOf", args.get(0)));
  if (!target) {
    return false;
  }

  // Step 2.
  RootedObject proto(cx);
  if (!GetPrototype(cx, target, &proto)) {
    return false;
  }
  args.rval().setObjectOrNull(proto);
  return true;
}

/* ES6 draft 26.1.10 Reflect.isExtensible(target) */
bool js::Reflect_isExtensible(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject target(
      cx,
      RequireObjectArg(cx, "`target`", "Reflect.isExtensible", args.get(0)));
  if (!target) {
    return false;
  }

  // Step 2.
  bool extensible;
  if (!IsExtensible(cx, target, &extensible)) {
    return false;
  }
  args.rval().setBoolean(extensible);
  return true;
}

// ES2018 draft rev c164be80f7ea91de5526b33d54e5c9321ed03d3f
// 26.1.10 Reflect.ownKeys ( target )
bool js::Reflect_ownKeys(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Reflect", "ownKeys");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject target(
      cx, RequireObjectArg(cx, "`target`", "Reflect.ownKeys", args.get(0)));
  if (!target) {
    return false;
  }

  // Steps 2-3.
  return GetOwnPropertyKeys(
      cx, target, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, args.rval());
}

/* ES6 26.1.12 Reflect.preventExtensions(target) */
static bool Reflect_preventExtensions(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject target(
      cx, RequireObjectArg(cx, "`target`", "Reflect.preventExtensions",
                           args.get(0)));
  if (!target) {
    return false;
  }

  // Step 2.
  ObjectOpResult result;
  if (!PreventExtensions(cx, target, result)) {
    return false;
  }
  args.rval().setBoolean(result.ok());
  return true;
}

/* ES6 26.1.13 Reflect.set(target, propertyKey, V [, receiver]) */
static bool Reflect_set(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject target(
      cx, RequireObjectArg(cx, "`target`", "Reflect.set", args.get(0)));
  if (!target) {
    return false;
  }

  // Steps 2-3.
  RootedValue propertyKey(cx, args.get(1));
  RootedId key(cx);
  if (!ToPropertyKey(cx, propertyKey, &key)) {
    return false;
  }

  // Step 4.
  RootedValue receiver(cx, args.length() > 3 ? args[3] : args.get(0));

  // Step 5.
  ObjectOpResult result;
  RootedValue value(cx, args.get(2));
  if (!SetProperty(cx, target, key, value, receiver, result)) {
    return false;
  }
  args.rval().setBoolean(result.ok());
  return true;
}

/*
 * ES6 26.1.3 Reflect.setPrototypeOf(target, proto)
 *
 * The specification is not quite similar enough to Object.setPrototypeOf to
 * share code.
 */
static bool Reflect_setPrototypeOf(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, RequireObjectArg(cx, "`target`",
                                        "Reflect.setPrototypeOf", args.get(0)));
  if (!obj) {
    return false;
  }

  // Step 2.
  if (!args.get(1).isObjectOrNull()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, "Reflect.setPrototypeOf",
                              "an object or null",
                              InformalValueTypeName(args.get(1)));
    return false;
  }
  RootedObject proto(cx, args.get(1).toObjectOrNull());

  // Step 4.
  ObjectOpResult result;
  if (!SetPrototype(cx, obj, proto, result)) {
    return false;
  }
  args.rval().setBoolean(result.ok());
  return true;
}

static const JSFunctionSpec reflect_methods[] = {
    JS_SELF_HOSTED_FN("apply", "Reflect_apply", 3, 0),
    JS_SELF_HOSTED_FN("construct", "Reflect_construct", 2, 0),
    JS_SELF_HOSTED_FN("defineProperty", "Reflect_defineProperty", 3, 0),
    JS_FN("deleteProperty", Reflect_deleteProperty, 2, 0),
    JS_SELF_HOSTED_FN("get", "Reflect_get", 2, 0),
    JS_SELF_HOSTED_FN("getOwnPropertyDescriptor",
                      "Reflect_getOwnPropertyDescriptor", 2, 0),
    JS_INLINABLE_FN("getPrototypeOf", Reflect_getPrototypeOf, 1, 0,
                    ReflectGetPrototypeOf),
    JS_SELF_HOSTED_FN("has", "Reflect_has", 2, 0),
    JS_FN("isExtensible", Reflect_isExtensible, 1, 0),
    JS_FN("ownKeys", Reflect_ownKeys, 1, 0),
    JS_FN("preventExtensions", Reflect_preventExtensions, 1, 0),
    JS_FN("set", Reflect_set, 3, 0),
    JS_FN("setPrototypeOf", Reflect_setPrototypeOf, 2, 0),
    JS_FS_END,
};

static const JSPropertySpec reflect_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Reflect", JSPROP_READONLY),
    JS_PS_END,
};

/*** Setup ******************************************************************/

static JSObject* CreateReflectObject(JSContext* cx, JSProtoKey key) {
  RootedObject proto(cx, &cx->global()->getObjectPrototype());
  return NewPlainObjectWithProto(cx, proto, TenuredObject);
}

static const ClassSpec ReflectClassSpec = {
    CreateReflectObject,
    nullptr,
    reflect_methods,
    reflect_properties,
};

const JSClass js::ReflectClass = {
    "Reflect",
    0,
    JS_NULL_CLASS_OPS,
    &ReflectClassSpec,
};
