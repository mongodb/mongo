/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Call and construct API. */

#ifndef js_CallAndConstruct_h
#define js_CallAndConstruct_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::Handle, JS::MutableHandle
#include "js/Value.h"       // JS::Value, JS::ObjectValue
#include "js/ValueArray.h"  // JS::HandleValueArray

struct JSContext;
class JSObject;
class JSFunction;

/*
 * API for determining callability and constructability. [[Call]] and
 * [[Construct]] are internal methods that aren't present on all objects, so it
 * is useful to ask if they are there or not. The standard itself asks these
 * questions routinely.
 */
namespace JS {

/**
 * Return true if the given object is callable. In ES6 terms, an object is
 * callable if it has a [[Call]] internal method.
 *
 * Implements: ES6 7.2.3 IsCallable(argument).
 *
 * Functions are callable. A scripted proxy or wrapper is callable if its
 * target is callable. Most other objects aren't callable.
 */
extern JS_PUBLIC_API bool IsCallable(JSObject* obj);

/**
 * Return true if the given object is a constructor. In ES6 terms, an object is
 * a constructor if it has a [[Construct]] internal method. The expression
 * `new obj()` throws a TypeError if obj is not a constructor.
 *
 * Implements: ES6 7.2.4 IsConstructor(argument).
 *
 * JS functions and classes are constructors. Arrow functions and most builtin
 * functions are not. A scripted proxy or wrapper is a constructor if its
 * target is a constructor.
 */
extern JS_PUBLIC_API bool IsConstructor(JSObject* obj);

} /* namespace JS */

/**
 * Call a function, passing a this-value and arguments. This is the C++
 * equivalent of `rval = Reflect.apply(fun, obj, args)`.
 *
 * Implements: ES6 7.3.12 Call(F, V, [argumentsList]).
 * Use this function to invoke the [[Call]] internal method.
 */
extern JS_PUBLIC_API bool JS_CallFunctionValue(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<JS::Value> fval,
    const JS::HandleValueArray& args, JS::MutableHandle<JS::Value> rval);

extern JS_PUBLIC_API bool JS_CallFunction(JSContext* cx,
                                          JS::Handle<JSObject*> obj,
                                          JS::Handle<JSFunction*> fun,
                                          const JS::HandleValueArray& args,
                                          JS::MutableHandle<JS::Value> rval);

/**
 * Perform the method call `rval = obj[name](args)`.
 */
extern JS_PUBLIC_API bool JS_CallFunctionName(
    JSContext* cx, JS::Handle<JSObject*> obj, const char* name,
    const JS::HandleValueArray& args, JS::MutableHandle<JS::Value> rval);

namespace JS {

static inline bool Call(JSContext* cx, Handle<JSObject*> thisObj,
                        Handle<JSFunction*> fun, const HandleValueArray& args,
                        MutableHandle<Value> rval) {
  return !!JS_CallFunction(cx, thisObj, fun, args, rval);
}

static inline bool Call(JSContext* cx, Handle<JSObject*> thisObj,
                        Handle<Value> fun, const HandleValueArray& args,
                        MutableHandle<Value> rval) {
  return !!JS_CallFunctionValue(cx, thisObj, fun, args, rval);
}

static inline bool Call(JSContext* cx, Handle<JSObject*> thisObj,
                        const char* name, const HandleValueArray& args,
                        MutableHandle<Value> rval) {
  return !!JS_CallFunctionName(cx, thisObj, name, args, rval);
}

extern JS_PUBLIC_API bool Call(JSContext* cx, Handle<Value> thisv,
                               Handle<Value> fun, const HandleValueArray& args,
                               MutableHandle<Value> rval);

static inline bool Call(JSContext* cx, Handle<Value> thisv,
                        Handle<JSObject*> funObj, const HandleValueArray& args,
                        MutableHandle<Value> rval) {
  MOZ_ASSERT(funObj);
  Rooted<Value> fun(cx, ObjectValue(*funObj));
  return Call(cx, thisv, fun, args, rval);
}

/**
 * Invoke a constructor. This is the C++ equivalent of
 * `rval = Reflect.construct(fun, args, newTarget)`.
 *
 * Construct() takes a `newTarget` argument that most callers don't need.
 * Consider using the four-argument Construct signature instead. (But if you're
 * implementing a subclass or a proxy handler's construct() method, this is the
 * right function to call.)
 *
 * Implements: ES6 7.3.13 Construct(F, [argumentsList], [newTarget]).
 * Use this function to invoke the [[Construct]] internal method.
 */
extern JS_PUBLIC_API bool Construct(JSContext* cx, Handle<Value> fun,
                                    Handle<JSObject*> newTarget,
                                    const HandleValueArray& args,
                                    MutableHandle<JSObject*> objp);

/**
 * Invoke a constructor. This is the C++ equivalent of
 * `rval = new fun(...args)`.
 *
 * Implements: ES6 7.3.13 Construct(F, [argumentsList], [newTarget]), when
 * newTarget is omitted.
 */
extern JS_PUBLIC_API bool Construct(JSContext* cx, Handle<Value> fun,
                                    const HandleValueArray& args,
                                    MutableHandle<JSObject*> objp);

} /* namespace JS */

#endif /* js_CallAndConstruct_h */
