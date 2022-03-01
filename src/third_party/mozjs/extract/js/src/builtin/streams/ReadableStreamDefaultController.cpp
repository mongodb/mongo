/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Class ReadableStreamDefaultController. */

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include "jsapi.h"        // JS_ReportErrorNumberASCII
#include "jsfriendapi.h"  // js::AssertSameCompartment

#include "builtin/streams/ClassSpecMacro.h"           // JS_STREAMS_CLASS_SPEC
#include "builtin/streams/MiscellaneousOperations.h"  // js::IsMaybeWrapped
#include "builtin/streams/PullIntoDescriptor.h"       // js::PullIntoDescriptor
#include "builtin/streams/QueueWithSizes.h"  // js::{DequeueValue,ResetQueue}
#include "builtin/streams/ReadableStream.h"  // js::ReadableStream, js::SetUpExternalReadableByteStreamController
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStream{,Default}Controller, js::ReadableByteStreamController, js::CheckReadableStreamControllerCanCloseOrEnqueue, js::ReadableStreamControllerCancelSteps, js::ReadableStreamDefaultControllerPullSteps, js::ReadableStreamControllerStart{,Failed}Handler
#include "builtin/streams/ReadableStreamDefaultControllerOperations.h"  // js::ReadableStreamController{CallPullIfNeeded,ClearAlgorithms,Error,GetDesiredSizeUnchecked}, js::ReadableStreamDefaultController{Close,Enqueue}
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStream{AddReadOrReadIntoRequest,CloseInternal,CreateReadResult}
#include "builtin/streams/ReadableStreamOperations.h"  // js::ReadableStreamTee_Cancel
#include "builtin/streams/ReadableStreamReader.h"  // js::ReadableStream{,Default}Reader
#include "builtin/streams/StreamController.h"  // js::StreamController
#include "builtin/streams/TeeState.h"          // js::TeeState
#include "js/ArrayBuffer.h"                    // JS::NewArrayBuffer
#include "js/Class.h"                          // js::ClassSpec
#include "js/friend/ErrorMessages.h"           // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined
#include "vm/SelfHosting.h"

#include "builtin/HandlerFunction-inl.h"  // js::TargetFromHandler
#include "builtin/streams/MiscellaneousOperations-inl.h"  // js::PromiseCall
#include "builtin/streams/ReadableStreamReader-inl.h"  // js::UnwrapReaderFromStream
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::UnwrapAnd{DowncastObject,TypeCheckThis}
#include "vm/JSContext-inl.h"  // JSContext::check
#include "vm/Realm-inl.h"      // js::AutoRealm

using js::ClassSpec;
using js::PromiseObject;
using js::ReadableStream;
using js::ReadableStreamController;
using js::ReadableStreamControllerCallPullIfNeeded;
using js::ReadableStreamControllerClearAlgorithms;
using js::ReadableStreamControllerError;
using js::ReadableStreamControllerGetDesiredSizeUnchecked;
using js::ReadableStreamDefaultController;
using js::ReadableStreamDefaultControllerClose;
using js::ReadableStreamDefaultControllerEnqueue;
using js::TargetFromHandler;
using js::UnwrapAndTypeCheckThis;

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::ObjectValue;
using JS::Rooted;
using JS::Value;

/*** 3.9. Class ReadableStreamDefaultController *****************************/

/**
 * Streams spec, 3.10.11. SetUpReadableStreamDefaultController, step 11
 * and
 * Streams spec, 3.13.26. SetUpReadableByteStreamController, step 16:
 *      Upon fulfillment of startPromise, [...]
 */
bool js::ReadableStreamControllerStartHandler(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableStreamController*> controller(
      cx, TargetFromHandler<ReadableStreamController>(args));

  // Step a: Set controller.[[started]] to true.
  controller->setStarted();

  // Step b: Assert: controller.[[pulling]] is false.
  MOZ_ASSERT(!controller->pulling());

  // Step c: Assert: controller.[[pullAgain]] is false.
  MOZ_ASSERT(!controller->pullAgain());

  // Step d: Perform
  //      ! ReadableStreamDefaultControllerCallPullIfNeeded(controller)
  //      (or ReadableByteStreamControllerCallPullIfNeeded(controller)).
  if (!ReadableStreamControllerCallPullIfNeeded(cx, controller)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 3.10.11. SetUpReadableStreamDefaultController, step 12
 * and
 * Streams spec, 3.13.26. SetUpReadableByteStreamController, step 17:
 *      Upon rejection of startPromise with reason r, [...]
 */
bool js::ReadableStreamControllerStartFailedHandler(JSContext* cx,
                                                    unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableStreamController*> controller(
      cx, TargetFromHandler<ReadableStreamController>(args));

  // Step a: Perform
  //      ! ReadableStreamDefaultControllerError(controller, r)
  //      (or ReadableByteStreamControllerError(controller, r)).
  if (!ReadableStreamControllerError(cx, controller, args.get(0))) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 3.9.3.
 * new ReadableStreamDefaultController( stream, underlyingSource, size,
 *                                      highWaterMark )
 */
bool ReadableStreamDefaultController::constructor(JSContext* cx, unsigned argc,
                                                  Value* vp) {
  // Step 1: Throw a TypeError.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_BOGUS_CONSTRUCTOR,
                            "ReadableStreamDefaultController");
  return false;
}

/**
 * Streams spec, 3.9.4.1. get desiredSize
 */
static bool ReadableStreamDefaultController_desiredSize(JSContext* cx,
                                                        unsigned argc,
                                                        Value* vp) {
  // Step 1: If ! IsReadableStreamDefaultController(this) is false, throw a
  //         TypeError exception.
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableStreamController*> unwrappedController(
      cx, UnwrapAndTypeCheckThis<ReadableStreamDefaultController>(
              cx, args, "get desiredSize"));
  if (!unwrappedController) {
    return false;
  }

  // 3.10.8. ReadableStreamDefaultControllerGetDesiredSize, steps 1-4.
  // 3.10.8. Step 1: Let stream be controller.[[controlledReadableStream]].
  ReadableStream* unwrappedStream = unwrappedController->stream();

  // 3.10.8. Step 2: Let state be stream.[[state]].
  // 3.10.8. Step 3: If state is "errored", return null.
  if (unwrappedStream->errored()) {
    args.rval().setNull();
    return true;
  }

  // 3.10.8. Step 4: If state is "closed", return 0.
  if (unwrappedStream->closed()) {
    args.rval().setInt32(0);
    return true;
  }

  // Step 2: Return ! ReadableStreamDefaultControllerGetDesiredSize(this).
  args.rval().setNumber(
      ReadableStreamControllerGetDesiredSizeUnchecked(unwrappedController));
  return true;
}

/**
 * Unified implementation of step 2 of 3.9.4.2 and 3.9.4.3,
 * and steps 2-3 of 3.11.4.3.
 */
[[nodiscard]] bool js::CheckReadableStreamControllerCanCloseOrEnqueue(
    JSContext* cx, Handle<ReadableStreamController*> unwrappedController,
    const char* action) {
  // 3.9.4.2. close(), step 2, and
  // 3.9.4.3. enqueue(chunk), step 2:
  //      If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(this) is false,
  //      throw a TypeError exception.
  // RSDCCanCloseOrEnqueue returns false in two cases: (1)
  // controller.[[closeRequested]] is true; (2) the stream is not readable,
  // i.e. already closed or errored. This amounts to exactly the same thing as
  // 3.11.4.3 steps 2-3 below, and we want different error messages for the two
  // cases anyway.

  // 3.11.4.3. Step 2: If this.[[closeRequested]] is true, throw a TypeError
  //                   exception.
  if (unwrappedController->closeRequested()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMCONTROLLER_CLOSED, action);
    return false;
  }

  // 3.11.4.3. Step 3: If this.[[controlledReadableByteStream]].[[state]] is
  //                   not "readable", throw a TypeError exception.
  ReadableStream* unwrappedStream = unwrappedController->stream();
  if (!unwrappedStream->readable()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMCONTROLLER_NOT_READABLE,
                              action);
    return false;
  }

  return true;
}

/**
 * Streams spec, 3.9.4.2 close()
 */
static bool ReadableStreamDefaultController_close(JSContext* cx, unsigned argc,
                                                  Value* vp) {
  // Step 1: If ! IsReadableStreamDefaultController(this) is false, throw a
  //         TypeError exception.
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableStreamDefaultController*> unwrappedController(
      cx, UnwrapAndTypeCheckThis<ReadableStreamDefaultController>(cx, args,
                                                                  "close"));
  if (!unwrappedController) {
    return false;
  }

  // Step 2: If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(this) is
  //         false, throw a TypeError exception.
  if (!CheckReadableStreamControllerCanCloseOrEnqueue(cx, unwrappedController,
                                                      "close")) {
    return false;
  }

  // Step 3: Perform ! ReadableStreamDefaultControllerClose(this).
  if (!ReadableStreamDefaultControllerClose(cx, unwrappedController)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 3.9.4.3. enqueue ( chunk )
 */
static bool ReadableStreamDefaultController_enqueue(JSContext* cx,
                                                    unsigned argc, Value* vp) {
  // Step 1: If ! IsReadableStreamDefaultController(this) is false, throw a
  //         TypeError exception.
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableStreamDefaultController*> unwrappedController(
      cx, UnwrapAndTypeCheckThis<ReadableStreamDefaultController>(cx, args,
                                                                  "enqueue"));
  if (!unwrappedController) {
    return false;
  }

  // Step 2: If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(this) is
  //         false, throw a TypeError exception.
  if (!CheckReadableStreamControllerCanCloseOrEnqueue(cx, unwrappedController,
                                                      "enqueue")) {
    return false;
  }

  // Step 3: Return ! ReadableStreamDefaultControllerEnqueue(this, chunk).
  if (!ReadableStreamDefaultControllerEnqueue(cx, unwrappedController,
                                              args.get(0))) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 3.9.4.4. error ( e )
 */
static bool ReadableStreamDefaultController_error(JSContext* cx, unsigned argc,
                                                  Value* vp) {
  // Step 1: If ! IsReadableStreamDefaultController(this) is false, throw a
  //         TypeError exception.
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableStreamDefaultController*> unwrappedController(
      cx, UnwrapAndTypeCheckThis<ReadableStreamDefaultController>(cx, args,
                                                                  "enqueue"));
  if (!unwrappedController) {
    return false;
  }

  // Step 2: Perform ! ReadableStreamDefaultControllerError(this, e).
  if (!ReadableStreamControllerError(cx, unwrappedController, args.get(0))) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static const JSPropertySpec ReadableStreamDefaultController_properties[] = {
    JS_PSG("desiredSize", ReadableStreamDefaultController_desiredSize, 0),
    JS_PS_END};

static const JSFunctionSpec ReadableStreamDefaultController_methods[] = {
    JS_FN("close", ReadableStreamDefaultController_close, 0, 0),
    JS_FN("enqueue", ReadableStreamDefaultController_enqueue, 1, 0),
    JS_FN("error", ReadableStreamDefaultController_error, 1, 0), JS_FS_END};

JS_STREAMS_CLASS_SPEC(ReadableStreamDefaultController, 0, SlotCount,
                      ClassSpec::DontDefineConstructor, 0, JS_NULL_CLASS_OPS);

/**
 * Unified implementation of ReadableStream controllers' [[CancelSteps]]
 * internal methods.
 * Streams spec, 3.9.5.1. [[CancelSteps]] ( reason )
 * and
 * Streams spec, 3.11.5.1. [[CancelSteps]] ( reason )
 */
[[nodiscard]] JSObject* js::ReadableStreamControllerCancelSteps(
    JSContext* cx, Handle<ReadableStreamController*> unwrappedController,
    Handle<Value> reason) {
  AssertSameCompartment(cx, reason);

  // Step 1 of 3.11.5.1: If this.[[pendingPullIntos]] is not empty,
  if (!unwrappedController->is<ReadableStreamDefaultController>()) {
    Rooted<ListObject*> unwrappedPendingPullIntos(
        cx, unwrappedController->as<ReadableByteStreamController>()
                .pendingPullIntos());

    if (unwrappedPendingPullIntos->length() != 0) {
      // Step a: Let firstDescriptor be the first element of
      //         this.[[pendingPullIntos]].
      PullIntoDescriptor* unwrappedDescriptor =
          UnwrapAndDowncastObject<PullIntoDescriptor>(
              cx, &unwrappedPendingPullIntos->get(0).toObject());
      if (!unwrappedDescriptor) {
        return nullptr;
      }

      // Step b: Set firstDescriptor.[[bytesFilled]] to 0.
      unwrappedDescriptor->setBytesFilled(0);
    }
  }

  Rooted<Value> unwrappedUnderlyingSource(
      cx, unwrappedController->underlyingSource());

  // Step 1 of 3.9.5.1, step 2 of 3.11.5.1: Perform ! ResetQueue(this).
  if (!ResetQueue(cx, unwrappedController)) {
    return nullptr;
  }

  // Step 2 of 3.9.5.1, step 3 of 3.11.5.1: Let result be the result of
  //     performing this.[[cancelAlgorithm]], passing reason.
  //
  // Our representation of cancel algorithms is a bit awkward, for
  // performance, so we must figure out which algorithm is being invoked.
  Rooted<JSObject*> result(cx);
  if (IsMaybeWrapped<TeeState>(unwrappedUnderlyingSource)) {
    // The cancel algorithm given in ReadableStreamTee step 13 or 14.
    MOZ_ASSERT(unwrappedUnderlyingSource.toObject().is<TeeState>(),
               "tee streams and controllers are always same-compartment with "
               "the TeeState object");
    Rooted<TeeState*> unwrappedTeeState(
        cx, &unwrappedUnderlyingSource.toObject().as<TeeState>());
    Rooted<ReadableStreamDefaultController*> unwrappedDefaultController(
        cx, &unwrappedController->as<ReadableStreamDefaultController>());
    result = ReadableStreamTee_Cancel(cx, unwrappedTeeState,
                                      unwrappedDefaultController, reason);
  } else if (unwrappedController->hasExternalSource()) {
    // An embedding-provided cancel algorithm.
    Rooted<Value> rval(cx);
    {
      AutoRealm ar(cx, unwrappedController);
      JS::ReadableStreamUnderlyingSource* source =
          unwrappedController->externalSource();
      Rooted<ReadableStream*> stream(cx, unwrappedController->stream());
      Rooted<Value> wrappedReason(cx, reason);
      if (!cx->compartment()->wrap(cx, &wrappedReason)) {
        return nullptr;
      }

      cx->check(stream, wrappedReason);
      rval = source->cancel(cx, stream, wrappedReason);
    }

    // Make sure the ReadableStreamControllerClearAlgorithms call below is
    // reached, even on error.
    if (!cx->compartment()->wrap(cx, &rval)) {
      result = nullptr;
    } else {
      result = PromiseObject::unforgeableResolve(cx, rval);
    }
  } else {
    // The algorithm created in
    // SetUpReadableByteStreamControllerFromUnderlyingSource step 5.
    Rooted<Value> unwrappedCancelMethod(cx,
                                        unwrappedController->cancelMethod());
    if (unwrappedCancelMethod.isUndefined()) {
      // CreateAlgorithmFromUnderlyingMethod step 7.
      result = PromiseResolvedWithUndefined(cx);
    } else {
      // CreateAlgorithmFromUnderlyingMethod steps 6.c.i-ii.
      {
        AutoRealm ar(cx, unwrappedController);

        // |unwrappedCancelMethod| and |unwrappedUnderlyingSource| come directly
        // from |unwrappedController| slots so must be same-compartment with it.
        cx->check(unwrappedCancelMethod);
        cx->check(unwrappedUnderlyingSource);

        Rooted<Value> wrappedReason(cx, reason);
        if (!cx->compartment()->wrap(cx, &wrappedReason)) {
          return nullptr;
        }

        // If PromiseCall fails, don't bail out until after the
        // ReadableStreamControllerClearAlgorithms call below.
        result = PromiseCall(cx, unwrappedCancelMethod,
                             unwrappedUnderlyingSource, wrappedReason);
      }
      if (!cx->compartment()->wrap(cx, &result)) {
        result = nullptr;
      }
    }
  }

  // Step 3 (or 4): Perform
  //      ! ReadableStreamDefaultControllerClearAlgorithms(this)
  //      (or ReadableByteStreamControllerClearAlgorithms(this)).
  ReadableStreamControllerClearAlgorithms(unwrappedController);

  // Step 4 (or 5): Return result.
  return result;
}

/**
 * Streams spec, 3.9.5.2.
 *     ReadableStreamDefaultController [[PullSteps]]( forAuthorCode )
 */
PromiseObject* js::ReadableStreamDefaultControllerPullSteps(
    JSContext* cx,
    Handle<ReadableStreamDefaultController*> unwrappedController) {
  // Step 1: Let stream be this.[[controlledReadableStream]].
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 2: If this.[[queue]] is not empty,
  Rooted<ListObject*> unwrappedQueue(cx);
  Rooted<Value> val(
      cx, unwrappedController->getFixedSlot(StreamController::Slot_Queue));
  if (val.isObject()) {
    unwrappedQueue = &val.toObject().as<ListObject>();
  }

  if (unwrappedQueue && unwrappedQueue->length() != 0) {
    // Step a: Let chunk be ! DequeueValue(this).
    Rooted<Value> chunk(cx);
    if (!DequeueValue(cx, unwrappedController, &chunk)) {
      return nullptr;
    }

    // Step b: If this.[[closeRequested]] is true and this.[[queue]] is empty,
    if (unwrappedController->closeRequested() &&
        unwrappedQueue->length() == 0) {
      // Step i: Perform ! ReadableStreamDefaultControllerClearAlgorithms(this).
      ReadableStreamControllerClearAlgorithms(unwrappedController);

      // Step ii: Perform ! ReadableStreamClose(stream).
      if (!ReadableStreamCloseInternal(cx, unwrappedStream)) {
        return nullptr;
      }
    }

    // Step c: Otherwise, perform
    //         ! ReadableStreamDefaultControllerCallPullIfNeeded(this).
    else {
      if (!ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController)) {
        return nullptr;
      }
    }

    // Step d: Return a promise resolved with
    //         ! ReadableStreamCreateReadResult(chunk, false, forAuthorCode).
    cx->check(chunk);
    ReadableStreamReader* unwrappedReader =
        UnwrapReaderFromStream(cx, unwrappedStream);
    if (!unwrappedReader) {
      return nullptr;
    }

    PlainObject* readResultObj = ReadableStreamCreateReadResult(
        cx, chunk, false, unwrappedReader->forAuthorCode());
    if (!readResultObj) {
      return nullptr;
    }

    Rooted<Value> readResult(cx, ObjectValue(*readResultObj));
    return PromiseObject::unforgeableResolveWithNonPromise(cx, readResult);
  }

  // Step 3: Let pendingPromise be
  //         ! ReadableStreamAddReadRequest(stream, forAuthorCode).
  Rooted<PromiseObject*> pendingPromise(
      cx, ReadableStreamAddReadOrReadIntoRequest(cx, unwrappedStream));
  if (!pendingPromise) {
    return nullptr;
  }

  // Step 4: Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(this).
  if (!ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController)) {
    return nullptr;
  }

  // Step 5: Return pendingPromise.
  return pendingPromise;
}
