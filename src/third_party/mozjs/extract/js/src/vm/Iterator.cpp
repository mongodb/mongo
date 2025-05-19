/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Iterator.h"

#include "js/Conversions.h"
#include "vm/Interpreter.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/ObjectOperations.h"
#include "vm/SelfHosting.h"
#include "vm/StringType.h"
#include "vm/JSContext-inl.h"  // JSContext::check
#include "vm/JSObject-inl.h"

using namespace js;

namespace JS {

// https://tc39.es/ecma262/#sec-getiterator
// GetIterator(obj [, hint [, method]])
JSObject* GetIteratorObject(JSContext* cx, HandleValue obj, bool isAsync) {
  cx->check(obj);

  FixedInvokeArgs<3> args(cx);
  args[0].set(obj);
  args[1].setBoolean(isAsync);
  args[2].setUndefined();  // method

  RootedValue rval(cx);
  if (!CallSelfHostedFunction(cx, cx->names().GetIterator, UndefinedHandleValue,
                              args, &rval)) {
    return nullptr;
  }

  MOZ_ASSERT(rval.isObject());
  return &rval.toObject();
}

// https://tc39.es/ecma262/#sec-iteratornext
bool IteratorNext(JSContext* cx, HandleObject iteratorRecord,
                  MutableHandleValue result) {
  cx->check(iteratorRecord);

  FixedInvokeArgs<1> args(cx);
  args[0].setObject(*iteratorRecord);
  return CallSelfHostedFunction(cx, cx->names().IteratorNext,
                                UndefinedHandleValue, args, result);
}

// https://tc39.es/ecma262/#sec-iteratorcomplete
bool IteratorComplete(JSContext* cx, HandleObject iterResult, bool* done) {
  cx->check(iterResult);

  RootedValue doneV(cx);
  if (!GetProperty(cx, iterResult, iterResult, cx->names().done, &doneV)) {
    return false;
  }

  *done = ToBoolean(doneV);
  return true;
}

// https://tc39.es/ecma262/#sec-iteratorvalue
bool IteratorValue(JSContext* cx, HandleObject iterResult,
                   MutableHandleValue value) {
  cx->check(iterResult);
  return GetProperty(cx, iterResult, iterResult, cx->names().value, value);
}

bool GetIteratorRecordIterator(JSContext* cx, HandleObject iteratorRecord,
                               MutableHandleValue iterator) {
  cx->check(iteratorRecord);
  return GetProperty(cx, iteratorRecord, iteratorRecord, cx->names().iterator,
                     iterator);
}

// https://tc39.es/ecma262/#sec-getmethod
static bool GetMethod(JSContext* cx, HandleValue v, Handle<PropertyName*> name,
                      MutableHandleValue result) {
  // Step 1. Let func be ? GetV(V, P).
  RootedValue func(cx);
  if (!GetProperty(cx, v, name, &func)) {
    return false;
  }

  // Step 2. If func is either undefined or null, return undefined.
  if (func.isNullOrUndefined()) {
    result.setUndefined();
    return true;
  }

  // Step 3. If IsCallable(func) is false, throw a TypeError exception.
  if (!IsCallable(func)) {
    return ReportIsNotFunction(cx, func, -1);
  }

  // Step 4. Return func.
  result.set(func);
  return true;
}

bool GetReturnMethod(JSContext* cx, HandleValue iterator,
                     MutableHandleValue result) {
  cx->check(iterator);
  // Step 2. Let returnMethod be GetMethod(iterator, "return").
  return GetMethod(cx, iterator, cx->names().return_, result);
}

}  // namespace JS
