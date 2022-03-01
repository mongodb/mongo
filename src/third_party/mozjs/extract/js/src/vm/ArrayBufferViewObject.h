/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayBufferViewObject_h
#define vm_ArrayBufferViewObject_h

#include "builtin/TypedArrayConstants.h"
#include "vm/ArrayBufferObject.h"
#include "vm/NativeObject.h"
#include "vm/SharedArrayObject.h"
#include "vm/SharedMem.h"

namespace js {

/*
 * ArrayBufferViewObject
 *
 * Common base class for all array buffer views (DataViewObject and
 * TypedArrayObject).
 */

class ArrayBufferViewObject : public NativeObject {
 public:
  // Underlying (Shared)ArrayBufferObject.
  static constexpr size_t BUFFER_SLOT = 0;
  static_assert(BUFFER_SLOT == JS_TYPEDARRAYLAYOUT_BUFFER_SLOT,
                "self-hosted code with burned-in constants must get the "
                "right buffer slot");

  // Slot containing length of the view in number of typed elements.
  static constexpr size_t LENGTH_SLOT = 1;

  // Offset of view within underlying (Shared)ArrayBufferObject.
  static constexpr size_t BYTEOFFSET_SLOT = 2;

  static constexpr size_t RESERVED_SLOTS = 3;

#ifdef DEBUG
  static const uint8_t ZeroLengthArrayData = 0x4A;
#endif

  // The raw pointer to the buffer memory, the "private" value.
  //
  // This offset is exposed for performance reasons - so that it
  // need not be looked up on accesses.
  static constexpr size_t DATA_SLOT = 3;

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
    return NativeObject::getPrivateDataOffset(DATA_SLOT);
  }

 private:
  void* dataPointerEither_() const {
    // Note, do not check whether shared or not
    // Keep synced with js::Get<Type>ArrayLengthAndData in jsfriendapi.h!
    return static_cast<void*>(getPrivate(DATA_SLOT));
  }

 public:
  [[nodiscard]] bool init(JSContext* cx, ArrayBufferObjectMaybeShared* buffer,
                          size_t byteOffset, size_t length,
                          uint32_t bytesPerElement);

  static ArrayBufferObjectMaybeShared* bufferObject(
      JSContext* cx, Handle<ArrayBufferViewObject*> obj);

  void notifyBufferDetached();

  void initDataPointer(SharedMem<uint8_t*> viewData) {
    // Install a pointer to the buffer location that corresponds
    // to offset zero within the typed array.
    //
    // The following unwrap is safe because the DATA_SLOT is
    // accessed only from jitted code and from the
    // dataPointerEither_() accessor above; in neither case does the
    // raw pointer escape untagged into C++ code.
    initPrivate(viewData.unwrap(/*safe - see above*/));
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
    JSObject* obj = bufferValue().toObjectOrNull();
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

  size_t byteOffset() const {
    return size_t(getFixedSlot(BYTEOFFSET_SLOT).toPrivate());
  }

  Value byteOffsetValue() const {
    size_t offset = byteOffset();
    return NumberValue(offset);
  }

  static void trace(JSTracer* trc, JSObject* obj);
};

}  // namespace js

template <>
bool JSObject::is<js::ArrayBufferViewObject>() const;

#endif  // vm_ArrayBufferViewObject_h
