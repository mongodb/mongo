/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/AsyncIteration.h"

#include "builtin/Promise.h"  // js::PromiseHandler, js::CreatePromiseObjectForAsyncGenerator, js::AsyncFromSyncIteratorMethod, js::ResolvePromiseInternal, js::RejectPromiseInternal, js::InternalAsyncGeneratorAwait
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "vm/CompletionKind.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/GeneratorObject.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/PlainObject.h"    // js::PlainObject
#include "vm/PromiseObject.h"  // js::PromiseObject
#include "vm/Realm.h"
#include "vm/SelfHosting.h"

#include "vm/JSObject-inl.h"
#include "vm/List-inl.h"

using namespace js;

// ---------------
// Async generator
// ---------------

const JSClass AsyncGeneratorObject::class_ = {
    "AsyncGenerator",
    JSCLASS_HAS_RESERVED_SLOTS(AsyncGeneratorObject::Slots),
    &classOps_,
};

const JSClassOps AsyncGeneratorObject::classOps_ = {
    nullptr,                                   // addProperty
    nullptr,                                   // delProperty
    nullptr,                                   // enumerate
    nullptr,                                   // newEnumerate
    nullptr,                                   // resolve
    nullptr,                                   // mayResolve
    nullptr,                                   // finalize
    nullptr,                                   // call
    nullptr,                                   // construct
    CallTraceMethod<AbstractGeneratorObject>,  // trace
};

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// OrdinaryCreateFromConstructor ( constructor, intrinsicDefaultProto
//                                 [ , internalSlotsList ] )
// https://tc39.es/ecma262/#sec-ordinarycreatefromconstructor
//
// specialized for AsyncGeneratorObjects.
static AsyncGeneratorObject* OrdinaryCreateFromConstructorAsynGen(
    JSContext* cx, HandleFunction constructor) {
  // Step 1: Assert...
  // (implicit)

  // Step 2. Let proto be
  //         ? GetPrototypeFromConstructor(constructor, intrinsicDefaultProto).
  RootedValue protoVal(cx);
  if (!GetProperty(cx, constructor, constructor, cx->names().prototype,
                   &protoVal)) {
    return nullptr;
  }

  RootedObject proto(cx, protoVal.isObject() ? &protoVal.toObject() : nullptr);
  if (!proto) {
    proto = GlobalObject::getOrCreateAsyncGeneratorPrototype(cx, cx->global());
    if (!proto) {
      return nullptr;
    }
  }

  // Step 3. Return ! OrdinaryObjectCreate(proto, internalSlotsList).
  return NewObjectWithGivenProto<AsyncGeneratorObject>(cx, proto);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorStart ( generator, generatorBody )
// https://tc39.es/ecma262/#sec-asyncgeneratorstart
//
// Steps 6-7.
/* static */
AsyncGeneratorObject* AsyncGeneratorObject::create(JSContext* cx,
                                                   HandleFunction asyncGen) {
  MOZ_ASSERT(asyncGen->isAsync() && asyncGen->isGenerator());

  AsyncGeneratorObject* generator =
      OrdinaryCreateFromConstructorAsynGen(cx, asyncGen);
  if (!generator) {
    return nullptr;
  }

  // Step 6. Set generator.[[AsyncGeneratorState]] to suspendedStart.
  generator->setSuspendedStart();

  // Step 7. Set generator.[[AsyncGeneratorQueue]] to a new empty List.
  generator->clearSingleQueueRequest();

  generator->clearCachedRequest();

  return generator;
}

/* static */
AsyncGeneratorRequest* AsyncGeneratorObject::createRequest(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator,
    CompletionKind completionKind, HandleValue completionValue,
    Handle<PromiseObject*> promise) {
  if (!generator->hasCachedRequest()) {
    return AsyncGeneratorRequest::create(cx, completionKind, completionValue,
                                         promise);
  }

  AsyncGeneratorRequest* request = generator->takeCachedRequest();
  request->init(completionKind, completionValue, promise);
  return request;
}

/* static */ [[nodiscard]] bool AsyncGeneratorObject::enqueueRequest(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator,
    Handle<AsyncGeneratorRequest*> request) {
  if (generator->isSingleQueue()) {
    if (generator->isSingleQueueEmpty()) {
      generator->setSingleQueueRequest(request);
      return true;
    }

    Rooted<ListObject*> queue(cx, ListObject::create(cx));
    if (!queue) {
      return false;
    }

    RootedValue requestVal(cx, ObjectValue(*generator->singleQueueRequest()));
    if (!queue->append(cx, requestVal)) {
      return false;
    }
    requestVal = ObjectValue(*request);
    if (!queue->append(cx, requestVal)) {
      return false;
    }

    generator->setQueue(queue);
    return true;
  }

  Rooted<ListObject*> queue(cx, generator->queue());
  RootedValue requestVal(cx, ObjectValue(*request));
  return queue->append(cx, requestVal);
}

/* static */
AsyncGeneratorRequest* AsyncGeneratorObject::dequeueRequest(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator) {
  if (generator->isSingleQueue()) {
    AsyncGeneratorRequest* request = generator->singleQueueRequest();
    generator->clearSingleQueueRequest();
    return request;
  }

  Rooted<ListObject*> queue(cx, generator->queue());
  return &queue->popFirstAs<AsyncGeneratorRequest>(cx);
}

/* static */
AsyncGeneratorRequest* AsyncGeneratorObject::peekRequest(
    Handle<AsyncGeneratorObject*> generator) {
  if (generator->isSingleQueue()) {
    return generator->singleQueueRequest();
  }

  return &generator->queue()->getAs<AsyncGeneratorRequest>(0);
}

const JSClass AsyncGeneratorRequest::class_ = {
    "AsyncGeneratorRequest",
    JSCLASS_HAS_RESERVED_SLOTS(AsyncGeneratorRequest::Slots)};

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorRequest Records
// https://tc39.es/ecma262/#sec-asyncgeneratorrequest-records
/* static */
AsyncGeneratorRequest* AsyncGeneratorRequest::create(
    JSContext* cx, CompletionKind completionKind, HandleValue completionValue,
    Handle<PromiseObject*> promise) {
  AsyncGeneratorRequest* request =
      NewObjectWithGivenProto<AsyncGeneratorRequest>(cx, nullptr);
  if (!request) {
    return nullptr;
  }

  request->init(completionKind, completionValue, promise);
  return request;
}

[[nodiscard]] static bool AsyncGeneratorResume(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator,
    CompletionKind completionKind, HandleValue argument);

[[nodiscard]] static bool AsyncGeneratorDrainQueue(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator);

[[nodiscard]] static bool AsyncGeneratorCompleteStepNormal(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue value,
    bool done);

[[nodiscard]] static bool AsyncGeneratorCompleteStepThrow(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator,
    HandleValue exception);

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorStart ( generator, generatorBody )
// https://tc39.es/ecma262/#sec-asyncgeneratorstart
//
// Steps 4.e-j. "return" case.
[[nodiscard]] static bool AsyncGeneratorReturned(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue value) {
  // Step 4.e. Set generator.[[AsyncGeneratorState]] to completed.
  generator->setCompleted();

  // Step 4.g. If result.[[Type]] is return, set result to
  //           NormalCompletion(result.[[Value]]).
  // (implicit)

  // Step 4.h. Perform ! AsyncGeneratorCompleteStep(generator, result, true).
  if (!AsyncGeneratorCompleteStepNormal(cx, generator, value, true)) {
    return false;
  }

  // Step 4.i. Perform ! AsyncGeneratorDrainQueue(generator).
  // Step 4.j. Return undefined.
  return AsyncGeneratorDrainQueue(cx, generator);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorStart ( generator, generatorBody )
// https://tc39.es/ecma262/#sec-asyncgeneratorstart
//
// Steps 4.e-j. "throw" case.
[[nodiscard]] static bool AsyncGeneratorThrown(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator) {
  // Step 4.e. Set generator.[[AsyncGeneratorState]] to completed.
  generator->setCompleted();

  // Not much we can do about uncatchable exceptions, so just bail.
  if (!cx->isExceptionPending()) {
    return false;
  }

  // Step 4.h. Perform ! AsyncGeneratorCompleteStep(generator, result, true).
  RootedValue value(cx);
  if (!GetAndClearException(cx, &value)) {
    return false;
  }
  if (!AsyncGeneratorCompleteStepThrow(cx, generator, value)) {
    return false;
  }

  // Step 4.i. Perform ! AsyncGeneratorDrainQueue(generator).
  // Step 4.j. Return undefined.
  return AsyncGeneratorDrainQueue(cx, generator);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorUnwrapYieldResumption ( resumptionValue )
// https://tc39.es/ecma262/#sec-asyncgeneratorunwrapyieldresumption
//
// Steps 4-5.
[[nodiscard]] static bool AsyncGeneratorYieldReturnAwaitedFulfilled(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue value) {
  MOZ_ASSERT(generator->isAwaitingYieldReturn(),
             "YieldReturn-Await fulfilled when not in "
             "'AwaitingYieldReturn' state");

  // Step 4. Assert: awaited.[[Type]] is normal.
  // Step 5. Return Completion { [[Type]]: return, [[Value]]:
  //         awaited.[[Value]], [[Target]]: empty }.
  return AsyncGeneratorResume(cx, generator, CompletionKind::Return, value);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorUnwrapYieldResumption ( resumptionValue )
// https://tc39.es/ecma262/#sec-asyncgeneratorunwrapyieldresumption
//
// Step 3.
[[nodiscard]] static bool AsyncGeneratorYieldReturnAwaitedRejected(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator,
    HandleValue reason) {
  MOZ_ASSERT(
      generator->isAwaitingYieldReturn(),
      "YieldReturn-Await rejected when not in 'AwaitingYieldReturn' state");

  // Step 3. If awaited.[[Type]] is throw, return Completion(awaited).
  return AsyncGeneratorResume(cx, generator, CompletionKind::Throw, reason);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorYield ( value )
// https://tc39.es/ecma262/#sec-asyncgeneratoryield
//
// Stesp 10-13.
[[nodiscard]] static bool AsyncGeneratorYield(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue value) {
  // Step 13.a.
  generator->setSuspendedYield();

  // Step 10. Perform
  //          ! AsyncGeneratorCompleteStep(generator, completion, false,
  //                                       previousRealm).
  if (!AsyncGeneratorCompleteStepNormal(cx, generator, value, false)) {
    return false;
  }

  // Steps 11-13.
  return AsyncGeneratorDrainQueue(cx, generator);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// Await in async function
// https://tc39.es/ecma262/#await
//
// Steps 3.c-f.
[[nodiscard]] static bool AsyncGeneratorAwaitedFulfilled(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue value) {
  MOZ_ASSERT(generator->isExecuting(),
             "Await fulfilled when not in 'Executing' state");

  // Step 3.c. Push asyncContext onto the execution context stack; asyncContext
  //           is now the running execution context.
  // Step 3.d. Resume the suspended evaluation of asyncContext using
  //           NormalCompletion(value) as the result of the operation that
  //           suspended it.
  // Step 3.f. Return undefined.
  return AsyncGeneratorResume(cx, generator, CompletionKind::Normal, value);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// Await in async function
// https://tc39.es/ecma262/#await
//
// Steps 5.c-f.
[[nodiscard]] static bool AsyncGeneratorAwaitedRejected(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator,
    HandleValue reason) {
  MOZ_ASSERT(generator->isExecuting(),
             "Await rejected when not in 'Executing' state");

  // Step 5.c. Push asyncContext onto the execution context stack; asyncContext
  //           is now the running execution context.
  // Step 5.d. Resume the suspended evaluation of asyncContext using
  //           ThrowCompletion(reason) as the result of the operation that
  //           suspended it.
  // Step 5.f. Return undefined.
  return AsyncGeneratorResume(cx, generator, CompletionKind::Throw, reason);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// Await in async function
// https://tc39.es/ecma262/#await
[[nodiscard]] static bool AsyncGeneratorAwait(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue value) {
  return InternalAsyncGeneratorAwait(
      cx, generator, value, PromiseHandler::AsyncGeneratorAwaitedFulfilled,
      PromiseHandler::AsyncGeneratorAwaitedRejected);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorCompleteStep ( generator, completion, done [ , realm ] )
// https://tc39.es/ecma262/#sec-asyncgeneratorcompletestep
//
// "normal" case.
[[nodiscard]] static bool AsyncGeneratorCompleteStepNormal(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue value,
    bool done) {
  // Step 1. Let queue be generator.[[AsyncGeneratorQueue]].
  // Step 2. Assert: queue is not empty.
  MOZ_ASSERT(!generator->isQueueEmpty());

  // Step 3. Let next be the first element of queue.
  // Step 4. Remove the first element from queue.
  AsyncGeneratorRequest* next =
      AsyncGeneratorObject::dequeueRequest(cx, generator);
  if (!next) {
    return false;
  }

  // Step 5. Let promiseCapability be next.[[Capability]].
  Rooted<PromiseObject*> resultPromise(cx, next->promise());

  generator->cacheRequest(next);

  // Step 6. Let value be completion.[[Value]].
  // (passed by caller)

  // Step 7. If completion.[[Type]] is throw, then
  // Step 8. Else,
  // Step 8.a. Assert: completion.[[Type]] is normal.

  // Step 8.b. If realm is present, then
  // (skipped)
  // Step 8.c. Else,

  // Step 8.c.i. Let iteratorResult be ! CreateIterResultObject(value, done).
  JSObject* resultObj = CreateIterResultObject(cx, value, done);
  if (!resultObj) {
    return false;
  }

  // Step 8.d. Perform
  //           ! Call(promiseCapability.[[Resolve]], undefined,
  //                  « iteratorResult »).
  RootedValue resultValue(cx, ObjectValue(*resultObj));
  return ResolvePromiseInternal(cx, resultPromise, resultValue);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorCompleteStep ( generator, completion, done [ , realm ] )
// https://tc39.es/ecma262/#sec-asyncgeneratorcompletestep
//
// "throw" case.
[[nodiscard]] static bool AsyncGeneratorCompleteStepThrow(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator,
    HandleValue exception) {
  // Step 1. Let queue be generator.[[AsyncGeneratorQueue]].
  // Step 2. Assert: queue is not empty.
  MOZ_ASSERT(!generator->isQueueEmpty());

  // Step 3. Let next be the first element of queue.
  // Step 4. Remove the first element from queue.
  AsyncGeneratorRequest* next =
      AsyncGeneratorObject::dequeueRequest(cx, generator);
  if (!next) {
    return false;
  }

  // Step 5. Let promiseCapability be next.[[Capability]].
  Rooted<PromiseObject*> resultPromise(cx, next->promise());

  generator->cacheRequest(next);

  // Step 6. Let value be completion.[[Value]].
  // (passed by caller)

  // Step 7. If completion.[[Type]] is throw, then
  // Step 7.a. Perform
  //           ! Call(promiseCapability.[[Reject]], undefined, « value »).
  return RejectPromiseInternal(cx, resultPromise, exception);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorAwaitReturn ( generator )
// https://tc39.es/ecma262/#sec-asyncgeneratorawaitreturn
//
// Steps 7.a-e.
[[nodiscard]] static bool AsyncGeneratorAwaitReturnFulfilled(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue value) {
  MOZ_ASSERT(generator->isAwaitingReturn(),
             "AsyncGeneratorResumeNext-Return fulfilled when not in "
             "'AwaitingReturn' state");

  // Step 7.a. Set generator.[[AsyncGeneratorState]] to completed.
  generator->setCompleted();

  // Step 7.b. Let result be NormalCompletion(value).
  // Step 7.c. Perform ! AsyncGeneratorCompleteStep(generator, result, true).
  if (!AsyncGeneratorCompleteStepNormal(cx, generator, value, true)) {
    return false;
  }

  // Step 7.d. Perform ! AsyncGeneratorDrainQueue(generator).
  // Step 7.e. Return undefined.
  return AsyncGeneratorDrainQueue(cx, generator);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorAwaitReturn ( generator )
// https://tc39.es/ecma262/#sec-asyncgeneratorawaitreturn
//
// Steps 9.a-e.
[[nodiscard]] static bool AsyncGeneratorAwaitReturnRejected(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue value) {
  MOZ_ASSERT(generator->isAwaitingReturn(),
             "AsyncGeneratorResumeNext-Return rejected when not in "
             "'AwaitingReturn' state");

  // Step 9.a. Set generator.[[AsyncGeneratorState]] to completed.
  generator->setCompleted();

  // Step 9.b. Let result be ThrowCompletion(reason).
  // Step 9.c. Perform ! AsyncGeneratorCompleteStep(generator, result, true).
  if (!AsyncGeneratorCompleteStepThrow(cx, generator, value)) {
    return false;
  }

  // Step 9.d. Perform ! AsyncGeneratorDrainQueue(generator).
  // Step 9.e. Return undefined.
  return AsyncGeneratorDrainQueue(cx, generator);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorAwaitReturn ( generator )
// https://tc39.es/ecma262/#sec-asyncgeneratorawaitreturn
[[nodiscard]] static bool AsyncGeneratorAwaitReturn(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, HandleValue next) {
  // Step 1. Let queue be generator.[[AsyncGeneratorQueue]].
  // Step 2. Assert: queue is not empty.
  MOZ_ASSERT(!generator->isQueueEmpty());

  // Step 3. Let next be the first element of queue.
  // (passed by caller)

  // Step 4. Let completion be next.[[Completion]].
  // Step 5. Assert: completion.[[Type]] is return.
  // (implicit)

  // Steps 6-11.
  return InternalAsyncGeneratorAwait(
      cx, generator, next, PromiseHandler::AsyncGeneratorAwaitReturnFulfilled,
      PromiseHandler::AsyncGeneratorAwaitReturnRejected);
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorDrainQueue ( generator )
// https://tc39.es/ecma262/#sec-asyncgeneratordrainqueue
[[nodiscard]] static bool AsyncGeneratorDrainQueue(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator) {
  // Step 1. Assert: generator.[[AsyncGeneratorState]] is completed.
  MOZ_ASSERT(!generator->isExecuting());
  MOZ_ASSERT(!generator->isAwaitingYieldReturn());
  if (generator->isAwaitingReturn()) {
    return true;
  }

  // Step 2. Let queue be generator.[[AsyncGeneratorQueue]].
  // Step 3. If queue is empty, return.
  if (generator->isQueueEmpty()) {
    return true;
  }

  // Step 4. Let done be false.
  // (implicit)

  // Step 5. Repeat, while done is false,
  while (true) {
    // Step 5.a. Let next be the first element of queue.
    Rooted<AsyncGeneratorRequest*> next(
        cx, AsyncGeneratorObject::peekRequest(generator));
    if (!next) {
      return false;
    }

    // Step 5.b. Let completion be next.[[Completion]].
    CompletionKind completionKind = next->completionKind();

    if (completionKind != CompletionKind::Normal) {
      if (generator->isSuspendedStart()) {
        generator->setCompleted();
      }
    }
    if (!generator->isCompleted()) {
      MOZ_ASSERT(generator->isSuspendedStart() ||
                 generator->isSuspendedYield());

      RootedValue argument(cx, next->completionValue());

      if (completionKind == CompletionKind::Return) {
        generator->setAwaitingYieldReturn();

        return InternalAsyncGeneratorAwait(
            cx, generator, argument,
            PromiseHandler::AsyncGeneratorYieldReturnAwaitedFulfilled,
            PromiseHandler::AsyncGeneratorYieldReturnAwaitedRejected);
      }

      return AsyncGeneratorResume(cx, generator, completionKind, argument);
    }

    // Step 5.c. If completion.[[Type]] is return, then
    if (completionKind == CompletionKind::Return) {
      RootedValue value(cx, next->completionValue());

      // Step 5.c.i. Set generator.[[AsyncGeneratorState]] to awaiting-return.
      generator->setAwaitingReturn();

      // Step 5.c.ii. Perform ! AsyncGeneratorAwaitReturn(generator).
      // Step 5.c.iii. Set done to true.
      return AsyncGeneratorAwaitReturn(cx, generator, value);
    }

    // Step 5.d. Else,
    if (completionKind == CompletionKind::Throw) {
      RootedValue value(cx, next->completionValue());

      // Step 5.d.ii. Perform
      //              ! AsyncGeneratorCompleteStep(generator, completion, true).
      if (!AsyncGeneratorCompleteStepThrow(cx, generator, value)) {
        return false;
      }
    } else {
      // Step 5.d.i. If completion.[[Type]] is normal, then
      // Step 5.d.i.1. Set completion to NormalCompletion(undefined).
      // Step 5.d.ii. Perform
      //              ! AsyncGeneratorCompleteStep(generator, completion, true).
      if (!AsyncGeneratorCompleteStepNormal(cx, generator, UndefinedHandleValue,
                                            true)) {
        return false;
      }
    }

    MOZ_ASSERT(!generator->isExecuting());
    MOZ_ASSERT(!generator->isAwaitingYieldReturn());
    if (generator->isAwaitingReturn()) {
      return true;
    }

    // Step 5.d.iii. If queue is empty, set done to true.
    if (generator->isQueueEmpty()) {
      return true;
    }
  }
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorValidate ( generator, generatorBrand )
// https://tc39.es/ecma262/#sec-asyncgeneratorvalidate
//
// Testing part.
[[nodiscard]] static bool IsAsyncGeneratorValid(HandleValue asyncGenVal) {
  // Step 1. Perform
  //         ? RequireInternalSlot(generator, [[AsyncGeneratorContext]]).
  // Step 2. Perform
  //         ? RequireInternalSlot(generator, [[AsyncGeneratorState]]).
  // Step 3. Perform
  //         ? RequireInternalSlot(generator, [[AsyncGeneratorQueue]]).
  // Step 4. If generator.[[GeneratorBrand]] is not the same value as
  //         generatorBrand, throw a TypeError exception.
  return asyncGenVal.isObject() &&
         asyncGenVal.toObject().canUnwrapAs<AsyncGeneratorObject>();
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorValidate ( generator, generatorBrand )
// https://tc39.es/ecma262/#sec-asyncgeneratorvalidate
//
// Throwing part.
[[nodiscard]] static bool AsyncGeneratorValidateThrow(
    JSContext* cx, MutableHandleValue result) {
  Rooted<PromiseObject*> resultPromise(
      cx, CreatePromiseObjectForAsyncGenerator(cx));
  if (!resultPromise) {
    return false;
  }

  RootedValue badGeneratorError(cx);
  if (!GetTypeError(cx, JSMSG_NOT_AN_ASYNC_GENERATOR, &badGeneratorError)) {
    return false;
  }

  if (!RejectPromiseInternal(cx, resultPromise, badGeneratorError)) {
    return false;
  }

  result.setObject(*resultPromise);
  return true;
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorEnqueue ( generator, completion, promiseCapability )
// https://tc39.es/ecma262/#sec-asyncgeneratorenqueue
[[nodiscard]] static bool AsyncGeneratorEnqueue(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator,
    CompletionKind completionKind, HandleValue completionValue,
    Handle<PromiseObject*> resultPromise) {
  // Step 1. Let request be
  //         AsyncGeneratorRequest { [[Completion]]: completion,
  //                                 [[Capability]]: promiseCapability }.
  Rooted<AsyncGeneratorRequest*> request(
      cx, AsyncGeneratorObject::createRequest(cx, generator, completionKind,
                                              completionValue, resultPromise));
  if (!request) {
    return false;
  }

  // Step 2. Append request to the end of generator.[[AsyncGeneratorQueue]].
  return AsyncGeneratorObject::enqueueRequest(cx, generator, request);
}

class MOZ_STACK_CLASS MaybeEnterAsyncGeneratorRealm {
  mozilla::Maybe<AutoRealm> ar_;

 public:
  MaybeEnterAsyncGeneratorRealm() = default;
  ~MaybeEnterAsyncGeneratorRealm() = default;

  // Enter async generator's realm, and wrap the method's argument value if
  // necessary.
  [[nodiscard]] bool maybeEnterAndWrap(JSContext* cx,
                                       Handle<AsyncGeneratorObject*> generator,
                                       MutableHandleValue value) {
    if (generator->compartment() == cx->compartment()) {
      return true;
    }

    ar_.emplace(cx, generator);
    return cx->compartment()->wrap(cx, value);
  }

  // Leave async generator's realm, and wrap the method's result value if
  // necessary.
  [[nodiscard]] bool maybeLeaveAndWrap(JSContext* cx,
                                       MutableHandleValue result) {
    if (!ar_) {
      return true;
    }
    ar_.reset();

    return cx->compartment()->wrap(cx, result);
  }
};

[[nodiscard]] static bool AsyncGeneratorMethodSanityCheck(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator) {
  if (generator->isSuspendedStart() || generator->isSuspendedYield()) {
    // The spec assumes the queue is empty when async generator methods are
    // called with those state, but our debugger allows calling those methods
    // in unexpected state, such as before suspendedStart.
    if (MOZ_UNLIKELY(!generator->isQueueEmpty())) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_SUSPENDED_QUEUE_NOT_EMPTY);
      return false;
    }
  }

  return true;
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGenerator.prototype.next ( value )
// https://tc39.es/ecma262/#sec-asyncgenerator-prototype-next
bool js::AsyncGeneratorNext(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 3. Let result be AsyncGeneratorValidate(generator, empty).
  // Step 4. IfAbruptRejectPromise(result, promiseCapability).
  // (reordered)
  if (!IsAsyncGeneratorValid(args.thisv())) {
    return AsyncGeneratorValidateThrow(cx, args.rval());
  }

  // Step 1. Let generator be the this value.
  // (implicit)
  Rooted<AsyncGeneratorObject*> generator(
      cx, &args.thisv().toObject().unwrapAs<AsyncGeneratorObject>());

  MaybeEnterAsyncGeneratorRealm maybeEnterRealm;

  RootedValue completionValue(cx, args.get(0));
  if (!maybeEnterRealm.maybeEnterAndWrap(cx, generator, &completionValue)) {
    return false;
  }

  // Step 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
  Rooted<PromiseObject*> resultPromise(
      cx, CreatePromiseObjectForAsyncGenerator(cx));
  if (!resultPromise) {
    return false;
  }

  if (!AsyncGeneratorMethodSanityCheck(cx, generator)) {
    return false;
  }

  // Steps 5-10.
  if (!AsyncGeneratorEnqueue(cx, generator, CompletionKind::Normal,
                             completionValue, resultPromise)) {
    return false;
  }
  if (!generator->isExecuting() && !generator->isAwaitingYieldReturn()) {
    if (!AsyncGeneratorDrainQueue(cx, generator)) {
      return false;
    }
  }

  // Step 6.c. Return promiseCapability.[[Promise]].
  // and
  // Step 11. Return promiseCapability.[[Promise]].
  args.rval().setObject(*resultPromise);

  return maybeEnterRealm.maybeLeaveAndWrap(cx, args.rval());
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGenerator.prototype.return ( value )
// https://tc39.es/ecma262/#sec-asyncgenerator-prototype-return
bool js::AsyncGeneratorReturn(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 3. Let result be AsyncGeneratorValidate(generator, empty).
  // Step 4. IfAbruptRejectPromise(result, promiseCapability).
  // (reordered)
  if (!IsAsyncGeneratorValid(args.thisv())) {
    return AsyncGeneratorValidateThrow(cx, args.rval());
  }

  // Step 1. Let generator be the this value.
  Rooted<AsyncGeneratorObject*> generator(
      cx, &args.thisv().toObject().unwrapAs<AsyncGeneratorObject>());

  MaybeEnterAsyncGeneratorRealm maybeEnterRealm;

  RootedValue completionValue(cx, args.get(0));
  if (!maybeEnterRealm.maybeEnterAndWrap(cx, generator, &completionValue)) {
    return false;
  }

  // Step 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
  Rooted<PromiseObject*> resultPromise(
      cx, CreatePromiseObjectForAsyncGenerator(cx));
  if (!resultPromise) {
    return false;
  }

  if (!AsyncGeneratorMethodSanityCheck(cx, generator)) {
    return false;
  }

  // Step 5. Let completion be
  //         Completion { [[Type]]: return, [[Value]]: value,
  //                      [[Target]]: empty }.
  // Step 6. Perform
  //         ! AsyncGeneratorEnqueue(generator, completion, promiseCapability).
  if (!AsyncGeneratorEnqueue(cx, generator, CompletionKind::Return,
                             completionValue, resultPromise)) {
    return false;
  }

  // Steps 7-10.
  if (!generator->isExecuting() && !generator->isAwaitingYieldReturn()) {
    if (!AsyncGeneratorDrainQueue(cx, generator)) {
      return false;
    }
  }

  // Step 11. Return promiseCapability.[[Promise]].
  args.rval().setObject(*resultPromise);

  return maybeEnterRealm.maybeLeaveAndWrap(cx, args.rval());
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGenerator.prototype.throw ( exception )
// https://tc39.es/ecma262/#sec-asyncgenerator-prototype-throw
bool js::AsyncGeneratorThrow(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 3. Let result be AsyncGeneratorValidate(generator, empty).
  // Step 4. IfAbruptRejectPromise(result, promiseCapability).
  // (reordered)
  if (!IsAsyncGeneratorValid(args.thisv())) {
    return AsyncGeneratorValidateThrow(cx, args.rval());
  }

  // Step 1. Let generator be the this value.
  Rooted<AsyncGeneratorObject*> generator(
      cx, &args.thisv().toObject().unwrapAs<AsyncGeneratorObject>());

  MaybeEnterAsyncGeneratorRealm maybeEnterRealm;

  RootedValue completionValue(cx, args.get(0));
  if (!maybeEnterRealm.maybeEnterAndWrap(cx, generator, &completionValue)) {
    return false;
  }

  // Step 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
  Rooted<PromiseObject*> resultPromise(
      cx, CreatePromiseObjectForAsyncGenerator(cx));
  if (!resultPromise) {
    return false;
  }

  if (!AsyncGeneratorMethodSanityCheck(cx, generator)) {
    return false;
  }

  // Steps 5-11.
  if (!AsyncGeneratorEnqueue(cx, generator, CompletionKind::Throw,
                             completionValue, resultPromise)) {
    return false;
  }
  if (!generator->isExecuting() && !generator->isAwaitingYieldReturn()) {
    if (!AsyncGeneratorDrainQueue(cx, generator)) {
      return false;
    }
  }

  // Step 7.b. Return promiseCapability.[[Promise]].
  // and
  // Step 12. Return promiseCapability.[[Promise]].
  args.rval().setObject(*resultPromise);

  return maybeEnterRealm.maybeLeaveAndWrap(cx, args.rval());
}

// ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
//
// AsyncGeneratorResume ( generator, completion )
// https://tc39.es/ecma262/#sec-asyncgeneratorresume
[[nodiscard]] static bool AsyncGeneratorResume(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator,
    CompletionKind completionKind, HandleValue argument) {
  MOZ_ASSERT(!generator->isClosed(),
             "closed generator when resuming async generator");
  MOZ_ASSERT(generator->isSuspended(),
             "non-suspended generator when resuming async generator");

  // Step 1. Assert: generator.[[AsyncGeneratorState]] is either
  //         suspendedStart or suspendedYield.
  //
  // NOTE: We're using suspend/resume also for await. and the state can be
  //       anything.

  // Steps 2-4 are handled in generator.

  // Step 5. Set generator.[[AsyncGeneratorState]] to executing.
  generator->setExecuting();

  // Step 6. Push genContext onto the execution context stack; genContext is
  //         now the running execution context.
  // Step 7. Resume the suspended evaluation of genContext using completion as
  //         the result of the operation that suspended it. Let result be the
  //         completion record returned by the resumed computation.
  Handle<PropertyName*> funName = completionKind == CompletionKind::Normal
                                      ? cx->names().AsyncGeneratorNext
                                  : completionKind == CompletionKind::Throw
                                      ? cx->names().AsyncGeneratorThrow
                                      : cx->names().AsyncGeneratorReturn;
  FixedInvokeArgs<1> args(cx);
  args[0].set(argument);
  RootedValue thisOrRval(cx, ObjectValue(*generator));
  if (!CallSelfHostedFunction(cx, funName, thisOrRval, args, &thisOrRval)) {
    // 25.5.3.2, steps 5.f, 5.g.
    if (!generator->isClosed()) {
      generator->setClosed(cx);
    }
    return AsyncGeneratorThrown(cx, generator);
  }

  // 6.2.3.1, steps 2-9.
  if (generator->isAfterAwait()) {
    return AsyncGeneratorAwait(cx, generator, thisOrRval);
  }

  // 25.5.3.7, steps 5-6, 9.
  if (generator->isAfterYield()) {
    return AsyncGeneratorYield(cx, generator, thisOrRval);
  }

  // 25.5.3.2, steps 5.d-g.
  return AsyncGeneratorReturned(cx, generator, thisOrRval);
}

static const JSFunctionSpec async_generator_methods[] = {
    JS_FN("next", js::AsyncGeneratorNext, 1, 0),
    JS_FN("throw", js::AsyncGeneratorThrow, 1, 0),
    JS_FN("return", js::AsyncGeneratorReturn, 1, 0), JS_FS_END};

static JSObject* CreateAsyncGeneratorFunction(JSContext* cx, JSProtoKey key) {
  RootedObject proto(cx, &cx->global()->getFunctionConstructor());
  Handle<PropertyName*> name = cx->names().AsyncGeneratorFunction;

  // ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
  //
  // The AsyncGeneratorFunction Constructor
  // https://tc39.es/ecma262/#sec-asyncgeneratorfunction-constructor
  return NewFunctionWithProto(cx, AsyncGeneratorConstructor, 1,
                              FunctionFlags::NATIVE_CTOR, nullptr, name, proto,
                              gc::AllocKind::FUNCTION, TenuredObject);
}

static JSObject* CreateAsyncGeneratorFunctionPrototype(JSContext* cx,
                                                       JSProtoKey key) {
  return NewTenuredObjectWithFunctionPrototype(cx, cx->global());
}

static bool AsyncGeneratorFunctionClassFinish(JSContext* cx,
                                              HandleObject asyncGenFunction,
                                              HandleObject asyncGenerator) {
  Handle<GlobalObject*> global = cx->global();

  // Change the "constructor" property to non-writable before adding any other
  // properties, so it's still the last property and can be modified without a
  // dictionary-mode transition.
  MOZ_ASSERT(asyncGenerator->as<NativeObject>().getLastProperty().key() ==
             NameToId(cx->names().constructor));
  MOZ_ASSERT(!asyncGenerator->as<NativeObject>().inDictionaryMode());

  RootedValue asyncGenFunctionVal(cx, ObjectValue(*asyncGenFunction));
  if (!DefineDataProperty(cx, asyncGenerator, cx->names().constructor,
                          asyncGenFunctionVal, JSPROP_READONLY)) {
    return false;
  }
  MOZ_ASSERT(!asyncGenerator->as<NativeObject>().inDictionaryMode());

  RootedObject asyncIterProto(
      cx, GlobalObject::getOrCreateAsyncIteratorPrototype(cx, global));
  if (!asyncIterProto) {
    return false;
  }

  // ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
  //
  // AsyncGenerator Objects
  // https://tc39.es/ecma262/#sec-asyncgenerator-objects
  RootedObject asyncGenProto(cx, GlobalObject::createBlankPrototypeInheriting(
                                     cx, &PlainObject::class_, asyncIterProto));
  if (!asyncGenProto) {
    return false;
  }
  if (!DefinePropertiesAndFunctions(cx, asyncGenProto, nullptr,
                                    async_generator_methods) ||
      !DefineToStringTag(cx, asyncGenProto, cx->names().AsyncGenerator)) {
    return false;
  }

  // ES2022 draft rev 193211a3d889a61e74ef7da1475dfa356e029f29
  //
  // Properties of the AsyncGeneratorFunction Prototype Object
  // https://tc39.es/ecma262/#sec-properties-of-asyncgeneratorfunction-prototype
  if (!LinkConstructorAndPrototype(cx, asyncGenerator, asyncGenProto,
                                   JSPROP_READONLY, JSPROP_READONLY) ||
      !DefineToStringTag(cx, asyncGenerator,
                         cx->names().AsyncGeneratorFunction)) {
    return false;
  }

  global->setAsyncGeneratorPrototype(asyncGenProto);

  return true;
}

static const ClassSpec AsyncGeneratorFunctionClassSpec = {
    CreateAsyncGeneratorFunction,
    CreateAsyncGeneratorFunctionPrototype,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    AsyncGeneratorFunctionClassFinish,
    ClassSpec::DontDefineConstructor};

const JSClass js::AsyncGeneratorFunctionClass = {
    "AsyncGeneratorFunction", 0, JS_NULL_CLASS_OPS,
    &AsyncGeneratorFunctionClassSpec};

[[nodiscard]] bool js::AsyncGeneratorPromiseReactionJob(
    JSContext* cx, PromiseHandler handler,
    Handle<AsyncGeneratorObject*> generator, HandleValue argument) {
  // Await's handlers don't return a value, nor throw any exceptions.
  // They fail only on OOM.
  switch (handler) {
    case PromiseHandler::AsyncGeneratorAwaitedFulfilled:
      return AsyncGeneratorAwaitedFulfilled(cx, generator, argument);

    case PromiseHandler::AsyncGeneratorAwaitedRejected:
      return AsyncGeneratorAwaitedRejected(cx, generator, argument);

    case PromiseHandler::AsyncGeneratorAwaitReturnFulfilled:
      return AsyncGeneratorAwaitReturnFulfilled(cx, generator, argument);

    case PromiseHandler::AsyncGeneratorAwaitReturnRejected:
      return AsyncGeneratorAwaitReturnRejected(cx, generator, argument);

    case PromiseHandler::AsyncGeneratorYieldReturnAwaitedFulfilled:
      return AsyncGeneratorYieldReturnAwaitedFulfilled(cx, generator, argument);

    case PromiseHandler::AsyncGeneratorYieldReturnAwaitedRejected:
      return AsyncGeneratorYieldReturnAwaitedRejected(cx, generator, argument);

    default:
      MOZ_CRASH("Bad handler in AsyncGeneratorPromiseReactionJob");
  }
}

// ---------------------
// AsyncFromSyncIterator
// ---------------------

const JSClass AsyncFromSyncIteratorObject::class_ = {
    "AsyncFromSyncIteratorObject",
    JSCLASS_HAS_RESERVED_SLOTS(AsyncFromSyncIteratorObject::Slots)};

/*
 * ES2024 draft rev 53454a9a596d90473d2152ef04656d605162cd4c
 *
 * CreateAsyncFromSyncIterator ( syncIteratorRecord )
 * https://tc39.es/ecma262/#sec-createasyncfromsynciterator
 */
JSObject* js::CreateAsyncFromSyncIterator(JSContext* cx, HandleObject iter,
                                          HandleValue nextMethod) {
  // Steps 1-5.
  return AsyncFromSyncIteratorObject::create(cx, iter, nextMethod);
}

/*
 * ES2024 draft rev 53454a9a596d90473d2152ef04656d605162cd4c
 *
 * CreateAsyncFromSyncIterator ( syncIteratorRecord )
 * https://tc39.es/ecma262/#sec-createasyncfromsynciterator
 */
/* static */
JSObject* AsyncFromSyncIteratorObject::create(JSContext* cx, HandleObject iter,
                                              HandleValue nextMethod) {
  // Step 1. Let asyncIterator be
  //         OrdinaryObjectCreate(%AsyncFromSyncIteratorPrototype%, «
  //         [[SyncIteratorRecord]] »).
  RootedObject proto(cx,
                     GlobalObject::getOrCreateAsyncFromSyncIteratorPrototype(
                         cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  AsyncFromSyncIteratorObject* asyncIter =
      NewObjectWithGivenProto<AsyncFromSyncIteratorObject>(cx, proto);
  if (!asyncIter) {
    return nullptr;
  }

  // Step 3. Let nextMethod be ! Get(asyncIterator, "next").
  // (done in caller)

  // Step 2. Set asyncIterator.[[SyncIteratorRecord]] to syncIteratorRecord.
  // Step 4. Let iteratorRecord be the Iterator Record { [[Iterator]]:
  //         asyncIterator, [[NextMethod]]: nextMethod, [[Done]]: false }.
  asyncIter->init(iter, nextMethod);

  // Step 5. Return iteratorRecord.
  return asyncIter;
}

/**
 * ES2024 draft rev 53454a9a596d90473d2152ef04656d605162cd4c
 *
 * %AsyncFromSyncIteratorPrototype%.next ( [ value ] )
 * https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%.next
 */
static bool AsyncFromSyncIteratorNext(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return AsyncFromSyncIteratorMethod(cx, args, CompletionKind::Normal);
}

/**
 * ES2024 draft rev 53454a9a596d90473d2152ef04656d605162cd4c
 *
 * %AsyncFromSyncIteratorPrototype%.return ( [ value ] )
 * https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%.return
 */
static bool AsyncFromSyncIteratorReturn(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return AsyncFromSyncIteratorMethod(cx, args, CompletionKind::Return);
}

/**
 * ES2024 draft rev 53454a9a596d90473d2152ef04656d605162cd4c
 *
 * %AsyncFromSyncIteratorPrototype%.throw ( [ value ] )
 * https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%.throw
 */
static bool AsyncFromSyncIteratorThrow(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return AsyncFromSyncIteratorMethod(cx, args, CompletionKind::Throw);
}

static const JSFunctionSpec async_from_sync_iter_methods[] = {
    JS_FN("next", AsyncFromSyncIteratorNext, 1, 0),
    JS_FN("throw", AsyncFromSyncIteratorThrow, 1, 0),
    JS_FN("return", AsyncFromSyncIteratorReturn, 1, 0), JS_FS_END};

bool GlobalObject::initAsyncFromSyncIteratorProto(
    JSContext* cx, Handle<GlobalObject*> global) {
  if (global->hasBuiltinProto(ProtoKind::AsyncFromSyncIteratorProto)) {
    return true;
  }

  RootedObject asyncIterProto(
      cx, GlobalObject::getOrCreateAsyncIteratorPrototype(cx, global));
  if (!asyncIterProto) {
    return false;
  }

  // ES2024 draft rev 53454a9a596d90473d2152ef04656d605162cd4c
  //
  // The %AsyncFromSyncIteratorPrototype% Object
  // https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%-object
  RootedObject asyncFromSyncIterProto(
      cx, GlobalObject::createBlankPrototypeInheriting(cx, &PlainObject::class_,
                                                       asyncIterProto));
  if (!asyncFromSyncIterProto) {
    return false;
  }
  if (!DefinePropertiesAndFunctions(cx, asyncFromSyncIterProto, nullptr,
                                    async_from_sync_iter_methods) ||
      !DefineToStringTag(cx, asyncFromSyncIterProto,
                         cx->names().Async_from_Sync_Iterator_)) {
    return false;
  }

  global->initBuiltinProto(ProtoKind::AsyncFromSyncIteratorProto,
                           asyncFromSyncIterProto);
  return true;
}

// -------------
// AsyncIterator
// -------------

static const JSFunctionSpec async_iterator_proto_methods[] = {
    JS_SELF_HOSTED_SYM_FN(asyncIterator, "AsyncIteratorIdentity", 0, 0),
    JS_FS_END};

static const JSFunctionSpec async_iterator_proto_methods_with_helpers[] = {
    JS_SELF_HOSTED_FN("map", "AsyncIteratorMap", 1, 0),
    JS_SELF_HOSTED_FN("filter", "AsyncIteratorFilter", 1, 0),
    JS_SELF_HOSTED_FN("take", "AsyncIteratorTake", 1, 0),
    JS_SELF_HOSTED_FN("drop", "AsyncIteratorDrop", 1, 0),
    JS_SELF_HOSTED_FN("asIndexedPairs", "AsyncIteratorAsIndexedPairs", 0, 0),
    JS_SELF_HOSTED_FN("flatMap", "AsyncIteratorFlatMap", 1, 0),
    JS_SELF_HOSTED_FN("reduce", "AsyncIteratorReduce", 1, 0),
    JS_SELF_HOSTED_FN("toArray", "AsyncIteratorToArray", 0, 0),
    JS_SELF_HOSTED_FN("forEach", "AsyncIteratorForEach", 1, 0),
    JS_SELF_HOSTED_FN("some", "AsyncIteratorSome", 1, 0),
    JS_SELF_HOSTED_FN("every", "AsyncIteratorEvery", 1, 0),
    JS_SELF_HOSTED_FN("find", "AsyncIteratorFind", 1, 0),
    JS_SELF_HOSTED_SYM_FN(asyncIterator, "AsyncIteratorIdentity", 0, 0),
    JS_FS_END};

bool GlobalObject::initAsyncIteratorProto(JSContext* cx,
                                          Handle<GlobalObject*> global) {
  if (global->hasBuiltinProto(ProtoKind::AsyncIteratorProto)) {
    return true;
  }

  // 25.1.3 The %AsyncIteratorPrototype% Object
  RootedObject asyncIterProto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
  if (!asyncIterProto) {
    return false;
  }
  if (!DefinePropertiesAndFunctions(cx, asyncIterProto, nullptr,
                                    async_iterator_proto_methods)) {
    return false;
  }

  global->initBuiltinProto(ProtoKind::AsyncIteratorProto, asyncIterProto);
  return true;
}

// https://tc39.es/proposal-iterator-helpers/#sec-asynciterator as of revision
// 8f10db5.
static bool AsyncIteratorConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "AsyncIterator")) {
    return false;
  }
  // Throw TypeError if NewTarget is the active function object, preventing the
  // Iterator constructor from being used directly.
  if (args.callee() == args.newTarget().toObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BOGUS_CONSTRUCTOR, "AsyncIterator");
    return false;
  }

  // Step 2.
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_AsyncIterator,
                                          &proto)) {
    return false;
  }

  JSObject* obj = NewObjectWithClassProto<AsyncIteratorObject>(cx, proto);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static const ClassSpec AsyncIteratorObjectClassSpec = {
    GenericCreateConstructor<AsyncIteratorConstructor, 0,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<AsyncIteratorObject>,
    nullptr,
    nullptr,
    async_iterator_proto_methods_with_helpers,
    nullptr,
    nullptr,
};

const JSClass AsyncIteratorObject::class_ = {
    "AsyncIterator",
    JSCLASS_HAS_CACHED_PROTO(JSProto_AsyncIterator),
    JS_NULL_CLASS_OPS,
    &AsyncIteratorObjectClassSpec,
};

const JSClass AsyncIteratorObject::protoClass_ = {
    "AsyncIterator.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_AsyncIterator),
    JS_NULL_CLASS_OPS,
    &AsyncIteratorObjectClassSpec,
};

// Iterator Helper proposal
static const JSFunctionSpec async_iterator_helper_methods[] = {
    JS_SELF_HOSTED_FN("next", "AsyncIteratorHelperNext", 1, 0),
    JS_SELF_HOSTED_FN("return", "AsyncIteratorHelperReturn", 1, 0),
    JS_SELF_HOSTED_FN("throw", "AsyncIteratorHelperThrow", 1, 0),
    JS_FS_END,
};

static const JSClass AsyncIteratorHelperPrototypeClass = {
    "Async Iterator Helper", 0};

const JSClass AsyncIteratorHelperObject::class_ = {
    "Async Iterator Helper",
    JSCLASS_HAS_RESERVED_SLOTS(AsyncIteratorHelperObject::SlotCount),
};

/* static */
NativeObject* GlobalObject::getOrCreateAsyncIteratorHelperPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  return MaybeNativeObject(
      getOrCreateBuiltinProto(cx, global, ProtoKind::AsyncIteratorHelperProto,
                              initAsyncIteratorHelperProto));
}

/* static */
bool GlobalObject::initAsyncIteratorHelperProto(JSContext* cx,
                                                Handle<GlobalObject*> global) {
  if (global->hasBuiltinProto(ProtoKind::AsyncIteratorHelperProto)) {
    return true;
  }

  RootedObject asyncIterProto(
      cx, GlobalObject::getOrCreateAsyncIteratorPrototype(cx, global));
  if (!asyncIterProto) {
    return false;
  }

  RootedObject asyncIteratorHelperProto(
      cx, GlobalObject::createBlankPrototypeInheriting(
              cx, &AsyncIteratorHelperPrototypeClass, asyncIterProto));
  if (!asyncIteratorHelperProto) {
    return false;
  }
  if (!DefinePropertiesAndFunctions(cx, asyncIteratorHelperProto, nullptr,
                                    async_iterator_helper_methods)) {
    return false;
  }

  global->initBuiltinProto(ProtoKind::AsyncIteratorHelperProto,
                           asyncIteratorHelperProto);
  return true;
}

AsyncIteratorHelperObject* js::NewAsyncIteratorHelper(JSContext* cx) {
  RootedObject proto(cx, GlobalObject::getOrCreateAsyncIteratorHelperPrototype(
                             cx, cx->global()));
  if (!proto) {
    return nullptr;
  }
  return NewObjectWithGivenProto<AsyncIteratorHelperObject>(cx, proto);
}
