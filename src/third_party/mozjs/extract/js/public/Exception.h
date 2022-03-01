/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Exception_h
#define js_Exception_h

#include "mozilla/Attributes.h"

#include "jstypes.h"

#include "js/RootingAPI.h"  // JS::{Handle,Rooted}
#include "js/TypeDecls.h"
#include "js/Value.h"  // JS::Value, JS::Handle<JS::Value>

class JSErrorReport;

namespace JS {
enum class ExceptionStackBehavior : bool {
  // Do not capture any stack.
  DoNotCapture,

  // Capture the current JS stack when setting the exception. It may be
  // retrieved by JS::GetPendingExceptionStack.
  Capture
};
}  // namespace JS

extern JS_PUBLIC_API bool JS_IsExceptionPending(JSContext* cx);

extern JS_PUBLIC_API bool JS_IsThrowingOutOfMemory(JSContext* cx);

extern JS_PUBLIC_API bool JS_GetPendingException(JSContext* cx,
                                                 JS::MutableHandleValue vp);

extern JS_PUBLIC_API void JS_SetPendingException(
    JSContext* cx, JS::HandleValue v,
    JS::ExceptionStackBehavior behavior = JS::ExceptionStackBehavior::Capture);

extern JS_PUBLIC_API void JS_ClearPendingException(JSContext* cx);

/**
 * If the given object is an exception object, the exception will have (or be
 * able to lazily create) an error report struct, and this function will return
 * the address of that struct.  Otherwise, it returns nullptr. The lifetime
 * of the error report struct that might be returned is the same as the
 * lifetime of the exception object.
 */
extern JS_PUBLIC_API JSErrorReport* JS_ErrorFromException(JSContext* cx,
                                                          JS::HandleObject obj);

namespace JS {

// This class encapsulates a (pending) exception and the corresponding optional
// SavedFrame stack object captured when the pending exception was set
// on the JSContext. This fuzzily correlates with a `throw` statement in JS,
// although arbitrary JSAPI consumers or VM code may also set pending exceptions
// via `JS_SetPendingException`.
//
// This is not the same stack as `e.stack` when `e` is an `Error` object.
// (That would be JS::ExceptionStackOrNull).
class MOZ_STACK_CLASS ExceptionStack {
  Rooted<Value> exception_;
  Rooted<JSObject*> stack_;

  friend JS_PUBLIC_API bool GetPendingExceptionStack(
      JSContext* cx, JS::ExceptionStack* exceptionStack);

  void init(HandleValue exception, HandleObject stack) {
    exception_ = exception;
    stack_ = stack;
  }

 public:
  explicit ExceptionStack(JSContext* cx) : exception_(cx), stack_(cx) {}

  ExceptionStack(JSContext* cx, HandleValue exception, HandleObject stack)
      : exception_(cx, exception), stack_(cx, stack) {}

  HandleValue exception() const { return exception_; }

  // |stack| can be null.
  HandleObject stack() const { return stack_; }
};

/**
 * Save and later restore the current exception state of a given JSContext.
 * This is useful for implementing behavior in C++ that's like try/catch
 * or try/finally in JS.
 *
 * Typical usage:
 *
 *     bool ok = JS::Evaluate(cx, ...);
 *     AutoSaveExceptionState savedExc(cx);
 *     ... cleanup that might re-enter JS ...
 *     return ok;
 */
class JS_PUBLIC_API AutoSaveExceptionState {
 private:
  JSContext* context;
  bool wasPropagatingForcedReturn;
  bool wasOverRecursed;
  bool wasThrowing;
  RootedValue exceptionValue;
  RootedObject exceptionStack;

 public:
  /*
   * Take a snapshot of cx's current exception state. Then clear any current
   * pending exception in cx.
   */
  explicit AutoSaveExceptionState(JSContext* cx);

  /*
   * If neither drop() nor restore() was called, restore the exception
   * state only if no exception is currently pending on cx.
   */
  ~AutoSaveExceptionState();

  /*
   * Discard any stored exception state.
   * If this is called, the destructor is a no-op.
   */
  void drop();

  /*
   * Replace cx's exception state with the stored exception state. Then
   * discard the stored exception state. If this is called, the
   * destructor is a no-op.
   */
  void restore();
};

// Get the current pending exception value and stack.
// This function asserts that there is a pending exception.
// If this function returns false, then retrieving the current pending exception
// failed and might have been overwritten by a new exception.
extern JS_PUBLIC_API bool GetPendingExceptionStack(
    JSContext* cx, JS::ExceptionStack* exceptionStack);

// Similar to GetPendingExceptionStack, but also clears the current
// pending exception.
extern JS_PUBLIC_API bool StealPendingExceptionStack(
    JSContext* cx, JS::ExceptionStack* exceptionStack);

// Set both the exception value and its associated stack on the context as
// the current pending exception.
extern JS_PUBLIC_API void SetPendingExceptionStack(
    JSContext* cx, const JS::ExceptionStack& exceptionStack);

/**
 * If the given object is an exception object (or an unwrappable
 * cross-compartment wrapper for one), return the stack for that exception, if
 * any.  Will return null if the given object is not an exception object
 * (including if it's null or a security wrapper that can't be unwrapped) or if
 * the exception has no stack.
 */
extern JS_PUBLIC_API JSObject* ExceptionStackOrNull(JS::HandleObject obj);

}  // namespace JS

#endif  // js_Exception_h
