/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Writable stream default controller abstract operations. */

#include "builtin/streams/WritableStreamDefaultControllerOperations.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jsapi.h"  // JS_ReportErrorASCII

#include "builtin/streams/MiscellaneousOperations.h"  // js::CreateAlgorithmFromUnderlyingMethod, js::InvokeOrNoop
#include "builtin/streams/QueueWithSizes.h"  // js::{EnqueueValueWithSize,QueueIsEmpty,ResetQueue}
#include "builtin/streams/WritableStream.h"  // js::WritableStream
#include "builtin/streams/WritableStreamDefaultController.h"  // js::WritableStreamDefaultController
#include "builtin/streams/WritableStreamOperations.h"  // js::WritableStream{CloseQueuedOrInFlight,DealWithRejection,{Start,Finish}Erroring,UpdateBackpressure,Mark{Close,FirstWrite}RequestInFlight}
#include "js/CallArgs.h"                               // JS::CallArgs{,FromVp}
#include "js/Promise.h"     // JS::AddPromiseReactions
#include "js/RootingAPI.h"  // JS::Handle, JS::Rooted
#include "js/Value.h"  // JS::{,Int32,Magic,Object}Value, JS::UndefinedHandleValue, JS_WRITABLESTREAM_CLOSE_RECORD
#include "vm/Compartment.h"  // JS::Compartment
#include "vm/JSContext.h"    // JSContext
#include "vm/JSObject.h"     // JSObject
#include "vm/List.h"         // js::ListObject
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined
#include "vm/Runtime.h"        // JSAtomState

#include "builtin/HandlerFunction-inl.h"  // js::TargetFromHandler
#include "builtin/streams/MiscellaneousOperations-inl.h"  // js::PromiseCall
#include "builtin/streams/QueueWithSizes-inl.h"           // js::PeekQueueValue
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap
#include "vm/JSContext-inl.h"    // JSContext::check
#include "vm/JSObject-inl.h"  // js::IsCallable, js::NewBuiltinClassInstance, js::NewObjectWithClassProto
#include "vm/Realm-inl.h"  // js::AutoRealm

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::Int32Value;
using JS::MagicValue;
using JS::ObjectValue;
using JS::Rooted;
using JS::UndefinedHandleValue;
using JS::Value;

using js::IsCallable;
using js::ListObject;
using js::NewHandler;
using js::PeekQueueValue;
using js::PromiseObject;
using js::PromiseResolvedWithUndefined;
using js::TargetFromHandler;
using js::WritableStream;
using js::WritableStreamCloseQueuedOrInFlight;
using js::WritableStreamDefaultController;
using js::WritableStreamFinishErroring;
using js::WritableStreamMarkCloseRequestInFlight;
using js::WritableStreamMarkFirstWriteRequestInFlight;
using js::WritableStreamUpdateBackpressure;

/*** 4.7. Writable stream default controller internal methods ***************/

/**
 * Streams spec, 4.7.5.1.
 *      [[AbortSteps]]( reason )
 */
JSObject* js::WritableStreamControllerAbortSteps(
    JSContext* cx, Handle<WritableStreamDefaultController*> unwrappedController,
    Handle<Value> reason) {
  cx->check(reason);

  // Step 1: Let result be the result of performing this.[[abortAlgorithm]],
  //         passing reason.
  // CreateAlgorithmFromUnderlyingMethod(underlyingSink, "abort", 1, « »)
  Rooted<Value> unwrappedAbortMethod(cx, unwrappedController->abortMethod());
  Rooted<JSObject*> result(cx);
  if (unwrappedAbortMethod.isUndefined()) {
    // CreateAlgorithmFromUnderlyingMethod step 7.
    result = PromiseResolvedWithUndefined(cx);
    if (!result) {
      return nullptr;
    }
  } else {
    // CreateAlgorithmFromUnderlyingMethod step 6.c.i-ii.
    {
      AutoRealm ar(cx, unwrappedController);
      cx->check(unwrappedAbortMethod);

      Rooted<Value> underlyingSink(cx, unwrappedController->underlyingSink());
      cx->check(underlyingSink);

      Rooted<Value> wrappedReason(cx, reason);
      if (!cx->compartment()->wrap(cx, &wrappedReason)) {
        return nullptr;
      }

      result =
          PromiseCall(cx, unwrappedAbortMethod, underlyingSink, wrappedReason);
      if (!result) {
        return nullptr;
      }
    }
    if (!cx->compartment()->wrap(cx, &result)) {
      return nullptr;
    }
  }

  // Step 2: Perform ! WritableStreamDefaultControllerClearAlgorithms(this).
  WritableStreamDefaultControllerClearAlgorithms(unwrappedController);

  // Step 3: Return result.
  return result;
}

/**
 * Streams spec, 4.7.5.2.
 *      [[ErrorSteps]]()
 */
bool js::WritableStreamControllerErrorSteps(
    JSContext* cx,
    Handle<WritableStreamDefaultController*> unwrappedController) {
  // Step 1: Perform ! ResetQueue(this).
  return ResetQueue(cx, unwrappedController);
}

/*** 4.8. Writable stream default controller abstract operations ************/

[[nodiscard]] static bool WritableStreamDefaultControllerAdvanceQueueIfNeeded(
    JSContext* cx,
    Handle<WritableStreamDefaultController*> unwrappedController);

/**
 * Streams spec, 4.8.2. SetUpWritableStreamDefaultController, step 16:
 *      Upon fulfillment of startPromise, [...]
 */
bool js::WritableStreamControllerStartHandler(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<WritableStreamDefaultController*> unwrappedController(
      cx, TargetFromHandler<WritableStreamDefaultController>(args));

  // Step a: Assert: stream.[[state]] is "writable" or "erroring".
#ifdef DEBUG
  const auto* unwrappedStream = unwrappedController->stream();
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());
#endif

  // Step b: Set controller.[[started]] to true.
  unwrappedController->setStarted();

  // Step c: Perform
  //      ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
  if (!WritableStreamDefaultControllerAdvanceQueueIfNeeded(
          cx, unwrappedController)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 4.8.2. SetUpWritableStreamDefaultController, step 17:
 *      Upon rejection of startPromise with reason r, [...]
 */
bool js::WritableStreamControllerStartFailedHandler(JSContext* cx,
                                                    unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<WritableStreamDefaultController*> unwrappedController(
      cx, TargetFromHandler<WritableStreamDefaultController>(args));

  Rooted<WritableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step a: Assert: stream.[[state]] is "writable" or "erroring".
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());

  // Step b: Set controller.[[started]] to true.
  unwrappedController->setStarted();

  // Step c: Perform ! WritableStreamDealWithRejection(stream, r).
  if (!WritableStreamDealWithRejection(cx, unwrappedStream, args.get(0))) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 4.8.2.
 *      SetUpWritableStreamDefaultController(stream, controller,
 *          startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm,
 *          highWaterMark, sizeAlgorithm )
 *
 * The standard algorithm takes a `controller` argument which must be a new,
 * blank object. This implementation creates a new controller instead.
 *
 * In the spec, four algorithms (startAlgorithm, writeAlgorithm, closeAlgorithm,
 * abortAlgorithm) are passed as arguments to this routine. This implementation
 * passes these "algorithms" as data, using five arguments: sinkAlgorithms,
 * underlyingSink, writeMethod, closeMethod, and abortMethod. The sinkAlgorithms
 * argument tells how to interpret the other three:
 *
 * -   SinkAlgorithms::Script - We're creating a stream from a JS source.  The
 *     caller is `new WritableStream(underlyingSink)` or
 *     `JS::NewWritableDefaultStreamObject`. `underlyingSink` is the sink;
 *     `writeMethod`, `closeMethod`, and `abortMethod` are its .write, .close,
 *     and .abort methods, which the caller has already extracted and
 *     type-checked: each one must be either a callable JS object or undefined.
 *
 *     Script streams use the start/write/close/abort algorithms defined in
 *     4.8.3. SetUpWritableStreamDefaultControllerFromUnderlyingSink, which
 *     call JS methods of the underlyingSink.
 *
 * -   SinkAlgorithms::Transform - We're creating a transform stream.
 *     `underlyingSink` is a Transform object. `writeMethod`, `closeMethod, and
 *     `abortMethod` are undefined.
 *
 *     Transform streams use the write/close/abort algorithms given in
 *     5.3.2 InitializeTransformStream.
 *
 * An additional sizeAlgorithm in the spec is an algorithm used to compute the
 * size of a chunk.  Per MakeSizeAlgorithmFromSizeFunction, we just save the
 * |size| value used to create that algorithm, then -- inline -- perform the
 * requisite algorithm steps.  (Hence the unadorned name |size|.)
 *
 * Note: All arguments must be same-compartment with cx.  WritableStream
 * controllers are always created in the same compartment as the stream.
 */
[[nodiscard]] bool js::SetUpWritableStreamDefaultController(
    JSContext* cx, Handle<WritableStream*> stream,
    SinkAlgorithms sinkAlgorithms, Handle<Value> underlyingSink,
    Handle<Value> writeMethod, Handle<Value> closeMethod,
    Handle<Value> abortMethod, double highWaterMark, Handle<Value> size) {
  cx->check(stream);
  cx->check(underlyingSink);
  cx->check(writeMethod);
  MOZ_ASSERT(writeMethod.isUndefined() || IsCallable(writeMethod));
  cx->check(closeMethod);
  MOZ_ASSERT(closeMethod.isUndefined() || IsCallable(closeMethod));
  cx->check(abortMethod);
  MOZ_ASSERT(abortMethod.isUndefined() || IsCallable(abortMethod));
  MOZ_ASSERT(highWaterMark >= 0);
  cx->check(size);
  MOZ_ASSERT(size.isUndefined() || IsCallable(size));

  // Done elsewhere in the standard: Create the new controller.
  Rooted<WritableStreamDefaultController*> controller(
      cx, NewBuiltinClassInstance<WritableStreamDefaultController>(cx));
  if (!controller) {
    return false;
  }

  // Step 1: Assert: ! IsWritableStream(stream) is true.
  // (guaranteed by |stream|'s type)

  // Step 2: Assert: stream.[[writableStreamController]] is undefined.
  MOZ_ASSERT(!stream->hasController());

  // Step 3: Set controller.[[controlledWritableStream]] to stream.
  controller->setStream(stream);

  // Step 4: Set stream.[[writableStreamController]] to controller.
  stream->setController(controller);

  // Step 5: Perform ! ResetQueue(controller).
  if (!ResetQueue(cx, controller)) {
    return false;
  }

  // Step 6: Set controller.[[started]] to false.
  controller->setFlags(0);
  MOZ_ASSERT(!controller->started());

  // Step 7: Set controller.[[strategySizeAlgorithm]] to sizeAlgorithm.
  controller->setStrategySize(size);

  // Step 8: Set controller.[[strategyHWM]] to highWaterMark.
  controller->setStrategyHWM(highWaterMark);

  // Step 9: Set controller.[[writeAlgorithm]] to writeAlgorithm.
  // Step 10: Set controller.[[closeAlgorithm]] to closeAlgorithm.
  // Step 11: Set controller.[[abortAlgorithm]] to abortAlgorithm.
  // (In this implementation, all [[*Algorithm]] are determined by the
  // underlyingSink in combination with the corresponding *Method field.)
  controller->setUnderlyingSink(underlyingSink);
  controller->setWriteMethod(writeMethod);
  controller->setCloseMethod(closeMethod);
  controller->setAbortMethod(abortMethod);

  // Step 12: Let backpressure be
  //          ! WritableStreamDefaultControllerGetBackpressure(controller).
  bool backpressure =
      WritableStreamDefaultControllerGetBackpressure(controller);

  // Step 13: Perform ! WritableStreamUpdateBackpressure(stream, backpressure).
  if (!WritableStreamUpdateBackpressure(cx, stream, backpressure)) {
    return false;
  }

  // Step 14: Let startResult be the result of performing startAlgorithm. (This
  //          may throw an exception.)
  Rooted<Value> startResult(cx);
  if (sinkAlgorithms == SinkAlgorithms::Script) {
    Rooted<Value> controllerVal(cx, ObjectValue(*controller));
    if (!InvokeOrNoop(cx, underlyingSink, cx->names().start, controllerVal,
                      &startResult)) {
      return false;
    }
  }

  // Step 15: Let startPromise be a promise resolved with startResult.
  Rooted<JSObject*> startPromise(
      cx, PromiseObject::unforgeableResolve(cx, startResult));
  if (!startPromise) {
    return false;
  }

  // Step 16: Upon fulfillment of startPromise,
  //    Assert: stream.[[state]] is "writable" or "erroring".
  //    Set controller.[[started]] to true.
  //  Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
  // Step 17: Upon rejection of startPromise with reason r,
  //    Assert: stream.[[state]] is "writable" or "erroring".
  //    Set controller.[[started]] to true.
  //    Perform ! WritableStreamDealWithRejection(stream, r).
  Rooted<JSObject*> onStartFulfilled(
      cx, NewHandler(cx, WritableStreamControllerStartHandler, controller));
  if (!onStartFulfilled) {
    return false;
  }
  Rooted<JSObject*> onStartRejected(
      cx,
      NewHandler(cx, WritableStreamControllerStartFailedHandler, controller));
  if (!onStartRejected) {
    return false;
  }

  return JS::AddPromiseReactions(cx, startPromise, onStartFulfilled,
                                 onStartRejected);
}

/**
 * Streams spec, 4.8.3.
 *      SetUpWritableStreamDefaultControllerFromUnderlyingSink( stream,
 *          underlyingSink, highWaterMark, sizeAlgorithm )
 */
[[nodiscard]] bool js::SetUpWritableStreamDefaultControllerFromUnderlyingSink(
    JSContext* cx, Handle<WritableStream*> stream, Handle<Value> underlyingSink,
    double highWaterMark, Handle<Value> sizeAlgorithm) {
  cx->check(stream);
  cx->check(underlyingSink);
  cx->check(sizeAlgorithm);

  // Step 1: Assert: underlyingSink is not undefined.
  MOZ_ASSERT(!underlyingSink.isUndefined());

  // Step 2: Let controller be ObjectCreate(the original value of
  //         WritableStreamDefaultController's prototype property).
  // (Deferred to SetUpWritableStreamDefaultController.)

  // Step 3: Let startAlgorithm be the following steps:
  //         a. Return ? InvokeOrNoop(underlyingSink, "start",
  //                                  « controller »).
  SinkAlgorithms sinkAlgorithms = SinkAlgorithms::Script;

  // Step 4: Let writeAlgorithm be
  //         ? CreateAlgorithmFromUnderlyingMethod(underlyingSink, "write", 1,
  //                                               « controller »).
  Rooted<Value> writeMethod(cx);
  if (!CreateAlgorithmFromUnderlyingMethod(cx, underlyingSink,
                                           "WritableStream sink.write method",
                                           cx->names().write, &writeMethod)) {
    return false;
  }

  // Step 5: Let closeAlgorithm be
  //         ? CreateAlgorithmFromUnderlyingMethod(underlyingSink, "close", 0,
  //                                               « »).
  Rooted<Value> closeMethod(cx);
  if (!CreateAlgorithmFromUnderlyingMethod(cx, underlyingSink,
                                           "WritableStream sink.close method",
                                           cx->names().close, &closeMethod)) {
    return false;
  }

  // Step 6: Let abortAlgorithm be
  //         ? CreateAlgorithmFromUnderlyingMethod(underlyingSink, "abort", 1,
  //                                               « »).
  Rooted<Value> abortMethod(cx);
  if (!CreateAlgorithmFromUnderlyingMethod(cx, underlyingSink,
                                           "WritableStream sink.abort method",
                                           cx->names().abort, &abortMethod)) {
    return false;
  }

  // Step 6. Perform ? SetUpWritableStreamDefaultController(stream,
  //             controller, startAlgorithm, writeAlgorithm, closeAlgorithm,
  //             abortAlgorithm, highWaterMark, sizeAlgorithm).
  return SetUpWritableStreamDefaultController(
      cx, stream, sinkAlgorithms, underlyingSink, writeMethod, closeMethod,
      abortMethod, highWaterMark, sizeAlgorithm);
}

/**
 * Streams spec, 4.8.4.
 *      WritableStreamDefaultControllerClearAlgorithms ( controller )
 */
void js::WritableStreamDefaultControllerClearAlgorithms(
    WritableStreamDefaultController* unwrappedController) {
  // Note: This operation will be performed multiple times in some edge cases,
  //       so it can't assert that the various algorithms initially haven't been
  //       cleared.

  // Step 1: Set controller.[[writeAlgorithm]] to undefined.
  unwrappedController->clearWriteMethod();

  // Step 2: Set controller.[[closeAlgorithm]] to undefined.
  unwrappedController->clearCloseMethod();

  // Step 3: Set controller.[[abortAlgorithm]] to undefined.
  unwrappedController->clearAbortMethod();

  // Step 4: Set controller.[[strategySizeAlgorithm]] to undefined.
  unwrappedController->clearStrategySize();
}

/**
 * Streams spec, 4.8.5.
 *      WritableStreamDefaultControllerClose ( controller )
 */
bool js::WritableStreamDefaultControllerClose(
    JSContext* cx,
    Handle<WritableStreamDefaultController*> unwrappedController) {
  // Step 1: Perform ! EnqueueValueWithSize(controller, "close", 0).
  {
    Rooted<Value> v(cx, MagicValue(JS_WRITABLESTREAM_CLOSE_RECORD));
    Rooted<Value> size(cx, Int32Value(0));
    if (!EnqueueValueWithSize(cx, unwrappedController, v, size)) {
      return false;
    }
  }

  // Step 2: Perform
  //         ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
  return WritableStreamDefaultControllerAdvanceQueueIfNeeded(
      cx, unwrappedController);
}

/**
 * Streams spec, 4.8.6.
 *      WritableStreamDefaultControllerGetChunkSize ( controller, chunk )
 */
bool js::WritableStreamDefaultControllerGetChunkSize(
    JSContext* cx, Handle<WritableStreamDefaultController*> unwrappedController,
    Handle<Value> chunk, MutableHandle<Value> returnValue) {
  cx->check(chunk);

  // Step 1: Let returnValue be the result of performing
  //         controller.[[strategySizeAlgorithm]], passing in chunk, and
  //         interpreting the result as an ECMAScript completion value.

  // We don't store a literal [[strategySizeAlgorithm]], only the value that if
  // passed through |MakeSizeAlgorithmFromSizeFunction| wouldn't have triggered
  // an error.  Perform the algorithm that function would return.
  Rooted<Value> unwrappedStrategySize(cx, unwrappedController->strategySize());
  if (unwrappedStrategySize.isUndefined()) {
    // 6.3.8 step 1: If size is undefined, return an algorithm that returns 1.
    // ...and then from this function...
    // Step 3: Return returnValue.[[Value]].
    returnValue.setInt32(1);
    return true;
  }

  MOZ_ASSERT(IsCallable(unwrappedStrategySize));

  {
    bool success;
    {
      AutoRealm ar(cx, unwrappedController);
      cx->check(unwrappedStrategySize);

      Rooted<Value> wrappedChunk(cx, chunk);
      if (!cx->compartment()->wrap(cx, &wrappedChunk)) {
        return false;
      }

      // 6.3.8 step 3 (of |MakeSizeAlgorithmFromSizeFunction|):
      //         Return an algorithm that performs the following steps, taking a
      //         chunk argument:
      //     a. Return ? Call(size, undefined, « chunk »).
      success = Call(cx, unwrappedStrategySize, UndefinedHandleValue,
                     wrappedChunk, returnValue);
    }

    // Step 3: (If returnValue is [not] an abrupt completion, )
    //         Return returnValue.[[Value]].  (reordered for readability)
    if (success) {
      return cx->compartment()->wrap(cx, returnValue);
    }
  }

  // Step 2: If returnValue is an abrupt completion,
  if (!cx->isExceptionPending() || !cx->getPendingException(returnValue)) {
    // Uncatchable error.  Die immediately without erroring the stream.
    return false;
  }
  cx->check(returnValue);

  cx->clearPendingException();

  // Step 2.a: Perform
  //           ! WritableStreamDefaultControllerErrorIfNeeded(
  //                 controller, returnValue.[[Value]]).
  if (!WritableStreamDefaultControllerErrorIfNeeded(cx, unwrappedController,
                                                    returnValue)) {
    return false;
  }

  // Step 2.b: Return 1.
  returnValue.setInt32(1);
  return true;
}

/**
 * Streams spec, 4.8.7.
 *      WritableStreamDefaultControllerGetDesiredSize ( controller )
 */
double js::WritableStreamDefaultControllerGetDesiredSize(
    const WritableStreamDefaultController* controller) {
  return controller->strategyHWM() - controller->queueTotalSize();
}

/**
 * Streams spec, 4.8.8.
 *      WritableStreamDefaultControllerWrite ( controller, chunk, chunkSize )
 */
bool js::WritableStreamDefaultControllerWrite(
    JSContext* cx, Handle<WritableStreamDefaultController*> unwrappedController,
    Handle<Value> chunk, Handle<Value> chunkSize) {
  MOZ_ASSERT(!chunk.isMagic());
  cx->check(chunk);
  cx->check(chunkSize);

  // Step 1: Let writeRecord be Record {[[chunk]]: chunk}.
  // Step 2: Let enqueueResult be
  //         EnqueueValueWithSize(controller, writeRecord, chunkSize).
  bool succeeded =
      EnqueueValueWithSize(cx, unwrappedController, chunk, chunkSize);

  // Step 3: If enqueueResult is an abrupt completion,
  if (!succeeded) {
    Rooted<Value> enqueueResult(cx);
    if (!cx->isExceptionPending() || !cx->getPendingException(&enqueueResult)) {
      // Uncatchable error.  Die immediately without erroring the stream.
      return false;
    }
    cx->check(enqueueResult);

    cx->clearPendingException();

    // Step 3.a: Perform ! WritableStreamDefaultControllerErrorIfNeeded(
    //                 controller, enqueueResult.[[Value]]).
    // Step 3.b: Return.
    return WritableStreamDefaultControllerErrorIfNeeded(cx, unwrappedController,
                                                        enqueueResult);
  }

  // Step 4: Let stream be controller.[[controlledWritableStream]].
  Rooted<WritableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 5: If ! WritableStreamCloseQueuedOrInFlight(stream) is false and
  //         stream.[[state]] is "writable",
  if (!WritableStreamCloseQueuedOrInFlight(unwrappedStream) &&
      unwrappedStream->writable()) {
    // Step 5.a: Let backpressure be
    //           ! WritableStreamDefaultControllerGetBackpressure(controller).
    bool backpressure =
        WritableStreamDefaultControllerGetBackpressure(unwrappedController);

    // Step 5.b: Perform
    //           ! WritableStreamUpdateBackpressure(stream, backpressure).
    if (!WritableStreamUpdateBackpressure(cx, unwrappedStream, backpressure)) {
      return false;
    }
  }

  // Step 6: Perform
  //         ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
  return WritableStreamDefaultControllerAdvanceQueueIfNeeded(
      cx, unwrappedController);
}

[[nodiscard]] static bool WritableStreamDefaultControllerProcessIfNeeded(
    JSContext* cx,
    Handle<WritableStreamDefaultController*> unwrappedController);

/**
 * Streams spec, 4.8.9.
 *      WritableStreamDefaultControllerAdvanceQueueIfNeeded ( controller )
 */
[[nodiscard]] bool WritableStreamDefaultControllerAdvanceQueueIfNeeded(
    JSContext* cx,
    Handle<WritableStreamDefaultController*> unwrappedController) {
  // Step 2: If controller.[[started]] is false, return.
  if (!unwrappedController->started()) {
    return true;
  }

  // Step 1: Let stream be controller.[[controlledWritableStream]].
  Rooted<WritableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 3: If stream.[[inFlightWriteRequest]] is not undefined, return.
  if (!unwrappedStream->inFlightWriteRequest().isUndefined()) {
    return true;
  }

  // Step 4: Let state be stream.[[state]].
  // Step 5: Assert: state is not "closed" or "errored".
  // Step 6: If state is "erroring",
  MOZ_ASSERT(!unwrappedStream->closed());
  MOZ_ASSERT(!unwrappedStream->errored());
  if (unwrappedStream->erroring()) {
    // Step 6a: Perform ! WritableStreamFinishErroring(stream).
    // Step 6b: Return.
    return WritableStreamFinishErroring(cx, unwrappedStream);
  }

  // Step 7: If controller.[[queue]] is empty, return.
  // Step 8: Let writeRecord be ! PeekQueueValue(controller).
  // Step 9: If writeRecord is "close", perform
  //         ! WritableStreamDefaultControllerProcessClose(controller).
  // Step 10: Otherwise, perform
  //          ! WritableStreamDefaultControllerProcessWrite(
  //              controller, writeRecord.[[chunk]]).
  return WritableStreamDefaultControllerProcessIfNeeded(cx,
                                                        unwrappedController);
}

/**
 * Streams spec, 4.8.10.
 *      WritableStreamDefaultControllerErrorIfNeeded ( controller, error )
 */
bool js::WritableStreamDefaultControllerErrorIfNeeded(
    JSContext* cx, Handle<WritableStreamDefaultController*> unwrappedController,
    Handle<Value> error) {
  cx->check(error);

  // Step 1: If controller.[[controlledWritableStream]].[[state]] is "writable",
  //         perform ! WritableStreamDefaultControllerError(controller, error).
  if (unwrappedController->stream()->writable()) {
    if (!WritableStreamDefaultControllerError(cx, unwrappedController, error)) {
      return false;
    }
  }

  return true;
}

// 4.8.11 step 5: Let sinkClosePromise be the result of performing
//                controller.[[closeAlgorithm]].
[[nodiscard]] static JSObject* PerformCloseAlgorithm(
    JSContext* cx,
    Handle<WritableStreamDefaultController*> unwrappedController) {
  // 4.8.3 step 5: Let closeAlgorithm be
  //               ? CreateAlgorithmFromUnderlyingMethod(underlyingSink,
  //                                                     "close", 0, « »).

  // Step 1: Assert: underlyingObject is not undefined.
  // Step 2: Assert: ! IsPropertyKey(methodName) is true (implicit).
  // Step 3: Assert: algoArgCount is 0 or 1 (omitted).
  // Step 4: Assert: extraArgs is a List (omitted).
  // Step 5: Let method be ? GetV(underlyingObject, methodName).
  //
  // These steps were performed in |CreateAlgorithmFromUnderlyingMethod|.  The
  // spec stores away algorithms for later invocation; we instead store the
  // value that determines the algorithm to be created -- either |undefined|, or
  // a callable object that's called with context-specific arguments.

  // Step 7: (If method is undefined,) Return an algorithm which returns a
  //         promise resolved with undefined (implicit).
  if (unwrappedController->closeMethod().isUndefined()) {
    return PromiseResolvedWithUndefined(cx);
  }

  // Step 6: If method is not undefined,

  // Step 6.a: If ! IsCallable(method) is false, throw a TypeError exception.
  MOZ_ASSERT(IsCallable(unwrappedController->closeMethod()));

  // Step 6.b: If algoArgCount is 0, return an algorithm that performs the
  //           following steps:
  // Step 6.b.ii: Return ! PromiseCall(method, underlyingObject, extraArgs).
  Rooted<Value> closeMethod(cx, unwrappedController->closeMethod());
  if (!cx->compartment()->wrap(cx, &closeMethod)) {
    return nullptr;
  }

  Rooted<Value> underlyingSink(cx, unwrappedController->underlyingSink());
  if (!cx->compartment()->wrap(cx, &underlyingSink)) {
    return nullptr;
  }

  return PromiseCall(cx, closeMethod, underlyingSink);
}

// 4.8.12 step 3: Let sinkWritePromise be the result of performing
//                controller.[[writeAlgorithm]], passing in chunk.
[[nodiscard]] static JSObject* PerformWriteAlgorithm(
    JSContext* cx, Handle<WritableStreamDefaultController*> unwrappedController,
    Handle<Value> chunk) {
  cx->check(chunk);

  // 4.8.3 step 4: Let writeAlgorithm be
  //               ? CreateAlgorithmFromUnderlyingMethod(underlyingSink,
  //                                                     "write", 1,
  //                                                     « controller »).

  // Step 1: Assert: underlyingObject is not undefined.
  // Step 2: Assert: ! IsPropertyKey(methodName) is true (implicit).
  // Step 3: Assert: algoArgCount is 0 or 1 (omitted).
  // Step 4: Assert: extraArgs is a List (omitted).
  // Step 5: Let method be ? GetV(underlyingObject, methodName).
  //
  // These steps were performed in |CreateAlgorithmFromUnderlyingMethod|.  The
  // spec stores away algorithms for later invocation; we instead store the
  // value that determines the algorithm to be created -- either |undefined|, or
  // a callable object that's called with context-specific arguments.

  // Step 7: (If method is undefined,) Return an algorithm which returns a
  //         promise resolved with undefined (implicit).
  if (unwrappedController->writeMethod().isUndefined()) {
    return PromiseResolvedWithUndefined(cx);
  }

  // Step 6: If method is not undefined,

  // Step 6.a: If ! IsCallable(method) is false, throw a TypeError exception.
  MOZ_ASSERT(IsCallable(unwrappedController->writeMethod()));

  // Step 6.c: Otherwise (if algoArgCount is not 0), return an algorithm that
  //           performs the following steps, taking an arg argument:
  // Step 6.c.i:  Let fullArgs be a List consisting of arg followed by the
  //              elements of extraArgs in order.
  // Step 6.c.ii: Return ! PromiseCall(method, underlyingObject, fullArgs).
  Rooted<Value> writeMethod(cx, unwrappedController->writeMethod());
  if (!cx->compartment()->wrap(cx, &writeMethod)) {
    return nullptr;
  }

  Rooted<Value> underlyingSink(cx, unwrappedController->underlyingSink());
  if (!cx->compartment()->wrap(cx, &underlyingSink)) {
    return nullptr;
  }

  Rooted<Value> controller(cx, ObjectValue(*unwrappedController));
  if (!cx->compartment()->wrap(cx, &controller)) {
    return nullptr;
  }

  return PromiseCall(cx, writeMethod, underlyingSink, chunk, controller);
}

/**
 * Streams spec, 4.8.11 step 7:
 * Upon fulfillment of sinkClosePromise,
 */
[[nodiscard]] static bool WritableStreamCloseHandler(JSContext* cx,
                                                     unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<WritableStream*> unwrappedStream(
      cx, TargetFromHandler<WritableStream>(args));

  // Step 7.a: Perform ! WritableStreamFinishInFlightClose(stream).
  if (!WritableStreamFinishInFlightClose(cx, unwrappedStream)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 4.8.11 step 8:
 * Upon rejection of sinkClosePromise with reason reason,
 */
[[nodiscard]] static bool WritableStreamCloseFailedHandler(JSContext* cx,
                                                           unsigned argc,
                                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<WritableStream*> unwrappedStream(
      cx, TargetFromHandler<WritableStream>(args));

  // Step 8.a: Perform
  //           ! WritableStreamFinishInFlightCloseWithError(stream, reason).
  if (!WritableStreamFinishInFlightCloseWithError(cx, unwrappedStream,
                                                  args.get(0))) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 4.8.12 step 4:
 * Upon fulfillment of sinkWritePromise,
 */
[[nodiscard]] static bool WritableStreamWriteHandler(JSContext* cx,
                                                     unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<WritableStream*> unwrappedStream(
      cx, TargetFromHandler<WritableStream>(args));

  // Step 4.a: Perform ! WritableStreamFinishInFlightWrite(stream).
  if (!WritableStreamFinishInFlightWrite(cx, unwrappedStream)) {
    return false;
  }

  // Step 4.b: Let state be stream.[[state]].
  // Step 4.c: Assert: state is "writable" or "erroring".
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());

  // Step 4.d: Perform ! DequeueValue(controller).
  DequeueValue(unwrappedStream->controller(), cx);

  // Step 4.e: If ! WritableStreamCloseQueuedOrInFlight(stream) is false and
  //           state is "writable",
  if (!WritableStreamCloseQueuedOrInFlight(unwrappedStream) &&
      unwrappedStream->writable()) {
    // Step 4.e.i: Let backpressure be
    //             ! WritableStreamDefaultControllerGetBackpressure(
    //                   controller).
    bool backpressure = WritableStreamDefaultControllerGetBackpressure(
        unwrappedStream->controller());

    // Step 4.e.ii: Perform
    //              ! WritableStreamUpdateBackpressure(stream, backpressure).
    if (!WritableStreamUpdateBackpressure(cx, unwrappedStream, backpressure)) {
      return false;
    }
  }

  // Step 4.f: Perform
  //           ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(
  //                 controller).
  Rooted<WritableStreamDefaultController*> unwrappedController(
      cx, unwrappedStream->controller());
  if (!WritableStreamDefaultControllerAdvanceQueueIfNeeded(
          cx, unwrappedController)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 4.8.12 step 5:
 * Upon rejection of sinkWritePromise with reason,
 */
[[nodiscard]] static bool WritableStreamWriteFailedHandler(JSContext* cx,
                                                           unsigned argc,
                                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<WritableStream*> unwrappedStream(
      cx, TargetFromHandler<WritableStream>(args));

  // Step 5.a: If stream.[[state]] is "writable", perform
  //           ! WritableStreamDefaultControllerClearAlgorithms(controller).
  if (unwrappedStream->writable()) {
    WritableStreamDefaultControllerClearAlgorithms(
        unwrappedStream->controller());
  }

  // Step 5.b: Perform
  //           ! WritableStreamFinishInFlightWriteWithError(stream, reason).
  if (!WritableStreamFinishInFlightWriteWithError(cx, unwrappedStream,
                                                  args.get(0))) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 4.8.9 (steps 7-10),
 *      WritableStreamDefaultControllerAdvanceQueueIfNeeded ( controller )
 * Streams spec, 4.8.11.
 *      WritableStreamDefaultControllerProcessClose ( controller )
 * Streams spec, 4.8.12.
 *      WritableStreamDefaultControllerProcessWrite ( controller, chunk )
 */
bool WritableStreamDefaultControllerProcessIfNeeded(
    JSContext* cx,
    Handle<WritableStreamDefaultController*> unwrappedController) {
  // Step 7: If controller.[[queue]] is empty, return.
  ListObject* unwrappedQueue = unwrappedController->queue();
  if (QueueIsEmpty(unwrappedQueue)) {
    return true;
  }

  // Step 8: Let writeRecord be ! PeekQueueValue(controller).
  // Step 9: If writeRecord is "close", perform
  //         ! WritableStreamDefaultControllerProcessClose(controller).
  // Step 10: Otherwise, perform
  //          ! WritableStreamDefaultControllerProcessWrite(
  //                controller, writeRecord.[[chunk]]).
  Rooted<JSObject*> sinkWriteOrClosePromise(cx);
  JSNative onFulfilledFunc, onRejectedFunc;
  if (PeekQueueValue(unwrappedQueue).isMagic(JS_WRITABLESTREAM_CLOSE_RECORD)) {
    MOZ_ASSERT(unwrappedQueue->length() == 2);

    onFulfilledFunc = WritableStreamCloseHandler;
    onRejectedFunc = WritableStreamCloseFailedHandler;

    // 4.8.11 step 1: Let stream be controller.[[controlledWritableStream]].
    // 4.8.11 step 2: Perform ! WritableStreamMarkCloseRequestInFlight(stream).
    WritableStreamMarkCloseRequestInFlight(unwrappedController->stream());

    // 4.8.11 step 3: Perform ! DequeueValue(controller).
    DequeueValue(unwrappedController, cx);

    // 4.8.11 step 4: Assert: controller.[[queue]] is empty.
    MOZ_ASSERT(unwrappedQueue->isEmpty());

    // 4.8.11 step 5: Let sinkClosePromise be the result of performing
    //         controller.[[closeAlgorithm]].
    sinkWriteOrClosePromise = PerformCloseAlgorithm(cx, unwrappedController);
  } else {
    onFulfilledFunc = WritableStreamWriteHandler;
    onRejectedFunc = WritableStreamWriteFailedHandler;

    Rooted<Value> chunk(cx, PeekQueueValue(unwrappedQueue));
    if (!cx->compartment()->wrap(cx, &chunk)) {
      return false;
    }

    // 4.8.12 step 1: Let stream be controller.[[controlledWritableStream]].
    // 4.8.12 step 2: Perform
    //                ! WritableStreamMarkFirstWriteRequestInFlight(stream).
    WritableStreamMarkFirstWriteRequestInFlight(unwrappedController->stream());

    // 4.8.12 step 3: Let sinkWritePromise be the result of performing
    //                controller.[[writeAlgorithm]], passing in chunk.
    sinkWriteOrClosePromise =
        PerformWriteAlgorithm(cx, unwrappedController, chunk);
  }
  if (!sinkWriteOrClosePromise) {
    return false;
  }

  Rooted<JSObject*> stream(cx, unwrappedController->stream());
  if (!cx->compartment()->wrap(cx, &stream)) {
    return false;
  }

  // Step 7: Upon fulfillment of sinkClosePromise,
  // Step 4: Upon fulfillment of sinkWritePromise,
  // Step 8: Upon rejection of sinkClosePromise with reason reason,
  // Step 5: Upon rejection of sinkWritePromise with reason,
  Rooted<JSObject*> onFulfilled(cx, NewHandler(cx, onFulfilledFunc, stream));
  if (!onFulfilled) {
    return false;
  }
  Rooted<JSObject*> onRejected(cx, NewHandler(cx, onRejectedFunc, stream));
  if (!onRejected) {
    return false;
  }
  return JS::AddPromiseReactions(cx, sinkWriteOrClosePromise, onFulfilled,
                                 onRejected);
}

/**
 * Streams spec, 4.8.13.
 *      WritableStreamDefaultControllerGetBackpressure ( controller )
 */
bool js::WritableStreamDefaultControllerGetBackpressure(
    const WritableStreamDefaultController* unwrappedController) {
  return WritableStreamDefaultControllerGetDesiredSize(unwrappedController) <=
         0.0;
}

/**
 * Streams spec, 4.8.14.
 *      WritableStreamDefaultControllerError ( controller, error )
 */
bool js::WritableStreamDefaultControllerError(
    JSContext* cx, Handle<WritableStreamDefaultController*> unwrappedController,
    Handle<Value> error) {
  cx->check(error);

  // Step 1: Let stream be controller.[[controlledWritableStream]].
  Rooted<WritableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 2: Assert: stream.[[state]] is "writable".
  MOZ_ASSERT(unwrappedStream->writable());

  // Step 3: Perform
  //         ! WritableStreamDefaultControllerClearAlgorithms(controller).
  WritableStreamDefaultControllerClearAlgorithms(unwrappedController);

  // Step 4: Perform ! WritableStreamStartErroring(stream, error).
  return WritableStreamStartErroring(cx, unwrappedStream, error);
}
