/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Public and friend stream APIs for external use. */

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include <stdint.h>  // uint32_t, uintptr_t

#include "jsapi.h"        // js::AssertHeapIsIdle, JS_ReportErrorNumberASCII
#include "jsfriendapi.h"  // js::IsObjectInContextCompartment
#include "jstypes.h"      // JS_{FRIEND,PUBLIC}_API

#include "builtin/Stream.h"  // js::ReadableByteStreamController{,Close}, js::ReadableStreamDefaultController{,Close}, js::StreamController
#include "builtin/streams/ReadableStream.h"  // js::ReadableStream
#include "builtin/streams/ReadableStreamController.h"  // js::CheckReadableStreamControllerCanCloseOrEnqueue
#include "builtin/streams/ReadableStreamDefaultControllerOperations.h"  // js::ReadableStreamController{Error,GetDesiredSizeUnchecked}, js::SetUpReadableStreamDefaultControllerFromUnderlyingSource
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStream{Cancel,FulfillReadOrReadIntoRequest,GetNumReadRequests,HasDefaultReader}
#include "builtin/streams/ReadableStreamOperations.h"  // js::ReadableStreamTee
#include "builtin/streams/ReadableStreamReader.h"  // js::ReadableStream{,Default}Reader, js::ForAuthorCodeBool
#include "builtin/streams/StreamController.h"  // js::StreamController
#include "gc/Zone.h"                           // JS::Zone
#include "js/experimental/TypedData.h"  // JS_GetArrayBufferViewData, JS_NewUint8Array
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/GCAPI.h"       // JS::AutoCheckCannotGC, JS::AutoSuppressGCAnalysis
#include "js/Object.h"      // JS::SetPrivate
#include "js/RootingAPI.h"  // JS::{,Mutable}Handle, JS::Rooted
#include "js/Stream.h"      // JS::ReadableStreamUnderlyingSource
#include "js/Value.h"       // JS::{,Object,Undefined}Value
#include "vm/ArrayBufferViewObject.h"  // js::ArrayBufferViewObject
#include "vm/JSContext.h"              // JSContext, CHECK_THREAD
#include "vm/JSObject.h"               // JSObject
#include "vm/PlainObject.h"            // js::PlainObject
#include "vm/PromiseObject.h"          // js::PromiseObject

#include "builtin/streams/ReadableStreamReader-inl.h"  // js::UnwrapStreamFromReader
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::UnwrapAndDowncastObject
#include "vm/JSObject-inl.h"  // js::NewBuiltinClassInstance
#include "vm/Realm-inl.h"     // js::AutoRealm

using js::ArrayBufferViewObject;
using js::AssertHeapIsIdle;
using js::AutoRealm;
using js::CheckReadableStreamControllerCanCloseOrEnqueue;
using js::ForAuthorCodeBool;
using js::GetErrorMessage;
using js::IsObjectInContextCompartment;
using js::NewBuiltinClassInstance;
using js::PlainObject;
using js::ReadableByteStreamController;
using js::ReadableByteStreamControllerClose;
using js::ReadableStream;
using js::ReadableStreamController;
using js::ReadableStreamControllerError;
using js::ReadableStreamControllerGetDesiredSizeUnchecked;
using js::ReadableStreamDefaultController;
using js::ReadableStreamDefaultControllerClose;
using js::ReadableStreamDefaultReader;
using js::ReadableStreamFulfillReadOrReadIntoRequest;
using js::ReadableStreamGetNumReadRequests;
using js::ReadableStreamHasDefaultReader;
using js::ReadableStreamReader;
using js::ReadableStreamTee;
using js::SetUpReadableStreamDefaultControllerFromUnderlyingSource;
using js::StreamController;
using js::UnwrapAndDowncastObject;
using js::UnwrapStreamFromReader;

JS_PUBLIC_API JSObject* js::UnwrapReadableStream(JSObject* obj) {
  return obj->maybeUnwrapIf<ReadableStream>();
}

JS_PUBLIC_API JSObject* JS::NewReadableDefaultStreamObject(
    JSContext* cx, JS::Handle<JSObject*> underlyingSource /* = nullptr */,
    JS::Handle<JSFunction*> size /* = nullptr */,
    double highWaterMark /* = 1 */,
    JS::Handle<JSObject*> proto /* = nullptr */) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(underlyingSource, size, proto);
  MOZ_ASSERT(highWaterMark >= 0);

  // A copy of ReadableStream::constructor, with most of the
  // argument-checking done implicitly by C++ type checking.
  Rooted<ReadableStream*> stream(cx, ReadableStream::create(cx));
  if (!stream) {
    return nullptr;
  }
  Rooted<Value> sourceVal(cx);
  if (underlyingSource) {
    sourceVal.setObject(*underlyingSource);
  } else {
    JSObject* source = NewBuiltinClassInstance<PlainObject>(cx);
    if (!source) {
      return nullptr;
    }
    sourceVal.setObject(*source);
  }
  Rooted<Value> sizeVal(cx, size ? ObjectValue(*size) : UndefinedValue());

  if (!SetUpReadableStreamDefaultControllerFromUnderlyingSource(
          cx, stream, sourceVal, highWaterMark, sizeVal)) {
    return nullptr;
  }

  return stream;
}

JS_PUBLIC_API JSObject* JS::NewReadableExternalSourceStreamObject(
    JSContext* cx, JS::ReadableStreamUnderlyingSource* underlyingSource,
    void* nsISupportsObject_alreadyAddreffed /* = nullptr */,
    Handle<JSObject*> proto /* = nullptr */) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_ASSERT(underlyingSource);
  MOZ_ASSERT((uintptr_t(underlyingSource) & 1) == 0,
             "external underlying source pointers must be aligned");
  cx->check(proto);

  return ReadableStream::createExternalSourceStream(
      cx, underlyingSource, nsISupportsObject_alreadyAddreffed, proto);
}

JS_PUBLIC_API bool JS::IsReadableStream(JSObject* obj) {
  return obj->canUnwrapAs<ReadableStream>();
}

JS_PUBLIC_API bool JS::IsReadableStreamReader(JSObject* obj) {
  return obj->canUnwrapAs<ReadableStreamDefaultReader>();
}

JS_PUBLIC_API bool JS::IsReadableStreamDefaultReader(JSObject* obj) {
  return obj->canUnwrapAs<ReadableStreamDefaultReader>();
}

template <class T>
[[nodiscard]] static T* APIUnwrapAndDowncast(JSContext* cx, JSObject* obj) {
  cx->check(obj);
  return UnwrapAndDowncastObject<T>(cx, obj);
}

JS_PUBLIC_API bool JS::ReadableStreamIsReadable(JSContext* cx,
                                                Handle<JSObject*> streamObj,
                                                bool* result) {
  ReadableStream* unwrappedStream =
      APIUnwrapAndDowncast<ReadableStream>(cx, streamObj);
  if (!unwrappedStream) {
    return false;
  }

  *result = unwrappedStream->readable();
  return true;
}

JS_PUBLIC_API bool JS::ReadableStreamIsLocked(JSContext* cx,
                                              Handle<JSObject*> streamObj,
                                              bool* result) {
  ReadableStream* unwrappedStream =
      APIUnwrapAndDowncast<ReadableStream>(cx, streamObj);
  if (!unwrappedStream) {
    return false;
  }

  *result = unwrappedStream->locked();
  return true;
}

JS_PUBLIC_API bool JS::ReadableStreamIsDisturbed(JSContext* cx,
                                                 Handle<JSObject*> streamObj,
                                                 bool* result) {
  ReadableStream* unwrappedStream =
      APIUnwrapAndDowncast<ReadableStream>(cx, streamObj);
  if (!unwrappedStream) {
    return false;
  }

  *result = unwrappedStream->disturbed();
  return true;
}

JS_PUBLIC_API JSObject* JS::ReadableStreamCancel(JSContext* cx,
                                                 Handle<JSObject*> streamObj,
                                                 Handle<Value> reason) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(reason);

  Rooted<ReadableStream*> unwrappedStream(
      cx, APIUnwrapAndDowncast<ReadableStream>(cx, streamObj));
  if (!unwrappedStream) {
    return nullptr;
  }

  return js::ReadableStreamCancel(cx, unwrappedStream, reason);
}

JS_PUBLIC_API bool JS::ReadableStreamGetMode(JSContext* cx,
                                             Handle<JSObject*> streamObj,
                                             JS::ReadableStreamMode* mode) {
  ReadableStream* unwrappedStream =
      APIUnwrapAndDowncast<ReadableStream>(cx, streamObj);
  if (!unwrappedStream) {
    return false;
  }

  *mode = unwrappedStream->mode();
  return true;
}

JS_PUBLIC_API JSObject* JS::ReadableStreamGetReader(
    JSContext* cx, Handle<JSObject*> streamObj, ReadableStreamReaderMode mode) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  Rooted<ReadableStream*> unwrappedStream(
      cx, APIUnwrapAndDowncast<ReadableStream>(cx, streamObj));
  if (!unwrappedStream) {
    return nullptr;
  }

  JSObject* result = CreateReadableStreamDefaultReader(cx, unwrappedStream,
                                                       ForAuthorCodeBool::No);
  MOZ_ASSERT_IF(result, IsObjectInContextCompartment(result, cx));
  return result;
}

JS_PUBLIC_API bool JS::ReadableStreamGetExternalUnderlyingSource(
    JSContext* cx, Handle<JSObject*> streamObj,
    JS::ReadableStreamUnderlyingSource** source) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  Rooted<ReadableStream*> unwrappedStream(
      cx, APIUnwrapAndDowncast<ReadableStream>(cx, streamObj));
  if (!unwrappedStream) {
    return false;
  }

  MOZ_ASSERT(unwrappedStream->mode() == JS::ReadableStreamMode::ExternalSource);
  if (unwrappedStream->locked()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAM_LOCKED);
    return false;
  }
  if (!unwrappedStream->readable()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMCONTROLLER_NOT_READABLE,
                              "ReadableStreamGetExternalUnderlyingSource");
    return false;
  }

  auto unwrappedController =
      &unwrappedStream->controller()->as<ReadableByteStreamController>();
  unwrappedController->setSourceLocked();
  *source = unwrappedController->externalSource();
  return true;
}

JS_PUBLIC_API bool JS::ReadableStreamReleaseExternalUnderlyingSource(
    JSContext* cx, Handle<JSObject*> streamObj) {
  ReadableStream* unwrappedStream =
      APIUnwrapAndDowncast<ReadableStream>(cx, streamObj);
  if (!unwrappedStream) {
    return false;
  }

  MOZ_ASSERT(unwrappedStream->mode() == JS::ReadableStreamMode::ExternalSource);
  MOZ_ASSERT(unwrappedStream->locked());
  MOZ_ASSERT(unwrappedStream->controller()->sourceLocked());
  unwrappedStream->controller()->clearSourceLocked();
  return true;
}

JS_PUBLIC_API bool JS::ReadableStreamUpdateDataAvailableFromSource(
    JSContext* cx, JS::Handle<JSObject*> streamObj, uint32_t availableData) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  Rooted<ReadableStream*> unwrappedStream(
      cx, APIUnwrapAndDowncast<ReadableStream>(cx, streamObj));
  if (!unwrappedStream) {
    return false;
  }

  // This is based on Streams spec 3.11.4.4. enqueue(chunk) steps 1-3 and
  // 3.13.9. ReadableByteStreamControllerEnqueue(controller, chunk) steps
  // 8-9.
  //
  // Adapted to handling updates signaled by the embedding for streams with
  // external underlying sources.
  //
  // The remaining steps of those two functions perform checks and asserts
  // that don't apply to streams with external underlying sources.

  Rooted<ReadableByteStreamController*> unwrappedController(
      cx, &unwrappedStream->controller()->as<ReadableByteStreamController>());

  // Step 2: If this.[[closeRequested]] is true, throw a TypeError exception.
  if (unwrappedController->closeRequested()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMCONTROLLER_CLOSED, "enqueue");
    return false;
  }

  // Step 3: If this.[[controlledReadableStream]].[[state]] is not "readable",
  //         throw a TypeError exception.
  if (!unwrappedController->stream()->readable()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMCONTROLLER_NOT_READABLE,
                              "enqueue");
    return false;
  }

  unwrappedController->clearPullFlags();

#if DEBUG
  uint32_t oldAvailableData =
      unwrappedController->getFixedSlot(StreamController::Slot_TotalSize)
          .toInt32();
#endif  // DEBUG
  unwrappedController->setQueueTotalSize(availableData);

  // 3.139. ReadableByteStreamControllerEnqueue
  // Step 8.a: If ! ReadableStreamGetNumReadRequests(stream) is 0,
  // Reordered because for externally-sourced streams it applies regardless
  // of reader type.
  if (ReadableStreamGetNumReadRequests(unwrappedStream) == 0) {
    return true;
  }

  // Step 8: If ! ReadableStreamHasDefaultReader(stream) is true
  bool hasDefaultReader;
  if (!ReadableStreamHasDefaultReader(cx, unwrappedStream, &hasDefaultReader)) {
    return false;
  }
  if (hasDefaultReader) {
    // Step b: Otherwise,
    // Step i: Assert: controller.[[queue]] is empty.
    MOZ_ASSERT(oldAvailableData == 0);

    // Step ii: Let transferredView be
    //          ! Construct(%Uint8Array%, transferredBuffer,
    //                      byteOffset, byteLength).
    JSObject* viewObj = JS_NewUint8Array(cx, availableData);
    if (!viewObj) {
      return false;
    }
    Rooted<ArrayBufferViewObject*> transferredView(
        cx, &viewObj->as<ArrayBufferViewObject>());
    if (!transferredView) {
      return false;
    }

    JS::ReadableStreamUnderlyingSource* source =
        unwrappedController->externalSource();

    size_t bytesWritten;
    {
      AutoRealm ar(cx, unwrappedStream);
      JS::AutoSuppressGCAnalysis suppressGC(cx);
      JS::AutoCheckCannotGC noGC;
      bool dummy;
      void* buffer = JS_GetArrayBufferViewData(transferredView, &dummy, noGC);
      source->writeIntoReadRequestBuffer(cx, unwrappedStream, buffer,
                                         availableData, &bytesWritten);
    }

    // Step iii: Perform ! ReadableStreamFulfillReadRequest(stream,
    //                                                      transferredView,
    //                                                      false).
    Rooted<Value> chunk(cx, ObjectValue(*transferredView));
    if (!ReadableStreamFulfillReadOrReadIntoRequest(cx, unwrappedStream, chunk,
                                                    false)) {
      return false;
    }

    unwrappedController->setQueueTotalSize(availableData - bytesWritten);
  } else {
    // Step 9: Otherwise, if ! ReadableStreamHasBYOBReader(stream) is true,
    //         [...]
    // (Omitted. BYOB readers are not implemented.)

    // Step 10: Otherwise,
    // Step a: Assert: ! IsReadableStreamLocked(stream) is false.
    MOZ_ASSERT(!unwrappedStream->locked());

    // Step b: Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(
    //         controller, transferredBuffer, byteOffset, byteLength).
    // (Not needed for external underlying sources.)
  }

  return true;
}

JS_PUBLIC_API void JS::ReadableStreamReleaseCCObject(JSObject* streamObj) {
  MOZ_ASSERT(JS::IsReadableStream(streamObj));
  JS::SetPrivate(streamObj, nullptr);
}

JS_PUBLIC_API bool JS::ReadableStreamTee(JSContext* cx,
                                         Handle<JSObject*> streamObj,
                                         MutableHandle<JSObject*> branch1Obj,
                                         MutableHandle<JSObject*> branch2Obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  Rooted<ReadableStream*> unwrappedStream(
      cx, APIUnwrapAndDowncast<ReadableStream>(cx, streamObj));
  if (!unwrappedStream) {
    return false;
  }

  Rooted<ReadableStream*> branch1Stream(cx);
  Rooted<ReadableStream*> branch2Stream(cx);
  if (!ReadableStreamTee(cx, unwrappedStream, false, &branch1Stream,
                         &branch2Stream)) {
    return false;
  }

  branch1Obj.set(branch1Stream);
  branch2Obj.set(branch2Stream);
  return true;
}

JS_PUBLIC_API bool JS::ReadableStreamGetDesiredSize(JSContext* cx,
                                                    JSObject* streamObj,
                                                    bool* hasValue,
                                                    double* value) {
  ReadableStream* unwrappedStream =
      APIUnwrapAndDowncast<ReadableStream>(cx, streamObj);
  if (!unwrappedStream) {
    return false;
  }

  if (unwrappedStream->errored()) {
    *hasValue = false;
    return true;
  }

  *hasValue = true;

  if (unwrappedStream->closed()) {
    *value = 0;
    return true;
  }

  *value = ReadableStreamControllerGetDesiredSizeUnchecked(
      unwrappedStream->controller());
  return true;
}

JS_PUBLIC_API bool JS::ReadableStreamClose(JSContext* cx,
                                           Handle<JSObject*> streamObj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  Rooted<ReadableStream*> unwrappedStream(
      cx, APIUnwrapAndDowncast<ReadableStream>(cx, streamObj));
  if (!unwrappedStream) {
    return false;
  }

  Rooted<ReadableStreamController*> unwrappedControllerObj(
      cx, unwrappedStream->controller());
  if (!CheckReadableStreamControllerCanCloseOrEnqueue(
          cx, unwrappedControllerObj, "close")) {
    return false;
  }

  if (unwrappedControllerObj->is<ReadableStreamDefaultController>()) {
    Rooted<ReadableStreamDefaultController*> unwrappedController(cx);
    unwrappedController =
        &unwrappedControllerObj->as<ReadableStreamDefaultController>();
    return ReadableStreamDefaultControllerClose(cx, unwrappedController);
  }

  Rooted<ReadableByteStreamController*> unwrappedController(cx);
  unwrappedController =
      &unwrappedControllerObj->as<ReadableByteStreamController>();
  return ReadableByteStreamControllerClose(cx, unwrappedController);
}

JS_PUBLIC_API bool JS::ReadableStreamEnqueue(JSContext* cx,
                                             Handle<JSObject*> streamObj,
                                             Handle<Value> chunk) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(chunk);

  Rooted<ReadableStream*> unwrappedStream(
      cx, APIUnwrapAndDowncast<ReadableStream>(cx, streamObj));
  if (!unwrappedStream) {
    return false;
  }

  if (unwrappedStream->mode() != JS::ReadableStreamMode::Default) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAM_NOT_DEFAULT_CONTROLLER,
                              "JS::ReadableStreamEnqueue");
    return false;
  }

  Rooted<ReadableStreamDefaultController*> unwrappedController(cx);
  unwrappedController =
      &unwrappedStream->controller()->as<ReadableStreamDefaultController>();

  MOZ_ASSERT(!unwrappedController->closeRequested());
  MOZ_ASSERT(unwrappedStream->readable());

  return ReadableStreamDefaultControllerEnqueue(cx, unwrappedController, chunk);
}

JS_PUBLIC_API bool JS::ReadableStreamError(JSContext* cx,
                                           Handle<JSObject*> streamObj,
                                           Handle<Value> error) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(error);

  Rooted<ReadableStream*> unwrappedStream(
      cx, APIUnwrapAndDowncast<ReadableStream>(cx, streamObj));
  if (!unwrappedStream) {
    return false;
  }

  Rooted<ReadableStreamController*> unwrappedController(
      cx, unwrappedStream->controller());
  return ReadableStreamControllerError(cx, unwrappedController, error);
}

JS_PUBLIC_API bool JS::ReadableStreamReaderIsClosed(JSContext* cx,
                                                    Handle<JSObject*> readerObj,
                                                    bool* result) {
  Rooted<ReadableStreamReader*> unwrappedReader(
      cx, APIUnwrapAndDowncast<ReadableStreamReader>(cx, readerObj));
  if (!unwrappedReader) {
    return false;
  }

  *result = unwrappedReader->isClosed();
  return true;
}

JS_PUBLIC_API bool JS::ReadableStreamReaderCancel(JSContext* cx,
                                                  Handle<JSObject*> readerObj,
                                                  Handle<Value> reason) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(reason);

  Rooted<ReadableStreamReader*> unwrappedReader(
      cx, APIUnwrapAndDowncast<ReadableStreamReader>(cx, readerObj));
  if (!unwrappedReader) {
    return false;
  }
  MOZ_ASSERT(unwrappedReader->forAuthorCode() == ForAuthorCodeBool::No,
             "C++ code should not touch readers created by scripts");

  return ReadableStreamReaderGenericCancel(cx, unwrappedReader, reason);
}

JS_PUBLIC_API bool JS::ReadableStreamReaderReleaseLock(
    JSContext* cx, Handle<JSObject*> readerObj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  Rooted<ReadableStreamReader*> unwrappedReader(
      cx, APIUnwrapAndDowncast<ReadableStreamReader>(cx, readerObj));
  if (!unwrappedReader) {
    return false;
  }
  MOZ_ASSERT(unwrappedReader->forAuthorCode() == ForAuthorCodeBool::No,
             "C++ code should not touch readers created by scripts");

#ifdef DEBUG
  Rooted<ReadableStream*> unwrappedStream(
      cx, UnwrapStreamFromReader(cx, unwrappedReader));
  if (!unwrappedStream) {
    return false;
  }
  MOZ_ASSERT(ReadableStreamGetNumReadRequests(unwrappedStream) == 0);
#endif  // DEBUG

  return ReadableStreamReaderGenericRelease(cx, unwrappedReader);
}

JS_PUBLIC_API JSObject* JS::ReadableStreamDefaultReaderRead(
    JSContext* cx, Handle<JSObject*> readerObj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  Rooted<ReadableStreamDefaultReader*> unwrappedReader(
      cx, APIUnwrapAndDowncast<ReadableStreamDefaultReader>(cx, readerObj));
  if (!unwrappedReader) {
    return nullptr;
  }
  MOZ_ASSERT(unwrappedReader->forAuthorCode() == ForAuthorCodeBool::No,
             "C++ code should not touch readers created by scripts");

  return js::ReadableStreamDefaultReaderRead(cx, unwrappedReader);
}

void JS::InitPipeToHandling(const JSClass* abortSignalClass,
                            AbortSignalIsAborted isAborted, JSContext* cx) {
  cx->runtime()->initPipeToHandling(abortSignalClass, isAborted);
}
