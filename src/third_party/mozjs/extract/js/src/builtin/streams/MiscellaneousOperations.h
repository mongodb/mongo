/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Miscellaneous operations. */

#ifndef builtin_streams_MiscellaneousOperations_h
#define builtin_streams_MiscellaneousOperations_h

#include "jstypes.h"           // JS_PUBLIC_API
#include "js/CallArgs.h"       // JS::CallArgs
#include "js/RootingAPI.h"     // JS::{,Mutable}Handle
#include "js/Value.h"          // JS::Value
#include "vm/JSObject.h"       // JSObject
#include "vm/PromiseObject.h"  // js::PromiseObject

struct JS_PUBLIC_API JSContext;

namespace js {

class PropertyName;

[[nodiscard]] extern PromiseObject* PromiseRejectedWithPendingError(
    JSContext* cx);

[[nodiscard]] inline bool ReturnPromiseRejectedWithPendingError(
    JSContext* cx, const JS::CallArgs& args) {
  PromiseObject* promise = PromiseRejectedWithPendingError(cx);
  if (!promise) {
    return false;
  }

  args.rval().setObject(*promise);
  return true;
}

/**
 * Streams spec, 6.3.1.
 *      CreateAlgorithmFromUnderlyingMethod ( underlyingObject, methodName,
 *                                            algoArgCount, extraArgs )
 *
 * This function only partly implements the standard algorithm. We do not
 * actually create a new JSFunction completely encapsulating the new algorithm.
 * Instead, this just gets the specified method and checks for errors. It's the
 * caller's responsibility to make sure that later, when the algorithm is
 * "performed", the appropriate steps are carried out.
 */
[[nodiscard]] extern bool CreateAlgorithmFromUnderlyingMethod(
    JSContext* cx, JS::Handle<JS::Value> underlyingObject,
    const char* methodNameForErrorMessage, JS::Handle<PropertyName*> methodName,
    JS::MutableHandle<JS::Value> method);

/**
 * Streams spec, 6.3.2. InvokeOrNoop ( O, P, args )
 * As it happens, all callers pass exactly one argument.
 */
[[nodiscard]] extern bool InvokeOrNoop(JSContext* cx, JS::Handle<JS::Value> O,
                                       JS::Handle<PropertyName*> P,
                                       JS::Handle<JS::Value> arg,
                                       JS::MutableHandle<JS::Value> rval);

/**
 * Streams spec, 6.3.7. ValidateAndNormalizeHighWaterMark ( highWaterMark )
 */
[[nodiscard]] extern bool ValidateAndNormalizeHighWaterMark(
    JSContext* cx, JS::Handle<JS::Value> highWaterMarkVal,
    double* highWaterMark);

/**
 * Streams spec, 6.3.8. MakeSizeAlgorithmFromSizeFunction ( size )
 */
[[nodiscard]] extern bool MakeSizeAlgorithmFromSizeFunction(
    JSContext* cx, JS::Handle<JS::Value> size);

template <class T>
inline bool IsMaybeWrapped(const JS::Handle<JS::Value> v) {
  return v.isObject() && v.toObject().canUnwrapAs<T>();
}

}  // namespace js

#endif  // builtin_streams_MiscellaneousOperations_h
