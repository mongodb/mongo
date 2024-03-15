/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_AsyncIteration_h
#define vm_AsyncIteration_h

#include "builtin/Promise.h"  // js::PromiseHandler
#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "vm/GeneratorObject.h"
#include "vm/JSObject.h"
#include "vm/List.h"
#include "vm/PromiseObject.h"

// [SMDOC] Async generators
//
// # Start
//
// When an async generator is called, it synchronously runs until the
// JSOp::InitialYield and then suspends, just like a sync generator, and returns
// an async generator object (js::AsyncGeneratorObject).
//
//
// # Request queue
//
// When next/return/throw is called on the async generator object,
// js::AsyncGeneratorEnqueue performs the following:
//   * Create a new AsyncGeneratorRequest and enqueue it in the generator
//     object's request queue.
//   * Resume the generator with the oldest request, if the generator is
//     suspended (see "Resume" section below)
//   * Return the promise for the request
//
// This is done in js::AsyncGeneratorEnqueue, which corresponds to
// AsyncGeneratorEnqueue in the spec,
// and js::AsyncGeneratorResumeNext corresponds to the following:
//   * AsyncGeneratorResolve
//   * AsyncGeneratorReject
//   * AsyncGeneratorResumeNext
//
// The returned promise is resolved when the resumption for the request
// completes with yield/throw/return, in js::AsyncGeneratorResolve and
// js::AsyncGeneratorReject.
// They correspond to AsyncGeneratorResolve and AsyncGeneratorReject in the
// spec.
//
//
// # Await
//
// Async generator's `await` is implemented differently than async function's
// `await`.
//
// The bytecode is the following:
// (ignoring CanSkipAwait; see the comment in AsyncFunction.h for more details)
//
// ```
//   (operand here)                  # VALUE
//   GetAliasedVar ".generator"      # VALUE .generator
//   Await 0                         # RVAL GENERATOR RESUMEKIND
//
//   AfterYield                      # RVAL GENERATOR RESUMEKIND
//   CheckResumeKind                 # RVAL
// ```
//
// Async generators don't use JSOp::AsyncAwait, and that part is handled
// in js::AsyncGeneratorResume, and js::AsyncGeneratorAwait called there.
//
// Both JSOp::Await and JSOp::Yield behave in the exactly same way,
// and js::AsyncGeneratorResume checks the last opcode and branches for
// await/yield/return cases.
//
//
// # Reaction jobs and resume after await
//
// This is almost same as for async functions (see AsyncFunction.h).
//
// The reaction record for the job is marked as "this is for async generator"
// (see js::AsyncGeneratorAwait), and handled specially in
// js::PromiseReactionJob, which calls js::AsyncGeneratorPromiseReactionJob.
//
//
// # Yield
//
// `yield` is implemented with the following bytecode sequence:
// (Ignoring CanSkipAwait for simplicity)
//
// ```
//   (operand here)                  # VALUE
//   GetAliasedVar ".generator"      # VALUE .generator
//   Await 1                         # RVAL GENERATOR RESUMEKIND
//   AfterYield                      # RVAL GENERATOR RESUMEKIND
//   CheckResumeKind                 # RVAL
//
//   GetAliasedVar ".generator"      # RVAL .generator
//   Yield 2                         # RVAL2 GENERATOR RESUMEKIND
//
//   AfterYield                      # RVAL2 GENERATOR RESUMEKIND
//   CheckResumeKind                 # RVAL2
// ```
//
// The 1st part (JSOp::Await + JSOp::CheckResumeKind) performs an implicit
// `await`, as specified in AsyncGeneratorYield step 5.
//
// AsyncGeneratorYield ( value )
// https://tc39.es/ecma262/#sec-asyncgeneratoryield
//
//   5. Set value to ? Await(value).
//
// The 2nd part (JSOp::Yield) suspends execution and yields the result of
// `await`, as specified in AsyncGeneratorYield steps 1-4, 6-7, 9-10.
//
// AsyncGeneratorYield ( value )
// https://tc39.es/ecma262/#sec-asyncgeneratoryield
//
//   1. Let genContext be the running execution context.
//   2. Assert: genContext is the execution context of a generator.
//   3. Let generator be the value of the Generator component of genContext.
//   4. Assert: GetGeneratorKind() is async.
//   ..
//   6. Set generator.[[AsyncGeneratorState]] to suspendedYield.
//   7. Remove genContext from the execution context stack and restore the
//      execution context that is at the top of the execution context stack as
//      the running execution context.
//   8. ...
//   9. Return ! AsyncGeneratorResolve(generator, value, false).
//   10. NOTE: This returns to the evaluation of the operation that had most
//       previously resumed evaluation of genContext.
//
// The last part (JSOp::CheckResumeKind) checks the resumption type and
// resumes/throws/returns the execution, as specified in AsyncGeneratorYield
// step 8.
//
//   8. Set the code evaluation state of genContext such that when evaluation is
//      resumed with a Completion resumptionValue the following steps will be
//      performed:
//     a. If resumptionValue.[[Type]] is not return, return
//        Completion(resumptionValue).
//     b. Let awaited be Await(resumptionValue.[[Value]]).
//     c. If awaited.[[Type]] is throw, return Completion(awaited).
//     d. Assert: awaited.[[Type]] is normal.
//     e. Return Completion { [[Type]]: return, [[Value]]: awaited.[[Value]],
//        [[Target]]: empty }.
//     f. NOTE: When one of the above steps returns, it returns to the
//        evaluation of the YieldExpression production that originally called
//        this abstract operation.
//
// Resumption with `AsyncGenerator.prototype.return` is handled differently.
// See "Resumption with return" section below.
//
//
// # Return
//
// `return` with operand is implemented with the following bytecode sequence:
// (Ignoring CanSkipAwait for simplicity)
//
// ```
//   (operand here)                  # VALUE
//   GetAliasedVar ".generator"      # VALUE .generator
//   Await 0                         # RVAL GENERATOR RESUMEKIND
//   AfterYield                      # RVAL GENERATOR RESUMEKIND
//   CheckResumeKind                 # RVAL
//
//   SetRval                         #
//   GetAliasedVar ".generator"      # .generator
//   FinalYieldRval                  #
// ```
//
// The 1st part (JSOp::Await + JSOp::CheckResumeKind) performs implicit
// `await`, as specified in ReturnStatement's Evaluation step 3.
//
// ReturnStatement: return Expression;
// https://tc39.es/ecma262/#sec-return-statement-runtime-semantics-evaluation
//
//   3. If ! GetGeneratorKind() is async, set exprValue to ? Await(exprValue).
//
// And the 2nd part corresponds to AsyncGeneratorStart steps 5.a-e, 5.g.
//
// AsyncGeneratorStart ( generator, generatorBody )
// https://tc39.es/ecma262/#sec-asyncgeneratorstart
//
//   5. Set the code evaluation state of genContext such that when evaluation
//      is resumed for that execution context the following steps will be
//      performed:
//     a. Let result be the result of evaluating generatorBody.
//     b. Assert: If we return here, the async generator either threw an
//        exception or performed either an implicit or explicit return.
//     c. Remove genContext from the execution context stack and restore the
//        execution context that is at the top of the execution context stack
//        as the running execution context.
//     d. Set generator.[[AsyncGeneratorState]] to completed.
//     e. If result is a normal completion, let resultValue be undefined.
//     ...
//     g. Return ! AsyncGeneratorResolve(generator, resultValue, true).
//
// `return` without operand or implicit return is implicit with the following
// bytecode sequence:
//
// ```
//   Undefined                       # undefined
//   SetRval                         #
//   GetAliasedVar ".generator"      # .generator
//   FinalYieldRval                  #
// ```
//
// This is also AsyncGeneratorStart steps 5.a-e, 5.g.
//
//
// # Throw
//
// Unlike async function, async generator doesn't use implicit try-catch,
// but the throw completion is handled by js::AsyncGeneratorResume,
// and js::AsyncGeneratorThrown is called there.
//
//   5. ...
//     f. Else,
//       i. Let resultValue be result.[[Value]].
//       ii. If result.[[Type]] is not return, then
//         1. Return ! AsyncGeneratorReject(generator, resultValue).
//
//
// # Resumption with return
//
// Resumption with return completion is handled in js::AsyncGeneratorResumeNext.
//
// If the generator is suspended, it doesn't immediately resume the generator
// script itself, but handles implicit `await` it in
// js::AsyncGeneratorResumeNext.
// (See PromiseHandlerAsyncGeneratorYieldReturnAwaitedFulfilled and
// PromiseHandlerAsyncGeneratorYieldReturnAwaitedRejected), and resumes the
// generator with the result of await.
// And the return completion is finally handled in JSOp::CheckResumeKind
// after JSOp::Yield.
//
// This corresponds to AsyncGeneratorYield step 8.
//
// AsyncGeneratorYield ( value )
// https://tc39.es/ecma262/#sec-asyncgeneratoryield
//
//   8. Set the code evaluation state of genContext such that when evaluation
//      is resumed with a Completion resumptionValue the following steps will
//      be performed:
//     ..
//     b. Let awaited be Await(resumptionValue.[[Value]]).
//     c. If awaited.[[Type]] is throw, return Completion(awaited).
//     d. Assert: awaited.[[Type]] is normal.
//     e. Return Completion { [[Type]]: return, [[Value]]: awaited.[[Value]],
//        [[Target]]: empty }.
//
// If the generator is already completed, it awaits on the return value,
// (See PromiseHandlerAsyncGeneratorResumeNextReturnFulfilled and
//  PromiseHandlerAsyncGeneratorResumeNextReturnRejected), and resolves the
// request's promise with the value.
//
// It corresponds to AsyncGeneratorResumeNext step 10.b.i.
//
// AsyncGeneratorResumeNext ( generator )
// https://tc39.es/ecma262/#sec-asyncgeneratorresumenext
//
//   10. If completion is an abrupt completion, then
//     ..
//     b. If state is completed, then
//       i. If completion.[[Type]] is return, then
//         1. Set generator.[[AsyncGeneratorState]] to awaiting-return.
//         2. Let promise be ? PromiseResolve(%Promise%, completion.[[Value]]).
//         3. Let stepsFulfilled be the algorithm steps defined in
//            AsyncGeneratorResumeNext Return Processor Fulfilled Functions.
//         4. Let onFulfilled be ! CreateBuiltinFunction(stepsFulfilled, «
//            [[Generator]] »).
//         5. Set onFulfilled.[[Generator]] to generator.
//         6. Let stepsRejected be the algorithm steps defined in
//            AsyncGeneratorResumeNext Return Processor Rejected Functions.
//         7. Let onRejected be ! CreateBuiltinFunction(stepsRejected, «
//            [[Generator]] »).
//         8. Set onRejected.[[Generator]] to generator.
//         9. Perform ! PerformPromiseThen(promise, onFulfilled, onRejected).
//         10. Return undefined.
//

namespace js {

class AsyncGeneratorObject;
enum class CompletionKind : uint8_t;

extern const JSClass AsyncGeneratorFunctionClass;

[[nodiscard]] bool AsyncGeneratorPromiseReactionJob(
    JSContext* cx, PromiseHandler handler,
    Handle<AsyncGeneratorObject*> generator, HandleValue argument);

bool AsyncGeneratorNext(JSContext* cx, unsigned argc, Value* vp);
bool AsyncGeneratorReturn(JSContext* cx, unsigned argc, Value* vp);
bool AsyncGeneratorThrow(JSContext* cx, unsigned argc, Value* vp);

// AsyncGeneratorRequest record in the spec.
// Stores the info from AsyncGenerator#{next,return,throw}.
//
// This object is reused across multiple requests as an optimization, and
// stored in the Slot_CachedRequest slot.
class AsyncGeneratorRequest : public NativeObject {
 private:
  enum AsyncGeneratorRequestSlots {
    // Int32 value with CompletionKind.
    //   Normal: next
    //   Return: return
    //   Throw:  throw
    Slot_CompletionKind = 0,

    // The value passed to AsyncGenerator#{next,return,throw}.
    Slot_CompletionValue,

    // The promise returned by AsyncGenerator#{next,return,throw}.
    Slot_Promise,

    Slots,
  };

  void init(CompletionKind completionKind, const Value& completionValue,
            PromiseObject* promise) {
    setFixedSlot(Slot_CompletionKind,
                 Int32Value(static_cast<int32_t>(completionKind)));
    setFixedSlot(Slot_CompletionValue, completionValue);
    setFixedSlot(Slot_Promise, ObjectValue(*promise));
  }

  // Clear the request data for reuse.
  void clearData() {
    setFixedSlot(Slot_CompletionValue, NullValue());
    setFixedSlot(Slot_Promise, NullValue());
  }

  friend AsyncGeneratorObject;

 public:
  static const JSClass class_;

  static AsyncGeneratorRequest* create(JSContext* cx,
                                       CompletionKind completionKind,
                                       HandleValue completionValue,
                                       Handle<PromiseObject*> promise);

  CompletionKind completionKind() const {
    return static_cast<CompletionKind>(
        getFixedSlot(Slot_CompletionKind).toInt32());
  }
  JS::Value completionValue() const {
    return getFixedSlot(Slot_CompletionValue);
  }
  PromiseObject* promise() const {
    return &getFixedSlot(Slot_Promise).toObject().as<PromiseObject>();
  }
};

class AsyncGeneratorObject : public AbstractGeneratorObject {
 private:
  enum AsyncGeneratorObjectSlots {
    // Int32 value containing one of the |State| fields from below.
    Slot_State = AbstractGeneratorObject::RESERVED_SLOTS,

    // * null value if this async generator has no requests
    // * AsyncGeneratorRequest if this async generator has only one request
    // * list object if this async generator has 2 or more requests
    Slot_QueueOrRequest,

    // Cached AsyncGeneratorRequest for later use.
    // undefined if there's no cache.
    Slot_CachedRequest,

    Slots
  };

 public:
  enum State {
    // "suspendedStart" in the spec.
    // Suspended after invocation.
    State_SuspendedStart,

    // "suspendedYield" in the spec
    // Suspended with `yield` expression.
    State_SuspendedYield,

    // "executing" in the spec.
    // Resumed from initial suspend or yield, and either running the script
    // or awaiting for `await` expression.
    State_Executing,

    // Part of "executing" in the spec.
    // Awaiting on the value passed by AsyncGenerator#return which is called
    // while executing.
    State_AwaitingYieldReturn,

    // "awaiting-return" in the spec.
    // Awaiting on the value passed by AsyncGenerator#return which is called
    // after completed.
    State_AwaitingReturn,

    // "completed" in the spec.
    // The generator is completed.
    State_Completed
  };

  State state() const {
    return static_cast<State>(getFixedSlot(Slot_State).toInt32());
  }
  void setState(State state_) { setFixedSlot(Slot_State, Int32Value(state_)); }

 private:
  // Queue is implemented in 2 ways.  If only one request is queued ever,
  // request is stored directly to the slot.  Once 2 requests are queued, a
  // list is created and requests are appended into it, and the list is
  // stored to the slot.

  bool isSingleQueue() const {
    return getFixedSlot(Slot_QueueOrRequest).isNull() ||
           getFixedSlot(Slot_QueueOrRequest)
               .toObject()
               .is<AsyncGeneratorRequest>();
  }
  bool isSingleQueueEmpty() const {
    return getFixedSlot(Slot_QueueOrRequest).isNull();
  }
  void setSingleQueueRequest(AsyncGeneratorRequest* request) {
    setFixedSlot(Slot_QueueOrRequest, ObjectValue(*request));
  }
  void clearSingleQueueRequest() {
    setFixedSlot(Slot_QueueOrRequest, NullValue());
  }
  AsyncGeneratorRequest* singleQueueRequest() const {
    return &getFixedSlot(Slot_QueueOrRequest)
                .toObject()
                .as<AsyncGeneratorRequest>();
  }

  ListObject* queue() const {
    return &getFixedSlot(Slot_QueueOrRequest).toObject().as<ListObject>();
  }
  void setQueue(ListObject* queue_) {
    setFixedSlot(Slot_QueueOrRequest, ObjectValue(*queue_));
  }

 public:
  static const JSClass class_;
  static const JSClassOps classOps_;

  static AsyncGeneratorObject* create(JSContext* cx, HandleFunction asyncGen);

  bool isSuspendedStart() const { return state() == State_SuspendedStart; }
  bool isSuspendedYield() const { return state() == State_SuspendedYield; }
  bool isExecuting() const { return state() == State_Executing; }
  bool isAwaitingYieldReturn() const {
    return state() == State_AwaitingYieldReturn;
  }
  bool isAwaitingReturn() const { return state() == State_AwaitingReturn; }
  bool isCompleted() const { return state() == State_Completed; }

  void setSuspendedStart() { setState(State_SuspendedStart); }
  void setSuspendedYield() { setState(State_SuspendedYield); }
  void setExecuting() { setState(State_Executing); }
  void setAwaitingYieldReturn() { setState(State_AwaitingYieldReturn); }
  void setAwaitingReturn() { setState(State_AwaitingReturn); }
  void setCompleted() { setState(State_Completed); }

  [[nodiscard]] static bool enqueueRequest(
      JSContext* cx, Handle<AsyncGeneratorObject*> generator,
      Handle<AsyncGeneratorRequest*> request);

  static AsyncGeneratorRequest* dequeueRequest(
      JSContext* cx, Handle<AsyncGeneratorObject*> generator);

  static AsyncGeneratorRequest* peekRequest(
      Handle<AsyncGeneratorObject*> generator);

  bool isQueueEmpty() const {
    if (isSingleQueue()) {
      return isSingleQueueEmpty();
    }
    return queue()->getDenseInitializedLength() == 0;
  }

  // This function does either of the following:
  //   * return a cached request object with the slots updated
  //   * create a new request object with the slots set
  static AsyncGeneratorRequest* createRequest(
      JSContext* cx, Handle<AsyncGeneratorObject*> generator,
      CompletionKind completionKind, HandleValue completionValue,
      Handle<PromiseObject*> promise);

  // Stores the given request to the generator's cache after clearing its data
  // slots.  The cached request will be reused in the subsequent createRequest
  // call.
  void cacheRequest(AsyncGeneratorRequest* request) {
    if (hasCachedRequest()) {
      return;
    }

    request->clearData();
    setFixedSlot(Slot_CachedRequest, ObjectValue(*request));
  }

 private:
  bool hasCachedRequest() const {
    return getFixedSlot(Slot_CachedRequest).isObject();
  }

  AsyncGeneratorRequest* takeCachedRequest() {
    auto request = &getFixedSlot(Slot_CachedRequest)
                        .toObject()
                        .as<AsyncGeneratorRequest>();
    clearCachedRequest();
    return request;
  }

  void clearCachedRequest() { setFixedSlot(Slot_CachedRequest, NullValue()); }
};

JSObject* CreateAsyncFromSyncIterator(JSContext* cx, HandleObject iter,
                                      HandleValue nextMethod);

class AsyncFromSyncIteratorObject : public NativeObject {
 private:
  enum AsyncFromSyncIteratorObjectSlots {
    // Object that implements the sync iterator protocol.
    Slot_Iterator = 0,

    // The `next` property of the iterator object.
    Slot_NextMethod = 1,

    Slots
  };

  void init(JSObject* iterator, const Value& nextMethod) {
    setFixedSlot(Slot_Iterator, ObjectValue(*iterator));
    setFixedSlot(Slot_NextMethod, nextMethod);
  }

 public:
  static const JSClass class_;

  static JSObject* create(JSContext* cx, HandleObject iter,
                          HandleValue nextMethod);

  JSObject* iterator() const { return &getFixedSlot(Slot_Iterator).toObject(); }

  const Value& nextMethod() const { return getFixedSlot(Slot_NextMethod); }
};

class AsyncIteratorObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass protoClass_;
};

// Iterator Helpers proposal
class AsyncIteratorHelperObject : public NativeObject {
 public:
  static const JSClass class_;

  enum { GeneratorSlot, SlotCount };

  static_assert(GeneratorSlot == ASYNC_ITERATOR_HELPER_GENERATOR_SLOT,
                "GeneratorSlot must match self-hosting define for generator "
                "object slot.");
};

AsyncIteratorHelperObject* NewAsyncIteratorHelper(JSContext* cx);

}  // namespace js

#endif /* vm_AsyncIteration_h */
