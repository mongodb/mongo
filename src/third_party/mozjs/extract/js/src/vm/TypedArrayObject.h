/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_TypedArrayObject_h
#define vm_TypedArrayObject_h

#include "mozilla/Maybe.h"
#include "mozilla/TextUtils.h"

#include "gc/Barrier.h"
#include "gc/MaybeRooted.h"
#include "js/Class.h"
#include "js/experimental/TypedData.h"  // js::detail::TypedArrayLengthSlot
#include "js/Result.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "vm/ArrayBufferObject.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/JSObject.h"
#include "vm/SharedArrayObject.h"

#define JS_FOR_EACH_TYPED_ARRAY(MACRO) \
  MACRO(int8_t, Int8)                  \
  MACRO(uint8_t, Uint8)                \
  MACRO(int16_t, Int16)                \
  MACRO(uint16_t, Uint16)              \
  MACRO(int32_t, Int32)                \
  MACRO(uint32_t, Uint32)              \
  MACRO(float, Float32)                \
  MACRO(double, Float64)               \
  MACRO(uint8_clamped, Uint8Clamped)   \
  MACRO(int64_t, BigInt64)             \
  MACRO(uint64_t, BigUint64)

namespace js {

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

  static const JSClass classes[Scalar::MaxTypedArrayViewType];
  static const JSClass protoClasses[Scalar::MaxTypedArrayViewType];
  static const JSClass sharedTypedArrayPrototypeClass;

  static const JSClass* classForType(Scalar::Type type) {
    MOZ_ASSERT(type < Scalar::MaxTypedArrayViewType);
    return &classes[type];
  }

  static const JSClass* protoClassForType(Scalar::Type type) {
    MOZ_ASSERT(type < Scalar::MaxTypedArrayViewType);
    return &protoClasses[type];
  }

  static constexpr size_t FIXED_DATA_START = DATA_SLOT + 1;

  // For typed arrays which can store their data inline, the array buffer
  // object is created lazily.
  static constexpr uint32_t INLINE_BUFFER_LIMIT =
      (NativeObject::MAX_FIXED_SLOTS - FIXED_DATA_START) * sizeof(Value);

  static inline gc::AllocKind AllocKindForLazyBuffer(size_t nbytes);

  inline Scalar::Type type() const;
  inline size_t bytesPerElement() const;

  static bool ensureHasBuffer(JSContext* cx, Handle<TypedArrayObject*> tarray);

  size_t byteLength() const { return length() * bytesPerElement(); }

  size_t length() const {
    return size_t(getFixedSlot(LENGTH_SLOT).toPrivate());
  }

  Value byteLengthValue() const {
    size_t len = byteLength();
    return NumberValue(len);
  }

  Value lengthValue() const {
    size_t len = length();
    return NumberValue(len);
  }

  bool hasInlineElements() const;
  void setInlineElements();
  uint8_t* elementsRaw() const {
    return *(uint8_t**)((((char*)this) + ArrayBufferViewObject::dataOffset()));
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

  template <AllowGC allowGC>
  bool getElement(JSContext* cx, size_t index,
                  typename MaybeRooted<Value, allowGC>::MutableHandleType val);
  bool getElementPure(size_t index, Value* vp);

  /*
   * Copy all elements from this typed array to vp. vp must point to rooted
   * memory.
   */
  static bool getElements(JSContext* cx, Handle<TypedArrayObject*> tarray,
                          Value* vp);

  static bool GetTemplateObjectForNative(JSContext* cx, Native native,
                                         const JS::HandleValueArray args,
                                         MutableHandleObject res);

  /*
   * Maximum allowed byte length for any typed array.
   */
  static size_t maxByteLength() {
    return ArrayBufferObject::maxBufferByteLength();
  }

  static bool isOriginalLengthGetter(Native native);

  static bool isOriginalByteOffsetGetter(Native native);

  static bool isOriginalByteLengthGetter(Native native);

  static void finalize(JSFreeOp* fop, JSObject* obj);
  static size_t objectMoved(JSObject* obj, JSObject* old);

  /* Initialization bits */

  static const JSFunctionSpec protoFunctions[];
  static const JSPropertySpec protoAccessors[];
  static const JSFunctionSpec staticFunctions[];
  static const JSPropertySpec staticProperties[];

  /* Accessors and functions */

  static bool is(HandleValue v);

  static bool set(JSContext* cx, unsigned argc, Value* vp);
  static bool copyWithin(JSContext* cx, unsigned argc, Value* vp);

  bool convertForSideEffect(JSContext* cx, HandleValue v) const;

 private:
  static bool set_impl(JSContext* cx, const CallArgs& args);
  static bool copyWithin_impl(JSContext* cx, const CallArgs& args);
};

[[nodiscard]] bool TypedArray_bufferGetter(JSContext* cx, unsigned argc,
                                           Value* vp);

extern TypedArrayObject* NewTypedArrayWithTemplateAndLength(
    JSContext* cx, HandleObject templateObj, int32_t len);

extern TypedArrayObject* NewTypedArrayWithTemplateAndArray(
    JSContext* cx, HandleObject templateObj, HandleObject array);

extern TypedArrayObject* NewTypedArrayWithTemplateAndBuffer(
    JSContext* cx, HandleObject templateObj, HandleObject arrayBuffer,
    HandleValue byteOffset, HandleValue length);

inline bool IsTypedArrayClass(const JSClass* clasp) {
  return &TypedArrayObject::classes[0] <= clasp &&
         clasp < &TypedArrayObject::classes[Scalar::MaxTypedArrayViewType];
}

inline Scalar::Type GetTypedArrayClassType(const JSClass* clasp) {
  MOZ_ASSERT(IsTypedArrayClass(clasp));
  return static_cast<Scalar::Type>(clasp - &TypedArrayObject::classes[0]);
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
[[nodiscard]] bool StringToTypedArrayIndex(JSContext* cx,
                                           mozilla::Range<const CharT> s,
                                           mozilla::Maybe<uint64_t>* indexp);

// A string |s| is a TypedArray index (or: canonical numeric index string) iff
// |s| is "-0" or |SameValue(ToString(ToNumber(s)), s)| is true. So check for
// any characters which can start the string representation of a number,
// including "NaN" and "Infinity".
template <typename CharT>
inline bool CanStartTypedArrayIndex(CharT ch) {
  return mozilla::IsAsciiDigit(ch) || ch == '-' || ch == 'N' || ch == 'I';
}

[[nodiscard]] inline bool ToTypedArrayIndex(JSContext* cx, jsid id,
                                            mozilla::Maybe<uint64_t>* indexp) {
  if (JSID_IS_INT(id)) {
    int32_t i = JSID_TO_INT(id);
    MOZ_ASSERT(i >= 0);
    indexp->emplace(i);
    return true;
  }

  if (MOZ_UNLIKELY(!JSID_IS_STRING(id))) {
    MOZ_ASSERT(indexp->isNothing());
    return true;
  }

  JS::AutoCheckCannotGC nogc;
  JSAtom* atom = id.toAtom();

  if (atom->empty() || !CanStartTypedArrayIndex(atom->latin1OrTwoByteChar(0))) {
    MOZ_ASSERT(indexp->isNothing());
    return true;
  }

  if (atom->hasLatin1Chars()) {
    mozilla::Range<const Latin1Char> chars = atom->latin1Range(nogc);
    return StringToTypedArrayIndex(cx, chars, indexp);
  }

  mozilla::Range<const char16_t> chars = atom->twoByteRange(nogc);
  return StringToTypedArrayIndex(cx, chars, indexp);
}

bool SetTypedArrayElement(JSContext* cx, Handle<TypedArrayObject*> obj,
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

}  // namespace js

template <>
inline bool JSObject::is<js::TypedArrayObject>() const {
  return js::IsTypedArrayClass(getClass());
}

#endif /* vm_TypedArrayObject_h */
