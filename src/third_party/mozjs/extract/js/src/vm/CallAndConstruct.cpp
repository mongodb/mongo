/* -*- Mode.h: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/CallAndConstruct.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jstypes.h"                  // JS_PUBLIC_API
#include "gc/Zone.h"                  // js::Zone
#include "js/Context.h"               // AssertHeapIsIdle
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/RootingAPI.h"    // JS::Rooted, JS::Handle, JS::MutableHandle
#include "js/Value.h"         // JS::Value, JS::*Value
#include "js/ValueArray.h"    // JS::HandleValueArray
#include "vm/BytecodeUtil.h"  // JSDVG_IGNORE_STACK
#include "vm/Interpreter.h"   // js::Call, js::Construct
#include "vm/JSAtomUtils.h"   // js::Atomize
#include "vm/JSContext.h"     // JSContext, CHECK_THREAD, ReportValueError
#include "vm/JSObject.h"      // JSObject
#include "vm/Stack.h"  // js::InvokeArgs, js::FillArgumentsFromArraylike, js::ConstructArgs
#include "vm/StringType.h"  // JSAtom

#include "vm/JSAtomUtils-inl.h"       // js::AtomToId
#include "vm/JSContext-inl.h"         // JSContext::check
#include "vm/JSObject-inl.h"          // js::IsConstructor
#include "vm/ObjectOperations-inl.h"  // js::GetProperty

using namespace js;

JS_PUBLIC_API bool JS::IsCallable(JSObject* obj) { return obj->isCallable(); }

JS_PUBLIC_API bool JS::IsConstructor(JSObject* obj) {
  return obj->isConstructor();
}

JS_PUBLIC_API bool JS_CallFunctionValue(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        JS::Handle<JS::Value> fval,
                                        const JS::HandleValueArray& args,
                                        JS::MutableHandle<JS::Value> rval) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, fval, args);

  js::InvokeArgs iargs(cx);
  if (!FillArgumentsFromArraylike(cx, iargs, args)) {
    return false;
  }

  JS::Rooted<JS::Value> thisv(cx, JS::ObjectOrNullValue(obj));
  return js::Call(cx, fval, thisv, iargs, rval);
}

JS_PUBLIC_API bool JS_CallFunction(JSContext* cx, JS::Handle<JSObject*> obj,
                                   JS::Handle<JSFunction*> fun,
                                   const JS::HandleValueArray& args,
                                   JS::MutableHandle<JS::Value> rval) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, fun, args);

  js::InvokeArgs iargs(cx);
  if (!FillArgumentsFromArraylike(cx, iargs, args)) {
    return false;
  }

  JS::Rooted<JS::Value> fval(cx, JS::ObjectValue(*fun));
  JS::Rooted<JS::Value> thisv(cx, JS::ObjectOrNullValue(obj));
  return js::Call(cx, fval, thisv, iargs, rval);
}

JS_PUBLIC_API bool JS_CallFunctionName(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char* name,
                                       const JS::HandleValueArray& args,
                                       JS::MutableHandle<JS::Value> rval) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, args);

  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }

  JS::Rooted<JS::Value> fval(cx);
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  if (!GetProperty(cx, obj, obj, id, &fval)) {
    return false;
  }

  js::InvokeArgs iargs(cx);
  if (!FillArgumentsFromArraylike(cx, iargs, args)) {
    return false;
  }

  JS::Rooted<JS::Value> thisv(cx, JS::ObjectOrNullValue(obj));
  return js::Call(cx, fval, thisv, iargs, rval);
}

JS_PUBLIC_API bool JS::Call(JSContext* cx, JS::Handle<JS::Value> thisv,
                            JS::Handle<JS::Value> fval,
                            const JS::HandleValueArray& args,
                            JS::MutableHandle<JS::Value> rval) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(thisv, fval, args);

  js::InvokeArgs iargs(cx);
  if (!FillArgumentsFromArraylike(cx, iargs, args)) {
    return false;
  }

  return js::Call(cx, fval, thisv, iargs, rval);
}

JS_PUBLIC_API bool JS::Construct(JSContext* cx, JS::Handle<JS::Value> fval,
                                 JS::Handle<JSObject*> newTarget,
                                 const JS::HandleValueArray& args,
                                 JS::MutableHandle<JSObject*> objp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(fval, newTarget, args);

  if (!js::IsConstructor(fval)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, fval,
                     nullptr);
    return false;
  }

  JS::Rooted<JS::Value> newTargetVal(cx, JS::ObjectValue(*newTarget));
  if (!js::IsConstructor(newTargetVal)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK,
                     newTargetVal, nullptr);
    return false;
  }

  js::ConstructArgs cargs(cx);
  if (!FillArgumentsFromArraylike(cx, cargs, args)) {
    return false;
  }

  return js::Construct(cx, fval, cargs, newTargetVal, objp);
}

JS_PUBLIC_API bool JS::Construct(JSContext* cx, JS::Handle<JS::Value> fval,
                                 const JS::HandleValueArray& args,
                                 JS::MutableHandle<JSObject*> objp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(fval, args);

  if (!js::IsConstructor(fval)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, fval,
                     nullptr);
    return false;
  }

  js::ConstructArgs cargs(cx);
  if (!FillArgumentsFromArraylike(cx, cargs, args)) {
    return false;
  }

  return js::Construct(cx, fval, cargs, fval, objp);
}
