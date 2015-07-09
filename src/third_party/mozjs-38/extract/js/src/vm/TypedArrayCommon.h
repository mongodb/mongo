/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_TypedArrayCommon_h
#define vm_TypedArrayCommon_h

/* Utilities and common inline code for TypedArray and SharedTypedArray */

#include "mozilla/Assertions.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/PodOperations.h"

#include "js/Conversions.h"
#include "js/Value.h"

#include "vm/SharedTypedArrayObject.h"
#include "vm/TypedArrayObject.h"

namespace js {

// Definitions below are shared between TypedArrayObject and
// SharedTypedArrayObject.

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

template<typename NativeType> static inline Scalar::Type TypeIDOfType();
template<> inline Scalar::Type TypeIDOfType<int8_t>() { return Scalar::Int8; }
template<> inline Scalar::Type TypeIDOfType<uint8_t>() { return Scalar::Uint8; }
template<> inline Scalar::Type TypeIDOfType<int16_t>() { return Scalar::Int16; }
template<> inline Scalar::Type TypeIDOfType<uint16_t>() { return Scalar::Uint16; }
template<> inline Scalar::Type TypeIDOfType<int32_t>() { return Scalar::Int32; }
template<> inline Scalar::Type TypeIDOfType<uint32_t>() { return Scalar::Uint32; }
template<> inline Scalar::Type TypeIDOfType<float>() { return Scalar::Float32; }
template<> inline Scalar::Type TypeIDOfType<double>() { return Scalar::Float64; }
template<> inline Scalar::Type TypeIDOfType<uint8_clamped>() { return Scalar::Uint8Clamped; }

inline bool
IsAnyTypedArray(JSObject* obj)
{
    return obj->is<TypedArrayObject>() || obj->is<SharedTypedArrayObject>();
}

inline uint32_t
AnyTypedArrayLength(JSObject* obj)
{
    if (obj->is<TypedArrayObject>())
        return obj->as<TypedArrayObject>().length();
    return obj->as<SharedTypedArrayObject>().length();
}

inline Scalar::Type
AnyTypedArrayType(JSObject* obj)
{
    if (obj->is<TypedArrayObject>())
        return obj->as<TypedArrayObject>().type();
    return obj->as<SharedTypedArrayObject>().type();
}

inline Shape*
AnyTypedArrayShape(JSObject* obj)
{
    if (obj->is<TypedArrayObject>())
        return obj->as<TypedArrayObject>().lastProperty();
    return obj->as<SharedTypedArrayObject>().lastProperty();
}

inline const TypedArrayLayout&
AnyTypedArrayLayout(const JSObject* obj)
{
    if (obj->is<TypedArrayObject>())
        return obj->as<TypedArrayObject>().layout();
    return obj->as<SharedTypedArrayObject>().layout();
}

inline void*
AnyTypedArrayViewData(const JSObject* obj)
{
    if (obj->is<TypedArrayObject>())
        return obj->as<TypedArrayObject>().viewData();
    return obj->as<SharedTypedArrayObject>().viewData();
}

inline uint32_t
AnyTypedArrayBytesPerElement(const JSObject* obj)
{
    if (obj->is<TypedArrayObject>())
        return obj->as<TypedArrayObject>().bytesPerElement();
    return obj->as<SharedTypedArrayObject>().bytesPerElement();
}

inline uint32_t
AnyTypedArrayByteLength(const JSObject* obj)
{
    if (obj->is<TypedArrayObject>())
        return obj->as<TypedArrayObject>().byteLength();
    return obj->as<SharedTypedArrayObject>().byteLength();
}

inline bool
IsAnyTypedArrayClass(const Class* clasp)
{
    return IsTypedArrayClass(clasp) || IsSharedTypedArrayClass(clasp);
}

template<class SpecificArray>
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

        T* dest = static_cast<T*>(target->viewData()) + offset;
        uint32_t count = AnyTypedArrayLength(source);

        if (AnyTypedArrayType(source) == target->type()) {
            mozilla::PodCopy(dest, static_cast<T*>(AnyTypedArrayViewData(source)), count);
            return true;
        }

#ifdef __arm__
#  define JS_VOLATILE_ARM volatile // Inhibit unaligned accesses on ARM.
#else
#  define JS_VOLATILE_ARM /* nothing */
#endif

        void* data = AnyTypedArrayViewData(source);
        switch (AnyTypedArrayType(source)) {
          case Scalar::Int8: {
            JS_VOLATILE_ARM
            int8_t* src = static_cast<int8_t*>(data);

            for (uint32_t i = 0; i < count; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Uint8:
          case Scalar::Uint8Clamped: {
            JS_VOLATILE_ARM
            uint8_t* src = static_cast<uint8_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Int16: {
            JS_VOLATILE_ARM
            int16_t* src = static_cast<int16_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Uint16: {
            JS_VOLATILE_ARM
            uint16_t* src = static_cast<uint16_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Int32: {
            JS_VOLATILE_ARM
            int32_t* src = static_cast<int32_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Uint32: {
            JS_VOLATILE_ARM
            uint32_t* src = static_cast<uint32_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Float32: {
            JS_VOLATILE_ARM
            float* src = static_cast<float*>(data);
            for (uint32_t i = 0; i < count; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Float64: {
            JS_VOLATILE_ARM
            double* src = static_cast<double*>(data);
            for (uint32_t i = 0; i < count; ++i)
                *dest++ = T(*src++);
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

            T* dest = static_cast<T*>(target->viewData()) + offset;

            MOZ_ASSERT(!canConvertInfallibly(MagicValue(JS_ELEMENTS_HOLE)),
                       "the following loop must abort on holes");

            const Value* srcValues = source->as<NativeObject>().getDenseElements();
            for (; i < bound; i++) {
                if (!canConvertInfallibly(srcValues[i]))
                    break;
                dest[i] = infallibleValueToNative(srcValues[i]);
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
            void* data = target->viewData();
            static_cast<T*>(data)[offset + i] = n;
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

        T* dest = static_cast<T*>(target->viewData()) + offset;
        uint32_t len = source->length();

        if (source->type() == target->type()) {
            mozilla::PodMove(dest, static_cast<T*>(source->viewData()), len);
            return true;
        }

        // Copy |source| in case it overlaps the target elements being set.
        size_t sourceByteLen = len * source->bytesPerElement();
        void* data = target->zone()->template pod_malloc<uint8_t>(sourceByteLen);
        if (!data)
            return false;
        mozilla::PodCopy(static_cast<uint8_t*>(data),
                         static_cast<uint8_t*>(source->viewData()),
                         sourceByteLen);

        switch (source->type()) {
          case Scalar::Int8: {
            int8_t* src = static_cast<int8_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Uint8:
          case Scalar::Uint8Clamped: {
            uint8_t* src = static_cast<uint8_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Int16: {
            int16_t* src = static_cast<int16_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Uint16: {
            uint16_t* src = static_cast<uint16_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Int32: {
            int32_t* src = static_cast<int32_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Uint32: {
            uint32_t* src = static_cast<uint32_t*>(data);
            for (uint32_t i = 0; i < len; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Float32: {
            float* src = static_cast<float*>(data);
            for (uint32_t i = 0; i < len; ++i)
                *dest++ = T(*src++);
            break;
          }
          case Scalar::Float64: {
            double* src = static_cast<double*>(data);
            for (uint32_t i = 0; i < len; ++i)
                *dest++ = T(*src++);
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
    static_assert(mozilla::IsSame<SomeTypedArray, TypedArrayObject>::value ||
                  mozilla::IsSame<SomeTypedArray, SharedTypedArrayObject>::value,
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
    /* subarray(start[, end]) */
    static bool
    subarray(JSContext* cx, CallArgs args)
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
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
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
    static bool
    copyWithin(JSContext* cx, CallArgs args)
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
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
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

        uint8_t* data = static_cast<uint8_t*>(obj->viewData());
        mozilla::PodMove(&data[byteDest], &data[byteSrc], byteSize);

        // Step 19.
        args.rval().set(args.thisv());
        return true;
    }

    /* set(array[, offset]) */
    static bool
    set(JSContext* cx, CallArgs args)
    {
        MOZ_ASSERT(SomeTypedArray::is(args.thisv()));

        Rooted<SomeTypedArray*> target(cx, &args.thisv().toObject().as<SomeTypedArray>());

        // The first argument must be either a typed array or arraylike.
        if (args.length() == 0 || !args[0].isObject()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return false;
        }

        int32_t offset = 0;
        if (args.length() > 1) {
            if (!ToInt32(cx, args[1], &offset))
                return false;

            if (offset < 0 || uint32_t(offset) > target->length()) {
                // the given offset is bogus
                JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                                     JSMSG_TYPED_ARRAY_BAD_INDEX, "2");
                return false;
            }
        }

        RootedObject arg0(cx, &args[0].toObject());
        if (IsAnyTypedArray(arg0)) {
            if (AnyTypedArrayLength(arg0) > target->length() - offset) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
                return false;
            }

            if (!setFromAnyTypedArray(cx, target, arg0, offset))
                return false;
        } else {
            uint32_t len;
            if (!GetLengthProperty(cx, arg0, &len))
                return false;

            if (uint32_t(offset) > target->length() || len > target->length() - offset) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
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

        switch (target->type()) {
          case Scalar::Int8:
            return ElementSpecific<Int8ArrayType>::setFromAnyTypedArray(cx, target, source, offset);
          case Scalar::Uint8:
            return ElementSpecific<Uint8ArrayType>::setFromAnyTypedArray(cx, target, source, offset);
          case Scalar::Int16:
            return ElementSpecific<Int16ArrayType>::setFromAnyTypedArray(cx, target, source, offset);
          case Scalar::Uint16:
            return ElementSpecific<Uint16ArrayType>::setFromAnyTypedArray(cx, target, source, offset);
          case Scalar::Int32:
            return ElementSpecific<Int32ArrayType>::setFromAnyTypedArray(cx, target, source, offset);
          case Scalar::Uint32:
            return ElementSpecific<Uint32ArrayType>::setFromAnyTypedArray(cx, target, source, offset);
          case Scalar::Float32:
            return ElementSpecific<Float32ArrayType>::setFromAnyTypedArray(cx, target, source, offset);
          case Scalar::Float64:
            return ElementSpecific<Float64ArrayType>::setFromAnyTypedArray(cx, target, source, offset);
          case Scalar::Uint8Clamped:
            return ElementSpecific<Uint8ClampedArrayType>::setFromAnyTypedArray(cx, target, source, offset);
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

        switch (target->type()) {
          case Scalar::Int8:
            return ElementSpecific<Int8ArrayType>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Uint8:
            return ElementSpecific<Uint8ArrayType>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Int16:
            return ElementSpecific<Int16ArrayType>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Uint16:
            return ElementSpecific<Uint16ArrayType>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Int32:
            return ElementSpecific<Int32ArrayType>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Uint32:
            return ElementSpecific<Uint32ArrayType>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Float32:
            return ElementSpecific<Float32ArrayType>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Float64:
            return ElementSpecific<Float64ArrayType>::setFromNonTypedArray(cx, target, source, len, offset);
          case Scalar::Uint8Clamped:
            return ElementSpecific<Uint8ClampedArrayType>::setFromNonTypedArray(cx, target, source, len, offset);
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
