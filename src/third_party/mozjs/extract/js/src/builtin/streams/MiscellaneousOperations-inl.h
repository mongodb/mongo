/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Miscellaneous operations. */

#ifndef builtin_streams_MiscellaneousOperations_inl_h
#define builtin_streams_MiscellaneousOperations_inl_h

#include "builtin/streams/MiscellaneousOperations.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "js/Promise.h"        // JS::{Resolve,Reject}Promise
#include "js/RootingAPI.h"     // JS::Rooted, JS::{,Mutable}Handle
#include "js/Value.h"          // JS::UndefinedHandleValue, JS::Value
#include "vm/Compartment.h"    // JS::Compartment
#include "vm/Interpreter.h"    // js::Call
#include "vm/JSContext.h"      // JSContext
#include "vm/JSObject.h"       // JSObject
#include "vm/PromiseObject.h"  // js::PromiseObject

#include "vm/Compartment-inl.h"  // JS::Compartment::wrap
#include "vm/JSContext-inl.h"    // JSContext::check
#include "vm/JSObject-inl.h"     // js::IsCallable

namespace js {

/**
 * Streams spec, 6.3.5. PromiseCall ( F, V, args )
 * There must be 0-2 |args| arguments, all convertible to JS::Handle<JS::Value>.
 */
template <class... Args>
[[nodiscard]] inline JSObject* PromiseCall(JSContext* cx,
                                           JS::Handle<JS::Value> F,
                                           JS::Handle<JS::Value> V,
                                           Args&&... args) {
  cx->check(F);
  cx->check(V);
  cx->check(args...);

  // Step 1: Assert: ! IsCallable(F) is true.
  MOZ_ASSERT(IsCallable(F));

  // Step 2: Assert: V is not undefined.
  MOZ_ASSERT(!V.isUndefined());

  // Step 3: Assert: args is a List (implicit).
  // Step 4: Let returnValue be Call(F, V, args).
  JS::Rooted<JS::Value> rval(cx);
  if (!Call(cx, F, V, args..., &rval)) {
    // Step 5: If returnValue is an abrupt completion, return a promise rejected
    // with returnValue.[[Value]].
    return PromiseRejectedWithPendingError(cx);
  }

  // Step 6: Otherwise, return a promise resolved with returnValue.[[Value]].
  return PromiseObject::unforgeableResolve(cx, rval);
}

/**
 * Resolve the unwrapped promise |unwrappedPromise| with |value|.
 */
[[nodiscard]] inline bool ResolveUnwrappedPromiseWithValue(
    JSContext* cx, JSObject* unwrappedPromise, JS::Handle<JS::Value> value) {
  cx->check(value);

  JS::Rooted<JSObject*> promise(cx, unwrappedPromise);
  if (!cx->compartment()->wrap(cx, &promise)) {
    return false;
  }

  return JS::ResolvePromise(cx, promise, value);
}

/**
 * Resolve the unwrapped promise |unwrappedPromise| with |undefined|.
 */
[[nodiscard]] inline bool ResolveUnwrappedPromiseWithUndefined(
    JSContext* cx, JSObject* unwrappedPromise) {
  return ResolveUnwrappedPromiseWithValue(cx, unwrappedPromise,
                                          JS::UndefinedHandleValue);
}

/**
 * Reject the unwrapped promise |unwrappedPromise| with |error|, overwriting
 * |*unwrappedPromise| with its wrapped form.
 */
[[nodiscard]] inline bool RejectUnwrappedPromiseWithError(
    JSContext* cx, JS::MutableHandle<JSObject*> unwrappedPromise,
    JS::Handle<JS::Value> error) {
  cx->check(error);

  if (!cx->compartment()->wrap(cx, unwrappedPromise)) {
    return false;
  }

  return JS::RejectPromise(cx, unwrappedPromise, error);
}

/**
 * Reject the unwrapped promise |unwrappedPromise| with |error|.
 */
[[nodiscard]] inline bool RejectUnwrappedPromiseWithError(
    JSContext* cx, JSObject* unwrappedPromise, JS::Handle<JS::Value> error) {
  JS::Rooted<JSObject*> promise(cx, unwrappedPromise);
  return RejectUnwrappedPromiseWithError(cx, &promise, error);
}

}  // namespace js

#endif  // builtin_streams_MiscellaneousOperations_inl_h
