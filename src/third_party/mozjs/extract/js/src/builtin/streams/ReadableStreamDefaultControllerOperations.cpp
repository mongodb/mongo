/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Readable stream default controller abstract operations. */

#include "builtin/streams/ReadableStreamDefaultControllerOperations.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include "jsfriendapi.h"  // js::AssertSameCompartment

#include "builtin/Stream.h"  // js::ReadableByteStreamControllerClearPendingPullIntos
#include "builtin/streams/MiscellaneousOperations.h"  // js::CreateAlgorithmFromUnderlyingMethod, js::InvokeOrNoop, js::IsMaybeWrapped
#include "builtin/streams/QueueWithSizes.h"  // js::EnqueueValueWithSize, js::ResetQueue
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStream{,Default}Controller, js::ReadableByteStreamController, js::ReadableStreamControllerStart{,Failed}Handler
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStream{CloseInternal,ErrorInternal,FulfillReadOrReadIntoRequest,GetNumReadRequests}
#include "builtin/streams/ReadableStreamOperations.h"  // js::ReadableStreamTee_Pull, js::SetUpReadableStreamDefaultController
#include "builtin/streams/TeeState.h"  // js::TeeState
#include "js/CallArgs.h"               // JS::CallArgs{,FromVp}
#include "js/Promise.h"                // JS::AddPromiseReactions
#include "js/RootingAPI.h"             // JS::Handle, JS::Rooted
#include "js/Stream.h"                 // JS::ReadableStreamUnderlyingSource
#include "js/Value.h"  // JS::{,Int32,Object}Value, JS::UndefinedHandleValue
#include "vm/Compartment.h"  // JS::Compartment
#include "vm/Interpreter.h"  // js::Call, js::GetAndClearExceptionAndStack
#include "vm/JSContext.h"    // JSContext
#include "vm/JSObject.h"     // JSObject
#include "vm/List.h"         // js::ListObject
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined
#include "vm/Runtime.h"        // JSAtomState
#include "vm/SavedFrame.h"  // js::SavedFrame

#include "builtin/HandlerFunction-inl.h"                  // js::NewHandler
#include "builtin/streams/MiscellaneousOperations-inl.h"  // js::PromiseCall
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::UnwrapCalleeSlot
#include "vm/JSContext-inl.h"    // JSContext::check
#include "vm/JSObject-inl.h"     // js::IsCallable, js::NewBuiltinClassInstance
#include "vm/Realm-inl.h"        // js::AutoRealm

using js::ReadableByteStreamController;
using js::ReadableStream;
using js::ReadableStreamController;
using js::ReadableStreamControllerCallPullIfNeeded;
using js::ReadableStreamControllerError;
using js::ReadableStreamGetNumReadRequests;
using js::UnwrapCalleeSlot;

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::Rooted;
using JS::UndefinedHandleValue;
using JS::Value;

/*** 3.10. Readable stream default controller abstract operations ***********/

// Streams spec, 3.10.1. IsReadableStreamDefaultController ( x )
// Implemented via is<ReadableStreamDefaultController>()

/**
 * Streams spec, 3.10.2 and 3.13.3. step 7:
 *      Upon fulfillment of pullPromise, [...]
 */
static bool ControllerPullHandler(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<ReadableStreamController*> unwrappedController(
      cx, UnwrapCalleeSlot<ReadableStreamController>(cx, args, 0));
  if (!unwrappedController) {
    return false;
  }

  bool pullAgain = unwrappedController->pullAgain();

  // Step a: Set controller.[[pulling]] to false.
  // Step b.i: Set controller.[[pullAgain]] to false.
  unwrappedController->clearPullFlags();

  // Step b: If controller.[[pullAgain]] is true,
  if (pullAgain) {
    // Step ii: Perform
    //          ! ReadableStreamDefaultControllerCallPullIfNeeded(controller)
    //          (or ReadableByteStreamControllerCallPullIfNeeded(controller)).
    if (!ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController)) {
      return false;
    }
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 3.10.2 and 3.13.3. step 8:
 * Upon rejection of pullPromise with reason e,
 */
static bool ControllerPullFailedHandler(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Handle<Value> e = args.get(0);

  Rooted<ReadableStreamController*> controller(
      cx, UnwrapCalleeSlot<ReadableStreamController>(cx, args, 0));
  if (!controller) {
    return false;
  }

  // Step a: Perform ! ReadableStreamDefaultControllerError(controller, e).
  //         (ReadableByteStreamControllerError in 3.12.3.)
  if (!ReadableStreamControllerError(cx, controller, e)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool ReadableStreamControllerShouldCallPull(
    ReadableStreamController* unwrappedController);

/**
 * Streams spec, 3.10.2
 *      ReadableStreamDefaultControllerCallPullIfNeeded ( controller )
 * Streams spec, 3.13.3.
 *      ReadableByteStreamControllerCallPullIfNeeded ( controller )
 */
[[nodiscard]] bool js::ReadableStreamControllerCallPullIfNeeded(
    JSContext* cx, Handle<ReadableStreamController*> unwrappedController) {
  // Step 1: Let shouldPull be
  //         ! ReadableStreamDefaultControllerShouldCallPull(controller).
  // (ReadableByteStreamDefaultControllerShouldCallPull in 3.13.3.)
  bool shouldPull = ReadableStreamControllerShouldCallPull(unwrappedController);

  // Step 2: If shouldPull is false, return.
  if (!shouldPull) {
    return true;
  }

  // Step 3: If controller.[[pulling]] is true,
  if (unwrappedController->pulling()) {
    // Step a: Set controller.[[pullAgain]] to true.
    unwrappedController->setPullAgain();

    // Step b: Return.
    return true;
  }

  // Step 4: Assert: controller.[[pullAgain]] is false.
  MOZ_ASSERT(!unwrappedController->pullAgain());

  // Step 5: Set controller.[[pulling]] to true.
  unwrappedController->setPulling();

  // We use this variable in step 7. For ease of error-handling, we wrap it
  // early.
  Rooted<JSObject*> wrappedController(cx, unwrappedController);
  if (!cx->compartment()->wrap(cx, &wrappedController)) {
    return false;
  }

  // Step 6: Let pullPromise be the result of performing
  //         controller.[[pullAlgorithm]].
  // Our representation of pull algorithms is a bit awkward, for performance,
  // so we must figure out which algorithm is being invoked.
  Rooted<JSObject*> pullPromise(cx);
  Rooted<Value> unwrappedUnderlyingSource(
      cx, unwrappedController->underlyingSource());

  if (IsMaybeWrapped<TeeState>(unwrappedUnderlyingSource)) {
    // The pull algorithm given in ReadableStreamTee step 12.
    MOZ_ASSERT(unwrappedUnderlyingSource.toObject().is<TeeState>(),
               "tee streams and controllers are always same-compartment with "
               "the TeeState object");
    Rooted<TeeState*> unwrappedTeeState(
        cx, &unwrappedUnderlyingSource.toObject().as<TeeState>());
    pullPromise = ReadableStreamTee_Pull(cx, unwrappedTeeState);
  } else if (unwrappedController->hasExternalSource()) {
    // An embedding-provided pull algorithm.
    {
      AutoRealm ar(cx, unwrappedController);
      JS::ReadableStreamUnderlyingSource* source =
          unwrappedController->externalSource();
      Rooted<ReadableStream*> stream(cx, unwrappedController->stream());
      double desiredSize =
          ReadableStreamControllerGetDesiredSizeUnchecked(unwrappedController);
      source->requestData(cx, stream, desiredSize);
    }
    pullPromise = PromiseResolvedWithUndefined(cx);
  } else {
    // The pull algorithm created in
    // SetUpReadableStreamDefaultControllerFromUnderlyingSource step 4.
    Rooted<Value> unwrappedPullMethod(cx, unwrappedController->pullMethod());
    if (unwrappedPullMethod.isUndefined()) {
      // CreateAlgorithmFromUnderlyingMethod step 7.
      pullPromise = PromiseResolvedWithUndefined(cx);
    } else {
      // CreateAlgorithmFromUnderlyingMethod step 6.b.i.
      {
        AutoRealm ar(cx, unwrappedController);

        // |unwrappedPullMethod| and |unwrappedUnderlyingSource| come directly
        // from |unwrappedController| slots so must be same-compartment with it.
        cx->check(unwrappedPullMethod);
        cx->check(unwrappedUnderlyingSource);

        Rooted<Value> controller(cx, ObjectValue(*unwrappedController));
        cx->check(controller);

        pullPromise = PromiseCall(cx, unwrappedPullMethod,
                                  unwrappedUnderlyingSource, controller);
        if (!pullPromise) {
          return false;
        }
      }
      if (!cx->compartment()->wrap(cx, &pullPromise)) {
        return false;
      }
    }
  }
  if (!pullPromise) {
    return false;
  }

  // Step 7: Upon fulfillment of pullPromise, [...]
  // Step 8. Upon rejection of pullPromise with reason e, [...]
  Rooted<JSObject*> onPullFulfilled(
      cx, NewHandler(cx, ControllerPullHandler, wrappedController));
  if (!onPullFulfilled) {
    return false;
  }
  Rooted<JSObject*> onPullRejected(
      cx, NewHandler(cx, ControllerPullFailedHandler, wrappedController));
  if (!onPullRejected) {
    return false;
  }
  return JS::AddPromiseReactions(cx, pullPromise, onPullFulfilled,
                                 onPullRejected);
}

/**
 * Streams spec, 3.10.3.
 *      ReadableStreamDefaultControllerShouldCallPull ( controller )
 * Streams spec, 3.13.25.
 *      ReadableByteStreamControllerShouldCallPull ( controller )
 */
static bool ReadableStreamControllerShouldCallPull(
    ReadableStreamController* unwrappedController) {
  // Step 1: Let stream be controller.[[controlledReadableStream]]
  //         (or [[controlledReadableByteStream]]).
  ReadableStream* unwrappedStream = unwrappedController->stream();

  // 3.10.3. Step 2:
  //      If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller)
  //      is false, return false.
  // This turns out to be the same as 3.13.25 steps 2-3.

  // 3.13.25 Step 2: If stream.[[state]] is not "readable", return false.
  if (!unwrappedStream->readable()) {
    return false;
  }

  // 3.13.25 Step 3: If controller.[[closeRequested]] is true, return false.
  if (unwrappedController->closeRequested()) {
    return false;
  }

  // Step 3 (or 4):
  //      If controller.[[started]] is false, return false.
  if (!unwrappedController->started()) {
    return false;
  }

  // 3.10.3.
  // Step 4: If ! IsReadableStreamLocked(stream) is true and
  //      ! ReadableStreamGetNumReadRequests(stream) > 0, return true.
  //
  // 3.13.25.
  // Step 5: If ! ReadableStreamHasDefaultReader(stream) is true and
  //         ! ReadableStreamGetNumReadRequests(stream) > 0, return true.
  // Step 6: If ! ReadableStreamHasBYOBReader(stream) is true and
  //         ! ReadableStreamGetNumReadIntoRequests(stream) > 0, return true.
  //
  // All of these amount to the same thing in this implementation:
  if (unwrappedStream->locked() &&
      ReadableStreamGetNumReadRequests(unwrappedStream) > 0) {
    return true;
  }

  // Step 5 (or 7):
  //      Let desiredSize be
  //      ! ReadableStreamDefaultControllerGetDesiredSize(controller).
  //      (ReadableByteStreamControllerGetDesiredSize in 3.13.25.)
  double desiredSize =
      ReadableStreamControllerGetDesiredSizeUnchecked(unwrappedController);

  // Step 6 (or 8): Assert: desiredSize is not null (implicit).
  // Step 7 (or 9): If desiredSize > 0, return true.
  // Step 8 (or 10): Return false.
  return desiredSize > 0;
}

/**
 * Streams spec, 3.10.4.
 *      ReadableStreamDefaultControllerClearAlgorithms ( controller )
 * and 3.13.4.
 *      ReadableByteStreamControllerClearAlgorithms ( controller )
 */
void js::ReadableStreamControllerClearAlgorithms(
    Handle<ReadableStreamController*> controller) {
  // Step 1: Set controller.[[pullAlgorithm]] to undefined.
  // Step 2: Set controller.[[cancelAlgorithm]] to undefined.
  // (In this implementation, the UnderlyingSource slot is part of the
  // representation of these algorithms.)
  controller->setPullMethod(UndefinedHandleValue);
  controller->setCancelMethod(UndefinedHandleValue);
  ReadableStreamController::clearUnderlyingSource(controller);

  // Step 3 (of 3.10.4 only) : Set controller.[[strategySizeAlgorithm]] to
  // undefined.
  if (controller->is<ReadableStreamDefaultController>()) {
    controller->as<ReadableStreamDefaultController>().setStrategySize(
        UndefinedHandleValue);
  }
}

/**
 * Streams spec, 3.10.5. ReadableStreamDefaultControllerClose ( controller )
 */
[[nodiscard]] bool js::ReadableStreamDefaultControllerClose(
    JSContext* cx,
    Handle<ReadableStreamDefaultController*> unwrappedController) {
  // Step 1: Let stream be controller.[[controlledReadableStream]].
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 2: Assert:
  //         ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller)
  //         is true.
  MOZ_ASSERT(!unwrappedController->closeRequested());
  MOZ_ASSERT(unwrappedStream->readable());

  // Step 3: Set controller.[[closeRequested]] to true.
  unwrappedController->setCloseRequested();

  // Step 4: If controller.[[queue]] is empty,
  Rooted<ListObject*> unwrappedQueue(cx, unwrappedController->queue());
  if (unwrappedQueue->length() == 0) {
    // Step a: Perform
    //         ! ReadableStreamDefaultControllerClearAlgorithms(controller).
    ReadableStreamControllerClearAlgorithms(unwrappedController);

    // Step b: Perform ! ReadableStreamClose(stream).
    return ReadableStreamCloseInternal(cx, unwrappedStream);
  }

  return true;
}

/**
 * Streams spec, 3.10.6.
 *      ReadableStreamDefaultControllerEnqueue ( controller, chunk )
 */
[[nodiscard]] bool js::ReadableStreamDefaultControllerEnqueue(
    JSContext* cx, Handle<ReadableStreamDefaultController*> unwrappedController,
    Handle<Value> chunk) {
  AssertSameCompartment(cx, chunk);

  // Step 1: Let stream be controller.[[controlledReadableStream]].
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 2: Assert:
  //      ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller) is
  //      true.
  MOZ_ASSERT(!unwrappedController->closeRequested());
  MOZ_ASSERT(unwrappedStream->readable());

  // Step 3: If ! IsReadableStreamLocked(stream) is true and
  //         ! ReadableStreamGetNumReadRequests(stream) > 0, perform
  //         ! ReadableStreamFulfillReadRequest(stream, chunk, false).
  if (unwrappedStream->locked() &&
      ReadableStreamGetNumReadRequests(unwrappedStream) > 0) {
    if (!ReadableStreamFulfillReadOrReadIntoRequest(cx, unwrappedStream, chunk,
                                                    false)) {
      return false;
    }
  } else {
    // Step 4: Otherwise,
    // Step a: Let result be the result of performing
    //         controller.[[strategySizeAlgorithm]], passing in chunk, and
    //         interpreting the result as an ECMAScript completion value.
    // Step c: (on success) Let chunkSize be result.[[Value]].
    Rooted<Value> chunkSize(cx, Int32Value(1));
    bool success = true;
    Rooted<Value> strategySize(cx, unwrappedController->strategySize());
    if (!strategySize.isUndefined()) {
      if (!cx->compartment()->wrap(cx, &strategySize)) {
        return false;
      }
      success = Call(cx, strategySize, UndefinedHandleValue, chunk, &chunkSize);
    }

    // Step d: Let enqueueResult be
    //         EnqueueValueWithSize(controller, chunk, chunkSize).
    if (success) {
      success = EnqueueValueWithSize(cx, unwrappedController, chunk, chunkSize);
    }

    // Step b: If result is an abrupt completion,
    // and
    // Step e: If enqueueResult is an abrupt completion,
    if (!success) {
      Rooted<Value> exn(cx);
      Rooted<SavedFrame*> stack(cx);
      if (!cx->isExceptionPending() ||
          !GetAndClearExceptionAndStack(cx, &exn, &stack)) {
        // Uncatchable error. Die immediately without erroring the
        // stream.
        return false;
      }

      // Step b.i: Perform ! ReadableStreamDefaultControllerError(
      //           controller, result.[[Value]]).
      // Step e.i: Perform ! ReadableStreamDefaultControllerError(
      //           controller, enqueueResult.[[Value]]).
      if (!ReadableStreamControllerError(cx, unwrappedController, exn)) {
        return false;
      }

      // Step b.ii: Return result.
      // Step e.ii: Return enqueueResult.
      // (I.e., propagate the exception.)
      cx->setPendingException(exn, stack);
      return false;
    }
  }

  // Step 5: Perform
  //         ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
  return ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController);
}

/**
 * Streams spec, 3.10.7. ReadableStreamDefaultControllerError ( controller, e )
 * Streams spec, 3.13.11. ReadableByteStreamControllerError ( controller, e )
 */
[[nodiscard]] bool js::ReadableStreamControllerError(
    JSContext* cx, Handle<ReadableStreamController*> unwrappedController,
    Handle<Value> e) {
  MOZ_ASSERT(!cx->isExceptionPending());
  AssertSameCompartment(cx, e);

  // Step 1: Let stream be controller.[[controlledReadableStream]]
  //         (or controller.[[controlledReadableByteStream]]).
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 2: If stream.[[state]] is not "readable", return.
  if (!unwrappedStream->readable()) {
    return true;
  }

  // Step 3 of 3.13.10:
  // Perform ! ReadableByteStreamControllerClearPendingPullIntos(controller).
  if (unwrappedController->is<ReadableByteStreamController>()) {
    Rooted<ReadableByteStreamController*> unwrappedByteStreamController(
        cx, &unwrappedController->as<ReadableByteStreamController>());
    if (!ReadableByteStreamControllerClearPendingPullIntos(
            cx, unwrappedByteStreamController)) {
      return false;
    }
  }

  // Step 3 (or 4): Perform ! ResetQueue(controller).
  if (!ResetQueue(cx, unwrappedController)) {
    return false;
  }

  // Step 4 (or 5):
  //      Perform ! ReadableStreamDefaultControllerClearAlgorithms(controller)
  //      (or ReadableByteStreamControllerClearAlgorithms(controller)).
  ReadableStreamControllerClearAlgorithms(unwrappedController);

  // Step 5 (or 6): Perform ! ReadableStreamError(stream, e).
  return ReadableStreamErrorInternal(cx, unwrappedStream, e);
}

/**
 * Streams spec, 3.10.8.
 *      ReadableStreamDefaultControllerGetDesiredSize ( controller )
 * Streams spec 3.13.14.
 *      ReadableByteStreamControllerGetDesiredSize ( controller )
 */
[[nodiscard]] double js::ReadableStreamControllerGetDesiredSizeUnchecked(
    ReadableStreamController* controller) {
  // Steps 1-4 done at callsites, so only assert that they have been done.
#if DEBUG
  ReadableStream* stream = controller->stream();
  MOZ_ASSERT(!(stream->errored() || stream->closed()));
#endif  // DEBUG

  // Step 5: Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
  return controller->strategyHWM() - controller->queueTotalSize();
}

/**
 * Streams spec, 3.10.11.
 *      SetUpReadableStreamDefaultController(stream, controller,
 *          startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark,
 *          sizeAlgorithm )
 *
 * The standard algorithm takes a `controller` argument which must be a new,
 * blank object. This implementation creates a new controller instead.
 *
 * In the spec, three algorithms (startAlgorithm, pullAlgorithm,
 * cancelAlgorithm) are passed as arguments to this routine. This
 * implementation passes these "algorithms" as data, using four arguments:
 * sourceAlgorithms, underlyingSource, pullMethod, and cancelMethod. The
 * sourceAlgorithms argument tells how to interpret the other three:
 *
 * -   SourceAlgorithms::Script - We're creating a stream from a JS source.
 *     The caller is `new ReadableStream(underlyingSource)` or
 *     `JS::NewReadableDefaultStreamObject`. `underlyingSource` is the
 *     source; `pullMethod` and `cancelMethod` are its .pull and
 *     .cancel methods, which the caller has already extracted and
 *     type-checked: each one must be either a callable JS object or undefined.
 *
 *     Script streams use the start/pull/cancel algorithms defined in
 *     3.10.12. SetUpReadableStreamDefaultControllerFromUnderlyingSource, which
 *     call JS methods of the underlyingSource.
 *
 * -   SourceAlgorithms::Tee - We're creating a tee stream. `underlyingSource`
 *     is a TeeState object. `pullMethod` and `cancelMethod` are undefined.
 *
 *     Tee streams use the start/pull/cancel algorithms given in
 *     3.4.10. ReadableStreamTee.
 *
 * Note: All arguments must be same-compartment with cx. ReadableStream
 * controllers are always created in the same compartment as the stream.
 */
[[nodiscard]] bool js::SetUpReadableStreamDefaultController(
    JSContext* cx, Handle<ReadableStream*> stream,
    SourceAlgorithms sourceAlgorithms, Handle<Value> underlyingSource,
    Handle<Value> pullMethod, Handle<Value> cancelMethod, double highWaterMark,
    Handle<Value> size) {
  cx->check(stream, underlyingSource, size);
  MOZ_ASSERT(pullMethod.isUndefined() || IsCallable(pullMethod));
  MOZ_ASSERT(cancelMethod.isUndefined() || IsCallable(cancelMethod));
  MOZ_ASSERT_IF(sourceAlgorithms != SourceAlgorithms::Script,
                pullMethod.isUndefined());
  MOZ_ASSERT_IF(sourceAlgorithms != SourceAlgorithms::Script,
                cancelMethod.isUndefined());
  MOZ_ASSERT(highWaterMark >= 0);
  MOZ_ASSERT(size.isUndefined() || IsCallable(size));

  // Done elsewhere in the standard: Create the new controller.
  Rooted<ReadableStreamDefaultController*> controller(
      cx, NewBuiltinClassInstance<ReadableStreamDefaultController>(cx));
  if (!controller) {
    return false;
  }

  // Step 1: Assert: stream.[[readableStreamController]] is undefined.
  MOZ_ASSERT(!stream->hasController());

  // Step 2: Set controller.[[controlledReadableStream]] to stream.
  controller->setStream(stream);

  // Step 3: Set controller.[[queue]] and controller.[[queueTotalSize]] to
  //         undefined (implicit), then perform ! ResetQueue(controller).
  if (!ResetQueue(cx, controller)) {
    return false;
  }

  // Step 4: Set controller.[[started]], controller.[[closeRequested]],
  //         controller.[[pullAgain]], and controller.[[pulling]] to false.
  controller->setFlags(0);

  // Step 5: Set controller.[[strategySizeAlgorithm]] to sizeAlgorithm
  //         and controller.[[strategyHWM]] to highWaterMark.
  controller->setStrategySize(size);
  controller->setStrategyHWM(highWaterMark);

  // Step 6: Set controller.[[pullAlgorithm]] to pullAlgorithm.
  // (In this implementation, the pullAlgorithm is determined by the
  // underlyingSource in combination with the pullMethod field.)
  controller->setUnderlyingSource(underlyingSource);
  controller->setPullMethod(pullMethod);

  // Step 7: Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
  controller->setCancelMethod(cancelMethod);

  // Step 8: Set stream.[[readableStreamController]] to controller.
  stream->setController(controller);

  // Step 9: Let startResult be the result of performing startAlgorithm.
  Rooted<Value> startResult(cx);
  if (sourceAlgorithms == SourceAlgorithms::Script) {
    Rooted<Value> controllerVal(cx, ObjectValue(*controller));
    if (!InvokeOrNoop(cx, underlyingSource, cx->names().start, controllerVal,
                      &startResult)) {
      return false;
    }
  }

  // Step 10: Let startPromise be a promise resolved with startResult.
  Rooted<JSObject*> startPromise(
      cx, PromiseObject::unforgeableResolve(cx, startResult));
  if (!startPromise) {
    return false;
  }

  // Step 11: Upon fulfillment of startPromise, [...]
  // Step 12: Upon rejection of startPromise with reason r, [...]
  Rooted<JSObject*> onStartFulfilled(
      cx, NewHandler(cx, ReadableStreamControllerStartHandler, controller));
  if (!onStartFulfilled) {
    return false;
  }
  Rooted<JSObject*> onStartRejected(
      cx,
      NewHandler(cx, ReadableStreamControllerStartFailedHandler, controller));
  if (!onStartRejected) {
    return false;
  }
  if (!JS::AddPromiseReactions(cx, startPromise, onStartFulfilled,
                               onStartRejected)) {
    return false;
  }

  return true;
}

/**
 * Streams spec, 3.10.12.
 *      SetUpReadableStreamDefaultControllerFromUnderlyingSource( stream,
 *          underlyingSource, highWaterMark, sizeAlgorithm )
 */
[[nodiscard]] bool js::SetUpReadableStreamDefaultControllerFromUnderlyingSource(
    JSContext* cx, Handle<ReadableStream*> stream,
    Handle<Value> underlyingSource, double highWaterMark,
    Handle<Value> sizeAlgorithm) {
  // Step 1: Assert: underlyingSource is not undefined.
  MOZ_ASSERT(!underlyingSource.isUndefined());

  // Step 2: Let controller be ObjectCreate(the original value of
  //         ReadableStreamDefaultController's prototype property).
  // (Deferred to SetUpReadableStreamDefaultController.)

  // Step 3: Let startAlgorithm be the following steps:
  //         a. Return ? InvokeOrNoop(underlyingSource, "start",
  //                                  « controller »).
  SourceAlgorithms sourceAlgorithms = SourceAlgorithms::Script;

  // Step 4: Let pullAlgorithm be
  //         ? CreateAlgorithmFromUnderlyingMethod(underlyingSource, "pull",
  //                                               0, « controller »).
  Rooted<Value> pullMethod(cx);
  if (!CreateAlgorithmFromUnderlyingMethod(cx, underlyingSource,
                                           "ReadableStream source.pull method",
                                           cx->names().pull, &pullMethod)) {
    return false;
  }

  // Step 5. Let cancelAlgorithm be
  //         ? CreateAlgorithmFromUnderlyingMethod(underlyingSource,
  //                                               "cancel", 1, « »).
  Rooted<Value> cancelMethod(cx);
  if (!CreateAlgorithmFromUnderlyingMethod(
          cx, underlyingSource, "ReadableStream source.cancel method",
          cx->names().cancel, &cancelMethod)) {
    return false;
  }

  // Step 6. Perform ? SetUpReadableStreamDefaultController(stream,
  //             controller, startAlgorithm, pullAlgorithm, cancelAlgorithm,
  //             highWaterMark, sizeAlgorithm).
  return SetUpReadableStreamDefaultController(
      cx, stream, sourceAlgorithms, underlyingSource, pullMethod, cancelMethod,
      highWaterMark, sizeAlgorithm);
}
