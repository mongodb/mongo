/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* General readable stream abstract operations. */

#include "builtin/streams/ReadableStreamOperations.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include "builtin/Array.h"                // js::NewDenseFullyAllocatedArray
#include "builtin/Promise.h"              // js::RejectPromiseWithPendingError
#include "builtin/streams/PipeToState.h"  // js::PipeToState
#include "builtin/streams/ReadableStream.h"  // js::ReadableStream
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStream{,Default}Controller
#include "builtin/streams/ReadableStreamDefaultControllerOperations.h"  // js::ReadableStreamDefaultController{Close,Enqueue}, js::ReadableStreamControllerError, js::SourceAlgorithms
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStreamCancel
#include "builtin/streams/ReadableStreamReader.h"  // js::CreateReadableStreamDefaultReader, js::ForAuthorCodeBool, js::ReadableStream{,Default}Reader, js::ReadableStreamDefaultReaderRead
#include "builtin/streams/TeeState.h"              // js::TeeState
#include "js/CallArgs.h"                           // JS::CallArgs{,FromVp}
#include "js/Promise.h"  // JS::CallOriginalPromiseThen, JS::AddPromiseReactions
#include "js/RootingAPI.h"        // JS::{,Mutable}Handle, JS::Rooted
#include "js/Value.h"             // JS::Value, JS::UndefinedHandleValue
#include "vm/JSContext.h"         // JSContext
#include "vm/NativeObject.h"      // js::NativeObject
#include "vm/ObjectOperations.h"  // js::GetProperty
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined

#include "builtin/HandlerFunction-inl.h"  // js::NewHandler, js::TargetFromHandler
#include "builtin/streams/MiscellaneousOperations-inl.h"  // js::ResolveUnwrappedPromiseWithValue
#include "builtin/streams/ReadableStreamReader-inl.h"  // js::UnwrapReaderFromStream
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::Unwrap{Callee,Internal}Slot
#include "vm/JSContext-inl.h"  // JSContext::check
#include "vm/JSObject-inl.h"   // js::IsCallable, js::NewObjectWithClassProto
#include "vm/Realm-inl.h"      // js::AutoRealm

using js::IsCallable;
using js::NewHandler;
using js::NewObjectWithClassProto;
using js::PromiseObject;
using js::ReadableStream;
using js::ReadableStreamDefaultController;
using js::ReadableStreamDefaultControllerEnqueue;
using js::ReadableStreamDefaultReader;
using js::ReadableStreamReader;
using js::SourceAlgorithms;
using js::TargetFromHandler;
using js::TeeState;
using js::UnwrapCalleeSlot;

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::MutableHandle;
using JS::ObjectValue;
using JS::Rooted;
using JS::UndefinedHandleValue;
using JS::Value;

/*** 3.4. General readable stream abstract operations ***********************/

// Streams spec, 3.4.1. AcquireReadableStreamBYOBReader ( stream )
// Always inlined.

// Streams spec, 3.4.2. AcquireReadableStreamDefaultReader ( stream )
// Always inlined. See CreateReadableStreamDefaultReader.

/**
 * Streams spec, 3.4.3. CreateReadableStream (
 *                          startAlgorithm, pullAlgorithm, cancelAlgorithm
 *                          [, highWaterMark [, sizeAlgorithm ] ] )
 *
 * The start/pull/cancelAlgorithm arguments are represented instead as four
 * arguments: sourceAlgorithms, underlyingSource, pullMethod, cancelMethod.
 * See the comment on SetUpReadableStreamDefaultController.
 */
[[nodiscard]] static ReadableStream* CreateReadableStream(
    JSContext* cx, SourceAlgorithms sourceAlgorithms,
    Handle<Value> underlyingSource,
    Handle<Value> pullMethod = UndefinedHandleValue,
    Handle<Value> cancelMethod = UndefinedHandleValue, double highWaterMark = 1,
    Handle<Value> sizeAlgorithm = UndefinedHandleValue,
    Handle<JSObject*> proto = nullptr) {
  cx->check(underlyingSource, sizeAlgorithm, proto);
  MOZ_ASSERT(sizeAlgorithm.isUndefined() || IsCallable(sizeAlgorithm));

  // Step 1: If highWaterMark was not passed, set it to 1 (implicit).
  // Step 2: If sizeAlgorithm was not passed, set it to an algorithm that
  //         returns 1 (implicit).
  // Step 3: Assert: ! IsNonNegativeNumber(highWaterMark) is true.
  MOZ_ASSERT(highWaterMark >= 0);

  // Step 4: Let stream be ObjectCreate(the original value of ReadableStream's
  //         prototype property).
  // Step 5: Perform ! InitializeReadableStream(stream).
  Rooted<ReadableStream*> stream(cx,
                                 ReadableStream::create(cx, nullptr, proto));
  if (!stream) {
    return nullptr;
  }

  // Step 6: Let controller be ObjectCreate(the original value of
  //         ReadableStreamDefaultController's prototype property).
  // Step 7: Perform ? SetUpReadableStreamDefaultController(stream,
  //         controller, startAlgorithm, pullAlgorithm, cancelAlgorithm,
  //         highWaterMark, sizeAlgorithm).
  if (!SetUpReadableStreamDefaultController(
          cx, stream, sourceAlgorithms, underlyingSource, pullMethod,
          cancelMethod, highWaterMark, sizeAlgorithm)) {
    return nullptr;
  }

  // Step 8: Return stream.
  return stream;
}

// Streams spec, 3.4.4. CreateReadableByteStream (
//                          startAlgorithm, pullAlgorithm, cancelAlgorithm
//                          [, highWaterMark [, autoAllocateChunkSize ] ] )
// Not implemented.

/**
 * Streams spec, 3.4.5. InitializeReadableStream ( stream )
 */
/* static */ [[nodiscard]] ReadableStream* ReadableStream::create(
    JSContext* cx, void* nsISupportsObject_alreadyAddreffed /* = nullptr */,
    Handle<JSObject*> proto /* = nullptr */) {
  // In the spec, InitializeReadableStream is always passed a newly created
  // ReadableStream object. We instead create it here and return it below.
  Rooted<ReadableStream*> stream(
      cx, NewObjectWithClassProto<ReadableStream>(cx, proto));
  if (!stream) {
    return nullptr;
  }

  stream->setPrivate(nsISupportsObject_alreadyAddreffed);

  // Step 1: Set stream.[[state]] to "readable".
  stream->initStateBits(Readable);
  MOZ_ASSERT(stream->readable());

  // Step 2: Set stream.[[reader]] and stream.[[storedError]] to
  //         undefined (implicit).
  MOZ_ASSERT(!stream->hasReader());
  MOZ_ASSERT(stream->storedError().isUndefined());

  // Step 3: Set stream.[[disturbed]] to false (done in step 1).
  MOZ_ASSERT(!stream->disturbed());

  return stream;
}

// Streams spec, 3.4.6. IsReadableStream ( x )
// Using UnwrapAndTypeCheck templates instead.

// Streams spec, 3.4.7. IsReadableStreamDisturbed ( stream )
// Using stream->disturbed() instead.

/**
 * Streams spec, 3.4.8. IsReadableStreamLocked ( stream )
 */
bool ReadableStream::locked() const {
  // Step 1: Assert: ! IsReadableStream(stream) is true (implicit).
  // Step 2: If stream.[[reader]] is undefined, return false.
  // Step 3: Return true.
  // Special-casing for streams with external sources. Those can be locked
  // explicitly via JSAPI, which is indicated by a controller flag.
  // IsReadableStreamLocked is called from the controller's constructor, at
  // which point we can't yet call stream->controller(), but the source also
  // can't be locked yet.
  if (hasController() && controller()->sourceLocked()) {
    return true;
  }
  return hasReader();
}

// Streams spec, 3.4.9. IsReadableStreamAsyncIterator ( x )
//
// Not implemented.

/**
 * Streams spec, 3.4.10. ReadableStreamTee steps 12.c.i-x.
 */
static bool TeeReaderReadHandler(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<TeeState*> unwrappedTeeState(cx,
                                      UnwrapCalleeSlot<TeeState>(cx, args, 0));
  if (!unwrappedTeeState) {
    return false;
  }

  Handle<Value> resultVal = args.get(0);

  // Step 12.c.i: Set reading to false.
  unwrappedTeeState->unsetReading();

  // Step 12.c.ii: Assert: Type(result) is Object.
  Rooted<JSObject*> result(cx, &resultVal.toObject());

  bool done;
  {
    // Step 12.c.iii: Let done be ? Get(result, "done").
    // (This can fail only if `result` was nuked.)
    Rooted<Value> doneVal(cx);
    if (!GetProperty(cx, result, result, cx->names().done, &doneVal)) {
      return false;
    }

    // Step 12.c.iv: Assert: Type(done) is Boolean.
    done = doneVal.toBoolean();
  }

  if (done) {
    // Step 12.3 close steps

    // Step 1: Set reading to false (done unconditionally above).
    // Step 2: If canceled1 is false, perform
    //         ! ReadableStreamDefaultControllerClose(branch1.[[controller]]).
    if (!unwrappedTeeState->canceled1()) {
      Rooted<ReadableStreamDefaultController*> unwrappedBranch1(
          cx, unwrappedTeeState->branch1());
      if (!ReadableStreamDefaultControllerClose(cx, unwrappedBranch1)) {
        return false;
      }
    }

    // Step 3: If canceled2 is false, perform
    //         ! ReadableStreamDefaultControllerClose(branch2.[[controller]]).
    if (!unwrappedTeeState->canceled2()) {
      Rooted<ReadableStreamDefaultController*> unwrappedBranch2(
          cx, unwrappedTeeState->branch2());
      if (!ReadableStreamDefaultControllerClose(cx, unwrappedBranch2)) {
        return false;
      }
    }

    // Step 4: If canceled1 is false or canceled2 is false,
    //         resolve cancelPromise with undefined.
    if (!unwrappedTeeState->canceled1() || !unwrappedTeeState->canceled2()) {
      Rooted<PromiseObject*> unwrappedCancelPromise(
          cx, unwrappedTeeState->cancelPromise());
      MOZ_ASSERT(unwrappedCancelPromise != nullptr);

      if (!ResolveUnwrappedPromiseWithUndefined(cx, unwrappedCancelPromise)) {
        return false;
      }
    }

    args.rval().setUndefined();
    return true;
  }

  // Step 12.c.vi: Let value be ! Get(result, "value").
  // (This can fail only if `result` was nuked.)
  Rooted<Value> value(cx);
  if (!GetProperty(cx, result, result, cx->names().value, &value)) {
    return false;
  }

  // Step 12.c.vii: Let value1 and value2 be value.
  // Step 12.c.viii: If canceled2 is false and cloneForBranch2 is true, set
  //                 value2 to
  //                 ? StructuredDeserialize(? StructuredSerialize(value2),
  //                                         the current Realm Record).
  // We don't yet support any specifications that use cloneForBranch2, and
  // the Streams spec doesn't offer any way for author code to enable it,
  // so it's always false here.
  auto& value1 = value;
  MOZ_ASSERT(!unwrappedTeeState->cloneForBranch2(),
             "support for cloneForBranch2=true is not yet implemented");
  auto& value2 = value;

  Rooted<ReadableStreamDefaultController*> unwrappedController(cx);

  // Step 12.c.ix: If canceled1 is false, perform
  //               ? ReadableStreamDefaultControllerEnqueue(
  //                     branch1.[[readableStreamController]], value1).
  if (!unwrappedTeeState->canceled1()) {
    unwrappedController = unwrappedTeeState->branch1();
    if (!ReadableStreamDefaultControllerEnqueue(cx, unwrappedController,
                                                value1)) {
      return false;
    }
  }

  // Step 12.c.x: If canceled2 is false, perform
  //              ? ReadableStreamDefaultControllerEnqueue(
  //                    branch2.[[readableStreamController]], value2).
  if (!unwrappedTeeState->canceled2()) {
    unwrappedController = unwrappedTeeState->branch2();
    if (!ReadableStreamDefaultControllerEnqueue(cx, unwrappedController,
                                                value2)) {
      return false;
    }
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 3.4.10. ReadableStreamTee step 12, "Let pullAlgorithm be the
 * following steps:"
 */
[[nodiscard]] PromiseObject* js::ReadableStreamTee_Pull(
    JSContext* cx, JS::Handle<TeeState*> unwrappedTeeState) {
  // Combine step 12.a/12.e far below, and handle steps 12.b-12.d after
  // inverting step 12.a's "If reading is true" condition.
  if (!unwrappedTeeState->reading()) {
    // Step 12.b: Set reading to true.
    unwrappedTeeState->setReading();

    // Implicit in the spec: Unpack `reader` from the TeeState (by way of the
    // stream stored in one of its slots).
    Rooted<ReadableStreamDefaultReader*> unwrappedReader(cx);
    {
      Rooted<ReadableStream*> unwrappedStream(
          cx, UnwrapInternalSlot<ReadableStream>(cx, unwrappedTeeState,
                                                 TeeState::Slot_Stream));
      if (!unwrappedStream) {
        return nullptr;
      }
      ReadableStreamReader* unwrappedReaderObj =
          UnwrapReaderFromStream(cx, unwrappedStream);
      if (!unwrappedReaderObj) {
        return nullptr;
      }

      unwrappedReader = &unwrappedReaderObj->as<ReadableStreamDefaultReader>();
    }

    // Step 12.c: Let readPromise be the result of reacting to
    //            ! ReadableStreamDefaultReaderRead(reader) with the following
    //            fulfillment steps given the argument result: [...]
    // Step 12.d: Set readPromise.[[PromiseIsHandled]] to true.

    // First, perform |ReadableStreamDefaultReaderRead(reader)|.
    Rooted<PromiseObject*> readerReadResultPromise(
        cx, js::ReadableStreamDefaultReaderRead(cx, unwrappedReader));
    if (!readerReadResultPromise) {
      return nullptr;
    }

    // Next, create a function to perform the fulfillment steps under step 12.c
    // (implemented in the |TeeReaderReadHandler| C++ function).
    Rooted<JSObject*> teeState(cx, unwrappedTeeState);
    if (!cx->compartment()->wrap(cx, &teeState)) {
      return nullptr;
    }

    Rooted<JSObject*> onFulfilled(
        cx, NewHandler(cx, TeeReaderReadHandler, teeState));
    if (!onFulfilled) {
      return nullptr;
    }

    // Finally, perform those fulfillment steps when |readerReadResultPromise|
    // fulfills.  (Step 12.c doesn't provide rejection steps, so don't handle
    // rejection.)
    //
    // The spec's |readPromise| promise is unobservable, so implement this using
    // a JSAPI function that acts as if it created |readPromise| but doesn't
    // actually do so.
    //
    // Step 12.d causes |readPromise| to be treated as handled, even if it
    // rejects.  Use |JS::AddPromiseReactionsIgnoringUnhandledRejection|, not
    // |JS::AddPromiseReactions|, to avoid reporting a freshly-consed-up promise
    // as rejected if |readerReadResultPromise| rejects.
    if (!JS::AddPromiseReactionsIgnoringUnhandledRejection(
            cx, readerReadResultPromise, onFulfilled, nullptr)) {
      return nullptr;
    }
  }

  // Step 12.a: (If reading is true,) return a promise resolved with undefined.
  // Step 12.e: Return a promise resolved with undefined.
  return PromiseResolvedWithUndefined(cx);
}

/**
 * Cancel one branch of a tee'd stream with the given |reason_|.
 *
 * Streams spec, 3.4.10. ReadableStreamTee steps 13 and 14: "Let
 * cancel1Algorithm/cancel2Algorithm be the following steps, taking a reason
 * argument:"
 */
[[nodiscard]] JSObject* js::ReadableStreamTee_Cancel(
    JSContext* cx, JS::Handle<TeeState*> unwrappedTeeState,
    JS::Handle<ReadableStreamDefaultController*> unwrappedBranch,
    JS::Handle<Value> reason) {
  Rooted<ReadableStream*> unwrappedStream(
      cx, UnwrapInternalSlot<ReadableStream>(cx, unwrappedTeeState,
                                             TeeState::Slot_Stream));
  if (!unwrappedStream) {
    return nullptr;
  }

  bool bothBranchesCanceled = false;

  // Step 13/14.a: Set canceled1/canceled2 to true.
  // Step 13/14.b: Set reason1/reason2 to reason.
  {
    AutoRealm ar(cx, unwrappedTeeState);

    Rooted<Value> unwrappedReason(cx, reason);
    if (!cx->compartment()->wrap(cx, &unwrappedReason)) {
      return nullptr;
    }

    if (unwrappedBranch->isTeeBranch1()) {
      unwrappedTeeState->setCanceled1(unwrappedReason);
      bothBranchesCanceled = unwrappedTeeState->canceled2();
    } else {
      MOZ_ASSERT(unwrappedBranch->isTeeBranch2());
      unwrappedTeeState->setCanceled2(unwrappedReason);
      bothBranchesCanceled = unwrappedTeeState->canceled1();
    }
  }

  Rooted<PromiseObject*> unwrappedCancelPromise(
      cx, unwrappedTeeState->cancelPromise());
  MOZ_ASSERT(unwrappedCancelPromise != nullptr);

  // Step 13/14.c: If canceled2/canceled1 is true,
  if (bothBranchesCanceled) {
    // Step 13/14.c.i: Let compositeReason be
    //                 ! CreateArrayFromList(« reason1, reason2 »).
    Rooted<Value> compositeReason(cx);
    {
      Rooted<Value> reason1(cx, unwrappedTeeState->reason1());
      Rooted<Value> reason2(cx, unwrappedTeeState->reason2());
      if (!cx->compartment()->wrap(cx, &reason1) ||
          !cx->compartment()->wrap(cx, &reason2)) {
        return nullptr;
      }

      ArrayObject* reasonArray = NewDenseFullyAllocatedArray(cx, 2);
      if (!reasonArray) {
        return nullptr;
      }
      reasonArray->setDenseInitializedLength(2);
      reasonArray->initDenseElement(0, reason1);
      reasonArray->initDenseElement(1, reason2);

      compositeReason = ObjectValue(*reasonArray);
    }

    // Step 13/14.c.ii: Let cancelResult be
    //                  ! ReadableStreamCancel(stream, compositeReason).
    // In our implementation, this can fail with OOM. The best course then
    // is to reject cancelPromise with an OOM error.
    Rooted<JSObject*> cancelResult(
        cx, js::ReadableStreamCancel(cx, unwrappedStream, compositeReason));
    if (!cancelResult) {
      // Handle the OOM case mentioned above.
      AutoRealm ar(cx, unwrappedCancelPromise);
      if (!RejectPromiseWithPendingError(cx, unwrappedCancelPromise)) {
        return nullptr;
      }
    } else {
      // Step 13/14.c.iii: Resolve cancelPromise with cancelResult.
      Rooted<Value> cancelResultVal(cx, ObjectValue(*cancelResult));
      if (!ResolveUnwrappedPromiseWithValue(cx, unwrappedCancelPromise,
                                            cancelResultVal)) {
        return nullptr;
      }
    }
  }

  // Step 13/14.d: Return cancelPromise.
  Rooted<JSObject*> cancelPromise(cx, unwrappedCancelPromise);
  if (!cx->compartment()->wrap(cx, &cancelPromise)) {
    return nullptr;
  }

  return cancelPromise;
}

/*
 * https://streams.spec.whatwg.org/#readable-stream-tee
 * ReadableStreamTee(stream, cloneForBranch2)
 *
 * Step 18: Upon rejection of reader.[[closedPromise]] with reason r,
 */
static bool TeeReaderErroredHandler(JSContext* cx, unsigned argc,
                                    JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<TeeState*> teeState(cx, TargetFromHandler<TeeState>(args));
  Handle<Value> reason = args.get(0);

  Rooted<ReadableStreamDefaultController*> unwrappedBranchController(cx);

  // Step 18.1: Perform
  //               ! ReadableStreamDefaultControllerError(
  //                   branch1.[[controller]], r).
  unwrappedBranchController = teeState->branch1();
  if (!ReadableStreamControllerError(cx, unwrappedBranchController, reason)) {
    return false;
  }

  // Step 18.2: Perform
  //            ! ReadableStreamDefaultControllerError(
  //                branch2.[[controller]], r).
  unwrappedBranchController = teeState->branch2();
  if (!ReadableStreamControllerError(cx, unwrappedBranchController, reason)) {
    return false;
  }

  // Step 18.3: If canceled1 is false or canceled2 is false,
  //            resolve cancelPromise with undefined.
  if (!teeState->canceled1() || !teeState->canceled2()) {
    Rooted<PromiseObject*> unwrappedCancelPromise(cx,
                                                  teeState->cancelPromise());
    MOZ_ASSERT(unwrappedCancelPromise != nullptr);

    if (!ResolveUnwrappedPromiseWithUndefined(cx, unwrappedCancelPromise)) {
      return false;
    }
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 3.4.10. ReadableStreamTee ( stream, cloneForBranch2 )
 */
[[nodiscard]] bool js::ReadableStreamTee(
    JSContext* cx, JS::Handle<ReadableStream*> unwrappedStream,
    bool cloneForBranch2, JS::MutableHandle<ReadableStream*> branch1Stream,
    JS::MutableHandle<ReadableStream*> branch2Stream) {
  // Step 1: Assert: ! IsReadableStream(stream) is true (implicit).

  // Step 2: Assert: Type(cloneForBranch2) is Boolean (implicit).
  //
  // The streams spec only ever passes |cloneForBranch2 = false|.  It's expected
  // that external specs that pass |cloneForBranch2 = true| will at some point
  // come into existence, but we don't presently implement any such specs.
  MOZ_ASSERT(!cloneForBranch2,
             "support for cloneForBranch2=true is not yet implemented");

  // Step 3: Let reader be ? AcquireReadableStreamDefaultReader(stream).
  Rooted<ReadableStreamDefaultReader*> reader(
      cx, CreateReadableStreamDefaultReader(cx, unwrappedStream,
                                            ForAuthorCodeBool::No));
  if (!reader) {
    return false;
  }

  // Several algorithms close over the variables initialized in the next few
  // steps, so we allocate them in an object, the TeeState. The algorithms
  // also close over `stream` and `reader`, so TeeState gets a reference to
  // the stream.
  //
  // Step 4: Let reading be false.
  // Step 5: Let canceled1 be false.
  // Step 6: Let canceled2 be false.
  // Step 7: Let reason1 be undefined.
  // Step 8: Let reason2 be undefined.
  // Step 9: Let branch1 be undefined.
  // Step 10: Let branch2 be undefined.
  // Step 11: Let cancelPromise be a new promise.
  Rooted<TeeState*> teeState(cx, TeeState::create(cx, unwrappedStream));
  if (!teeState) {
    return false;
  }

  MOZ_ASSERT(!teeState->reading());
  MOZ_ASSERT(!teeState->canceled1());
  MOZ_ASSERT(!teeState->canceled2());

  // Step 12: Let pullAlgorithm be the following steps: [...]
  // Step 13: Let cancel1Algorithm be the following steps: [...]
  // Step 14: Let cancel2Algorithm be the following steps: [...]
  // Step 15: Let startAlgorithm be an algorithm that returns undefined.
  //
  // Implicit. Our implementation does not use objects to represent
  // [[pullAlgorithm]], [[cancelAlgorithm]], and so on. Instead, we decide
  // which one to perform based on class checks. For example, our
  // implementation of ReadableStreamControllerCallPullIfNeeded checks
  // whether the stream's underlyingSource is a TeeState object.

  // Step 16: Set branch1 to
  //          ! CreateReadableStream(startAlgorithm, pullAlgorithm,
  //                                 cancel1Algorithm).
  Rooted<Value> underlyingSource(cx, ObjectValue(*teeState));
  branch1Stream.set(
      CreateReadableStream(cx, SourceAlgorithms::Tee, underlyingSource));
  if (!branch1Stream) {
    return false;
  }

  Rooted<ReadableStreamDefaultController*> branch1(cx);
  branch1 = &branch1Stream->controller()->as<ReadableStreamDefaultController>();
  branch1->setTeeBranch1();
  teeState->setBranch1(branch1);

  // Step 17: Set branch2 to
  //          ! CreateReadableStream(startAlgorithm, pullAlgorithm,
  //                                 cancel2Algorithm).
  branch2Stream.set(
      CreateReadableStream(cx, SourceAlgorithms::Tee, underlyingSource));
  if (!branch2Stream) {
    return false;
  }

  Rooted<ReadableStreamDefaultController*> branch2(cx);
  branch2 = &branch2Stream->controller()->as<ReadableStreamDefaultController>();
  branch2->setTeeBranch2();
  teeState->setBranch2(branch2);

  // Step 18: Upon rejection of reader.[[closedPromise]] with reason r, [...]
  Rooted<JSObject*> closedPromise(cx, reader->closedPromise());

  Rooted<JSObject*> onRejected(
      cx, NewHandler(cx, TeeReaderErroredHandler, teeState));
  if (!onRejected) {
    return false;
  }

  if (!JS::AddPromiseReactions(cx, closedPromise, nullptr, onRejected)) {
    return false;
  }

  // Step 19: Return « branch1, branch2 ».
  return true;
}

/**
 * Streams spec, 3.4.10.
 *      ReadableStreamPipeTo ( source, dest, preventClose, preventAbort,
 *                             preventCancel, signal )
 */
PromiseObject* js::ReadableStreamPipeTo(JSContext* cx,
                                        Handle<ReadableStream*> unwrappedSource,
                                        Handle<WritableStream*> unwrappedDest,
                                        bool preventClose, bool preventAbort,
                                        bool preventCancel,
                                        Handle<JSObject*> signal) {
  cx->check(signal);

  // Step 1. Assert: ! IsReadableStream(source) is true.
  // Step 2. Assert: ! IsWritableStream(dest) is true.
  // Step 3. Assert: Type(preventClose) is Boolean, Type(preventAbort) is
  //         Boolean, and Type(preventCancel) is Boolean.
  // (These are guaranteed by the type system.)

  // Step 12: Let promise be a new promise.
  //
  // We reorder this so that this promise can be rejected and returned in case
  // of internal error.
  Rooted<PromiseObject*> promise(cx, PromiseObject::createSkippingExecutor(cx));
  if (!promise) {
    return nullptr;
  }

  // Steps 4-11, 13-14.
  Rooted<PipeToState*> pipeToState(
      cx,
      PipeToState::create(cx, promise, unwrappedSource, unwrappedDest,
                          preventClose, preventAbort, preventCancel, signal));
  if (!pipeToState) {
    if (!RejectPromiseWithPendingError(cx, promise)) {
      return nullptr;
    }

    return promise;
  }

  // Step 15.
  return promise;
}
