/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayBufferViewObject_h
#define vm_ArrayBufferViewObject_h

#include "mozilla/Maybe.h"

#include "builtin/TypedArrayConstants.h"
#include "vm/ArrayBufferObject.h"
#include "vm/NativeObject.h"
#include "vm/SharedArrayObject.h"
#include "vm/SharedMem.h"

namespace js {

class JS_PUBLIC_API GenericPrinter;
class JSONPrinter;

/*
 * ArrayBufferViewObject
 *
 * Common base class for all array buffer views (DataViewObject and
 * TypedArrayObject).
 */

class ArrayBufferViewObject : public NativeObject {
 public:
  // Underlying (Shared)ArrayBufferObject. ObjectValue if there is
  // a buffer. Otherwise, the buffer is implicit because the data
  // is held inline, and the buffer slot will store the pinned status
  // (FalseValue or TrueValue).
  static constexpr size_t BUFFER_SLOT = 0;
  static_assert(BUFFER_SLOT == JS_TYPEDARRAYLAYOUT_BUFFER_SLOT,
                "self-hosted code with burned-in constants must get the "
                "right buffer slot");

  // Slot containing length of the view in number of typed elements.
  static constexpr size_t LENGTH_SLOT = 1;

  // Offset of view within underlying (Shared)ArrayBufferObject.
  static constexpr size_t BYTEOFFSET_SLOT = 2;

  // Pointer to raw buffer memory.
  static constexpr size_t DATA_SLOT = 3;

  static constexpr size_t RESERVED_SLOTS = 4;

  // Additional slots for views on resizable/growable (Shared)ArrayBufferObject.

  static const uint8_t AUTO_LENGTH_SLOT = 4;
  static const uint8_t INITIAL_LENGTH_SLOT = 5;
  static const uint8_t INITIAL_BYTE_OFFSET_SLOT = 6;

  static constexpr size_t RESIZABLE_RESERVED_SLOTS = 7;

#ifdef DEBUG
  static const uint8_t ZeroLengthArrayData = 0x4A;
#endif

  static constexpr int bufferOffset() {
    return NativeObject::getFixedSlotOffset(BUFFER_SLOT);
  }
  static constexpr int lengthOffset() {
    return NativeObject::getFixedSlotOffset(LENGTH_SLOT);
  }
  static constexpr int byteOffsetOffset() {
    return NativeObject::getFixedSlotOffset(BYTEOFFSET_SLOT);
  }
  static constexpr int dataOffset() {
    return NativeObject::getFixedSlotOffset(DATA_SLOT);
  }
  static constexpr int autoLengthOffset() {
    return NativeObject::getFixedSlotOffset(AUTO_LENGTH_SLOT);
  }
  static constexpr int initialLengthOffset() {
    return NativeObject::getFixedSlotOffset(INITIAL_LENGTH_SLOT);
  }
  static constexpr int initialByteOffsetOffset() {
    return NativeObject::getFixedSlotOffset(INITIAL_BYTE_OFFSET_SLOT);
  }

 private:
  void* dataPointerEither_() const {
    // Note, do not check whether shared or not
    // Keep synced with js::Get<Type>ArrayLengthAndData in jsfriendapi.h!
    return maybePtrFromReservedSlot<void>(DATA_SLOT);
  }

 public:
  [[nodiscard]] bool init(JSContext* cx, ArrayBufferObjectMaybeShared* buffer,
                          size_t byteOffset, size_t length,
                          uint32_t bytesPerElement);

  enum class AutoLength : bool { No, Yes };

  [[nodiscard]] bool initResizable(JSContext* cx,
                                   ArrayBufferObjectMaybeShared* buffer,
                                   size_t byteOffset, size_t length,
                                   uint32_t bytesPerElement,
                                   AutoLength autoLength);

  static ArrayBufferObjectMaybeShared* ensureBufferObject(
      JSContext* cx, Handle<ArrayBufferViewObject*> obj);

  void notifyBufferDetached();
  void notifyBufferResized();
  void notifyBufferMoved(uint8_t* srcBufStart, uint8_t* dstBufStart);

  void initDataPointer(SharedMem<uint8_t*> viewData) {
    // Install a pointer to the buffer location that corresponds
    // to offset zero within the typed array.
    //
    // The following unwrap is safe because the DATA_SLOT is
    // accessed only from jitted code and from the
    // dataPointerEither_() accessor above; in neither case does the
    // raw pointer escape untagged into C++ code.
    void* data = viewData.unwrap(/*safe - see above*/);
    initReservedSlot(DATA_SLOT, PrivateValue(data));
  }

  SharedMem<void*> dataPointerShared() const {
    return SharedMem<void*>::shared(dataPointerEither_());
  }
  SharedMem<void*> dataPointerEither() const {
    if (isSharedMemory()) {
      return SharedMem<void*>::shared(dataPointerEither_());
    }
    return SharedMem<void*>::unshared(dataPointerEither_());
  }
  void* dataPointerUnshared() const {
    MOZ_ASSERT(!isSharedMemory());
    return dataPointerEither_();
  }

  Value bufferValue() const { return getFixedSlot(BUFFER_SLOT); }
  bool hasBuffer() const { return bufferValue().isObject(); }

  ArrayBufferObject* bufferUnshared() const {
    MOZ_ASSERT(!isSharedMemory());
    ArrayBufferObjectMaybeShared* obj = bufferEither();
    if (!obj) {
      return nullptr;
    }
    return &obj->as<ArrayBufferObject>();
  }
  SharedArrayBufferObject* bufferShared() const {
    MOZ_ASSERT(isSharedMemory());
    ArrayBufferObjectMaybeShared* obj = bufferEither();
    if (!obj) {
      return nullptr;
    }
    return &obj->as<SharedArrayBufferObject>();
  }
  ArrayBufferObjectMaybeShared* bufferEither() const {
    JSObject* obj =
        bufferValue().isBoolean() ? nullptr : bufferValue().toObjectOrNull();
    if (!obj) {
      return nullptr;
    }
    MOZ_ASSERT(isSharedMemory() ? obj->is<SharedArrayBufferObject>()
                                : obj->is<ArrayBufferObject>());
    return &obj->as<ArrayBufferObjectMaybeShared>();
  }

  bool hasDetachedBuffer() const {
    // Shared buffers can't be detached.
    if (isSharedMemory()) {
      return false;
    }

    // A typed array with a null buffer has never had its buffer exposed to
    // become detached.
    ArrayBufferObject* buffer = bufferUnshared();
    if (!buffer) {
      return false;
    }

    return buffer->isDetached();
  }

  bool hasResizableBuffer() const;

 private:
  bool hasDetachedBufferOrIsOutOfBounds() const {
    // Shared buffers can't be detached or get out-of-bounds.
    if (isSharedMemory()) {
      return false;
    }

    // A view with a null buffer has never had its buffer exposed to become
    // detached or get out-of-bounds.
    auto* buffer = bufferUnshared();
    if (!buffer) {
      return false;
    }

    return buffer->isDetached() || (buffer->isResizable() && isOutOfBounds());
  }

 public:
  bool isLengthPinned() const {
    Value buffer = bufferValue();
    if (buffer.isBoolean()) {
      return buffer.toBoolean();
    }
    if (isSharedMemory()) {
      return true;
    }
    return bufferUnshared()->isLengthPinned();
  }

  bool pinLength(bool pin) {
    if (isSharedMemory()) {
      // Always pinned, cannot change.
      return false;
    }

    if (hasBuffer()) {
      return bufferUnshared()->pinLength(pin);
    }

    // No ArrayBuffer (data is inline in the view). bufferValue() is a
    // BooleanValue saying whether the length is currently pinned.
    MOZ_ASSERT(bufferValue().isBoolean());

    bool wasPinned = bufferValue().toBoolean();
    if (wasPinned == pin) {
      return false;
    }

    setFixedSlot(BUFFER_SLOT, JS::BooleanValue(pin));
    return true;
  }

  static bool ensureNonInline(JSContext* cx,
                              JS::Handle<ArrayBufferViewObject*> view);

 private:
  void computeResizableLengthAndByteOffset(size_t bytesPerElement);

  size_t bytesPerElement() const;

 protected:
  size_t lengthSlotValue() const {
    return size_t(getFixedSlot(LENGTH_SLOT).toPrivate());
  }

  size_t byteOffsetSlotValue() const {
    return size_t(getFixedSlot(BYTEOFFSET_SLOT).toPrivate());
  }

  /**
   * Offset into the buffer's data-pointer. Different from |byteOffset| for
   * views on non-detached resizable buffers which are currently out-of-bounds.
   */
  size_t dataPointerOffset() const;

  /**
   * Return the current length, or |Nothing| if the view is detached or
   * out-of-bounds.
   */
  mozilla::Maybe<size_t> length() const;

 public:
  /**
   * Return the current byteOffset, or |Nothing| if the view is detached or
   * out-of-bounds.
   */
  mozilla::Maybe<size_t> byteOffset() const;

 private:
  size_t initialByteOffsetValue() const {
    // No assertion for resizable buffers here, because this method is called
    // from dataPointerOffset(), which can be called during tracing.
    return size_t(getFixedSlot(INITIAL_BYTE_OFFSET_SLOT).toPrivate());
  }

 public:
  // The following methods can only be called on views for resizable buffers.

  bool isAutoLength() const {
    MOZ_ASSERT(hasResizableBuffer());
    return getFixedSlot(AUTO_LENGTH_SLOT).toBoolean();
  }

  size_t initialLength() const {
    MOZ_ASSERT(hasResizableBuffer());
    return size_t(getFixedSlot(INITIAL_LENGTH_SLOT).toPrivate());
  }

  size_t initialByteOffset() const {
    MOZ_ASSERT(hasResizableBuffer());
    return initialByteOffsetValue();
  }

  bool isOutOfBounds() const {
    MOZ_ASSERT(hasResizableBuffer());

    // The view is out-of-bounds if the length and byteOffset slots are both set
    // to zero and the initial length or initial byteOffset are non-zero. If the
    // initial length and initial byteOffset are both zero, the view can never
    // get out-of-bounds.
    return lengthSlotValue() == 0 && byteOffsetSlotValue() == 0 &&
           (initialLength() > 0 || initialByteOffset() > 0);
  }

 public:
  static void trace(JSTracer* trc, JSObject* obj);

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dumpOwnFields(js::JSONPrinter& json) const;
  void dumpOwnStringContent(js::GenericPrinter& out) const;
#endif
};

}  // namespace js

template <>
bool JSObject::is<js::ArrayBufferViewObject>() const;

#endif  // vm_ArrayBufferViewObject_h
