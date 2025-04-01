/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/DataViewObject.h"

#include "mozilla/Casting.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/IntegerTypeTraits.h"
#include "mozilla/WrappingOperations.h"

#include <algorithm>
#include <string.h>
#include <type_traits>

#include "jsnum.h"

#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "js/Conversions.h"
#include "js/experimental/TypedData.h"  // JS_NewDataView
#include "js/friend/ErrorMessages.h"    // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/Wrapper.h"
#include "util/DifferentialTesting.h"
#include "vm/ArrayBufferObject.h"
#include "vm/Compartment.h"
#include "vm/Float16.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/SharedMem.h"
#include "vm/Uint8Clamped.h"
#include "vm/WrapperObject.h"

#include "vm/ArrayBufferObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using JS::CanonicalizeNaN;
using JS::ToInt32;
using mozilla::AssertedCast;
using mozilla::WrapToSigned;

static bool IsDataView(HandleValue v) {
  return v.isObject() && v.toObject().is<DataViewObject>();
}

DataViewObject* DataViewObject::create(
    JSContext* cx, size_t byteOffset, size_t byteLength,
    Handle<ArrayBufferObjectMaybeShared*> arrayBuffer, HandleObject proto) {
  MOZ_ASSERT(!arrayBuffer->isResizable());
  MOZ_ASSERT(!arrayBuffer->isDetached());

  auto* obj = NewObjectWithClassProto<FixedLengthDataViewObject>(cx, proto);
  if (!obj || !obj->init(cx, arrayBuffer, byteOffset, byteLength,
                         /* bytesPerElement = */ 1)) {
    return nullptr;
  }

  return obj;
}

ResizableDataViewObject* ResizableDataViewObject::create(
    JSContext* cx, size_t byteOffset, size_t byteLength, AutoLength autoLength,
    Handle<ArrayBufferObjectMaybeShared*> arrayBuffer, HandleObject proto) {
  MOZ_ASSERT(arrayBuffer->isResizable());
  MOZ_ASSERT(!arrayBuffer->isDetached());
  MOZ_ASSERT(autoLength == AutoLength::No || byteLength == 0,
             "byte length is zero for 'auto' length views");

  auto* obj = NewObjectWithClassProto<ResizableDataViewObject>(cx, proto);
  if (!obj || !obj->initResizable(cx, arrayBuffer, byteOffset, byteLength,
                                  /* bytesPerElement = */ 1, autoLength)) {
    return nullptr;
  }

  return obj;
}

// ES2017 draft rev 931261ecef9b047b14daacf82884134da48dfe0f
// 24.3.2.1 DataView (extracted part of the main algorithm)
bool DataViewObject::getAndCheckConstructorArgs(
    JSContext* cx, HandleObject bufobj, const CallArgs& args,
    size_t* byteOffsetPtr, size_t* byteLengthPtr, AutoLength* autoLengthPtr) {
  // Step 3.
  if (!bufobj->is<ArrayBufferObjectMaybeShared>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, "DataView",
                              "ArrayBuffer", bufobj->getClass()->name);
    return false;
  }
  auto buffer = bufobj.as<ArrayBufferObjectMaybeShared>();

  // Step 4.
  uint64_t offset;
  if (!ToIndex(cx, args.get(1), &offset)) {
    return false;
  }

  // Step 5.
  if (buffer->isDetached()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return false;
  }

  // Step 6.
  size_t bufferByteLength = buffer->byteLength();

  // Step 7.
  if (offset > bufferByteLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OFFSET_OUT_OF_BUFFER);
    return false;
  }
  MOZ_ASSERT(offset <= ArrayBufferObject::ByteLengthLimit);

  uint64_t viewByteLength = 0;
  auto autoLength = AutoLength::No;
  if (!args.hasDefined(2)) {
    if (buffer->isResizable()) {
      autoLength = AutoLength::Yes;
    } else {
      // Step 8.a
      viewByteLength = bufferByteLength - offset;
    }
  } else {
    // Step 9.a.
    if (!ToIndex(cx, args.get(2), &viewByteLength)) {
      return false;
    }

    MOZ_ASSERT(offset + viewByteLength >= offset,
               "can't overflow: both numbers are less than "
               "DOUBLE_INTEGRAL_PRECISION_LIMIT");

    // Step 9.b.
    if (offset + viewByteLength > bufferByteLength) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_INVALID_DATA_VIEW_LENGTH);
      return false;
    }
  }
  MOZ_ASSERT(viewByteLength <= ArrayBufferObject::ByteLengthLimit);

  *byteOffsetPtr = offset;
  *byteLengthPtr = viewByteLength;
  *autoLengthPtr = autoLength;
  return true;
}

static bool CheckConstructorArgs(JSContext* cx,
                                 Handle<ArrayBufferObjectMaybeShared*> buffer,
                                 size_t byteOffset, size_t byteLength) {
  if (buffer->isDetached()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return false;
  }

  if (buffer->isResizable()) {
    size_t bufferByteLength = buffer->byteLength();
    if (byteOffset + byteLength > bufferByteLength) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_OFFSET_OUT_OF_BUFFER);
      return false;
    }
  }

  return true;
}

bool DataViewObject::constructSameCompartment(JSContext* cx,
                                              HandleObject bufobj,
                                              const CallArgs& args) {
  MOZ_ASSERT(args.isConstructing());
  cx->check(bufobj);

  size_t byteOffset = 0;
  size_t byteLength = 0;
  auto autoLength = AutoLength::No;
  if (!getAndCheckConstructorArgs(cx, bufobj, args, &byteOffset, &byteLength,
                                  &autoLength)) {
    return false;
  }

  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_DataView, &proto)) {
    return false;
  }

  auto buffer = bufobj.as<ArrayBufferObjectMaybeShared>();

  // GetPrototypeFromBuiltinConstructor may have detached or resized the buffer,
  // so we have to revalidate the arguments.
  if (!CheckConstructorArgs(cx, buffer, byteOffset, byteLength)) {
    return false;
  }

  DataViewObject* obj;
  if (!buffer->isResizable()) {
    obj = DataViewObject::create(cx, byteOffset, byteLength, buffer, proto);
  } else {
    obj = ResizableDataViewObject::create(cx, byteOffset, byteLength,
                                          autoLength, buffer, proto);
  }
  if (!obj) {
    return false;
  }
  args.rval().setObject(*obj);
  return true;
}

// Create a DataView object in another compartment.
//
// ES6 supports creating a DataView in global A (using global A's DataView
// constructor) backed by an ArrayBuffer created in global B.
//
// Our DataViewObject implementation doesn't support a DataView in
// compartment A backed by an ArrayBuffer in compartment B. So in this case,
// we create the DataView in B (!) and return a cross-compartment wrapper.
//
// Extra twist: the spec says the new DataView's [[Prototype]] must be
// A's DataView.prototype. So even though we're creating the DataView in B,
// its [[Prototype]] must be (a cross-compartment wrapper for) the
// DataView.prototype in A.
bool DataViewObject::constructWrapped(JSContext* cx, HandleObject bufobj,
                                      const CallArgs& args) {
  MOZ_ASSERT(args.isConstructing());
  MOZ_ASSERT(bufobj->is<WrapperObject>());

  RootedObject unwrapped(cx, CheckedUnwrapStatic(bufobj));
  if (!unwrapped) {
    ReportAccessDenied(cx);
    return false;
  }

  // NB: This entails the IsArrayBuffer check
  size_t byteOffset = 0;
  size_t byteLength = 0;
  auto autoLength = AutoLength::No;
  if (!getAndCheckConstructorArgs(cx, unwrapped, args, &byteOffset, &byteLength,
                                  &autoLength)) {
    return false;
  }

  // Make sure to get the [[Prototype]] for the created view from this
  // compartment.
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_DataView, &proto)) {
    return false;
  }

  auto unwrappedBuffer = unwrapped.as<ArrayBufferObjectMaybeShared>();

  // GetPrototypeFromBuiltinConstructor may have detached or resized the buffer,
  // so we have to revalidate the arguments.
  if (!CheckConstructorArgs(cx, unwrappedBuffer, byteOffset, byteLength)) {
    return false;
  }

  Rooted<GlobalObject*> global(cx, cx->realm()->maybeGlobal());
  if (!proto) {
    proto = GlobalObject::getOrCreateDataViewPrototype(cx, global);
    if (!proto) {
      return false;
    }
  }

  RootedObject dv(cx);
  {
    JSAutoRealm ar(cx, unwrapped);

    RootedObject wrappedProto(cx, proto);
    if (!cx->compartment()->wrap(cx, &wrappedProto)) {
      return false;
    }

    if (!unwrappedBuffer->isResizable()) {
      dv = DataViewObject::create(cx, byteOffset, byteLength, unwrappedBuffer,
                                  wrappedProto);
    } else {
      dv = ResizableDataViewObject::create(cx, byteOffset, byteLength,
                                           autoLength, unwrappedBuffer,
                                           wrappedProto);
    }
    if (!dv) {
      return false;
    }
  }

  if (!cx->compartment()->wrap(cx, &dv)) {
    return false;
  }

  args.rval().setObject(*dv);
  return true;
}

bool DataViewObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "DataView")) {
    return false;
  }

  RootedObject bufobj(cx);
  if (!GetFirstArgumentAsObject(cx, args, "DataView constructor", &bufobj)) {
    return false;
  }

  if (bufobj->is<WrapperObject>()) {
    return constructWrapped(cx, bufobj, args);
  }
  return constructSameCompartment(cx, bufobj, args);
}

template <typename NativeType>
SharedMem<uint8_t*> DataViewObject::getDataPointer(uint64_t offset,
                                                   size_t length,
                                                   bool* isSharedMemory) {
  MOZ_ASSERT(length <= *byteLength());
  MOZ_ASSERT(offsetIsInBounds<NativeType>(offset, length));

  MOZ_ASSERT(offset < SIZE_MAX);
  *isSharedMemory = this->isSharedMemory();
  return dataPointerEither().cast<uint8_t*>() + size_t(offset);
}

static void ReportOutOfBounds(JSContext* cx, DataViewObject* dataView) {
  if (dataView->hasDetachedBuffer()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
  } else {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_RESIZED_BOUNDS);
  }
}

template <typename T>
static inline std::enable_if_t<sizeof(T) != 1> SwapBytes(T* value,
                                                         bool isLittleEndian) {
  if (isLittleEndian) {
    mozilla::NativeEndian::swapToLittleEndianInPlace(value, 1);
  } else {
    mozilla::NativeEndian::swapToBigEndianInPlace(value, 1);
  }
}

template <typename T>
static inline std::enable_if_t<sizeof(T) == 1> SwapBytes(T* value,
                                                         bool isLittleEndian) {
  // mozilla::NativeEndian doesn't support int8_t/uint8_t types.
}

static inline void Memcpy(uint8_t* dest, uint8_t* src, size_t nbytes) {
  memcpy(dest, src, nbytes);
}

static inline void Memcpy(uint8_t* dest, SharedMem<uint8_t*> src,
                          size_t nbytes) {
  jit::AtomicOperations::memcpySafeWhenRacy(dest, src, nbytes);
}

static inline void Memcpy(SharedMem<uint8_t*> dest, uint8_t* src,
                          size_t nbytes) {
  jit::AtomicOperations::memcpySafeWhenRacy(dest, src, nbytes);
}

template <typename DataType, typename BufferPtrType>
struct DataViewIO {
  using ReadWriteType =
      typename mozilla::UnsignedStdintTypeForSize<sizeof(DataType)>::Type;

  static constexpr auto alignMask =
      std::min<size_t>(alignof(void*), sizeof(DataType)) - 1;

  static void fromBuffer(DataType* dest, BufferPtrType unalignedBuffer,
                         bool isLittleEndian) {
    MOZ_ASSERT((reinterpret_cast<uintptr_t>(dest) & alignMask) == 0);
    Memcpy((uint8_t*)dest, unalignedBuffer, sizeof(ReadWriteType));
    ReadWriteType* rwDest = reinterpret_cast<ReadWriteType*>(dest);
    SwapBytes(rwDest, isLittleEndian);
  }

  static void toBuffer(BufferPtrType unalignedBuffer, const DataType* src,
                       bool isLittleEndian) {
    MOZ_ASSERT((reinterpret_cast<uintptr_t>(src) & alignMask) == 0);
    ReadWriteType temp = *reinterpret_cast<const ReadWriteType*>(src);
    SwapBytes(&temp, isLittleEndian);
    Memcpy(unalignedBuffer, (uint8_t*)&temp, sizeof(ReadWriteType));
  }
};

template <typename NativeType>
NativeType DataViewObject::read(uint64_t offset, size_t length,
                                bool isLittleEndian) {
  bool isSharedMemory;
  SharedMem<uint8_t*> data =
      getDataPointer<NativeType>(offset, length, &isSharedMemory);
  MOZ_ASSERT(data);

  NativeType val = 0;
  if (isSharedMemory) {
    DataViewIO<NativeType, SharedMem<uint8_t*>>::fromBuffer(&val, data,
                                                            isLittleEndian);
  } else {
    DataViewIO<NativeType, uint8_t*>::fromBuffer(&val, data.unwrapUnshared(),
                                                 isLittleEndian);
  }

  return val;
}

#ifdef NIGHTLY_BUILD
template <>
float16 DataViewObject::read(uint64_t offset, size_t length,
                             bool isLittleEndian) {
  float16 val{};
  val.val = read<uint16_t>(offset, length, isLittleEndian);
  return val;
}
#endif

template uint32_t DataViewObject::read(uint64_t offset, size_t length,
                                       bool isLittleEndian);

// https://tc39.github.io/ecma262/#sec-getviewvalue
// GetViewValue ( view, requestIndex, isLittleEndian, type )
template <typename NativeType>
/* static */
bool DataViewObject::read(JSContext* cx, Handle<DataViewObject*> obj,
                          const CallArgs& args, NativeType* val) {
  // Step 1. done by the caller
  // Step 2. unnecessary assert

  // Step 3.
  uint64_t getIndex;
  if (!ToIndex(cx, args.get(0), &getIndex)) {
    return false;
  }

  // Step 4.
  bool isLittleEndian = args.length() >= 2 && ToBoolean(args[1]);

  // Steps 5-6.
  auto viewSize = obj->byteLength();
  if (MOZ_UNLIKELY(!viewSize)) {
    ReportOutOfBounds(cx, obj);
    return false;
  }

  // Steps 7-10.
  if (!offsetIsInBounds<NativeType>(getIndex, *viewSize)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OFFSET_OUT_OF_DATAVIEW);
    return false;
  }

  // Steps 11-12.
  *val = obj->read<NativeType>(getIndex, *viewSize, isLittleEndian);
  return true;
}

template <typename T>
static inline T WrappingConvert(int32_t value) {
  if (std::is_unsigned_v<T>) {
    return static_cast<T>(value);
  }

  return WrapToSigned(static_cast<typename std::make_unsigned_t<T>>(value));
}

template <typename NativeType>
static inline bool WebIDLCast(JSContext* cx, HandleValue value,
                              NativeType* out) {
  int32_t i;
  if (!ToInt32(cx, value, &i)) {
    return false;
  }

  *out = WrappingConvert<NativeType>(i);
  return true;
}

template <>
inline bool WebIDLCast<int64_t>(JSContext* cx, HandleValue value,
                                int64_t* out) {
  BigInt* bi = ToBigInt(cx, value);
  if (!bi) {
    return false;
  }
  *out = BigInt::toInt64(bi);
  return true;
}

template <>
inline bool WebIDLCast<uint64_t>(JSContext* cx, HandleValue value,
                                 uint64_t* out) {
  BigInt* bi = ToBigInt(cx, value);
  if (!bi) {
    return false;
  }
  *out = BigInt::toUint64(bi);
  return true;
}

#ifdef NIGHTLY_BUILD
template <>
inline bool WebIDLCast<float16>(JSContext* cx, HandleValue value,
                                float16* out) {
  double temp;
  if (!ToNumber(cx, value, &temp)) {
    return false;
  }
  *out = float16(temp);
  return true;
}
#endif

template <>
inline bool WebIDLCast<float>(JSContext* cx, HandleValue value, float* out) {
  double temp;
  if (!ToNumber(cx, value, &temp)) {
    return false;
  }
  *out = static_cast<float>(temp);
  return true;
}

template <>
inline bool WebIDLCast<double>(JSContext* cx, HandleValue value, double* out) {
  return ToNumber(cx, value, out);
}

// https://tc39.github.io/ecma262/#sec-setviewvalue
// SetViewValue ( view, requestIndex, isLittleEndian, type, value )
template <typename NativeType>
/* static */
bool DataViewObject::write(JSContext* cx, Handle<DataViewObject*> obj,
                           const CallArgs& args) {
  // Step 1. done by the caller
  // Step 2. unnecessary assert

  // Step 3.
  uint64_t getIndex;
  if (!ToIndex(cx, args.get(0), &getIndex)) {
    return false;
  }

  // Steps 4-5. Call ToBigInt(value) or ToNumber(value) depending on the type.
  NativeType value;
  if (!WebIDLCast(cx, args.get(1), &value)) {
    return false;
  }

  // See the comment in ElementSpecific::doubleToNative.
  if (js::SupportDifferentialTesting() && TypeIsFloatingPoint<NativeType>()) {
    if constexpr (std::is_same_v<NativeType, float16>) {
      value = JS::CanonicalizeNaN(value.toDouble());
    } else {
      value = JS::CanonicalizeNaN(value);
    }
  }

  // Step 6.
  bool isLittleEndian = args.length() >= 3 && ToBoolean(args[2]);

  // Steps 7-8.
  auto viewSize = obj->byteLength();
  if (MOZ_UNLIKELY(!viewSize)) {
    ReportOutOfBounds(cx, obj);
    return false;
  }

  // Steps 9-12.
  if (!offsetIsInBounds<NativeType>(getIndex, *viewSize)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OFFSET_OUT_OF_DATAVIEW);
    return false;
  }

  // Steps 13-14.
  bool isSharedMemory;
  SharedMem<uint8_t*> data =
      obj->getDataPointer<NativeType>(getIndex, *viewSize, &isSharedMemory);
  MOZ_ASSERT(data);

  if (isSharedMemory) {
    DataViewIO<NativeType, SharedMem<uint8_t*>>::toBuffer(data, &value,
                                                          isLittleEndian);
  } else {
    DataViewIO<NativeType, uint8_t*>::toBuffer(data.unwrapUnshared(), &value,
                                               isLittleEndian);
  }
  return true;
}

bool DataViewObject::getInt8Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  int8_t val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }
  args.rval().setInt32(val);
  return true;
}

bool DataViewObject::fun_getInt8(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getInt8Impl>(cx, args);
}

bool DataViewObject::getUint8Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  uint8_t val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }
  args.rval().setInt32(val);
  return true;
}

bool DataViewObject::fun_getUint8(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getUint8Impl>(cx, args);
}

bool DataViewObject::getInt16Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  int16_t val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }
  args.rval().setInt32(val);
  return true;
}

bool DataViewObject::fun_getInt16(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getInt16Impl>(cx, args);
}

bool DataViewObject::getUint16Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  uint16_t val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }
  args.rval().setInt32(val);
  return true;
}

bool DataViewObject::fun_getUint16(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getUint16Impl>(cx, args);
}

bool DataViewObject::getInt32Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  int32_t val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }
  args.rval().setInt32(val);
  return true;
}

bool DataViewObject::fun_getInt32(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getInt32Impl>(cx, args);
}

bool DataViewObject::getUint32Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  uint32_t val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }
  args.rval().setNumber(val);
  return true;
}

bool DataViewObject::fun_getUint32(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getUint32Impl>(cx, args);
}

// BigInt proposal 7.26
// DataView.prototype.getBigInt64 ( byteOffset [ , littleEndian ] )
bool DataViewObject::getBigInt64Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  int64_t val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }

  BigInt* bi = BigInt::createFromInt64(cx, val);
  if (!bi) {
    return false;
  }
  args.rval().setBigInt(bi);
  return true;
}

bool DataViewObject::fun_getBigInt64(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getBigInt64Impl>(cx, args);
}

// BigInt proposal 7.27
// DataView.prototype.getBigUint64 ( byteOffset [ , littleEndian ] )
bool DataViewObject::getBigUint64Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  int64_t val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }

  BigInt* bi = BigInt::createFromUint64(cx, val);
  if (!bi) {
    return false;
  }
  args.rval().setBigInt(bi);
  return true;
}

bool DataViewObject::fun_getBigUint64(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getBigUint64Impl>(cx, args);
}

#ifdef NIGHTLY_BUILD
bool DataViewObject::getFloat16Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  float16 val{};
  if (!read(cx, thisView, args, &val)) {
    return false;
  }

  args.rval().setDouble(CanonicalizeNaN(val.toDouble()));
  return true;
}

bool DataViewObject::fun_getFloat16(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getFloat16Impl>(cx, args);
}
#endif

bool DataViewObject::getFloat32Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  float val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }

  args.rval().setDouble(CanonicalizeNaN(val));
  return true;
}

bool DataViewObject::fun_getFloat32(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getFloat32Impl>(cx, args);
}

bool DataViewObject::getFloat64Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  double val;
  if (!read(cx, thisView, args, &val)) {
    return false;
  }

  args.rval().setDouble(CanonicalizeNaN(val));
  return true;
}

bool DataViewObject::fun_getFloat64(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, getFloat64Impl>(cx, args);
}

bool DataViewObject::setInt8Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<int8_t>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setInt8(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setInt8Impl>(cx, args);
}

bool DataViewObject::setUint8Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<uint8_t>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setUint8(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setUint8Impl>(cx, args);
}

bool DataViewObject::setInt16Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<int16_t>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setInt16(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setInt16Impl>(cx, args);
}

bool DataViewObject::setUint16Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<uint16_t>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setUint16(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setUint16Impl>(cx, args);
}

bool DataViewObject::setInt32Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<int32_t>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setInt32(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setInt32Impl>(cx, args);
}

bool DataViewObject::setUint32Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<uint32_t>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setUint32(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setUint32Impl>(cx, args);
}

// BigInt proposal 7.28
// DataView.prototype.setBigInt64 ( byteOffset, value [ , littleEndian ] )
bool DataViewObject::setBigInt64Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<int64_t>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setBigInt64(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setBigInt64Impl>(cx, args);
}

// BigInt proposal 7.29
// DataView.prototype.setBigUint64 ( byteOffset, value [ , littleEndian ] )
bool DataViewObject::setBigUint64Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<uint64_t>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setBigUint64(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setBigUint64Impl>(cx, args);
}

#ifdef NIGHTLY_BUILD
bool DataViewObject::setFloat16Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<float16>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setFloat16(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setFloat16Impl>(cx, args);
}
#endif

bool DataViewObject::setFloat32Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<float>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setFloat32(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setFloat32Impl>(cx, args);
}

bool DataViewObject::setFloat64Impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsDataView(args.thisv()));

  Rooted<DataViewObject*> thisView(
      cx, &args.thisv().toObject().as<DataViewObject>());

  if (!write<double>(cx, thisView, args)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DataViewObject::fun_setFloat64(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, setFloat64Impl>(cx, args);
}

bool DataViewObject::bufferGetterImpl(JSContext* cx, const CallArgs& args) {
  auto* thisView = &args.thisv().toObject().as<DataViewObject>();
  args.rval().set(thisView->bufferValue());
  return true;
}

bool DataViewObject::bufferGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, bufferGetterImpl>(cx, args);
}

bool DataViewObject::byteLengthGetterImpl(JSContext* cx, const CallArgs& args) {
  auto* thisView = &args.thisv().toObject().as<DataViewObject>();

  // Step 6.
  auto byteLength = thisView->byteLength();
  if (MOZ_UNLIKELY(!byteLength)) {
    ReportOutOfBounds(cx, thisView);
    return false;
  }

  // Step 7.
  args.rval().set(NumberValue(*byteLength));
  return true;
}

bool DataViewObject::byteLengthGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, byteLengthGetterImpl>(cx, args);
}

bool DataViewObject::byteOffsetGetterImpl(JSContext* cx, const CallArgs& args) {
  auto* thisView = &args.thisv().toObject().as<DataViewObject>();

  // Step 6.
  auto byteOffset = thisView->byteOffset();
  if (MOZ_UNLIKELY(!byteOffset)) {
    ReportOutOfBounds(cx, thisView);
    return false;
  }

  // Step 7.
  args.rval().set(NumberValue(*byteOffset));
  return true;
}

bool DataViewObject::byteOffsetGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDataView, byteOffsetGetterImpl>(cx, args);
}

static const JSClassOps DataViewObjectClassOps = {
    nullptr,                       // addProperty
    nullptr,                       // delProperty
    nullptr,                       // enumerate
    nullptr,                       // newEnumerate
    nullptr,                       // resolve
    nullptr,                       // mayResolve
    nullptr,                       // finalize
    nullptr,                       // call
    nullptr,                       // construct
    ArrayBufferViewObject::trace,  // trace
};

static JSObject* CreateDataViewPrototype(JSContext* cx, JSProtoKey key) {
  return GlobalObject::createBlankPrototype(cx, cx->global(),
                                            &DataViewObject::protoClass_);
}

const ClassSpec DataViewObject::classSpec_ = {
    GenericCreateConstructor<DataViewObject::construct, 1,
                             gc::AllocKind::FUNCTION>,
    CreateDataViewPrototype,
    nullptr,
    nullptr,
    DataViewObject::methods,
    DataViewObject::properties,
};

const JSClass FixedLengthDataViewObject::class_ = {
    "DataView",
    JSCLASS_HAS_RESERVED_SLOTS(FixedLengthDataViewObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_DataView),
    &DataViewObjectClassOps,
    &DataViewObject::classSpec_,
};

const JSClass ResizableDataViewObject::class_ = {
    "DataView",
    JSCLASS_HAS_RESERVED_SLOTS(ResizableDataViewObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_DataView),
    &DataViewObjectClassOps,
    &DataViewObject::classSpec_,
};

const JSClass* const JS::DataView::FixedLengthClassPtr =
    &FixedLengthDataViewObject::class_;
const JSClass* const JS::DataView::ResizableClassPtr =
    &ResizableDataViewObject::class_;

const JSClass DataViewObject::protoClass_ = {
    "DataView.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_DataView),
    JS_NULL_CLASS_OPS,
    &DataViewObject::classSpec_,
};

const JSFunctionSpec DataViewObject::methods[] = {
    JS_INLINABLE_FN("getInt8", DataViewObject::fun_getInt8, 1, 0,
                    DataViewGetInt8),
    JS_INLINABLE_FN("getUint8", DataViewObject::fun_getUint8, 1, 0,
                    DataViewGetUint8),
    JS_INLINABLE_FN("getInt16", DataViewObject::fun_getInt16, 1, 0,
                    DataViewGetInt16),
    JS_INLINABLE_FN("getUint16", DataViewObject::fun_getUint16, 1, 0,
                    DataViewGetUint16),
    JS_INLINABLE_FN("getInt32", DataViewObject::fun_getInt32, 1, 0,
                    DataViewGetInt32),
    JS_INLINABLE_FN("getUint32", DataViewObject::fun_getUint32, 1, 0,
                    DataViewGetUint32),
#ifdef NIGHTLY_BUILD
    // TODO: See Bug 1835034 for JIT support for Float16Array
    JS_FN("getFloat16", DataViewObject::fun_getFloat16, 1, 0),
#endif
    JS_INLINABLE_FN("getFloat32", DataViewObject::fun_getFloat32, 1, 0,
                    DataViewGetFloat32),
    JS_INLINABLE_FN("getFloat64", DataViewObject::fun_getFloat64, 1, 0,
                    DataViewGetFloat64),
    JS_INLINABLE_FN("getBigInt64", DataViewObject::fun_getBigInt64, 1, 0,
                    DataViewGetBigInt64),
    JS_INLINABLE_FN("getBigUint64", DataViewObject::fun_getBigUint64, 1, 0,
                    DataViewGetBigUint64),
    JS_INLINABLE_FN("setInt8", DataViewObject::fun_setInt8, 2, 0,
                    DataViewSetInt8),
    JS_INLINABLE_FN("setUint8", DataViewObject::fun_setUint8, 2, 0,
                    DataViewSetUint8),
    JS_INLINABLE_FN("setInt16", DataViewObject::fun_setInt16, 2, 0,
                    DataViewSetInt16),
    JS_INLINABLE_FN("setUint16", DataViewObject::fun_setUint16, 2, 0,
                    DataViewSetUint16),
    JS_INLINABLE_FN("setInt32", DataViewObject::fun_setInt32, 2, 0,
                    DataViewSetInt32),
    JS_INLINABLE_FN("setUint32", DataViewObject::fun_setUint32, 2, 0,
                    DataViewSetUint32),
#ifdef NIGHTLY_BUILD
    // TODO: See Bug 1835034 for JIT support for Float16Array
    JS_FN("setFloat16", DataViewObject::fun_setFloat16, 2, 0),
#endif
    JS_INLINABLE_FN("setFloat32", DataViewObject::fun_setFloat32, 2, 0,
                    DataViewSetFloat32),
    JS_INLINABLE_FN("setFloat64", DataViewObject::fun_setFloat64, 2, 0,
                    DataViewSetFloat64),
    JS_INLINABLE_FN("setBigInt64", DataViewObject::fun_setBigInt64, 2, 0,
                    DataViewSetBigInt64),
    JS_INLINABLE_FN("setBigUint64", DataViewObject::fun_setBigUint64, 2, 0,
                    DataViewSetBigUint64),
    JS_FS_END};

const JSPropertySpec DataViewObject::properties[] = {
    JS_PSG("buffer", DataViewObject::bufferGetter, 0),
    JS_PSG("byteLength", DataViewObject::byteLengthGetter, 0),
    JS_PSG("byteOffset", DataViewObject::byteOffsetGetter, 0),
    JS_STRING_SYM_PS(toStringTag, "DataView", JSPROP_READONLY), JS_PS_END};

JS_PUBLIC_API JSObject* JS_NewDataView(JSContext* cx, HandleObject buffer,
                                       size_t byteOffset, size_t byteLength) {
  JSProtoKey key = JSProto_DataView;
  RootedObject constructor(cx, GlobalObject::getOrCreateConstructor(cx, key));
  if (!constructor) {
    return nullptr;
  }

  FixedConstructArgs<3> cargs(cx);

  cargs[0].setObject(*buffer);
  cargs[1].setNumber(byteOffset);
  cargs[2].setNumber(byteLength);

  RootedValue fun(cx, ObjectValue(*constructor));
  RootedObject obj(cx);
  if (!Construct(cx, fun, cargs, fun, &obj)) {
    return nullptr;
  }
  return obj;
}

JSObject* js::NewDataView(JSContext* cx, HandleObject buffer,
                          size_t byteOffset) {
  JSProtoKey key = JSProto_DataView;
  RootedObject constructor(cx, GlobalObject::getOrCreateConstructor(cx, key));
  if (!constructor) {
    return nullptr;
  }

  FixedConstructArgs<2> cargs(cx);

  cargs[0].setObject(*buffer);
  cargs[1].setNumber(byteOffset);

  RootedValue fun(cx, ObjectValue(*constructor));
  RootedObject obj(cx);
  if (!Construct(cx, fun, cargs, fun, &obj)) {
    return nullptr;
  }
  return obj;
}
