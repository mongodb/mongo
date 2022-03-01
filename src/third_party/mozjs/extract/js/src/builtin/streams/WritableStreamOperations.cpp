/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Writable stream abstract operations. */

#include "builtin/streams/WritableStreamOperations.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stdint.h>  // uint32_t

#include "jsapi.h"  // JS_ReportErrorASCII

#include "builtin/streams/MiscellaneousOperations.h"  // js::PromiseRejectedWithPendingError
#include "builtin/streams/WritableStream.h"  // js::WritableStream
#include "builtin/streams/WritableStreamDefaultController.h"  // js::WritableStreamDefaultController{,Close}, js::WritableStream::controller
#include "builtin/streams/WritableStreamDefaultControllerOperations.h"  // js::WritableStreamControllerErrorSteps
#include "builtin/streams/WritableStreamWriterOperations.h"  // js::WritableStreamDefaultWriterEnsureReadyPromiseRejected
#include "js/CallArgs.h"              // JS::CallArgs{,FromVp}
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Promise.h"               // JS::{Reject,Resolve}Promise
#include "js/RootingAPI.h"            // JS::Handle, JS::Rooted
#include "js/Value.h"  // JS::Value, JS::ObjecValue, JS::UndefinedHandleValue
#include "vm/Compartment.h"  // JS::Compartment
#include "vm/JSContext.h"    // JSContext
#include "vm/List.h"         // js::ListObject
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined

#include "builtin/HandlerFunction-inl.h"  // js::NewHandler, js::TargetFromHandler
#include "builtin/Promise-inl.h"          // js::SetSettledPromiseIsHandled
#include "builtin/streams/MiscellaneousOperations-inl.h"  // js::ResolveUnwrappedPromiseWithUndefined, js::RejectUnwrappedPromiseWithError
#include "builtin/streams/WritableStream-inl.h"  // js::UnwrapWriterFromStream
#include "builtin/streams/WritableStreamDefaultWriter-inl.h"  // js::WritableStreamDefaultWriter::closedPromise
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::UnwrapAndDowncastObject
#include "vm/JSContext-inl.h"  // JSContext::check
#include "vm/JSObject-inl.h"   // js::NewObjectWithClassProto
#include "vm/List-inl.h"       // js::{AppendTo,StoreNew}ListInFixedSlot
#include "vm/Realm-inl.h"      // js::AutoRealm

using js::ExtraFromHandler;
using js::PromiseObject;
using js::TargetFromHandler;
using js::UnwrapAndDowncastObject;
using js::WritableStream;
using js::WritableStreamDefaultController;
using js::WritableStreamRejectCloseAndClosedPromiseIfNeeded;

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::ObjectValue;
using JS::RejectPromise;
using JS::ResolvePromise;
using JS::Rooted;
using JS::UndefinedHandleValue;
using JS::Value;

/*** 4.3. General writable stream abstract operations. **********************/

/**
 * Streams spec, 4.3.4. InitializeWritableStream ( stream )
 */
/* static */ [[nodiscard]] WritableStream* WritableStream::create(
    JSContext* cx, void* nsISupportsObject_alreadyAddreffed /* = nullptr */,
    Handle<JSObject*> proto /* = nullptr */) {
  cx->check(proto);

  // In the spec, InitializeWritableStream is always passed a newly created
  // WritableStream object. We instead create it here and return it below.
  Rooted<WritableStream*> stream(
      cx, NewObjectWithClassProto<WritableStream>(cx, proto));
  if (!stream) {
    return nullptr;
  }

  stream->setPrivate(nsISupportsObject_alreadyAddreffed);

  stream->initWritableState();

  // Step 1: Set stream.[[state]] to "writable".
  MOZ_ASSERT(stream->writable());

  // Step 2: Set stream.[[storedError]], stream.[[writer]],
  //         stream.[[writableStreamController]],
  //         stream.[[inFlightWriteRequest]], stream.[[closeRequest]],
  //         stream.[[inFlightCloseRequest]] and stream.[[pendingAbortRequest]]
  //         to undefined.
  MOZ_ASSERT(stream->storedError().isUndefined());
  MOZ_ASSERT(!stream->hasWriter());
  MOZ_ASSERT(!stream->hasController());
  MOZ_ASSERT(!stream->haveInFlightWriteRequest());
  MOZ_ASSERT(stream->inFlightWriteRequest().isUndefined());
  MOZ_ASSERT(stream->closeRequest().isUndefined());
  MOZ_ASSERT(stream->inFlightCloseRequest().isUndefined());
  MOZ_ASSERT(!stream->hasPendingAbortRequest());

  // Step 3: Set stream.[[writeRequests]] to a new empty List.
  if (!StoreNewListInFixedSlot(cx, stream,
                               WritableStream::Slot_WriteRequests)) {
    return nullptr;
  }

  // Step 4: Set stream.[[backpressure]] to false.
  MOZ_ASSERT(!stream->backpressure());

  return stream;
}

void WritableStream::clearInFlightWriteRequest(JSContext* cx) {
  MOZ_ASSERT(stateIsInitialized());
  MOZ_ASSERT(haveInFlightWriteRequest());

  writeRequests()->popFirst(cx);
  setFlag(HaveInFlightWriteRequest, false);

  MOZ_ASSERT(!haveInFlightWriteRequest());
  MOZ_ASSERT(inFlightWriteRequest().isUndefined());
}

/**
 * Streams spec, 4.3.6.
 *      WritableStreamAbort ( stream, reason )
 *
 * Note: The object (a promise) returned by this function is in the current
 *       compartment and does not require special wrapping to be put to use.
 */
JSObject* js::WritableStreamAbort(JSContext* cx,
                                  Handle<WritableStream*> unwrappedStream,
                                  Handle<Value> reason) {
  cx->check(reason);

  // Step 1: Let state be stream.[[state]].
  // Step 2: If state is "closed" or "errored", return a promise resolved with
  //         undefined.
  if (unwrappedStream->closed() || unwrappedStream->errored()) {
    return PromiseResolvedWithUndefined(cx);
  }

  // Step 3: If stream.[[pendingAbortRequest]] is not undefined, return
  //         stream.[[pendingAbortRequest]].[[promise]].
  if (unwrappedStream->hasPendingAbortRequest()) {
    Rooted<JSObject*> pendingPromise(
        cx, unwrappedStream->pendingAbortRequestPromise());
    if (!cx->compartment()->wrap(cx, &pendingPromise)) {
      return nullptr;
    }
    return pendingPromise;
  }

  // Step 4: Assert: state is "writable" or "erroring".
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());

  // Step 7: Let promise be a new promise (reordered).
  Rooted<PromiseObject*> promise(cx, PromiseObject::createSkippingExecutor(cx));
  if (!promise) {
    return nullptr;
  }

  // Step 5: Let wasAlreadyErroring be false.
  // Step 6: If state is "erroring",
  // Step 6.a: Set wasAlreadyErroring to true.
  // Step 6.b: Set reason to undefined.
  bool wasAlreadyErroring = unwrappedStream->erroring();
  Handle<Value> pendingReason =
      wasAlreadyErroring ? UndefinedHandleValue : reason;

  // Step 8: Set stream.[[pendingAbortRequest]] to
  //         Record {[[promise]]: promise, [[reason]]: reason,
  //                 [[wasAlreadyErroring]]: wasAlreadyErroring}.
  {
    AutoRealm ar(cx, unwrappedStream);

    Rooted<JSObject*> wrappedPromise(cx, promise);
    Rooted<Value> wrappedPendingReason(cx, pendingReason);

    JS::Compartment* comp = cx->compartment();
    if (!comp->wrap(cx, &wrappedPromise) ||
        !comp->wrap(cx, &wrappedPendingReason)) {
      return nullptr;
    }

    unwrappedStream->setPendingAbortRequest(
        wrappedPromise, wrappedPendingReason, wasAlreadyErroring);
  }

  // Step 9: If wasAlreadyErroring is false, perform
  //         ! WritableStreamStartErroring(stream, reason).
  if (!wasAlreadyErroring) {
    if (!WritableStreamStartErroring(cx, unwrappedStream, pendingReason)) {
      return nullptr;
    }
  }

  // Step 10: Return promise.
  return promise;
}

/**
 * Streams spec, 4.3.7.
 *      WritableStreamClose ( stream )
 *
 * Note: The object (a promise) returned by this function is in the current
 *       compartment and does not require special wrapping to be put to use.
 */
JSObject* js::WritableStreamClose(JSContext* cx,
                                  Handle<WritableStream*> unwrappedStream) {
  // Step 1: Let state be stream.[[state]].
  // Step 2: If state is "closed" or "errored", return a promise rejected with a
  //         TypeError exception.
  if (unwrappedStream->closed() || unwrappedStream->errored()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAM_CLOSED_OR_ERRORED);
    return PromiseRejectedWithPendingError(cx);
  }

  // Step 3: Assert: state is "writable" or "erroring".
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());

  // Step 4: Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
  MOZ_ASSERT(!WritableStreamCloseQueuedOrInFlight(unwrappedStream));

  // Step 5: Let promise be a new promise.
  Rooted<PromiseObject*> promise(cx, PromiseObject::createSkippingExecutor(cx));
  if (!promise) {
    return nullptr;
  }

  // Step 6: Set stream.[[closeRequest]] to promise.
  {
    AutoRealm ar(cx, unwrappedStream);
    Rooted<JSObject*> wrappedPromise(cx, promise);
    if (!cx->compartment()->wrap(cx, &wrappedPromise)) {
      return nullptr;
    }

    unwrappedStream->setCloseRequest(promise);
  }

  // Step 7: Let writer be stream.[[writer]].
  // Step 8: If writer is not undefined, and stream.[[backpressure]] is true,
  //         and state is "writable", resolve writer.[[readyPromise]] with
  //         undefined.
  if (unwrappedStream->hasWriter() && unwrappedStream->backpressure() &&
      unwrappedStream->writable()) {
    Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
        cx, UnwrapWriterFromStream(cx, unwrappedStream));
    if (!unwrappedWriter) {
      return nullptr;
    }

    if (!ResolveUnwrappedPromiseWithUndefined(
            cx, unwrappedWriter->readyPromise())) {
      return nullptr;
    }
  }

  // Step 9: Perform
  //         ! WritableStreamDefaultControllerClose(
  //               stream.[[writableStreamController]]).
  Rooted<WritableStreamDefaultController*> unwrappedController(
      cx, unwrappedStream->controller());
  if (!WritableStreamDefaultControllerClose(cx, unwrappedController)) {
    return nullptr;
  }

  // Step 10: Return promise.
  return promise;
}

/*** 4.4. Writable stream abstract operations used by controllers ***********/

/**
 * Streams spec, 4.4.1.
 *      WritableStreamAddWriteRequest ( stream )
 */
[[nodiscard]] PromiseObject* js::WritableStreamAddWriteRequest(
    JSContext* cx, Handle<WritableStream*> unwrappedStream) {
  // Step 1: Assert: ! IsWritableStreamLocked(stream) is true.
  MOZ_ASSERT(unwrappedStream->isLocked());

  // Step 2: Assert: stream.[[state]] is "writable".
  MOZ_ASSERT(unwrappedStream->writable());

  // Step 3: Let promise be a new promise.
  Rooted<PromiseObject*> promise(cx, PromiseObject::createSkippingExecutor(cx));
  if (!promise) {
    return nullptr;
  }

  // Step 4: Append promise as the last element of stream.[[writeRequests]].
  if (!AppendToListInFixedSlot(cx, unwrappedStream,
                               WritableStream::Slot_WriteRequests, promise)) {
    return nullptr;
  }

  // Step 5: Return promise.
  return promise;
}

/**
 * Streams spec, 4.4.2.
 *      WritableStreamDealWithRejection ( stream, error )
 */
[[nodiscard]] bool js::WritableStreamDealWithRejection(
    JSContext* cx, Handle<WritableStream*> unwrappedStream,
    Handle<Value> error) {
  cx->check(error);

  // Step 1: Let state be stream.[[state]].
  // Step 2: If state is "writable",
  if (unwrappedStream->writable()) {
    // Step 2a: Perform ! WritableStreamStartErroring(stream, error).
    // Step 2b: Return.
    return WritableStreamStartErroring(cx, unwrappedStream, error);
  }

  // Step 3: Assert: state is "erroring".
  MOZ_ASSERT(unwrappedStream->erroring());

  // Step 4: Perform ! WritableStreamFinishErroring(stream).
  return WritableStreamFinishErroring(cx, unwrappedStream);
}

static bool WritableStreamHasOperationMarkedInFlight(
    const WritableStream* unwrappedStream);

/**
 * Streams spec, 4.4.3.
 *      WritableStreamStartErroring ( stream, reason )
 */
[[nodiscard]] bool js::WritableStreamStartErroring(
    JSContext* cx, Handle<WritableStream*> unwrappedStream,
    Handle<Value> reason) {
  cx->check(reason);

  // Step 1: Assert: stream.[[storedError]] is undefined.
  MOZ_ASSERT(unwrappedStream->storedError().isUndefined());

  // Step 2: Assert: stream.[[state]] is "writable".
  MOZ_ASSERT(unwrappedStream->writable());

  // Step 3: Let controller be stream.[[writableStreamController]].
  // Step 4: Assert: controller is not undefined.
  MOZ_ASSERT(unwrappedStream->hasController());
  Rooted<WritableStreamDefaultController*> unwrappedController(
      cx, unwrappedStream->controller());

  // Step 5: Set stream.[[state]] to "erroring".
  unwrappedStream->setErroring();

  // Step 6: Set stream.[[storedError]] to reason.
  {
    AutoRealm ar(cx, unwrappedStream);
    Rooted<Value> wrappedReason(cx, reason);
    if (!cx->compartment()->wrap(cx, &wrappedReason)) {
      return false;
    }
    unwrappedStream->setStoredError(wrappedReason);
  }

  // Step 7: Let writer be stream.[[writer]].
  // Step 8: If writer is not undefined, perform
  //         ! WritableStreamDefaultWriterEnsureReadyPromiseRejected(
  //             writer, reason).
  if (unwrappedStream->hasWriter()) {
    Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
        cx, UnwrapWriterFromStream(cx, unwrappedStream));
    if (!unwrappedWriter) {
      return false;
    }

    if (!WritableStreamDefaultWriterEnsureReadyPromiseRejected(
            cx, unwrappedWriter, reason)) {
      return false;
    }
  }

  // Step 9: If ! WritableStreamHasOperationMarkedInFlight(stream) is false and
  //         controller.[[started]] is true, perform
  //         ! WritableStreamFinishErroring(stream).
  if (!WritableStreamHasOperationMarkedInFlight(unwrappedStream) &&
      unwrappedController->started()) {
    if (!WritableStreamFinishErroring(cx, unwrappedStream)) {
      return false;
    }
  }

  return true;
}

/**
 * Streams spec, 4.4.4 WritableStreamFinishErroring ( stream )
 *     Step 13: Upon fulfillment of promise, [...]
 */
static bool AbortRequestPromiseFulfilledHandler(JSContext* cx, unsigned argc,
                                                Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 13.a: Resolve abortRequest.[[promise]] with undefined.
  Rooted<JSObject*> abortRequestPromise(cx, TargetFromHandler<JSObject>(args));
  if (!ResolvePromise(cx, abortRequestPromise, UndefinedHandleValue)) {
    return false;
  }

  // Step 13.b: Perform
  //            ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
  Rooted<WritableStream*> unwrappedStream(
      cx, UnwrapAndDowncastObject<WritableStream>(
              cx, ExtraFromHandler<JSObject>(args)));
  if (!unwrappedStream) {
    return false;
  }

  if (!WritableStreamRejectCloseAndClosedPromiseIfNeeded(cx, unwrappedStream)) {
    return false;
  }

  args.rval().setUndefined();
  return false;
}

/**
 * Streams spec, 4.4.4 WritableStreamFinishErroring ( stream )
 *     Step 14: Upon rejection of promise with reason reason, [...]
 */
static bool AbortRequestPromiseRejectedHandler(JSContext* cx, unsigned argc,
                                               Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 14.a: Reject abortRequest.[[promise]] with reason.
  Rooted<JSObject*> abortRequestPromise(cx, TargetFromHandler<JSObject>(args));
  if (!RejectPromise(cx, abortRequestPromise, args.get(0))) {
    return false;
  }

  // Step 14.b: Perform
  //            ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
  Rooted<WritableStream*> unwrappedStream(
      cx, UnwrapAndDowncastObject<WritableStream>(
              cx, ExtraFromHandler<JSObject>(args)));
  if (!unwrappedStream) {
    return false;
  }

  if (!WritableStreamRejectCloseAndClosedPromiseIfNeeded(cx, unwrappedStream)) {
    return false;
  }

  args.rval().setUndefined();
  return false;
}

/**
 * Streams spec, 4.4.4.
 *      WritableStreamFinishErroring ( stream )
 */
[[nodiscard]] bool js::WritableStreamFinishErroring(
    JSContext* cx, Handle<WritableStream*> unwrappedStream) {
  // Step 1: Assert: stream.[[state]] is "erroring".
  MOZ_ASSERT(unwrappedStream->erroring());

  // Step 2: Assert: ! WritableStreamHasOperationMarkedInFlight(stream) is
  //         false.
  MOZ_ASSERT(!WritableStreamHasOperationMarkedInFlight(unwrappedStream));

  // Step 3: Set stream.[[state]] to "errored".
  unwrappedStream->setErrored();

  // Step 4: Perform ! stream.[[writableStreamController]].[[ErrorSteps]]().
  {
    Rooted<WritableStreamDefaultController*> unwrappedController(
        cx, unwrappedStream->controller());
    if (!WritableStreamControllerErrorSteps(cx, unwrappedController)) {
      return false;
    }
  }

  // Step 5: Let storedError be stream.[[storedError]].
  Rooted<Value> storedError(cx, unwrappedStream->storedError());
  if (!cx->compartment()->wrap(cx, &storedError)) {
    return false;
  }

  // Step 6: Repeat for each writeRequest that is an element of
  //         stream.[[writeRequests]],
  {
    Rooted<ListObject*> unwrappedWriteRequests(
        cx, unwrappedStream->writeRequests());
    Rooted<JSObject*> writeRequest(cx);
    uint32_t len = unwrappedWriteRequests->length();
    for (uint32_t i = 0; i < len; i++) {
      // Step 6.a: Reject writeRequest with storedError.
      writeRequest = &unwrappedWriteRequests->get(i).toObject();
      if (!RejectUnwrappedPromiseWithError(cx, &writeRequest, storedError)) {
        return false;
      }
    }
  }

  // Step 7: Set stream.[[writeRequests]] to an empty List.
  // We optimize this to discard the list entirely.  (A brief scan of the
  // streams spec should verify that [[writeRequests]] is never accessed on a
  // stream when |stream.[[state]] === "errored"|, set in step 3 above.)
  unwrappedStream->clearWriteRequests();

  // Step 8: If stream.[[pendingAbortRequest]] is undefined,
  if (!unwrappedStream->hasPendingAbortRequest()) {
    // Step 8.a: Perform
    //           ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
    // Step 8.b: Return.
    return WritableStreamRejectCloseAndClosedPromiseIfNeeded(cx,
                                                             unwrappedStream);
  }

  // Step 9: Let abortRequest be stream.[[pendingAbortRequest]].
  // Step 10: Set stream.[[pendingAbortRequest]] to undefined.
  Rooted<Value> abortRequestReason(
      cx, unwrappedStream->pendingAbortRequestReason());
  if (!cx->compartment()->wrap(cx, &abortRequestReason)) {
    return false;
  }
  Rooted<JSObject*> abortRequestPromise(
      cx, unwrappedStream->pendingAbortRequestPromise());
  bool wasAlreadyErroring =
      unwrappedStream->pendingAbortRequestWasAlreadyErroring();
  unwrappedStream->clearPendingAbortRequest();

  // Step 11: If abortRequest.[[wasAlreadyErroring]] is true,
  if (wasAlreadyErroring) {
    // Step 11.a: Reject abortRequest.[[promise]] with storedError.
    if (!RejectUnwrappedPromiseWithError(cx, &abortRequestPromise,
                                         storedError)) {
      return false;
    }

    // Step 11.b: Perform
    //            ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
    // Step 11.c: Return.
    return WritableStreamRejectCloseAndClosedPromiseIfNeeded(cx,
                                                             unwrappedStream);
  }

  // Step 12: Let promise be
  //          ! stream.[[writableStreamController]].[[AbortSteps]](
  //                abortRequest.[[reason]]).
  Rooted<WritableStreamDefaultController*> unwrappedController(
      cx, unwrappedStream->controller());
  Rooted<JSObject*> promise(
      cx, WritableStreamControllerAbortSteps(cx, unwrappedController,
                                             abortRequestReason));
  if (!promise) {
    return false;
  }
  cx->check(promise);

  if (!cx->compartment()->wrap(cx, &abortRequestPromise)) {
    return false;
  }

  Rooted<JSObject*> stream(cx, unwrappedStream);
  if (!cx->compartment()->wrap(cx, &stream)) {
    return false;
  }

  // Step 13: Upon fulfillment of promise, [...]
  // Step 14: Upon rejection of promise with reason reason, [...]
  Rooted<JSObject*> onFulfilled(
      cx, NewHandlerWithExtra(cx, AbortRequestPromiseFulfilledHandler,
                              abortRequestPromise, stream));
  if (!onFulfilled) {
    return false;
  }
  Rooted<JSObject*> onRejected(
      cx, NewHandlerWithExtra(cx, AbortRequestPromiseRejectedHandler,
                              abortRequestPromise, stream));
  if (!onRejected) {
    return false;
  }

  return JS::AddPromiseReactions(cx, promise, onFulfilled, onRejected);
}

/**
 * Streams spec, 4.4.5.
 *      WritableStreamFinishInFlightWrite ( stream )
 */
[[nodiscard]] bool js::WritableStreamFinishInFlightWrite(
    JSContext* cx, Handle<WritableStream*> unwrappedStream) {
  // Step 1: Assert: stream.[[inFlightWriteRequest]] is not undefined.
  MOZ_ASSERT(unwrappedStream->haveInFlightWriteRequest());

  // Step 2: Resolve stream.[[inFlightWriteRequest]] with undefined.
  if (!ResolveUnwrappedPromiseWithUndefined(
          cx, &unwrappedStream->inFlightWriteRequest().toObject())) {
    return false;
  }

  // Step 3: Set stream.[[inFlightWriteRequest]] to undefined.
  unwrappedStream->clearInFlightWriteRequest(cx);
  MOZ_ASSERT(!unwrappedStream->haveInFlightWriteRequest());

  return true;
}

/**
 * Streams spec, 4.4.6.
 *      WritableStreamFinishInFlightWriteWithError ( stream, error )
 */
[[nodiscard]] bool js::WritableStreamFinishInFlightWriteWithError(
    JSContext* cx, Handle<WritableStream*> unwrappedStream,
    Handle<Value> error) {
  cx->check(error);

  // Step 1: Assert: stream.[[inFlightWriteRequest]] is not undefined.
  MOZ_ASSERT(unwrappedStream->haveInFlightWriteRequest());

  // Step 2:  Reject stream.[[inFlightWriteRequest]] with error.
  if (!RejectUnwrappedPromiseWithError(
          cx, &unwrappedStream->inFlightWriteRequest().toObject(), error)) {
    return false;
  }

  // Step 3:  Set stream.[[inFlightWriteRequest]] to undefined.
  unwrappedStream->clearInFlightWriteRequest(cx);

  // Step 4:  Assert: stream.[[state]] is "writable" or "erroring".
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());

  // Step 5:  Perform ! WritableStreamDealWithRejection(stream, error).
  return WritableStreamDealWithRejection(cx, unwrappedStream, error);
}

/**
 * Streams spec, 4.4.7.
 *      WritableStreamFinishInFlightClose ( stream )
 */
[[nodiscard]] bool js::WritableStreamFinishInFlightClose(
    JSContext* cx, Handle<WritableStream*> unwrappedStream) {
  // Step 1: Assert: stream.[[inFlightCloseRequest]] is not undefined.
  MOZ_ASSERT(unwrappedStream->haveInFlightCloseRequest());

  // Step 2: Resolve stream.[[inFlightCloseRequest]] with undefined.
  if (!ResolveUnwrappedPromiseWithUndefined(
          cx, &unwrappedStream->inFlightCloseRequest().toObject())) {
    return false;
  }

  // Step 3: Set stream.[[inFlightCloseRequest]] to undefined.
  unwrappedStream->clearInFlightCloseRequest();
  MOZ_ASSERT(unwrappedStream->inFlightCloseRequest().isUndefined());

  // Step 4: Let state be stream.[[state]].
  // Step 5: Assert: stream.[[state]] is "writable" or "erroring".
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());

  // Step 6: If state is "erroring",
  if (unwrappedStream->erroring()) {
    // Step 6.a: Set stream.[[storedError]] to undefined.
    unwrappedStream->clearStoredError();

    // Step 6.b: If stream.[[pendingAbortRequest]] is not undefined,
    if (unwrappedStream->hasPendingAbortRequest()) {
      // Step 6.b.i: Resolve stream.[[pendingAbortRequest]].[[promise]] with
      //             undefined.
      if (!ResolveUnwrappedPromiseWithUndefined(
              cx, unwrappedStream->pendingAbortRequestPromise())) {
        return false;
      }

      // Step 6.b.ii: Set stream.[[pendingAbortRequest]] to undefined.
      unwrappedStream->clearPendingAbortRequest();
    }
  }

  // Step 7: Set stream.[[state]] to "closed".
  unwrappedStream->setClosed();

  // Step 8: Let writer be stream.[[writer]].
  // Step 9: If writer is not undefined, resolve writer.[[closedPromise]] with
  //         undefined.
  if (unwrappedStream->hasWriter()) {
    WritableStreamDefaultWriter* unwrappedWriter =
        UnwrapWriterFromStream(cx, unwrappedStream);
    if (!unwrappedWriter) {
      return false;
    }

    if (!ResolveUnwrappedPromiseWithUndefined(
            cx, unwrappedWriter->closedPromise())) {
      return false;
    }
  }

  // Step 10: Assert: stream.[[pendingAbortRequest]] is undefined.
  MOZ_ASSERT(!unwrappedStream->hasPendingAbortRequest());

  // Step 11: Assert: stream.[[storedError]] is undefined.
  MOZ_ASSERT(unwrappedStream->storedError().isUndefined());

  return true;
}

/**
 * Streams spec, 4.4.8.
 *      WritableStreamFinishInFlightCloseWithError ( stream, error )
 */
[[nodiscard]] bool js::WritableStreamFinishInFlightCloseWithError(
    JSContext* cx, Handle<WritableStream*> unwrappedStream,
    Handle<Value> error) {
  cx->check(error);

  // Step 1: Assert: stream.[[inFlightCloseRequest]] is not undefined.
  MOZ_ASSERT(unwrappedStream->haveInFlightCloseRequest());
  MOZ_ASSERT(!unwrappedStream->inFlightCloseRequest().isUndefined());

  // Step 2: Reject stream.[[inFlightCloseRequest]] with error.
  if (!RejectUnwrappedPromiseWithError(
          cx, &unwrappedStream->inFlightCloseRequest().toObject(), error)) {
    return false;
  }

  // Step 3: Set stream.[[inFlightCloseRequest]] to undefined.
  unwrappedStream->clearInFlightCloseRequest();

  // Step 4: Assert: stream.[[state]] is "writable" or "erroring".
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());

  // Step 5: If stream.[[pendingAbortRequest]] is not undefined,
  if (unwrappedStream->hasPendingAbortRequest()) {
    // Step 5.a: Reject stream.[[pendingAbortRequest]].[[promise]] with error.
    if (!RejectUnwrappedPromiseWithError(
            cx, unwrappedStream->pendingAbortRequestPromise(), error)) {
      return false;
    }

    // Step 5.b: Set stream.[[pendingAbortRequest]] to undefined.
    unwrappedStream->clearPendingAbortRequest();
  }

  // Step 6: Perform ! WritableStreamDealWithRejection(stream, error).
  return WritableStreamDealWithRejection(cx, unwrappedStream, error);
}

/**
 * Streams spec, 4.4.9.
 *      WritableStreamCloseQueuedOrInFlight ( stream )
 */
bool js::WritableStreamCloseQueuedOrInFlight(
    const WritableStream* unwrappedStream) {
  // Step 1: If stream.[[closeRequest]] is undefined and
  //         stream.[[inFlightCloseRequest]] is undefined, return false.
  // Step 2: Return true.
  return unwrappedStream->haveCloseRequestOrInFlightCloseRequest();
}

/**
 * Streams spec, 4.4.10.
 *      WritableStreamHasOperationMarkedInFlight ( stream )
 */
bool WritableStreamHasOperationMarkedInFlight(
    const WritableStream* unwrappedStream) {
  // Step 1: If stream.[[inFlightWriteRequest]] is undefined and
  //         controller.[[inFlightCloseRequest]] is undefined, return false.
  // Step 2: Return true.
  return unwrappedStream->haveInFlightWriteRequest() ||
         unwrappedStream->haveInFlightCloseRequest();
}

/**
 * Streams spec, 4.4.11.
 *      WritableStreamMarkCloseRequestInFlight ( stream )
 */
void js::WritableStreamMarkCloseRequestInFlight(
    WritableStream* unwrappedStream) {
  // Step 1: Assert: stream.[[inFlightCloseRequest]] is undefined.
  MOZ_ASSERT(!unwrappedStream->haveInFlightCloseRequest());

  // Step 2: Assert: stream.[[closeRequest]] is not undefined.
  MOZ_ASSERT(!unwrappedStream->closeRequest().isUndefined());

  // Step 3: Set stream.[[inFlightCloseRequest]] to stream.[[closeRequest]].
  // Step 4: Set stream.[[closeRequest]] to undefined.
  unwrappedStream->convertCloseRequestToInFlightCloseRequest();
}

/**
 * Streams spec, 4.4.12.
 *      WritableStreamMarkFirstWriteRequestInFlight ( stream )
 */
void js::WritableStreamMarkFirstWriteRequestInFlight(
    WritableStream* unwrappedStream) {
  // Step 1: Assert: stream.[[inFlightWriteRequest]] is undefined.
  MOZ_ASSERT(!unwrappedStream->haveInFlightWriteRequest());

  // Step 2: Assert: stream.[[writeRequests]] is not empty.
  MOZ_ASSERT(unwrappedStream->writeRequests()->length() > 0);

  // Step 3: Let writeRequest be the first element of stream.[[writeRequests]].
  // Step 4: Remove writeRequest from stream.[[writeRequests]], shifting all
  //         other elements downward (so that the second becomes the first, and
  //         so on).
  // Step 5: Set stream.[[inFlightWriteRequest]] to writeRequest.
  // In our implementation, we model [[inFlightWriteRequest]] as merely the
  // first element of [[writeRequests]], plus a flag indicating there's an
  // in-flight request.  Set the flag and be done with it.
  unwrappedStream->setHaveInFlightWriteRequest();
}

/**
 * Streams spec, 4.4.13.
 *      WritableStreamRejectCloseAndClosedPromiseIfNeeded ( stream )
 */
[[nodiscard]] bool js::WritableStreamRejectCloseAndClosedPromiseIfNeeded(
    JSContext* cx, Handle<WritableStream*> unwrappedStream) {
  // Step 1: Assert: stream.[[state]] is "errored".
  MOZ_ASSERT(unwrappedStream->errored());

  Rooted<Value> storedError(cx, unwrappedStream->storedError());
  if (!cx->compartment()->wrap(cx, &storedError)) {
    return false;
  }

  // Step 2: If stream.[[closeRequest]] is not undefined,
  if (!unwrappedStream->closeRequest().isUndefined()) {
    // Step 2.a: Assert: stream.[[inFlightCloseRequest]] is undefined.
    MOZ_ASSERT(unwrappedStream->inFlightCloseRequest().isUndefined());

    // Step 2.b: Reject stream.[[closeRequest]] with stream.[[storedError]].
    if (!RejectUnwrappedPromiseWithError(
            cx, &unwrappedStream->closeRequest().toObject(), storedError)) {
      return false;
    }

    // Step 2.c: Set stream.[[closeRequest]] to undefined.
    unwrappedStream->clearCloseRequest();
  }

  // Step 3: Let writer be stream.[[writer]].
  // Step 4: If writer is not undefined,
  if (unwrappedStream->hasWriter()) {
    Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
        cx, UnwrapWriterFromStream(cx, unwrappedStream));
    if (!unwrappedWriter) {
      return false;
    }

    // Step 4.a: Reject writer.[[closedPromise]] with stream.[[storedError]].
    if (!RejectUnwrappedPromiseWithError(cx, unwrappedWriter->closedPromise(),
                                         storedError)) {
      return false;
    }

    // Step 4.b: Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
    Rooted<PromiseObject*> unwrappedClosedPromise(
        cx, UnwrapAndDowncastObject<PromiseObject>(
                cx, unwrappedWriter->closedPromise()));
    if (!unwrappedClosedPromise) {
      return false;
    }

    js::SetSettledPromiseIsHandled(cx, unwrappedClosedPromise);
  }

  return true;
}

/**
 * Streams spec, 4.4.14.
 *      WritableStreamUpdateBackpressure ( stream, backpressure )
 */
[[nodiscard]] bool js::WritableStreamUpdateBackpressure(
    JSContext* cx, Handle<WritableStream*> unwrappedStream, bool backpressure) {
  // Step 1: Assert: stream.[[state]] is "writable".
  MOZ_ASSERT(unwrappedStream->writable());

  // Step 2: Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
  MOZ_ASSERT(!WritableStreamCloseQueuedOrInFlight(unwrappedStream));

  // Step 3: Let writer be stream.[[writer]].
  // Step 4: If writer is not undefined and backpressure is not
  //         stream.[[backpressure]],
  if (unwrappedStream->hasWriter() &&
      backpressure != unwrappedStream->backpressure()) {
    Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
        cx, UnwrapWriterFromStream(cx, unwrappedStream));
    if (!unwrappedWriter) {
      return false;
    }

    // Step 4.a: If backpressure is true, set writer.[[readyPromise]] to a new
    //           promise.
    if (backpressure) {
      Rooted<JSObject*> promise(cx, PromiseObject::createSkippingExecutor(cx));
      if (!promise) {
        return false;
      }

      AutoRealm ar(cx, unwrappedWriter);
      if (!cx->compartment()->wrap(cx, &promise)) {
        return false;
      }
      unwrappedWriter->setReadyPromise(promise);
    } else {
      // Step 4.b: Otherwise,
      // Step 4.b.i: Assert: backpressure is false.  (guaranteed by type)
      // Step 4.b.ii: Resolve writer.[[readyPromise]] with undefined.
      if (!ResolveUnwrappedPromiseWithUndefined(
              cx, unwrappedWriter->readyPromise())) {
        return false;
      }
    }
  }

  // Step 5: Set stream.[[backpressure]] to backpressure.
  unwrappedStream->setBackpressure(backpressure);

  return true;
}
