/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_AsyncFunction_h
#define vm_AsyncFunction_h

#include "js/Class.h"
#include "vm/AsyncFunctionResolveKind.h"  // AsyncFunctionResolveKind
#include "vm/GeneratorObject.h"
#include "vm/JSObject.h"
#include "vm/PromiseObject.h"

// [SMDOC] Async functions
//
// # Implementation
//
// Async functions are implemented based on generators, in terms of
// suspend/resume.
// Instead of returning the generator object itself, they return the async
// function's result promise to the caller.
//
// The async function's result promise is stored in the generator object
// (js::AsyncFunctionGeneratorObject) and retrieved from it whenever the
// execution needs it.
//
//
// # Start
//
// When an async function is called, it synchronously runs until the first
// `await` or `return`.  This works just like a normal function.
//
// This corresponds to steps 1-3, 5-9 of AsyncFunctionStart.
//
// AsyncFunctionStart ( promiseCapability, asyncFunctionBody )
// https://tc39.es/ecma262/#sec-async-functions-abstract-operations-async-function-start
//
//   1. Let runningContext be the running execution context.
//   2. Let asyncContext be a copy of runningContext.
//   3. NOTE: Copying the execution state is required for the step below to
//      resume its execution. It is ill-defined to resume a currently executing
//      context.
//   ...
//   5. Push asyncContext onto the execution context stack; asyncContext is now
//      the running execution context.
//   6. Resume the suspended evaluation of asyncContext. Let result be the value
//      returned by the resumed computation.
//   7. Assert: When we return here, asyncContext has already been removed from
//      the execution context stack and runningContext is the currently running
//      execution context.
//   8. Assert: result is a normal completion with a value of undefined. The
//      possible sources of completion values are Await or, if the async
//      function doesn't await anything, step 4.g above.
//   9. Return.
//
// Unlike generators, async functions don't contain JSOp::InitialYield and
// don't suspend immediately when call.
//
//
// # Return
//
// Explicit/implicit `return` is implemented with the following bytecode
// sequence:
//
// ```
//   GetAliasedVar ".generator"      # VALUE .generator
//   AsyncResolve 0                  # PROMISE
//   SetRval                         #
//   GetAliasedVar ".generator"      # .generator
//   FinalYieldRval                  #
// ```
//
// JSOp::Resolve (js::AsyncFunctionResolve) resolves the current async
// function's result promise. Then this sets it as the function's return value.
// (The return value is observable if the caller is still on the stack--
// that is, the async function is returning without ever awaiting.
// Otherwise we're returning to the microtask loop, which ignores the
// return value.)
//
// This corresponds to AsyncFunctionStart steps 4.a-e. 4.g.
//
//   4. Set the code evaluation state of asyncContext such that when evaluation
//      is resumed for that execution context the following steps will be
//      performed:
//     a. Let result be the result of evaluating asyncFunctionBody.
//     b. Assert: If we return here, the async function either threw an
//        exception or performed an implicit or explicit return; all awaiting
//        is done.
//     c. Remove asyncContext from the execution context stack and restore the
//        execution context that is at the top of the execution context stack as
//        the running execution context.
//     d. If result.[[Type]] is normal, then
//       i. Perform
//          ! Call(promiseCapability.[[Resolve]], undefined, «undefined»).
//     e. Else if result.[[Type]] is return, then
//       i. Perform
//          ! Call(promiseCapability.[[Resolve]], undefined,
//                 «result.[[Value]]»).
//     ...
//     g. Return.
//
//
// # Throw
//
// The body part of an async function is enclosed by an implicit try-catch
// block, to catch `throw` completion of the function body.
//
// If an exception is thrown by the function body, the catch block catches it
// and rejects the async function's result promise.
//
// If there's an expression in parameters, the entire parameters part is also
// enclosed by a separate implicit try-catch block.
//
// ```
//   Try                             #
//   (parameter expressions here)    #
//   Goto BODY                       #
//
//   JumpTarget from try             #
//   Exception                       # EXCEPTION
//   GetAliasedVar ".generator"      # EXCEPTION .generator
//   AsyncResolve 1                  # PROMISE
//   SetRval                         #
//   GetAliasedVar ".generator"      # .generator
//   FinalYieldRval                  #
//
// BODY:
//   JumpTarget                      #
//   Try                             #
//   (body here)                     #
//
//   JumpTarget from try             #
//   Exception                       # EXCEPTION
//   GetAliasedVar ".generator"      # EXCEPTION .generator
//   AsyncResolve 1                  # PROMISE
//   SetRval                         #
//   GetAliasedVar ".generator"      # .generator
//   FinalYieldRval                  #
// ```
//
// This corresponds to AsyncFunctionStart steps 4.f-g.
//
//   4. ...
//     f. Else,
//       i. Assert: result.[[Type]] is throw.
//       ii. Perform
//           ! Call(promiseCapability.[[Reject]], undefined,
//                  «result.[[Value]]»).
//     g. Return.
//
//
// # Await
//
// `await` is implemented with the following bytecode sequence:
// (ignoring CanSkipAwait for now, see "Optimization for await" section)
//
// ```
//   (operand here)                  # VALUE
//   GetAliasedVar ".generator"      # VALUE .generator
//   AsyncAwait                      # PROMISE
//
//   GetAliasedVar ".generator"      # PROMISE .generator
//   Await 0                         # RVAL GENERATOR RESUMEKIND
//
//   AfterYield                      # RVAL GENERATOR RESUMEKIND
//   CheckResumeKind                 # RVAL
// ```
//
// JSOp::AsyncAwait corresponds to Await steps 1-9, and JSOp::Await corresponds
// to Await steps 10-12 in the spec.
//
// See the next section for JSOp::CheckResumeKind.
//
// After them, the async function is suspended, and if this is the first await
// in the execution, the async function's result promise is returned to the
// caller.
//
// Await
// https://tc39.es/ecma262/#await
//
//   1. Let asyncContext be the running execution context.
//   2. Let promise be ? PromiseResolve(%Promise%, value).
//   3. Let stepsFulfilled be the algorithm steps defined in Await Fulfilled
//      Functions.
//   4. Let onFulfilled be ! CreateBuiltinFunction(stepsFulfilled, «
//      [[AsyncContext]] »).
//   5. Set onFulfilled.[[AsyncContext]] to asyncContext.
//   6. Let stepsRejected be the algorithm steps defined in Await Rejected
//      Functions.
//   7. Let onRejected be ! CreateBuiltinFunction(stepsRejected, «
//      [[AsyncContext]] »).
//   8. Set onRejected.[[AsyncContext]] to asyncContext.
//   9. Perform ! PerformPromiseThen(promise, onFulfilled, onRejected).
//   10. Remove asyncContext from the execution context stack and restore the
//       execution context that is at the top of the execution context stack as
//       the running execution context.
//   11. Set the code evaluation state of asyncContext such that when evaluation
//       is resumed with a Completion completion, the following steps of the
//       algorithm that invoked Await will be performed, with completion
//       available.
//   12. Return.
//   13. NOTE: This returns to the evaluation of the operation that had most
//       previously resumed evaluation of asyncContext.
//
// (See comments above AsyncAwait and Await in js/src/vm/Opcodes.h for more
//  details)
//
//
// # Reaction jobs and resume after await
//
// When an async function performs `await` and the operand becomes settled, a
// new reaction job for the operand is enqueued to the job queue.
//
// The reaction record for the job is marked as "this is for async function"
// (see js::AsyncFunctionAwait), and handled specially in
// js::PromiseReactionJob.
//
// When the await operand resolves (either with fulfillment or rejection),
// the async function is resumed from the job queue, by calling
// js::AsyncFunctionAwaitedFulfilled or js::AsyncFunctionAwaitedRejected
// from js::AsyncFunctionPromiseReactionJob.
//
// The execution resumes from JSOp::AfterYield, with the resolved value
// and the resume kind, either normal or throw, corresponds to fulfillment or
// rejection, on the stack.
//
// The resume kind is handled by JSOp::CheckResumeKind after that.
//
// If the resume kind is normal (=fulfillment), the async function resumes
// the execution with the resolved value as the result of `await`.
//
// If the resume kind is throw (=rejection), it throws the resolved value,
// and it will be caught by the try-catch explained above.
//
//
// # Optimization for await
//
// Suspending the execution and going into the embedding's job queue is slow
// and hard to optimize.
//
// If the following conditions are met, we don't have to perform the above
// but just use the await operand as the result of await.
//
//   1. The await operand is either non-promise or already-fulfilled promise,
//      so that the result value is already known
//   2. There's no jobs in the job queue,
//      so that we don't have to perform other jobs before resuming from
//      await
//   3. Promise constructor/prototype are not modified,
//      so that the optimization isn't visible to the user code
//
// This is implemented by the following bytecode sequence:
//
// ```
//   (operand here)                  # VALUE
//
//   CanSkipAwait                    # VALUE, CAN_SKIP
//   MaybeExtractAwaitValue          # VALUE_OR_RVAL, CAN_SKIP
//   JumpIfTrue END                  # VALUE
//
//   JumpTarget                      # VALUE
//   GetAliasedVar ".generator"      # VALUE .generator
//   Await 0                         # RVAL GENERATOR RESUMEKIND
//   AfterYield                      # RVAL GENERATOR RESUMEKIND
//   CheckResumeKind                 # RVAL
//
// END:
//   JumpTarget                      # RVAL
// ```
//
// JSOp::CanSkipAwait checks the above conditions. MaybeExtractAwaitValue will
// replace Value if it can be skipped, and then the await is jumped over.

namespace js {

class AsyncFunctionGeneratorObject;

extern const JSClass AsyncFunctionClass;

// Resume the async function when the `await` operand resolves.
// Split into two functions depending on whether the awaited value was
// fulfilled or rejected.
[[nodiscard]] bool AsyncFunctionAwaitedFulfilled(
    JSContext* cx, Handle<AsyncFunctionGeneratorObject*> generator,
    HandleValue value);

[[nodiscard]] bool AsyncFunctionAwaitedRejected(
    JSContext* cx, Handle<AsyncFunctionGeneratorObject*> generator,
    HandleValue reason);

// Resolve the async function's promise object with the given value and then
// return the promise object.
JSObject* AsyncFunctionResolve(JSContext* cx,
                               Handle<AsyncFunctionGeneratorObject*> generator,
                               HandleValue valueOrReason,
                               AsyncFunctionResolveKind resolveKind);

class AsyncFunctionGeneratorObject : public AbstractGeneratorObject {
 public:
  enum {
    PROMISE_SLOT = AbstractGeneratorObject::RESERVED_SLOTS,

    RESERVED_SLOTS
  };

  static const JSClass class_;
  static const JSClassOps classOps_;

  static AsyncFunctionGeneratorObject* create(JSContext* cx,
                                              HandleFunction asyncGen);

  static AsyncFunctionGeneratorObject* create(JSContext* cx,
                                              Handle<ModuleObject*> module);

  PromiseObject* promise() {
    return &getFixedSlot(PROMISE_SLOT).toObject().as<PromiseObject>();
  }
};

}  // namespace js

#endif /* vm_AsyncFunction_h */
