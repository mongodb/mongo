/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ReadableStream.prototype.pipeTo state. */

#include "builtin/streams/PipeToState-inl.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Maybe.h"  // mozilla::Maybe, mozilla::Nothing, mozilla::Some

#include "jsapi.h"  // JS_ReportErrorNumberASCII

#include "builtin/Promise.h"  // js::RejectPromiseWithPendingError
#include "builtin/streams/ReadableStream.h"        // js::ReadableStream
#include "builtin/streams/ReadableStreamReader.h"  // js::CreateReadableStreamDefaultReader, js::ForAuthorCodeBool, js::ReadableStreamDefaultReader, js::ReadableStreamReaderGenericRelease
#include "builtin/streams/WritableStream.h"        // js::WritableStream
#include "builtin/streams/WritableStreamDefaultWriter.h"  // js::CreateWritableStreamDefaultWriter, js::WritableStreamDefaultWriter
#include "builtin/streams/WritableStreamOperations.h"  // js::WritableStreamCloseQueuedOrInFlight
#include "builtin/streams/WritableStreamWriterOperations.h"  // js::WritableStreamDefaultWriter{GetDesiredSize,Release,Write}
#include "js/CallArgs.h"              // JS::CallArgsFromVp, JS::CallArgs
#include "js/Class.h"                 // JSClass, JSCLASS_HAS_RESERVED_SLOTS
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Promise.h"               // JS::AddPromiseReactions
#include "js/RootingAPI.h"            // JS::Handle, JS::Rooted
#include "js/Value.h"  // JS::{,Int32,Magic,Object}Value, JS::UndefinedHandleValue
#include "vm/JSContext.h"      // JSContext
#include "vm/PromiseObject.h"  // js::PromiseObject
#include "vm/Runtime.h"        // JSRuntime

#include "builtin/HandlerFunction-inl.h"  // js::ExtraValueFromHandler, js::NewHandler{,WithExtraValue}, js::TargetFromHandler
#include "builtin/streams/ReadableStreamReader-inl.h"  // js::UnwrapReaderFromStream, js::UnwrapStreamFromReader
#include "builtin/streams/WritableStream-inl.h"  // js::UnwrapWriterFromStream
#include "builtin/streams/WritableStreamDefaultWriter-inl.h"  // js::UnwrapStreamFromWriter
#include "vm/JSContext-inl.h"  // JSContext::check
#include "vm/JSObject-inl.h"   // js::NewBuiltinClassInstance
#include "vm/Realm-inl.h"      // js::AutoRealm

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::Int32Value;
using JS::MagicValue;
using JS::ObjectValue;
using JS::Rooted;
using JS::UndefinedHandleValue;
using JS::Value;

using js::ExtraValueFromHandler;
using js::GetErrorMessage;
using js::NewHandler;
using js::NewHandlerWithExtraValue;
using js::PipeToState;
using js::PromiseObject;
using js::ReadableStream;
using js::ReadableStreamDefaultReader;
using js::ReadableStreamReaderGenericRelease;
using js::TargetFromHandler;
using js::UnwrapReaderFromStream;
using js::UnwrapStreamFromWriter;
using js::UnwrapWriterFromStream;
using js::WritableStream;
using js::WritableStreamDefaultWriter;
using js::WritableStreamDefaultWriterRelease;
using js::WritableStreamDefaultWriterWrite;

static ReadableStream* GetUnwrappedSource(JSContext* cx,
                                          Handle<PipeToState*> state) {
  cx->check(state);

  Rooted<ReadableStreamDefaultReader*> reader(cx, state->reader());
  cx->check(reader);

  return UnwrapStreamFromReader(cx, reader);
}

static WritableStream* GetUnwrappedDest(JSContext* cx,
                                        Handle<PipeToState*> state) {
  cx->check(state);

  Rooted<WritableStreamDefaultWriter*> writer(cx, state->writer());
  cx->check(writer);

  return UnwrapStreamFromWriter(cx, writer);
}

static bool WritableAndNotClosing(const WritableStream* unwrappedDest) {
  return unwrappedDest->writable() &&
         WritableStreamCloseQueuedOrInFlight(unwrappedDest);
}

[[nodiscard]] static bool Finalize(JSContext* cx, Handle<PipeToState*> state,
                                   Handle<Maybe<Value>> error) {
  cx->check(state);
  cx->check(error);

  // Step 1: Perform ! WritableStreamDefaultWriterRelease(writer).
  Rooted<WritableStreamDefaultWriter*> writer(cx, state->writer());
  cx->check(writer);
  if (!WritableStreamDefaultWriterRelease(cx, writer)) {
    return false;
  }

  // Step 2: Perform ! ReadableStreamReaderGenericRelease(reader).
  Rooted<ReadableStreamDefaultReader*> reader(cx, state->reader());
  cx->check(reader);
  if (!ReadableStreamReaderGenericRelease(cx, reader)) {
    return false;
  }

  // Step 3: If signal is not undefined, remove abortAlgorithm from signal.
  // XXX

  Rooted<PromiseObject*> promise(cx, state->promise());
  cx->check(promise);

  // Step 4: If error was given, reject promise with error.
  if (error.isSome()) {
    Rooted<Value> errorVal(cx, *error.get());
    return PromiseObject::reject(cx, promise, errorVal);
  }

  // Step 5: Otherwise, resolve promise with undefined.
  return PromiseObject::resolve(cx, promise, UndefinedHandleValue);
}

[[nodiscard]] static bool Finalize(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
  cx->check(state);

  Rooted<Maybe<Value>> optionalError(cx, Nothing());
  if (Value maybeError = ExtraValueFromHandler(args);
      !maybeError.isMagic(JS_READABLESTREAM_PIPETO_FINALIZE_WITHOUT_ERROR)) {
    optionalError = Some(maybeError);
  }
  cx->check(optionalError);

  if (!Finalize(cx, state, optionalError)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

// Shutdown with an action, steps d-f:
//   d. Let p be the result of performing action.
//   e. Upon fulfillment of p, finalize, passing along originalError if it was
//      given.
//   f. Upon rejection of p with reason newError, finalize with newError.
[[nodiscard]] static bool ActAndFinalize(JSContext* cx,
                                         Handle<PipeToState*> state,
                                         Handle<Maybe<Value>> error) {
  // Step d: Let p be the result of performing action.
  Rooted<JSObject*> p(cx);
  switch (state->shutdownAction()) {
    // This corresponds to the action performed by |abortAlgorithm| in
    // ReadableStreamPipeTo step 14.1.5.
    case PipeToState::ShutdownAction::AbortAlgorithm: {
      MOZ_ASSERT(error.get().isSome());

      // From ReadableStreamPipeTo:
      // Step 14.1.2: Let actions be an empty ordered set.
      // Step 14.1.3: If preventAbort is false, append the following action to
      //              actions:
      // Step 14.1.3.1: If dest.[[state]] is "writable", return
      //                ! WritableStreamAbort(dest, error).
      // Step 14.1.3.2: Otherwise, return a promise resolved with undefined.
      // Step 14.1.4: If preventCancel is false, append the following action
      //               action to actions:
      // Step 14.1.4.1.: If source.[[state]] is "readable", return
      //                 ! ReadableStreamCancel(source, error).
      // Step 14.1.4.2: Otherwise, return a promise resolved with undefined.
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_READABLESTREAM_METHOD_NOT_IMPLEMENTED,
                                "any required actions during abortAlgorithm");
      return false;
    }

    // This corresponds to the action in "shutdown with an action of
    // ! WritableStreamAbort(dest, source.[[storedError]]) and with
    // source.[[storedError]]."
    case PipeToState::ShutdownAction::AbortDestStream: {
      MOZ_ASSERT(error.get().isSome());

      Rooted<WritableStream*> unwrappedDest(cx, GetUnwrappedDest(cx, state));
      if (!unwrappedDest) {
        return false;
      }

      Rooted<Value> sourceStoredError(cx, *error.get());
      cx->check(sourceStoredError);

      p = WritableStreamAbort(cx, unwrappedDest, sourceStoredError);
      break;
    }

    // This corresponds to two actions:
    //
    // * The action in "shutdown with an action of
    //   ! ReadableStreamCancel(source, dest.[[storedError]]) and with
    //   dest.[[storedError]]" as used in "Errors must be propagated backward:
    //   if dest.[[state]] is or becomes 'errored'".
    // * The action in "shutdown with an action of
    //   ! ReadableStreamCancel(source, destClosed) and with destClosed" as used
    //   in "Closing must be propagated backward: if
    //   ! WritableStreamCloseQueuedOrInFlight(dest) is true or dest.[[state]]
    //   is 'closed'".
    //
    // The different reason-values are passed as |error|.
    case PipeToState::ShutdownAction::CancelSource: {
      MOZ_ASSERT(error.get().isSome());

      Rooted<ReadableStream*> unwrappedSource(cx,
                                              GetUnwrappedSource(cx, state));
      if (!unwrappedSource) {
        return false;
      }

      Rooted<Value> reason(cx, *error.get());
      cx->check(reason);

      p = ReadableStreamCancel(cx, unwrappedSource, reason);
      break;
    }

    // This corresponds to the action in "shutdown with an action of
    // ! WritableStreamDefaultWriterCloseWithErrorPropagation(writer)" as done
    // in "Closing must be propagated forward: if source.[[state]] is or becomes
    // 'closed'".
    case PipeToState::ShutdownAction::CloseWriterWithErrorPropagation: {
      MOZ_ASSERT(error.get().isNothing());

      Rooted<WritableStreamDefaultWriter*> writer(cx, state->writer());
      cx->check(writer);  // just for good measure: we don't depend on this

      p = WritableStreamDefaultWriterCloseWithErrorPropagation(cx, writer);
      break;
    }
  }
  if (!p) {
    return false;
  }

  // Step e: Upon fulfillment of p, finalize, passing along originalError if it
  //         was given.
  Rooted<JSFunction*> onFulfilled(cx);
  {
    Rooted<Value> optionalError(
        cx, error.isSome()
                ? *error.get()
                : MagicValue(JS_READABLESTREAM_PIPETO_FINALIZE_WITHOUT_ERROR));
    onFulfilled = NewHandlerWithExtraValue(cx, Finalize, state, optionalError);
    if (!onFulfilled) {
      return false;
    }
  }

  // Step f: Upon rejection of p with reason newError, finalize with newError.
  auto OnRejected = [](JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);

    Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
    cx->check(state);

    Rooted<Maybe<Value>> newError(cx, Some(args[0]));
    cx->check(newError);
    if (!Finalize(cx, state, newError)) {
      return false;
    }

    args.rval().setUndefined();
    return true;
  };

  Rooted<JSFunction*> onRejected(cx, NewHandler(cx, OnRejected, state));
  if (!onRejected) {
    return false;
  }

  return JS::AddPromiseReactions(cx, p, onFulfilled, onRejected);
}

[[nodiscard]] static bool ActAndFinalize(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
  cx->check(state);

  Rooted<Maybe<Value>> optionalError(cx, Nothing());
  if (Value maybeError = ExtraValueFromHandler(args);
      !maybeError.isMagic(JS_READABLESTREAM_PIPETO_FINALIZE_WITHOUT_ERROR)) {
    optionalError = Some(maybeError);
  }
  cx->check(optionalError);

  if (!ActAndFinalize(cx, state, optionalError)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

// Shutdown with an action: if any of the above requirements ask to shutdown
// with an action action, optionally with an error originalError, then:
[[nodiscard]] static bool ShutdownWithAction(
    JSContext* cx, Handle<PipeToState*> state,
    PipeToState::ShutdownAction action, Handle<Maybe<Value>> originalError) {
  cx->check(state);
  cx->check(originalError);

  // Step a: If shuttingDown is true, abort these substeps.
  if (state->shuttingDown()) {
    return true;
  }

  // Step b: Set shuttingDown to true.
  state->setShuttingDown();

  // Save the action away for later, potentially asynchronous, use.
  state->setShutdownAction(action);

  // Step c: If dest.[[state]] is "writable" and
  //         ! WritableStreamCloseQueuedOrInFlight(dest) is false,
  WritableStream* unwrappedDest = GetUnwrappedDest(cx, state);
  if (!unwrappedDest) {
    return false;
  }
  if (WritableAndNotClosing(unwrappedDest)) {
    // Step c.i:  If any chunks have been read but not yet written, write them
    //            to dest.
    //
    // Any chunk that has been read, will have been processed and a pending
    // write for it created by this point.  (A pending read has not been "read".
    // And any pending read, will not be processed into a pending write because
    // of the |state->setShuttingDown()| above in concert with the early exit
    // in this case in |ReadFulfilled|.)

    // Step c.ii: Wait until every chunk that has been read has been written
    //            (i.e. the corresponding promises have settled).
    if (PromiseObject* p = state->lastWriteRequest()) {
      Rooted<PromiseObject*> lastWriteRequest(cx, p);

      Rooted<Value> extra(
          cx,
          originalError.isSome()
              ? *originalError.get()
              : MagicValue(JS_READABLESTREAM_PIPETO_FINALIZE_WITHOUT_ERROR));

      Rooted<JSFunction*> actAndfinalize(
          cx, NewHandlerWithExtraValue(cx, ActAndFinalize, state, extra));
      if (!actAndfinalize) {
        return false;
      }

      return JS::AddPromiseReactions(cx, lastWriteRequest, actAndfinalize,
                                     actAndfinalize);
    }

    // If no last write request was ever created, we can fall through and
    // synchronously perform the remaining steps.
  }

  // Step d: Let p be the result of performing action.
  // Step e: Upon fulfillment of p, finalize, passing along originalError if it
  //         was given.
  // Step f: Upon rejection of p with reason newError, finalize with newError.
  return ActAndFinalize(cx, state, originalError);
}

// Shutdown: if any of the above requirements or steps ask to shutdown,
// optionally with an error error, then:
[[nodiscard]] static bool Shutdown(JSContext* cx, Handle<PipeToState*> state,
                                   Handle<Maybe<Value>> error) {
  cx->check(state);
  cx->check(error);

  // Step a: If shuttingDown is true, abort these substeps.
  if (state->shuttingDown()) {
    return true;
  }

  // Step b: Set shuttingDown to true.
  state->setShuttingDown();

  // Step c: If dest.[[state]] is "writable" and
  //         ! WritableStreamCloseQueuedOrInFlight(dest) is false,
  WritableStream* unwrappedDest = GetUnwrappedDest(cx, state);
  if (!unwrappedDest) {
    return false;
  }
  if (WritableAndNotClosing(unwrappedDest)) {
    // Step 1: If any chunks have been read but not yet written, write them to
    //         dest.
    //
    // Any chunk that has been read, will have been processed and a pending
    // write for it created by this point.  (A pending read has not been "read".
    // And any pending read, will not be processed into a pending write because
    // of the |state->setShuttingDown()| above in concert with the early exit
    // in this case in |ReadFulfilled|.)

    // Step 2: Wait until every chunk that has been read has been written
    //         (i.e. the corresponding promises have settled).
    if (PromiseObject* p = state->lastWriteRequest()) {
      Rooted<PromiseObject*> lastWriteRequest(cx, p);

      Rooted<Value> extra(
          cx,
          error.isSome()
              ? *error.get()
              : MagicValue(JS_READABLESTREAM_PIPETO_FINALIZE_WITHOUT_ERROR));

      Rooted<JSFunction*> finalize(
          cx, NewHandlerWithExtraValue(cx, Finalize, state, extra));
      if (!finalize) {
        return false;
      }

      return JS::AddPromiseReactions(cx, lastWriteRequest, finalize, finalize);
    }

    // If no last write request was ever created, we can fall through and
    // synchronously perform the remaining steps.
  }

  // Step d: Finalize, passing along error if it was given.
  return Finalize(cx, state, error);
}

/**
 * Streams spec, 3.4.11. ReadableStreamPipeTo step 14:
 * "a. Errors must be propagated forward: if source.[[state]] is or becomes
 * 'errored', then..."
 */
[[nodiscard]] static bool OnSourceErrored(
    JSContext* cx, Handle<PipeToState*> state,
    Handle<ReadableStream*> unwrappedSource) {
  cx->check(state);

  Rooted<Maybe<Value>> storedError(cx, Some(unwrappedSource->storedError()));
  if (!cx->compartment()->wrap(cx, &storedError)) {
    return false;
  }

  // If |source| becomes errored not during a pending read, it's clear we must
  // react immediately.
  //
  // But what if |source| becomes errored *during* a pending read?  Should this
  // first error, or the pending-read second error, predominate?  Two semantics
  // are possible when |source|/|dest| become closed or errored while there's a
  // pending read:
  //
  //   1. Wait until the read fulfills or rejects, then respond to the
  //      closure/error without regard to the read having fulfilled or rejected.
  //      (This will simply not react to the read being rejected, or it will
  //      queue up the read chunk to be written during shutdown.)
  //   2. React to the closure/error immediately per "Error and close states
  //      must be propagated".  Then when the read fulfills or rejects later, do
  //      nothing.
  //
  // The spec doesn't clearly require either semantics.  It requires that
  // *already-read* chunks be written (at least if |dest| didn't become errored
  // or closed such that no further writes can occur).  But it's silent as to
  // not-fully-read chunks.  (These semantic differences may only be observable
  // with very carefully constructed readable/writable streams.)
  //
  // It seems best, generally, to react to the temporally-earliest problem that
  // arises, so we implement option #2.  (Blink, in contrast, currently
  // implements option #1.)
  //
  // All specified reactions to a closure/error invoke either the shutdown, or
  // shutdown with an action, algorithms.  Those algorithms each abort if either
  // shutdown algorithm has already been invoked.  So we don't need to do
  // anything special here to deal with a pending read.

  // ii. Otherwise (if preventAbort is true), shutdown with
  //     source.[[storedError]].
  if (state->preventAbort()) {
    if (!Shutdown(cx, state, storedError)) {
      return false;
    }
  }
  // i. (If preventAbort is false,) shutdown with an action of
  //    ! WritableStreamAbort(dest, source.[[storedError]]) and with
  //    source.[[storedError]].
  else {
    if (!ShutdownWithAction(cx, state,
                            PipeToState::ShutdownAction::AbortDestStream,
                            storedError)) {
      return false;
    }
  }

  return true;
}

/**
 * Streams spec, 3.4.11. ReadableStreamPipeTo step 14:
 * "b. Errors must be propagated backward: if dest.[[state]] is or becomes
 * 'errored', then..."
 */
[[nodiscard]] static bool OnDestErrored(JSContext* cx,
                                        Handle<PipeToState*> state,
                                        Handle<WritableStream*> unwrappedDest) {
  cx->check(state);

  Rooted<Maybe<Value>> storedError(cx, Some(unwrappedDest->storedError()));
  if (!cx->compartment()->wrap(cx, &storedError)) {
    return false;
  }

  // As in |OnSourceErrored| above, we must deal with the case of |dest|
  // erroring before a pending read has fulfilled or rejected.
  //
  // As noted there, we handle the *first* error that arises.  And because this
  // algorithm immediately invokes a shutdown algorithm, and shutting down will
  // inhibit future shutdown attempts, we don't need to do anything special
  // *here*, either.

  // ii. Otherwise (if preventCancel is true), shutdown with
  //     dest.[[storedError]].
  if (state->preventCancel()) {
    if (!Shutdown(cx, state, storedError)) {
      return false;
    }
  }
  // i. If preventCancel is false, shutdown with an action of
  //    ! ReadableStreamCancel(source, dest.[[storedError]]) and with
  //    dest.[[storedError]].
  else {
    if (!ShutdownWithAction(cx, state,
                            PipeToState::ShutdownAction::CancelSource,
                            storedError)) {
      return false;
    }
  }

  return true;
}

/**
 * Streams spec, 3.4.11. ReadableStreamPipeTo step 14:
 * "c. Closing must be propagated forward: if source.[[state]] is or becomes
 * 'closed', then..."
 */
[[nodiscard]] static bool OnSourceClosed(JSContext* cx,
                                         Handle<PipeToState*> state) {
  cx->check(state);

  Rooted<Maybe<Value>> noError(cx, Nothing());

  // It shouldn't be possible for |source| to become closed *during* a pending
  // read: such spontaneous closure *should* be enqueued for processing *after*
  // the settling of the pending read.  (Note also that a [[closedPromise]]
  // resolution in |ReadableStreamClose| occurs only after all pending reads are
  // resolved.)  So we need not do anything to handle a source closure while a
  // read is in progress.

  // ii. Otherwise (if preventClose is true), shutdown.
  if (state->preventClose()) {
    if (!Shutdown(cx, state, noError)) {
      return false;
    }
  }
  // i. If preventClose is false, shutdown with an action of
  //    ! WritableStreamDefaultWriterCloseWithErrorPropagation(writer).
  else {
    if (!ShutdownWithAction(
            cx, state,
            PipeToState::ShutdownAction::CloseWriterWithErrorPropagation,
            noError)) {
      return false;
    }
  }

  return true;
}

/**
 * Streams spec, 3.4.11. ReadableStreamPipeTo step 14:
 * "d. Closing must be propagated backward: if
 * ! WritableStreamCloseQueuedOrInFlight(dest) is true or dest.[[state]] is
 * 'closed', then..."
 */
[[nodiscard]] static bool OnDestClosed(JSContext* cx,
                                       Handle<PipeToState*> state) {
  cx->check(state);

  // i. Assert: no chunks have been read or written.
  //
  // This assertion holds when this function is called by
  // |SourceOrDestErroredOrClosed|, before any async internal piping operations
  // happen.
  //
  // But it wouldn't hold for streams that can spontaneously close of their own
  // accord, like say a hypothetical DOM TCP socket.  I think?
  //
  // XXX Add this assertion if it really does hold (and is easily performed),
  //     else report a spec bug.

  // ii. Let destClosed be a new TypeError.
  Rooted<Maybe<Value>> destClosed(cx, Nothing());
  {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WRITABLESTREAM_WRITE_CLOSING_OR_CLOSED);
    Rooted<Value> v(cx);
    if (!cx->isExceptionPending() || !GetAndClearException(cx, &v)) {
      return false;
    }

    destClosed = Some(v.get());
  }

  // As in all the |On{Source,Dest}{Closed,Errored}| above, we must consider the
  // possibility that we're in the middle of a pending read.  |state->writer()|
  // has a lock on |dest| here, so we know only we can be writing chunks to
  // |dest| -- but there's no reason why |dest| couldn't become closed of its
  // own accord here (for example, a socket might become closed on its own), and
  // such closure may or may not be equivalent to error.
  //
  // For the reasons noted in |OnSourceErrored|, we process closure in the
  // middle of a pending read immediately, without delaying for that read to
  // fulfill or reject.  We trigger a shutdown operation below, which will
  // ensure shutdown only occurs once, so we need not do anything special here.

  // iv. Otherwise (if preventCancel is true), shutdown with destClosed.
  if (state->preventCancel()) {
    if (!Shutdown(cx, state, destClosed)) {
      return false;
    }
  }
  // iii. If preventCancel is false, shutdown with an action of
  //      ! ReadableStreamCancel(source, destClosed) and with destClosed.
  else {
    if (!ShutdownWithAction(
            cx, state, PipeToState::ShutdownAction::CancelSource, destClosed)) {
      return false;
    }
  }

  return true;
}

/**
 * Streams spec, 3.4.11. ReadableStreamPipeTo step 14:
 * "Error and close states must be propagated: the following conditions must be
 * applied in order.", as applied at the very start of piping, before any reads
 * from source or writes to dest have been triggered.
 */
[[nodiscard]] static bool SourceOrDestErroredOrClosed(
    JSContext* cx, Handle<PipeToState*> state,
    Handle<ReadableStream*> unwrappedSource,
    Handle<WritableStream*> unwrappedDest, bool* erroredOrClosed) {
  cx->check(state);

  *erroredOrClosed = true;

  // a. Errors must be propagated forward: if source.[[state]] is or becomes
  //    "errored", then
  if (unwrappedSource->errored()) {
    return OnSourceErrored(cx, state, unwrappedSource);
  }

  // b. Errors must be propagated backward: if dest.[[state]] is or becomes
  //    "errored", then
  if (unwrappedDest->errored()) {
    return OnDestErrored(cx, state, unwrappedDest);
  }

  // c. Closing must be propagated forward: if source.[[state]] is or becomes
  //    "closed", then
  if (unwrappedSource->closed()) {
    return OnSourceClosed(cx, state);
  }

  // d. Closing must be propagated backward: if
  //    ! WritableStreamCloseQueuedOrInFlight(dest) is true or dest.[[state]] is
  //    "closed", then
  if (WritableStreamCloseQueuedOrInFlight(unwrappedDest) ||
      unwrappedDest->closed()) {
    return OnDestClosed(cx, state);
  }

  *erroredOrClosed = false;
  return true;
}

[[nodiscard]] static bool OnSourceClosed(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
  cx->check(state);

  if (!OnSourceClosed(cx, state)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

[[nodiscard]] static bool OnSourceErrored(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
  cx->check(state);

  Rooted<ReadableStream*> unwrappedSource(cx, GetUnwrappedSource(cx, state));
  if (!unwrappedSource) {
    return false;
  }

  if (!OnSourceErrored(cx, state, unwrappedSource)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

[[nodiscard]] static bool OnDestClosed(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
  cx->check(state);

  if (!OnDestClosed(cx, state)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

[[nodiscard]] static bool OnDestErrored(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
  cx->check(state);

  Rooted<WritableStream*> unwrappedDest(cx, GetUnwrappedDest(cx, state));
  if (!unwrappedDest) {
    return false;
  }

  if (!OnDestErrored(cx, state, unwrappedDest)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

template <class StreamAccessor, class Stream>
static inline JSObject* GetClosedPromise(
    JSContext* cx, Handle<Stream*> unwrappedStream,
    StreamAccessor* (&unwrapAccessorFromStream)(JSContext*, Handle<Stream*>)) {
  StreamAccessor* unwrappedAccessor =
      unwrapAccessorFromStream(cx, unwrappedStream);
  if (!unwrappedAccessor) {
    return nullptr;
  }

  return unwrappedAccessor->closedPromise();
}

[[nodiscard]] static bool ReadFromSource(JSContext* cx,
                                         Handle<PipeToState*> state);

static bool ReadFulfilled(JSContext* cx, Handle<PipeToState*> state,
                          Handle<JSObject*> result) {
  cx->check(state);
  cx->check(result);

  state->clearPendingRead();

  // "Shutdown must stop activity: if shuttingDown becomes true, the user agent
  // must not initiate further reads from reader, and must only perform writes
  // of already-read chunks".
  //
  // We may reach this point after |On{Source,Dest}{Clos,Error}ed| has responded
  // to an out-of-band change.  Per the comment in |OnSourceErrored|, we want to
  // allow the implicated shutdown to proceed, and we don't want to interfere
  // with or additionally alter its operation.  Particularly, we don't want to
  // queue up the successfully-read chunk (if there was one, and this isn't just
  // reporting "done") to be written: it wasn't "already-read" when that
  // error/closure happened.
  //
  // All specified reactions to a closure/error invoke either the shutdown, or
  // shutdown with an action, algorithms.  Those algorithms each abort if either
  // shutdown algorithm has already been invoked.  So we check for shutdown here
  // in case of asynchronous closure/error and abort if shutdown has already
  // started (and possibly finished).
  if (state->shuttingDown()) {
    return true;
  }

  {
    bool done;
    {
      Rooted<Value> doneVal(cx);
      if (!GetProperty(cx, result, result, cx->names().done, &doneVal)) {
        return false;
      }
      done = doneVal.toBoolean();
    }

    if (done) {
      // All chunks have been read from |reader| and written to |writer| (but
      // not necessarily fulfilled yet, in the latter case).  Proceed as if
      // |source| is now closed.  (This will asynchronously wait until any
      // pending writes have fulfilled.)
      return OnSourceClosed(cx, state);
    }
  }

  // A chunk was read, and *at the time the read was requested*, |dest| was
  // ready to accept a write.  (Only one read is processed at a time per
  // |state->hasPendingRead()|, so this condition remains true now.)  Write the
  // chunk to |dest|.
  {
    Rooted<Value> chunk(cx);
    if (!GetProperty(cx, result, result, cx->names().value, &chunk)) {
      return false;
    }

    Rooted<WritableStreamDefaultWriter*> writer(cx, state->writer());
    cx->check(writer);

    PromiseObject* writeRequest =
        WritableStreamDefaultWriterWrite(cx, writer, chunk);
    if (!writeRequest) {
      return false;
    }

    // Stash away this new last write request.  (The shutdown process will react
    // to this write request to finish shutdown only once all pending writes are
    // completed.)
    state->updateLastWriteRequest(writeRequest);
  }

  // Read another chunk if this write didn't fill up |dest|.
  //
  // While we (properly) ignored |state->shuttingDown()| earlier, this call will
  // *not* initiate a fresh read if |!state->shuttingDown()|.
  return ReadFromSource(cx, state);
}

static bool ReadFulfilled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
  cx->check(state);

  Rooted<JSObject*> result(cx, &args[0].toObject());
  cx->check(result);

  if (!ReadFulfilled(cx, state, result)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool ReadFromSource(JSContext* cx, unsigned argc, Value* vp);

[[nodiscard]] static bool ReadFromSource(JSContext* cx,
                                         Handle<PipeToState*> state) {
  cx->check(state);

  MOZ_ASSERT(!state->hasPendingRead(),
             "should only have one read in flight at a time, because multiple "
             "reads could cause the latter read to ignore backpressure "
             "signals");

  // "Shutdown must stop activity: if shuttingDown becomes true, the user agent
  // must not initiate further reads from reader..."
  if (state->shuttingDown()) {
    return true;
  }

  Rooted<WritableStreamDefaultWriter*> writer(cx, state->writer());
  cx->check(writer);

  // "While WritableStreamDefaultWriterGetDesiredSize(writer) is ≤ 0 or is null,
  // the user agent must not read from reader."
  Rooted<Value> desiredSize(cx);
  if (!WritableStreamDefaultWriterGetDesiredSize(cx, writer, &desiredSize)) {
    return false;
  }

  // If we're in the middle of erroring or are fully errored, either way the
  // |dest|-closed reaction queued up in |StartPiping| will do the right
  // thing, so do nothing here.
  if (desiredSize.isNull()) {
#ifdef DEBUG
    {
      WritableStream* unwrappedDest = GetUnwrappedDest(cx, state);
      if (!unwrappedDest) {
        return false;
      }

      MOZ_ASSERT(unwrappedDest->erroring() || unwrappedDest->errored());
    }
#endif

    return true;
  }

  // If |dest| isn't ready to receive writes yet (i.e. backpressure applies),
  // resume when it is.
  MOZ_ASSERT(desiredSize.isNumber());
  if (desiredSize.toNumber() <= 0) {
    Rooted<JSObject*> readyPromise(cx, writer->readyPromise());
    cx->check(readyPromise);

    Rooted<JSFunction*> readFromSource(cx,
                                       NewHandler(cx, ReadFromSource, state));
    if (!readFromSource) {
      return false;
    }

    // Resume when there's writable capacity.  Don't bother handling rejection:
    // if this happens, the stream is going to be errored shortly anyway, and
    // |StartPiping| has us ready to react to that already.
    //
    // XXX Double-check the claim that we need not handle rejections and that a
    //     rejection of [[readyPromise]] *necessarily* is always followed by
    //     rejection of [[closedPromise]].
    return JS::AddPromiseReactionsIgnoringUnhandledRejection(
        cx, readyPromise, readFromSource, nullptr);
  }

  // |dest| is ready to receive at least one write.  Read one chunk from the
  // reader now that we're not subject to backpressure.
  Rooted<ReadableStreamDefaultReader*> reader(cx, state->reader());
  cx->check(reader);

  Rooted<PromiseObject*> readRequest(
      cx, js::ReadableStreamDefaultReaderRead(cx, reader));
  if (!readRequest) {
    return false;
  }

  Rooted<JSFunction*> readFulfilled(cx, NewHandler(cx, ReadFulfilled, state));
  if (!readFulfilled) {
    return false;
  }

#ifdef DEBUG
  MOZ_ASSERT(!state->pendingReadWouldBeRejected());

  // The specification for ReadableStreamError ensures that rejecting a read or
  // read-into request is immediately followed by rejecting the reader's
  // [[closedPromise]].  Therefore, it does not appear *necessary* to handle the
  // rejected case -- the [[closedPromise]] reaction will do so for us.
  //
  // However, this is all very stateful and gnarly, so we implement a rejection
  // handler that sets a flag to indicate the read was rejected.  Then if the
  // [[closedPromise]] reaction function is invoked, we can assert that *if*
  // a read is recorded as pending at that instant, a reject handler would have
  // been invoked for it.
  auto ReadRejected = [](JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);

    Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
    cx->check(state);

    state->setPendingReadWouldBeRejected();

    args.rval().setUndefined();
    return true;
  };

  Rooted<JSFunction*> readRejected(cx, NewHandler(cx, ReadRejected, state));
  if (!readRejected) {
    return false;
  }
#else
  auto readRejected = nullptr;
#endif

  // Once the chunk is read, immediately write it and attempt to read more.
  // Don't bother handling a rejection: |source| will be closed/errored, and
  // |StartPiping| poised us to react to that already.
  if (!JS::AddPromiseReactionsIgnoringUnhandledRejection(
          cx, readRequest, readFulfilled, readRejected)) {
    return false;
  }

  // The spec is clear that a write started before an error/stream-closure is
  // encountered must be completed before shutdown.  It is *not* clear that a
  // read that hasn't yet fulfilled should delay shutdown (or until that read's
  // successive write is completed).
  //
  // It seems easiest to explain, both from a user perspective (no read is ever
  // just dropped on the ground) and an implementer perspective (if we *don't*
  // delay, then a read could be started, a shutdown could be started, then the
  // read could finish but we can't write it which arguably conflicts with the
  // requirement that chunks that have been read must be written before shutdown
  // completes), to delay.  XXX file a spec issue to require this!
  state->setPendingRead();
  return true;
}

static bool ReadFromSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<PipeToState*> state(cx, TargetFromHandler<PipeToState>(args));
  cx->check(state);

  if (!ReadFromSource(cx, state)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

[[nodiscard]] static bool StartPiping(JSContext* cx, Handle<PipeToState*> state,
                                      Handle<ReadableStream*> unwrappedSource,
                                      Handle<WritableStream*> unwrappedDest) {
  cx->check(state);

  // "Shutdown must stop activity: if shuttingDown becomes true, the user agent
  // must not initiate further reads from reader..."
  MOZ_ASSERT(!state->shuttingDown(), "can't be shutting down when starting");

  // "Error and close states must be propagated: the following conditions must
  // be applied in order."
  //
  // Before piping has started, we have to check for source/dest being errored
  // or closed manually.
  bool erroredOrClosed;
  if (!SourceOrDestErroredOrClosed(cx, state, unwrappedSource, unwrappedDest,
                                   &erroredOrClosed)) {
    return false;
  }
  if (erroredOrClosed) {
    return true;
  }

  // *After* piping has started, add reactions to respond to source/dest
  // becoming errored or closed.
  {
    Rooted<JSObject*> unwrappedClosedPromise(cx);
    Rooted<JSObject*> onClosed(cx);
    Rooted<JSObject*> onErrored(cx);

    auto ReactWhenClosedOrErrored =
        [&unwrappedClosedPromise, &onClosed, &onErrored, &state](
            JSContext* cx, JSNative onClosedFunc, JSNative onErroredFunc) {
          onClosed = NewHandler(cx, onClosedFunc, state);
          if (!onClosed) {
            return false;
          }

          onErrored = NewHandler(cx, onErroredFunc, state);
          if (!onErrored) {
            return false;
          }

          return JS::AddPromiseReactions(cx, unwrappedClosedPromise, onClosed,
                                         onErrored);
        };

    unwrappedClosedPromise =
        GetClosedPromise(cx, unwrappedSource, UnwrapReaderFromStream);
    if (!unwrappedClosedPromise) {
      return false;
    }

    if (!ReactWhenClosedOrErrored(cx, OnSourceClosed, OnSourceErrored)) {
      return false;
    }

    unwrappedClosedPromise =
        GetClosedPromise(cx, unwrappedDest, UnwrapWriterFromStream);
    if (!unwrappedClosedPromise) {
      return false;
    }

    if (!ReactWhenClosedOrErrored(cx, OnDestClosed, OnDestErrored)) {
      return false;
    }
  }

  return ReadFromSource(cx, state);
}

/**
 * Stream spec, 4.8.1. ReadableStreamPipeTo ( source, dest,
 *                                            preventClose, preventAbort,
 *                                            preventCancel[, signal] )
 * Step 14.1 abortAlgorithm.
 */
[[nodiscard]] static bool PerformAbortAlgorithm(JSContext* cx,
                                                Handle<PipeToState*> state) {
  cx->check(state);

  // Step 14.1: Let abortAlgorithm be the following steps:
  // Step 14.1.1: Let error be a new "AbortError" DOMException.
  // Step 14.1.2: Let actions be an empty ordered set.
  // Step 14.1.3: If preventAbort is false, append the following action to
  //              actions:
  // Step 14.1.3.1: If dest.[[state]] is "writable", return
  //                ! WritableStreamAbort(dest, error).
  // Step 14.1.3.2: Otherwise, return a promise resolved with undefined.
  // Step 14.1.4: If preventCancel is false, append the following action action
  //              to actions:
  // Step 14.1.4.1: If source.[[state]] is "readable", return
  //                ! ReadableStreamCancel(source, error).
  // Step 14.1.4.2: Otherwise, return a promise resolved with undefined.
  // Step 14.1.5: Shutdown with an action consisting of getting a promise to
  //              wait for all of the actions in actions, and with error.
  // XXX jwalden
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_READABLESTREAM_METHOD_NOT_IMPLEMENTED,
                            "abortAlgorithm steps");
  return false;
}

/**
 * Stream spec, 3.4.11. ReadableStreamPipeTo ( source, dest,
 *                                             preventClose, preventAbort,
 *                                             preventCancel, signal )
 * Steps 4-11, 13-14.
 */
/* static */ PipeToState* PipeToState::create(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<ReadableStream*> unwrappedSource,
    Handle<WritableStream*> unwrappedDest, bool preventClose, bool preventAbort,
    bool preventCancel, Handle<JSObject*> signal) {
  cx->check(promise);
  cx->check(signal);

  Rooted<PipeToState*> state(cx,
                             NewTenuredBuiltinClassInstance<PipeToState>(cx));
  if (!state) {
    return nullptr;
  }

  // Step 4. Assert: signal is undefined or signal is an instance of the
  //         AbortSignal interface.
  MOZ_ASSERT(state->getFixedSlot(Slot_Signal).isUndefined());
  if (signal) {
    // |signal| is double-checked to be an |AbortSignal| further down.
    state->initFixedSlot(Slot_Signal, ObjectValue(*signal));
  }

  // Step 5: Assert: ! IsReadableStreamLocked(source) is false.
  MOZ_ASSERT(!unwrappedSource->locked());

  // Step 6: Assert: ! IsWritableStreamLocked(dest) is false.
  MOZ_ASSERT(!unwrappedDest->isLocked());

  MOZ_ASSERT(state->getFixedSlot(Slot_Promise).isUndefined());
  state->initFixedSlot(Slot_Promise, ObjectValue(*promise));

  // Step 7: If ! IsReadableByteStreamController(
  //                  source.[[readableStreamController]]) is true, let reader
  //         be either ! AcquireReadableStreamBYOBReader(source) or
  //         ! AcquireReadableStreamDefaultReader(source), at the user agent’s
  //         discretion.
  // Step 8: Otherwise, let reader be
  //         ! AcquireReadableStreamDefaultReader(source).
  // We don't implement byte streams, so we always acquire a default reader.
  {
    ReadableStreamDefaultReader* reader = CreateReadableStreamDefaultReader(
        cx, unwrappedSource, ForAuthorCodeBool::No);
    if (!reader) {
      return nullptr;
    }

    MOZ_ASSERT(state->getFixedSlot(Slot_Reader).isUndefined());
    state->initFixedSlot(Slot_Reader, ObjectValue(*reader));
  }

  // Step 9: Let writer be ! AcquireWritableStreamDefaultWriter(dest).
  {
    WritableStreamDefaultWriter* writer =
        CreateWritableStreamDefaultWriter(cx, unwrappedDest);
    if (!writer) {
      return nullptr;
    }

    MOZ_ASSERT(state->getFixedSlot(Slot_Writer).isUndefined());
    state->initFixedSlot(Slot_Writer, ObjectValue(*writer));
  }

  // Step 10: Set source.[[disturbed]] to true.
  unwrappedSource->setDisturbed();

  state->initFlags(preventClose, preventAbort, preventCancel);
  MOZ_ASSERT(state->preventClose() == preventClose);
  MOZ_ASSERT(state->preventAbort() == preventAbort);
  MOZ_ASSERT(state->preventCancel() == preventCancel);

  // Step 11: Let shuttingDown be false.
  MOZ_ASSERT(!state->shuttingDown(), "should be set to false by initFlags");

  // Step 12 ("Let promise be a new promise.") was performed by the caller and
  // |promise| was its result.

  // XXX This used to be step 13 but is now step 14, all the step-comments of
  //     the overall algorithm need renumbering.
  // Step 13: If signal is not undefined,
  if (signal) {
    // Step 14.2: If signal’s aborted flag is set, perform abortAlgorithm and
    //         return promise.
    bool aborted;
    {
      // Sadly, we can't assert |signal| is an |AbortSignal| here because it
      // could have become a nuked CCW since it was type-checked.
      JSObject* unwrappedSignal = UnwrapSignalFromPipeToState(cx, state);
      if (!unwrappedSignal) {
        return nullptr;
      }

      JSRuntime* rt = cx->runtime();
      MOZ_ASSERT(unwrappedSignal->hasClass(rt->maybeAbortSignalClass()));

      AutoRealm ar(cx, unwrappedSignal);
      aborted = rt->abortSignalIsAborted(unwrappedSignal);
    }
    if (aborted) {
      if (!PerformAbortAlgorithm(cx, state)) {
        return nullptr;
      }

      // Returning |state| here will cause |promise| to be returned by the
      // overall algorithm.
      return state;
    }

    // Step 14.3: Add abortAlgorithm to signal.
    // XXX jwalden need JSAPI to add an algorithm/steps to an AbortSignal
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAM_METHOD_NOT_IMPLEMENTED,
                              "adding abortAlgorithm to signal");
    return nullptr;
  }

  // Step 14: In parallel, using reader and writer, read all chunks from source
  //          and write them to dest.
  if (!StartPiping(cx, state, unwrappedSource, unwrappedDest)) {
    return nullptr;
  }

  return state;
}

const JSClass PipeToState::class_ = {"PipeToState",
                                     JSCLASS_HAS_RESERVED_SLOTS(SlotCount)};
