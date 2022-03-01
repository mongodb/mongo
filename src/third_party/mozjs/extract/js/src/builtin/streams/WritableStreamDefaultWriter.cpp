/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Class WritableStreamDefaultWriter. */

#include "builtin/streams/WritableStreamDefaultWriter-inl.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jsapi.h"  // JS_ReportErrorASCII, JS_ReportErrorNumberASCII

#include "builtin/streams/ClassSpecMacro.h"  // JS_STREAMS_CLASS_SPEC
#include "builtin/streams/MiscellaneousOperations.h"  // js::ReturnPromiseRejectedWithPendingError
#include "builtin/streams/WritableStream.h"  // js::WritableStream
#include "builtin/streams/WritableStreamOperations.h"  // js::WritableStreamCloseQueuedOrInFlight
#include "builtin/streams/WritableStreamWriterOperations.h"  // js::WritableStreamDefaultWriter{Abort,GetDesiredSize,Release,Write}
#include "js/CallArgs.h"              // JS::CallArgs{,FromVp}
#include "js/Class.h"                 // js::ClassSpec, JS_NULL_CLASS_OPS
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"  // JS{Function,Property}Spec, JS_{FS,PS}_END, JS_{FN,PSG}
#include "js/RootingAPI.h"   // JS::Handle
#include "js/Value.h"        // JS::Value
#include "vm/Compartment.h"  // JS::Compartment
#include "vm/JSContext.h"    // JSContext
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined

#include "builtin/Promise-inl.h"  // js::SetSettledPromiseIsHandled
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::UnwrapAndTypeCheck{Argument,This}
#include "vm/JSObject-inl.h"      // js::NewObjectWithClassProto
#include "vm/NativeObject-inl.h"  // js::ThrowIfNotConstructing
#include "vm/Realm-inl.h"         // js::AutoRealm

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::Rooted;
using JS::Value;

using js::ClassSpec;
using js::GetErrorMessage;
using js::PromiseObject;
using js::ReturnPromiseRejectedWithPendingError;
using js::UnwrapAndTypeCheckArgument;
using js::UnwrapAndTypeCheckThis;
using js::WritableStream;
using js::WritableStreamCloseQueuedOrInFlight;
using js::WritableStreamDefaultWriter;
using js::WritableStreamDefaultWriterGetDesiredSize;
using js::WritableStreamDefaultWriterRelease;
using js::WritableStreamDefaultWriterWrite;

/*** 4.5. Class WritableStreamDefaultWriter *********************************/

/**
 * Stream spec, 4.5.3. new WritableStreamDefaultWriter(stream)
 * Steps 3-9.
 */
[[nodiscard]] WritableStreamDefaultWriter*
js::CreateWritableStreamDefaultWriter(JSContext* cx,
                                      Handle<WritableStream*> unwrappedStream,
                                      Handle<JSObject*> proto /* = nullptr */) {
  Rooted<WritableStreamDefaultWriter*> writer(
      cx, NewObjectWithClassProto<WritableStreamDefaultWriter>(cx, proto));
  if (!writer) {
    return nullptr;
  }

  // Step 3: Set this.[[ownerWritableStream]] to stream.
  {
    Rooted<JSObject*> stream(cx, unwrappedStream);
    if (!cx->compartment()->wrap(cx, &stream)) {
      return nullptr;
    }
    writer->setStream(stream);
  }

  // Step 4 is moved to the end.

  // Step 5: Let state be stream.[[state]].
  // Step 6: If state is "writable",
  if (unwrappedStream->writable()) {
    // Step 6.a: If ! WritableStreamCloseQueuedOrInFlight(stream) is false and
    //           stream.[[backpressure]] is true, set this.[[readyPromise]] to a
    //           new promise.
    PromiseObject* promise;
    if (!WritableStreamCloseQueuedOrInFlight(unwrappedStream) &&
        unwrappedStream->backpressure()) {
      promise = PromiseObject::createSkippingExecutor(cx);
    }
    // Step 6.b: Otherwise, set this.[[readyPromise]] to a promise resolved with
    //           undefined.
    else {
      promise = PromiseResolvedWithUndefined(cx);
    }
    if (!promise) {
      return nullptr;
    }
    writer->setReadyPromise(promise);

    // Step 6.c: Set this.[[closedPromise]] to a new promise.
    promise = PromiseObject::createSkippingExecutor(cx);
    if (!promise) {
      return nullptr;
    }

    writer->setClosedPromise(promise);
  }
  // Step 8: Otherwise, if state is "closed",
  else if (unwrappedStream->closed()) {
    // Step 8.a: Set this.[[readyPromise]] to a promise resolved with undefined.
    PromiseObject* readyPromise = PromiseResolvedWithUndefined(cx);
    if (!readyPromise) {
      return nullptr;
    }

    writer->setReadyPromise(readyPromise);

    // Step 8.b: Set this.[[closedPromise]] to a promise resolved with
    //           undefined.
    PromiseObject* closedPromise = PromiseResolvedWithUndefined(cx);
    if (!closedPromise) {
      return nullptr;
    }

    writer->setClosedPromise(closedPromise);
  } else {
    // Wrap stream.[[StoredError]] just once for either step 7 or step 9.
    Rooted<Value> storedError(cx, unwrappedStream->storedError());
    if (!cx->compartment()->wrap(cx, &storedError)) {
      return nullptr;
    }

    // Step 7: Otherwise, if state is "erroring",
    if (unwrappedStream->erroring()) {
      // Step 7.a: Set this.[[readyPromise]] to a promise rejected with
      //           stream.[[storedError]].
      Rooted<JSObject*> promise(
          cx, PromiseObject::unforgeableReject(cx, storedError));
      if (!promise) {
        return nullptr;
      }

      writer->setReadyPromise(promise);

      // Step 7.b: Set this.[[readyPromise]].[[PromiseIsHandled]] to true.
      js::SetSettledPromiseIsHandled(cx, promise.as<PromiseObject>());

      // Step 7.c: Set this.[[closedPromise]] to a new promise.
      JSObject* closedPromise = PromiseObject::createSkippingExecutor(cx);
      if (!closedPromise) {
        return nullptr;
      }

      writer->setClosedPromise(closedPromise);
    }
    // Step 9: Otherwise,
    else {
      // Step 9.a: Assert: state is "errored".
      MOZ_ASSERT(unwrappedStream->errored());

      Rooted<JSObject*> promise(cx);

      // Step 9.b: Let storedError be stream.[[storedError]].
      // Step 9.c: Set this.[[readyPromise]] to a promise rejected with
      //           storedError.
      promise = PromiseObject::unforgeableReject(cx, storedError);
      if (!promise) {
        return nullptr;
      }

      writer->setReadyPromise(promise);

      // Step 9.d: Set this.[[readyPromise]].[[PromiseIsHandled]] to true.
      js::SetSettledPromiseIsHandled(cx, promise.as<PromiseObject>());

      // Step 9.e: Set this.[[closedPromise]] to a promise rejected with
      //           storedError.
      promise = PromiseObject::unforgeableReject(cx, storedError);
      if (!promise) {
        return nullptr;
      }

      writer->setClosedPromise(promise);

      // Step 9.f: Set this.[[closedPromise]].[[PromiseIsHandled]] to true.
      js::SetSettledPromiseIsHandled(cx, promise.as<PromiseObject>());
    }
  }

  // Step 4 (reordered): Set stream.[[writer]] to this.
  // Doing this last prevents a partially-initialized writer from being attached
  // to the stream (and possibly left there on OOM).
  {
    AutoRealm ar(cx, unwrappedStream);
    Rooted<JSObject*> wrappedWriter(cx, writer);
    if (!cx->compartment()->wrap(cx, &wrappedWriter)) {
      return nullptr;
    }
    unwrappedStream->setWriter(wrappedWriter);
  }

  return writer;
}

/**
 * Streams spec, 4.5.3.
 * new WritableStreamDefaultWriter(stream)
 */
bool WritableStreamDefaultWriter::constructor(JSContext* cx, unsigned argc,
                                              Value* vp) {
  MOZ_ASSERT(cx->realm()->creationOptions().getWritableStreamsEnabled(),
             "WritableStream should be enabled in this realm if we reach here");

  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "WritableStreamDefaultWriter")) {
    return false;
  }

  // Step 1: If ! IsWritableStream(stream) is false, throw a TypeError
  //         exception.
  Rooted<WritableStream*> unwrappedStream(
      cx, UnwrapAndTypeCheckArgument<WritableStream>(
              cx, args, "WritableStreamDefaultWriter constructor", 0));
  if (!unwrappedStream) {
    return false;
  }

  // Step 2: If ! IsWritableStreamLocked(stream) is true, throw a TypeError
  //         exception.
  if (unwrappedStream->isLocked()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAM_ALREADY_LOCKED);
    return false;
  }

  // Implicit in the spec: Find the prototype object to use.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Null, &proto)) {
    return false;
  }

  // Steps 3-9.
  Rooted<WritableStreamDefaultWriter*> writer(
      cx, CreateWritableStreamDefaultWriter(cx, unwrappedStream, proto));
  if (!writer) {
    return false;
  }

  args.rval().setObject(*writer);
  return true;
}

/**
 * Streams spec, 4.5.4.1. get closed
 */
[[nodiscard]] static bool WritableStreamDefaultWriter_closed(JSContext* cx,
                                                             unsigned argc,
                                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsWritableStreamDefaultWriter(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
      cx, UnwrapAndTypeCheckThis<WritableStreamDefaultWriter>(cx, args,
                                                              "get closed"));
  if (!unwrappedWriter) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 2: Return this.[[closedPromise]].
  Rooted<JSObject*> closedPromise(cx, unwrappedWriter->closedPromise());
  if (!cx->compartment()->wrap(cx, &closedPromise)) {
    return false;
  }

  args.rval().setObject(*closedPromise);
  return true;
}

/**
 * Streams spec, 4.5.4.2. get desiredSize
 */
[[nodiscard]] static bool WritableStreamDefaultWriter_desiredSize(JSContext* cx,
                                                                  unsigned argc,
                                                                  Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsWritableStreamDefaultWriter(this) is false, throw a
  //         TypeError exception.
  Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
      cx, UnwrapAndTypeCheckThis<WritableStreamDefaultWriter>(
              cx, args, "get desiredSize"));
  if (!unwrappedWriter) {
    return false;
  }

  // Step 2: If this.[[ownerWritableStream]] is undefined, throw a TypeError
  //         exception.
  if (!unwrappedWriter->hasStream()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAMWRITER_NOT_OWNED,
                              "get desiredSize");
    return false;
  }

  // Step 3: Return ! WritableStreamDefaultWriterGetDesiredSize(this).
  if (!WritableStreamDefaultWriterGetDesiredSize(cx, unwrappedWriter,
                                                 args.rval())) {
    return false;
  }

  MOZ_ASSERT(args.rval().isNull() || args.rval().isNumber(),
             "expected a type that'll never require wrapping");
  return true;
}

/**
 * Streams spec, 4.5.4.3. get ready
 */
[[nodiscard]] static bool WritableStreamDefaultWriter_ready(JSContext* cx,
                                                            unsigned argc,
                                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsWritableStreamDefaultWriter(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
      cx, UnwrapAndTypeCheckThis<WritableStreamDefaultWriter>(cx, args,
                                                              "get ready"));
  if (!unwrappedWriter) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 2: Return this.[[readyPromise]].
  Rooted<JSObject*> readyPromise(cx, unwrappedWriter->readyPromise());
  if (!cx->compartment()->wrap(cx, &readyPromise)) {
    return false;
  }

  args.rval().setObject(*readyPromise);
  return true;
}

/**
 * Streams spec, 4.5.4.4. abort(reason)
 */
[[nodiscard]] static bool WritableStreamDefaultWriter_abort(JSContext* cx,
                                                            unsigned argc,
                                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsWritableStreamDefaultWriter(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
      cx,
      UnwrapAndTypeCheckThis<WritableStreamDefaultWriter>(cx, args, "abort"));
  if (!unwrappedWriter) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 2: If this.[[ownerWritableStream]] is undefined, return a promise
  //         rejected with a TypeError exception.
  if (!unwrappedWriter->hasStream()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAMWRITER_NOT_OWNED, "abort");
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 3: Return ! WritableStreamDefaultWriterAbort(this, reason).
  JSObject* promise =
      WritableStreamDefaultWriterAbort(cx, unwrappedWriter, args.get(0));
  if (!promise) {
    return false;
  }
  cx->check(promise);

  args.rval().setObject(*promise);
  return true;
}

/**
 * Streams spec, 4.5.4.5. close()
 */
[[nodiscard]] static bool WritableStreamDefaultWriter_close(JSContext* cx,
                                                            unsigned argc,
                                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsWritableStreamDefaultWriter(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
      cx,
      UnwrapAndTypeCheckThis<WritableStreamDefaultWriter>(cx, args, "close"));
  if (!unwrappedWriter) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 2: Let stream be this.[[ownerWritableStream]].
  // Step 3: If stream is undefined, return a promise rejected with a TypeError
  //         exception.
  if (!unwrappedWriter->hasStream()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAMWRITER_NOT_OWNED, "write");
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  WritableStream* unwrappedStream = UnwrapStreamFromWriter(cx, unwrappedWriter);
  if (!unwrappedStream) {
    return false;
  }

  // Step 4: If ! WritableStreamCloseQueuedOrInFlight(stream) is true, return a
  //         promise rejected with a TypeError exception.
  if (WritableStreamCloseQueuedOrInFlight(unwrappedStream)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAM_CLOSE_CLOSING_OR_CLOSED);
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 5: Return ! WritableStreamDefaultWriterClose(this).
  JSObject* promise = WritableStreamDefaultWriterClose(cx, unwrappedWriter);
  if (!promise) {
    return false;
  }
  cx->check(promise);

  args.rval().setObject(*promise);
  return true;
}

/**
 * Streams spec, 4.5.4.6. releaseLock()
 */
[[nodiscard]] static bool WritableStreamDefaultWriter_releaseLock(JSContext* cx,
                                                                  unsigned argc,
                                                                  Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsWritableStreamDefaultWriter(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
      cx,
      UnwrapAndTypeCheckThis<WritableStreamDefaultWriter>(cx, args, "close"));
  if (!unwrappedWriter) {
    return false;
  }

  // Step 2: Let stream be this.[[ownerWritableStream]].
  // Step 3: If stream is undefined, return.
  if (!unwrappedWriter->hasStream()) {
    args.rval().setUndefined();
    return true;
  }

  // Step 4: Assert: stream.[[writer]] is not undefined.
#ifdef DEBUG
  {
    WritableStream* unwrappedStream =
        UnwrapStreamFromWriter(cx, unwrappedWriter);
    if (!unwrappedStream) {
      return false;
    }
    MOZ_ASSERT(unwrappedStream->hasWriter());
  }
#endif

  // Step 5: Perform ! WritableStreamDefaultWriterRelease(this).
  if (!WritableStreamDefaultWriterRelease(cx, unwrappedWriter)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 4.5.4.7. write(chunk)
 */
[[nodiscard]] static bool WritableStreamDefaultWriter_write(JSContext* cx,
                                                            unsigned argc,
                                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsWritableStreamDefaultWriter(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<WritableStreamDefaultWriter*> unwrappedWriter(
      cx,
      UnwrapAndTypeCheckThis<WritableStreamDefaultWriter>(cx, args, "write"));
  if (!unwrappedWriter) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 2: If this.[[ownerWritableStream]] is undefined, return a promise
  //         rejected with a TypeError exception.
  if (!unwrappedWriter->hasStream()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAMWRITER_NOT_OWNED, "write");
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 3: Return this.[[readyPromise]].
  PromiseObject* promise =
      WritableStreamDefaultWriterWrite(cx, unwrappedWriter, args.get(0));
  if (!promise) {
    return false;
  }
  cx->check(promise);

  args.rval().setObject(*promise);
  return true;
}

static const JSPropertySpec WritableStreamDefaultWriter_properties[] = {
    JS_PSG("closed", WritableStreamDefaultWriter_closed, 0),
    JS_PSG("desiredSize", WritableStreamDefaultWriter_desiredSize, 0),
    JS_PSG("ready", WritableStreamDefaultWriter_ready, 0), JS_PS_END};

static const JSFunctionSpec WritableStreamDefaultWriter_methods[] = {
    JS_FN("abort", WritableStreamDefaultWriter_abort, 1, 0),
    JS_FN("close", WritableStreamDefaultWriter_close, 0, 0),
    JS_FN("releaseLock", WritableStreamDefaultWriter_releaseLock, 0, 0),
    JS_FN("write", WritableStreamDefaultWriter_write, 1, 0), JS_FS_END};

JS_STREAMS_CLASS_SPEC(WritableStreamDefaultWriter, 1, SlotCount,
                      ClassSpec::DontDefineConstructor, 0, JS_NULL_CLASS_OPS);
