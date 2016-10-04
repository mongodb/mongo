/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsnum_h
#define jsnum_h

#include "mozilla/FloatingPoint.h"
#include "mozilla/Range.h"

#include "NamespaceImports.h"

#include "js/Conversions.h"


// This macro is should be `one' if current compiler supports builtin functions
// like __builtin_sadd_overflow.
#if __GNUC__ >= 5
    // GCC 5 and above supports these functions.
    #define BUILTIN_CHECKED_ARITHMETIC_SUPPORTED(x) 1
#else
    // For CLANG, we use its own function to check for this.
    #ifdef __has_builtin
        #define BUILTIN_CHECKED_ARITHMETIC_SUPPORTED(x) __has_builtin(x)
    #endif
#endif
#ifndef BUILTIN_CHECKED_ARITHMETIC_SUPPORTED
    #define BUILTIN_CHECKED_ARITHMETIC_SUPPORTED(x) 0
#endif

namespace js {

class StringBuffer;

extern bool
InitRuntimeNumberState(JSRuntime* rt);

#if !EXPOSE_INTL_API
extern void
FinishRuntimeNumberState(JSRuntime* rt);
#endif

/* Initialize the Number class, returning its prototype object. */
extern JSObject*
InitNumberClass(JSContext* cx, HandleObject obj);

} /* namespace js */

/*
 * String constants for global function names, used in jsapi.c and jsnum.c.
 */
extern const char js_isNaN_str[];
extern const char js_isFinite_str[];
extern const char js_parseFloat_str[];
extern const char js_parseInt_str[];

class JSAtom;

namespace js {

/*
 * When base == 10, this function implements ToString() as specified by
 * ECMA-262-5 section 9.8.1; but note that it handles integers specially for
 * performance.  See also js::NumberToCString().
 */
template <AllowGC allowGC>
extern JSString*
NumberToString(ExclusiveContext* cx, double d);

extern JSAtom*
NumberToAtom(ExclusiveContext* cx, double d);

template <AllowGC allowGC>
extern JSFlatString*
Int32ToString(ExclusiveContext* cx, int32_t i);

extern JSAtom*
Int32ToAtom(ExclusiveContext* cx, int32_t si);

/*
 * Convert an integer or double (contained in the given value) to a string and
 * append to the given buffer.
 */
extern bool JS_FASTCALL
NumberValueToStringBuffer(JSContext* cx, const Value& v, StringBuffer& sb);

/* Same as js_NumberToString, different signature. */
extern JSFlatString*
NumberToString(JSContext* cx, double d);

extern JSFlatString*
IndexToString(JSContext* cx, uint32_t index);

/*
 * Usually a small amount of static storage is enough, but sometimes we need
 * to dynamically allocate much more.  This struct encapsulates that.
 * Dynamically allocated memory will be freed when the object is destroyed.
 */
struct ToCStringBuf
{
    /*
     * The longest possible result that would need to fit in sbuf is
     * (-0x80000000).toString(2), which has length 33.  Longer cases are
     * possible, but they'll go in dbuf.
     */
    static const size_t sbufSize = 34;
    char sbuf[sbufSize];
    char* dbuf;

    ToCStringBuf();
    ~ToCStringBuf();
};

/*
 * Convert a number to a C string.  When base==10, this function implements
 * ToString() as specified by ECMA-262-5 section 9.8.1.  It handles integral
 * values cheaply.  Return nullptr if we ran out of memory.  See also
 * NumberToCString().
 */
extern char*
NumberToCString(JSContext* cx, ToCStringBuf* cbuf, double d, int base = 10);

/*
 * The largest positive integer such that all positive integers less than it
 * may be precisely represented using the IEEE-754 double-precision format.
 */
const double DOUBLE_INTEGRAL_PRECISION_LIMIT = uint64_t(1) << 53;

/*
 * Parse a decimal number encoded in |chars|.  The decimal number must be
 * sufficiently small that it will not overflow the integrally-precise range of
 * the double type -- that is, the number will be smaller than
 * DOUBLE_INTEGRAL_PRECISION_LIMIT
 */
template <typename CharT>
extern double
ParseDecimalNumber(const mozilla::Range<const CharT> chars);

/*
 * Compute the positive integer of the given base described immediately at the
 * start of the range [start, end) -- no whitespace-skipping, no magical
 * leading-"0" octal or leading-"0x" hex behavior, no "+"/"-" parsing, just
 * reading the digits of the integer.  Return the index one past the end of the
 * digits of the integer in *endp, and return the integer itself in *dp.  If
 * base is 10 or a power of two the returned integer is the closest possible
 * double; otherwise extremely large integers may be slightly inaccurate.
 *
 * If [start, end) does not begin with a number with the specified base,
 * *dp == 0 and *endp == start upon return.
 */
template <typename CharT>
extern bool
GetPrefixInteger(ExclusiveContext* cx, const CharT* start, const CharT* end, int base,
                 const CharT** endp, double* dp);

/*
 * This is like GetPrefixInteger, but only deals with base 10, and doesn't have
 * and |endp| outparam.  It should only be used when the characters are known to
 * only contain digits.
 */
extern bool
GetDecimalInteger(ExclusiveContext* cx, const char16_t* start, const char16_t* end, double* dp);

extern bool
StringToNumber(ExclusiveContext* cx, JSString* str, double* result);

/* ES5 9.3 ToNumber, overwriting *vp with the appropriate number value. */
MOZ_ALWAYS_INLINE bool
ToNumber(JSContext* cx, JS::MutableHandleValue vp)
{
    if (vp.isNumber())
        return true;
    double d;
    extern JS_PUBLIC_API(bool) ToNumberSlow(JSContext* cx, Value v, double* dp);
    if (!ToNumberSlow(cx, vp, &d))
        return false;

    vp.setNumber(d);
    return true;
}

bool
num_parseInt(JSContext* cx, unsigned argc, Value* vp);

}  /* namespace js */

/*
 * Similar to strtod except that it replaces overflows with infinities of the
 * correct sign, and underflows with zeros of the correct sign.  Guaranteed to
 * return the closest double number to the given input in dp.
 *
 * Also allows inputs of the form [+|-]Infinity, which produce an infinity of
 * the appropriate sign.  The case of the "Infinity" string must match exactly.
 * If the string does not contain a number, set *dEnd to begin and return 0.0
 * in *d.
 *
 * Return false if out of memory.
 */
template <typename CharT>
extern bool
js_strtod(js::ExclusiveContext* cx, const CharT* begin, const CharT* end,
          const CharT** dEnd, double* d);

namespace js {

extern bool
num_toString(JSContext* cx, unsigned argc, Value* vp);

extern bool
num_valueOf(JSContext* cx, unsigned argc, Value* vp);

static MOZ_ALWAYS_INLINE bool
ValueFitsInInt32(const Value& v, int32_t* pi)
{
    if (v.isInt32()) {
        *pi = v.toInt32();
        return true;
    }
    return v.isDouble() && mozilla::NumberIsInt32(v.toDouble(), pi);
}

/*
 * Returns true if the given value is definitely an index: that is, the value
 * is a number that's an unsigned 32-bit integer.
 *
 * This method prioritizes common-case speed over accuracy in every case.  It
 * can produce false negatives (but not false positives): some values which are
 * indexes will be reported not to be indexes by this method.  Users must
 * consider this possibility when using this method.
 */
static MOZ_ALWAYS_INLINE bool
IsDefinitelyIndex(const Value& v, uint32_t* indexp)
{
    if (v.isInt32() && v.toInt32() >= 0) {
        *indexp = v.toInt32();
        return true;
    }

    int32_t i;
    if (v.isDouble() && mozilla::NumberIsInt32(v.toDouble(), &i) && i >= 0) {
        *indexp = uint32_t(i);
        return true;
    }

    return false;
}

/* ES5 9.4 ToInteger. */
static inline bool
ToInteger(JSContext* cx, HandleValue v, double* dp)
{
    if (v.isInt32()) {
        *dp = v.toInt32();
        return true;
    }
    if (v.isDouble()) {
        *dp = v.toDouble();
    } else {
        extern JS_PUBLIC_API(bool) ToNumberSlow(JSContext* cx, Value v, double* dp);
        if (!ToNumberSlow(cx, v, dp))
            return false;
    }
    *dp = JS::ToInteger(*dp);
    return true;
}

/* ES6 7.1.15 ToLength, but clamped to the [0,2^32-2] range.  If the
 * return value is false then *overflow will be true iff the value was
 * not clampable to uint32_t range.
 *
 * For JSContext and ExclusiveContext.
 */
template<typename T>
bool ToLengthClamped(T* cx, HandleValue v, uint32_t* out, bool* overflow);

inline bool
SafeAdd(int32_t one, int32_t two, int32_t* res)
{
#if BUILTIN_CHECKED_ARITHMETIC_SUPPORTED(__builtin_sadd_overflow)
    // Using compiler's builtin function.
    return !__builtin_sadd_overflow(one, two, res);
#else
    // Use unsigned for the 32-bit operation since signed overflow gets
    // undefined behavior.
    *res = uint32_t(one) + uint32_t(two);
    int64_t ores = (int64_t)one + (int64_t)two;
    return ores == (int64_t)*res;
#endif
}

inline bool
SafeSub(int32_t one, int32_t two, int32_t* res)
{
#if BUILTIN_CHECKED_ARITHMETIC_SUPPORTED(__builtin_ssub_overflow)
    return !__builtin_ssub_overflow(one, two, res);
#else
    *res = uint32_t(one) - uint32_t(two);
    int64_t ores = (int64_t)one - (int64_t)two;
    return ores == (int64_t)*res;
#endif
}

inline bool
SafeMul(int32_t one, int32_t two, int32_t* res)
{
#if BUILTIN_CHECKED_ARITHMETIC_SUPPORTED(__builtin_smul_overflow)
    return !__builtin_smul_overflow(one, two, res);
#else
    *res = uint32_t(one) * uint32_t(two);
    int64_t ores = (int64_t)one * (int64_t)two;
    return ores == (int64_t)*res;
#endif
}

extern bool
ToNumberSlow(ExclusiveContext* cx, Value v, double* dp);

// Variant of ToNumber which takes an ExclusiveContext instead of a JSContext.
// ToNumber is part of the API and can't use ExclusiveContext directly.
MOZ_ALWAYS_INLINE bool
ToNumber(ExclusiveContext* cx, const Value& v, double* out)
{
    if (v.isNumber()) {
        *out = v.toNumber();
        return true;
    }
    return ToNumberSlow(cx, v, out);
}

void FIX_FPU();

} /* namespace js */

#endif /* jsnum_h */
