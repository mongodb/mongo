/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_TypedArrayObject_inl_h
#define vm_TypedArrayObject_inl_h

/* Utilities and common inline code for TypedArray */

#include "vm/TypedArrayObject.h"

#include "mozilla/Assertions.h"
#include "mozilla/Compiler.h"
#include "mozilla/FloatingPoint.h"

#include <algorithm>
#include <type_traits>

#include "jsnum.h"

#include "builtin/Array.h"
#include "gc/Zone.h"
#include "jit/AtomicOperations.h"
#include "js/Conversions.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "js/Value.h"
#include "util/DifferentialTesting.h"
#include "util/Memory.h"
#include "vm/BigIntType.h"
#include "vm/JSContext.h"
#include "vm/NativeObject.h"
#include "vm/Uint8Clamped.h"

#include "gc/ObjectKind-inl.h"
#include "vm/ObjectOperations-inl.h"

namespace js {

template <typename To, typename From>
inline To ConvertNumber(From src);

template <>
inline int8_t ConvertNumber<int8_t, float>(float src) {
  return JS::ToInt8(src);
}

template <>
inline uint8_t ConvertNumber<uint8_t, float>(float src) {
  return JS::ToUint8(src);
}

template <>
inline uint8_clamped ConvertNumber<uint8_clamped, float>(float src) {
  return uint8_clamped(src);
}

template <>
inline int16_t ConvertNumber<int16_t, float>(float src) {
  return JS::ToInt16(src);
}

template <>
inline uint16_t ConvertNumber<uint16_t, float>(float src) {
  return JS::ToUint16(src);
}

template <>
inline int32_t ConvertNumber<int32_t, float>(float src) {
  return JS::ToInt32(src);
}

template <>
inline uint32_t ConvertNumber<uint32_t, float>(float src) {
  return JS::ToUint32(src);
}

template <>
inline int64_t ConvertNumber<int64_t, float>(float src) {
  return JS::ToInt64(src);
}

template <>
inline uint64_t ConvertNumber<uint64_t, float>(float src) {
  return JS::ToUint64(src);
}

template <>
inline int8_t ConvertNumber<int8_t, double>(double src) {
  return JS::ToInt8(src);
}

template <>
inline uint8_t ConvertNumber<uint8_t, double>(double src) {
  return JS::ToUint8(src);
}

template <>
inline uint8_clamped ConvertNumber<uint8_clamped, double>(double src) {
  return uint8_clamped(src);
}

template <>
inline int16_t ConvertNumber<int16_t, double>(double src) {
  return JS::ToInt16(src);
}

template <>
inline uint16_t ConvertNumber<uint16_t, double>(double src) {
  return JS::ToUint16(src);
}

template <>
inline int32_t ConvertNumber<int32_t, double>(double src) {
  return JS::ToInt32(src);
}

template <>
inline uint32_t ConvertNumber<uint32_t, double>(double src) {
  return JS::ToUint32(src);
}

template <>
inline int64_t ConvertNumber<int64_t, double>(double src) {
  return JS::ToInt64(src);
}

template <>
inline uint64_t ConvertNumber<uint64_t, double>(double src) {
  return JS::ToUint64(src);
}

template <typename To, typename From>
inline To ConvertNumber(From src) {
  static_assert(
      !std::is_floating_point_v<From> ||
          (std::is_floating_point_v<From> && std::is_floating_point_v<To>),
      "conversion from floating point to int should have been handled by "
      "specializations above");
  return To(src);
}

template <typename NativeType>
struct TypeIDOfType;
template <>
struct TypeIDOfType<int8_t> {
  static const Scalar::Type id = Scalar::Int8;
  static const JSProtoKey protoKey = JSProto_Int8Array;
};
template <>
struct TypeIDOfType<uint8_t> {
  static const Scalar::Type id = Scalar::Uint8;
  static const JSProtoKey protoKey = JSProto_Uint8Array;
};
template <>
struct TypeIDOfType<int16_t> {
  static const Scalar::Type id = Scalar::Int16;
  static const JSProtoKey protoKey = JSProto_Int16Array;
};
template <>
struct TypeIDOfType<uint16_t> {
  static const Scalar::Type id = Scalar::Uint16;
  static const JSProtoKey protoKey = JSProto_Uint16Array;
};
template <>
struct TypeIDOfType<int32_t> {
  static const Scalar::Type id = Scalar::Int32;
  static const JSProtoKey protoKey = JSProto_Int32Array;
};
template <>
struct TypeIDOfType<uint32_t> {
  static const Scalar::Type id = Scalar::Uint32;
  static const JSProtoKey protoKey = JSProto_Uint32Array;
};
template <>
struct TypeIDOfType<int64_t> {
  static const Scalar::Type id = Scalar::BigInt64;
  static const JSProtoKey protoKey = JSProto_BigInt64Array;
};
template <>
struct TypeIDOfType<uint64_t> {
  static const Scalar::Type id = Scalar::BigUint64;
  static const JSProtoKey protoKey = JSProto_BigUint64Array;
};
template <>
struct TypeIDOfType<float> {
  static const Scalar::Type id = Scalar::Float32;
  static const JSProtoKey protoKey = JSProto_Float32Array;
};
template <>
struct TypeIDOfType<double> {
  static const Scalar::Type id = Scalar::Float64;
  static const JSProtoKey protoKey = JSProto_Float64Array;
};
template <>
struct TypeIDOfType<uint8_clamped> {
  static const Scalar::Type id = Scalar::Uint8Clamped;
  static const JSProtoKey protoKey = JSProto_Uint8ClampedArray;
};

class SharedOps {
 public:
  template <typename T>
  static T load(SharedMem<T*> addr) {
    return js::jit::AtomicOperations::loadSafeWhenRacy(addr);
  }

  template <typename T>
  static void store(SharedMem<T*> addr, T value) {
    js::jit::AtomicOperations::storeSafeWhenRacy(addr, value);
  }

  template <typename T>
  static void memcpy(SharedMem<T*> dest, SharedMem<T*> src, size_t size) {
    js::jit::AtomicOperations::memcpySafeWhenRacy(dest, src, size);
  }

  template <typename T>
  static void memmove(SharedMem<T*> dest, SharedMem<T*> src, size_t size) {
    js::jit::AtomicOperations::memmoveSafeWhenRacy(dest, src, size);
  }

  template <typename T>
  static void podCopy(SharedMem<T*> dest, SharedMem<T*> src, size_t nelem) {
    js::jit::AtomicOperations::podCopySafeWhenRacy(dest, src, nelem);
  }

  template <typename T>
  static void podMove(SharedMem<T*> dest, SharedMem<T*> src, size_t nelem) {
    js::jit::AtomicOperations::podMoveSafeWhenRacy(dest, src, nelem);
  }

  static SharedMem<void*> extract(TypedArrayObject* obj) {
    return obj->dataPointerEither();
  }
};

class UnsharedOps {
 public:
  template <typename T>
  static T load(SharedMem<T*> addr) {
    return *addr.unwrapUnshared();
  }

  template <typename T>
  static void store(SharedMem<T*> addr, T value) {
    *addr.unwrapUnshared() = value;
  }

  template <typename T>
  static void memcpy(SharedMem<T*> dest, SharedMem<T*> src, size_t size) {
    ::memcpy(dest.unwrapUnshared(), src.unwrapUnshared(), size);
  }

  template <typename T>
  static void memmove(SharedMem<T*> dest, SharedMem<T*> src, size_t size) {
    ::memmove(dest.unwrapUnshared(), src.unwrapUnshared(), size);
  }

  template <typename T>
  static void podCopy(SharedMem<T*> dest, SharedMem<T*> src, size_t nelem) {
    // std::copy_n better matches the argument values/types of this
    // function, but as noted below it allows the input/output ranges to
    // overlap.  std::copy does not, so use it so the compiler has extra
    // ability to optimize.
    const auto* first = src.unwrapUnshared();
    const auto* last = first + nelem;
    auto* result = dest.unwrapUnshared();
    std::copy(first, last, result);
  }

  template <typename T>
  static void podMove(SharedMem<T*> dest, SharedMem<T*> src, size_t n) {
    // std::copy_n copies from |src| to |dest| starting from |src|, so
    // input/output ranges *may* permissibly overlap, as this function
    // allows.
    const auto* start = src.unwrapUnshared();
    auto* result = dest.unwrapUnshared();
    std::copy_n(start, n, result);
  }

  static SharedMem<void*> extract(TypedArrayObject* obj) {
    return SharedMem<void*>::unshared(obj->dataPointerUnshared());
  }
};

template <typename T, typename Ops>
class ElementSpecific {
 public:
  /*
   * Copy |source|'s elements into |target|, starting at |target[offset]|.
   * Act as if the assignments occurred from a fresh copy of |source|, in
   * case the two memory ranges overlap.
   */
  static bool setFromTypedArray(Handle<TypedArrayObject*> target,
                                Handle<TypedArrayObject*> source,
                                size_t offset) {
    // WARNING: |source| may be an unwrapped typed array from a different
    // compartment. Proceed with caution!

    MOZ_ASSERT(TypeIDOfType<T>::id == target->type(),
               "calling wrong setFromTypedArray specialization");
    MOZ_ASSERT(!target->hasDetachedBuffer(), "target isn't detached");
    MOZ_ASSERT(!source->hasDetachedBuffer(), "source isn't detached");

    MOZ_ASSERT(offset <= target->length());
    MOZ_ASSERT(source->length() <= target->length() - offset);

    if (TypedArrayObject::sameBuffer(target, source)) {
      return setFromOverlappingTypedArray(target, source, offset);
    }

    SharedMem<T*> dest =
        target->dataPointerEither().template cast<T*>() + offset;
    size_t count = source->length();

    if (source->type() == target->type()) {
      Ops::podCopy(dest, source->dataPointerEither().template cast<T*>(),
                   count);
      return true;
    }

    SharedMem<void*> data = Ops::extract(source);
    switch (source->type()) {
      case Scalar::Int8: {
        SharedMem<int8_t*> src = data.cast<int8_t*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      case Scalar::Uint8:
      case Scalar::Uint8Clamped: {
        SharedMem<uint8_t*> src = data.cast<uint8_t*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      case Scalar::Int16: {
        SharedMem<int16_t*> src = data.cast<int16_t*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      case Scalar::Uint16: {
        SharedMem<uint16_t*> src = data.cast<uint16_t*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      case Scalar::Int32: {
        SharedMem<int32_t*> src = data.cast<int32_t*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      case Scalar::Uint32: {
        SharedMem<uint32_t*> src = data.cast<uint32_t*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      case Scalar::BigInt64: {
        SharedMem<int64_t*> src = data.cast<int64_t*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      case Scalar::BigUint64: {
        SharedMem<uint64_t*> src = data.cast<uint64_t*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      case Scalar::Float32: {
        SharedMem<float*> src = data.cast<float*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      case Scalar::Float64: {
        SharedMem<double*> src = data.cast<double*>();
        for (size_t i = 0; i < count; ++i) {
          Ops::store(dest++, ConvertNumber<T>(Ops::load(src++)));
        }
        break;
      }
      default:
        MOZ_CRASH("setFromTypedArray with a typed array with bogus type");
    }

    return true;
  }

  /*
   * Copy |source[0]| to |source[len]| (exclusive) elements into the typed
   * array |target|, starting at index |offset|.  |source| must not be a
   * typed array.
   */
  static bool setFromNonTypedArray(JSContext* cx,
                                   Handle<TypedArrayObject*> target,
                                   HandleObject source, size_t len,
                                   size_t offset = 0) {
    MOZ_ASSERT(target->type() == TypeIDOfType<T>::id,
               "target type and NativeType must match");
    MOZ_ASSERT(!target->hasDetachedBuffer(), "target isn't detached");
    MOZ_ASSERT(!source->is<TypedArrayObject>(),
               "use setFromTypedArray instead of this method");

    size_t i = 0;
    if (source->is<NativeObject>()) {
      // Attempt fast-path infallible conversion of dense elements up to
      // the first potentially side-effectful lookup or conversion.
      size_t bound = std::min<size_t>(
          source->as<NativeObject>().getDenseInitializedLength(), len);

      SharedMem<T*> dest =
          target->dataPointerEither().template cast<T*>() + offset;

      MOZ_ASSERT(!canConvertInfallibly(MagicValue(JS_ELEMENTS_HOLE)),
                 "the following loop must abort on holes");

      const Value* srcValues = source->as<NativeObject>().getDenseElements();
      for (; i < bound; i++) {
        if (!canConvertInfallibly(srcValues[i])) {
          break;
        }
        Ops::store(dest + i, infallibleValueToNative(srcValues[i]));
      }
      if (i == len) {
        return true;
      }
    }

    // Convert and copy any remaining elements generically.
    RootedValue v(cx);
    for (; i < len; i++) {
      if constexpr (sizeof(i) == sizeof(uint32_t)) {
        if (!GetElement(cx, source, source, uint32_t(i), &v)) {
          return false;
        }
      } else {
        if (!GetElementLargeIndex(cx, source, source, i, &v)) {
          return false;
        }
      }

      T n;
      if (!valueToNative(cx, v, &n)) {
        return false;
      }

      len = std::min<size_t>(len, target->length());
      if (i >= len) {
        break;
      }

      // Compute every iteration in case getElement/valueToNative
      // detaches the underlying array buffer or GC moves the data.
      SharedMem<T*> dest =
          target->dataPointerEither().template cast<T*>() + offset + i;
      Ops::store(dest, n);
    }

    return true;
  }

  /*
   * Copy |source| into the typed array |target|.
   */
  static bool initFromIterablePackedArray(JSContext* cx,
                                          Handle<TypedArrayObject*> target,
                                          HandleArrayObject source) {
    MOZ_ASSERT(target->type() == TypeIDOfType<T>::id,
               "target type and NativeType must match");
    MOZ_ASSERT(!target->hasDetachedBuffer(), "target isn't detached");
    MOZ_ASSERT(IsPackedArray(source), "source array must be packed");
    MOZ_ASSERT(source->getDenseInitializedLength() <= target->length());

    size_t len = source->getDenseInitializedLength();
    size_t i = 0;

    // Attempt fast-path infallible conversion of dense elements up to the
    // first potentially side-effectful conversion.

    SharedMem<T*> dest = target->dataPointerEither().template cast<T*>();

    const Value* srcValues = source->getDenseElements();
    for (; i < len; i++) {
      if (!canConvertInfallibly(srcValues[i])) {
        break;
      }
      Ops::store(dest + i, infallibleValueToNative(srcValues[i]));
    }
    if (i == len) {
      return true;
    }

    // Convert any remaining elements by first collecting them into a
    // temporary list, and then copying them into the typed array.
    RootedValueVector values(cx);
    if (!values.append(srcValues + i, len - i)) {
      return false;
    }

    RootedValue v(cx);
    for (size_t j = 0; j < values.length(); i++, j++) {
      v = values[j];

      T n;
      if (!valueToNative(cx, v, &n)) {
        return false;
      }

      // |target| is a newly allocated typed array and not yet visible to
      // content script, so valueToNative can't detach the underlying
      // buffer.
      MOZ_ASSERT(i < target->length());

      // Compute every iteration in case GC moves the data.
      SharedMem<T*> newDest = target->dataPointerEither().template cast<T*>();
      Ops::store(newDest + i, n);
    }

    return true;
  }

 private:
  static bool setFromOverlappingTypedArray(Handle<TypedArrayObject*> target,
                                           Handle<TypedArrayObject*> source,
                                           size_t offset) {
    // WARNING: |source| may be an unwrapped typed array from a different
    // compartment. Proceed with caution!

    MOZ_ASSERT(TypeIDOfType<T>::id == target->type(),
               "calling wrong setFromTypedArray specialization");
    MOZ_ASSERT(!target->hasDetachedBuffer(), "target isn't detached");
    MOZ_ASSERT(!source->hasDetachedBuffer(), "source isn't detached");
    MOZ_ASSERT(TypedArrayObject::sameBuffer(target, source),
               "the provided arrays don't actually overlap, so it's "
               "undesirable to use this method");

    MOZ_ASSERT(offset <= target->length());
    MOZ_ASSERT(source->length() <= target->length() - offset);

    SharedMem<T*> dest =
        target->dataPointerEither().template cast<T*>() + offset;
    size_t len = source->length();

    if (source->type() == target->type()) {
      SharedMem<T*> src = source->dataPointerEither().template cast<T*>();
      Ops::podMove(dest, src, len);
      return true;
    }

    // Copy |source| in case it overlaps the target elements being set.
    size_t sourceByteLen = len * source->bytesPerElement();
    void* data = target->zone()->template pod_malloc<uint8_t>(sourceByteLen);
    if (!data) {
      return false;
    }
    Ops::memcpy(SharedMem<void*>::unshared(data), source->dataPointerEither(),
                sourceByteLen);

    switch (source->type()) {
      case Scalar::Int8: {
        int8_t* src = static_cast<int8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      case Scalar::Uint8:
      case Scalar::Uint8Clamped: {
        uint8_t* src = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      case Scalar::Int16: {
        int16_t* src = static_cast<int16_t*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      case Scalar::Uint16: {
        uint16_t* src = static_cast<uint16_t*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      case Scalar::Int32: {
        int32_t* src = static_cast<int32_t*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      case Scalar::Uint32: {
        uint32_t* src = static_cast<uint32_t*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      case Scalar::BigInt64: {
        int64_t* src = static_cast<int64_t*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      case Scalar::BigUint64: {
        uint64_t* src = static_cast<uint64_t*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      case Scalar::Float32: {
        float* src = static_cast<float*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      case Scalar::Float64: {
        double* src = static_cast<double*>(data);
        for (size_t i = 0; i < len; ++i) {
          Ops::store(dest++, ConvertNumber<T>(*src++));
        }
        break;
      }
      default:
        MOZ_CRASH(
            "setFromOverlappingTypedArray with a typed array with bogus type");
    }

    js_free(data);
    return true;
  }

  static bool canConvertInfallibly(const Value& v) {
    if (TypeIDOfType<T>::id == Scalar::BigInt64 ||
        TypeIDOfType<T>::id == Scalar::BigUint64) {
      // Numbers, Null, Undefined, and Symbols throw a TypeError. Strings may
      // OOM and Objects may have side-effects.
      return v.isBigInt() || v.isBoolean();
    }
    // BigInts and Symbols throw a TypeError. Strings may OOM and Objects may
    // have side-effects.
    return v.isNumber() || v.isBoolean() || v.isNull() || v.isUndefined();
  }

  static T infallibleValueToNative(const Value& v) {
    if (TypeIDOfType<T>::id == Scalar::BigInt64) {
      if (v.isBigInt()) {
        return T(BigInt::toInt64(v.toBigInt()));
      }
      return T(v.toBoolean());
    }
    if (TypeIDOfType<T>::id == Scalar::BigUint64) {
      if (v.isBigInt()) {
        return T(BigInt::toUint64(v.toBigInt()));
      }
      return T(v.toBoolean());
    }
    if (v.isInt32()) {
      return T(v.toInt32());
    }
    if (v.isDouble()) {
      return doubleToNative(v.toDouble());
    }
    if (v.isBoolean()) {
      return T(v.toBoolean());
    }
    if (v.isNull()) {
      return T(0);
    }

    MOZ_ASSERT(v.isUndefined());
    return TypeIsFloatingPoint<T>() ? T(JS::GenericNaN()) : T(0);
  }

  static bool valueToNative(JSContext* cx, HandleValue v, T* result) {
    MOZ_ASSERT(!v.isMagic());

    if (MOZ_LIKELY(canConvertInfallibly(v))) {
      *result = infallibleValueToNative(v);
      return true;
    }

    if (std::is_same_v<T, int64_t>) {
      JS_TRY_VAR_OR_RETURN_FALSE(cx, *result, ToBigInt64(cx, v));
      return true;
    }

    if (std::is_same_v<T, uint64_t>) {
      JS_TRY_VAR_OR_RETURN_FALSE(cx, *result, ToBigUint64(cx, v));
      return true;
    }

    double d;
    MOZ_ASSERT(v.isString() || v.isObject() || v.isSymbol() || v.isBigInt());
    if (!(v.isString() ? StringToNumber(cx, v.toString(), &d)
                       : ToNumber(cx, v, &d))) {
      return false;
    }

    *result = doubleToNative(d);
    return true;
  }

  static T doubleToNative(double d) {
    if (TypeIsFloatingPoint<T>()) {
      // The JS spec doesn't distinguish among different NaN values, and
      // it deliberately doesn't specify the bit pattern written to a
      // typed array when NaN is written into it.  This bit-pattern
      // inconsistency could confuse differential testing, so always
      // canonicalize NaN values in differential testing.
      if (js::SupportDifferentialTesting()) {
        d = JS::CanonicalizeNaN(d);
      }
      return T(d);
    }
    if (MOZ_UNLIKELY(mozilla::IsNaN(d))) {
      return T(0);
    }
    if (TypeIDOfType<T>::id == Scalar::Uint8Clamped) {
      return T(d);
    }
    if (TypeIsUnsigned<T>()) {
      return T(JS::ToUint32(d));
    }
    return T(JS::ToInt32(d));
  }
};

/* static */ gc::AllocKind js::TypedArrayObject::AllocKindForLazyBuffer(
    size_t nbytes) {
  MOZ_ASSERT(nbytes <= INLINE_BUFFER_LIMIT);
  if (nbytes == 0) {
    nbytes += sizeof(uint8_t);
  }
  size_t dataSlots = AlignBytes(nbytes, sizeof(Value)) / sizeof(Value);
  MOZ_ASSERT(nbytes <= dataSlots * sizeof(Value));
  return gc::GetGCObjectKind(FIXED_DATA_START + dataSlots);
}

}  // namespace js

#endif  // vm_TypedArrayObject_inl_h
