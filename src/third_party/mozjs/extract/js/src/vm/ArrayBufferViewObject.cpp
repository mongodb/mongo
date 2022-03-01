/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ArrayBufferViewObject.h"

#include "builtin/DataViewObject.h"
#include "gc/Nursery.h"
#include "js/experimental/TypedData.h"  // JS_GetArrayBufferView{Data,Buffer,Length,ByteOffset}, JS_GetObjectAsArrayBufferView, JS_IsArrayBufferViewObject
#include "js/SharedArrayBuffer.h"
#include "vm/JSContext.h"
#include "vm/TypedArrayObject.h"

#include "gc/Nursery-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

/*
 * This method is used to trace TypedArrayObjects and DataViewObjects. We need
 * a custom tracer to move the object's data pointer if its owner was moved and
 * stores its data inline.
 */
/* static */
void ArrayBufferViewObject::trace(JSTracer* trc, JSObject* objArg) {
  ArrayBufferViewObject* obj = &objArg->as<ArrayBufferViewObject>();
  HeapSlot& bufSlot = obj->getFixedSlotRef(BUFFER_SLOT);
  TraceEdge(trc, &bufSlot, "ArrayBufferViewObject.buffer");

  // Update obj's data pointer if it moved.
  if (bufSlot.isObject()) {
    if (gc::MaybeForwardedObjectIs<ArrayBufferObject>(&bufSlot.toObject())) {
      ArrayBufferObject& buf =
          gc::MaybeForwardedObjectAs<ArrayBufferObject>(&bufSlot.toObject());
      size_t offset = obj->byteOffset();

      MOZ_ASSERT_IF(buf.dataPointer() == nullptr, offset == 0);

      // The data may or may not be inline with the buffer. The buffer
      // can only move during a compacting GC, in which case its
      // objectMoved hook has already updated the buffer's data pointer.
      size_t nfixed = obj->numFixedSlotsMaybeForwarded();
      obj->setPrivateUnbarriered(nfixed, buf.dataPointer() + offset);
    }
  }
}

template <>
bool JSObject::is<js::ArrayBufferViewObject>() const {
  return is<DataViewObject>() || is<TypedArrayObject>();
}

void ArrayBufferViewObject::notifyBufferDetached() {
  MOZ_ASSERT(!isSharedMemory());
  MOZ_ASSERT(hasBuffer());

  setFixedSlot(LENGTH_SLOT, PrivateValue(size_t(0)));
  setFixedSlot(BYTEOFFSET_SLOT, PrivateValue(size_t(0)));

  setPrivate(nullptr);
}

/* static */
ArrayBufferObjectMaybeShared* ArrayBufferViewObject::bufferObject(
    JSContext* cx, Handle<ArrayBufferViewObject*> thisObject) {
  if (thisObject->is<TypedArrayObject>()) {
    Rooted<TypedArrayObject*> typedArray(cx,
                                         &thisObject->as<TypedArrayObject>());
    if (!TypedArrayObject::ensureHasBuffer(cx, typedArray)) {
      return nullptr;
    }
  }
  return thisObject->bufferEither();
}

bool ArrayBufferViewObject::init(JSContext* cx,
                                 ArrayBufferObjectMaybeShared* buffer,
                                 size_t byteOffset, size_t length,
                                 uint32_t bytesPerElement) {
  MOZ_ASSERT_IF(!buffer, byteOffset == 0);
  MOZ_ASSERT_IF(buffer, !buffer->isDetached());

  MOZ_ASSERT(byteOffset <= ArrayBufferObject::maxBufferByteLength());
  MOZ_ASSERT(length <= ArrayBufferObject::maxBufferByteLength());
  MOZ_ASSERT(byteOffset + length <= ArrayBufferObject::maxBufferByteLength());

  MOZ_ASSERT_IF(is<TypedArrayObject>(),
                length <= TypedArrayObject::maxByteLength() / bytesPerElement);

  // The isSharedMemory property is invariant.  Self-hosting code that
  // sets BUFFER_SLOT or the private slot (if it does) must maintain it by
  // always setting those to reference shared memory.
  if (buffer && buffer->is<SharedArrayBufferObject>()) {
    setIsSharedMemory();
  }

  initFixedSlot(BYTEOFFSET_SLOT, PrivateValue(byteOffset));
  initFixedSlot(LENGTH_SLOT, PrivateValue(length));
  initFixedSlot(BUFFER_SLOT, ObjectOrNullValue(buffer));

  if (buffer) {
    SharedMem<uint8_t*> ptr = buffer->dataPointerEither();
    initDataPointer(ptr + byteOffset);

    // Only ArrayBuffers used for inline typed objects can have
    // nursery-allocated data and we shouldn't see such buffers here.
    MOZ_ASSERT_IF(buffer->byteLength() > 0, !cx->nursery().isInside(ptr));
  } else {
    MOZ_ASSERT(is<TypedArrayObject>());
    MOZ_ASSERT(length * bytesPerElement <=
               TypedArrayObject::INLINE_BUFFER_LIMIT);
    void* data = fixedData(TypedArrayObject::FIXED_DATA_START);
    initPrivate(data);
    memset(data, 0, length * bytesPerElement);
#ifdef DEBUG
    if (length == 0) {
      uint8_t* elements = static_cast<uint8_t*>(data);
      elements[0] = ZeroLengthArrayData;
    }
#endif
  }

#ifdef DEBUG
  if (buffer) {
    size_t viewByteLength = length * bytesPerElement;
    size_t viewByteOffset = byteOffset;
    size_t bufferByteLength = buffer->byteLength();
    // Unwraps are safe: both are for the pointer value.
    MOZ_ASSERT_IF(buffer->is<ArrayBufferObject>(),
                  buffer->dataPointerEither().unwrap(/*safe*/) <=
                      dataPointerEither().unwrap(/*safe*/));
    MOZ_ASSERT(bufferByteLength - viewByteOffset >= viewByteLength);
    MOZ_ASSERT(viewByteOffset <= bufferByteLength);
  }

  // Verify that the private slot is at the expected place.
  MOZ_ASSERT(numFixedSlots() == DATA_SLOT);
#endif

  // ArrayBufferObjects track their views to support detaching.
  if (buffer && buffer->is<ArrayBufferObject>()) {
    if (!buffer->as<ArrayBufferObject>().addView(cx, this)) {
      return false;
    }
  }

  return true;
}

/* JS Public API */

JS_PUBLIC_API bool JS_IsArrayBufferViewObject(JSObject* obj) {
  return obj->canUnwrapAs<ArrayBufferViewObject>();
}

JS_PUBLIC_API JSObject* js::UnwrapArrayBufferView(JSObject* obj) {
  return obj->maybeUnwrapIf<ArrayBufferViewObject>();
}

JS_PUBLIC_API void* JS_GetArrayBufferViewData(JSObject* obj,
                                              bool* isSharedMemory,
                                              const JS::AutoRequireNoGC&) {
  ArrayBufferViewObject* view = obj->maybeUnwrapAs<ArrayBufferViewObject>();
  if (!view) {
    return nullptr;
  }

  *isSharedMemory = view->isSharedMemory();
  return view->dataPointerEither().unwrap(
      /*safe - caller sees isSharedMemory flag*/);
}

JS_PUBLIC_API uint8_t* JS_GetArrayBufferViewFixedData(JSObject* obj,
                                                      uint8_t* buffer,
                                                      size_t bufSize) {
  ArrayBufferViewObject* view = obj->maybeUnwrapAs<ArrayBufferViewObject>();
  if (!view) {
    return nullptr;
  }

  // Disallow shared memory until it is needed.
  if (view->isSharedMemory()) {
    return nullptr;
  }

  // TypedArrays (but not DataViews) can have inline data, in which case we
  // need to copy into the given buffer.
  if (view->is<TypedArrayObject>()) {
    TypedArrayObject* ta = &view->as<TypedArrayObject>();
    if (ta->hasInlineElements()) {
      size_t bytes = ta->byteLength();
      if (bytes > bufSize) {
        return nullptr;  // Does not fit.
      }
      memcpy(buffer, view->dataPointerUnshared(), bytes);
      return buffer;
    }
  }

  return static_cast<uint8_t*>(view->dataPointerUnshared());
}

JS_PUBLIC_API JSObject* JS_GetArrayBufferViewBuffer(JSContext* cx,
                                                    HandleObject obj,
                                                    bool* isSharedMemory) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  Rooted<ArrayBufferViewObject*> unwrappedView(
      cx, obj->maybeUnwrapAs<ArrayBufferViewObject>());
  if (!unwrappedView) {
    ReportAccessDenied(cx);
    return nullptr;
  }

  ArrayBufferObjectMaybeShared* unwrappedBuffer;
  {
    AutoRealm ar(cx, unwrappedView);
    unwrappedBuffer = ArrayBufferViewObject::bufferObject(cx, unwrappedView);
    if (!unwrappedBuffer) {
      return nullptr;
    }
  }
  *isSharedMemory = unwrappedBuffer->is<SharedArrayBufferObject>();

  RootedObject buffer(cx, unwrappedBuffer);
  if (!cx->compartment()->wrap(cx, &buffer)) {
    return nullptr;
  }

  return buffer;
}

JS_PUBLIC_API size_t JS_GetArrayBufferViewByteLength(JSObject* obj) {
  obj = obj->maybeUnwrapAs<ArrayBufferViewObject>();
  if (!obj) {
    return 0;
  }
  size_t length = obj->is<DataViewObject>()
                      ? obj->as<DataViewObject>().byteLength()
                      : obj->as<TypedArrayObject>().byteLength();
  return length;
}

JS_PUBLIC_API size_t JS_GetArrayBufferViewByteOffset(JSObject* obj) {
  obj = obj->maybeUnwrapAs<ArrayBufferViewObject>();
  if (!obj) {
    return 0;
  }
  size_t offset = obj->is<DataViewObject>()
                      ? obj->as<DataViewObject>().byteOffset()
                      : obj->as<TypedArrayObject>().byteOffset();
  return offset;
}

JS_PUBLIC_API JSObject* JS_GetObjectAsArrayBufferView(JSObject* obj,
                                                      size_t* length,
                                                      bool* isSharedMemory,
                                                      uint8_t** data) {
  obj = obj->maybeUnwrapIf<ArrayBufferViewObject>();
  if (!obj) {
    return nullptr;
  }

  js::GetArrayBufferViewLengthAndData(obj, length, isSharedMemory, data);
  return obj;
}

JS_PUBLIC_API void js::GetArrayBufferViewLengthAndData(JSObject* obj,
                                                       size_t* length,
                                                       bool* isSharedMemory,
                                                       uint8_t** data) {
  MOZ_ASSERT(obj->is<ArrayBufferViewObject>());

  size_t byteLength = obj->is<DataViewObject>()
                          ? obj->as<DataViewObject>().byteLength()
                          : obj->as<TypedArrayObject>().byteLength();
  *length = byteLength;

  ArrayBufferViewObject& view = obj->as<ArrayBufferViewObject>();
  *isSharedMemory = view.isSharedMemory();
  *data = static_cast<uint8_t*>(
      view.dataPointerEither().unwrap(/*safe - caller sees isShared flag*/));
}

JS_PUBLIC_API bool JS::IsArrayBufferViewShared(JSObject* obj) {
  ArrayBufferViewObject* view = obj->maybeUnwrapAs<ArrayBufferViewObject>();
  if (!view) {
    return false;
  }
  return view->isSharedMemory();
}

JS_PUBLIC_API bool JS::IsLargeArrayBufferView(JSObject* obj) {
#ifdef JS_64BIT
  obj = &obj->unwrapAs<ArrayBufferViewObject>();
  size_t len = obj->is<DataViewObject>()
                   ? obj->as<DataViewObject>().byteLength()
                   : obj->as<TypedArrayObject>().byteLength();
  return len > ArrayBufferObject::MaxByteLengthForSmallBuffer;
#else
  // Large ArrayBuffers are not supported on 32-bit.
  MOZ_ASSERT(ArrayBufferObject::maxBufferByteLength() ==
             ArrayBufferObject::MaxByteLengthForSmallBuffer);
  return false;
#endif
}
