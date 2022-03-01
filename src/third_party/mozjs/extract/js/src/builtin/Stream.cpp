/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Stream.h"

#include "js/Stream.h"

#include <stdint.h>  // int32_t

#include "builtin/streams/ClassSpecMacro.h"           // JS_STREAMS_CLASS_SPEC
#include "builtin/streams/MiscellaneousOperations.h"  // js::CreateAlgorithmFromUnderlyingMethod, js::InvokeOrNoop, js::IsMaybeWrapped, js::PromiseCall, js::PromiseRejectedWithPendingError
#include "builtin/streams/PullIntoDescriptor.h"       // js::PullIntoDescriptor
#include "builtin/streams/QueueWithSizes.h"  // js::{EnqueueValueWithSize,ResetQueue}
#include "builtin/streams/ReadableStream.h"  // js::ReadableStream, js::SetUpExternalReadableByteStreamController
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStream{,Default}Controller, js::ReadableStreamDefaultControllerPullSteps, js::ReadableStreamControllerStart{,Failed}Handler
#include "builtin/streams/ReadableStreamDefaultControllerOperations.h"  // js::ReadableStreamControllerClearAlgorithms
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStream{AddReadOrReadIntoRequest,CloseInternal,CreateReadResult,ErrorInternal,FulfillReadOrReadIntoRequest,GetNumReadRequests,HasDefaultReader}
#include "builtin/streams/ReadableStreamReader.h"  // js::ReadableStream{,Default}Reader, js::CreateReadableStreamDefaultReader, js::ReadableStreamReaderGeneric{Cancel,Initialize,Release}, js::ReadableStreamDefaultReaderRead
#include "js/ArrayBuffer.h"                        // JS::NewArrayBuffer
#include "js/experimental/TypedData.h"  // JS_GetArrayBufferViewData, JS_NewUint8Array{,WithBuffer}
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined
#include "vm/SelfHosting.h"

#include "builtin/HandlerFunction-inl.h"  // js::NewHandler
#include "builtin/streams/ReadableStreamReader-inl.h"  // js::Unwrap{ReaderFromStream{,NoThrow},StreamFromReader}
#include "vm/Compartment-inl.h"
#include "vm/List-inl.h"  // js::ListObject, js::StoreNewListInFixedSlot
#include "vm/NativeObject-inl.h"

using namespace js;

#if 0  // disable user-defined byte streams

class ByteStreamChunk : public NativeObject
{
  private:
    enum Slots {
        Slot_Buffer = 0,
        Slot_ByteOffset,
        Slot_ByteLength,
        SlotCount
    };

  public:
    static const JSClass class_;

    ArrayBufferObject* buffer() {
        return &getFixedSlot(Slot_Buffer).toObject().as<ArrayBufferObject>();
    }
    uint32_t byteOffset() { return getFixedSlot(Slot_ByteOffset).toInt32(); }
    void SetByteOffset(uint32_t offset) {
        setFixedSlot(Slot_ByteOffset, Int32Value(offset));
    }
    uint32_t byteLength() { return getFixedSlot(Slot_ByteLength).toInt32(); }
    void SetByteLength(uint32_t length) {
        setFixedSlot(Slot_ByteLength, Int32Value(length));
    }

    static ByteStreamChunk* create(JSContext* cx, HandleObject buffer, uint32_t byteOffset,
                                   uint32_t byteLength)
    {
        Rooted<ByteStreamChunk*> chunk(cx, NewBuiltinClassInstance<ByteStreamChunk>(cx));
        if (!chunk) {
            return nullptr;
        }

        chunk->setFixedSlot(Slot_Buffer, ObjectValue(*buffer));
        chunk->setFixedSlot(Slot_ByteOffset, Int32Value(byteOffset));
        chunk->setFixedSlot(Slot_ByteLength, Int32Value(byteLength));
        return chunk;
    }
};

const JSClass ByteStreamChunk::class_ = {
    "ByteStreamChunk",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount)
};

#endif  // user-defined byte streams

/*** 3.3. ReadableStreamAsyncIteratorPrototype ******************************/

// Not implemented.

/*** 3.7. Class ReadableStreamBYOBReader ************************************/

// Not implemented.

/*** 3.11. Class ReadableByteStreamController *******************************/

#if 0  // disable user-defined byte streams

/**
 * Streams spec, 3.10.3
 *      new ReadableByteStreamController ( stream, underlyingSource,
 *                                         highWaterMark )
 * Steps 3 - 16.
 *
 * Note: All arguments must be same-compartment with cx. ReadableStream
 * controllers are always created in the same compartment as the stream.
 */
[[nodiscard]] static ReadableByteStreamController*
CreateReadableByteStreamController(JSContext* cx,
                                   Handle<ReadableStream*> stream,
                                   HandleValue underlyingByteSource,
                                   HandleValue highWaterMarkVal)
{
    cx->check(stream, underlyingByteSource, highWaterMarkVal);

    Rooted<ReadableByteStreamController*> controller(cx,
        NewBuiltinClassInstance<ReadableByteStreamController>(cx));
    if (!controller) {
        return nullptr;
    }

    // Step 3: Set this.[[controlledReadableStream]] to stream.
    controller->setStream(stream);

    // Step 4: Set this.[[underlyingByteSource]] to underlyingByteSource.
    controller->setUnderlyingSource(underlyingByteSource);

    // Step 5: Set this.[[pullAgain]], and this.[[pulling]] to false.
    controller->setFlags(0);

    // Step 6: Perform ! ReadableByteStreamControllerClearPendingPullIntos(this).
    if (!ReadableByteStreamControllerClearPendingPullIntos(cx, controller)) {
        return nullptr;
    }

    // Step 7: Perform ! ResetQueue(this).
    if (!ResetQueue(cx, controller)) {
        return nullptr;
    }

    // Step 8: Set this.[[started]] and this.[[closeRequested]] to false.
    // These should be false by default, unchanged since step 5.
    MOZ_ASSERT(controller->flags() == 0);

    // Step 9: Set this.[[strategyHWM]] to
    //         ? ValidateAndNormalizeHighWaterMark(highWaterMark).
    double highWaterMark;
    if (!ValidateAndNormalizeHighWaterMark(cx, highWaterMarkVal, &highWaterMark)) {
        return nullptr;
    }
    controller->setStrategyHWM(highWaterMark);

    // Step 10: Let autoAllocateChunkSize be
    //          ? GetV(underlyingByteSource, "autoAllocateChunkSize").
    RootedValue autoAllocateChunkSize(cx);
    if (!GetProperty(cx, underlyingByteSource, cx->names().autoAllocateChunkSize,
                     &autoAllocateChunkSize))
    {
        return nullptr;
    }

    // Step 11: If autoAllocateChunkSize is not undefined,
    if (!autoAllocateChunkSize.isUndefined()) {
        // Step a: If ! IsInteger(autoAllocateChunkSize) is false, or if
        //         autoAllocateChunkSize ≤ 0, throw a RangeError exception.
        if (!IsInteger(autoAllocateChunkSize) || autoAllocateChunkSize.toNumber() <= 0) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                      JSMSG_READABLEBYTESTREAMCONTROLLER_BAD_CHUNKSIZE);
            return nullptr;
        }
    }

    // Step 12: Set this.[[autoAllocateChunkSize]] to autoAllocateChunkSize.
    controller->setAutoAllocateChunkSize(autoAllocateChunkSize);

    // Step 13: Set this.[[pendingPullIntos]] to a new empty List.
    if (!StoreNewListInFixedSlot(cx, controller,
                                 ReadableByteStreamController::Slot_PendingPullIntos)) {
        return nullptr;
    }

    // Step 14: Let controller be this (implicit).

    // Step 15: Let startResult be
    //          ? InvokeOrNoop(underlyingSource, "start", « this »).
    RootedValue startResult(cx);
    RootedValue controllerVal(cx, ObjectValue(*controller));
    if (!InvokeOrNoop(cx, underlyingByteSource, cx->names().start, controllerVal, &startResult)) {
        return nullptr;
    }

    // Step 16: Let startPromise be a promise resolved with startResult:
    RootedObject startPromise(cx, PromiseObject::unforgeableResolve(cx, startResult));
    if (!startPromise) {
        return nullptr;
    }

    RootedObject onStartFulfilled(cx, NewHandler(cx, ReadableStreamControllerStartHandler, controller));
    if (!onStartFulfilled) {
        return nullptr;
    }

    RootedObject onStartRejected(cx, NewHandler(cx, ControllerStartFailedHandler, controller));
    if (!onStartRejected) {
        return nullptr;
    }

    if (!JS::AddPromiseReactions(cx, startPromise, onStartFulfilled, onStartRejected)) {
        return nullptr;
    }

    return controller;
}

#endif  // user-defined byte streams

/**
 * Streams spec, 3.11.3.
 * new ReadableByteStreamController ( stream, underlyingByteSource,
 *                                    highWaterMark )
 */
bool ReadableByteStreamController::constructor(JSContext* cx, unsigned argc,
                                               Value* vp) {
  // Step 1: Throw a TypeError exception.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_BOGUS_CONSTRUCTOR,
                            "ReadableByteStreamController");
  return false;
}

// Disconnect the source from a controller without calling finalize() on it,
// unless this class is reset(). This ensures that finalize() will not be called
// on the source if setting up the controller fails.
class MOZ_RAII AutoClearUnderlyingSource {
  Rooted<ReadableStreamController*> controller_;

 public:
  AutoClearUnderlyingSource(JSContext* cx, ReadableStreamController* controller)
      : controller_(cx, controller) {}

  ~AutoClearUnderlyingSource() {
    if (controller_) {
      ReadableStreamController::clearUnderlyingSource(
          controller_, /* finalizeSource */ false);
    }
  }

  void reset() { controller_ = nullptr; }
};

/**
 * Version of SetUpReadableByteStreamController that's specialized for handling
 * external, embedding-provided, underlying sources.
 */
[[nodiscard]] bool js::SetUpExternalReadableByteStreamController(
    JSContext* cx, Handle<ReadableStream*> stream,
    JS::ReadableStreamUnderlyingSource* source) {
  // Done elsewhere in the standard: Create the controller object.
  Rooted<ReadableByteStreamController*> controller(
      cx, NewBuiltinClassInstance<ReadableByteStreamController>(cx));
  if (!controller) {
    return false;
  }

  AutoClearUnderlyingSource autoClear(cx, controller);

  // Step 1: Assert: stream.[[readableStreamController]] is undefined.
  MOZ_ASSERT(!stream->hasController());

  // Step 2: If autoAllocateChunkSize is not undefined, [...]
  // (It's treated as undefined.)

  // Step 3: Set controller.[[controlledReadableByteStream]] to stream.
  controller->setStream(stream);

  // Step 4: Set controller.[[pullAgain]] and controller.[[pulling]] to false.
  controller->setFlags(0);
  MOZ_ASSERT(!controller->pullAgain());
  MOZ_ASSERT(!controller->pulling());

  // Step 5: Perform
  //         ! ReadableByteStreamControllerClearPendingPullIntos(controller).
  // Omitted. This step is apparently redundant; see
  // <https://github.com/whatwg/streams/issues/975>.

  // Step 6: Perform ! ResetQueue(this).
  controller->setQueueTotalSize(0);

  // Step 7: Set controller.[[closeRequested]] and controller.[[started]] to
  //         false (implicit).
  MOZ_ASSERT(!controller->closeRequested());
  MOZ_ASSERT(!controller->started());

  // Step 8: Set controller.[[strategyHWM]] to
  //         ? ValidateAndNormalizeHighWaterMark(highWaterMark).
  controller->setStrategyHWM(0);

  // Step 9: Set controller.[[pullAlgorithm]] to pullAlgorithm.
  // Step 10: Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
  // (These algorithms are given by source's virtual methods.)
  controller->setExternalSource(source);

  // Step 11: Set controller.[[autoAllocateChunkSize]] to
  //          autoAllocateChunkSize (implicit).
  MOZ_ASSERT(controller->autoAllocateChunkSize().isUndefined());

  // Step 12: Set this.[[pendingPullIntos]] to a new empty List.
  if (!StoreNewListInFixedSlot(
          cx, controller,
          ReadableByteStreamController::Slot_PendingPullIntos)) {
    return false;
  }

  // Step 13: Set stream.[[readableStreamController]] to controller.
  stream->setController(controller);

  // Step 14: Let startResult be the result of performing startAlgorithm.
  // (For external sources, this algorithm does nothing and returns undefined.)
  // Step 15: Let startPromise be a promise resolved with startResult.
  Rooted<PromiseObject*> startPromise(cx, PromiseResolvedWithUndefined(cx));
  if (!startPromise) {
    return false;
  }

  // Step 16: Upon fulfillment of startPromise, [...]
  // Step 17: Upon rejection of startPromise with reason r, [...]
  RootedObject onStartFulfilled(
      cx, NewHandler(cx, ReadableStreamControllerStartHandler, controller));
  if (!onStartFulfilled) {
    return false;
  }
  RootedObject onStartRejected(
      cx,
      NewHandler(cx, ReadableStreamControllerStartFailedHandler, controller));
  if (!onStartRejected) {
    return false;
  }
  if (!JS::AddPromiseReactions(cx, startPromise, onStartFulfilled,
                               onStartRejected)) {
    return false;
  }

  autoClear.reset();
  return true;
}

static const JSPropertySpec ReadableByteStreamController_properties[] = {
    JS_PS_END};

static const JSFunctionSpec ReadableByteStreamController_methods[] = {
    JS_FS_END};

static void ReadableByteStreamControllerFinalize(JSFreeOp* fop, JSObject* obj) {
  ReadableByteStreamController& controller =
      obj->as<ReadableByteStreamController>();

  if (controller.getFixedSlot(ReadableStreamController::Slot_Flags)
          .isUndefined()) {
    return;
  }

  if (!controller.hasExternalSource()) {
    return;
  }

  controller.externalSource()->finalize();
}

static const JSClassOps ReadableByteStreamControllerClassOps = {
    nullptr,                               // addProperty
    nullptr,                               // delProperty
    nullptr,                               // enumerate
    nullptr,                               // newEnumerate
    nullptr,                               // resolve
    nullptr,                               // mayResolve
    ReadableByteStreamControllerFinalize,  // finalize
    nullptr,                               // call
    nullptr,                               // hasInstance
    nullptr,                               // construct
    nullptr,                               // trace
};

JS_STREAMS_CLASS_SPEC(ReadableByteStreamController, 0, SlotCount,
                      ClassSpec::DontDefineConstructor,
                      JSCLASS_BACKGROUND_FINALIZE,
                      &ReadableByteStreamControllerClassOps);

// Streams spec, 3.11.5.1. [[CancelSteps]] ()
// Unified with 3.9.5.1 above.

[[nodiscard]] static bool ReadableByteStreamControllerHandleQueueDrain(
    JSContext* cx, Handle<ReadableStreamController*> unwrappedController);

/**
 * Streams spec, 3.11.5.2. [[PullSteps]] ( forAuthorCode )
 */
[[nodiscard]] static PromiseObject* ReadableByteStreamControllerPullSteps(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController) {
  // Step 1: Let stream be this.[[controlledReadableByteStream]].
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 2: Assert: ! ReadableStreamHasDefaultReader(stream) is true.
#ifdef DEBUG
  bool result;
  if (!ReadableStreamHasDefaultReader(cx, unwrappedStream, &result)) {
    return nullptr;
  }
  MOZ_ASSERT(result);
#endif

  RootedValue val(cx);
  // Step 3: If this.[[queueTotalSize]] > 0,
  double queueTotalSize = unwrappedController->queueTotalSize();
  if (queueTotalSize > 0) {
    // Step 3.a: Assert: ! ReadableStreamGetNumReadRequests(_stream_) is 0.
    MOZ_ASSERT(ReadableStreamGetNumReadRequests(unwrappedStream) == 0);

    RootedObject view(cx);

    MOZ_RELEASE_ASSERT(unwrappedStream->mode() ==
                       JS::ReadableStreamMode::ExternalSource);
#if 0   // disable user-defined byte streams
        if (unwrappedStream->mode() == JS::ReadableStreamMode::ExternalSource)
#endif  // user-defined byte streams
    {
      JS::ReadableStreamUnderlyingSource* source =
          unwrappedController->externalSource();

      view = JS_NewUint8Array(cx, queueTotalSize);
      if (!view) {
        return nullptr;
      }

      size_t bytesWritten;
      {
        AutoRealm ar(cx, unwrappedStream);
        JS::AutoSuppressGCAnalysis suppressGC(cx);
        JS::AutoCheckCannotGC noGC;
        bool dummy;
        void* buffer = JS_GetArrayBufferViewData(view, &dummy, noGC);

        source->writeIntoReadRequestBuffer(cx, unwrappedStream, buffer,
                                           queueTotalSize, &bytesWritten);
      }

      queueTotalSize = queueTotalSize - bytesWritten;
    }

#if 0   // disable user-defined byte streams
        else {
            // Step 3.b: Let entry be the first element of this.[[queue]].
            // Step 3.c: Remove entry from this.[[queue]], shifting all other
            //           elements downward (so that the second becomes the
            //           first, and so on).
            Rooted<ListObject*> unwrappedQueue(cx, unwrappedController->queue());
            Rooted<ByteStreamChunk*> unwrappedEntry(cx,
                UnwrapAndDowncastObject<ByteStreamChunk>(
                    cx, &unwrappedQueue->popFirstAs<JSObject>(cx)));
            if (!unwrappedEntry) {
                return nullptr;
            }

            queueTotalSize = queueTotalSize - unwrappedEntry->byteLength();

            // Step 3.f: Let view be ! Construct(%Uint8Array%,
            //                                   « entry.[[buffer]],
            //                                     entry.[[byteOffset]],
            //                                     entry.[[byteLength]] »).
            // (reordered)
            RootedObject buffer(cx, unwrappedEntry->buffer());
            if (!cx->compartment()->wrap(cx, &buffer)) {
                return nullptr;
            }

            uint32_t byteOffset = unwrappedEntry->byteOffset();
            view = JS_NewUint8ArrayWithBuffer(cx, buffer, byteOffset, unwrappedEntry->byteLength());
            if (!view) {
                return nullptr;
            }
        }
#endif  // user-defined byte streams

    // Step 3.d: Set this.[[queueTotalSize]] to
    //           this.[[queueTotalSize]] − entry.[[byteLength]].
    // (reordered)
    unwrappedController->setQueueTotalSize(queueTotalSize);

    // Step 3.e: Perform ! ReadableByteStreamControllerHandleQueueDrain(this).
    // (reordered)
    if (!ReadableByteStreamControllerHandleQueueDrain(cx,
                                                      unwrappedController)) {
      return nullptr;
    }

    // Step 3.g: Return a promise resolved with
    //           ! ReadableStreamCreateReadResult(view, false, forAuthorCode).
    val.setObject(*view);
    ReadableStreamReader* unwrappedReader =
        UnwrapReaderFromStream(cx, unwrappedStream);
    if (!unwrappedReader) {
      return nullptr;
    }
    Rooted<PlainObject*> readResult(
        cx, ReadableStreamCreateReadResult(cx, val, false,
                                           unwrappedReader->forAuthorCode()));
    if (!readResult) {
      return nullptr;
    }
    val.setObject(*readResult);

    return PromiseObject::unforgeableResolveWithNonPromise(cx, val);
  }

  // Step 4: Let autoAllocateChunkSize be this.[[autoAllocateChunkSize]].
  val = unwrappedController->autoAllocateChunkSize();

  // Step 5: If autoAllocateChunkSize is not undefined,
  if (!val.isUndefined()) {
    double autoAllocateChunkSize = val.toNumber();

    // Step 5.a: Let buffer be
    //           Construct(%ArrayBuffer%, « autoAllocateChunkSize »).
    JSObject* bufferObj = JS::NewArrayBuffer(cx, autoAllocateChunkSize);

    // Step 5.b: If buffer is an abrupt completion,
    //           return a promise rejected with buffer.[[Value]].
    if (!bufferObj) {
      return PromiseRejectedWithPendingError(cx);
    }

    RootedArrayBufferObject buffer(cx, &bufferObj->as<ArrayBufferObject>());

    // Step 5.c: Let pullIntoDescriptor be
    //           Record {[[buffer]]: buffer.[[Value]],
    //                   [[byteOffset]]: 0,
    //                   [[byteLength]]: autoAllocateChunkSize,
    //                   [[bytesFilled]]: 0,
    //                   [[elementSize]]: 1,
    //                   [[ctor]]: %Uint8Array%,
    //                   [[readerType]]: `"default"`}.
    RootedObject pullIntoDescriptor(
        cx, PullIntoDescriptor::create(cx, buffer, 0, autoAllocateChunkSize, 0,
                                       1, nullptr, ReaderType::Default));
    if (!pullIntoDescriptor) {
      return PromiseRejectedWithPendingError(cx);
    }

    // Step 5.d: Append pullIntoDescriptor as the last element of
    //           this.[[pendingPullIntos]].
    if (!AppendToListInFixedSlot(
            cx, unwrappedController,
            ReadableByteStreamController::Slot_PendingPullIntos,
            pullIntoDescriptor)) {
      return nullptr;
    }
  }

  // Step 6: Let promise be ! ReadableStreamAddReadRequest(stream,
  //                                                       forAuthorCode).
  Rooted<PromiseObject*> promise(
      cx, ReadableStreamAddReadOrReadIntoRequest(cx, unwrappedStream));
  if (!promise) {
    return nullptr;
  }

  // Step 7: Perform ! ReadableByteStreamControllerCallPullIfNeeded(this).
  if (!ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController)) {
    return nullptr;
  }

  // Step 8: Return promise.
  return promise;
}

/**
 * Unified implementation of ReadableStream controllers' [[PullSteps]] internal
 * methods.
 * Streams spec, 3.9.5.2. [[PullSteps]] ( forAuthorCode )
 * and
 * Streams spec, 3.11.5.2. [[PullSteps]] ( forAuthorCode )
 */
[[nodiscard]] PromiseObject* js::ReadableStreamControllerPullSteps(
    JSContext* cx, Handle<ReadableStreamController*> unwrappedController) {
  if (unwrappedController->is<ReadableStreamDefaultController>()) {
    Rooted<ReadableStreamDefaultController*> unwrappedDefaultController(
        cx, &unwrappedController->as<ReadableStreamDefaultController>());
    return ReadableStreamDefaultControllerPullSteps(cx,
                                                    unwrappedDefaultController);
  }

  Rooted<ReadableByteStreamController*> unwrappedByteController(
      cx, &unwrappedController->as<ReadableByteStreamController>());
  return ReadableByteStreamControllerPullSteps(cx, unwrappedByteController);
}

/*** 3.13. Readable stream BYOB controller abstract operations **************/

// Streams spec, 3.13.1. IsReadableStreamBYOBRequest ( x )
// Implemented via is<ReadableStreamBYOBRequest>()

// Streams spec, 3.13.2. IsReadableByteStreamController ( x )
// Implemented via is<ReadableByteStreamController>()

// Streams spec, 3.13.3.
//      ReadableByteStreamControllerCallPullIfNeeded ( controller )
// Unified with 3.9.2 above.

[[nodiscard]] static bool ReadableByteStreamControllerInvalidateBYOBRequest(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController);

/**
 * Streams spec, 3.13.5.
 *      ReadableByteStreamControllerClearPendingPullIntos ( controller )
 */
[[nodiscard]] bool js::ReadableByteStreamControllerClearPendingPullIntos(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController) {
  // Step 1: Perform
  //         ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
  if (!ReadableByteStreamControllerInvalidateBYOBRequest(cx,
                                                         unwrappedController)) {
    return false;
  }

  // Step 2: Set controller.[[pendingPullIntos]] to a new empty List.
  return StoreNewListInFixedSlot(
      cx, unwrappedController,
      ReadableByteStreamController::Slot_PendingPullIntos);
}

/**
 * Streams spec, 3.13.6. ReadableByteStreamControllerClose ( controller )
 */
[[nodiscard]] bool js::ReadableByteStreamControllerClose(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController) {
  // Step 1: Let stream be controller.[[controlledReadableByteStream]].
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 2: Assert: controller.[[closeRequested]] is false.
  MOZ_ASSERT(!unwrappedController->closeRequested());

  // Step 3: Assert: stream.[[state]] is "readable".
  MOZ_ASSERT(unwrappedStream->readable());

  // Step 4: If controller.[[queueTotalSize]] > 0,
  if (unwrappedController->queueTotalSize() > 0) {
    // Step a: Set controller.[[closeRequested]] to true.
    unwrappedController->setCloseRequested();

    // Step b: Return.
    return true;
  }

  // Step 5: If controller.[[pendingPullIntos]] is not empty,
  Rooted<ListObject*> unwrappedPendingPullIntos(
      cx, unwrappedController->pendingPullIntos());
  if (unwrappedPendingPullIntos->length() != 0) {
    // Step a: Let firstPendingPullInto be the first element of
    //         controller.[[pendingPullIntos]].
    Rooted<PullIntoDescriptor*> unwrappedFirstPendingPullInto(
        cx, UnwrapAndDowncastObject<PullIntoDescriptor>(
                cx, &unwrappedPendingPullIntos->get(0).toObject()));
    if (!unwrappedFirstPendingPullInto) {
      return false;
    }

    // Step b: If firstPendingPullInto.[[bytesFilled]] > 0,
    if (unwrappedFirstPendingPullInto->bytesFilled() > 0) {
      // Step i: Let e be a new TypeError exception.
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_READABLEBYTESTREAMCONTROLLER_CLOSE_PENDING_PULL);
      RootedValue e(cx);
      RootedSavedFrame stack(cx);
      if (!cx->isExceptionPending() ||
          !GetAndClearExceptionAndStack(cx, &e, &stack)) {
        // Uncatchable error. Die immediately without erroring the
        // stream.
        return false;
      }

      // Step ii: Perform ! ReadableByteStreamControllerError(controller, e).
      if (!ReadableStreamControllerError(cx, unwrappedController, e)) {
        return false;
      }

      // Step iii: Throw e.
      cx->setPendingException(e, stack);
      return false;
    }
  }

  // Step 6: Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
  ReadableStreamControllerClearAlgorithms(unwrappedController);

  // Step 7: Perform ! ReadableStreamClose(stream).
  return ReadableStreamCloseInternal(cx, unwrappedStream);
}

// Streams spec, 3.13.11. ReadableByteStreamControllerError ( controller, e )
// Unified with 3.10.7 above.

// Streams spec 3.13.14.
//      ReadableByteStreamControllerGetDesiredSize ( controller )
// Unified with 3.10.8 above.

/**
 * Streams spec, 3.13.15.
 *      ReadableByteStreamControllerHandleQueueDrain ( controller )
 */
[[nodiscard]] static bool ReadableByteStreamControllerHandleQueueDrain(
    JSContext* cx, Handle<ReadableStreamController*> unwrappedController) {
  MOZ_ASSERT(unwrappedController->is<ReadableByteStreamController>());

  // Step 1: Assert: controller.[[controlledReadableStream]].[[state]]
  //                 is "readable".
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());
  MOZ_ASSERT(unwrappedStream->readable());

  // Step 2: If controller.[[queueTotalSize]] is 0 and
  //         controller.[[closeRequested]] is true,
  if (unwrappedController->queueTotalSize() == 0 &&
      unwrappedController->closeRequested()) {
    // Step a: Perform
    //         ! ReadableByteStreamControllerClearAlgorithms(controller).
    ReadableStreamControllerClearAlgorithms(unwrappedController);

    // Step b: Perform
    //         ! ReadableStreamClose(controller.[[controlledReadableStream]]).
    return ReadableStreamCloseInternal(cx, unwrappedStream);
  }

  // Step 3: Otherwise,
  // Step a: Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
  return ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController);
}

enum BYOBRequestSlots {
  BYOBRequestSlot_Controller,
  BYOBRequestSlot_View,
  BYOBRequestSlotCount
};

/**
 * Streams spec 3.13.16.
 *      ReadableByteStreamControllerInvalidateBYOBRequest ( controller )
 */
[[nodiscard]] static bool ReadableByteStreamControllerInvalidateBYOBRequest(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController) {
  // Step 1: If controller.[[byobRequest]] is undefined, return.
  RootedValue unwrappedBYOBRequestVal(cx, unwrappedController->byobRequest());
  if (unwrappedBYOBRequestVal.isUndefined()) {
    return true;
  }

  RootedNativeObject unwrappedBYOBRequest(
      cx, UnwrapAndDowncastValue<NativeObject>(cx, unwrappedBYOBRequestVal));
  if (!unwrappedBYOBRequest) {
    return false;
  }

  // Step 2: Set controller.[[byobRequest]]
  //                       .[[associatedReadableByteStreamController]]
  //         to undefined.
  unwrappedBYOBRequest->setFixedSlot(BYOBRequestSlot_Controller,
                                     UndefinedValue());

  // Step 3: Set controller.[[byobRequest]].[[view]] to undefined.
  unwrappedBYOBRequest->setFixedSlot(BYOBRequestSlot_View, UndefinedValue());

  // Step 4: Set controller.[[byobRequest]] to undefined.
  unwrappedController->clearBYOBRequest();

  return true;
}

// Streams spec, 3.13.25.
//      ReadableByteStreamControllerShouldCallPull ( controller )
// Unified with 3.10.3 above.
