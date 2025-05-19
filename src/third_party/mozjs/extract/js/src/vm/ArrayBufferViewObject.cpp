/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ArrayBufferViewObject.h"

#include "builtin/DataViewObject.h"
#include "gc/Nursery.h"
#include "js/ErrorReport.h"
#include "js/experimental/TypedData.h"  // JS_GetArrayBufferView{Data,Buffer,Length,ByteOffset}, JS_GetObjectAsArrayBufferView, JS_IsArrayBufferViewObject
#include "js/SharedArrayBuffer.h"
#include "vm/Compartment.h"
#include "vm/JSContext.h"
#include "vm/TypedArrayObject.h"

#include "gc/Nursery-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

// This method is used to trace TypedArrayObjects and DataViewObjects. It
// updates the object's data pointer if it points to inline data in an object
// that was moved.
/* static */
void ArrayBufferViewObject::trace(JSTracer* trc, JSObject* obj) {
  ArrayBufferViewObject* view = &obj->as<ArrayBufferViewObject>();

  // Update view's data pointer if it moved.
  if (view->hasBuffer()) {
    JSObject* bufferObj = &view->bufferValue().toObject();
    ArrayBufferObject* buffer = nullptr;
    if (gc::MaybeForwardedObjectIs<FixedLengthArrayBufferObject>(bufferObj)) {
      buffer =
          &gc::MaybeForwardedObjectAs<FixedLengthArrayBufferObject>(bufferObj);
    } else if (gc::MaybeForwardedObjectIs<ResizableArrayBufferObject>(
                   bufferObj)) {
      buffer =
          &gc::MaybeForwardedObjectAs<ResizableArrayBufferObject>(bufferObj);
    }
    if (buffer) {
      size_t offset = view->dataPointerOffset();
      MOZ_ASSERT_IF(!buffer->dataPointer(), offset == 0);

      // The data may or may not be inline with the buffer. The buffer can only
      // move during a compacting GC, in which case its objectMoved hook has
      // already updated the buffer's data pointer.
      view->notifyBufferMoved(
          static_cast<uint8_t*>(view->dataPointerEither_()) - offset,
          buffer->dataPointer());
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
  MOZ_ASSERT(!bufferUnshared()->isLengthPinned());

  setFixedSlot(LENGTH_SLOT, PrivateValue(size_t(0)));
  setFixedSlot(BYTEOFFSET_SLOT, PrivateValue(size_t(0)));
  setFixedSlot(DATA_SLOT, UndefinedValue());
}

void ArrayBufferViewObject::notifyBufferResized() {
  MOZ_ASSERT(!isSharedMemory());
  MOZ_ASSERT(hasBuffer());
  MOZ_ASSERT(!bufferUnshared()->isLengthPinned());
  MOZ_ASSERT(bufferUnshared()->isResizable());

  computeResizableLengthAndByteOffset(bytesPerElement());
}

void ArrayBufferViewObject::notifyBufferMoved(uint8_t* srcBufStart,
                                              uint8_t* dstBufStart) {
  MOZ_ASSERT(!isSharedMemory());
  MOZ_ASSERT(hasBuffer());

  if (srcBufStart != dstBufStart) {
    void* data = dstBufStart + dataPointerOffset();
    getFixedSlotRef(DATA_SLOT).unbarrieredSet(PrivateValue(data));
  }
}

/* static */
bool ArrayBufferViewObject::ensureNonInline(
    JSContext* cx, Handle<ArrayBufferViewObject*> view) {
  MOZ_ASSERT(!view->isSharedMemory());
  // Create an ArrayBuffer for the data if it was in the view.
  ArrayBufferObjectMaybeShared* buffer = ensureBufferObject(cx, view);
  if (!buffer) {
    return false;
  }
  Rooted<ArrayBufferObject*> unsharedBuffer(cx,
                                            &buffer->as<ArrayBufferObject>());
  return ArrayBufferObject::ensureNonInline(cx, unsharedBuffer);
}

/* static */
ArrayBufferObjectMaybeShared* ArrayBufferViewObject::ensureBufferObject(
    JSContext* cx, Handle<ArrayBufferViewObject*> thisObject) {
  if (thisObject->is<TypedArrayObject>()) {
    Rooted<TypedArrayObject*> typedArray(cx,
                                         &thisObject->as<TypedArrayObject>());
    if (!TypedArrayObject::ensureHasBuffer(cx, typedArray)) {
      return nullptr;
    }
  }
  auto* buffer = thisObject->bufferEither();
  if (!buffer) {
    MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "ABV has no buffer");
  }
  return buffer;
}

bool ArrayBufferViewObject::init(JSContext* cx,
                                 ArrayBufferObjectMaybeShared* buffer,
                                 size_t byteOffset, size_t length,
                                 uint32_t bytesPerElement) {
  MOZ_ASSERT_IF(!buffer, byteOffset == 0);
  MOZ_ASSERT_IF(buffer, !buffer->isDetached());

  MOZ_ASSERT(byteOffset <= ArrayBufferObject::ByteLengthLimit);
  MOZ_ASSERT(length <= ArrayBufferObject::ByteLengthLimit);
  MOZ_ASSERT(byteOffset + length <= ArrayBufferObject::ByteLengthLimit);

  MOZ_ASSERT_IF(is<TypedArrayObject>(),
                length <= TypedArrayObject::ByteLengthLimit / bytesPerElement);

  // The isSharedMemory property is invariant.  Self-hosting code that
  // sets BUFFER_SLOT or the private slot (if it does) must maintain it by
  // always setting those to reference shared memory.
  if (buffer && buffer->is<SharedArrayBufferObject>()) {
    setIsSharedMemory();
  }

  initFixedSlot(BYTEOFFSET_SLOT, PrivateValue(byteOffset));
  initFixedSlot(LENGTH_SLOT, PrivateValue(length));
  if (buffer) {
    initFixedSlot(BUFFER_SLOT, ObjectValue(*buffer));
  } else {
    MOZ_ASSERT(!isSharedMemory());
    initFixedSlot(BUFFER_SLOT, JS::FalseValue());
  }

  if (buffer) {
    SharedMem<uint8_t*> ptr = buffer->dataPointerEither();
    initDataPointer(ptr + byteOffset);

    // Only ArrayBuffers used for inline typed objects can have
    // nursery-allocated data and we shouldn't see such buffers here.
    MOZ_ASSERT_IF(buffer->byteLength() > 0, !cx->nursery().isInside(ptr));
  } else {
    MOZ_ASSERT(is<FixedLengthTypedArrayObject>());
    MOZ_ASSERT(length * bytesPerElement <=
               FixedLengthTypedArrayObject::INLINE_BUFFER_LIMIT);
    void* data = fixedData(FixedLengthTypedArrayObject::FIXED_DATA_START);
    initReservedSlot(DATA_SLOT, PrivateValue(data));
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
#endif

  // ArrayBufferObjects track their views to support detaching.
  if (buffer && buffer->is<ArrayBufferObject>()) {
    if (!buffer->as<ArrayBufferObject>().addView(cx, this)) {
      return false;
    }
  }

  return true;
}

bool ArrayBufferViewObject::initResizable(JSContext* cx,
                                          ArrayBufferObjectMaybeShared* buffer,
                                          size_t byteOffset, size_t length,
                                          uint32_t bytesPerElement,
                                          AutoLength autoLength) {
  MOZ_ASSERT(buffer->isResizable());

  initFixedSlot(AUTO_LENGTH_SLOT, BooleanValue(static_cast<bool>(autoLength)));
  initFixedSlot(INITIAL_LENGTH_SLOT, PrivateValue(length));
  initFixedSlot(INITIAL_BYTE_OFFSET_SLOT, PrivateValue(byteOffset));

  if (!init(cx, buffer, byteOffset, length, bytesPerElement)) {
    return false;
  }

  // Compute the actual byteLength and byteOffset for non-shared buffers.
  if (!isSharedMemory()) {
    computeResizableLengthAndByteOffset(bytesPerElement);
  }

  MOZ_ASSERT(!isOutOfBounds(), "can't create out-of-bounds views");

  return true;
}

void ArrayBufferViewObject::computeResizableLengthAndByteOffset(
    size_t bytesPerElement) {
  MOZ_ASSERT(!isSharedMemory());
  MOZ_ASSERT(hasBuffer());
  MOZ_ASSERT(bufferUnshared()->isResizable());

  size_t byteOffsetStart = initialByteOffset();
  size_t bufferByteLength = bufferUnshared()->byteLength();

  // Out-of-bounds if the byteOffset exceeds the buffer length.
  if (byteOffsetStart > bufferByteLength) {
    setFixedSlot(LENGTH_SLOT, PrivateValue(size_t(0)));
    setFixedSlot(BYTEOFFSET_SLOT, PrivateValue(size_t(0)));
    return;
  }

  size_t length;
  if (isAutoLength()) {
    length = (bufferByteLength - byteOffsetStart) / bytesPerElement;
  } else {
    length = initialLength();

    // Out-of-bounds if the byteOffset end index exceeds the buffer length.
    size_t byteOffsetEnd = byteOffsetStart + length * bytesPerElement;
    if (byteOffsetEnd > bufferByteLength) {
      setFixedSlot(LENGTH_SLOT, PrivateValue(size_t(0)));
      setFixedSlot(BYTEOFFSET_SLOT, PrivateValue(size_t(0)));
      return;
    }
  }

  setFixedSlot(LENGTH_SLOT, PrivateValue(length));
  setFixedSlot(BYTEOFFSET_SLOT, PrivateValue(byteOffsetStart));
}

size_t ArrayBufferViewObject::bytesPerElement() const {
  if (is<TypedArrayObject>()) {
    return as<TypedArrayObject>().bytesPerElement();
  }

  MOZ_ASSERT(is<DataViewObject>());
  return 1;
}

bool ArrayBufferViewObject::hasResizableBuffer() const {
  if (auto* buffer = bufferEither()) {
    return buffer->isResizable();
  }
  return false;
}

size_t ArrayBufferViewObject::dataPointerOffset() const {
  // Views without a buffer have a zero offset.
  if (!hasBuffer()) {
    MOZ_ASSERT(byteOffsetSlotValue() == 0);
    return 0;
  }

  // Views on shared buffers store the offset in |byteOffset|.
  if (isSharedMemory()) {
    return byteOffsetSlotValue();
  }

  // Can be called during tracing, so the buffer is possibly forwarded.
  const auto* bufferObj = gc::MaybeForwarded(&bufferValue().toObject());

  // Two distinct classes are used for non-shared buffers.
  MOZ_ASSERT(
      gc::MaybeForwardedObjectIs<FixedLengthArrayBufferObject>(bufferObj) ||
      gc::MaybeForwardedObjectIs<ResizableArrayBufferObject>(bufferObj));

  // Ensure these two classes can be casted to ArrayBufferObject.
  static_assert(
      std::is_base_of_v<ArrayBufferObject, FixedLengthArrayBufferObject>);
  static_assert(
      std::is_base_of_v<ArrayBufferObject, ResizableArrayBufferObject>);

  // Manual cast necessary because the buffer is possibly forwarded.
  const auto* buffer = static_cast<const ArrayBufferObject*>(bufferObj);

  // Views on resizable buffers store the offset in |initialByteOffset|.
  if (buffer->isResizable() && !buffer->isDetached()) {
    return initialByteOffsetValue();
  }

  // Callers expect that this method returns zero for detached buffers.
  MOZ_ASSERT_IF(buffer->isDetached(), byteOffsetSlotValue() == 0);

  // Views on fixed-length buffers store the offset in |byteOffset|.
  return byteOffsetSlotValue();
}

mozilla::Maybe<size_t> ArrayBufferViewObject::byteOffset() const {
  // |byteOffset| is set to zero for detached or out-of-bounds views, so a
  // non-zero value indicates the view is in-bounds.
  size_t byteOffset = byteOffsetSlotValue();
  if (byteOffset > 0) {
    MOZ_ASSERT(!hasDetachedBuffer());
    MOZ_ASSERT_IF(hasResizableBuffer(), !isOutOfBounds());
    return mozilla::Some(byteOffset);
  }
  if (hasDetachedBufferOrIsOutOfBounds()) {
    return mozilla::Nothing{};
  }
  return mozilla::Some(0);
}

mozilla::Maybe<size_t> ArrayBufferViewObject::length() const {
  // |length| is set to zero for detached or out-of-bounds views, so a non-zero
  // value indicates the view is in-bounds.
  size_t length = lengthSlotValue();
  if (MOZ_LIKELY(length > 0)) {
    MOZ_ASSERT(!hasDetachedBuffer());
    MOZ_ASSERT_IF(hasResizableBuffer(), !isOutOfBounds());
    MOZ_ASSERT(!isSharedMemory() || !hasResizableBuffer() || !isAutoLength(),
               "length is zero for auto-length growable shared buffers");
    return mozilla::Some(length);
  }

  if (hasDetachedBufferOrIsOutOfBounds()) {
    return mozilla::Nothing{};
  }

  if (isSharedMemory()) {
    auto* buffer = bufferShared();
    MOZ_ASSERT(buffer, "shared memory doesn't use inline data");

    // Views backed by a growable SharedArrayBuffer can never get out-of-bounds,
    // but we have to dynamically compute the length when the auto-length flag
    // is set.
    if (buffer->isGrowable() && isAutoLength()) {
      size_t bufferByteLength = buffer->byteLength();
      size_t byteOffset = byteOffsetSlotValue();
      MOZ_ASSERT(byteOffset <= bufferByteLength);
      MOZ_ASSERT(byteOffset == initialByteOffset(),
                 "views on growable shared buffers can't get out-of-bounds");

      return mozilla::Some((bufferByteLength - byteOffset) / bytesPerElement());
    }
  }
  return mozilla::Some(0);
}

#if defined(DEBUG) || defined(JS_JITSPEW)
void ArrayBufferViewObject::dumpOwnFields(js::JSONPrinter& json) const {
  json.formatProperty("length", "%zu",
                      size_t(getFixedSlot(LENGTH_SLOT).toPrivate()));
  json.formatProperty("byteOffset", "%zu",
                      size_t(getFixedSlot(BYTEOFFSET_SLOT).toPrivate()));
  void* data = dataPointerEither_();
  if (data) {
    json.formatProperty("data", "0x%p", data);
  } else {
    json.nullProperty("data");
  }
}

void ArrayBufferViewObject::dumpOwnStringContent(
    js::GenericPrinter& out) const {
  out.printf("length=%zu, byteOffset=%zu, ",
             size_t(getFixedSlot(LENGTH_SLOT).toPrivate()),
             size_t(getFixedSlot(BYTEOFFSET_SLOT).toPrivate()));
  void* data = dataPointerEither_();
  if (data) {
    out.printf("data=0x%p", data);
  } else {
    out.put("data=null");
  }
}
#endif

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
  if (view->is<FixedLengthTypedArrayObject>()) {
    auto* ta = &view->as<FixedLengthTypedArrayObject>();
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
    MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "access to buffer denied");
    ReportAccessDenied(cx);
    return nullptr;
  }

  ArrayBufferObjectMaybeShared* unwrappedBuffer;
  {
    AutoRealm ar(cx, unwrappedView);
    unwrappedBuffer =
        ArrayBufferViewObject::ensureBufferObject(cx, unwrappedView);
    if (!unwrappedBuffer) {
      return nullptr;
    }
  }
  *isSharedMemory = unwrappedBuffer->is<SharedArrayBufferObject>();

  RootedObject buffer(cx, unwrappedBuffer);
  if (!cx->compartment()->wrap(cx, &buffer)) {
    MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "wrapping buffer failed");
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
                      ? obj->as<DataViewObject>().byteLength().valueOr(0)
                      : obj->as<TypedArrayObject>().byteLength().valueOr(0);
  return length;
}

bool JS::ArrayBufferView::isDetached() const {
  MOZ_ASSERT(obj);
  return obj->as<ArrayBufferViewObject>().hasDetachedBuffer();
}

bool JS::ArrayBufferView::isResizable() const {
  MOZ_ASSERT(obj);
  return obj->as<ArrayBufferViewObject>().hasResizableBuffer();
}

JS_PUBLIC_API size_t JS_GetArrayBufferViewByteOffset(JSObject* obj) {
  obj = obj->maybeUnwrapAs<ArrayBufferViewObject>();
  if (!obj) {
    return 0;
  }
  size_t offset = obj->is<DataViewObject>()
                      ? obj->as<DataViewObject>().byteOffset().valueOr(0)
                      : obj->as<TypedArrayObject>().byteOffset().valueOr(0);
  return offset;
}

JS_PUBLIC_API mozilla::Span<uint8_t> JS::ArrayBufferView::getData(
    bool* isSharedMemory, const AutoRequireNoGC&) {
  MOZ_ASSERT(obj->is<ArrayBufferViewObject>());
  size_t byteLength = obj->is<DataViewObject>()
                          ? obj->as<DataViewObject>().byteLength().valueOr(0)
                          : obj->as<TypedArrayObject>().byteLength().valueOr(0);
  ArrayBufferViewObject& view = obj->as<ArrayBufferViewObject>();
  *isSharedMemory = view.isSharedMemory();
  return {static_cast<uint8_t*>(view.dataPointerEither().unwrap(
              /*safe - caller sees isShared flag*/)),
          byteLength};
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
  JS::AutoAssertNoGC nogc;
  auto span =
      JS::ArrayBufferView::fromObject(obj).getData(isSharedMemory, nogc);
  *data = span.data();
  *length = span.Length();
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
                   ? obj->as<DataViewObject>().byteLength().valueOr(0)
                   : obj->as<TypedArrayObject>().byteLength().valueOr(0);
  return len > ArrayBufferObject::ByteLengthLimitForSmallBuffer;
#else
  // Large ArrayBuffers are not supported on 32-bit.
  static_assert(ArrayBufferObject::ByteLengthLimit ==
                ArrayBufferObject::ByteLengthLimitForSmallBuffer);
  return false;
#endif
}

JS_PUBLIC_API bool JS::IsResizableArrayBufferView(JSObject* obj) {
  auto* view = &obj->unwrapAs<ArrayBufferViewObject>();
  if (auto* buffer = view->bufferEither()) {
    return buffer->isResizable();
  }
  return false;
}

JS_PUBLIC_API bool JS::PinArrayBufferOrViewLength(JSObject* obj, bool pin) {
  ArrayBufferObjectMaybeShared* buffer =
      obj->maybeUnwrapIf<ArrayBufferObjectMaybeShared>();
  if (buffer) {
    return buffer->pinLength(pin);
  }

  ArrayBufferViewObject* view = obj->maybeUnwrapAs<ArrayBufferViewObject>();
  if (view) {
    return view->pinLength(pin);
  }

  MOZ_DIAGNOSTIC_ASSERT(!js::TlsContext.get()->brittleMode,
                        "invalid type in PinABOVLength");
  return false;
}

JS_PUBLIC_API bool JS::EnsureNonInlineArrayBufferOrView(JSContext* cx,
                                                        JSObject* obj) {
  if (obj->is<SharedArrayBufferObject>()) {
    // Always locked and out of line.
    return true;
  }

  auto* buffer = obj->maybeUnwrapIf<ArrayBufferObject>();
  if (buffer) {
    Rooted<ArrayBufferObject*> rootedBuffer(cx, buffer);
    return ArrayBufferObject::ensureNonInline(cx, rootedBuffer);
  }

  auto* view = obj->maybeUnwrapIf<ArrayBufferViewObject>();
  if (view) {
    if (view->isSharedMemory()) {
      // Always locked and out of line.
      return true;
    }
    Rooted<ArrayBufferViewObject*> rootedView(cx, view);
    return ArrayBufferViewObject::ensureNonInline(cx, rootedView);
  }

  MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "unhandled type");
  JS_ReportErrorASCII(cx, "unhandled type");
  return false;
}
