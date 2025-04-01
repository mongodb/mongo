/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_DataViewObject_h
#define vm_DataViewObject_h

#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"

#include "js/Class.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/JSObject.h"

namespace js {

class ArrayBufferObjectMaybeShared;

// In the DataViewObject, the private slot contains a raw pointer into
// the buffer.  The buffer may be shared memory and the raw pointer
// should not be exposed without sharedness information accompanying
// it.
//
// DataViewObject is an abstract base class and has exactly two concrete
// subclasses, FixedLengthDataViewObject and ResizableDataViewObject.

class DataViewObject : public ArrayBufferViewObject {
 protected:
  static const ClassSpec classSpec_;

 private:
  template <typename NativeType>
  SharedMem<uint8_t*> getDataPointer(uint64_t offset, size_t length,
                                     bool* isSharedMemory);

  static bool bufferGetterImpl(JSContext* cx, const CallArgs& args);
  static bool bufferGetter(JSContext* cx, unsigned argc, Value* vp);

  static bool byteLengthGetterImpl(JSContext* cx, const CallArgs& args);
  static bool byteLengthGetter(JSContext* cx, unsigned argc, Value* vp);

  static bool byteOffsetGetterImpl(JSContext* cx, const CallArgs& args);
  static bool byteOffsetGetter(JSContext* cx, unsigned argc, Value* vp);

  static bool getAndCheckConstructorArgs(JSContext* cx, HandleObject bufobj,
                                         const CallArgs& args,
                                         size_t* byteOffsetPtr,
                                         size_t* byteLengthPtr,
                                         AutoLength* autoLengthPtr);
  static bool constructSameCompartment(JSContext* cx, HandleObject bufobj,
                                       const CallArgs& args);
  static bool constructWrapped(JSContext* cx, HandleObject bufobj,
                               const CallArgs& args);

  static DataViewObject* create(
      JSContext* cx, size_t byteOffset, size_t byteLength,
      Handle<ArrayBufferObjectMaybeShared*> arrayBuffer, HandleObject proto);

 public:
  static const JSClass protoClass_;

  /**
   * Return the current byteLength, or |Nothing| if the DataView is detached or
   * out-of-bounds.
   */
  mozilla::Maybe<size_t> byteLength() {
    return ArrayBufferViewObject::length();
  }

  template <typename NativeType>
  static bool offsetIsInBounds(uint64_t offset, size_t byteLength) {
    return offsetIsInBounds(sizeof(NativeType), offset, byteLength);
  }
  static bool offsetIsInBounds(uint32_t byteSize, uint64_t offset,
                               size_t byteLength) {
    MOZ_ASSERT(byteSize <= 8);
    mozilla::CheckedInt<uint64_t> endOffset(offset);
    endOffset += byteSize;
    return endOffset.isValid() && endOffset.value() <= byteLength;
  }

  static bool isOriginalByteOffsetGetter(Native native) {
    return native == byteOffsetGetter;
  }

  static bool isOriginalByteLengthGetter(Native native) {
    return native == byteLengthGetter;
  }

  static bool construct(JSContext* cx, unsigned argc, Value* vp);

  static bool getInt8Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getInt8(JSContext* cx, unsigned argc, Value* vp);

  static bool getUint8Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getUint8(JSContext* cx, unsigned argc, Value* vp);

  static bool getInt16Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getInt16(JSContext* cx, unsigned argc, Value* vp);

  static bool getUint16Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getUint16(JSContext* cx, unsigned argc, Value* vp);

  static bool getInt32Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getInt32(JSContext* cx, unsigned argc, Value* vp);

  static bool getUint32Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getUint32(JSContext* cx, unsigned argc, Value* vp);

  static bool getBigInt64Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getBigInt64(JSContext* cx, unsigned argc, Value* vp);

  static bool getBigUint64Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getBigUint64(JSContext* cx, unsigned argc, Value* vp);

  static bool getFloat16Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getFloat16(JSContext* cx, unsigned argc, Value* vp);

  static bool getFloat32Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getFloat32(JSContext* cx, unsigned argc, Value* vp);

  static bool getFloat64Impl(JSContext* cx, const CallArgs& args);
  static bool fun_getFloat64(JSContext* cx, unsigned argc, Value* vp);

  static bool setInt8Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setInt8(JSContext* cx, unsigned argc, Value* vp);

  static bool setUint8Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setUint8(JSContext* cx, unsigned argc, Value* vp);

  static bool setInt16Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setInt16(JSContext* cx, unsigned argc, Value* vp);

  static bool setUint16Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setUint16(JSContext* cx, unsigned argc, Value* vp);

  static bool setInt32Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setInt32(JSContext* cx, unsigned argc, Value* vp);

  static bool setUint32Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setUint32(JSContext* cx, unsigned argc, Value* vp);

  static bool setBigInt64Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setBigInt64(JSContext* cx, unsigned argc, Value* vp);

  static bool setBigUint64Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setBigUint64(JSContext* cx, unsigned argc, Value* vp);

  static bool setFloat16Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setFloat16(JSContext* cx, unsigned argc, Value* vp);

  static bool setFloat32Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setFloat32(JSContext* cx, unsigned argc, Value* vp);

  static bool setFloat64Impl(JSContext* cx, const CallArgs& args);
  static bool fun_setFloat64(JSContext* cx, unsigned argc, Value* vp);

  template <typename NativeType>
  NativeType read(uint64_t offset, size_t length, bool isLittleEndian);

  template <typename NativeType>
  static bool read(JSContext* cx, Handle<DataViewObject*> obj,
                   const CallArgs& args, NativeType* val);
  template <typename NativeType>
  static bool write(JSContext* cx, Handle<DataViewObject*> obj,
                    const CallArgs& args);

 private:
  static const JSFunctionSpec methods[];
  static const JSPropertySpec properties[];
};

/**
 * DataView whose buffer is a fixed-length (Shared)ArrayBuffer object.
 */
class FixedLengthDataViewObject : public DataViewObject {
 public:
  static const JSClass class_;

  size_t byteOffset() const {
    return ArrayBufferViewObject::byteOffsetSlotValue();
  }

  size_t byteLength() const { return ArrayBufferViewObject::lengthSlotValue(); }
};

/**
 * DataView whose buffer is a resizable (Shared)ArrayBuffer object.
 */
class ResizableDataViewObject : public DataViewObject {
  friend class DataViewObject;

  static ResizableDataViewObject* create(
      JSContext* cx, size_t byteOffset, size_t byteLength,
      AutoLength autoLength, Handle<ArrayBufferObjectMaybeShared*> arrayBuffer,
      HandleObject proto);

 public:
  static const uint8_t RESERVED_SLOTS = RESIZABLE_RESERVED_SLOTS;

  static const JSClass class_;
};

// For structured cloning.
JSObject* NewDataView(JSContext* cx, HandleObject buffer, size_t byteOffset);

}  // namespace js

template <>
inline bool JSObject::is<js::DataViewObject>() const {
  return is<js::FixedLengthDataViewObject>() ||
         is<js::ResizableDataViewObject>();
}

#endif /* vm_DataViewObject_h */
