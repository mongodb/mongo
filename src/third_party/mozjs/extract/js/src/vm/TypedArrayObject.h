/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_TypedArrayObject_h
#define vm_TypedArrayObject_h

#include "mozilla/Maybe.h"
#include "mozilla/TextUtils.h"

#include "gc/AllocKind.h"
#include "gc/MaybeRooted.h"
#include "js/Class.h"
#include "js/experimental/TypedData.h"  // js::detail::TypedArrayLengthSlot
#include "js/ScalarType.h"              // js::Scalar::Type
#include "vm/ArrayBufferObject.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/JSObject.h"
#include "vm/SharedArrayObject.h"

namespace js {

enum class ArraySortResult : uint32_t;

namespace jit {
class TrampolineNativeFrameLayout;
}

/*
 * TypedArrayObject
 *
 * The non-templated base class for the specific typed implementations.
 * This class holds all the member variables that are used by
 * the subclasses.
 */

class TypedArrayObject : public ArrayBufferViewObject {
 public:
  static_assert(js::detail::TypedArrayLengthSlot == LENGTH_SLOT,
                "bad inlined constant in TypedData.h");
  static_assert(js::detail::TypedArrayDataSlot == DATA_SLOT,
                "bad inlined constant in TypedData.h");

  static bool sameBuffer(Handle<TypedArrayObject*> a,
                         Handle<TypedArrayObject*> b) {
    // Inline buffers.
    if (!a->hasBuffer() || !b->hasBuffer()) {
      return a.get() == b.get();
    }

    // Shared buffers.
    if (a->isSharedMemory() && b->isSharedMemory()) {
      return a->bufferShared()->globalID() == b->bufferShared()->globalID();
    }

    return a->bufferEither() == b->bufferEither();
  }

  static const JSClass anyClasses[2][Scalar::MaxTypedArrayViewType];
  static const JSClass (&fixedLengthClasses)[Scalar::MaxTypedArrayViewType];
  static const JSClass (&resizableClasses)[Scalar::MaxTypedArrayViewType];
  static const JSClass protoClasses[Scalar::MaxTypedArrayViewType];
  static const JSClass sharedTypedArrayPrototypeClass;

  static const JSClass* protoClassForType(Scalar::Type type) {
    MOZ_ASSERT(type < Scalar::MaxTypedArrayViewType);
    return &protoClasses[type];
  }

  inline Scalar::Type type() const;
  inline size_t bytesPerElement() const;

  static bool ensureHasBuffer(JSContext* cx,
                              Handle<TypedArrayObject*> typedArray);

 public:
  /**
   * Return the current length, or |Nothing| if the TypedArray is detached or
   * out-of-bounds.
   */
  mozilla::Maybe<size_t> length() const {
    return ArrayBufferViewObject::length();
  }

  /**
   * Return the current byteLength, or |Nothing| if the TypedArray is detached
   * or out-of-bounds.
   */
  mozilla::Maybe<size_t> byteLength() const {
    return length().map(
        [this](size_t value) { return value * bytesPerElement(); });
  }

  // Self-hosted TypedArraySubarray function needs to read [[ByteOffset]], even
  // when it's currently out-of-bounds.
  size_t byteOffsetMaybeOutOfBounds() const {
    // dataPointerOffset() returns the [[ByteOffset]] spec value, except when
    // the buffer is detached. (bug 1840991)
    return ArrayBufferViewObject::dataPointerOffset();
  }

  template <AllowGC allowGC>
  bool getElement(JSContext* cx, size_t index,
                  typename MaybeRooted<Value, allowGC>::MutableHandleType val);
  bool getElementPure(size_t index, Value* vp);

  /*
   * Copy |length| elements from this typed array to vp. vp must point to rooted
   * memory. |length| must not exceed the typed array's current length.
   */
  static bool getElements(JSContext* cx, Handle<TypedArrayObject*> tarray,
                          size_t length, Value* vp);

  static bool GetTemplateObjectForNative(JSContext* cx, Native native,
                                         const JS::HandleValueArray args,
                                         MutableHandleObject res);

  // Maximum allowed byte length for any typed array.
  static constexpr size_t ByteLengthLimit = ArrayBufferObject::ByteLengthLimit;

  static bool isOriginalLengthGetter(Native native);

  static bool isOriginalByteOffsetGetter(Native native);

  static bool isOriginalByteLengthGetter(Native native);

  /* Initialization bits */

  static const JSFunctionSpec protoFunctions[];
  static const JSPropertySpec protoAccessors[];
  static const JSFunctionSpec staticFunctions[];
  static const JSPropertySpec staticProperties[];

  /* Accessors and functions */

  static bool set(JSContext* cx, unsigned argc, Value* vp);
  static bool copyWithin(JSContext* cx, unsigned argc, Value* vp);
  static bool sort(JSContext* cx, unsigned argc, Value* vp);

  bool convertValue(JSContext* cx, HandleValue v,
                    MutableHandleValue result) const;

 private:
  static bool set_impl(JSContext* cx, const CallArgs& args);
  static bool copyWithin_impl(JSContext* cx, const CallArgs& args);
};

class FixedLengthTypedArrayObject : public TypedArrayObject {
 public:
  static constexpr size_t FIXED_DATA_START = RESERVED_SLOTS;

  // For typed arrays which can store their data inline, the array buffer
  // object is created lazily.
  static constexpr uint32_t INLINE_BUFFER_LIMIT =
      (NativeObject::MAX_FIXED_SLOTS - FIXED_DATA_START) * sizeof(Value);

  inline gc::AllocKind allocKindForTenure() const;
  static inline gc::AllocKind AllocKindForLazyBuffer(size_t nbytes);

  size_t byteOffset() const {
    return ArrayBufferViewObject::byteOffsetSlotValue();
  }

  size_t byteLength() const { return length() * bytesPerElement(); }

  size_t length() const { return ArrayBufferViewObject::lengthSlotValue(); }

  bool hasInlineElements() const;
  void setInlineElements();
  uint8_t* elementsRaw() const {
    return maybePtrFromReservedSlot<uint8_t>(DATA_SLOT);
  }
  uint8_t* elements() const {
    assertZeroLengthArrayData();
    return elementsRaw();
  }

#ifdef DEBUG
  void assertZeroLengthArrayData() const;
#else
  void assertZeroLengthArrayData() const {};
#endif

  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static size_t objectMoved(JSObject* obj, JSObject* old);
};

class ResizableTypedArrayObject : public TypedArrayObject {
 public:
  static const uint8_t RESERVED_SLOTS = RESIZABLE_RESERVED_SLOTS;
};

extern TypedArrayObject* NewTypedArrayWithTemplateAndLength(
    JSContext* cx, HandleObject templateObj, int32_t len);

extern TypedArrayObject* NewTypedArrayWithTemplateAndArray(
    JSContext* cx, HandleObject templateObj, HandleObject array);

extern TypedArrayObject* NewTypedArrayWithTemplateAndBuffer(
    JSContext* cx, HandleObject templateObj, HandleObject arrayBuffer,
    HandleValue byteOffset, HandleValue length);

extern TypedArrayObject* NewUint8ArrayWithLength(
    JSContext* cx, int32_t len, gc::Heap heap = gc::Heap::Default);

inline bool IsFixedLengthTypedArrayClass(const JSClass* clasp) {
  return std::begin(TypedArrayObject::fixedLengthClasses) <= clasp &&
         clasp < std::end(TypedArrayObject::fixedLengthClasses);
}

inline bool IsResizableTypedArrayClass(const JSClass* clasp) {
  return std::begin(TypedArrayObject::resizableClasses) <= clasp &&
         clasp < std::end(TypedArrayObject::resizableClasses);
}

inline bool IsTypedArrayClass(const JSClass* clasp) {
  MOZ_ASSERT(std::end(TypedArrayObject::fixedLengthClasses) ==
                 std::begin(TypedArrayObject::resizableClasses),
             "TypedArray classes are in contiguous memory");
  return std::begin(TypedArrayObject::fixedLengthClasses) <= clasp &&
         clasp < std::end(TypedArrayObject::resizableClasses);
}

inline Scalar::Type GetTypedArrayClassType(const JSClass* clasp) {
  MOZ_ASSERT(IsTypedArrayClass(clasp));
  if (clasp < std::end(TypedArrayObject::fixedLengthClasses)) {
    return static_cast<Scalar::Type>(clasp -
                                     &TypedArrayObject::fixedLengthClasses[0]);
  }
  return static_cast<Scalar::Type>(clasp -
                                   &TypedArrayObject::resizableClasses[0]);
}

bool IsTypedArrayConstructor(const JSObject* obj);

bool IsTypedArrayConstructor(HandleValue v, Scalar::Type type);

JSNative TypedArrayConstructorNative(Scalar::Type type);

// In WebIDL terminology, a BufferSource is either an ArrayBuffer or a typed
// array view. In either case, extract the dataPointer/byteLength.
bool IsBufferSource(JSObject* object, SharedMem<uint8_t*>* dataPointer,
                    size_t* byteLength);

inline Scalar::Type TypedArrayObject::type() const {
  return GetTypedArrayClassType(getClass());
}

inline size_t TypedArrayObject::bytesPerElement() const {
  return Scalar::byteSize(type());
}

// ES2020 draft rev a5375bdad264c8aa264d9c44f57408087761069e
// 7.1.16 CanonicalNumericIndexString
//
// Checks whether or not the string is a canonical numeric index string. If the
// string is a canonical numeric index which is not representable as a uint64_t,
// the returned index is UINT64_MAX.
template <typename CharT>
mozilla::Maybe<uint64_t> StringToTypedArrayIndex(mozilla::Range<const CharT> s);

// A string |s| is a TypedArray index (or: canonical numeric index string) iff
// |s| is "-0" or |SameValue(ToString(ToNumber(s)), s)| is true. So check for
// any characters which can start the string representation of a number,
// including "NaN" and "Infinity".
template <typename CharT>
inline bool CanStartTypedArrayIndex(CharT ch) {
  return mozilla::IsAsciiDigit(ch) || ch == '-' || ch == 'N' || ch == 'I';
}

[[nodiscard]] inline mozilla::Maybe<uint64_t> ToTypedArrayIndex(jsid id) {
  if (id.isInt()) {
    int32_t i = id.toInt();
    MOZ_ASSERT(i >= 0);
    return mozilla::Some(i);
  }

  if (MOZ_UNLIKELY(!id.isString())) {
    return mozilla::Nothing();
  }

  JS::AutoCheckCannotGC nogc;
  JSAtom* atom = id.toAtom();

  if (atom->empty() || !CanStartTypedArrayIndex(atom->latin1OrTwoByteChar(0))) {
    return mozilla::Nothing();
  }

  if (atom->hasLatin1Chars()) {
    mozilla::Range<const Latin1Char> chars = atom->latin1Range(nogc);
    return StringToTypedArrayIndex(chars);
  }

  mozilla::Range<const char16_t> chars = atom->twoByteRange(nogc);
  return StringToTypedArrayIndex(chars);
}

bool SetTypedArrayElement(JSContext* cx, Handle<TypedArrayObject*> obj,
                          uint64_t index, HandleValue v,
                          ObjectOpResult& result);

bool SetTypedArrayElementOutOfBounds(JSContext* cx,
                                     Handle<TypedArrayObject*> obj,
                                     uint64_t index, HandleValue v,
                                     ObjectOpResult& result);

/*
 * Implements [[DefineOwnProperty]] for TypedArrays when the property
 * key is a TypedArray index.
 */
bool DefineTypedArrayElement(JSContext* cx, Handle<TypedArrayObject*> obj,
                             uint64_t index, Handle<PropertyDescriptor> desc,
                             ObjectOpResult& result);

static inline constexpr unsigned TypedArrayShift(Scalar::Type viewType) {
  switch (viewType) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
      return 0;
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Float16:
      return 1;
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
      return 2;
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::Int64:
    case Scalar::Float64:
      return 3;
    default:
      MOZ_CRASH("Unexpected array type");
  }
}

static inline constexpr unsigned TypedArrayElemSize(Scalar::Type viewType) {
  return 1u << TypedArrayShift(viewType);
}

extern ArraySortResult TypedArraySortFromJit(
    JSContext* cx, jit::TrampolineNativeFrameLayout* frame);

}  // namespace js

template <>
inline bool JSObject::is<js::TypedArrayObject>() const {
  return js::IsTypedArrayClass(getClass());
}

template <>
inline bool JSObject::is<js::FixedLengthTypedArrayObject>() const {
  return js::IsFixedLengthTypedArrayClass(getClass());
}

template <>
inline bool JSObject::is<js::ResizableTypedArrayObject>() const {
  return js::IsResizableTypedArrayClass(getClass());
}

#endif /* vm_TypedArrayObject_h */
