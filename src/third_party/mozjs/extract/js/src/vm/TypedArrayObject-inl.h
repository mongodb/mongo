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

#include "gc/Zone.h"
#include "jit/AtomicOperations.h"
#include "js/Conversions.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "js/Value.h"
#include "util/DifferentialTesting.h"
#include "util/Memory.h"
#include "vm/ArrayObject.h"
#include "vm/BigIntType.h"
#include "vm/Float16.h"
#include "vm/NativeObject.h"
#include "vm/Uint8Clamped.h"

#include "gc/ObjectKind-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

namespace js {

// Use static_assert in compilers which support CWG2518. In all other cases
// fall back to MOZ_CRASH.
//
// https://cplusplus.github.io/CWG/issues/2518.html
#if defined(__clang__)
#  if (__clang_major__ >= 17)
#    define STATIC_ASSERT_IN_UNEVALUATED_CONTEXT 1
#  else
#    define STATIC_ASSERT_IN_UNEVALUATED_CONTEXT 0
#  endif
#elif MOZ_IS_GCC
#  if MOZ_GCC_VERSION_AT_LEAST(13, 1, 0)
#    define STATIC_ASSERT_IN_UNEVALUATED_CONTEXT 1
#  else
#    define STATIC_ASSERT_IN_UNEVALUATED_CONTEXT 0
#  endif
#else
#  define STATIC_ASSERT_IN_UNEVALUATED_CONTEXT 0
#endif

template <typename T>
inline auto ToFloatingPoint(T value) {
  static_assert(!std::numeric_limits<T>::is_integer);

  if constexpr (std::is_floating_point_v<T>) {
    return value;
  } else {
    return static_cast<double>(value);
  }
}

template <typename To, typename From>
inline To ConvertNumber(From src) {
  if constexpr (!std::numeric_limits<From>::is_integer) {
    if constexpr (std::is_same_v<From, To>) {
      return src;
    } else if constexpr (!std::numeric_limits<To>::is_integer) {
      return static_cast<To>(ToFloatingPoint(src));
    } else if constexpr (std::is_same_v<int8_t, To>) {
      return JS::ToInt8(ToFloatingPoint(src));
    } else if constexpr (std::is_same_v<uint8_t, To>) {
      return JS::ToUint8(ToFloatingPoint(src));
    } else if constexpr (std::is_same_v<uint8_clamped, To>) {
      return uint8_clamped(ToFloatingPoint(src));
    } else if constexpr (std::is_same_v<int16_t, To>) {
      return JS::ToInt16(ToFloatingPoint(src));
    } else if constexpr (std::is_same_v<uint16_t, To>) {
      return JS::ToUint16(ToFloatingPoint(src));
    } else if constexpr (std::is_same_v<int32_t, To>) {
      return JS::ToInt32(ToFloatingPoint(src));
    } else if constexpr (std::is_same_v<uint32_t, To>) {
      return JS::ToUint32(ToFloatingPoint(src));
    } else {
#if STATIC_ASSERT_IN_UNEVALUATED_CONTEXT
      static_assert(false,
                    "conversion from floating point to int should have been "
                    "handled by specializations above");
#else
      MOZ_CRASH(
          "conversion from floating point to int should have been "
          "handled by specializations above");
#endif
    }
  } else {
    return static_cast<To>(src);
  }
}

#undef STATIC_ASSERT_IN_UNEVALUATED_CONTEXT

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
struct TypeIDOfType<float16> {
  static const Scalar::Type id = Scalar::Float16;
  static const JSProtoKey protoKey = JSProto_Float16Array;
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

/**
 * Check if |targetType| and |sourceType| have compatible bit-level
 * representations to allow bitwise copying.
 */
constexpr bool CanUseBitwiseCopy(Scalar::Type targetType,
                                 Scalar::Type sourceType) {
  switch (targetType) {
    case Scalar::Int8:
    case Scalar::Uint8:
      return sourceType == Scalar::Int8 || sourceType == Scalar::Uint8 ||
             sourceType == Scalar::Uint8Clamped;

    case Scalar::Uint8Clamped:
      return sourceType == Scalar::Uint8 || sourceType == Scalar::Uint8Clamped;

    case Scalar::Int16:
    case Scalar::Uint16:
      return sourceType == Scalar::Int16 || sourceType == Scalar::Uint16;

    case Scalar::Int32:
    case Scalar::Uint32:
      return sourceType == Scalar::Int32 || sourceType == Scalar::Uint32;

    case Scalar::Float16:
      return sourceType == Scalar::Float16;

    case Scalar::Float32:
      return sourceType == Scalar::Float32;

    case Scalar::Float64:
      return sourceType == Scalar::Float64;

    case Scalar::BigInt64:
    case Scalar::BigUint64:
      return sourceType == Scalar::BigInt64 || sourceType == Scalar::BigUint64;

    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      // GCC8 doesn't like MOZ_CRASH in constexpr functions, so we can't use it
      // here to catch invalid typed array types.
      break;
  }
  return false;
}

template <typename T, typename Ops>
class ElementSpecific {
  static constexpr bool canUseBitwiseCopy(Scalar::Type sourceType) {
    return CanUseBitwiseCopy(TypeIDOfType<T>::id, sourceType);
  }

  template <typename From>
  static inline constexpr bool canCopyBitwise =
      canUseBitwiseCopy(TypeIDOfType<From>::id);

  template <typename From, typename LoadOps = Ops>
  static typename std::enable_if_t<!canCopyBitwise<From>> store(
      SharedMem<T*> dest, SharedMem<void*> data, size_t count) {
    SharedMem<From*> src = data.cast<From*>();
    for (size_t i = 0; i < count; ++i) {
      Ops::store(dest++, ConvertNumber<T>(LoadOps::load(src++)));
    }
  }

  template <typename From, typename LoadOps = Ops>
  static typename std::enable_if_t<canCopyBitwise<From>> store(
      SharedMem<T*> dest, SharedMem<void*> data, size_t count) {
    MOZ_ASSERT_UNREACHABLE("caller handles bitwise copies");
  }

  template <typename LoadOps = Ops, typename U = T>
  static typename std::enable_if_t<!std::is_same_v<U, int64_t> &&
                                   !std::is_same_v<U, uint64_t>>
  storeTo(SharedMem<T*> dest, Scalar::Type type, SharedMem<void*> data,
          size_t count) {
    static_assert(std::is_same_v<T, U>,
                  "template parameter U only used to disable this declaration");
    switch (type) {
      case Scalar::Int8: {
        store<int8_t, LoadOps>(dest, data, count);
        break;
      }
      case Scalar::Uint8:
      case Scalar::Uint8Clamped: {
        store<uint8_t, LoadOps>(dest, data, count);
        break;
      }
      case Scalar::Int16: {
        store<int16_t, LoadOps>(dest, data, count);
        break;
      }
      case Scalar::Uint16: {
        store<uint16_t, LoadOps>(dest, data, count);
        break;
      }
      case Scalar::Int32: {
        store<int32_t, LoadOps>(dest, data, count);
        break;
      }
      case Scalar::Uint32: {
        store<uint32_t, LoadOps>(dest, data, count);
        break;
      }
      case Scalar::Float16: {
        store<float16, LoadOps>(dest, data, count);
        break;
      }
      case Scalar::Float32: {
        store<float, LoadOps>(dest, data, count);
        break;
      }
      case Scalar::Float64: {
        store<double, LoadOps>(dest, data, count);
        break;
      }
      case Scalar::BigInt64:
      case Scalar::BigUint64:
        MOZ_FALLTHROUGH_ASSERT("unexpected int64/uint64 typed array");
      default:
        MOZ_CRASH("setFromTypedArray with a typed array with bogus type");
    }
  }

  template <typename LoadOps = Ops, typename U = T>
  static typename std::enable_if_t<std::is_same_v<U, int64_t> ||
                                   std::is_same_v<U, uint64_t>>
  storeTo(SharedMem<T*> dest, Scalar::Type type, SharedMem<void*> data,
          size_t count) {
    static_assert(std::is_same_v<T, U>,
                  "template parameter U only used to disable this declaration");
    MOZ_ASSERT_UNREACHABLE("caller handles int64<>uint64 bitwise copies");
  }

 public:
  /*
   * Copy |source|'s elements into |target|, starting at |target[offset]|.
   * Act as if the assignments occurred from a fresh copy of |source|, in
   * case the two memory ranges overlap.
   */
  static bool setFromTypedArray(Handle<TypedArrayObject*> target,
                                size_t targetLength,
                                Handle<TypedArrayObject*> source,
                                size_t sourceLength, size_t offset) {
    // WARNING: |source| may be an unwrapped typed array from a different
    // compartment. Proceed with caution!

    MOZ_ASSERT(TypeIDOfType<T>::id == target->type(),
               "calling wrong setFromTypedArray specialization");
    MOZ_ASSERT(Scalar::isBigIntType(target->type()) ==
                   Scalar::isBigIntType(source->type()),
               "can't convert between BigInt and Number");
    MOZ_ASSERT(!target->hasDetachedBuffer(), "target isn't detached");
    MOZ_ASSERT(!source->hasDetachedBuffer(), "source isn't detached");
    MOZ_ASSERT(*target->length() >= targetLength, "target isn't shrunk");
    MOZ_ASSERT(*source->length() >= sourceLength, "source isn't shrunk");

    MOZ_ASSERT(offset <= targetLength);
    MOZ_ASSERT(sourceLength <= targetLength - offset);

    // Return early when copying no elements.
    //
    // Note: `SharedMem::cast` asserts the memory is properly aligned. Non-zero
    // memory is correctly aligned, this is statically asserted below. Zero
    // memory can have a different alignment, so we have to return early.
    if (sourceLength == 0) {
      return true;
    }

    if (TypedArrayObject::sameBuffer(target, source)) {
      return setFromOverlappingTypedArray(target, targetLength, source,
                                          sourceLength, offset);
    }

    // `malloc` returns memory at least as strictly aligned as for max_align_t
    // and the alignment of max_align_t is a multiple of the size of `T`,
    // so `SharedMem::cast` will be called with properly aligned memory.
    static_assert(alignof(std::max_align_t) % sizeof(T) == 0);

    SharedMem<T*> dest = Ops::extract(target).template cast<T*>() + offset;
    SharedMem<void*> data = Ops::extract(source);

    if (canUseBitwiseCopy(source->type())) {
      Ops::podCopy(dest, data.template cast<T*>(), sourceLength);
    } else {
      storeTo(dest, source->type(), data, sourceLength);
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
    MOZ_ASSERT(!source->is<TypedArrayObject>(),
               "use setFromTypedArray instead of this method");
    MOZ_ASSERT_IF(target->hasDetachedBuffer(), target->length().isNothing());

    size_t i = 0;
    if (source->is<NativeObject>()) {
      size_t targetLength = target->length().valueOr(0);
      if (offset <= targetLength && len <= targetLength - offset) {
        // Attempt fast-path infallible conversion of dense elements up to
        // the first potentially side-effectful lookup or conversion.
        size_t bound = std::min<size_t>(
            source->as<NativeObject>().getDenseInitializedLength(), len);

        SharedMem<T*> dest = Ops::extract(target).template cast<T*>() + offset;

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

      // Ignore out-of-bounds writes, but still execute getElement/valueToNative
      // because of observable side-effects.
      if (offset + i >= target->length().valueOr(0)) {
        continue;
      }

      MOZ_ASSERT(!target->hasDetachedBuffer());

      // Compute every iteration in case getElement/valueToNative
      // detaches the underlying array buffer or GC moves the data.
      SharedMem<T*> dest =
          Ops::extract(target).template cast<T*>() + offset + i;
      Ops::store(dest, n);
    }

    return true;
  }

  /*
   * Copy |source| into the typed array |target|.
   */
  static bool initFromIterablePackedArray(
      JSContext* cx, Handle<FixedLengthTypedArrayObject*> target,
      Handle<ArrayObject*> source) {
    MOZ_ASSERT(target->type() == TypeIDOfType<T>::id,
               "target type and NativeType must match");
    MOZ_ASSERT(!target->hasDetachedBuffer(), "target isn't detached");
    MOZ_ASSERT(IsPackedArray(source), "source array must be packed");
    MOZ_ASSERT(source->getDenseInitializedLength() <= target->length());

    size_t len = source->getDenseInitializedLength();
    size_t i = 0;

    // Attempt fast-path infallible conversion of dense elements up to the
    // first potentially side-effectful conversion.

    SharedMem<T*> dest = Ops::extract(target).template cast<T*>();

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
      SharedMem<T*> newDest = Ops::extract(target).template cast<T*>();
      Ops::store(newDest + i, n);
    }

    return true;
  }

 private:
  static bool setFromOverlappingTypedArray(Handle<TypedArrayObject*> target,
                                           size_t targetLength,
                                           Handle<TypedArrayObject*> source,
                                           size_t sourceLength, size_t offset) {
    // WARNING: |source| may be an unwrapped typed array from a different
    // compartment. Proceed with caution!

    MOZ_ASSERT(TypeIDOfType<T>::id == target->type(),
               "calling wrong setFromTypedArray specialization");
    MOZ_ASSERT(Scalar::isBigIntType(target->type()) ==
                   Scalar::isBigIntType(source->type()),
               "can't convert between BigInt and Number");
    MOZ_ASSERT(!target->hasDetachedBuffer(), "target isn't detached");
    MOZ_ASSERT(!source->hasDetachedBuffer(), "source isn't detached");
    MOZ_ASSERT(*target->length() >= targetLength, "target isn't shrunk");
    MOZ_ASSERT(*source->length() >= sourceLength, "source isn't shrunk");
    MOZ_ASSERT(TypedArrayObject::sameBuffer(target, source),
               "the provided arrays don't actually overlap, so it's "
               "undesirable to use this method");

    MOZ_ASSERT(offset <= targetLength);
    MOZ_ASSERT(sourceLength <= targetLength - offset);

    SharedMem<T*> dest = Ops::extract(target).template cast<T*>() + offset;
    size_t len = sourceLength;

    if (canUseBitwiseCopy(source->type())) {
      SharedMem<T*> src = Ops::extract(source).template cast<T*>();
      Ops::podMove(dest, src, len);
      return true;
    }

    // Copy |source| because it overlaps the target elements being set.
    size_t sourceByteLen = len * source->bytesPerElement();
    auto temp = target->zone()->template make_pod_array<uint8_t>(sourceByteLen);
    if (!temp) {
      return false;
    }

    auto data = SharedMem<void*>::unshared(temp.get());
    Ops::memcpy(data, Ops::extract(source), sourceByteLen);

    storeTo<UnsharedOps>(dest, source->type(), data, len);

    return true;
  }

  static bool canConvertInfallibly(const Value& v) {
    if (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t>) {
      // Numbers, Null, Undefined, and Symbols throw a TypeError. Strings may
      // OOM and Objects may have side-effects.
      return v.isBigInt() || v.isBoolean();
    }
    // BigInts and Symbols throw a TypeError. Strings may OOM and Objects may
    // have side-effects.
    return v.isNumber() || v.isBoolean() || v.isNull() || v.isUndefined();
  }

  static T infallibleValueToNative(const Value& v) {
    if constexpr (std::is_same_v<T, int64_t>) {
      if (v.isBigInt()) {
        return T(BigInt::toInt64(v.toBigInt()));
      }
      return T(v.toBoolean());
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      if (v.isBigInt()) {
        return T(BigInt::toUint64(v.toBigInt()));
      }
      return T(v.toBoolean());
    } else {
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
      return !std::numeric_limits<T>::is_integer ? T(JS::GenericNaN()) : T(0);
    }
  }

  static bool valueToNative(JSContext* cx, HandleValue v, T* result) {
    MOZ_ASSERT(!v.isMagic());

    if (MOZ_LIKELY(canConvertInfallibly(v))) {
      *result = infallibleValueToNative(v);
      return true;
    }

    if constexpr (std::is_same_v<T, int64_t>) {
      JS_TRY_VAR_OR_RETURN_FALSE(cx, *result, ToBigInt64(cx, v));
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      JS_TRY_VAR_OR_RETURN_FALSE(cx, *result, ToBigUint64(cx, v));
    } else {
      MOZ_ASSERT(v.isString() || v.isObject() || v.isSymbol() || v.isBigInt());

      double d;
      if (!(v.isString() ? StringToNumber(cx, v.toString(), &d)
                         : ToNumber(cx, v, &d))) {
        return false;
      }
      *result = doubleToNative(d);
    }
    return true;
  }

  static T doubleToNative(double d) {
    if constexpr (!std::numeric_limits<T>::is_integer) {
      // The JS spec doesn't distinguish among different NaN values, and
      // it deliberately doesn't specify the bit pattern written to a
      // typed array when NaN is written into it.  This bit-pattern
      // inconsistency could confuse differential testing, so always
      // canonicalize NaN values in differential testing.
      if (js::SupportDifferentialTesting()) {
        d = JS::CanonicalizeNaN(d);
      }
    }
    return ConvertNumber<T>(d);
  }
};

inline gc::AllocKind js::FixedLengthTypedArrayObject::allocKindForTenure()
    const {
  // Fixed length typed arrays in the nursery may have a lazily allocated
  // buffer. Make sure there is room for the array's fixed data when moving the
  // array.

  using namespace js::gc;

  if (hasBuffer()) {
    return NativeObject::allocKindForTenure();
  }

  AllocKind allocKind;
  if (hasInlineElements()) {
    allocKind = AllocKindForLazyBuffer(byteLength());
  } else {
    allocKind = GetGCObjectKind(getClass());
  }

  MOZ_ASSERT(GetObjectFinalizeKind(getClass()) == gc::FinalizeKind::Background);
  return GetFinalizedAllocKind(allocKind, gc::FinalizeKind::Background);
}

/* static */ gc::AllocKind
js::FixedLengthTypedArrayObject::AllocKindForLazyBuffer(size_t nbytes) {
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
