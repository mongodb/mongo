/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Promise_h
#define builtin_Promise_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::{,Mutable}Handle
#include "js/TypeDecls.h"   // JS::HandleObjectVector

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {
class CallArgs;
class Value;
}  // namespace JS

namespace js {

class AsyncFunctionGeneratorObject;
class AsyncGeneratorObject;
class PromiseObject;
class SavedFrame;

enum class CompletionKind : uint8_t;

enum class PromiseHandler : uint32_t {
  Identity = 0,
  Thrower,

  // ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
  //
  // Await in async function
  // https://tc39.es/ecma262/#await
  //
  // Step 3. fulfilledClosure Abstract Closure.
  // Step 5. rejectedClosure Abstract Closure.
  AsyncFunctionAwaitedFulfilled,
  AsyncFunctionAwaitedRejected,

  // Await in async generator
  // https://tc39.es/ecma262/#await
  //
  // Step 3. fulfilledClosure Abstract Closure.
  // Step 5. rejectedClosure Abstract Closure.
  AsyncGeneratorAwaitedFulfilled,
  AsyncGeneratorAwaitedRejected,

  // AsyncGeneratorAwaitReturn ( generator )
  // https://tc39.es/ecma262/#sec-asyncgeneratorawaitreturn
  //
  // Step 7. fulfilledClosure Abstract Closure.
  // Step 9. rejectedClosure Abstract Closure.
  AsyncGeneratorAwaitReturnFulfilled,
  AsyncGeneratorAwaitReturnRejected,

  // AsyncGeneratorUnwrapYieldResumption
  // https://tc39.es/ecma262/#sec-asyncgeneratorunwrapyieldresumption
  //
  // Steps 3-5 for awaited.[[Type]] handling.
  AsyncGeneratorYieldReturnAwaitedFulfilled,
  AsyncGeneratorYieldReturnAwaitedRejected,

  // AsyncFromSyncIteratorContinuation ( result, promiseCapability )
  // https://tc39.es/ecma262/#sec-asyncfromsynciteratorcontinuation
  //
  // Steps 7. unwrap Abstract Closure.
  AsyncFromSyncIteratorValueUnwrapDone,
  AsyncFromSyncIteratorValueUnwrapNotDone,

  // One past the maximum allowed PromiseHandler value.
  Limit
};

// Promise.prototype.then.
extern bool Promise_then(JSContext* cx, unsigned argc, JS::Value* vp);

// Promise[Symbol.species].
extern bool Promise_static_species(JSContext* cx, unsigned argc, JS::Value* vp);

// Promise.resolve.
extern bool Promise_static_resolve(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Unforgeable version of the JS builtin Promise.all.
 *
 * Takes a HandleValueVector of Promise objects and returns a promise that's
 * resolved with an array of resolution values when all those promises have
 * been resolved, or rejected with the rejection value of the first rejected
 * promise.
 *
 * Asserts that all objects in the `promises` vector are, maybe wrapped,
 * instances of `Promise` or a subclass of `Promise`.
 */
[[nodiscard]] JSObject* GetWaitForAllPromise(JSContext* cx,
                                             JS::HandleObjectVector promises);

/**
 * Enqueues resolve/reject reactions in the given Promise's reactions lists
 * as though by calling the original value of Promise.prototype.then, and
 * without regard to any Promise subclassing used in `promiseObj` itself.
 */
[[nodiscard]] PromiseObject* OriginalPromiseThen(
    JSContext* cx, JS::Handle<JSObject*> promiseObj,
    JS::Handle<JSObject*> onFulfilled, JS::Handle<JSObject*> onRejected);

enum class UnhandledRejectionBehavior { Ignore, Report };

/**
 * React to[0] `unwrappedPromise` (which may not be from the current realm) as
 * if by using a fresh promise created for the provided nullable fulfill/reject
 * IsCallable objects.
 *
 * However, no dependent Promise will be created, and mucking with `Promise`,
 * `Promise.prototype.then`, and `Promise[Symbol.species]` will not alter this
 * function's behavior.
 *
 * If `unwrappedPromise` rejects and `onRejected_` is null, handling is
 * determined by `behavior`.  If `behavior == Report`, a fresh Promise will be
 * constructed and rejected on the fly (and thus will be reported as unhandled).
 * But if `behavior == Ignore`, the rejection is ignored and is not reported as
 * unhandled.
 *
 * Note: Reactions pushed using this function contain a null `promise` field.
 * That field is only ever used by devtools, which have to treat these reactions
 * specially.
 *
 * 0. The sense of "react" here is the sense of the term as defined by Web IDL:
 *    https://heycam.github.io/webidl/#dfn-perform-steps-once-promise-is-settled
 */
[[nodiscard]] extern bool ReactToUnwrappedPromise(
    JSContext* cx, JS::Handle<PromiseObject*> unwrappedPromise,
    JS::Handle<JSObject*> onFulfilled_, JS::Handle<JSObject*> onRejected_,
    UnhandledRejectionBehavior behavior);

/**
 * PromiseResolve ( C, x )
 *
 * The abstract operation PromiseResolve, given a constructor and a value,
 * returns a new promise resolved with that value.
 */
[[nodiscard]] JSObject* PromiseResolve(JSContext* cx,
                                       JS::Handle<JSObject*> constructor,
                                       JS::Handle<JS::Value> value);

/**
 * Reject |promise| with the value of the current pending exception.
 *
 * |promise| must be from the current realm.  Callers must enter the realm of
 * |promise| if they are not already in it.
 */
[[nodiscard]] bool RejectPromiseWithPendingError(
    JSContext* cx, JS::Handle<PromiseObject*> promise);

/**
 * Create the promise object which will be used as the return value of an async
 * function.
 */
[[nodiscard]] PromiseObject* CreatePromiseObjectForAsync(JSContext* cx);

/**
 * Returns true if the given object is a promise created by
 * either CreatePromiseObjectForAsync function or async generator's method.
 */
[[nodiscard]] bool IsPromiseForAsyncFunctionOrGenerator(JSObject* promise);

[[nodiscard]] bool AsyncFunctionReturned(
    JSContext* cx, JS::Handle<PromiseObject*> resultPromise,
    JS::Handle<JS::Value> value);

[[nodiscard]] bool AsyncFunctionThrown(JSContext* cx,
                                       JS::Handle<PromiseObject*> resultPromise,
                                       JS::Handle<JS::Value> reason);

// Start awaiting `value` in an async function (, but doesn't suspend the
// async function's execution!). Returns the async function's result promise.
[[nodiscard]] JSObject* AsyncFunctionAwait(
    JSContext* cx, JS::Handle<AsyncFunctionGeneratorObject*> genObj,
    JS::Handle<JS::Value> value);

// If the await operation can be skipped and the resolution value for `val` can
// be acquired, stored the resolved value to `resolved` and `true` to
// `*canSkip`.  Otherwise, stores `false` to `*canSkip`.
[[nodiscard]] bool CanSkipAwait(JSContext* cx, JS::Handle<JS::Value> val,
                                bool* canSkip);
[[nodiscard]] bool ExtractAwaitValue(JSContext* cx, JS::Handle<JS::Value> val,
                                     JS::MutableHandle<JS::Value> resolved);

bool AsyncFromSyncIteratorMethod(JSContext* cx, JS::CallArgs& args,
                                 CompletionKind completionKind);

// Callback for describing promise reaction records, for use with
// PromiseObject::getReactionRecords.
struct PromiseReactionRecordBuilder {
  // A reaction record created by a call to 'then' or 'catch', with functions to
  // call on resolution or rejection, and the promise that will be settled
  // according to the result of calling them.
  //
  // Note that resolve, reject, and result may not be same-compartment with cx,
  // or with the promise we're inspecting. This function presents the values
  // exactly as they appear in the reaction record. They may also be wrapped or
  // unwrapped.
  //
  // Some reaction records refer to internal resolution or rejection functions
  // that are not naturally represented as debuggee JavaScript functions. In
  // this case, resolve and reject may be nullptr.
  [[nodiscard]] virtual bool then(JSContext* cx, JS::Handle<JSObject*> resolve,
                                  JS::Handle<JSObject*> reject,
                                  JS::Handle<JSObject*> result) = 0;

  // A reaction record created when one native promise is resolved to another.
  // The 'promise' argument is the promise that will be settled in the same way
  // the promise this reaction record is attached to is settled.
  //
  // Note that promise may not be same-compartment with cx. This function
  // presents the promise exactly as it appears in the reaction record.
  [[nodiscard]] virtual bool direct(
      JSContext* cx, JS::Handle<PromiseObject*> unwrappedPromise) = 0;

  // A reaction record that resumes an asynchronous function suspended at an
  // await expression. The 'generator' argument is the generator object
  // representing the call.
  //
  // Note that generator may not be same-compartment with cx. This function
  // presents the generator exactly as it appears in the reaction record.
  [[nodiscard]] virtual bool asyncFunction(
      JSContext* cx,
      JS::Handle<AsyncFunctionGeneratorObject*> unwrappedGenerator) = 0;

  // A reaction record that resumes an asynchronous generator suspended at an
  // await expression. The 'generator' argument is the generator object
  // representing the call.
  //
  // Note that generator may not be same-compartment with cx. This function
  // presents the generator exactly as it appears in the reaction record.
  [[nodiscard]] virtual bool asyncGenerator(
      JSContext* cx, JS::Handle<AsyncGeneratorObject*> unwrappedGenerator) = 0;
};

[[nodiscard]] PromiseObject* CreatePromiseObjectForAsyncGenerator(
    JSContext* cx);

[[nodiscard]] bool ResolvePromiseInternal(JSContext* cx,
                                          JS::Handle<JSObject*> promise,
                                          JS::Handle<JS::Value> resolutionVal);
[[nodiscard]] bool RejectPromiseInternal(
    JSContext* cx, JS::Handle<PromiseObject*> promise,
    JS::Handle<JS::Value> reason,
    JS::Handle<SavedFrame*> unwrappedRejectionStack = nullptr);

[[nodiscard]] bool InternalAsyncGeneratorAwait(
    JSContext* cx, JS::Handle<AsyncGeneratorObject*> generator,
    JS::Handle<JS::Value> value, PromiseHandler onFulfilled,
    PromiseHandler onRejected);

bool IsPromiseWithDefaultResolvingFunction(PromiseObject* promise);
void SetAlreadyResolvedPromiseWithDefaultResolvingFunction(
    PromiseObject* promise);

}  // namespace js

#endif  // builtin_Promise_h
