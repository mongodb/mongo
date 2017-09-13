/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_TypedArrayCommon_h
#define vm_TypedArrayCommon_h

/* Utilities and common inline code for TypedArray */

#include "mozilla/Assertions.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/PodOperations.h"

#include "jsarray.h"
#include "jscntxt.h"
#include "jsnum.h"

#include "jit/AtomicOperations.h"

#include "js/Conversions.h"
#include "js/Value.h"

#include "vm/TypedArrayObject.h"

namespace js {

// ValueIsLength happens not to be according to ES6, which mandates
// the use of ToLength, which in turn includes ToNumber, ToInteger,
// and clamping.  ValueIsLength is used in the current TypedArray code
// but will disappear when that code is made spec-compliant.

inline bool
ValueIsLength(const Value& v, uint32_t* len)
{
    if (v.isInt32()) {
        int32_t i = v.toInt32();
        if (i < 0)
            return false;
        *len = i;
        return true;
    }

    if (v.isDouble()) {
        double d = v.toDouble();
        if (mozilla::IsNaN(d))
            return false;

        uint32_t length = uint32_t(d);
        if (d != double(length))
            return false;

        *len = length;
        return true;
    }

    return false;
}

template<typename NativeType> struct TypeIDOfType;
template<> struct TypeIDOfType<int8_t> { static const Scalar::Type id = Scalar::Int8; };
template<> struct TypeIDOfType<uint8_t> { static const Scalar::Type id = Scalar::Uint8; };
template<> struct TypeIDOfType<int16_t> { static const Scalar::Type id = Scalar::Int16; };
template<> struct TypeIDOfType<uint16_t> { static const Scalar::Type id = Scalar::Uint16; };
template<> struct TypeIDOfType<int32_t> { static const Scalar::Type id = Scalar::Int32; };
template<> struct TypeIDOfType<uint32_t> { static const Scalar::Type id = Scalar::Uint32; };
template<> struct TypeIDOfType<float> { static const Scalar::Type id = Scalar::Float32; };
template<> struct TypeIDOfType<double> { static const Scalar::Type id = Scalar::Float64; };
template<> struct TypeIDOfType<uint8_clamped> { static const Scalar::Type id = Scalar::Uint8Clamped; };

inline bool
IsAnyTypedArray(JSObject* obj)
{
    return obj->is<TypedArrayObject>();
}

inline uint32_t
AnyTypedArrayLength(JSObject* obj)
{
    return obj->as<TypedArrayObject>().length();
}

inline Scalar::Type
AnyTypedArrayType(JSObject* obj)
{
    return obj->as<TypedArrayObject>().type();
}

inline Shape*
AnyTypedArrayShape(JSObject* obj)
{
    return obj->as<TypedArrayObject>().lastProperty();
}

inline SharedMem<void*>
AnyTypedArrayViewData(const JSObject* obj)
{
    return obj->as<TypedArrayObject>().viewDataEither();
}

inline uint32_t
AnyTypedArrayBytesPerElement(const JSObject* obj)
{
    return obj->as<TypedArrayObject>().bytesPerElement();
}

inline uint32_t
AnyTypedArrayByteLength(const JSObject* obj)
{
    return obj->as<TypedArrayObject>().byteLength();
}

inline bool
AnyTypedArrayIsDetached(const JSObject* obj)
{
    return obj->as<TypedArrayObject>().isNeutered();
}

inline bool
IsAnyTypedArrayClass(const Class* clasp)
{
    return IsTypedArrayClass(clasp);
}

class SharedOps
{
  public:
    template<typename T>
    static T load(SharedMem<T*> addr) {
        return js::jit::AtomicOperations::loadSafeWhenRacy(addr);
    }

    template<typename T>
    static void store(SharedMem<T*> addr, T value) {
        js::jit::AtomicOperations::storeSafeWhenRacy(addr, value);
    }

    template<typename T>
    static void memcpy(SharedMem<T*> dest, SharedMem<T*> src, size_t size) {
        js::jit::AtomicOperations::memcpySafeWhenRacy(dest, src, size);
    }

    template<typename T>
    static void memmove(SharedMem<T*> dest, SharedMem<T*> src, size_t size) {
        js::jit::AtomicOperations::memmoveSafeWhenRacy(dest, src, size);
    }

    static SharedMem<void*> extract(TypedArrayObject* obj) {
        return obj->viewDataEither();
    }
};

class UnsharedOps
{
  public:
    template<typename T>
    static T load(SharedMem<T*> addr) {
        return *addr.unwrapUnshared();
    }

    template<typename T>
    static void store(SharedMem<T*> addr, T value) {
        *addr.unwrapUnshared() = value;
    }

    template<typename T>
    static void memcpy(SharedMem<T*> dest, SharedMem<T*> src, size_t size) {
        ::memcpy(dest.unwrapUnshared(), src.unwrapUnshared(), size);
    }

    template<typename T>
    static void memmove(SharedMem<T*> dest, SharedMem<T*> src, size_t size) {
        ::memmove(dest.unwrapUnshared(), src.unwrapUnshared(), size);
    }

    static SharedMem<void*> extract(TypedArrayObject* obj) {
        return SharedMem<void*>::unshared(obj->viewDataUnshared());
    }
};

template<class SpecificArray, typename Ops>
class ElementSpecific
{
    typedef typename SpecificArray::ElementType T;
    typedef typename SpecificArray::SomeTypedArray SomeTypedArray;

  public:
    /*
     * Copy |source|'s elements into |target|, starting at |target[offset]|.
     * Act as if the assignments occurred from a fresh copy of |source|, in
     * case the two memory ranges overlap.
     */
    static bool
    setFromAnyTypedArray(JSContext* cx,
                         Handle<SomeTypedArray*> target, HandleObject source,
                         uint32_t offset)
    {
        MOZ_ASSERT(SpecificArray::ArrayTypeID() == target->type(),
                   "calling wrong setFromAnyTypedArray specialization");

        MOZ_ASSERT(offset <= target->length());
        MOZ_ASSERT(AnyTypedArrayLength(source) <= target->length() - offset);

        if (source->is<SomeTypedArray>()) {
            Rooted<SomeTypedArray*> src(cx, source.as<SomeTypedArray>());
            if (SomeTypedArray::sameBuffer(target, src))
                return setFromOverlappingTypedArray(cx, target, src, offset);
        }

        SharedMem<T*> dest = AnyTypedArrayViewData(target).template cast<T*>() + offset;
        uint32_t count = AnyTypedArrayLength(source);

        if (AnyTypedArrayType(source) == target->type()) {
            Ops::memcpy(dest.template cast<void*>(), AnyTypedArrayViewData(source), count*sizeof(T));
            return true;
        }

        // Inhibit unaligned accesses on ARM (bug 1097253, a compiler bug).
#ifdef __arm__
#  define JS_VOLATILE_ARM volatile
#else
#  define JS_VOLATILE_ARM
#endif

        SharedMem<void*> data = Ops::extract(source.as<TypedArrayObject>());
        switch (AnyTypedArrayType(source)) {
          case Scalar::Int8: {
            SharedMem<JS_VOLATILE_ARM int8_t*> src = data.cast<JS_VOLATILE_ARM int8_t*>();
            for (uint32_t i = 0; i < count; ++i)
                Ops::store(dest++, T(Ops::load(src++)));
            break;
          }
          case Scalar::Uint8:
          case Scalar::Uint8Clamped: {
            SharedMem<JS_VOLATILE_ARM uint8_t*> src = data.cast<JS_VOLATILE_ARM uint8_t*>();
            for (uint32_t i = 0; i < count; ++i)
                Ops::store(dest++, T(Ops::load(src++)));
            break;
          }
          case Scalar::Int16: {
            SharedMem<JS_VOLATILE_ARM int16_t*> src = data.cast<JS_VOLATILE_ARM int16_t*>();
            for (uint32_t i = 0; i < count; ++i)
                Ops::store(dest++, T(Ops::load(src++)));
            break;
          }
          case Scalar::Uint16: {
            SharedMem<JS_VOLATILE_ARM uint16_t*> src = data.cast<JS_VOLATILE_ARM uint16_t*>();
            for (uint32_t i = 0; i < count; ++i)
                Ops::store(dest++, T(Ops::load(src++)));
            break;
          }
          case Scalar::Int32: {
            SharedMem<JS_VOLATILE_ARM int32_t*> src = data.cast<JS_VOLATILE_ARM int32_t*>();
            for (uint32_t i = 0; i < count; ++i)
                Ops::store(dest++, T(Ops::load(src++)));
            break;
          }
          case Scalar::Uint32: {
            SharedMem<JS_VOLATILE_ARM uint32_t*> src = data.cast<JS_VOLATILE_ARM uint32_t*>();
            for (uint32_t i = 0; i < count; ++i)
                Ops::store(dest++, T(Ops::load(src++)));
            break;
          }
          case Scalar::Float32: {
            SharedMem<JS_VOLATILE_ARM float*> src = data.cast<JS_VOLATILE_ARM float*>();
            for (uint32_t i = 0; i < count; ++i)
                Ops::store(dest++, T(Ops::load(src++)));
            break;
          }
          case Scalar::Float64: {
            SharedMem<JS_VOLATILE_ARM double*> src = data.cast<JS_VOLATILE_ARM double*>();
            for (uint32_t i = 0; i < count; ++i)
                Ops::store(dest++, T(Ops::load(src++)));
            break;
          }
          default:
            MOZ_CRASH("setFromAnyTypedArray with a typed array with bogus type");
        }

#undef JS_VOLATILE_ARM

        return true;
    }

    /*
     * Copy |source[0]| to |source[len]| (exclusive) elements into the typed
     * array |target|, starting at index |offset|.  |source| must not be a
     * typed array.
     */
    static bool
    setFromNonTypedArray(JSContext* cx, Handle<SomeTypedArray*> target, HandleObject source,
                         uint32_t len, uint32_t offset = 0)
    {
        MOZ_ASSERT(target->type() == SpecificArray::ArrayTypeID(),
                   "target type and NativeType must match");
        MOZ_ASSERT(!IsAnyTypedArray(source),
                   "use setFromAnyTypedArray instead of this method");

        uint32_t i = 0;
        if (source->isNative()) {
            // Attempt fast-path infallible conversion of dense elements up to
            // the first potentially side-effectful lookup or conversion.
            uint32_t bound = Min(source->as<NativeObject>().getDenseInitializedLength(), len);

            SharedMem<T*> dest = AnyTypedArrayViewData(target).template cast<T*>() + offset;

            MOZ_ASSERT(!canConvertInfallibly(MagicValue(JS_ELEMENTS_HOLE)),
                       "the following loop must abort on holes");

            const Value* srcValues = source->as<NativeObject>().getDenseElements();
            for (; i < bound; i++) {
                if (!canConvertInfallibly(srcValues[i]))
                    break;
                Ops::store(dest + i, infallibleValueToNative(srcValues[i]));
            }
            if (i == len)
                return true;
        }

        // Convert and copy any remaining elements generically.
        RootedValue v(cx);
        for (; i < len; i++) {
            if (!GetElement(cx, source, source, i, &v))
                return false;

            T n;
            if (!valueToNative(cx, v, &n))
                return false;

            len = Min(len, target->length());
            if (i >= len)
                break;

            // Compute every iteration in case getElement/valueToNative is wacky.
            Ops::store(AnyTypedArrayViewData(target).template cast<T*>() + offset + i, n);
        }

        return true;
    }

  private:
    static bool
    setFromOverlappingTypedArray(JSContext* cx,
                                 Handle<SomeTypedArray*> target,
                                 Handle<SomeTypedArray*> source,
                                 uint32_t offset)
    {
        MOZ_ASSERT(SpecificArray::ArrayTypeID() == target->type(),
                   "calling wrong setFromTypedArray specialization");
        MOZ_ASSERT(SomeTypedArray::sameBuffer(target, source),
                   "provided arrays don't actually overlap, so it's "
                   "undesirable to use this method");

        MOZ_ASSERT(offset <= target->length());
        MOZ_ASSERT(source->length() <= target->length() - offset);

        SharedMem<T*> dest = AnyTypedArrayViewData(target).template cast<T*>() + offset;
        uint32_t len = source->length();

        if (source->type() == target->type()) {
            Ops::memmove(dest, AnyTypedArrayViewData(source).template cast<T*>(), len*sizeof(T));
            return true;
        }

        // Copy |source| in case it overlaps the target elements being set.
        size_t sourceByteLen = len * source->bytesPerElement();
        void* data = target->zone()->template pod_malloc<uint8_t>(sourceByteLen);
        if (!data)
            return false;
        Ops::memcpy(SharedMem<void*>::unshared(data),
                    AnyTypedArrayViewData(source),
                    sourceByteLen);

        switch (source->type()) {
          case Scalar::Int8: {
            int8_t* src = static_cast<int8_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                Ops::store(dest++, T(*src++));
            break;
          }
          case Scalar::Uint8:
          case Scalar::Uint8Clamped: {
            uint8_t* src = static_cast<uint8_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                Ops::store(dest++, T(*src++));
            break;
          }
          case Scalar::Int16: {
            int16_t* src = static_cast<int16_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                Ops::store(dest++, T(*src++));
            break;
          }
          case Scalar::Uint16: {
            uint16_t* src = static_cast<uint16_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                Ops::store(dest++, T(*src++));
            break;
          }
          case Scalar::Int32: {
            int32_t* src = static_cast<int32_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                Ops::store(dest++, T(*src++));
            break;
          }
          case Scalar::Uint32: {
            uint32_t* src = static_cast<uint32_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                Ops::store(dest++, T(*src++));
            break;
          }
          case Scalar::Float32: {
            float* src = static_cast<float*>(data);
            for (uint32_t i = 0; i < len; ++i)
                Ops::store(dest++, T(*src++));
            break;
          }
          case Scalar::Float64: {
            double* src = static_cast<double*>(data);
            for (uint32_t i = 0; i < len; ++i)
                Ops::store(dest++, T(*src++));
            break;
          }
          default:
            MOZ_CRASH("setFromOverlappingTypedArray with a typed array with bogus type");
        }

        js_free(data);
        return true;
    }

    static bool
    canConvertInfallibly(const Value& v)
    {
        return v.isNumber() || v.isBoolean() || v.isNull() || v.isUndefined();
    }

    static T
    infallibleValueToNative(const Value& v)
    {
        if (v.isInt32())
            return T(v.toInt32());
        if (v.isDouble())
            return doubleToNative(v.toDouble());
        if (v.isBoolean())
            return T(v.toBoolean());
        if (v.isNull())
            return T(0);

        MOZ_ASSERT(v.isUndefined());
        return TypeIsFloatingPoint<T>() ? T(JS::GenericNaN()) : T(0);
    }

    static bool
    valueToNative(JSContext* cx, const Value& v, T* result)
    {
        MOZ_ASSERT(!v.isMagic());

        if (MOZ_LIKELY(canConvertInfallibly(v))) {
            *result = infallibleValueToNative(v);
            return true;
        }

        double d;
        MOZ_ASSERT(v.isString() || v.isObject() || v.isSymbol());
        if (!(v.isString() ? StringToNumber(cx, v.toString(), &d) : ToNumber(cx, v, &d)))
            return false;

        *result = doubleToNative(d);
        return true;
    }

    static T
    doubleToNative(double d)
    {
        if (TypeIsFloatingPoint<T>()) {
#ifdef JS_MORE_DETERMINISTIC
            // The JS spec doesn't distinguish among different NaN values, and
            // it deliberately doesn't specify the bit pattern written to a
            // typed array when NaN is written into it.  This bit-pattern
            // inconsistency could confuse deterministic testing, so always
            // canonicalize NaN values in more-deterministic builds.
            d = JS::CanonicalizeNaN(d);
#endif
            return T(d);
        }
        if (MOZ_UNLIKELY(mozilla::IsNaN(d)))
            return T(0);
        if (SpecificArray::ArrayTypeID() == Scalar::Uint8Clamped)
            return T(d);
        if (TypeIsUnsigned<T>())
            return T(JS::ToUint32(d));
        return T(JS::ToInt32(d));
    }
};

template<typename SomeTypedArray>
class TypedArrayMethods
{
    static_assert(mozilla::IsSame<SomeTypedArray, TypedArrayObject>::value,
                  "methods must be shared/unshared-specific, not "
                  "element-type-specific");

    typedef typename SomeTypedArray::BufferType BufferType;

    typedef typename SomeTypedArray::template OfType<int8_t>::Type Int8ArrayType;
    typedef typename SomeTypedArray::template OfType<uint8_t>::Type Uint8ArrayType;
    typedef typename SomeTypedArray::template OfType<int16_t>::Type Int16ArrayType;
    typedef typename SomeTypedArray::template OfType<uint16_t>::Type Uint16ArrayType;
    typedef typename SomeTypedArray::template OfType<int32_t>::Type Int32ArrayType;
    typedef typename SomeTypedArray::template OfType<uint32_t>::Type Uint32ArrayType;
    typedef typename SomeTypedArray::template OfType<float>::Type Float32ArrayType;
    typedef typename SomeTypedArray::template OfType<double>::Type Float64ArrayType;
    typedef typename SomeTypedArray::template OfType<uint8_clamped>::Type Uint8ClampedArrayType;

  public:
    // subarray(start[, end])
    // %TypedArray%.prototype.subarray is a self-hosted method, so this code is
    // only used for shared typed arrays.  We should self-host both methods
    // eventually (but note TypedArraySubarray will require changes to be used
    // with shared typed arrays), but we need to rejigger the shared typed
    // array prototype chain before we can do that.
    static bool
    subarray(JSContext* cx, const CallArgs& args)
    {
        MOZ_ASSERT(SomeTypedArray::is(args.thisv()));

        Rooted<SomeTypedArray*> tarray(cx, &args.thisv().toObject().as<SomeTypedArray>());

        // These are the default values.
        uint32_t initialLength = tarray->length();
        uint32_t begin = 0, end = initialLength;

        if (args.length() > 0) {
            if (!ToClampedIndex(cx, args[0], initialLength, &begin))
                return false;

            if (args.length() > 1) {
                if (!ToClampedIndex(cx, args[1], initialLength, &end))
                    return false;
            }
        }

        if (begin > end)
            begin = end;

        if (begin > tarray->length() || end > tarray->length() || begin > end) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
            return false;
        }

        if (!SomeTypedArray::ensureHasBuffer(cx, tarray))
            return false;

        Rooted<BufferType*> bufobj(cx, tarray->buffer());
        MOZ_ASSERT(bufobj);

        uint32_t length = end - begin;

        size_t elementSize = tarray->bytesPerElement();
        MOZ_ASSERT(begin < UINT32_MAX / elementSize);

        uint32_t arrayByteOffset = tarray->byteOffset();
        MOZ_ASSERT(UINT32_MAX - begin * elementSize >= arrayByteOffset);

        uint32_t byteOffset = arrayByteOffset + begin * elementSize;

        JSObject* nobj = nullptr;
        switch (tarray->type()) {
          case Scalar::Int8:
            nobj = Int8ArrayType::makeInstance(cx, bufobj, byteOffset, length);
            break;
          case Scalar::Uint8:
            nobj = Uint8ArrayType::makeInstance(cx, bufobj, byteOffset, length);
            break;
          case Scalar::Int16:
            nobj = Int16ArrayType::makeInstance(cx, bufobj, byteOffset, length);
            break;
          case Scalar::Uint16:
            nobj = Uint16ArrayType::makeInstance(cx, bufobj, byteOffset, length);
            break;
          case Scalar::Int32:
            nobj = Int32ArrayType::makeInstance(cx, bufobj, byteOffset, length);
            break;
          case Scalar::Uint32:
            nobj = Uint32ArrayType::makeInstance(cx, bufobj, byteOffset, length);
            break;
          case Scalar::Float32:
            nobj = Float32ArrayType::makeInstance(cx, bufobj, byteOffset, length);
            break;
          case Scalar::Float64:
            nobj = Float64ArrayType::makeInstance(cx, bufobj, byteOffset, length);
            break;
          case Scalar::Uint8Clamped:
            nobj = Uint8ClampedArrayType::makeInstance(cx, bufobj, byteOffset, length);
            break;
          default:
            MOZ_CRASH("nonsense target element type");
            break;
        }
        if (!nobj)
            return false;

        args.rval().setObject(*nobj);
        return true;
    }

    /* copyWithin(target, start[, end]) */
    // ES6 draft rev 26, 22.2.3.5
    // %TypedArray%.prototype.copyWithin is a self-hosted method, so this code
    // is only used for shared typed arrays.  We should self-host both methods
    // eventually (but note TypedArrayCopyWithin will require changes to be
    // usable for shared typed arrays), but we need to rejigger the shared
    // typed array prototype chain before we can do that.
    static bool
    copyWithin(JSContext* cx, const CallArgs& args)
    {
        MOZ_ASSERT(SomeTypedArray::is(args.thisv()));

        // Steps 1-2.
        Rooted<SomeTypedArray*> obj(cx, &args.thisv().toObject().as<SomeTypedArray>());

        // Steps 3-4.
        uint32_t len = obj->length();

        // Steps 6-8.
        uint32_t to;
        if (!ToClampedIndex(cx, args.get(0), len, &to))
            return false;

        // Steps 9-11.
        uint32_t from;
        if (!ToClampedIndex(cx, args.get(1), len, &from))
            return false;

        // Steps 12-14.
        uint32_t final;
        if (args.get(2).isUndefined()) {
            final = len;
        } else {
            if (!ToClampedIndex(cx, args.get(2), len, &final))
                return false;
        }

        // Steps 15-18.

        // If |final - from < 0|, then |count| will be less than 0, so step 18
        // never loops.  Exit early so |count| can use a non-negative type.
        // Also exit early if elements are being moved to their pre-existing
        // location.
        if (final < from || to == from) {
            args.rval().setObject(*obj);
            return true;
        }

        uint32_t count = Min(final - from, len - to);
        uint32_t lengthDuringMove = obj->length(); // beware ToClampedIndex

        // Technically |from + count| and |to + count| can't overflow, because
        // buffer contents are limited to INT32_MAX length.  But eventually
        // we're going to lift this restriction, and the extra checking cost is
        // negligible, so just handle it anyway.
        if (from > lengthDuringMove ||
            to > lengthDuringMove ||
            count > lengthDuringMove - from ||
            count > lengthDuringMove - to)
        {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return false;
        }

        const size_t ElementSize = obj->bytesPerElement();

        MOZ_ASSERT(to <= UINT32_MAX / ElementSize);
        uint32_t byteDest = to * ElementSize;

        MOZ_ASSERT(from <= UINT32_MAX / ElementSize);
        uint32_t byteSrc = from * ElementSize;

        MOZ_ASSERT(count <= UINT32_MAX / ElementSize);
        uint32_t byteSize = count * ElementSize;


#ifdef DEBUG
        uint32_t viewByteLength = obj->byteLength();
        MOZ_ASSERT(byteSize <= viewByteLength);
        MOZ_ASSERT(byteDest <= viewByteLength);
        MOZ_ASSERT(byteSrc <= viewByteLength);
        MOZ_ASSERT(byteDest <= viewByteLength - byteSize);
        MOZ_ASSERT(byteSrc <= viewByteLength - byteSize);
#endif

        SharedMem<uint8_t*> data = AnyTypedArrayViewData(obj).template cast<uint8_t*>();
        SharedOps::memmove(data + byteDest, data + byteSrc, byteSize);

        // Step 19.
        args.rval().set(args.thisv());
        return true;
    }

    /* set(array[, offset]) */
    static bool
    set(JSContext* cx, const CallArgs& args)
    {
        MOZ_ASSERT(SomeTypedArray::is(args.thisv()));

        Rooted<SomeTypedArray*> target(cx, &args.thisv().toObject().as<SomeTypedArray>());

        // The first argument must be either a typed array or arraylike.
        if (args.length() == 0 || !args[0].isObject()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return false;
        }

        int32_t offset = 0;
        if (args.length() > 1) {
            if (!ToInt32(cx, args[1], &offset))
                return false;

            if (offset < 0 || uint32_t(offset) > target->length()) {
                // the given offset is bogus
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
                return false;
            }
        }

        RootedObject arg0(cx, &args[0].toObject());
        if (IsAnyTypedArray(arg0)) {
            if (AnyTypedArrayLength(arg0) > target->length() - offset) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
                return false;
            }

            if (!setFromAnyTypedArray(cx, target, arg0, offset))
                return false;
        } else {
            uint32_t len;
            if (!GetLengthProperty(cx, arg0, &len))
                return false;

            if (uint32_t(offset) > target->length() || len > target->length() - offset) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
                return false;
            }

            if (!setFromNonTypedArray(cx, target, arg0, len, offset))
                return false;
        }

        args.rval().setUndefined();
        return true;
    }

    static bool
    setFromArrayLike(JSContext* cx, Handle<SomeTypedArray*> target, HandleObject source, uint32_t len,
                     uint32_t offset = 0)
    {
        MOZ_ASSERT(offset <= target->length());
        MOZ_ASSERT(len <= target->length() - offset);

        if (IsAnyTypedArray(source))
            return setFromAnyTypedArray(cx, target, source, offset);

        return setFromNonTypedArray(cx, target, source, len, offset);
    }

  private:
     static bool
     setFromAnyTypedArray(JSContext* cx, Handle<SomeTypedArray*> target, HandleObject source,
                          uint32_t offset)
     {
         MOZ_ASSERT(IsAnyTypedArray(source), "use setFromNonTypedArray");

         bool isShared = target->isSharedMemory() || source->as<TypedArrayObject>().isSharedMemory();

         switch (target->type()) {
           case Scalar::Int8:
             if (isShared)
                 return ElementSpecific<Int8ArrayType, SharedOps>::setFromAnyTypedArray(cx, target, source, offset);
             return ElementSpecific<Int8ArrayType, UnsharedOps>::setFromAnyTypedArray(cx, target, source, offset);
           case Scalar::Uint8:
             if (isShared)
                 return ElementSpecific<Uint8ArrayType, SharedOps>::setFromAnyTypedArray(cx, target, source, offset);
             return ElementSpecific<Uint8ArrayType, UnsharedOps>::setFromAnyTypedArray(cx, target, source, offset);
           case Scalar::Int16:
             if (isShared)
                 return ElementSpecific<Int16ArrayType, SharedOps>::setFromAnyTypedArray(cx, target, source, offset);
             return ElementSpecific<Int16ArrayType, UnsharedOps>::setFromAnyTypedArray(cx, target, source, offset);
           case Scalar::Uint16:
             if (isShared)
                 return ElementSpecific<Uint16ArrayType, SharedOps>::setFromAnyTypedArray(cx, target, source, offset);
             return ElementSpecific<Uint16ArrayType, UnsharedOps>::setFromAnyTypedArray(cx, target, source, offset);
           case Scalar::Int32:
             if (isShared)
                 return ElementSpecific<Int32ArrayType, SharedOps>::setFromAnyTypedArray(cx, target, source, offset);
             return ElementSpecific<Int32ArrayType, UnsharedOps>::setFromAnyTypedArray(cx, target, source, offset);
           case Scalar::Uint32:
             if (isShared)
                 return ElementSpecific<Uint32ArrayType, SharedOps>::setFromAnyTypedArray(cx, target, source, offset);
             return ElementSpecific<Uint32ArrayType, UnsharedOps>::setFromAnyTypedArray(cx, target, source, offset);
           case Scalar::Float32:
             if (isShared)
                 return ElementSpecific<Float32ArrayType, SharedOps>::setFromAnyTypedArray(cx, target, source, offset);
             return ElementSpecific<Float32ArrayType, UnsharedOps>::setFromAnyTypedArray(cx, target, source, offset);
           case Scalar::Float64:
             if (isShared)
                 return ElementSpecific<Float64ArrayType, SharedOps>::setFromAnyTypedArray(cx, target, source, offset);
             return ElementSpecific<Float64ArrayType, UnsharedOps>::setFromAnyTypedArray(cx, target, source, offset);
           case Scalar::Uint8Clamped:
             if (isShared)
                 return ElementSpecific<Uint8ClampedArrayType, SharedOps>::setFromAnyTypedArray(cx, target, source, offset);
             return ElementSpecific<Uint8ClampedArrayType, UnsharedOps>::setFromAnyTypedArray(cx, target, source, offset);
           case Scalar::Float32x4:
           case Scalar::Int32x4:
           case Scalar::MaxTypedArrayViewType:
             break;
         }

         MOZ_CRASH("nonsense target element type");
     }

    static bool
    setFromNonTypedArray(JSContext* cx, Handle<SomeTypedArray*> target, HandleObject source,
                         uint32_t len, uint32_t offset)
    {
        MOZ_ASSERT(!IsAnyTypedArray(source), "use setFromAnyTypedArray");

        bool isShared = target->isSharedMemory();

        switch (target->type()) {
          case Scalar::Int8:
            if (isShared)
                return ElementSpecific<Int8ArrayType, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
            return ElementSpecific<Int8ArrayType, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Uint8:
            if (isShared)
                return ElementSpecific<Uint8ArrayType, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
            return ElementSpecific<Uint8ArrayType, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Int16:
            if (isShared)
                return ElementSpecific<Int16ArrayType, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
            return ElementSpecific<Int16ArrayType, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Uint16:
            if (isShared)
                return ElementSpecific<Uint16ArrayType, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
            return ElementSpecific<Uint16ArrayType, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Int32:
            if (isShared)
                return ElementSpecific<Int32ArrayType, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
            return ElementSpecific<Int32ArrayType, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Uint32:
            if (isShared)
                return ElementSpecific<Uint32ArrayType, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
            return ElementSpecific<Uint32ArrayType, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Float32:
            if (isShared)
                return ElementSpecific<Float32ArrayType, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
            return ElementSpecific<Float32ArrayType, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Float64:
            if (isShared)
                return ElementSpecific<Float64ArrayType, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
            return ElementSpecific<Float64ArrayType, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Uint8Clamped:
            if (isShared)
                return ElementSpecific<Uint8ClampedArrayType, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
            return ElementSpecific<Uint8ClampedArrayType, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Float32x4:
          case Scalar::Int32x4:
          case Scalar::MaxTypedArrayViewType:
            break;
        }
        MOZ_CRASH("bad target array type");
    }
};

} // namespace js

#endif // vm_TypedArrayCommon_h
