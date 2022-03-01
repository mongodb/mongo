/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Writable stream writer abstract operations. */

#include "builtin/streams/WritableStreamWriterOperations.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jsapi.h"  // JS_ReportErrorNumberASCII, JS_ReportErrorASCII

#include "builtin/streams/MiscellaneousOperations.h"  // js::PromiseRejectedWithPendingError
#include "builtin/streams/WritableStream.h"  // js::WritableStream
#include "builtin/streams/WritableStreamDefaultController.h"  // js::WritableStream::controller
#include "builtin/streams/WritableStreamDefaultControllerOperations.h"  // js::WritableStreamDefaultController{Close,GetDesiredSize}
#include "builtin/streams/WritableStreamDefaultWriter.h"  // js::WritableStreamDefaultWriter
#include "builtin/streams/WritableStreamOperations.h"  // js::WritableStream{Abort,CloseQueuedOrInFlight}
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Promise.h"               // JS::PromiseState
#include "js/Value.h"                 // JS::Value, JS::{Int32,Null}Value
#include "vm/Compartment.h"           // JS::Compartment
#include "vm/Interpreter.h"           // js::GetAndClearException
#include "vm/JSContext.h"             // JSContext
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined

#include "builtin/Promise-inl.h"  // js::SetSettledPromiseIsHandled
#include "builtin/streams/MiscellaneousOperations-inl.h"  // js::ResolveUnwrappedPromiseWithUndefined
#include "builtin/streams/WritableStream-inl.h"  // js::WritableStream::setCloseRequest
#include "builtin/streams/WritableStreamDefaultWriter-inl.h"  // js::UnwrapStreamFromWriter
#include "vm/Compartment-inl.h"  // js::UnwrapAnd{DowncastObject,TypeCheckThis}
#include "vm/JSContext-inl.h"    // JSContext::check
#include "vm/Realm-inl.h"        // js::AutoRealm

using JS::Handle;
using JS::Int32Value;
using JS::MutableHandle;
using JS::NullValue;
using JS::NumberValue;
using JS::Rooted;
using JS::Value;

using js::AutoRealm;
using js::PromiseObject;
using js::UnwrapAndDowncastObject;
using js::WritableStreamDefaultWriter;

/*** 4.6. Writable stream writer abstract operations ************************/

/**
 * Streams spec, 4.6.2.
 * WritableStreamDefaultWriterAbort ( writer, reason )
 */
JSObject* js::WritableStreamDefaultWriterAbort(
    JSContext* cx, Handle<WritableStreamDefaultWriter*> unwrappedWriter,
    Handle<Value> reason) {
  cx->check(reason);

  // Step 1: Let stream be writer.[[ownerWritableStream]].
  // Step 2: Assert: stream is not undefined.
  MOZ_ASSERT(unwrappedWriter->hasStream());
  Rooted<WritableStream*> unwrappedStream(
      cx, UnwrapStreamFromWriter(cx, unwrappedWriter));
  if (!unwrappedStream) {
    return nullptr;
  }

  // Step 3: Return ! WritableStreamAbort(stream, reason).
  return WritableStreamAbort(cx, unwrappedStream, reason);
}

/**
 * Streams spec, 4.6.3.
 * WritableStreamDefaultWriterClose ( writer )
 */
PromiseObject* js::WritableStreamDefaultWriterClose(
    JSContext* cx, Handle<WritableStreamDefaultWriter*> unwrappedWriter) {
  // Step 1: Let stream be writer.[[ownerWritableStream]].
  // Step 2: Assert: stream is not undefined.
  MOZ_ASSERT(unwrappedWriter->hasStream());
  Rooted<WritableStream*> unwrappedStream(
      cx, UnwrapStreamFromWriter(cx, unwrappedWriter));
  if (!unwrappedStream) {
    return PromiseRejectedWithPendingError(cx);
  }

  // Step 3: Let state be stream.[[state]].
  // Step 4: If state is "closed" or "errored", return a promise rejected with a
  //         TypeError exception.
  if (unwrappedStream->closed() || unwrappedStream->errored()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAM_CLOSED_OR_ERRORED);
    return PromiseRejectedWithPendingError(cx);
  }

  // Step 5: Assert: state is "writable" or "erroring".
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());

  // Step 6: Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
  MOZ_ASSERT(!WritableStreamCloseQueuedOrInFlight(unwrappedStream));

  // Step 7: Let promise be a new promise.
  Rooted<PromiseObject*> promise(cx, PromiseObject::createSkippingExecutor(cx));
  if (!promise) {
    return nullptr;
  }

  // Step 8: Set stream.[[closeRequest]] to promise.
  {
    AutoRealm ar(cx, unwrappedStream);
    Rooted<JSObject*> closeRequest(cx, promise);
    if (!cx->compartment()->wrap(cx, &closeRequest)) {
      return nullptr;
    }

    unwrappedStream->setCloseRequest(closeRequest);
  }

  // Step 9: If stream.[[backpressure]] is true and state is "writable", resolve
  //         writer.[[readyPromise]] with undefined.
  if (unwrappedStream->backpressure() && unwrappedStream->writable()) {
    if (!ResolveUnwrappedPromiseWithUndefined(
            cx, unwrappedWriter->readyPromise())) {
      return nullptr;
    }
  }

  // Step 10: Perform
  //          ! WritableStreamDefaultControllerClose(
  //              stream.[[writableStreamController]]).
  Rooted<WritableStreamDefaultController*> unwrappedController(
      cx, unwrappedStream->controller());
  if (!WritableStreamDefaultControllerClose(cx, unwrappedController)) {
    return nullptr;
  }

  // Step 11: Return promise.
  return promise;
}

/**
 * Streams spec.
 * WritableStreamDefaultWriterCloseWithErrorPropagation ( writer )
 */
PromiseObject* js::WritableStreamDefaultWriterCloseWithErrorPropagation(
    JSContext* cx, Handle<WritableStreamDefaultWriter*> unwrappedWriter) {
  // Step 1: Let stream be writer.[[ownerWritableStream]].
  // Step 2: Assert: stream is not undefined.
  WritableStream* unwrappedStream = UnwrapStreamFromWriter(cx, unwrappedWriter);
  if (!unwrappedStream) {
    return nullptr;
  }

  // Step 3: Let state be stream.[[state]].
  // Step 4: If ! WritableStreamCloseQueuedOrInFlight(stream) is true or state
  //         is "closed", return a promise resolved with undefined.
  if (WritableStreamCloseQueuedOrInFlight(unwrappedStream) ||
      unwrappedStream->closed()) {
    return PromiseResolvedWithUndefined(cx);
  }

  // Step 5: If state is "errored", return a promise rejected with
  //         stream.[[storedError]].
  if (unwrappedStream->errored()) {
    Rooted<Value> storedError(cx, unwrappedStream->storedError());
    if (!cx->compartment()->wrap(cx, &storedError)) {
      return nullptr;
    }

    return PromiseObject::unforgeableReject(cx, storedError);
  }

  // Step 6: Assert: state is "writable" or "erroring".
  MOZ_ASSERT(unwrappedStream->writable() ^ unwrappedStream->erroring());

  // Step 7: Return ! WritableStreamDefaultWriterClose(writer).
  return WritableStreamDefaultWriterClose(cx, unwrappedWriter);
}

using GetField = JSObject* (WritableStreamDefaultWriter::*)() const;
using SetField = void (WritableStreamDefaultWriter::*)(JSObject*);

static bool EnsurePromiseRejected(
    JSContext* cx, Handle<WritableStreamDefaultWriter*> unwrappedWriter,
    GetField getField, SetField setField, Handle<Value> error) {
  cx->check(error);

  Rooted<PromiseObject*> unwrappedPromise(
      cx, UnwrapAndDowncastObject<PromiseObject>(
              cx, (unwrappedWriter->*getField)()));
  if (!unwrappedPromise) {
    return false;
  }

  // 4.6.{5,6} step 1: If writer.[[<field>]].[[PromiseState]] is "pending",
  //                   reject writer.[[<field>]] with error.
  if (unwrappedPromise->state() == JS::PromiseState::Pending) {
    if (!RejectUnwrappedPromiseWithError(cx, unwrappedPromise, error)) {
      return false;
    }
  } else {
    // 4.6.{5,6} step 2: Otherwise, set writer.[[<field>]] to a promise rejected
    //                   with error.
    Rooted<JSObject*> rejectedWithError(
        cx, PromiseObject::unforgeableReject(cx, error));
    if (!rejectedWithError) {
      return false;
    }

    {
      AutoRealm ar(cx, unwrappedWriter);
      if (!cx->compartment()->wrap(cx, &rejectedWithError)) {
        return false;
      }
      (unwrappedWriter->*setField)(rejectedWithError);
    }

    // Directly-unobservable rejected promises aren't collapsed like resolved
    // promises, and this promise is created in the current realm, so it's
    // always an actual Promise.
    unwrappedPromise = &rejectedWithError->as<PromiseObject>();
  }

  // 4.6.{5,6} step 3: Set writer.[[<field>]].[[PromiseIsHandled]] to true.
  js::SetSettledPromiseIsHandled(cx, unwrappedPromise);
  return true;
}

/**
 * Streams spec, 4.6.5.
 *  WritableStreamDefaultWriterEnsureClosedPromiseRejected( writer, error )
 */
[[nodiscard]] bool js::WritableStreamDefaultWriterEnsureClosedPromiseRejected(
    JSContext* cx, Handle<WritableStreamDefaultWriter*> unwrappedWriter,
    Handle<Value> error) {
  return EnsurePromiseRejected(
      cx, unwrappedWriter, &WritableStreamDefaultWriter::closedPromise,
      &WritableStreamDefaultWriter::setClosedPromise, error);
}

/**
 * Streams spec, 4.6.6.
 *  WritableStreamDefaultWriterEnsureReadyPromiseRejected( writer, error )
 */
[[nodiscard]] bool js::WritableStreamDefaultWriterEnsureReadyPromiseRejected(
    JSContext* cx, Handle<WritableStreamDefaultWriter*> unwrappedWriter,
    Handle<Value> error) {
  return EnsurePromiseRejected(
      cx, unwrappedWriter, &WritableStreamDefaultWriter::readyPromise,
      &WritableStreamDefaultWriter::setReadyPromise, error);
}

/**
 * Streams spec, 4.6.7.
 * WritableStreamDefaultWriterGetDesiredSize ( writer )
 */
bool js::WritableStreamDefaultWriterGetDesiredSize(
    JSContext* cx, Handle<WritableStreamDefaultWriter*> unwrappedWriter,
    MutableHandle<Value> size) {
  // Step 1: Let stream be writer.[[ownerWritableStream]].
  const WritableStream* unwrappedStream =
      UnwrapStreamFromWriter(cx, unwrappedWriter);
  if (!unwrappedStream) {
    return false;
  }

  // Step 2: Let state be stream.[[state]].
  // Step 3: If state is "errored" or "erroring", return null.
  if (unwrappedStream->errored() || unwrappedStream->erroring()) {
    size.setNull();
  }
  // Step 4: If state is "closed", return 0.
  else if (unwrappedStream->closed()) {
    size.setInt32(0);
  }
  // Step 5: Return
  //         ! WritableStreamDefaultControllerGetDesiredSize(
  //             stream.[[writableStreamController]]).
  else {
    size.setNumber(WritableStreamDefaultControllerGetDesiredSize(
        unwrappedStream->controller()));
  }

  return true;
}

/**
 * Streams spec, 4.6.8.
 * WritableStreamDefaultWriterRelease ( writer )
 */
bool js::WritableStreamDefaultWriterRelease(
    JSContext* cx, Handle<WritableStreamDefaultWriter*> unwrappedWriter) {
  // Step 1: Let stream be writer.[[ownerWritableStream]].
  // Step 2: Assert: stream is not undefined.
  MOZ_ASSERT(unwrappedWriter->hasStream());
  Rooted<WritableStream*> unwrappedStream(
      cx, UnwrapStreamFromWriter(cx, unwrappedWriter));
  if (!unwrappedStream) {
    return false;
  }

  // Step 3: Assert: stream.[[writer]] is writer.
#ifdef DEBUG
  {
    WritableStreamDefaultWriter* unwrappedStreamWriter =
        UnwrapWriterFromStream(cx, unwrappedStream);
    if (!unwrappedStreamWriter) {
      return false;
    }

    MOZ_ASSERT(unwrappedStreamWriter == unwrappedWriter);
  }
#endif

  // Step 4: Let releasedError be a new TypeError.
  Rooted<Value> releasedError(cx);
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_WRITABLESTREAM_CANT_RELEASE_ALREADY_CLOSED);
  if (!cx->isExceptionPending() || !GetAndClearException(cx, &releasedError)) {
    return false;
  }

  // Step 5: Perform
  //         ! WritableStreamDefaultWriterEnsureReadyPromiseRejected(
  //               writer, releasedError).
  if (!WritableStreamDefaultWriterEnsureReadyPromiseRejected(
          cx, unwrappedWriter, releasedError)) {
    return false;
  }

  // Step 6: Perform
  //         ! WritableStreamDefaultWriterEnsureClosedPromiseRejected(
  //               writer, releasedError).
  if (!WritableStreamDefaultWriterEnsureClosedPromiseRejected(
          cx, unwrappedWriter, releasedError)) {
    return false;
  }

  // Step 7: Set stream.[[writer]] to undefined.
  unwrappedStream->clearWriter();

  // Step 8: Set writer.[[ownerWritableStream]] to undefined.
  unwrappedWriter->clearStream();
  return true;
}

/**
 * Streams spec, 4.6.9.
 * WritableStreamDefaultWriterWrite ( writer, chunk )
 */
PromiseObject* js::WritableStreamDefaultWriterWrite(
    JSContext* cx, Handle<WritableStreamDefaultWriter*> unwrappedWriter,
    Handle<Value> chunk) {
  cx->check(chunk);

  // Step 1: Let stream be writer.[[ownerWritableStream]].
  // Step 2: Assert: stream is not undefined.
  MOZ_ASSERT(unwrappedWriter->hasStream());
  Rooted<WritableStream*> unwrappedStream(
      cx, UnwrapStreamFromWriter(cx, unwrappedWriter));
  if (!unwrappedStream) {
    return nullptr;
  }

  // Step 3: Let controller be stream.[[writableStreamController]].
  Rooted<WritableStreamDefaultController*> unwrappedController(
      cx, unwrappedStream->controller());

  // Step 4: Let chunkSize be
  //         ! WritableStreamDefaultControllerGetChunkSize(controller, chunk).
  Rooted<Value> chunkSize(cx);
  if (!WritableStreamDefaultControllerGetChunkSize(cx, unwrappedController,
                                                   chunk, &chunkSize)) {
    return nullptr;
  }
  cx->check(chunkSize);

  // Step 5: If stream is not equal to writer.[[ownerWritableStream]], return a
  //         promise rejected with a TypeError exception.
  // (This is just an obscure way of saying "If step 4 caused writer's lock on
  // stream to be released", or concretely, "If writer.[[ownerWritableStream]]
  // is [now, newly] undefined".)
  if (!unwrappedWriter->hasStream()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAM_RELEASED_DURING_WRITE);
    return PromiseRejectedWithPendingError(cx);
  }

  auto RejectWithStoredError =
      [](JSContext* cx,
         Handle<WritableStream*> unwrappedStream) -> PromiseObject* {
    Rooted<Value> storedError(cx, unwrappedStream->storedError());
    if (!cx->compartment()->wrap(cx, &storedError)) {
      return nullptr;
    }

    return PromiseObject::unforgeableReject(cx, storedError);
  };

  // Step 6: Let state be stream.[[state]].
  // Step 7: If state is "errored", return a promise rejected with
  //         stream.[[storedError]].
  if (unwrappedStream->errored()) {
    return RejectWithStoredError(cx, unwrappedStream);
  }

  // Step 8: If ! WritableStreamCloseQueuedOrInFlight(stream) is true or state
  //         is "closed", return a promise rejected with a TypeError exception
  //         indicating that the stream is closing or closed.
  if (WritableStreamCloseQueuedOrInFlight(unwrappedStream) ||
      unwrappedStream->closed()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAM_WRITE_CLOSING_OR_CLOSED);
    return PromiseRejectedWithPendingError(cx);
  }

  // Step 9: If state is "erroring", return a promise rejected with
  //         stream.[[storedError]].
  if (unwrappedStream->erroring()) {
    return RejectWithStoredError(cx, unwrappedStream);
  }

  // Step 10: Assert: state is "writable".
  MOZ_ASSERT(unwrappedStream->writable());

  // Step 11: Let promise be ! WritableStreamAddWriteRequest(stream).
  Rooted<PromiseObject*> promise(
      cx, WritableStreamAddWriteRequest(cx, unwrappedStream));
  if (!promise) {
    return nullptr;
  }

  // Step 12: Perform
  //          ! WritableStreamDefaultControllerWrite(controller, chunk,
  //                                                 chunkSize).
  if (!WritableStreamDefaultControllerWrite(cx, unwrappedController, chunk,
                                            chunkSize)) {
    return nullptr;
  }

  // Step 13: Return promise.
  return promise;
}
