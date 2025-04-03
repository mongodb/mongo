/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/TypedArrayObject-inl.h"
#include "vm/TypedArrayObject.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/IntegerTypeTraits.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TextUtils.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <numeric>
#include <string.h>
#include <string_view>
#if !defined(XP_WIN) && !defined(__wasi__)
#  include <sys/mman.h>
#endif
#include <type_traits>

#include "jsnum.h"
#include "jstypes.h"

#include "builtin/Array.h"
#include "builtin/DataViewObject.h"
#include "gc/Barrier.h"
#include "gc/MaybeRooted.h"
#include "jit/InlinableNatives.h"
#include "js/Conversions.h"
#include "js/experimental/TypedData.h"  // JS_GetArrayBufferViewType, JS_GetTypedArray{Length,ByteOffset,ByteLength}, JS_IsTypedArrayObject
#include "js/friend/ErrorMessages.h"    // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/ScalarType.h"  // JS::Scalar::Type
#include "js/UniquePtr.h"
#include "js/Wrapper.h"
#include "util/DifferentialTesting.h"
#include "util/Text.h"
#include "util/WindowsWrapper.h"
#include "vm/ArrayBufferObject.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PIC.h"
#include "vm/SelfHosting.h"
#include "vm/SharedMem.h"
#include "vm/Uint8Clamped.h"
#include "vm/WrapperObject.h"

#include "gc/Nursery-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using JS::CanonicalizeNaN;
using JS::ToInt32;
using JS::ToUint32;
using mozilla::IsAsciiDigit;

/*
 * TypedArrayObject
 *
 * The non-templated base class for the specific typed implementations.
 * This class holds all the member variables that are used by
 * the subclasses.
 */

bool TypedArrayObject::convertForSideEffect(JSContext* cx,
                                            HandleValue v) const {
  switch (type()) {
    case Scalar::BigInt64:
    case Scalar::BigUint64: {
      return ToBigInt(cx, v) != nullptr;
    }
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
    case Scalar::Float64:
    case Scalar::Uint8Clamped: {
      double ignore;
      return ToNumber(cx, v, &ignore);
    }
    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      MOZ_CRASH("Unsupported TypedArray type");
  }
  MOZ_ASSERT_UNREACHABLE("Invalid scalar type");
  return false;
}

/* static */
bool TypedArrayObject::is(HandleValue v) {
  return v.isObject() && v.toObject().is<TypedArrayObject>();
}

/* static */
bool TypedArrayObject::ensureHasBuffer(JSContext* cx,
                                       Handle<TypedArrayObject*> tarray) {
  if (tarray->hasBuffer()) {
    return true;
  }

  size_t byteLength = tarray->byteLength();

  AutoRealm ar(cx, tarray);
  Rooted<ArrayBufferObject*> buffer(
      cx, ArrayBufferObject::createZeroed(cx, tarray->byteLength()));
  if (!buffer) {
    return false;
  }

  // Attaching the first view to an array buffer is infallible.
  MOZ_ALWAYS_TRUE(buffer->addView(cx, tarray));

  // tarray is not shared, because if it were it would have a buffer.
  memcpy(buffer->dataPointer(), tarray->dataPointerUnshared(), byteLength);

  // If the object is in the nursery, the buffer will be freed by the next
  // nursery GC. Free the data slot pointer if the object has no inline data.
  size_t nbytes = RoundUp(byteLength, sizeof(Value));
  Nursery& nursery = cx->nursery();
  if (tarray->isTenured() && !tarray->hasInlineElements() &&
      !nursery.isInside(tarray->elements())) {
    js_free(tarray->elements());
    RemoveCellMemory(tarray, nbytes, MemoryUse::TypedArrayElements);
  }

  tarray->setFixedSlot(TypedArrayObject::DATA_SLOT,
                       PrivateValue(buffer->dataPointer()));
  tarray->setFixedSlot(TypedArrayObject::BUFFER_SLOT, ObjectValue(*buffer));

  return true;
}

#ifdef DEBUG
void TypedArrayObject::assertZeroLengthArrayData() const {
  if (length() == 0 && !hasBuffer()) {
    uint8_t* end = fixedData(TypedArrayObject::FIXED_DATA_START);
    MOZ_ASSERT(end[0] == ZeroLengthArrayData);
  }
}
#endif

void TypedArrayObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(!IsInsideNursery(obj));
  TypedArrayObject* curObj = &obj->as<TypedArrayObject>();

  // Template objects or discarded objects (which didn't have enough room
  // for inner elements) don't have anything to free.
  if (!curObj->elementsRaw()) {
    return;
  }

  curObj->assertZeroLengthArrayData();

  // Typed arrays with a buffer object do not need to be free'd
  if (curObj->hasBuffer()) {
    return;
  }

  // Free the data slot pointer if it does not point into the old JSObject.
  if (!curObj->hasInlineElements()) {
    size_t nbytes = RoundUp(curObj->byteLength(), sizeof(Value));
    gcx->free_(obj, curObj->elements(), nbytes, MemoryUse::TypedArrayElements);
  }
}

/* static */
size_t TypedArrayObject::objectMoved(JSObject* obj, JSObject* old) {
  TypedArrayObject* newObj = &obj->as<TypedArrayObject>();
  const TypedArrayObject* oldObj = &old->as<TypedArrayObject>();
  MOZ_ASSERT(newObj->elementsRaw() == oldObj->elementsRaw());
  MOZ_ASSERT(obj->isTenured());

  // Typed arrays with a buffer object do not need an update.
  if (oldObj->hasBuffer()) {
    return 0;
  }

  if (!IsInsideNursery(old)) {
    // Update the data slot pointer if it points to the old JSObject.
    if (oldObj->hasInlineElements()) {
      newObj->setInlineElements();
    }

    return 0;
  }

  void* buf = oldObj->elements();

  // Discarded objects (which didn't have enough room for inner elements) don't
  // have any data to move.
  if (!buf) {
    return 0;
  }

  Nursery& nursery = obj->runtimeFromMainThread()->gc.nursery();
  if (!nursery.isInside(buf)) {
    nursery.removeMallocedBufferDuringMinorGC(buf);
    size_t nbytes = RoundUp(newObj->byteLength(), sizeof(Value));
    AddCellMemory(newObj, nbytes, MemoryUse::TypedArrayElements);
    return 0;
  }

  // Determine if we can use inline data for the target array. If this is
  // possible, the nursery will have picked an allocation size that is large
  // enough.
  size_t nbytes = oldObj->byteLength();
  MOZ_ASSERT(nbytes <= Nursery::MaxNurseryBufferSize);

  constexpr size_t headerSize = dataOffset() + sizeof(HeapSlot);

  // See AllocKindForLazyBuffer.
  gc::AllocKind newAllocKind = obj->asTenured().getAllocKind();
  MOZ_ASSERT_IF(nbytes == 0,
                headerSize + sizeof(uint8_t) <= GetGCKindBytes(newAllocKind));

  if (headerSize + nbytes <= GetGCKindBytes(newAllocKind)) {
    MOZ_ASSERT(oldObj->hasInlineElements());
#ifdef DEBUG
    if (nbytes == 0) {
      uint8_t* output = newObj->fixedData(TypedArrayObject::FIXED_DATA_START);
      output[0] = ZeroLengthArrayData;
    }
#endif
    newObj->setInlineElements();
  } else {
    MOZ_ASSERT(!oldObj->hasInlineElements());

    AutoEnterOOMUnsafeRegion oomUnsafe;
    nbytes = RoundUp(nbytes, sizeof(Value));
    void* data = newObj->zone()->pod_arena_malloc<uint8_t>(
        js::ArrayBufferContentsArena, nbytes);
    if (!data) {
      oomUnsafe.crash(
          "Failed to allocate typed array elements while tenuring.");
    }
    MOZ_ASSERT(!nursery.isInside(data));
    newObj->setReservedSlot(DATA_SLOT, PrivateValue(data));
    AddCellMemory(newObj, nbytes, MemoryUse::TypedArrayElements);
  }

  mozilla::PodCopy(newObj->elements(), oldObj->elements(), nbytes);

  // Set a forwarding pointer for the element buffers in case they were
  // preserved on the stack by Ion.
  nursery.setForwardingPointerWhileTenuring(
      oldObj->elements(), newObj->elements(),
      /* direct = */ nbytes >= sizeof(uintptr_t));

  return newObj->hasInlineElements() ? 0 : nbytes;
}

bool TypedArrayObject::hasInlineElements() const {
  return elements() == this->fixedData(TypedArrayObject::FIXED_DATA_START) &&
         byteLength() <= TypedArrayObject::INLINE_BUFFER_LIMIT;
}

void TypedArrayObject::setInlineElements() {
  char* dataSlot = reinterpret_cast<char*>(this) + dataOffset();
  *reinterpret_cast<void**>(dataSlot) =
      this->fixedData(TypedArrayObject::FIXED_DATA_START);
}

/* Helper clamped uint8_t type */

uint32_t js::ClampDoubleToUint8(const double x) {
  // Not < so that NaN coerces to 0
  if (!(x >= 0)) {
    return 0;
  }

  if (x > 255) {
    return 255;
  }

  double toTruncate = x + 0.5;
  uint8_t y = uint8_t(toTruncate);

  /*
   * now val is rounded to nearest, ties rounded up.  We want
   * rounded to nearest ties to even, so check whether we had a
   * tie.
   */
  if (y == toTruncate) {
    /*
     * It was a tie (since adding 0.5 gave us the exact integer
     * we want).  Since we rounded up, we either already have an
     * even number or we have an odd number but the number we
     * want is one less.  So just unconditionally masking out the
     * ones bit should do the trick to get us the value we
     * want.
     */
    return y & ~1;
  }

  return y;
}

namespace {

static TypedArrayObject* NewTypedArrayObject(JSContext* cx,
                                             const JSClass* clasp,
                                             HandleObject proto,
                                             gc::AllocKind allocKind,
                                             gc::Heap heap) {
  MOZ_ASSERT(proto);

  MOZ_ASSERT(CanChangeToBackgroundAllocKind(allocKind, clasp));
  allocKind = ForegroundToBackgroundAllocKind(allocKind);

  // Typed arrays can store data inline so we only use fixed slots to cover the
  // reserved slots, ignoring the AllocKind.
  MOZ_ASSERT(ClassCanHaveFixedData(clasp));
  constexpr size_t nfixed = TypedArrayObject::RESERVED_SLOTS;
  static_assert(nfixed <= NativeObject::MAX_FIXED_SLOTS);
  static_assert(nfixed == TypedArrayObject::FIXED_DATA_START);

  Rooted<SharedShape*> shape(
      cx,
      SharedShape::getInitialShape(cx, clasp, cx->realm(), AsTaggedProto(proto),
                                   nfixed, ObjectFlags()));
  if (!shape) {
    return nullptr;
  }

  NativeObject* obj = NativeObject::create(cx, allocKind, heap, shape);
  if (!obj) {
    return nullptr;
  }

  return &obj->as<TypedArrayObject>();
}

template <typename NativeType>
class TypedArrayObjectTemplate : public TypedArrayObject {
  friend class TypedArrayObject;

 public:
  static constexpr Scalar::Type ArrayTypeID() {
    return TypeIDOfType<NativeType>::id;
  }
  static constexpr JSProtoKey protoKey() {
    return TypeIDOfType<NativeType>::protoKey;
  }

  static constexpr bool ArrayTypeIsUnsigned() {
    return TypeIsUnsigned<NativeType>();
  }
  static constexpr bool ArrayTypeIsFloatingPoint() {
    return TypeIsFloatingPoint<NativeType>();
  }

  static constexpr size_t BYTES_PER_ELEMENT = sizeof(NativeType);

  static JSObject* createPrototype(JSContext* cx, JSProtoKey key) {
    Handle<GlobalObject*> global = cx->global();
    RootedObject typedArrayProto(
        cx, GlobalObject::getOrCreateTypedArrayPrototype(cx, global));
    if (!typedArrayProto) {
      return nullptr;
    }

    const JSClass* clasp = TypedArrayObject::protoClassForType(ArrayTypeID());
    return GlobalObject::createBlankPrototypeInheriting(cx, clasp,
                                                        typedArrayProto);
  }

  static JSObject* createConstructor(JSContext* cx, JSProtoKey key) {
    Handle<GlobalObject*> global = cx->global();
    RootedFunction ctorProto(
        cx, GlobalObject::getOrCreateTypedArrayConstructor(cx, global));
    if (!ctorProto) {
      return nullptr;
    }

    JSFunction* fun = NewFunctionWithProto(
        cx, class_constructor, 3, FunctionFlags::NATIVE_CTOR, nullptr,
        ClassName(key, cx), ctorProto, gc::AllocKind::FUNCTION, TenuredObject);

    if (fun) {
      fun->setJitInfo(&jit::JitInfo_TypedArrayConstructor);
    }

    return fun;
  }

  static inline const JSClass* instanceClass() {
    return TypedArrayObject::classForType(ArrayTypeID());
  }

  static bool is(HandleValue v) {
    return v.isObject() && v.toObject().hasClass(instanceClass());
  }

  static bool convertValue(JSContext* cx, HandleValue v, NativeType* result);

  static TypedArrayObject* newBuiltinClassInstance(JSContext* cx,
                                                   gc::AllocKind allocKind,
                                                   gc::Heap heap) {
    RootedObject proto(cx, GlobalObject::getOrCreatePrototype(cx, protoKey()));
    if (!proto) {
      return nullptr;
    }
    return NewTypedArrayObject(cx, instanceClass(), proto, allocKind, heap);
  }

  static TypedArrayObject* makeProtoInstance(JSContext* cx, HandleObject proto,
                                             gc::AllocKind allocKind) {
    MOZ_ASSERT(proto);
    return NewTypedArrayObject(cx, instanceClass(), proto, allocKind,
                               gc::Heap::Default);
  }

  static TypedArrayObject* makeInstance(
      JSContext* cx, Handle<ArrayBufferObjectMaybeShared*> buffer,
      size_t byteOffset, size_t len, HandleObject proto,
      gc::Heap heap = gc::Heap::Default) {
    MOZ_ASSERT(len <= MaxByteLength / BYTES_PER_ELEMENT);

    gc::AllocKind allocKind =
        buffer ? gc::GetGCObjectKind(instanceClass())
               : AllocKindForLazyBuffer(len * BYTES_PER_ELEMENT);

    AutoSetNewObjectMetadata metadata(cx);
    Rooted<TypedArrayObject*> obj(cx);
    if (proto) {
      obj = makeProtoInstance(cx, proto, allocKind);
    } else {
      obj = newBuiltinClassInstance(cx, allocKind, heap);
    }
    if (!obj || !obj->init(cx, buffer, byteOffset, len, BYTES_PER_ELEMENT)) {
      return nullptr;
    }

    return obj;
  }

  static TypedArrayObject* makeTemplateObject(JSContext* cx, int32_t len) {
    MOZ_ASSERT(len >= 0);
    size_t nbytes;
    MOZ_ALWAYS_TRUE(CalculateAllocSize<NativeType>(len, &nbytes));
    bool fitsInline = nbytes <= INLINE_BUFFER_LIMIT;
    gc::AllocKind allocKind = !fitsInline ? gc::GetGCObjectKind(instanceClass())
                                          : AllocKindForLazyBuffer(nbytes);
    MOZ_ASSERT(allocKind >= gc::GetGCObjectKind(instanceClass()));

    AutoSetNewObjectMetadata metadata(cx);

    Rooted<TypedArrayObject*> tarray(
        cx, newBuiltinClassInstance(cx, allocKind, gc::Heap::Tenured));
    if (!tarray) {
      return nullptr;
    }

    initTypedArraySlots(tarray, len);

    // Template objects don't need memory for their elements, since there
    // won't be any elements to store.
    MOZ_ASSERT(tarray->getReservedSlot(DATA_SLOT).isUndefined());

    return tarray;
  }

  static void initTypedArraySlots(TypedArrayObject* tarray, int32_t len) {
    MOZ_ASSERT(len >= 0);
    tarray->initFixedSlot(TypedArrayObject::BUFFER_SLOT, NullValue());
    tarray->initFixedSlot(TypedArrayObject::LENGTH_SLOT, PrivateValue(len));
    tarray->initFixedSlot(TypedArrayObject::BYTEOFFSET_SLOT,
                          PrivateValue(size_t(0)));

#ifdef DEBUG
    if (len == 0) {
      uint8_t* output = tarray->fixedData(TypedArrayObject::FIXED_DATA_START);
      output[0] = TypedArrayObject::ZeroLengthArrayData;
    }
#endif
  }

  static void initTypedArrayData(TypedArrayObject* tarray, void* buf,
                                 size_t nbytes, gc::AllocKind allocKind) {
    if (buf) {
      InitReservedSlot(tarray, TypedArrayObject::DATA_SLOT, buf, nbytes,
                       MemoryUse::TypedArrayElements);
    } else {
#ifdef DEBUG
      constexpr size_t dataOffset = ArrayBufferViewObject::dataOffset();
      constexpr size_t offset = dataOffset + sizeof(HeapSlot);
      MOZ_ASSERT(offset + nbytes <= GetGCKindBytes(allocKind));
#endif

      void* data = tarray->fixedData(FIXED_DATA_START);
      tarray->initReservedSlot(DATA_SLOT, PrivateValue(data));
      memset(data, 0, nbytes);
    }
  }

  static TypedArrayObject* makeTypedArrayWithTemplate(
      JSContext* cx, TypedArrayObject* templateObj, int32_t len) {
    if (len < 0 || size_t(len) > MaxByteLength / BYTES_PER_ELEMENT) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_ARRAY_LENGTH);
      return nullptr;
    }

    size_t nbytes = size_t(len) * BYTES_PER_ELEMENT;
    MOZ_ASSERT(nbytes <= MaxByteLength);

    bool fitsInline = nbytes <= INLINE_BUFFER_LIMIT;

    AutoSetNewObjectMetadata metadata(cx);

    gc::AllocKind allocKind = !fitsInline ? gc::GetGCObjectKind(instanceClass())
                                          : AllocKindForLazyBuffer(nbytes);
    MOZ_ASSERT(templateObj->getClass() == instanceClass());

    RootedObject proto(cx, templateObj->staticPrototype());
    TypedArrayObject* obj = makeProtoInstance(cx, proto, allocKind);
    if (!obj) {
      return nullptr;
    }

    initTypedArraySlots(obj, len);

    void* buf = nullptr;
    if (!fitsInline) {
      MOZ_ASSERT(len > 0);

      nbytes = RoundUp(nbytes, sizeof(Value));
      buf = cx->nursery().allocateZeroedBuffer(obj, nbytes,
                                               js::ArrayBufferContentsArena);
      if (!buf) {
        ReportOutOfMemory(cx);
        return nullptr;
      }
    }

    initTypedArrayData(obj, buf, nbytes, allocKind);

    return obj;
  }

  static TypedArrayObject* makeTypedArrayWithTemplate(
      JSContext* cx, TypedArrayObject* templateObj, HandleObject array) {
    MOZ_ASSERT(!IsWrapper(array));
    MOZ_ASSERT(!array->is<ArrayBufferObjectMaybeShared>());

    return fromArray(cx, array);
  }

  static TypedArrayObject* makeTypedArrayWithTemplate(
      JSContext* cx, TypedArrayObject* templateObj, HandleObject arrayBuffer,
      HandleValue byteOffsetValue, HandleValue lengthValue) {
    MOZ_ASSERT(!IsWrapper(arrayBuffer));
    MOZ_ASSERT(arrayBuffer->is<ArrayBufferObjectMaybeShared>());

    uint64_t byteOffset, length;
    if (!byteOffsetAndLength(cx, byteOffsetValue, lengthValue, &byteOffset,
                             &length)) {
      return nullptr;
    }

    return fromBufferSameCompartment(
        cx, arrayBuffer.as<ArrayBufferObjectMaybeShared>(), byteOffset, length,
        nullptr);
  }

  // ES2023 draft rev cf86f1cdc28e809170733d74ea64fd0f3dd79f78
  // 23.2.5.1 TypedArray ( ...args )
  static bool class_constructor(JSContext* cx, unsigned argc, Value* vp) {
    AutoJSConstructorProfilerEntry pseudoFrame(cx, "[TypedArray]");
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    if (!ThrowIfNotConstructing(cx, args, "typed array")) {
      return false;
    }

    // Steps 2-6.
    JSObject* obj = create(cx, args);
    if (!obj) {
      return false;
    }
    args.rval().setObject(*obj);
    return true;
  }

 private:
  static JSObject* create(JSContext* cx, const CallArgs& args) {
    MOZ_ASSERT(args.isConstructing());

    // Steps 5 and 6.c.
    if (args.length() == 0 || !args[0].isObject()) {
      // Step 6.c.ii.
      uint64_t len;
      if (!ToIndex(cx, args.get(0), JSMSG_BAD_ARRAY_LENGTH, &len)) {
        return nullptr;
      }

      // Steps 5.a and 6.c.iii.
      RootedObject proto(cx);
      if (!GetPrototypeFromBuiltinConstructor(cx, args, protoKey(), &proto)) {
        return nullptr;
      }

      return fromLength(cx, len, proto);
    }

    RootedObject dataObj(cx, &args[0].toObject());

    // Step 6.b.i.
    // 23.2.5.1.1 AllocateTypedArray, step 1.
    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, protoKey(), &proto)) {
      return nullptr;
    }

    // Steps 6.b.ii and 6.b.iv.
    if (!UncheckedUnwrap(dataObj)->is<ArrayBufferObjectMaybeShared>()) {
      return fromArray(cx, dataObj, proto);
    }

    // Steps 6.b.iii.1-2.
    // 23.2.5.1.3 InitializeTypedArrayFromArrayBuffer, steps 2 and 4.
    uint64_t byteOffset, length;
    if (!byteOffsetAndLength(cx, args.get(1), args.get(2), &byteOffset,
                             &length)) {
      return nullptr;
    }

    // Step 6.b.iii.3.
    if (dataObj->is<ArrayBufferObjectMaybeShared>()) {
      HandleArrayBufferObjectMaybeShared buffer =
          dataObj.as<ArrayBufferObjectMaybeShared>();
      return fromBufferSameCompartment(cx, buffer, byteOffset, length, proto);
    }
    return fromBufferWrapped(cx, dataObj, byteOffset, length, proto);
  }

  // ES2023 draft rev cf86f1cdc28e809170733d74ea64fd0f3dd79f78
  // 23.2.5.1.3 InitializeTypedArrayFromArrayBuffer ( O, buffer, byteOffset,
  // length ) Steps 2 and 4.
  static bool byteOffsetAndLength(JSContext* cx, HandleValue byteOffsetValue,
                                  HandleValue lengthValue, uint64_t* byteOffset,
                                  uint64_t* length) {
    // Step 2.
    *byteOffset = 0;
    if (!byteOffsetValue.isUndefined()) {
      if (!ToIndex(cx, byteOffsetValue, byteOffset)) {
        return false;
      }

      // Step 7.
      if (*byteOffset % BYTES_PER_ELEMENT != 0) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TYPED_ARRAY_CONSTRUCT_OFFSET_BOUNDS,
                                  Scalar::name(ArrayTypeID()),
                                  Scalar::byteSizeString(ArrayTypeID()));
        return false;
      }
    }

    // Step 4.
    *length = UINT64_MAX;
    if (!lengthValue.isUndefined()) {
      if (!ToIndex(cx, lengthValue, length)) {
        return false;
      }
    }

    return true;
  }

  // ES2023 draft rev cf86f1cdc28e809170733d74ea64fd0f3dd79f78
  // 23.2.5.1.3 InitializeTypedArrayFromArrayBuffer ( O, buffer, byteOffset,
  // length ) Steps 5-8.
  static bool computeAndCheckLength(
      JSContext* cx, HandleArrayBufferObjectMaybeShared bufferMaybeUnwrapped,
      uint64_t byteOffset, uint64_t lengthIndex, size_t* length) {
    MOZ_ASSERT(byteOffset % BYTES_PER_ELEMENT == 0);
    MOZ_ASSERT(byteOffset < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));
    MOZ_ASSERT_IF(lengthIndex != UINT64_MAX,
                  lengthIndex < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));

    // Step 5.
    if (bufferMaybeUnwrapped->isDetached()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TYPED_ARRAY_DETACHED);
      return false;
    }

    // Step 6.
    size_t bufferByteLength = bufferMaybeUnwrapped->byteLength();

    size_t len;
    if (lengthIndex == UINT64_MAX) {
      // Steps 7.a and 7.c.
      if (bufferByteLength % BYTES_PER_ELEMENT != 0) {
        // The given byte array doesn't map exactly to
        // |BYTES_PER_ELEMENT * N|
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TYPED_ARRAY_CONSTRUCT_OFFSET_MISALIGNED,
                                  Scalar::name(ArrayTypeID()),
                                  Scalar::byteSizeString(ArrayTypeID()));
        return false;
      }

      if (byteOffset > bufferByteLength) {
        // |byteOffset| is invalid.
        JS_ReportErrorNumberASCII(
            cx, GetErrorMessage, nullptr,
            JSMSG_TYPED_ARRAY_CONSTRUCT_OFFSET_LENGTH_BOUNDS,
            Scalar::name(ArrayTypeID()));
        return false;
      }

      // Step 7.b.
      size_t newByteLength = bufferByteLength - size_t(byteOffset);
      len = newByteLength / BYTES_PER_ELEMENT;
    } else {
      // Step 8.a.
      uint64_t newByteLength = lengthIndex * BYTES_PER_ELEMENT;

      // Step 8.b.
      if (byteOffset + newByteLength > bufferByteLength) {
        // |byteOffset + newByteLength| is too big for the arraybuffer
        JS_ReportErrorNumberASCII(
            cx, GetErrorMessage, nullptr,
            JSMSG_TYPED_ARRAY_CONSTRUCT_ARRAY_LENGTH_BOUNDS,
            Scalar::name(ArrayTypeID()));
        return false;
      }

      len = size_t(lengthIndex);
    }

    if (len > MaxByteLength / BYTES_PER_ELEMENT) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TYPED_ARRAY_CONSTRUCT_TOO_LARGE,
                                Scalar::name(ArrayTypeID()));
      return false;
    }

    MOZ_ASSERT(len < SIZE_MAX);
    *length = len;
    return true;
  }

  // ES2023 draft rev cf86f1cdc28e809170733d74ea64fd0f3dd79f78
  // 23.2.5.1.3 InitializeTypedArrayFromArrayBuffer ( O, buffer, byteOffset,
  // length ) Steps 5-13.
  static TypedArrayObject* fromBufferSameCompartment(
      JSContext* cx, HandleArrayBufferObjectMaybeShared buffer,
      uint64_t byteOffset, uint64_t lengthIndex, HandleObject proto) {
    // Steps 5-8.
    size_t length = 0;
    if (!computeAndCheckLength(cx, buffer, byteOffset, lengthIndex, &length)) {
      return nullptr;
    }

    // Steps 9-13.
    return makeInstance(cx, buffer, byteOffset, length, proto);
  }

  // Create a TypedArray object in another compartment.
  //
  // ES6 supports creating a TypedArray in global A (using global A's
  // TypedArray constructor) backed by an ArrayBuffer created in global B.
  //
  // Our TypedArrayObject implementation doesn't support a TypedArray in
  // compartment A backed by an ArrayBuffer in compartment B. So in this
  // case, we create the TypedArray in B (!) and return a cross-compartment
  // wrapper.
  //
  // Extra twist: the spec says the new TypedArray's [[Prototype]] must be
  // A's TypedArray.prototype. So even though we're creating the TypedArray
  // in B, its [[Prototype]] must be (a cross-compartment wrapper for) the
  // TypedArray.prototype in A.
  static JSObject* fromBufferWrapped(JSContext* cx, HandleObject bufobj,
                                     uint64_t byteOffset, uint64_t lengthIndex,
                                     HandleObject proto) {
    JSObject* unwrapped = CheckedUnwrapStatic(bufobj);
    if (!unwrapped) {
      ReportAccessDenied(cx);
      return nullptr;
    }

    if (!unwrapped->is<ArrayBufferObjectMaybeShared>()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TYPED_ARRAY_BAD_ARGS);
      return nullptr;
    }

    RootedArrayBufferObjectMaybeShared unwrappedBuffer(cx);
    unwrappedBuffer = &unwrapped->as<ArrayBufferObjectMaybeShared>();

    size_t length = 0;
    if (!computeAndCheckLength(cx, unwrappedBuffer, byteOffset, lengthIndex,
                               &length)) {
      return nullptr;
    }

    // Make sure to get the [[Prototype]] for the created typed array from
    // this compartment.
    RootedObject protoRoot(cx, proto);
    if (!protoRoot) {
      protoRoot = GlobalObject::getOrCreatePrototype(cx, protoKey());
      if (!protoRoot) {
        return nullptr;
      }
    }

    RootedObject typedArray(cx);
    {
      JSAutoRealm ar(cx, unwrappedBuffer);

      RootedObject wrappedProto(cx, protoRoot);
      if (!cx->compartment()->wrap(cx, &wrappedProto)) {
        return nullptr;
      }

      typedArray =
          makeInstance(cx, unwrappedBuffer, byteOffset, length, wrappedProto);
      if (!typedArray) {
        return nullptr;
      }
    }

    if (!cx->compartment()->wrap(cx, &typedArray)) {
      return nullptr;
    }

    return typedArray;
  }

 public:
  static JSObject* fromBuffer(JSContext* cx, HandleObject bufobj,
                              size_t byteOffset, int64_t lengthInt) {
    if (byteOffset % BYTES_PER_ELEMENT != 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TYPED_ARRAY_CONSTRUCT_OFFSET_BOUNDS,
                                Scalar::name(ArrayTypeID()),
                                Scalar::byteSizeString(ArrayTypeID()));
      return nullptr;  // invalid byteOffset
    }

    uint64_t lengthIndex = lengthInt >= 0 ? uint64_t(lengthInt) : UINT64_MAX;
    if (bufobj->is<ArrayBufferObjectMaybeShared>()) {
      HandleArrayBufferObjectMaybeShared buffer =
          bufobj.as<ArrayBufferObjectMaybeShared>();
      return fromBufferSameCompartment(cx, buffer, byteOffset, lengthIndex,
                                       nullptr);
    }
    return fromBufferWrapped(cx, bufobj, byteOffset, lengthIndex, nullptr);
  }

  static bool maybeCreateArrayBuffer(JSContext* cx, uint64_t count,
                                     MutableHandle<ArrayBufferObject*> buffer) {
    if (count > MaxByteLength / BYTES_PER_ELEMENT) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_ARRAY_LENGTH);
      return false;
    }
    size_t byteLength = count * BYTES_PER_ELEMENT;

    MOZ_ASSERT(byteLength <= MaxByteLength);
    static_assert(INLINE_BUFFER_LIMIT % BYTES_PER_ELEMENT == 0,
                  "ArrayBuffer inline storage shouldn't waste any space");

    if (byteLength <= INLINE_BUFFER_LIMIT) {
      // The array's data can be inline, and the buffer created lazily.
      return true;
    }

    ArrayBufferObject* buf = ArrayBufferObject::createZeroed(cx, byteLength);
    if (!buf) {
      return false;
    }

    buffer.set(buf);
    return true;
  }

  // ES2023 draft rev cf86f1cdc28e809170733d74ea64fd0f3dd79f78
  // 23.2.5.1.1 AllocateTypedArray ( constructorName, newTarget, defaultProto [
  // , length ] )
  static TypedArrayObject* fromLength(JSContext* cx, uint64_t nelements,
                                      HandleObject proto = nullptr,
                                      gc::Heap heap = gc::Heap::Default) {
    Rooted<ArrayBufferObject*> buffer(cx);
    if (!maybeCreateArrayBuffer(cx, nelements, &buffer)) {
      return nullptr;
    }

    return makeInstance(cx, buffer, 0, nelements, proto, heap);
  }

  static TypedArrayObject* fromArray(JSContext* cx, HandleObject other,
                                     HandleObject proto = nullptr);

  static TypedArrayObject* fromTypedArray(JSContext* cx, HandleObject other,
                                          bool isWrapped, HandleObject proto);

  static TypedArrayObject* fromObject(JSContext* cx, HandleObject other,
                                      HandleObject proto);

  static const NativeType getIndex(TypedArrayObject* tarray, size_t index) {
    MOZ_ASSERT(index < tarray->length());
    return jit::AtomicOperations::loadSafeWhenRacy(
        tarray->dataPointerEither().cast<NativeType*>() + index);
  }

  static void setIndex(TypedArrayObject& tarray, size_t index, NativeType val) {
    MOZ_ASSERT(index < tarray.length());
    jit::AtomicOperations::storeSafeWhenRacy(
        tarray.dataPointerEither().cast<NativeType*>() + index, val);
  }

  static bool getElement(JSContext* cx, TypedArrayObject* tarray, size_t index,
                         MutableHandleValue val);
  static bool getElementPure(TypedArrayObject* tarray, size_t index, Value* vp);

  static bool setElement(JSContext* cx, Handle<TypedArrayObject*> obj,
                         uint64_t index, HandleValue v, ObjectOpResult& result);
};

template <typename NativeType>
bool TypedArrayObjectTemplate<NativeType>::convertValue(JSContext* cx,
                                                        HandleValue v,
                                                        NativeType* result) {
  double d;
  if (!ToNumber(cx, v, &d)) {
    return false;
  }

  if (js::SupportDifferentialTesting()) {
    // See the comment in ElementSpecific::doubleToNative.
    d = JS::CanonicalizeNaN(d);
  }

  // Assign based on characteristics of the destination type
  if constexpr (ArrayTypeIsFloatingPoint()) {
    *result = NativeType(d);
  } else if constexpr (ArrayTypeIsUnsigned()) {
    static_assert(sizeof(NativeType) <= 4);
    uint32_t n = ToUint32(d);
    *result = NativeType(n);
  } else if constexpr (ArrayTypeID() == Scalar::Uint8Clamped) {
    // The uint8_clamped type has a special rounding converter
    // for doubles.
    *result = NativeType(d);
  } else {
    static_assert(sizeof(NativeType) <= 4);
    int32_t n = ToInt32(d);
    *result = NativeType(n);
  }
  return true;
}

template <>
bool TypedArrayObjectTemplate<int64_t>::convertValue(JSContext* cx,
                                                     HandleValue v,
                                                     int64_t* result) {
  JS_TRY_VAR_OR_RETURN_FALSE(cx, *result, ToBigInt64(cx, v));
  return true;
}

template <>
bool TypedArrayObjectTemplate<uint64_t>::convertValue(JSContext* cx,
                                                      HandleValue v,
                                                      uint64_t* result) {
  JS_TRY_VAR_OR_RETURN_FALSE(cx, *result, ToBigUint64(cx, v));
  return true;
}

// https://tc39.github.io/proposal-bigint/#sec-integerindexedelementset
// 9.4.5.11 IntegerIndexedElementSet ( O, index, value )
template <typename NativeType>
/* static */ bool TypedArrayObjectTemplate<NativeType>::setElement(
    JSContext* cx, Handle<TypedArrayObject*> obj, uint64_t index, HandleValue v,
    ObjectOpResult& result) {
  MOZ_ASSERT(!obj->hasDetachedBuffer());
  MOZ_ASSERT(index < obj->length());

  // Step 1 is enforced by the caller.

  // Steps 2-3.
  NativeType nativeValue;
  if (!convertValue(cx, v, &nativeValue)) {
    return false;
  }

  // Step 4.
  if (index < obj->length()) {
    MOZ_ASSERT(!obj->hasDetachedBuffer(),
               "detaching an array buffer sets the length to zero");
    TypedArrayObjectTemplate<NativeType>::setIndex(*obj, index, nativeValue);
  }

  // Step 5.
  return result.succeed();
}

#define CREATE_TYPE_FOR_TYPED_ARRAY(_, T, N) \
  typedef TypedArrayObjectTemplate<T> N##Array;
JS_FOR_EACH_TYPED_ARRAY(CREATE_TYPE_FOR_TYPED_ARRAY)
#undef CREATE_TYPE_FOR_TYPED_ARRAY

} /* anonymous namespace */

TypedArrayObject* js::NewTypedArrayWithTemplateAndLength(
    JSContext* cx, HandleObject templateObj, int32_t len) {
  MOZ_ASSERT(templateObj->is<TypedArrayObject>());
  TypedArrayObject* tobj = &templateObj->as<TypedArrayObject>();

  switch (tobj->type()) {
#define CREATE_TYPED_ARRAY(_, T, N)                                          \
  case Scalar::N:                                                            \
    return TypedArrayObjectTemplate<T>::makeTypedArrayWithTemplate(cx, tobj, \
                                                                   len);
    JS_FOR_EACH_TYPED_ARRAY(CREATE_TYPED_ARRAY)
#undef CREATE_TYPED_ARRAY
    default:
      MOZ_CRASH("Unsupported TypedArray type");
  }
}

TypedArrayObject* js::NewTypedArrayWithTemplateAndArray(
    JSContext* cx, HandleObject templateObj, HandleObject array) {
  MOZ_ASSERT(templateObj->is<TypedArrayObject>());
  TypedArrayObject* tobj = &templateObj->as<TypedArrayObject>();

  switch (tobj->type()) {
#define CREATE_TYPED_ARRAY(_, T, N)                                          \
  case Scalar::N:                                                            \
    return TypedArrayObjectTemplate<T>::makeTypedArrayWithTemplate(cx, tobj, \
                                                                   array);
    JS_FOR_EACH_TYPED_ARRAY(CREATE_TYPED_ARRAY)
#undef CREATE_TYPED_ARRAY
    default:
      MOZ_CRASH("Unsupported TypedArray type");
  }
}

TypedArrayObject* js::NewTypedArrayWithTemplateAndBuffer(
    JSContext* cx, HandleObject templateObj, HandleObject arrayBuffer,
    HandleValue byteOffset, HandleValue length) {
  MOZ_ASSERT(templateObj->is<TypedArrayObject>());
  TypedArrayObject* tobj = &templateObj->as<TypedArrayObject>();

  switch (tobj->type()) {
#define CREATE_TYPED_ARRAY(_, T, N)                                 \
  case Scalar::N:                                                   \
    return TypedArrayObjectTemplate<T>::makeTypedArrayWithTemplate( \
        cx, tobj, arrayBuffer, byteOffset, length);
    JS_FOR_EACH_TYPED_ARRAY(CREATE_TYPED_ARRAY)
#undef CREATE_TYPED_ARRAY
    default:
      MOZ_CRASH("Unsupported TypedArray type");
  }
}

TypedArrayObject* js::NewUint8ArrayWithLength(JSContext* cx, int32_t len,
                                              gc::Heap heap) {
  return TypedArrayObjectTemplate<uint8_t>::fromLength(cx, len, nullptr, heap);
}

template <typename T>
/* static */ TypedArrayObject* TypedArrayObjectTemplate<T>::fromArray(
    JSContext* cx, HandleObject other, HandleObject proto /* = nullptr */) {
  // Allow nullptr proto for FriendAPI methods, which don't care about
  // subclassing.
  if (other->is<TypedArrayObject>()) {
    return fromTypedArray(cx, other, /* wrapped= */ false, proto);
  }

  if (other->is<WrapperObject>() &&
      UncheckedUnwrap(other)->is<TypedArrayObject>()) {
    return fromTypedArray(cx, other, /* wrapped= */ true, proto);
  }

  return fromObject(cx, other, proto);
}

// ES2023 draft rev cf86f1cdc28e809170733d74ea64fd0f3dd79f78
// 23.2.5.1 TypedArray ( ...args )
// 23.2.5.1.2 InitializeTypedArrayFromTypedArray ( O, srcArray )
template <typename T>
/* static */ TypedArrayObject* TypedArrayObjectTemplate<T>::fromTypedArray(
    JSContext* cx, HandleObject other, bool isWrapped, HandleObject proto) {
  MOZ_ASSERT_IF(!isWrapped, other->is<TypedArrayObject>());
  MOZ_ASSERT_IF(isWrapped, other->is<WrapperObject>() &&
                               UncheckedUnwrap(other)->is<TypedArrayObject>());

  Rooted<TypedArrayObject*> srcArray(cx);
  if (!isWrapped) {
    srcArray = &other->as<TypedArrayObject>();
  } else {
    srcArray = other->maybeUnwrapAs<TypedArrayObject>();
    if (!srcArray) {
      ReportAccessDenied(cx);
      return nullptr;
    }
  }

  // InitializeTypedArrayFromTypedArray, step 1. (Skipped)

  // InitializeTypedArrayFromTypedArray, step 2.
  if (srcArray->hasDetachedBuffer()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }

  // InitializeTypedArrayFromTypedArray, steps 3-7. (Skipped)

  // InitializeTypedArrayFromTypedArray, step 8.
  size_t elementLength = srcArray->length();

  // InitializeTypedArrayFromTypedArray, step 9. (Skipped)

  // InitializeTypedArrayFromTypedArray, step 10.a. (Partial)
  // InitializeTypedArrayFromTypedArray, step 11.a.
  Rooted<ArrayBufferObject*> buffer(cx);
  if (!maybeCreateArrayBuffer(cx, elementLength, &buffer)) {
    return nullptr;
  }

  // InitializeTypedArrayFromTypedArray, step 11.b.
  if (Scalar::isBigIntType(ArrayTypeID()) !=
      Scalar::isBigIntType(srcArray->type())) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_NOT_COMPATIBLE,
                              srcArray->getClass()->name,
                              TypedArrayObject::classes[ArrayTypeID()].name);
    return nullptr;
  }

  // Step 6.b.i.
  // InitializeTypedArrayFromTypedArray, steps 12-15.
  Rooted<TypedArrayObject*> obj(
      cx, makeInstance(cx, buffer, 0, elementLength, proto));
  if (!obj) {
    return nullptr;
  }

  MOZ_RELEASE_ASSERT(!srcArray->hasDetachedBuffer());

  // InitializeTypedArrayFromTypedArray, steps 10.a. (Remaining parts)
  // InitializeTypedArrayFromTypedArray, steps 11.c-f.
  MOZ_ASSERT(!obj->isSharedMemory());
  if (srcArray->isSharedMemory()) {
    if (!ElementSpecific<T, SharedOps>::setFromTypedArray(obj, srcArray, 0)) {
      return nullptr;
    }
  } else {
    if (!ElementSpecific<T, UnsharedOps>::setFromTypedArray(obj, srcArray, 0)) {
      return nullptr;
    }
  }

  // Step 6.b.v.
  return obj;
}

static MOZ_ALWAYS_INLINE bool IsOptimizableInit(JSContext* cx,
                                                HandleObject iterable,
                                                bool* optimized) {
  MOZ_ASSERT(!*optimized);

  if (!IsPackedArray(iterable)) {
    return true;
  }

  ForOfPIC::Chain* stubChain = ForOfPIC::getOrCreate(cx);
  if (!stubChain) {
    return false;
  }

  return stubChain->tryOptimizeArray(cx, iterable.as<ArrayObject>(), optimized);
}

// ES2023 draft rev cf86f1cdc28e809170733d74ea64fd0f3dd79f78
// 23.2.5.1 TypedArray ( ...args )
// 23.2.5.1.4 InitializeTypedArrayFromList ( O, values )
// 23.2.5.1.5 InitializeTypedArrayFromArrayLike ( O, arrayLike )
template <typename T>
/* static */ TypedArrayObject* TypedArrayObjectTemplate<T>::fromObject(
    JSContext* cx, HandleObject other, HandleObject proto) {
  // Steps 1-4 and 6.a (Already performed in caller).

  // Steps 6.b.i (Allocation deferred until later).

  // Steps 6.b.ii-iii. (Not applicable)

  // Step 6.b.iv.

  bool optimized = false;
  if (!IsOptimizableInit(cx, other, &optimized)) {
    return nullptr;
  }

  // Fast path when iterable is a packed array using the default iterator.
  if (optimized) {
    // Steps 6.b.iv.2-3. (We don't need to call IterableToList for the fast
    // path).
    Handle<ArrayObject*> array = other.as<ArrayObject>();

    // InitializeTypedArrayFromList, step 1.
    size_t len = array->getDenseInitializedLength();

    // InitializeTypedArrayFromList, step 2.
    Rooted<ArrayBufferObject*> buffer(cx);
    if (!maybeCreateArrayBuffer(cx, len, &buffer)) {
      return nullptr;
    }

    // Steps 6.b.i.
    Rooted<TypedArrayObject*> obj(cx, makeInstance(cx, buffer, 0, len, proto));
    if (!obj) {
      return nullptr;
    }

    // InitializeTypedArrayFromList, steps 3-4.
    MOZ_ASSERT(!obj->isSharedMemory());
    if (!ElementSpecific<T, UnsharedOps>::initFromIterablePackedArray(cx, obj,
                                                                      array)) {
      return nullptr;
    }

    // InitializeTypedArrayFromList, step 5. (The assertion isn't applicable for
    // the fast path).

    // Step 6.b.v.
    return obj;
  }

  // Step 6.b.iv.1 (Assertion; implicit in our implementation).

  // Step 6.b.iv.2.
  RootedValue callee(cx);
  RootedId iteratorId(cx, PropertyKey::Symbol(cx->wellKnownSymbols().iterator));
  if (!GetProperty(cx, other, other, iteratorId, &callee)) {
    return nullptr;
  }

  // Steps 6.b.iv.3-4.
  RootedObject arrayLike(cx);
  if (!callee.isNullOrUndefined()) {
    // Throw if other[Symbol.iterator] isn't callable.
    if (!callee.isObject() || !callee.toObject().isCallable()) {
      RootedValue otherVal(cx, ObjectValue(*other));
      UniqueChars bytes =
          DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, otherVal, nullptr);
      if (!bytes) {
        return nullptr;
      }
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_NOT_ITERABLE,
                               bytes.get());
      return nullptr;
    }

    FixedInvokeArgs<2> args2(cx);
    args2[0].setObject(*other);
    args2[1].set(callee);

    // Step 6.b.iv.3.a.
    RootedValue rval(cx);
    if (!CallSelfHostedFunction(cx, cx->names().IterableToList,
                                UndefinedHandleValue, args2, &rval)) {
      return nullptr;
    }

    // Step 6.b.iv.3.b (Implemented below).
    arrayLike = &rval.toObject();
  } else {
    // Step 4.a is an assertion: object is not an Iterator. Testing this is
    // literally the very last thing we did, so we don't assert here.

    // Step 4.b (Implemented below).
    arrayLike = other;
  }

  // We implement InitializeTypedArrayFromList in terms of
  // InitializeTypedArrayFromArrayLike.

  // InitializeTypedArrayFromArrayLike, step 1.
  uint64_t len;
  if (!GetLengthProperty(cx, arrayLike, &len)) {
    return nullptr;
  }

  // InitializeTypedArrayFromArrayLike, step 2.
  Rooted<ArrayBufferObject*> buffer(cx);
  if (!maybeCreateArrayBuffer(cx, len, &buffer)) {
    return nullptr;
  }

  MOZ_ASSERT(len <= MaxByteLength / BYTES_PER_ELEMENT);

  // Steps 6.b.i.
  Rooted<TypedArrayObject*> obj(cx, makeInstance(cx, buffer, 0, len, proto));
  if (!obj) {
    return nullptr;
  }

  // InitializeTypedArrayFromArrayLike, steps 3-4.
  MOZ_ASSERT(!obj->isSharedMemory());
  if (!ElementSpecific<T, UnsharedOps>::setFromNonTypedArray(cx, obj, arrayLike,
                                                             len)) {
    return nullptr;
  }

  // Step 6.b.v.
  return obj;
}

bool TypedArrayConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TYPED_ARRAY_CALL_OR_CONSTRUCT,
                            args.isConstructing() ? "construct" : "call");
  return false;
}

template <typename T>
static bool GetTemplateObjectForNative(JSContext* cx,
                                       const JS::HandleValueArray args,
                                       MutableHandleObject res) {
  if (args.length() == 0) {
    return true;
  }

  HandleValue arg = args[0];
  if (arg.isInt32()) {
    uint32_t len = 0;
    if (arg.toInt32() >= 0) {
      len = arg.toInt32();
    }

    size_t nbytes;
    if (!js::CalculateAllocSize<T>(len, &nbytes) ||
        nbytes > TypedArrayObject::MaxByteLength) {
      return true;
    }

    res.set(TypedArrayObjectTemplate<T>::makeTemplateObject(cx, len));
    return !!res;
  }

  // We don't support wrappers, because of the complicated interaction between
  // wrapped ArrayBuffers and TypedArrays, see |fromBufferWrapped()|.
  if (arg.isObject() && !IsWrapper(&arg.toObject())) {
    // We don't use the template's length in the object case, so we can create
    // the template typed array with an initial length of zero.
    uint32_t len = 0;
    res.set(TypedArrayObjectTemplate<T>::makeTemplateObject(cx, len));
    return !!res;
  }

  return true;
}

/* static */ bool TypedArrayObject::GetTemplateObjectForNative(
    JSContext* cx, Native native, const JS::HandleValueArray args,
    MutableHandleObject res) {
  MOZ_ASSERT(!res);
#define CHECK_TYPED_ARRAY_CONSTRUCTOR(_, T, N)                     \
  if (native == &TypedArrayObjectTemplate<T>::class_constructor) { \
    return ::GetTemplateObjectForNative<T>(cx, args, res);         \
  }
  JS_FOR_EACH_TYPED_ARRAY(CHECK_TYPED_ARRAY_CONSTRUCTOR)
#undef CHECK_TYPED_ARRAY_CONSTRUCTOR
  return true;
}

static bool LengthGetterImpl(JSContext* cx, const CallArgs& args) {
  auto* tarr = &args.thisv().toObject().as<TypedArrayObject>();
  args.rval().set(tarr->lengthValue());
  return true;
}

static bool TypedArray_lengthGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<TypedArrayObject::is, LengthGetterImpl>(cx, args);
}

static bool ByteOffsetGetterImpl(JSContext* cx, const CallArgs& args) {
  auto* tarr = &args.thisv().toObject().as<TypedArrayObject>();
  args.rval().set(tarr->byteOffsetValue());
  return true;
}

static bool TypedArray_byteOffsetGetter(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<TypedArrayObject::is, ByteOffsetGetterImpl>(cx,
                                                                          args);
}

static bool ByteLengthGetterImpl(JSContext* cx, const CallArgs& args) {
  auto* tarr = &args.thisv().toObject().as<TypedArrayObject>();
  args.rval().set(tarr->byteLengthValue());
  return true;
}

static bool TypedArray_byteLengthGetter(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<TypedArrayObject::is, ByteLengthGetterImpl>(cx,
                                                                          args);
}

static bool BufferGetterImpl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(TypedArrayObject::is(args.thisv()));
  Rooted<TypedArrayObject*> tarray(
      cx, &args.thisv().toObject().as<TypedArrayObject>());
  if (!TypedArrayObject::ensureHasBuffer(cx, tarray)) {
    return false;
  }
  args.rval().set(tarray->bufferValue());
  return true;
}

static bool TypedArray_bufferGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<TypedArrayObject::is, BufferGetterImpl>(cx, args);
}

// ES2019 draft rev fc9ecdcd74294d0ca3146d4b274e2a8e79565dc3
// 22.2.3.32 get %TypedArray%.prototype [ @@toStringTag ]
static bool TypedArray_toStringTagGetter(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  if (!args.thisv().isObject()) {
    args.rval().setUndefined();
    return true;
  }

  JSObject* obj = CheckedUnwrapStatic(&args.thisv().toObject());
  if (!obj) {
    ReportAccessDenied(cx);
    return false;
  }

  // Step 3.
  if (!obj->is<TypedArrayObject>()) {
    args.rval().setUndefined();
    return true;
  }

  // Steps 4-6.
  JSProtoKey protoKey = StandardProtoKeyOrNull(obj);
  MOZ_ASSERT(protoKey);

  args.rval().setString(ClassName(protoKey, cx));
  return true;
}

/* static */ const JSPropertySpec TypedArrayObject::protoAccessors[] = {
    JS_PSG("length", TypedArray_lengthGetter, 0),
    JS_PSG("buffer", TypedArray_bufferGetter, 0),
    JS_PSG("byteLength", TypedArray_byteLengthGetter, 0),
    JS_PSG("byteOffset", TypedArray_byteOffsetGetter, 0),
    JS_SYM_GET(toStringTag, TypedArray_toStringTagGetter, 0),
    JS_PS_END};

template <typename T>
static inline bool SetFromTypedArray(Handle<TypedArrayObject*> target,
                                     Handle<TypedArrayObject*> source,
                                     size_t offset) {
  // WARNING: |source| may be an unwrapped typed array from a different
  // compartment. Proceed with caution!

  if (target->isSharedMemory() || source->isSharedMemory()) {
    return ElementSpecific<T, SharedOps>::setFromTypedArray(target, source,
                                                            offset);
  }
  return ElementSpecific<T, UnsharedOps>::setFromTypedArray(target, source,
                                                            offset);
}

template <typename T>
static inline bool SetFromNonTypedArray(JSContext* cx,
                                        Handle<TypedArrayObject*> target,
                                        HandleObject source, size_t len,
                                        size_t offset) {
  MOZ_ASSERT(!source->is<TypedArrayObject>(), "use SetFromTypedArray");

  if (target->isSharedMemory()) {
    return ElementSpecific<T, SharedOps>::setFromNonTypedArray(
        cx, target, source, len, offset);
  }
  return ElementSpecific<T, UnsharedOps>::setFromNonTypedArray(
      cx, target, source, len, offset);
}

// ES2023 draft rev 22cc56ab08fcab92a865978c0aa5c6f1d8ce250f
// 23.2.3.24.1 SetTypedArrayFromTypedArray ( target, targetOffset, source )
static bool SetTypedArrayFromTypedArray(JSContext* cx,
                                        Handle<TypedArrayObject*> target,
                                        double targetOffset,
                                        Handle<TypedArrayObject*> source) {
  // WARNING: |source| may be an unwrapped typed array from a different
  // compartment. Proceed with caution!

  MOZ_ASSERT(targetOffset >= 0);

  // Steps 1-2. (Performed in caller.)
  MOZ_ASSERT(!target->hasDetachedBuffer());

  // Steps 4-5.
  if (source->hasDetachedBuffer()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return false;
  }

  // Step 3 (Reordered).
  size_t targetLength = target->length();

  // Steps 13-14 (Split into two checks to provide better error messages).
  if (targetOffset > targetLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
    return false;
  }

  // Step 14 (Cont'd).
  size_t offset = size_t(targetOffset);
  if (source->length() > targetLength - offset) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SOURCE_ARRAY_TOO_LONG);
    return false;
  }

  // Step 15.
  if (Scalar::isBigIntType(target->type()) !=
      Scalar::isBigIntType(source->type())) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_NOT_COMPATIBLE,
        source->getClass()->name, target->getClass()->name);
    return false;
  }

  // Steps 6-12, 16-24.
  switch (target->type()) {
#define SET_FROM_TYPED_ARRAY(_, T, N)                                \
  case Scalar::N:                                                    \
    if (!SetFromTypedArray<T>(target, source, offset)) return false; \
    break;
    JS_FOR_EACH_TYPED_ARRAY(SET_FROM_TYPED_ARRAY)
#undef SET_FROM_TYPED_ARRAY
    default:
      MOZ_CRASH("Unsupported TypedArray type");
  }

  return true;
}

// ES2023 draft rev 22cc56ab08fcab92a865978c0aa5c6f1d8ce250f
// 23.2.3.24.1 SetTypedArrayFromArrayLike ( target, targetOffset, source )
static bool SetTypedArrayFromArrayLike(JSContext* cx,
                                       Handle<TypedArrayObject*> target,
                                       double targetOffset, HandleObject src) {
  MOZ_ASSERT(targetOffset >= 0);

  // Steps 1-2. (Performed in caller.)
  MOZ_ASSERT(!target->hasDetachedBuffer());

  // Step 3.
  // We can't reorder this step because side-effects in step 5 can detach the
  // underlying array buffer from the typed array.
  size_t targetLength = target->length();

  // Step 4. (Performed in caller.)

  // Step 5.
  uint64_t srcLength;
  if (!GetLengthProperty(cx, src, &srcLength)) {
    return false;
  }

  // Steps 6-7 (Split into two checks to provide better error messages).
  if (targetOffset > targetLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
    return false;
  }

  // Step 7 (Cont'd).
  size_t offset = size_t(targetOffset);
  if (srcLength > targetLength - offset) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SOURCE_ARRAY_TOO_LONG);
    return false;
  }

  MOZ_ASSERT(srcLength <= targetLength);

  // Steps 8-9.
  if (srcLength > 0) {
    switch (target->type()) {
#define SET_FROM_NON_TYPED_ARRAY(_, T, N)                             \
  case Scalar::N:                                                     \
    if (!SetFromNonTypedArray<T>(cx, target, src, srcLength, offset)) \
      return false;                                                   \
    break;
      JS_FOR_EACH_TYPED_ARRAY(SET_FROM_NON_TYPED_ARRAY)
#undef SET_FROM_NON_TYPED_ARRAY
      default:
        MOZ_CRASH("Unsupported TypedArray type");
    }
  }

  // Step 10.
  return true;
}

// ES2023 draft rev 22cc56ab08fcab92a865978c0aa5c6f1d8ce250f
// 23.2.3.24 %TypedArray%.prototype.set ( source [ , offset ] )
// 23.2.3.24.1 SetTypedArrayFromTypedArray ( target, targetOffset, source )
// 23.2.3.24.2 SetTypedArrayFromArrayLike ( target, targetOffset, source )
/* static */
bool TypedArrayObject::set_impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(TypedArrayObject::is(args.thisv()));

  // Steps 1-3 (Validation performed as part of CallNonGenericMethod).
  Rooted<TypedArrayObject*> target(
      cx, &args.thisv().toObject().as<TypedArrayObject>());

  // Steps 4-5.
  double targetOffset = 0;
  if (args.length() > 1) {
    // Step 4.
    if (!ToInteger(cx, args[1], &targetOffset)) {
      return false;
    }

    // Step 5.
    if (targetOffset < 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
      return false;
    }
  }

  // 23.2.3.24.1, steps 1-2.
  // 23.2.3.24.2, steps 1-2.
  if (target->hasDetachedBuffer()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return false;
  }

  // 23.2.3.24.2, step 4. (23.2.3.24.1 only applies if args[0] is a typed
  // array, so it doesn't make a difference there to apply ToObject here.)
  RootedObject src(cx, ToObject(cx, args.get(0)));
  if (!src) {
    return false;
  }

  Rooted<TypedArrayObject*> srcTypedArray(cx);
  {
    JSObject* obj = CheckedUnwrapStatic(src);
    if (!obj) {
      ReportAccessDenied(cx);
      return false;
    }

    if (obj->is<TypedArrayObject>()) {
      srcTypedArray = &obj->as<TypedArrayObject>();
    }
  }

  // Steps 6-7.
  if (srcTypedArray) {
    if (!SetTypedArrayFromTypedArray(cx, target, targetOffset, srcTypedArray)) {
      return false;
    }
  } else {
    if (!SetTypedArrayFromArrayLike(cx, target, targetOffset, src)) {
      return false;
    }
  }

  // Step 8.
  args.rval().setUndefined();
  return true;
}

/* static */
bool TypedArrayObject::set(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<TypedArrayObject::is, TypedArrayObject::set_impl>(
      cx, args);
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.2.3.5 %TypedArray%.prototype.copyWithin ( target, start [ , end ] )
/* static */
bool TypedArrayObject::copyWithin_impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(TypedArrayObject::is(args.thisv()));

  // Steps 1-2.
  Rooted<TypedArrayObject*> tarray(
      cx, &args.thisv().toObject().as<TypedArrayObject>());
  if (tarray->hasDetachedBuffer()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return false;
  }

  // Step 3.
  size_t len = tarray->length();

  // Step 4.
  double relativeTarget;
  if (!ToInteger(cx, args.get(0), &relativeTarget)) {
    return false;
  }

  // Step 5.
  uint64_t to;
  if (relativeTarget < 0) {
    to = std::max(len + relativeTarget, 0.0);
  } else {
    to = std::min(relativeTarget, double(len));
  }

  // Step 6.
  double relativeStart;
  if (!ToInteger(cx, args.get(1), &relativeStart)) {
    return false;
  }

  // Step 7.
  uint64_t from;
  if (relativeStart < 0) {
    from = std::max(len + relativeStart, 0.0);
  } else {
    from = std::min(relativeStart, double(len));
  }

  // Step 8.
  double relativeEnd;
  if (!args.hasDefined(2)) {
    relativeEnd = len;
  } else {
    if (!ToInteger(cx, args[2], &relativeEnd)) {
      return false;
    }
  }

  // Step 9.
  uint64_t final_;
  if (relativeEnd < 0) {
    final_ = std::max(len + relativeEnd, 0.0);
  } else {
    final_ = std::min(relativeEnd, double(len));
  }

  // Step 10.
  MOZ_ASSERT(to <= len);
  uint64_t count;
  if (from <= final_) {
    count = std::min(final_ - from, len - to);
  } else {
    count = 0;
  }

  // Step 11.
  //
  // Note that getting or setting a typed array element must throw if the
  // underlying buffer is detached, so the code below checks for detachment.
  // This happens *only* if a get/set occurs, i.e. when |count > 0|.
  //
  // Also note that this copies elements effectively by memmove, *not* in
  // step 11's specified order.  This is unobservable, even when the underlying
  // buffer is a SharedArrayBuffer instance, because the access is unordered and
  // therefore is allowed to have data races.

  if (count == 0) {
    args.rval().setObject(*tarray);
    return true;
  }

  if (tarray->hasDetachedBuffer()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return false;
  }

  // Don't multiply by |tarray->bytesPerElement()| in case the compiler can't
  // strength-reduce multiplication by 1/2/4/8 into the equivalent shift.
  const size_t ElementShift = TypedArrayShift(tarray->type());

  MOZ_ASSERT((SIZE_MAX >> ElementShift) > to);
  size_t byteDest = to << ElementShift;

  MOZ_ASSERT((SIZE_MAX >> ElementShift) > from);
  size_t byteSrc = from << ElementShift;

  MOZ_ASSERT((SIZE_MAX >> ElementShift) >= count);
  size_t byteSize = count << ElementShift;

#ifdef DEBUG
  {
    size_t viewByteLength = tarray->byteLength();
    MOZ_ASSERT(byteSize <= viewByteLength);
    MOZ_ASSERT(byteDest < viewByteLength);
    MOZ_ASSERT(byteSrc < viewByteLength);
    MOZ_ASSERT(byteDest <= viewByteLength - byteSize);
    MOZ_ASSERT(byteSrc <= viewByteLength - byteSize);
  }
#endif

  SharedMem<uint8_t*> data = tarray->dataPointerEither().cast<uint8_t*>();
  if (tarray->isSharedMemory()) {
    jit::AtomicOperations::memmoveSafeWhenRacy(data + byteDest, data + byteSrc,
                                               byteSize);
  } else {
    memmove(data.unwrapUnshared() + byteDest, data.unwrapUnshared() + byteSrc,
            byteSize);
  }

  args.rval().setObject(*tarray);
  return true;
}

/* static */
bool TypedArrayObject::copyWithin(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "[TypedArray].prototype",
                                        "copyWithin");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<TypedArrayObject::is,
                              TypedArrayObject::copyWithin_impl>(cx, args);
}

/* static */ const JSFunctionSpec TypedArrayObject::protoFunctions[] = {
    JS_SELF_HOSTED_FN("subarray", "TypedArraySubarray", 2, 0),
    JS_FN("set", TypedArrayObject::set, 1, 0),
    JS_FN("copyWithin", TypedArrayObject::copyWithin, 2, 0),
    JS_SELF_HOSTED_FN("every", "TypedArrayEvery", 1, 0),
    JS_SELF_HOSTED_FN("fill", "TypedArrayFill", 3, 0),
    JS_SELF_HOSTED_FN("filter", "TypedArrayFilter", 1, 0),
    JS_SELF_HOSTED_FN("find", "TypedArrayFind", 1, 0),
    JS_SELF_HOSTED_FN("findIndex", "TypedArrayFindIndex", 1, 0),
    JS_SELF_HOSTED_FN("findLast", "TypedArrayFindLast", 1, 0),
    JS_SELF_HOSTED_FN("findLastIndex", "TypedArrayFindLastIndex", 1, 0),
    JS_SELF_HOSTED_FN("forEach", "TypedArrayForEach", 1, 0),
    JS_SELF_HOSTED_FN("indexOf", "TypedArrayIndexOf", 2, 0),
    JS_SELF_HOSTED_FN("join", "TypedArrayJoin", 1, 0),
    JS_SELF_HOSTED_FN("lastIndexOf", "TypedArrayLastIndexOf", 1, 0),
    JS_SELF_HOSTED_FN("map", "TypedArrayMap", 1, 0),
    JS_SELF_HOSTED_FN("reduce", "TypedArrayReduce", 1, 0),
    JS_SELF_HOSTED_FN("reduceRight", "TypedArrayReduceRight", 1, 0),
    JS_SELF_HOSTED_FN("reverse", "TypedArrayReverse", 0, 0),
    JS_SELF_HOSTED_FN("slice", "TypedArraySlice", 2, 0),
    JS_SELF_HOSTED_FN("some", "TypedArraySome", 1, 0),
    JS_SELF_HOSTED_FN("sort", "TypedArraySort", 1, 0),
    JS_SELF_HOSTED_FN("entries", "TypedArrayEntries", 0, 0),
    JS_SELF_HOSTED_FN("keys", "TypedArrayKeys", 0, 0),
    JS_SELF_HOSTED_FN("values", "$TypedArrayValues", 0, 0),
    JS_SELF_HOSTED_SYM_FN(iterator, "$TypedArrayValues", 0, 0),
    JS_SELF_HOSTED_FN("includes", "TypedArrayIncludes", 2, 0),
    JS_SELF_HOSTED_FN("toString", "ArrayToString", 0, 0),
    JS_SELF_HOSTED_FN("toLocaleString", "TypedArrayToLocaleString", 2, 0),
    JS_SELF_HOSTED_FN("at", "TypedArrayAt", 1, 0),
    JS_SELF_HOSTED_FN("toReversed", "TypedArrayToReversed", 0, 0),
    JS_SELF_HOSTED_FN("toSorted", "TypedArrayToSorted", 1, 0),
    JS_SELF_HOSTED_FN("with", "TypedArrayWith", 2, 0),
    JS_FS_END,
};

/* static */ const JSFunctionSpec TypedArrayObject::staticFunctions[] = {
    JS_SELF_HOSTED_FN("from", "TypedArrayStaticFrom", 3, 0),
    JS_SELF_HOSTED_FN("of", "TypedArrayStaticOf", 0, 0),
    JS_FS_END,
};

/* static */ const JSPropertySpec TypedArrayObject::staticProperties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "$TypedArraySpecies", 0),
    JS_PS_END,
};

static JSObject* CreateSharedTypedArrayPrototype(JSContext* cx,
                                                 JSProtoKey key) {
  return GlobalObject::createBlankPrototype(
      cx, cx->global(), &TypedArrayObject::sharedTypedArrayPrototypeClass);
}

static const ClassSpec TypedArrayObjectSharedTypedArrayPrototypeClassSpec = {
    GenericCreateConstructor<TypedArrayConstructor, 0, gc::AllocKind::FUNCTION>,
    CreateSharedTypedArrayPrototype,
    TypedArrayObject::staticFunctions,
    TypedArrayObject::staticProperties,
    TypedArrayObject::protoFunctions,
    TypedArrayObject::protoAccessors,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

/* static */ const JSClass TypedArrayObject::sharedTypedArrayPrototypeClass = {
    "TypedArrayPrototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_TypedArray),
    JS_NULL_CLASS_OPS,
    &TypedArrayObjectSharedTypedArrayPrototypeClassSpec,
};

namespace {

// This default implementation is only valid for integer types less
// than 32-bits in size.
template <typename NativeType>
bool TypedArrayObjectTemplate<NativeType>::getElementPure(
    TypedArrayObject* tarray, size_t index, Value* vp) {
  static_assert(sizeof(NativeType) < 4,
                "this method must only handle NativeType values that are "
                "always exact int32_t values");

  *vp = Int32Value(getIndex(tarray, index));
  return true;
}

// We need to specialize for floats and other integer types.
template <>
bool TypedArrayObjectTemplate<int32_t>::getElementPure(TypedArrayObject* tarray,
                                                       size_t index,
                                                       Value* vp) {
  *vp = Int32Value(getIndex(tarray, index));
  return true;
}

template <>
bool TypedArrayObjectTemplate<uint32_t>::getElementPure(
    TypedArrayObject* tarray, size_t index, Value* vp) {
  uint32_t val = getIndex(tarray, index);
  *vp = NumberValue(val);
  return true;
}

template <>
bool TypedArrayObjectTemplate<float>::getElementPure(TypedArrayObject* tarray,
                                                     size_t index, Value* vp) {
  float val = getIndex(tarray, index);
  double dval = val;

  /*
   * Doubles in typed arrays could be typed-punned arrays of integers. This
   * could allow user code to break the engine-wide invariant that only
   * canonical nans are stored into jsvals, which means user code could
   * confuse the engine into interpreting a double-typed jsval as an
   * object-typed jsval.
   *
   * This could be removed for platforms/compilers known to convert a 32-bit
   * non-canonical nan to a 64-bit canonical nan.
   */
  *vp = JS::CanonicalizedDoubleValue(dval);
  return true;
}

template <>
bool TypedArrayObjectTemplate<double>::getElementPure(TypedArrayObject* tarray,
                                                      size_t index, Value* vp) {
  double val = getIndex(tarray, index);

  /*
   * Doubles in typed arrays could be typed-punned arrays of integers. This
   * could allow user code to break the engine-wide invariant that only
   * canonical nans are stored into jsvals, which means user code could
   * confuse the engine into interpreting a double-typed jsval as an
   * object-typed jsval.
   */
  *vp = JS::CanonicalizedDoubleValue(val);
  return true;
}

template <>
bool TypedArrayObjectTemplate<int64_t>::getElementPure(TypedArrayObject* tarray,
                                                       size_t index,
                                                       Value* vp) {
  return false;
}

template <>
bool TypedArrayObjectTemplate<uint64_t>::getElementPure(
    TypedArrayObject* tarray, size_t index, Value* vp) {
  return false;
}
} /* anonymous namespace */

namespace {

template <typename NativeType>
bool TypedArrayObjectTemplate<NativeType>::getElement(JSContext* cx,
                                                      TypedArrayObject* tarray,
                                                      size_t index,
                                                      MutableHandleValue val) {
  MOZ_ALWAYS_TRUE(getElementPure(tarray, index, val.address()));
  return true;
}

template <>
bool TypedArrayObjectTemplate<int64_t>::getElement(JSContext* cx,
                                                   TypedArrayObject* tarray,
                                                   size_t index,
                                                   MutableHandleValue val) {
  int64_t n = getIndex(tarray, index);
  BigInt* res = BigInt::createFromInt64(cx, n);
  if (!res) {
    return false;
  }
  val.setBigInt(res);
  return true;
}

template <>
bool TypedArrayObjectTemplate<uint64_t>::getElement(JSContext* cx,
                                                    TypedArrayObject* tarray,
                                                    size_t index,
                                                    MutableHandleValue val) {
  uint64_t n = getIndex(tarray, index);
  BigInt* res = BigInt::createFromUint64(cx, n);
  if (!res) {
    return false;
  }
  val.setBigInt(res);
  return true;
}
} /* anonymous namespace */

namespace js {

template <>
bool TypedArrayObject::getElement<CanGC>(JSContext* cx, size_t index,
                                         MutableHandleValue val) {
  switch (type()) {
#define GET_ELEMENT(_, T, N) \
  case Scalar::N:            \
    return N##Array::getElement(cx, this, index, val);
    JS_FOR_EACH_TYPED_ARRAY(GET_ELEMENT)
#undef GET_ELEMENT
    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      break;
  }

  MOZ_CRASH("Unknown TypedArray type");
}

template <>
bool TypedArrayObject::getElement<NoGC>(
    JSContext* cx, size_t index,
    typename MaybeRooted<Value, NoGC>::MutableHandleType vp) {
  return getElementPure(index, vp.address());
}

}  // namespace js

bool TypedArrayObject::getElementPure(size_t index, Value* vp) {
  switch (type()) {
#define GET_ELEMENT_PURE(_, T, N) \
  case Scalar::N:                 \
    return N##Array::getElementPure(this, index, vp);
    JS_FOR_EACH_TYPED_ARRAY(GET_ELEMENT_PURE)
#undef GET_ELEMENT
    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      break;
  }

  MOZ_CRASH("Unknown TypedArray type");
}

/* static */
bool TypedArrayObject::getElements(JSContext* cx,
                                   Handle<TypedArrayObject*> tarray,
                                   Value* vp) {
  size_t length = tarray->length();
  MOZ_ASSERT_IF(length > 0, !tarray->hasDetachedBuffer());

  switch (tarray->type()) {
#define GET_ELEMENTS(_, T, N)                                                  \
  case Scalar::N:                                                              \
    for (size_t i = 0; i < length; ++i, ++vp) {                                \
      if (!N##Array::getElement(cx, tarray, i,                                 \
                                MutableHandleValue::fromMarkedLocation(vp))) { \
        return false;                                                          \
      }                                                                        \
    }                                                                          \
    return true;
    JS_FOR_EACH_TYPED_ARRAY(GET_ELEMENTS)
#undef GET_ELEMENTS
    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      break;
  }

  MOZ_CRASH("Unknown TypedArray type");
}

/***
 *** JS impl
 ***/

/*
 * TypedArrayObject boilerplate
 */

static const JSClassOps TypedArrayClassOps = {
    nullptr,                       // addProperty
    nullptr,                       // delProperty
    nullptr,                       // enumerate
    nullptr,                       // newEnumerate
    nullptr,                       // resolve
    nullptr,                       // mayResolve
    TypedArrayObject::finalize,    // finalize
    nullptr,                       // call
    nullptr,                       // construct
    ArrayBufferViewObject::trace,  // trace
};

static const ClassExtension TypedArrayClassExtension = {
    TypedArrayObject::objectMoved,  // objectMovedOp
};

static const JSPropertySpec
    static_prototype_properties[Scalar::MaxTypedArrayViewType][2] = {
#define IMPL_TYPED_ARRAY_PROPERTIES(ExternalType, NativeType, Name) \
  {JS_INT32_PS("BYTES_PER_ELEMENT", Name##Array::BYTES_PER_ELEMENT, \
               JSPROP_READONLY | JSPROP_PERMANENT),                 \
   JS_PS_END},

        JS_FOR_EACH_TYPED_ARRAY(IMPL_TYPED_ARRAY_PROPERTIES)
#undef IMPL_TYPED_ARRAY_PROPERTIES
};

static const ClassSpec
    TypedArrayObjectClassSpecs[Scalar::MaxTypedArrayViewType] = {
#define IMPL_TYPED_ARRAY_CLASS_SPEC(ExternalType, NativeType, Name) \
  {Name##Array::createConstructor,                                  \
   Name##Array::createPrototype,                                    \
   nullptr,                                                         \
   static_prototype_properties[Scalar::Type::Name],                 \
   nullptr,                                                         \
   static_prototype_properties[Scalar::Type::Name],                 \
   nullptr,                                                         \
   JSProto_TypedArray},

        JS_FOR_EACH_TYPED_ARRAY(IMPL_TYPED_ARRAY_CLASS_SPEC)
#undef IMPL_TYPED_ARRAY_CLASS_SPEC
};

const JSClass TypedArrayObject::classes[Scalar::MaxTypedArrayViewType] = {
#define IMPL_TYPED_ARRAY_CLASS(ExternalType, NativeType, Name)           \
  {#Name "Array",                                                        \
   JSCLASS_HAS_RESERVED_SLOTS(TypedArrayObject::RESERVED_SLOTS) |        \
       JSCLASS_HAS_CACHED_PROTO(JSProto_##Name##Array) |                 \
       JSCLASS_DELAY_METADATA_BUILDER | JSCLASS_SKIP_NURSERY_FINALIZE |  \
       JSCLASS_BACKGROUND_FINALIZE,                                      \
   &TypedArrayClassOps, &TypedArrayObjectClassSpecs[Scalar::Type::Name], \
   &TypedArrayClassExtension},

    JS_FOR_EACH_TYPED_ARRAY(IMPL_TYPED_ARRAY_CLASS)
#undef IMPL_TYPED_ARRAY_CLASS
};

// The various typed array prototypes are supposed to 1) be normal objects,
// 2) stringify to "[object <name of constructor>]", and 3) (Gecko-specific)
// be xrayable.  The first and second requirements mandate (in the absence of
// @@toStringTag) a custom class.  The third requirement mandates that each
// prototype's class have the relevant typed array's cached JSProtoKey in them.
// Thus we need one class with cached prototype per kind of typed array, with a
// delegated ClassSpec.
//
// Actually ({}).toString.call(Uint8Array.prototype) should throw, because
// Uint8Array.prototype lacks the the typed array internal slots.  (Same as
// with %TypedArray%.prototype.)  It's not clear this is desirable (see
// above), but it's what we've always done, so keep doing it till we
// implement @@toStringTag or ES6 changes.
const JSClass TypedArrayObject::protoClasses[Scalar::MaxTypedArrayViewType] = {
#define IMPL_TYPED_ARRAY_PROTO_CLASS(ExternalType, NativeType, Name)         \
  {#Name "Array.prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_##Name##Array), \
   JS_NULL_CLASS_OPS, &TypedArrayObjectClassSpecs[Scalar::Type::Name]},

    JS_FOR_EACH_TYPED_ARRAY(IMPL_TYPED_ARRAY_PROTO_CLASS)
#undef IMPL_TYPED_ARRAY_PROTO_CLASS
};

/* static */
bool TypedArrayObject::isOriginalLengthGetter(Native native) {
  return native == TypedArray_lengthGetter;
}

/* static */
bool TypedArrayObject::isOriginalByteOffsetGetter(Native native) {
  return native == TypedArray_byteOffsetGetter;
}

/* static */
bool TypedArrayObject::isOriginalByteLengthGetter(Native native) {
  return native == TypedArray_byteLengthGetter;
}

bool js::IsTypedArrayConstructor(const JSObject* obj) {
#define CHECK_TYPED_ARRAY_CONSTRUCTOR(_, T, N)              \
  if (IsNativeFunction(obj, N##Array::class_constructor)) { \
    return true;                                            \
  }
  JS_FOR_EACH_TYPED_ARRAY(CHECK_TYPED_ARRAY_CONSTRUCTOR)
#undef CHECK_TYPED_ARRAY_CONSTRUCTOR
  return false;
}

bool js::IsTypedArrayConstructor(HandleValue v, Scalar::Type type) {
  return IsNativeFunction(v, TypedArrayConstructorNative(type));
}

JSNative js::TypedArrayConstructorNative(Scalar::Type type) {
#define TYPED_ARRAY_CONSTRUCTOR_NATIVE(_, T, N) \
  if (type == Scalar::N) {                      \
    return N##Array::class_constructor;         \
  }
  JS_FOR_EACH_TYPED_ARRAY(TYPED_ARRAY_CONSTRUCTOR_NATIVE)
#undef TYPED_ARRAY_CONSTRUCTOR_NATIVE

  MOZ_CRASH("unexpected typed array type");
}

bool js::IsBufferSource(JSObject* object, SharedMem<uint8_t*>* dataPointer,
                        size_t* byteLength) {
  if (object->is<TypedArrayObject>()) {
    TypedArrayObject& view = object->as<TypedArrayObject>();
    *dataPointer = view.dataPointerEither().cast<uint8_t*>();
    *byteLength = view.byteLength();
    return true;
  }

  if (object->is<DataViewObject>()) {
    DataViewObject& view = object->as<DataViewObject>();
    *dataPointer = view.dataPointerEither().cast<uint8_t*>();
    *byteLength = view.byteLength();
    return true;
  }

  if (object->is<ArrayBufferObject>()) {
    ArrayBufferObject& buffer = object->as<ArrayBufferObject>();
    *dataPointer = buffer.dataPointerShared();
    *byteLength = buffer.byteLength();
    return true;
  }

  if (object->is<SharedArrayBufferObject>()) {
    SharedArrayBufferObject& buffer = object->as<SharedArrayBufferObject>();
    *dataPointer = buffer.dataPointerShared();
    *byteLength = buffer.byteLength();
    return true;
  }

  return false;
}

template <typename CharT>
static inline bool StringIsInfinity(mozilla::Range<const CharT> s) {
  static constexpr std::string_view Infinity = "Infinity";

  // Compilers optimize this to one |cmp| instruction on x64 resp. two for x86,
  // when the input is a Latin-1 string, because the string "Infinity" is
  // exactly eight characters long, so it can be represented as a single uint64
  // value.
  return s.length() == Infinity.length() &&
         EqualChars(s.begin().get(), Infinity.data(), Infinity.length());
}

template <typename CharT>
static inline bool StringIsNaN(mozilla::Range<const CharT> s) {
  static constexpr std::string_view NaN = "NaN";

  // "NaN" is not as nicely optimizable as "Infinity", but oh well.
  return s.length() == NaN.length() &&
         EqualChars(s.begin().get(), NaN.data(), NaN.length());
}

template <typename CharT>
static mozilla::Maybe<uint64_t> StringToTypedArrayIndexSlow(
    mozilla::Range<const CharT> s) {
  const mozilla::RangedPtr<const CharT> start = s.begin();
  const mozilla::RangedPtr<const CharT> end = s.end();

  const CharT* actualEnd;
  double result = js_strtod(start.get(), end.get(), &actualEnd);

  // The complete string must have been parsed.
  if (actualEnd != end.get()) {
    return mozilla::Nothing();
  }

  // Now convert it back to a string.
  ToCStringBuf cbuf;
  size_t cstrlen;
  const char* cstr = js::NumberToCString(&cbuf, result, &cstrlen);
  MOZ_ASSERT(cstr);

  // Both strings must be equal for a canonical numeric index string.
  if (s.length() != cstrlen || !EqualChars(start.get(), cstr, cstrlen)) {
    return mozilla::Nothing();
  }

  // Directly perform IsInteger() check and encode negative and non-integer
  // indices as OOB.
  // See 9.4.5.2 [[HasProperty]], steps 3.b.iii and 3.b.v.
  // See 9.4.5.3 [[DefineOwnProperty]], steps 3.b.i and 3.b.iii.
  // See 9.4.5.8 IntegerIndexedElementGet, steps 5 and 8.
  // See 9.4.5.9 IntegerIndexedElementSet, steps 6 and 9.
  if (result < 0 || !IsInteger(result)) {
    return mozilla::Some(UINT64_MAX);
  }

  // Anything equals-or-larger than 2^53 is definitely OOB, encode it
  // accordingly so that the cast to uint64_t is well defined.
  if (result >= DOUBLE_INTEGRAL_PRECISION_LIMIT) {
    return mozilla::Some(UINT64_MAX);
  }

  // The string is an actual canonical numeric index.
  return mozilla::Some(result);
}

template <typename CharT>
mozilla::Maybe<uint64_t> js::StringToTypedArrayIndex(
    mozilla::Range<const CharT> s) {
  mozilla::RangedPtr<const CharT> cp = s.begin();
  const mozilla::RangedPtr<const CharT> end = s.end();

  MOZ_ASSERT(cp < end, "caller must check for empty strings");

  bool negative = false;
  if (*cp == '-') {
    negative = true;
    if (++cp == end) {
      return mozilla::Nothing();
    }
  }

  if (!IsAsciiDigit(*cp)) {
    // Check for "NaN", "Infinity", or "-Infinity".
    if ((!negative && StringIsNaN<CharT>({cp, end})) ||
        StringIsInfinity<CharT>({cp, end})) {
      return mozilla::Some(UINT64_MAX);
    }
    return mozilla::Nothing();
  }

  uint32_t digit = AsciiDigitToNumber(*cp++);

  // Don't allow leading zeros.
  if (digit == 0 && cp != end) {
    // The string may be of the form "0.xyz". The exponent form isn't possible
    // when the string starts with "0".
    if (*cp == '.') {
      return StringToTypedArrayIndexSlow(s);
    }
    return mozilla::Nothing();
  }

  uint64_t index = digit;

  for (; cp < end; cp++) {
    if (!IsAsciiDigit(*cp)) {
      // Take the slow path when the string has fractional parts or an exponent.
      if (*cp == '.' || *cp == 'e') {
        return StringToTypedArrayIndexSlow(s);
      }
      return mozilla::Nothing();
    }

    digit = AsciiDigitToNumber(*cp);

    static_assert(
        uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT) < (UINT64_MAX - 10) / 10,
        "2^53 is way below UINT64_MAX, so |10 * index + digit| can't overflow");

    index = 10 * index + digit;

    // Also take the slow path when the string is larger-or-equals 2^53.
    if (index >= uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT)) {
      return StringToTypedArrayIndexSlow(s);
    }
  }

  if (negative) {
    return mozilla::Some(UINT64_MAX);
  }
  return mozilla::Some(index);
}

template mozilla::Maybe<uint64_t> js::StringToTypedArrayIndex(
    mozilla::Range<const char16_t> s);

template mozilla::Maybe<uint64_t> js::StringToTypedArrayIndex(
    mozilla::Range<const Latin1Char> s);

bool js::SetTypedArrayElement(JSContext* cx, Handle<TypedArrayObject*> obj,
                              uint64_t index, HandleValue v,
                              ObjectOpResult& result) {
  TypedArrayObject* tobj = &obj->as<TypedArrayObject>();

  switch (tobj->type()) {
#define SET_TYPED_ARRAY_ELEMENT(_, T, N) \
  case Scalar::N:                        \
    return TypedArrayObjectTemplate<T>::setElement(cx, obj, index, v, result);
    JS_FOR_EACH_TYPED_ARRAY(SET_TYPED_ARRAY_ELEMENT)
#undef SET_TYPED_ARRAY_ELEMENT
    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      break;
  }

  MOZ_CRASH("Unsupported TypedArray type");
}

// ES2021 draft rev b3f9b5089bcc3ddd8486379015cd11eb1427a5eb
// 9.4.5.3 [[DefineOwnProperty]], step 3.b.
bool js::DefineTypedArrayElement(JSContext* cx, Handle<TypedArrayObject*> obj,
                                 uint64_t index,
                                 Handle<PropertyDescriptor> desc,
                                 ObjectOpResult& result) {
  // These are all substeps of 3.b.

  // Step i.
  if (index >= obj->length()) {
    if (obj->hasDetachedBuffer()) {
      return result.fail(JSMSG_TYPED_ARRAY_DETACHED);
    }
    return result.fail(JSMSG_DEFINE_BAD_INDEX);
  }

  // Step ii.
  if (desc.isAccessorDescriptor()) {
    return result.fail(JSMSG_CANT_REDEFINE_PROP);
  }

  // Step iii.
  if (desc.hasConfigurable() && !desc.configurable()) {
    return result.fail(JSMSG_CANT_REDEFINE_PROP);
  }

  // Step iv.
  if (desc.hasEnumerable() && !desc.enumerable()) {
    return result.fail(JSMSG_CANT_REDEFINE_PROP);
  }

  // Step v.
  if (desc.hasWritable() && !desc.writable()) {
    return result.fail(JSMSG_CANT_REDEFINE_PROP);
  }

  // Step vi.
  if (desc.hasValue()) {
    switch (obj->type()) {
#define DEFINE_TYPED_ARRAY_ELEMENT(_, T, N)                        \
  case Scalar::N:                                                  \
    return TypedArrayObjectTemplate<T>::setElement(cx, obj, index, \
                                                   desc.value(), result);
      JS_FOR_EACH_TYPED_ARRAY(DEFINE_TYPED_ARRAY_ELEMENT)
#undef DEFINE_TYPED_ARRAY_ELEMENT
      case Scalar::MaxTypedArrayViewType:
      case Scalar::Int64:
      case Scalar::Simd128:
        break;
    }

    MOZ_CRASH("Unsupported TypedArray type");
  }

  // Step vii.
  return result.succeed();
}

template <typename T, typename U>
static constexpr typename std::enable_if_t<std::is_unsigned_v<T>, U>
UnsignedSortValue(U val) {
  return val;
}

template <typename T, typename U>
static constexpr
    typename std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T>, U>
    UnsignedSortValue(U val) {
  // Flip sign bit.
  return val ^ static_cast<U>(std::numeric_limits<T>::min());
}

template <typename T, typename UnsignedT>
static constexpr
    typename std::enable_if_t<std::is_floating_point_v<T>, UnsignedT>
    UnsignedSortValue(UnsignedT val) {
  // Flip sign bit for positive numbers; flip all bits for negative numbers,
  // except negative NaNs.
  using FloatingPoint = mozilla::FloatingPoint<T>;
  static_assert(std::is_same_v<typename FloatingPoint::Bits, UnsignedT>,
                "FloatingPoint::Bits matches the unsigned int representation");

  // FF80'0000 is negative infinity, (FF80'0000, FFFF'FFFF] are all NaNs with
  // the sign-bit set (and the equivalent holds for double values). So any value
  // larger than negative infinity is a negative NaN.
  constexpr UnsignedT NegativeInfinity =
      FloatingPoint::kSignBit | FloatingPoint::kExponentBits;
  if (val > NegativeInfinity) {
    return val;
  }
  if (val & FloatingPoint::kSignBit) {
    return ~val;
  }
  return val ^ FloatingPoint::kSignBit;
}

template <typename T>
static typename std::enable_if_t<std::is_integral_v<T> ||
                                 std::is_same_v<T, uint8_clamped>>
TypedArrayStdSort(SharedMem<void*> data, size_t length) {
  T* unwrapped = data.cast<T*>().unwrapUnshared();
  std::sort(unwrapped, unwrapped + length);
}

template <typename T>
static typename std::enable_if_t<std::is_floating_point_v<T>> TypedArrayStdSort(
    SharedMem<void*> data, size_t length) {
  // Sort on the unsigned representation for performance reasons.
  using UnsignedT =
      typename mozilla::UnsignedStdintTypeForSize<sizeof(T)>::Type;
  UnsignedT* unwrapped = data.cast<UnsignedT*>().unwrapUnshared();
  std::sort(unwrapped, unwrapped + length, [](UnsignedT x, UnsignedT y) {
    constexpr auto SortValue = UnsignedSortValue<T, UnsignedT>;
    return SortValue(x) < SortValue(y);
  });
}

template <typename T, typename Ops>
static typename std::enable_if_t<std::is_same_v<Ops, UnsharedOps>, bool>
TypedArrayStdSort(JSContext* cx, TypedArrayObject* typedArray) {
  TypedArrayStdSort<T>(typedArray->dataPointerEither(), typedArray->length());
  return true;
}

template <typename T, typename Ops>
static typename std::enable_if_t<std::is_same_v<Ops, SharedOps>, bool>
TypedArrayStdSort(JSContext* cx, TypedArrayObject* typedArray) {
  // Always create a copy when sorting shared memory backed typed arrays to
  // ensure concurrent write accesses doesn't lead to UB when calling std::sort.
  size_t length = typedArray->length();
  auto ptr = cx->make_pod_array<T>(length);
  if (!ptr) {
    return false;
  }
  SharedMem<T*> unshared = SharedMem<T*>::unshared(ptr.get());
  SharedMem<T*> data = typedArray->dataPointerShared().cast<T*>();

  Ops::podCopy(unshared, data, length);

  TypedArrayStdSort<T>(unshared.template cast<void*>(), length);

  Ops::podCopy(data, unshared, length);

  return true;
}

template <typename T, typename Ops>
static bool TypedArrayCountingSort(JSContext* cx,
                                   TypedArrayObject* typedArray) {
  static_assert(std::is_integral_v<T> || std::is_same_v<T, uint8_clamped>,
                "Counting sort expects integral array elements");

  size_t length = typedArray->length();

  // Determined by performance testing.
  if (length <= 64) {
    return TypedArrayStdSort<T, Ops>(cx, typedArray);
  }

  // Map signed values onto the unsigned range when storing in buffer.
  using UnsignedT =
      typename mozilla::UnsignedStdintTypeForSize<sizeof(T)>::Type;
  constexpr T min = std::numeric_limits<T>::min();

  constexpr size_t InlineStorage = sizeof(T) == 1 ? 256 : 0;
  Vector<size_t, InlineStorage> buffer(cx);
  if (!buffer.resize(size_t(std::numeric_limits<UnsignedT>::max()) + 1)) {
    return false;
  }

  SharedMem<T*> data = typedArray->dataPointerEither().cast<T*>();

  // Populate the buffer.
  for (size_t i = 0; i < length; i++) {
    T val = Ops::load(data + i);
    buffer[UnsignedT(val - min)]++;
  }

  // Traverse the buffer in order and write back elements to array.
  UnsignedT val = UnsignedT(-1);  // intentional overflow on first increment
  for (size_t i = 0; i < length;) {
    // Invariant: sum(buffer[val:]) == length-i
    size_t j;
    do {
      j = buffer[++val];
    } while (j == 0);

    for (; j > 0; j--) {
      Ops::store(data + i++, T(val + min));
    }
  }

  return true;
}

template <typename T, typename U, typename Ops>
static void SortByColumn(SharedMem<U*> data, size_t length, SharedMem<U*> aux,
                         uint8_t col) {
  static_assert(std::is_unsigned_v<U>, "SortByColumn sorts on unsigned values");
  static_assert(std::is_same_v<Ops, UnsharedOps>,
                "SortByColumn only works on unshared data");

  // |counts| is used to compute the starting index position for each key.
  // Letting counts[0] always be 0, simplifies the transform step below.
  // Example:
  //
  // Computing frequency counts for the input [1 2 1] gives:
  //      0 1 2 3 ... (keys)
  //      0 0 2 1     (frequencies)
  //
  // Transforming frequencies to indexes gives:
  //      0 1 2 3 ... (keys)
  //      0 0 2 3     (indexes)

  constexpr size_t R = 256;

  // Initialize all entries to zero.
  size_t counts[R + 1] = {};

  const auto ByteAtCol = [col](U x) {
    U y = UnsignedSortValue<T, U>(x);
    return static_cast<uint8_t>(y >> (col * 8));
  };

  // Compute frequency counts.
  for (size_t i = 0; i < length; i++) {
    U val = Ops::load(data + i);
    uint8_t b = ByteAtCol(val);
    counts[b + 1]++;
  }

  // Transform counts to indices.
  std::partial_sum(std::begin(counts), std::end(counts), std::begin(counts));

  // Distribute
  for (size_t i = 0; i < length; i++) {
    U val = Ops::load(data + i);
    uint8_t b = ByteAtCol(val);
    size_t j = counts[b]++;
    MOZ_ASSERT(j < length,
               "index is in bounds when |data| can't be modified concurrently");
    UnsharedOps::store(aux + j, val);
  }

  // Copy back
  Ops::podCopy(data, aux, length);
}

template <typename T, typename Ops>
static bool TypedArrayRadixSort(JSContext* cx, TypedArrayObject* typedArray) {
  size_t length = typedArray->length();

  // Determined by performance testing.
  constexpr size_t StdSortMinCutoff = sizeof(T) == 2 ? 64 : 256;

  // Radix sort uses O(n) additional space, limit this space to 64 MB.
  constexpr size_t StdSortMaxCutoff = (64 * 1024 * 1024) / sizeof(T);

  if (length <= StdSortMinCutoff || length >= StdSortMaxCutoff) {
    return TypedArrayStdSort<T, Ops>(cx, typedArray);
  }

  if constexpr (sizeof(T) == 2) {
    // Radix sort uses O(n) additional space, so when |n| reaches 2^16, switch
    // over to counting sort to limit the additional space needed to 2^16.
    constexpr size_t CountingSortMaxCutoff = 65536;

    if (length >= CountingSortMaxCutoff) {
      return TypedArrayCountingSort<T, Ops>(cx, typedArray);
    }
  }

  using UnsignedT =
      typename mozilla::UnsignedStdintTypeForSize<sizeof(T)>::Type;

  auto ptr = cx->make_zeroed_pod_array<UnsignedT>(length);
  if (!ptr) {
    return false;
  }
  SharedMem<UnsignedT*> aux = SharedMem<UnsignedT*>::unshared(ptr.get());

  SharedMem<UnsignedT*> data =
      typedArray->dataPointerEither().cast<UnsignedT*>();

  // Always create a copy when sorting shared memory backed typed arrays to
  // ensure concurrent write accesses don't lead to computing bad indices.
  SharedMem<UnsignedT*> unshared;
  SharedMem<UnsignedT*> shared;
  UniquePtr<UnsignedT[], JS::FreePolicy> ptrUnshared;
  if constexpr (std::is_same_v<Ops, SharedOps>) {
    ptrUnshared = cx->make_pod_array<UnsignedT>(length);
    if (!ptrUnshared) {
      return false;
    }
    unshared = SharedMem<UnsignedT*>::unshared(ptrUnshared.get());
    shared = data;

    Ops::podCopy(unshared, shared, length);

    data = unshared;
  }

  for (uint8_t col = 0; col < sizeof(UnsignedT); col++) {
    SortByColumn<T, UnsignedT, UnsharedOps>(data, length, aux, col);
  }

  if constexpr (std::is_same_v<Ops, SharedOps>) {
    Ops::podCopy(shared, unshared, length);
  }

  return true;
}

using TypedArraySortFn = bool (*)(JSContext*, TypedArrayObject*);

template <typename T, typename Ops>
static constexpr typename std::enable_if_t<sizeof(T) == 1, TypedArraySortFn>
TypedArraySort() {
  return TypedArrayCountingSort<T, Ops>;
}

template <typename T, typename Ops>
static constexpr typename std::enable_if_t<sizeof(T) == 2 || sizeof(T) == 4,
                                           TypedArraySortFn>
TypedArraySort() {
  return TypedArrayRadixSort<T, Ops>;
}

template <typename T, typename Ops>
static constexpr typename std::enable_if_t<sizeof(T) == 8, TypedArraySortFn>
TypedArraySort() {
  return TypedArrayStdSort<T, Ops>;
}

bool js::intrinsic_TypedArrayNativeSort(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  TypedArrayObject* typedArray =
      UnwrapAndDowncastValue<TypedArrayObject>(cx, args[0]);
  if (!typedArray) {
    return false;
  }

  MOZ_RELEASE_ASSERT(!typedArray->hasDetachedBuffer());

  bool isShared = typedArray->isSharedMemory();
  switch (typedArray->type()) {
#define SORT(_, T, N)                                          \
  case Scalar::N:                                              \
    if (isShared) {                                            \
      if (!TypedArraySort<T, SharedOps>()(cx, typedArray)) {   \
        return false;                                          \
      }                                                        \
    } else {                                                   \
      if (!TypedArraySort<T, UnsharedOps>()(cx, typedArray)) { \
        return false;                                          \
      }                                                        \
    }                                                          \
    break;
    JS_FOR_EACH_TYPED_ARRAY(SORT)
#undef SORT
    default:
      MOZ_CRASH("Unsupported TypedArray type");
  }

  args.rval().set(args[0]);
  return true;
}

/* JS Public API */

#define IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(ExternalType, NativeType, Name)   \
  JS_PUBLIC_API JSObject* JS_New##Name##Array(JSContext* cx,                  \
                                              size_t nelements) {             \
    return TypedArrayObjectTemplate<NativeType>::fromLength(cx, nelements);   \
  }                                                                           \
                                                                              \
  JS_PUBLIC_API JSObject* JS_New##Name##ArrayFromArray(JSContext* cx,         \
                                                       HandleObject other) {  \
    return TypedArrayObjectTemplate<NativeType>::fromArray(cx, other);        \
  }                                                                           \
                                                                              \
  JS_PUBLIC_API JSObject* JS_New##Name##ArrayWithBuffer(                      \
      JSContext* cx, HandleObject arrayBuffer, size_t byteOffset,             \
      int64_t length) {                                                       \
    return TypedArrayObjectTemplate<NativeType>::fromBuffer(                  \
        cx, arrayBuffer, byteOffset, length);                                 \
  }                                                                           \
                                                                              \
  JS_PUBLIC_API JSObject* js::Unwrap##Name##Array(JSObject* obj) {            \
    obj = obj->maybeUnwrapIf<TypedArrayObject>();                             \
    if (!obj) {                                                               \
      return nullptr;                                                         \
    }                                                                         \
    const JSClass* clasp = obj->getClass();                                   \
    if (clasp != TypedArrayObjectTemplate<NativeType>::instanceClass()) {     \
      return nullptr;                                                         \
    }                                                                         \
    return obj;                                                               \
  }                                                                           \
                                                                              \
  JS_PUBLIC_API ExternalType* JS_Get##Name##ArrayLengthAndData(               \
      JSObject* obj, size_t* length, bool* isSharedMemory,                    \
      const JS::AutoRequireNoGC& nogc) {                                      \
    TypedArrayObject* tarr = obj->maybeUnwrapAs<TypedArrayObject>();          \
    if (!tarr) {                                                              \
      return nullptr;                                                         \
    }                                                                         \
    return JS::TypedArray<JS::Scalar::Name>::fromObject(tarr)                 \
        .getLengthAndData(length, isSharedMemory, nogc);                      \
  }                                                                           \
                                                                              \
  JS_PUBLIC_API ExternalType* JS_Get##Name##ArrayData(                        \
      JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC& nogc) { \
    size_t length;                                                            \
    return JS_Get##Name##ArrayLengthAndData(obj, &length, isSharedMemory,     \
                                            nogc);                            \
  }                                                                           \
  JS_PUBLIC_API JSObject* JS_GetObjectAs##Name##Array(                        \
      JSObject* obj, size_t* length, bool* isShared, ExternalType** data) {   \
    obj = js::Unwrap##Name##Array(obj);                                       \
    if (!obj) {                                                               \
      return nullptr;                                                         \
    }                                                                         \
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();                    \
    *length = tarr->length();                                                 \
    *isShared = tarr->isSharedMemory();                                       \
    *data = static_cast<ExternalType*>(tarr->dataPointerEither().unwrap(      \
        /*safe - caller sees isShared flag*/));                               \
    return obj;                                                               \
  }

JS_FOR_EACH_TYPED_ARRAY(IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS)
#undef IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS

JS_PUBLIC_API bool JS_IsTypedArrayObject(JSObject* obj) {
  return obj->canUnwrapAs<TypedArrayObject>();
}

JS_PUBLIC_API size_t JS_GetTypedArrayLength(JSObject* obj) {
  TypedArrayObject* tarr = obj->maybeUnwrapAs<TypedArrayObject>();
  if (!tarr) {
    return 0;
  }
  return tarr->length();
}

JS_PUBLIC_API size_t JS_GetTypedArrayByteOffset(JSObject* obj) {
  TypedArrayObject* tarr = obj->maybeUnwrapAs<TypedArrayObject>();
  if (!tarr) {
    return 0;
  }
  return tarr->byteOffset();
}

JS_PUBLIC_API size_t JS_GetTypedArrayByteLength(JSObject* obj) {
  TypedArrayObject* tarr = obj->maybeUnwrapAs<TypedArrayObject>();
  if (!tarr) {
    return 0;
  }
  return tarr->byteLength();
}

JS_PUBLIC_API bool JS_GetTypedArraySharedness(JSObject* obj) {
  TypedArrayObject* tarr = obj->maybeUnwrapAs<TypedArrayObject>();
  if (!tarr) {
    return false;
  }
  return tarr->isSharedMemory();
}

JS_PUBLIC_API JS::Scalar::Type JS_GetArrayBufferViewType(JSObject* obj) {
  ArrayBufferViewObject* view = obj->maybeUnwrapAs<ArrayBufferViewObject>();
  if (!view) {
    return Scalar::MaxTypedArrayViewType;
  }

  if (view->is<TypedArrayObject>()) {
    return view->as<TypedArrayObject>().type();
  }
  if (view->is<DataViewObject>()) {
    return Scalar::MaxTypedArrayViewType;
  }
  MOZ_CRASH("invalid ArrayBufferView type");
}

JS_PUBLIC_API size_t JS_MaxMovableTypedArraySize() {
  return TypedArrayObject::INLINE_BUFFER_LIMIT;
}

namespace JS {

const JSClass* const TypedArray_base::classes = TypedArrayObject::classes;

#define INSTANTIATE(ExternalType, NativeType, Name) \
  template class TypedArray<JS::Scalar::Name>;
JS_FOR_EACH_TYPED_ARRAY(INSTANTIATE)
#undef INSTANTIATE

JS::ArrayBufferOrView JS::ArrayBufferOrView::unwrap(JSObject* maybeWrapped) {
  if (!maybeWrapped) {
    return JS::ArrayBufferOrView(nullptr);
  }
  auto* ab = maybeWrapped->maybeUnwrapIf<ArrayBufferObjectMaybeShared>();
  if (ab) {
    return ArrayBufferOrView::fromObject(ab);
  }

  return ArrayBufferView::unwrap(maybeWrapped);
}

bool JS::ArrayBufferOrView::isDetached() const {
  MOZ_ASSERT(obj);
  if (obj->is<ArrayBufferObject>()) {
    return obj->as<ArrayBufferObject>().isDetached();
  } else {
    return obj->as<ArrayBufferViewObject>().hasDetachedBuffer();
  }
}

JS::TypedArray_base JS::TypedArray_base::fromObject(JSObject* unwrapped) {
  if (unwrapped && unwrapped->is<TypedArrayObject>()) {
    return TypedArray_base(unwrapped);
  }
  return TypedArray_base(nullptr);
}

// Template getLengthAndData function for TypedArrays, implemented here because
// it requires internal APIs.
template <JS::Scalar::Type EType>
typename TypedArray<EType>::DataType* TypedArray<EType>::getLengthAndData(
    size_t* length, bool* isSharedMemory, const AutoRequireNoGC&) {
  using ExternalType = TypedArray<EType>::DataType;
  if (!obj) {
    return nullptr;
  }
  TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
  MOZ_ASSERT(tarr);
  *length = tarr->length();
  *isSharedMemory = tarr->isSharedMemory();
  return static_cast<ExternalType*>(
      tarr->dataPointerEither().unwrap(/*safe - caller sees isShared*/));
};

// Force the method defined above to actually be instantianted in this
// compilation unit and emitted into the object file, since otherwise a binary
// could include the header file and emit an undefined symbol that would not be
// satisfied by the linker. (This happens with opt gtest, at least. In a DEBUG
// build, the header contains a call to this function so it will always be
// emitted.)
#define INSTANTIATE_GET_DATA(a, b, Name)                    \
  template typename TypedArray<JS::Scalar::Name>::DataType* \
  TypedArray<JS::Scalar::Name>::getLengthAndData(           \
      size_t* length, bool* isSharedMemory, const AutoRequireNoGC&);
JS_FOR_EACH_TYPED_ARRAY(INSTANTIATE_GET_DATA)
#undef INSTANTIATE_GET_DATA

} /* namespace JS */
