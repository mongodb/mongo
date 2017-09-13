/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ECMAScript conversion operations. */

#ifndef js_Conversions_h
#define js_Conversions_h

#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/TypeTraits.h"

#include <math.h>

#include "jspubtd.h"

#include "js/RootingAPI.h"
#include "js/Value.h"

struct JSContext;

namespace js {

/* DO NOT CALL THIS. Use JS::ToBoolean. */
extern JS_PUBLIC_API(bool)
ToBooleanSlow(JS::HandleValue v);

/* DO NOT CALL THIS.  Use JS::ToNumber. */
extern JS_PUBLIC_API(bool)
ToNumberSlow(JSContext* cx, JS::Value v, double* dp);

/* DO NOT CALL THIS. Use JS::ToInt8. */
extern JS_PUBLIC_API(bool)
ToInt8Slow(JSContext *cx, JS::HandleValue v, int8_t *out);

/* DO NOT CALL THIS. Use JS::ToInt16. */
extern JS_PUBLIC_API(bool)
ToInt16Slow(JSContext *cx, JS::HandleValue v, int16_t *out);

/* DO NOT CALL THIS. Use JS::ToInt32. */
extern JS_PUBLIC_API(bool)
ToInt32Slow(JSContext* cx, JS::HandleValue v, int32_t* out);

/* DO NOT CALL THIS. Use JS::ToUint32. */
extern JS_PUBLIC_API(bool)
ToUint32Slow(JSContext* cx, JS::HandleValue v, uint32_t* out);

/* DO NOT CALL THIS. Use JS::ToUint16. */
extern JS_PUBLIC_API(bool)
ToUint16Slow(JSContext* cx, JS::HandleValue v, uint16_t* out);

/* DO NOT CALL THIS. Use JS::ToInt64. */
extern JS_PUBLIC_API(bool)
ToInt64Slow(JSContext* cx, JS::HandleValue v, int64_t* out);

/* DO NOT CALL THIS. Use JS::ToUint64. */
extern JS_PUBLIC_API(bool)
ToUint64Slow(JSContext* cx, JS::HandleValue v, uint64_t* out);

/* DO NOT CALL THIS. Use JS::ToString. */
extern JS_PUBLIC_API(JSString*)
ToStringSlow(JSContext* cx, JS::HandleValue v);

/* DO NOT CALL THIS. Use JS::ToObject. */
extern JS_PUBLIC_API(JSObject*)
ToObjectSlow(JSContext* cx, JS::HandleValue v, bool reportScanStack);

} // namespace js

namespace JS {

namespace detail {

#ifdef JS_DEBUG
/**
 * Assert that we're not doing GC on cx, that we're in a request as
 * needed, and that the compartments for cx and v are correct.
 * Also check that GC would be safe at this point.
 */
extern JS_PUBLIC_API(void)
AssertArgumentsAreSane(JSContext* cx, HandleValue v);
#else
inline void AssertArgumentsAreSane(JSContext* cx, HandleValue v)
{}
#endif /* JS_DEBUG */

} // namespace detail

/**
 * ES6 draft 20141224, 7.1.1, second algorithm.
 *
 * Most users shouldn't call this -- use JS::ToBoolean, ToNumber, or ToString
 * instead.  This will typically only be called from custom convert hooks that
 * wish to fall back to the ES6 default conversion behavior shared by most
 * objects in JS, codified as OrdinaryToPrimitive.
 */
extern JS_PUBLIC_API(bool)
OrdinaryToPrimitive(JSContext* cx, HandleObject obj, JSType type, MutableHandleValue vp);

/* ES6 draft 20141224, 7.1.2. */
MOZ_ALWAYS_INLINE bool
ToBoolean(HandleValue v)
{
    if (v.isBoolean())
        return v.toBoolean();
    if (v.isInt32())
        return v.toInt32() != 0;
    if (v.isNullOrUndefined())
        return false;
    if (v.isDouble()) {
        double d = v.toDouble();
        return !mozilla::IsNaN(d) && d != 0;
    }
    if (v.isSymbol())
        return true;

    /* The slow path handles strings and objects. */
    return js::ToBooleanSlow(v);
}

/* ES6 draft 20141224, 7.1.3. */
MOZ_ALWAYS_INLINE bool
ToNumber(JSContext* cx, HandleValue v, double* out)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isNumber()) {
        *out = v.toNumber();
        return true;
    }
    return js::ToNumberSlow(cx, v, out);
}

/* ES6 draft 20141224, ToInteger (specialized for doubles). */
inline double
ToInteger(double d)
{
    if (d == 0)
        return d;

    if (!mozilla::IsFinite(d)) {
        if (mozilla::IsNaN(d))
            return 0;
        return d;
    }

    return d < 0 ? ceil(d) : floor(d);
}

/* ES6 draft 20141224, 7.1.5. */
MOZ_ALWAYS_INLINE bool
ToInt32(JSContext* cx, JS::HandleValue v, int32_t* out)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isInt32()) {
        *out = v.toInt32();
        return true;
    }
    return js::ToInt32Slow(cx, v, out);
}

/* ES6 draft 20141224, 7.1.6. */
MOZ_ALWAYS_INLINE bool
ToUint32(JSContext* cx, HandleValue v, uint32_t* out)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isInt32()) {
        *out = uint32_t(v.toInt32());
        return true;
    }
    return js::ToUint32Slow(cx, v, out);
}

/* ES6 draft 20141224, 7.1.7. */
MOZ_ALWAYS_INLINE bool
ToInt16(JSContext *cx, JS::HandleValue v, int16_t *out)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isInt32()) {
        *out = int16_t(v.toInt32());
        return true;
    }
    return js::ToInt16Slow(cx, v, out);
}

/* ES6 draft 20141224, 7.1.8. */
MOZ_ALWAYS_INLINE bool
ToUint16(JSContext* cx, HandleValue v, uint16_t* out)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isInt32()) {
        *out = uint16_t(v.toInt32());
        return true;
    }
    return js::ToUint16Slow(cx, v, out);
}

/* ES6 draft 20141224, 7.1.9 */
MOZ_ALWAYS_INLINE bool
ToInt8(JSContext *cx, JS::HandleValue v, int8_t *out)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isInt32()) {
        *out = int8_t(v.toInt32());
        return true;
    }
    return js::ToInt8Slow(cx, v, out);
}

/*
 * Non-standard, with behavior similar to that of ToInt32, except in its
 * producing an int64_t.
 */
MOZ_ALWAYS_INLINE bool
ToInt64(JSContext* cx, HandleValue v, int64_t* out)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isInt32()) {
        *out = int64_t(v.toInt32());
        return true;
    }
    return js::ToInt64Slow(cx, v, out);
}

/*
 * Non-standard, with behavior similar to that of ToUint32, except in its
 * producing a uint64_t.
 */
MOZ_ALWAYS_INLINE bool
ToUint64(JSContext* cx, HandleValue v, uint64_t* out)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isInt32()) {
        *out = uint64_t(v.toInt32());
        return true;
    }
    return js::ToUint64Slow(cx, v, out);
}

/* ES6 draft 20141224, 7.1.12. */
MOZ_ALWAYS_INLINE JSString*
ToString(JSContext* cx, HandleValue v)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isString())
        return v.toString();
    return js::ToStringSlow(cx, v);
}

/* ES6 draft 20141224, 7.1.13. */
inline JSObject*
ToObject(JSContext* cx, HandleValue v)
{
    detail::AssertArgumentsAreSane(cx, v);

    if (v.isObject())
        return &v.toObject();
    return js::ToObjectSlow(cx, v, false);
}

namespace detail {

/*
 * Convert a double value to ResultType (an unsigned integral type) using
 * ECMAScript-style semantics (that is, in like manner to how ECMAScript's
 * ToInt32 converts to int32_t).
 *
 *   If d is infinite or NaN, return 0.
 *   Otherwise compute d2 = sign(d) * floor(abs(d)), and return the ResultType
 *   value congruent to d2 mod 2**(bit width of ResultType).
 *
 * The algorithm below is inspired by that found in
 * <http://trac.webkit.org/changeset/67825/trunk/JavaScriptCore/runtime/JSValue.cpp>
 * but has been generalized to all integer widths.
 */
template<typename ResultType>
inline ResultType
ToUintWidth(double d)
{
    static_assert(mozilla::IsUnsigned<ResultType>::value,
                  "ResultType must be an unsigned type");

    uint64_t bits = mozilla::BitwiseCast<uint64_t>(d);
    unsigned DoubleExponentShift = mozilla::FloatingPoint<double>::kExponentShift;

    // Extract the exponent component.  (Be careful here!  It's not technically
    // the exponent in NaN, infinities, and subnormals.)
    int_fast16_t exp =
        int_fast16_t((bits & mozilla::FloatingPoint<double>::kExponentBits) >> DoubleExponentShift) -
        int_fast16_t(mozilla::FloatingPoint<double>::kExponentBias);

    // If the exponent's less than zero, abs(d) < 1, so the result is 0.  (This
    // also handles subnormals.)
    if (exp < 0)
        return 0;

    uint_fast16_t exponent = mozilla::AssertedCast<uint_fast16_t>(exp);

    // If the exponent is greater than or equal to the bits of precision of a
    // double plus ResultType's width, the number is either infinite, NaN, or
    // too large to have lower-order bits in the congruent value.  (Example:
    // 2**84 is exactly representable as a double.  The next exact double is
    // 2**84 + 2**32.  Thus if ResultType is int32_t, an exponent >= 84 implies
    // floor(abs(d)) == 0 mod 2**32.)  Return 0 in all these cases.
    const size_t ResultWidth = CHAR_BIT * sizeof(ResultType);
    if (exponent >= DoubleExponentShift + ResultWidth)
        return 0;

    // The significand contains the bits that will determine the final result.
    // Shift those bits left or right, according to the exponent, to their
    // locations in the unsigned binary representation of floor(abs(d)).
    static_assert(sizeof(ResultType) <= sizeof(uint64_t),
                  "Left-shifting below would lose upper bits");
    ResultType result = (exponent > DoubleExponentShift)
                        ? ResultType(bits << (exponent - DoubleExponentShift))
                        : ResultType(bits >> (DoubleExponentShift - exponent));

    // Two further complications remain.  First, |result| may contain bogus
    // sign/exponent bits.  Second, IEEE-754 numbers' significands (excluding
    // subnormals, but we already handled those) have an implicit leading 1
    // which may affect the final result.
    //
    // It may appear that there's complexity here depending on how ResultWidth
    // and DoubleExponentShift relate, but it turns out there's not.
    //
    // Assume ResultWidth < DoubleExponentShift:
    //   Only right-shifts leave bogus bits in |result|.  For this to happen,
    //   we must right-shift by > |DoubleExponentShift - ResultWidth|, implying
    //   |exponent < ResultWidth|.
    //   The implicit leading bit only matters if it appears in the final
    //   result -- if |2**exponent mod 2**ResultWidth != 0|.  This implies
    //   |exponent < ResultWidth|.
    // Otherwise assume ResultWidth >= DoubleExponentShift:
    //   Any left-shift less than |ResultWidth - DoubleExponentShift| leaves
    //   bogus bits in |result|.  This implies |exponent < ResultWidth|.  Any
    //   right-shift less than |ResultWidth| does too, which implies
    //   |DoubleExponentShift - ResultWidth < exponent|.  By assumption, then,
    //   |exponent| is negative, but we excluded that above.  So bogus bits
    //   need only |exponent < ResultWidth|.
    //   The implicit leading bit matters identically to the other case, so
    //   again, |exponent < ResultWidth|.
    if (exponent < ResultWidth) {
        ResultType implicitOne = ResultType(1) << exponent;
        result &= implicitOne - 1; // remove bogus bits
        result += implicitOne; // add the implicit bit
    }

    // Compute the congruent value in the signed range.
    return (bits & mozilla::FloatingPoint<double>::kSignBit) ? ~result + 1 : result;
}

template<typename ResultType>
inline ResultType
ToIntWidth(double d)
{
    static_assert(mozilla::IsSigned<ResultType>::value,
                  "ResultType must be a signed type");

    const ResultType MaxValue = (1ULL << (CHAR_BIT * sizeof(ResultType) - 1)) - 1;
    const ResultType MinValue = -MaxValue - 1;

    typedef typename mozilla::MakeUnsigned<ResultType>::Type UnsignedResult;
    UnsignedResult u = ToUintWidth<UnsignedResult>(d);
    if (u <= UnsignedResult(MaxValue))
        return static_cast<ResultType>(u);
    return (MinValue + static_cast<ResultType>(u - MaxValue)) - 1;
}

} // namespace detail

/* ES5 9.5 ToInt32 (specialized for doubles). */
inline int32_t
ToInt32(double d)
{
    // clang crashes compiling this when targeting arm-darwin:
    // https://llvm.org/bugs/show_bug.cgi?id=22974
#if defined (__arm__) && defined (__GNUC__) && !defined(__APPLE__)
    int32_t i;
    uint32_t    tmp0;
    uint32_t    tmp1;
    uint32_t    tmp2;
    asm (
    // We use a pure integer solution here. In the 'softfp' ABI, the argument
    // will start in r0 and r1, and VFP can't do all of the necessary ECMA
    // conversions by itself so some integer code will be required anyway. A
    // hybrid solution is faster on A9, but this pure integer solution is
    // notably faster for A8.

    // %0 is the result register, and may alias either of the %[QR]1 registers.
    // %Q4 holds the lower part of the mantissa.
    // %R4 holds the sign, exponent, and the upper part of the mantissa.
    // %1, %2 and %3 are used as temporary values.

    // Extract the exponent.
"   mov     %1, %R4, LSR #20\n"
"   bic     %1, %1, #(1 << 11)\n"  // Clear the sign.

    // Set the implicit top bit of the mantissa. This clobbers a bit of the
    // exponent, but we have already extracted that.
"   orr     %R4, %R4, #(1 << 20)\n"

    // Special Cases
    //   We should return zero in the following special cases:
    //    - Exponent is 0x000 - 1023: +/-0 or subnormal.
    //    - Exponent is 0x7ff - 1023: +/-INFINITY or NaN
    //      - This case is implicitly handled by the standard code path anyway,
    //        as shifting the mantissa up by the exponent will result in '0'.
    //
    // The result is composed of the mantissa, prepended with '1' and
    // bit-shifted left by the (decoded) exponent. Note that because the r1[20]
    // is the bit with value '1', r1 is effectively already shifted (left) by
    // 20 bits, and r0 is already shifted by 52 bits.

    // Adjust the exponent to remove the encoding offset. If the decoded
    // exponent is negative, quickly bail out with '0' as such values round to
    // zero anyway. This also catches +/-0 and subnormals.
"   sub     %1, %1, #0xff\n"
"   subs    %1, %1, #0x300\n"
"   bmi     8f\n"

    //  %1 = (decoded) exponent >= 0
    //  %R4 = upper mantissa and sign

    // ---- Lower Mantissa ----
"   subs    %3, %1, #52\n"         // Calculate exp-52
"   bmi     1f\n"

    // Shift r0 left by exp-52.
    // Ensure that we don't overflow ARM's 8-bit shift operand range.
    // We need to handle anything up to an 11-bit value here as we know that
    // 52 <= exp <= 1024 (0x400). Any shift beyond 31 bits results in zero
    // anyway, so as long as we don't touch the bottom 5 bits, we can use
    // a logical OR to push long shifts into the 32 <= (exp&0xff) <= 255 range.
"   bic     %2, %3, #0xff\n"
"   orr     %3, %3, %2, LSR #3\n"
    // We can now perform a straight shift, avoiding the need for any
    // conditional instructions or extra branches.
"   mov     %Q4, %Q4, LSL %3\n"
"   b       2f\n"
"1:\n" // Shift r0 right by 52-exp.
    // We know that 0 <= exp < 52, and we can shift up to 255 bits so 52-exp
    // will always be a valid shift and we can sk%3 the range check for this case.
"   rsb     %3, %1, #52\n"
"   mov     %Q4, %Q4, LSR %3\n"

    //  %1 = (decoded) exponent
    //  %R4 = upper mantissa and sign
    //  %Q4 = partially-converted integer

"2:\n"
    // ---- Upper Mantissa ----
    // This is much the same as the lower mantissa, with a few different
    // boundary checks and some masking to hide the exponent & sign bit in the
    // upper word.
    // Note that the upper mantissa is pre-shifted by 20 in %R4, but we shift
    // it left more to remove the sign and exponent so it is effectively
    // pre-shifted by 31 bits.
"   subs    %3, %1, #31\n"          // Calculate exp-31
"   mov     %1, %R4, LSL #11\n"     // Re-use %1 as a temporary register.
"   bmi     3f\n"

    // Shift %R4 left by exp-31.
    // Avoid overflowing the 8-bit shift range, as before.
"   bic     %2, %3, #0xff\n"
"   orr     %3, %3, %2, LSR #3\n"
    // Perform the shift.
"   mov     %2, %1, LSL %3\n"
"   b       4f\n"
"3:\n" // Shift r1 right by 31-exp.
    // We know that 0 <= exp < 31, and we can shift up to 255 bits so 31-exp
    // will always be a valid shift and we can skip the range check for this case.
"   rsb     %3, %3, #0\n"          // Calculate 31-exp from -(exp-31)
"   mov     %2, %1, LSR %3\n"      // Thumb-2 can't do "LSR %3" in "orr".

    //  %Q4 = partially-converted integer (lower)
    //  %R4 = upper mantissa and sign
    //  %2 = partially-converted integer (upper)

"4:\n"
    // Combine the converted parts.
"   orr     %Q4, %Q4, %2\n"
    // Negate the result if we have to, and move it to %0 in the process. To
    // avoid conditionals, we can do this by inverting on %R4[31], then adding
    // %R4[31]>>31.
"   eor     %Q4, %Q4, %R4, ASR #31\n"
"   add     %0, %Q4, %R4, LSR #31\n"
"   b       9f\n"
"8:\n"
    // +/-INFINITY, +/-0, subnormals, NaNs, and anything else out-of-range that
    // will result in a conversion of '0'.
"   mov     %0, #0\n"
"9:\n"
    : "=r" (i), "=&r" (tmp0), "=&r" (tmp1), "=&r" (tmp2), "=&r" (d)
    : "4" (d)
    : "cc"
        );
    return i;
#else
    return detail::ToIntWidth<int32_t>(d);
#endif
}

/* ES5 9.6 (specialized for doubles). */
inline uint32_t
ToUint32(double d)
{
    return detail::ToUintWidth<uint32_t>(d);
}

/* WEBIDL 4.2.4 */
inline int8_t
ToInt8(double d)
{
    return detail::ToIntWidth<int8_t>(d);
}

/* WEBIDL 4.2.6 */
inline int16_t
ToInt16(double d)
{
    return detail::ToIntWidth<int16_t>(d);
}

/* WEBIDL 4.2.10 */
inline int64_t
ToInt64(double d)
{
    return detail::ToIntWidth<int64_t>(d);
}

/* WEBIDL 4.2.11 */
inline uint64_t
ToUint64(double d)
{
    return detail::ToUintWidth<uint64_t>(d);
}

} // namespace JS

#endif /* js_Conversions_h */
