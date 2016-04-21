/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS number type and wrapper class.
 */

#include "jsnum.h"

#include "mozilla/double-conversion.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/PodOperations.h"
#include "mozilla/RangedPtr.h"

#ifdef HAVE_LOCALECONV
#include <locale.h>
#endif
#include <math.h>
#include <string.h>

#include "jsatom.h"
#include "jscntxt.h"
#include "jsdtoa.h"
#include "jsobj.h"
#include "jsstr.h"
#include "jstypes.h"

#include "js/Conversions.h"
#include "vm/GlobalObject.h"
#include "vm/StringBuffer.h"

#include "jsatominlines.h"

#include "vm/NativeObject-inl.h"
#include "vm/NumberObject-inl.h"
#include "vm/String-inl.h"

using namespace js;

using mozilla::Abs;
using mozilla::ArrayLength;
using mozilla::MinNumberValue;
using mozilla::NegativeInfinity;
using mozilla::PodCopy;
using mozilla::PositiveInfinity;
using mozilla::RangedPtr;

using JS::AutoCheckCannotGC;
using JS::GenericNaN;
using JS::ToInt8;
using JS::ToInt16;
using JS::ToInt32;
using JS::ToInt64;
using JS::ToUint32;
using JS::ToUint64;

/*
 * If we're accumulating a decimal number and the number is >= 2^53, then the
 * fast result from the loop in Get{Prefix,Decimal}Integer may be inaccurate.
 * Call js_strtod_harder to get the correct answer.
 */
template <typename CharT>
static bool
ComputeAccurateDecimalInteger(ExclusiveContext* cx, const CharT* start, const CharT* end,
                              double* dp)
{
    size_t length = end - start;
    ScopedJSFreePtr<char> cstr(cx->pod_malloc<char>(length + 1));
    if (!cstr)
        return false;

    for (size_t i = 0; i < length; i++) {
        char c = char(start[i]);
        MOZ_ASSERT(('0' <= c && c <= '9') || ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z'));
        cstr[i] = c;
    }
    cstr[length] = 0;

    char* estr;
    int err = 0;
    *dp = js_strtod_harder(cx->dtoaState(), cstr, &estr, &err);
    if (err == JS_DTOA_ENOMEM) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

namespace {

template <typename CharT>
class BinaryDigitReader
{
    const int base;      /* Base of number; must be a power of 2 */
    int digit;           /* Current digit value in radix given by base */
    int digitMask;       /* Mask to extract the next bit from digit */
    const CharT* start;  /* Pointer to the remaining digits */
    const CharT* end;    /* Pointer to first non-digit */

  public:
    BinaryDigitReader(int base, const CharT* start, const CharT* end)
      : base(base), digit(0), digitMask(0), start(start), end(end)
    {
    }

    /* Return the next binary digit from the number, or -1 if done. */
    int nextDigit() {
        if (digitMask == 0) {
            if (start == end)
                return -1;

            int c = *start++;
            MOZ_ASSERT(('0' <= c && c <= '9') || ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z'));
            if ('0' <= c && c <= '9')
                digit = c - '0';
            else if ('a' <= c && c <= 'z')
                digit = c - 'a' + 10;
            else
                digit = c - 'A' + 10;
            digitMask = base >> 1;
        }

        int bit = (digit & digitMask) != 0;
        digitMask >>= 1;
        return bit;
    }
};

} /* anonymous namespace */

/*
 * The fast result might also have been inaccurate for power-of-two bases. This
 * happens if the addition in value * 2 + digit causes a round-down to an even
 * least significant mantissa bit when the first dropped bit is a one.  If any
 * of the following digits in the number (which haven't been added in yet) are
 * nonzero, then the correct action would have been to round up instead of
 * down.  An example occurs when reading the number 0x1000000000000081, which
 * rounds to 0x1000000000000000 instead of 0x1000000000000100.
 */
template <typename CharT>
static double
ComputeAccurateBinaryBaseInteger(const CharT* start, const CharT* end, int base)
{
    BinaryDigitReader<CharT> bdr(base, start, end);

    /* Skip leading zeroes. */
    int bit;
    do {
        bit = bdr.nextDigit();
    } while (bit == 0);

    MOZ_ASSERT(bit == 1); // guaranteed by Get{Prefix,Decimal}Integer

    /* Gather the 53 significant bits (including the leading 1). */
    double value = 1.0;
    for (int j = 52; j > 0; j--) {
        bit = bdr.nextDigit();
        if (bit < 0)
            return value;
        value = value * 2 + bit;
    }

    /* bit2 is the 54th bit (the first dropped from the mantissa). */
    int bit2 = bdr.nextDigit();
    if (bit2 >= 0) {
        double factor = 2.0;
        int sticky = 0;  /* sticky is 1 if any bit beyond the 54th is 1 */
        int bit3;

        while ((bit3 = bdr.nextDigit()) >= 0) {
            sticky |= bit3;
            factor *= 2;
        }
        value += bit2 & (bit | sticky);
        value *= factor;
    }

    return value;
}

template <typename CharT>
double
js::ParseDecimalNumber(const mozilla::Range<const CharT> chars)
{
    MOZ_ASSERT(chars.length() > 0);
    uint64_t dec = 0;
    RangedPtr<const CharT> s = chars.start(), end = chars.end();
    do {
        CharT c = *s;
        MOZ_ASSERT('0' <= c && c <= '9');
        uint8_t digit = c - '0';
        uint64_t next = dec * 10 + digit;
        MOZ_ASSERT(next < DOUBLE_INTEGRAL_PRECISION_LIMIT,
                   "next value won't be an integrally-precise double");
        dec = next;
    } while (++s < end);
    return static_cast<double>(dec);
}

template double
js::ParseDecimalNumber(const mozilla::Range<const Latin1Char> chars);

template double
js::ParseDecimalNumber(const mozilla::Range<const char16_t> chars);

template <typename CharT>
bool
js::GetPrefixInteger(ExclusiveContext* cx, const CharT* start, const CharT* end, int base,
                     const CharT** endp, double* dp)
{
    MOZ_ASSERT(start <= end);
    MOZ_ASSERT(2 <= base && base <= 36);

    const CharT* s = start;
    double d = 0.0;
    for (; s < end; s++) {
        int digit;
        CharT c = *s;
        if ('0' <= c && c <= '9')
            digit = c - '0';
        else if ('a' <= c && c <= 'z')
            digit = c - 'a' + 10;
        else if ('A' <= c && c <= 'Z')
            digit = c - 'A' + 10;
        else
            break;
        if (digit >= base)
            break;
        d = d * base + digit;
    }

    *endp = s;
    *dp = d;

    /* If we haven't reached the limit of integer precision, we're done. */
    if (d < DOUBLE_INTEGRAL_PRECISION_LIMIT)
        return true;

    /*
     * Otherwise compute the correct integer from the prefix of valid digits
     * if we're computing for base ten or a power of two.  Don't worry about
     * other bases; see 15.1.2.2 step 13.
     */
    if (base == 10)
        return ComputeAccurateDecimalInteger(cx, start, s, dp);

    if ((base & (base - 1)) == 0)
        *dp = ComputeAccurateBinaryBaseInteger(start, s, base);

    return true;
}

template bool
js::GetPrefixInteger(ExclusiveContext* cx, const char16_t* start, const char16_t* end, int base,
                     const char16_t** endp, double* dp);

template bool
js::GetPrefixInteger(ExclusiveContext* cx, const Latin1Char* start, const Latin1Char* end,
                     int base, const Latin1Char** endp, double* dp);

bool
js::GetDecimalInteger(ExclusiveContext* cx, const char16_t* start, const char16_t* end, double* dp)
{
    MOZ_ASSERT(start <= end);

    const char16_t* s = start;
    double d = 0.0;
    for (; s < end; s++) {
        char16_t c = *s;
        MOZ_ASSERT('0' <= c && c <= '9');
        int digit = c - '0';
        d = d * 10 + digit;
    }

    *dp = d;

    // If we haven't reached the limit of integer precision, we're done.
    if (d < DOUBLE_INTEGRAL_PRECISION_LIMIT)
        return true;

    // Otherwise compute the correct integer from the prefix of valid digits.
    return ComputeAccurateDecimalInteger(cx, start, s, dp);
}

static bool
num_parseFloat(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    JSString* str = ToString<CanGC>(cx, args[0]);
    if (!str)
        return false;

    JSLinearString* linear = str->ensureLinear(cx);
    if (!linear)
        return false;

    double d;
    AutoCheckCannotGC nogc;
    if (linear->hasLatin1Chars()) {
        const Latin1Char* begin = linear->latin1Chars(nogc);
        const Latin1Char* end;
        if (!js_strtod(cx, begin, begin + linear->length(), &end, &d))
            return false;
        if (end == begin)
            d = GenericNaN();
    } else {
        const char16_t* begin = linear->twoByteChars(nogc);
        const char16_t* end;
        if (!js_strtod(cx, begin, begin + linear->length(), &end, &d))
            return false;
        if (end == begin)
            d = GenericNaN();
    }

    args.rval().setDouble(d);
    return true;
}

template <typename CharT>
static bool
ParseIntImpl(JSContext* cx, const CharT* chars, size_t length, bool stripPrefix, int32_t radix,
             double* res)
{
    /* Step 2. */
    const CharT* end = chars + length;
    const CharT* s = SkipSpace(chars, end);

    MOZ_ASSERT(chars <= s);
    MOZ_ASSERT(s <= end);

    /* Steps 3-4. */
    bool negative = (s != end && s[0] == '-');

    /* Step 5. */
    if (s != end && (s[0] == '-' || s[0] == '+'))
        s++;

    /* Step 10. */
    if (stripPrefix) {
        if (end - s >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
            radix = 16;
        }
    }

    /* Steps 11-15. */
    const CharT* actualEnd;
    double d;
    if (!GetPrefixInteger(cx, s, end, radix, &actualEnd, &d))
        return false;

    if (s == actualEnd)
        *res = GenericNaN();
    else
        *res = negative ? -d : d;
    return true;
}

/* ES5 15.1.2.2. */
bool
js::num_parseInt(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Fast paths and exceptional cases. */
    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    if (args.length() == 1 ||
        (args[1].isInt32() && (args[1].toInt32() == 0 || args[1].toInt32() == 10))) {
        if (args[0].isInt32()) {
            args.rval().set(args[0]);
            return true;
        }

        /*
         * Step 1 is |inputString = ToString(string)|. When string >=
         * 1e21, ToString(string) is in the form "NeM". 'e' marks the end of
         * the word, which would mean the result of parseInt(string) should be |N|.
         *
         * To preserve this behaviour, we can't use the fast-path when string >
         * 1e21, or else the result would be |NeM|.
         *
         * The same goes for values smaller than 1.0e-6, because the string would be in
         * the form of "Ne-M".
         */
        if (args[0].isDouble()) {
            double d = args[0].toDouble();
            if (1.0e-6 < d && d < 1.0e21) {
                args.rval().setNumber(floor(d));
                return true;
            }
            if (-1.0e21 < d && d < -1.0e-6) {
                args.rval().setNumber(-floor(-d));
                return true;
            }
            if (d == 0.0) {
                args.rval().setInt32(0);
                return true;
            }
        }
    }

    /* Step 1. */
    RootedString inputString(cx, ToString<CanGC>(cx, args[0]));
    if (!inputString)
        return false;
    args[0].setString(inputString);

    /* Steps 6-9. */
    bool stripPrefix = true;
    int32_t radix;
    if (!args.hasDefined(1)) {
        radix = 10;
    } else {
        if (!ToInt32(cx, args[1], &radix))
            return false;
        if (radix == 0) {
            radix = 10;
        } else {
            if (radix < 2 || radix > 36) {
                args.rval().setNaN();
                return true;
            }
            if (radix != 16)
                stripPrefix = false;
        }
    }

    JSLinearString* linear = inputString->ensureLinear(cx);
    if (!linear)
        return false;

    AutoCheckCannotGC nogc;
    size_t length = inputString->length();
    double number;
    if (linear->hasLatin1Chars()) {
        if (!ParseIntImpl(cx, linear->latin1Chars(nogc), length, stripPrefix, radix, &number))
            return false;
    } else {
        if (!ParseIntImpl(cx, linear->twoByteChars(nogc), length, stripPrefix, radix, &number))
            return false;
    }

    args.rval().setNumber(number);
    return true;
}

static const JSFunctionSpec number_functions[] = {
    JS_SELF_HOSTED_FN(js_isNaN_str, "Global_isNaN", 1, JSPROP_RESOLVING),
    JS_SELF_HOSTED_FN(js_isFinite_str, "Global_isFinite", 1, JSPROP_RESOLVING),
    JS_FS_END
};

const Class NumberObject::class_ = {
    js_Number_str,
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_HAS_CACHED_PROTO(JSProto_Number)
};

static bool
Number(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Sample JS_CALLEE before clobbering. */
    bool isConstructing = args.isConstructing();

    if (args.length() > 0) {
        if (!ToNumber(cx, args[0]))
            return false;
        args.rval().set(args[0]);
    } else {
        args.rval().setInt32(0);
    }

    if (!isConstructing)
        return true;

    RootedObject newTarget(cx, &args.newTarget().toObject());
    RootedObject proto(cx);
    if (!GetPrototypeFromConstructor(cx, newTarget, &proto))
        return false;
    JSObject* obj = NumberObject::create(cx, args.rval().toNumber(), proto);
    if (!obj)
        return false;
    args.rval().setObject(*obj);
    return true;
}

MOZ_ALWAYS_INLINE bool
IsNumber(HandleValue v)
{
    return v.isNumber() || (v.isObject() && v.toObject().is<NumberObject>());
}

static inline double
Extract(const Value& v)
{
    if (v.isNumber())
        return v.toNumber();
    return v.toObject().as<NumberObject>().unbox();
}

#if JS_HAS_TOSOURCE
MOZ_ALWAYS_INLINE bool
num_toSource_impl(JSContext* cx, const CallArgs& args)
{
    double d = Extract(args.thisv());

    StringBuffer sb(cx);
    if (!sb.append("(new Number(") ||
        !NumberValueToStringBuffer(cx, NumberValue(d), sb) ||
        !sb.append("))"))
    {
        return false;
    }

    JSString* str = sb.finishString();
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static bool
num_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsNumber, num_toSource_impl>(cx, args);
}
#endif

ToCStringBuf::ToCStringBuf() : dbuf(nullptr)
{
    static_assert(sbufSize >= DTOSTR_STANDARD_BUFFER_SIZE,
                  "builtin space must be large enough to store even the "
                  "longest string produced by a conversion");
}

ToCStringBuf::~ToCStringBuf()
{
    js_free(dbuf);
}

MOZ_ALWAYS_INLINE
static JSFlatString*
LookupDtoaCache(ExclusiveContext* cx, double d)
{
    if (JSCompartment* comp = cx->compartment()) {
        if (JSFlatString* str = comp->dtoaCache.lookup(10, d))
            return str;
    }

    return nullptr;
}

MOZ_ALWAYS_INLINE
static void
CacheNumber(ExclusiveContext* cx, double d, JSFlatString* str)
{
    if (JSCompartment* comp = cx->compartment())
        comp->dtoaCache.cache(10, d, str);
}

MOZ_ALWAYS_INLINE
static JSFlatString*
LookupInt32ToString(ExclusiveContext* cx, int32_t si)
{
    if (si >= 0 && StaticStrings::hasInt(si))
        return cx->staticStrings().getInt(si);

    return LookupDtoaCache(cx, si);
}

template <typename T>
MOZ_ALWAYS_INLINE
static T*
BackfillInt32InBuffer(int32_t si, T* buffer, size_t size, size_t* length)
{
    uint32_t ui = Abs(si);
    MOZ_ASSERT_IF(si == INT32_MIN, ui == uint32_t(INT32_MAX) + 1);

    RangedPtr<T> end(buffer + size - 1, buffer, size);
    *end = '\0';
    RangedPtr<T> start = BackfillIndexInCharBuffer(ui, end);
    if (si < 0)
        *--start = '-';

    *length = end - start;
    return start.get();
}

template <AllowGC allowGC>
JSFlatString*
js::Int32ToString(ExclusiveContext* cx, int32_t si)
{
    if (JSFlatString* str = LookupInt32ToString(cx, si))
        return str;

    Latin1Char buffer[JSFatInlineString::MAX_LENGTH_LATIN1 + 1];
    size_t length;
    Latin1Char* start = BackfillInt32InBuffer(si, buffer, ArrayLength(buffer), &length);

    mozilla::Range<const Latin1Char> chars(start, length);
    JSInlineString* str = NewInlineString<allowGC>(cx, chars);
    if (!str)
        return nullptr;

    CacheNumber(cx, si, str);
    return str;
}

template JSFlatString*
js::Int32ToString<CanGC>(ExclusiveContext* cx, int32_t si);

template JSFlatString*
js::Int32ToString<NoGC>(ExclusiveContext* cx, int32_t si);

JSAtom*
js::Int32ToAtom(ExclusiveContext* cx, int32_t si)
{
    if (JSFlatString* str = LookupInt32ToString(cx, si))
        return js::AtomizeString(cx, str);

    char buffer[JSFatInlineString::MAX_LENGTH_TWO_BYTE + 1];
    size_t length;
    char* start = BackfillInt32InBuffer(si, buffer, JSFatInlineString::MAX_LENGTH_TWO_BYTE + 1, &length);

    JSAtom* atom = Atomize(cx, start, length);
    if (!atom)
        return nullptr;

    CacheNumber(cx, si, atom);
    return atom;
}

/* Returns a non-nullptr pointer to inside cbuf.  */
static char*
Int32ToCString(ToCStringBuf* cbuf, int32_t i, size_t* len, int base = 10)
{
    uint32_t u = Abs(i);

    RangedPtr<char> cp(cbuf->sbuf + ToCStringBuf::sbufSize - 1, cbuf->sbuf, ToCStringBuf::sbufSize);
    char* end = cp.get();
    *cp = '\0';

    /* Build the string from behind. */
    switch (base) {
    case 10:
      cp = BackfillIndexInCharBuffer(u, cp);
      break;
    case 16:
      do {
          unsigned newu = u / 16;
          *--cp = "0123456789abcdef"[u - newu * 16];
          u = newu;
      } while (u != 0);
      break;
    default:
      MOZ_ASSERT(base >= 2 && base <= 36);
      do {
          unsigned newu = u / base;
          *--cp = "0123456789abcdefghijklmnopqrstuvwxyz"[u - newu * base];
          u = newu;
      } while (u != 0);
      break;
    }
    if (i < 0)
        *--cp = '-';

    *len = end - cp.get();
    return cp.get();
}

template <AllowGC allowGC>
static JSString*
NumberToStringWithBase(ExclusiveContext* cx, double d, int base);

MOZ_ALWAYS_INLINE bool
num_toString_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsNumber(args.thisv()));

    double d = Extract(args.thisv());

    int32_t base = 10;
    if (args.hasDefined(0)) {
        double d2;
        if (!ToInteger(cx, args[0], &d2))
            return false;

        if (d2 < 2 || d2 > 36) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_RADIX);
            return false;
        }

        base = int32_t(d2);
    }
    JSString* str = NumberToStringWithBase<CanGC>(cx, d, base);
    if (!str) {
        JS_ReportOutOfMemory(cx);
        return false;
    }
    args.rval().setString(str);
    return true;
}

bool
js::num_toString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsNumber, num_toString_impl>(cx, args);
}

#if !EXPOSE_INTL_API
MOZ_ALWAYS_INLINE bool
num_toLocaleString_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsNumber(args.thisv()));

    double d = Extract(args.thisv());

    RootedString str(cx, NumberToStringWithBase<CanGC>(cx, d, 10));
    if (!str) {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    /*
     * Create the string, move back to bytes to make string twiddling
     * a bit easier and so we can insert platform charset seperators.
     */
    JSAutoByteString numBytes(cx, str);
    if (!numBytes)
        return false;
    const char* num = numBytes.ptr();
    if (!num)
        return false;

    /*
     * Find the first non-integer value, whether it be a letter as in
     * 'Infinity', a decimal point, or an 'e' from exponential notation.
     */
    const char* nint = num;
    if (*nint == '-')
        nint++;
    while (*nint >= '0' && *nint <= '9')
        nint++;
    int digits = nint - num;
    const char* end = num + digits;
    if (!digits) {
        args.rval().setString(str);
        return true;
    }

    JSRuntime* rt = cx->runtime();
    size_t thousandsLength = strlen(rt->thousandsSeparator);
    size_t decimalLength = strlen(rt->decimalSeparator);

    /* Figure out how long resulting string will be. */
    int buflen = strlen(num);
    if (*nint == '.')
        buflen += decimalLength - 1; /* -1 to account for existing '.' */

    const char* numGrouping;
    const char* tmpGroup;
    numGrouping = tmpGroup = rt->numGrouping;
    int remainder = digits;
    if (*num == '-')
        remainder--;

    while (*tmpGroup != CHAR_MAX && *tmpGroup != '\0') {
        if (*tmpGroup >= remainder)
            break;
        buflen += thousandsLength;
        remainder -= *tmpGroup;
        tmpGroup++;
    }

    int nrepeat;
    if (*tmpGroup == '\0' && *numGrouping != '\0') {
        nrepeat = (remainder - 1) / tmpGroup[-1];
        buflen += thousandsLength * nrepeat;
        remainder -= nrepeat * tmpGroup[-1];
    } else {
        nrepeat = 0;
    }
    tmpGroup--;

    char* buf = cx->pod_malloc<char>(buflen + 1);
    if (!buf)
        return false;

    char* tmpDest = buf;
    const char* tmpSrc = num;

    while (*tmpSrc == '-' || remainder--) {
        MOZ_ASSERT(tmpDest - buf < buflen);
        *tmpDest++ = *tmpSrc++;
    }
    while (tmpSrc < end) {
        MOZ_ASSERT(tmpDest - buf + ptrdiff_t(thousandsLength) <= buflen);
        strcpy(tmpDest, rt->thousandsSeparator);
        tmpDest += thousandsLength;
        MOZ_ASSERT(tmpDest - buf + *tmpGroup <= buflen);
        js_memcpy(tmpDest, tmpSrc, *tmpGroup);
        tmpDest += *tmpGroup;
        tmpSrc += *tmpGroup;
        if (--nrepeat < 0)
            tmpGroup--;
    }

    if (*nint == '.') {
        MOZ_ASSERT(tmpDest - buf + ptrdiff_t(decimalLength) <= buflen);
        strcpy(tmpDest, rt->decimalSeparator);
        tmpDest += decimalLength;
        MOZ_ASSERT(tmpDest - buf + ptrdiff_t(strlen(nint + 1)) <= buflen);
        strcpy(tmpDest, nint + 1);
    } else {
        MOZ_ASSERT(tmpDest - buf + ptrdiff_t(strlen(nint)) <= buflen);
        strcpy(tmpDest, nint);
    }

    if (cx->runtime()->localeCallbacks && cx->runtime()->localeCallbacks->localeToUnicode) {
        Rooted<Value> v(cx, StringValue(str));
        bool ok = !!cx->runtime()->localeCallbacks->localeToUnicode(cx, buf, &v);
        if (ok)
            args.rval().set(v);
        js_free(buf);
        return ok;
    }

    str = NewStringCopyN<CanGC>(cx, buf, buflen);
    js_free(buf);
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

static bool
num_toLocaleString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsNumber, num_toLocaleString_impl>(cx, args);
}
#endif /* !EXPOSE_INTL_API */

MOZ_ALWAYS_INLINE bool
num_valueOf_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsNumber(args.thisv()));
    args.rval().setNumber(Extract(args.thisv()));
    return true;
}

bool
js::num_valueOf(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsNumber, num_valueOf_impl>(cx, args);
}

static const unsigned MAX_PRECISION = 100;

static bool
ComputePrecisionInRange(JSContext* cx, int minPrecision, int maxPrecision, HandleValue v,
                        int* precision)
{
    double prec;
    if (!ToInteger(cx, v, &prec))
        return false;
    if (minPrecision <= prec && prec <= maxPrecision) {
        *precision = int(prec);
        return true;
    }

    ToCStringBuf cbuf;
    if (char* numStr = NumberToCString(cx, &cbuf, prec, 10))
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PRECISION_RANGE, numStr);
    return false;
}

static bool
DToStrResult(JSContext* cx, double d, JSDToStrMode mode, int precision, const CallArgs& args)
{
    char buf[DTOSTR_VARIABLE_BUFFER_SIZE(MAX_PRECISION + 1)];
    char* numStr = js_dtostr(cx->mainThread().dtoaState, buf, sizeof buf, mode, precision, d);
    if (!numStr) {
        JS_ReportOutOfMemory(cx);
        return false;
    }
    JSString* str = NewStringCopyZ<CanGC>(cx, numStr);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

/*
 * In the following three implementations, we allow a larger range of precision
 * than ECMA requires; this is permitted by ECMA-262.
 */
MOZ_ALWAYS_INLINE bool
num_toFixed_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsNumber(args.thisv()));

    int precision;
    if (args.length() == 0) {
        precision = 0;
    } else {
        if (!ComputePrecisionInRange(cx, -20, MAX_PRECISION, args[0], &precision))
            return false;
    }

    return DToStrResult(cx, Extract(args.thisv()), DTOSTR_FIXED, precision, args);
}

static bool
num_toFixed(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsNumber, num_toFixed_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
num_toExponential_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsNumber(args.thisv()));

    JSDToStrMode mode;
    int precision;
    if (!args.hasDefined(0)) {
        mode = DTOSTR_STANDARD_EXPONENTIAL;
        precision = 0;
    } else {
        mode = DTOSTR_EXPONENTIAL;
        if (!ComputePrecisionInRange(cx, 0, MAX_PRECISION, args[0], &precision))
            return false;
    }

    return DToStrResult(cx, Extract(args.thisv()), mode, precision + 1, args);
}

static bool
num_toExponential(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsNumber, num_toExponential_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
num_toPrecision_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsNumber(args.thisv()));

    double d = Extract(args.thisv());

    if (!args.hasDefined(0)) {
        JSString* str = NumberToStringWithBase<CanGC>(cx, d, 10);
        if (!str) {
            JS_ReportOutOfMemory(cx);
            return false;
        }
        args.rval().setString(str);
        return true;
    }

    int precision;
    if (!ComputePrecisionInRange(cx, 1, MAX_PRECISION, args[0], &precision))
        return false;

    return DToStrResult(cx, d, DTOSTR_PRECISION, precision, args);
}

static bool
num_toPrecision(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsNumber, num_toPrecision_impl>(cx, args);
}

static const JSFunctionSpec number_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,       num_toSource,          0, 0),
#endif
    JS_FN(js_toString_str,       num_toString,          1, 0),
#if EXPOSE_INTL_API
    JS_SELF_HOSTED_FN(js_toLocaleString_str, "Number_toLocaleString", 0,0),
#else
    JS_FN(js_toLocaleString_str, num_toLocaleString,     0,0),
#endif
    JS_FN(js_valueOf_str,        num_valueOf,           0, 0),
    JS_FN("toFixed",             num_toFixed,           1, 0),
    JS_FN("toExponential",       num_toExponential,     1, 0),
    JS_FN("toPrecision",         num_toPrecision,       1, 0),
    JS_FS_END
};

// ES6 draft ES6 15.7.3.12
static bool
Number_isInteger(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1 || !args[0].isNumber()) {
        args.rval().setBoolean(false);
        return true;
    }
    Value val = args[0];
    args.rval().setBoolean(val.isInt32() ||
                           (mozilla::IsFinite(val.toDouble()) &&
                            JS::ToInteger(val.toDouble()) == val.toDouble()));
    return true;
}


static const JSFunctionSpec number_static_methods[] = {
    JS_SELF_HOSTED_FN("isFinite", "Number_isFinite", 1,0),
    JS_FN("isInteger", Number_isInteger, 1, 0),
    JS_SELF_HOSTED_FN("isNaN", "Number_isNaN", 1,0),
    JS_SELF_HOSTED_FN("isSafeInteger", "Number_isSafeInteger", 1,0),
    JS_FS_END
};


/*
 * Set the exception mask to mask all exceptions and set the FPU precision
 * to 53 bit mantissa (64 bit doubles).
 */
void
js::FIX_FPU()
{
#if (defined __GNUC__ && defined __i386__) || \
    (defined __SUNPRO_CC && defined __i386)
    short control;
    asm("fstcw %0" : "=m" (control) : );
    control &= ~0x300; // Lower bits 8 and 9 (precision control).
    control |= 0x2f3;  // Raise bits 0-5 (exception masks) and 9 (64-bit precision).
    asm("fldcw %0" : : "m" (control) );
#endif
}

bool
js::InitRuntimeNumberState(JSRuntime* rt)
{
    FIX_FPU();

    // XXX If EXPOSE_INTL_API becomes true all the time at some point,
    //     js::InitRuntimeNumberState is no longer fallible, and we should
    //     change its return type.
#if !EXPOSE_INTL_API
    /* Copy locale-specific separators into the runtime strings. */
    const char* thousandsSeparator;
    const char* decimalPoint;
    const char* grouping;
#ifdef HAVE_LOCALECONV
    struct lconv* locale = localeconv();
    thousandsSeparator = locale->thousands_sep;
    decimalPoint = locale->decimal_point;
    grouping = locale->grouping;
#else
    thousandsSeparator = getenv("LOCALE_THOUSANDS_SEP");
    decimalPoint = getenv("LOCALE_DECIMAL_POINT");
    grouping = getenv("LOCALE_GROUPING");
#endif
    if (!thousandsSeparator)
        thousandsSeparator = "'";
    if (!decimalPoint)
        decimalPoint = ".";
    if (!grouping)
        grouping = "\3\0";

    /*
     * We use single malloc to get the memory for all separator and grouping
     * strings.
     */
    size_t thousandsSeparatorSize = strlen(thousandsSeparator) + 1;
    size_t decimalPointSize = strlen(decimalPoint) + 1;
    size_t groupingSize = strlen(grouping) + 1;

    char* storage = js_pod_malloc<char>(thousandsSeparatorSize +
                                        decimalPointSize +
                                        groupingSize);
    if (!storage)
        return false;

    js_memcpy(storage, thousandsSeparator, thousandsSeparatorSize);
    rt->thousandsSeparator = storage;
    storage += thousandsSeparatorSize;

    js_memcpy(storage, decimalPoint, decimalPointSize);
    rt->decimalSeparator = storage;
    storage += decimalPointSize;

    js_memcpy(storage, grouping, groupingSize);
    rt->numGrouping = grouping;
#endif /* !EXPOSE_INTL_API */
    return true;
}

#if !EXPOSE_INTL_API
void
js::FinishRuntimeNumberState(JSRuntime* rt)
{
    /*
     * The free also releases the memory for decimalSeparator and numGrouping
     * strings.
     */
    char* storage = const_cast<char*>(rt->thousandsSeparator);
    js_free(storage);
}
#endif

JSObject*
js::InitNumberClass(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->isNative());

    /* XXX must do at least once per new thread, so do it per JSContext... */
    FIX_FPU();

    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());

    RootedObject numberProto(cx, global->createBlankPrototype(cx, &NumberObject::class_));
    if (!numberProto)
        return nullptr;
    numberProto->as<NumberObject>().setPrimitiveValue(0);

    RootedFunction ctor(cx);
    ctor = global->createConstructor(cx, Number, cx->names().Number, 1);
    if (!ctor)
        return nullptr;

    if (!LinkConstructorAndPrototype(cx, ctor, numberProto))
        return nullptr;

    /*
     * Our NaN must be one particular canonical value, because we rely on NaN
     * encoding for our value representation.  See Value.h.
     */
    static JSConstDoubleSpec number_constants[] = {
        {"NaN",               GenericNaN()               },
        {"POSITIVE_INFINITY", mozilla::PositiveInfinity<double>() },
        {"NEGATIVE_INFINITY", mozilla::NegativeInfinity<double>() },
        {"MAX_VALUE",         1.7976931348623157E+308    },
        {"MIN_VALUE",         MinNumberValue<double>()   },
        /* ES6 (April 2014 draft) 20.1.2.6 */
        {"MAX_SAFE_INTEGER",  9007199254740991           },
        /* ES6 (April 2014 draft) 20.1.2.10 */
        {"MIN_SAFE_INTEGER", -9007199254740991,          },
        /* ES6 (May 2013 draft) 15.7.3.7 */
        {"EPSILON", 2.2204460492503130808472633361816e-16},
        {0,0}
    };

    /* Add numeric constants (MAX_VALUE, NaN, &c.) to the Number constructor. */
    if (!JS_DefineConstDoubles(cx, ctor, number_constants))
        return nullptr;

    if (!DefinePropertiesAndFunctions(cx, ctor, nullptr, number_static_methods))
        return nullptr;

    if (!DefinePropertiesAndFunctions(cx, numberProto, nullptr, number_methods))
        return nullptr;

    if (!JS_DefineFunctions(cx, global, number_functions))
        return nullptr;

    /* Number.parseInt should be the same function object as global parseInt. */
    RootedId parseIntId(cx, NameToId(cx->names().parseInt));
    JSFunction* parseInt = DefineFunction(cx, global, parseIntId, num_parseInt, 2,
                                          JSPROP_RESOLVING);
    if (!parseInt)
        return nullptr;
    RootedValue parseIntValue(cx, ObjectValue(*parseInt));
    if (!DefineProperty(cx, ctor, parseIntId, parseIntValue, nullptr, nullptr, 0))
        return nullptr;

    /* Number.parseFloat should be the same function object as global parseFloat. */
    RootedId parseFloatId(cx, NameToId(cx->names().parseFloat));
    JSFunction* parseFloat = DefineFunction(cx, global, parseFloatId, num_parseFloat, 1,
                                            JSPROP_RESOLVING);
    if (!parseFloat)
        return nullptr;
    RootedValue parseFloatValue(cx, ObjectValue(*parseFloat));
    if (!DefineProperty(cx, ctor, parseFloatId, parseFloatValue, nullptr, nullptr, 0))
        return nullptr;

    RootedValue valueNaN(cx, cx->runtime()->NaNValue);
    RootedValue valueInfinity(cx, cx->runtime()->positiveInfinityValue);

    /* ES5 15.1.1.1, 15.1.1.2 */
    if (!NativeDefineProperty(cx, global, cx->names().NaN, valueNaN, nullptr, nullptr,
                              JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_RESOLVING) ||
        !NativeDefineProperty(cx, global, cx->names().Infinity, valueInfinity, nullptr, nullptr,
                              JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_RESOLVING))
    {
        return nullptr;
    }

    if (!GlobalObject::initBuiltinConstructor(cx, global, JSProto_Number, ctor, numberProto))
        return nullptr;

    return numberProto;
}

static char*
FracNumberToCString(ExclusiveContext* cx, ToCStringBuf* cbuf, double d, int base = 10)
{
#ifdef DEBUG
    {
        int32_t _;
        MOZ_ASSERT(!mozilla::NumberIsInt32(d, &_));
    }
#endif

    char* numStr;
    if (base == 10) {
        /*
         * This is V8's implementation of the algorithm described in the
         * following paper:
         *
         *   Printing floating-point numbers quickly and accurately with integers.
         *   Florian Loitsch, PLDI 2010.
         */
        const double_conversion::DoubleToStringConverter& converter
            = double_conversion::DoubleToStringConverter::EcmaScriptConverter();
        double_conversion::StringBuilder builder(cbuf->sbuf, cbuf->sbufSize);
        converter.ToShortest(d, &builder);
        numStr = builder.Finalize();
    } else {
        numStr = cbuf->dbuf = js_dtobasestr(cx->dtoaState(), base, d);
    }
    return numStr;
}

char*
js::NumberToCString(JSContext* cx, ToCStringBuf* cbuf, double d, int base/* = 10*/)
{
    int32_t i;
    size_t len;
    return mozilla::NumberIsInt32(d, &i)
           ? Int32ToCString(cbuf, i, &len, base)
           : FracNumberToCString(cx, cbuf, d, base);
}

template <AllowGC allowGC>
static JSString*
NumberToStringWithBase(ExclusiveContext* cx, double d, int base)
{
    ToCStringBuf cbuf;
    char* numStr;

    /*
     * Caller is responsible for error reporting. When called from trace,
     * returning nullptr here will cause us to fall of trace and then retry
     * from the interpreter (which will report the error).
     */
    if (base < 2 || base > 36)
        return nullptr;

    JSCompartment* comp = cx->compartment();

    int32_t i;
    if (mozilla::NumberIsInt32(d, &i)) {
        if (base == 10 && StaticStrings::hasInt(i))
            return cx->staticStrings().getInt(i);
        if (unsigned(i) < unsigned(base)) {
            if (i < 10)
                return cx->staticStrings().getInt(i);
            char16_t c = 'a' + i - 10;
            MOZ_ASSERT(StaticStrings::hasUnit(c));
            return cx->staticStrings().getUnit(c);
        }

        if (JSFlatString* str = comp->dtoaCache.lookup(base, d))
            return str;

        size_t len;
        numStr = Int32ToCString(&cbuf, i, &len, base);
        MOZ_ASSERT(!cbuf.dbuf && numStr >= cbuf.sbuf && numStr < cbuf.sbuf + cbuf.sbufSize);
    } else {
        if (JSFlatString* str = comp->dtoaCache.lookup(base, d))
            return str;

        numStr = FracNumberToCString(cx, &cbuf, d, base);
        if (!numStr) {
            ReportOutOfMemory(cx);
            return nullptr;
        }
        MOZ_ASSERT_IF(base == 10,
                      !cbuf.dbuf && numStr >= cbuf.sbuf && numStr < cbuf.sbuf + cbuf.sbufSize);
        MOZ_ASSERT_IF(base != 10,
                      cbuf.dbuf && cbuf.dbuf == numStr);
    }

    JSFlatString* s = NewStringCopyZ<allowGC>(cx, numStr);

    comp->dtoaCache.cache(base, d, s);
    return s;
}

template <AllowGC allowGC>
JSString*
js::NumberToString(ExclusiveContext* cx, double d)
{
    return NumberToStringWithBase<allowGC>(cx, d, 10);
}

template JSString*
js::NumberToString<CanGC>(ExclusiveContext* cx, double d);

template JSString*
js::NumberToString<NoGC>(ExclusiveContext* cx, double d);

JSAtom*
js::NumberToAtom(ExclusiveContext* cx, double d)
{
    int32_t si;
    if (mozilla::NumberIsInt32(d, &si))
        return Int32ToAtom(cx, si);

    if (JSFlatString* str = LookupDtoaCache(cx, d))
        return AtomizeString(cx, str);

    ToCStringBuf cbuf;
    char* numStr = FracNumberToCString(cx, &cbuf, d);
    if (!numStr) {
        ReportOutOfMemory(cx);
        return nullptr;
    }
    MOZ_ASSERT(!cbuf.dbuf && numStr >= cbuf.sbuf && numStr < cbuf.sbuf + cbuf.sbufSize);

    size_t length = strlen(numStr);
    JSAtom* atom = Atomize(cx, numStr, length);
    if (!atom)
        return nullptr;

    CacheNumber(cx, d, atom);

    return atom;
}

JSFlatString*
js::NumberToString(JSContext* cx, double d)
{
    if (JSString* str = NumberToStringWithBase<CanGC>(cx, d, 10))
        return &str->asFlat();
    return nullptr;
}

JSFlatString*
js::IndexToString(JSContext* cx, uint32_t index)
{
    if (StaticStrings::hasUint(index))
        return cx->staticStrings().getUint(index);

    JSCompartment* c = cx->compartment();
    if (JSFlatString* str = c->dtoaCache.lookup(10, index))
        return str;

    Latin1Char buffer[JSFatInlineString::MAX_LENGTH_LATIN1 + 1];
    RangedPtr<Latin1Char> end(buffer + JSFatInlineString::MAX_LENGTH_LATIN1,
                              buffer, JSFatInlineString::MAX_LENGTH_LATIN1 + 1);
    *end = '\0';
    RangedPtr<Latin1Char> start = BackfillIndexInCharBuffer(index, end);

    mozilla::Range<const Latin1Char> chars(start.get(), end - start);
    JSInlineString* str = NewInlineString<CanGC>(cx, chars);
    if (!str)
        return nullptr;

    c->dtoaCache.cache(10, index, str);
    return str;
}

bool JS_FASTCALL
js::NumberValueToStringBuffer(JSContext* cx, const Value& v, StringBuffer& sb)
{
    /* Convert to C-string. */
    ToCStringBuf cbuf;
    const char* cstr;
    size_t cstrlen;
    if (v.isInt32()) {
        cstr = Int32ToCString(&cbuf, v.toInt32(), &cstrlen);
        MOZ_ASSERT(cstrlen == strlen(cstr));
    } else {
        cstr = NumberToCString(cx, &cbuf, v.toDouble());
        if (!cstr) {
            JS_ReportOutOfMemory(cx);
            return false;
        }
        cstrlen = strlen(cstr);
    }

    /*
     * Inflate to char16_t string.  The input C-string characters are < 127, so
     * even if char16_t units are UTF-8, all chars should map to one char16_t.
     */
    MOZ_ASSERT(!cbuf.dbuf && cstrlen < cbuf.sbufSize);
    return sb.append(cstr, cstrlen);
}

template <typename CharT>
static bool
CharsToNumber(ExclusiveContext* cx, const CharT* chars, size_t length, double* result)
{
    if (length == 1) {
        CharT c = chars[0];
        if ('0' <= c && c <= '9')
            *result = c - '0';
        else if (unicode::IsSpace(c))
            *result = 0.0;
        else
            *result = GenericNaN();
        return true;
    }

    const CharT* end = chars + length;
    const CharT* bp = SkipSpace(chars, end);

    /* ECMA doesn't allow signed non-decimal numbers (bug 273467). */
    if (end - bp >= 2 && bp[0] == '0') {
        int radix = 0;
        if (bp[1] == 'b' || bp[1] == 'B')
            radix = 2;
        else if (bp[1] == 'o' || bp[1] == 'O')
            radix = 8;
        else if (bp[1] == 'x' || bp[1] == 'X')
            radix = 16;

        if (radix != 0) {
            /*
             * It's probably a non-decimal number. Accept if there's at least one digit after
             * the 0b|0o|0x, and if no non-whitespace characters follow all the digits.
             */
            const CharT* endptr;
            double d;
            if (!GetPrefixInteger(cx, bp + 2, end, radix, &endptr, &d) ||
                endptr == bp + 2 ||
                SkipSpace(endptr, end) != end)
            {
                *result = GenericNaN();
            } else {
                *result = d;
            }
            return true;
        }
    }

    /*
     * Note that ECMA doesn't treat a string beginning with a '0' as
     * an octal number here. This works because all such numbers will
     * be interpreted as decimal by js_strtod.  Also, any hex numbers
     * that have made it here (which can only be negative ones) will
     * be treated as 0 without consuming the 'x' by js_strtod.
     */
    const CharT* ep;
    double d;
    if (!js_strtod(cx, bp, end, &ep, &d)) {
        *result = GenericNaN();
        return false;
    }

    if (SkipSpace(ep, end) != end)
        *result = GenericNaN();
    else
        *result = d;

    return true;
}

bool
js::StringToNumber(ExclusiveContext* cx, JSString* str, double* result)
{
    AutoCheckCannotGC nogc;
    JSLinearString* linearStr = str->ensureLinear(cx);
    if (!linearStr)
        return false;

    return linearStr->hasLatin1Chars()
           ? CharsToNumber(cx, linearStr->latin1Chars(nogc), str->length(), result)
           : CharsToNumber(cx, linearStr->twoByteChars(nogc), str->length(), result);
}

bool
js::ToNumberSlow(ExclusiveContext* cx, Value v, double* out)
{
    MOZ_ASSERT(!v.isNumber());
    goto skip_int_double;
    for (;;) {
        if (v.isNumber()) {
            *out = v.toNumber();
            return true;
        }

      skip_int_double:
        if (!v.isObject()) {
            if (v.isString())
                return StringToNumber(cx, v.toString(), out);
            if (v.isBoolean()) {
                *out = v.toBoolean() ? 1.0 : 0.0;
                return true;
            }
            if (v.isNull()) {
                *out = 0.0;
                return true;
            }
            if (v.isSymbol()) {
                if (cx->isJSContext()) {
                    JS_ReportErrorNumber(cx->asJSContext(), GetErrorMessage, nullptr,
                                         JSMSG_SYMBOL_TO_NUMBER);
                }
                return false;
            }

            MOZ_ASSERT(v.isUndefined());
            *out = GenericNaN();
            return true;
        }

        if (!cx->isJSContext())
            return false;

        RootedValue v2(cx, v);
        if (!ToPrimitive(cx->asJSContext(), JSTYPE_NUMBER, &v2))
            return false;
        v = v2;
        if (v.isObject())
            break;
    }

    *out = GenericNaN();
    return true;
}

JS_PUBLIC_API(bool)
js::ToNumberSlow(JSContext* cx, Value v, double* out)
{
    return ToNumberSlow(static_cast<ExclusiveContext*>(cx), v, out);
}

/*
 * Convert a value to an int8_t, according to the WebIDL rules for byte
 * conversion. Return converted value in *out on success, false on failure.
 */
JS_PUBLIC_API(bool)
js::ToInt8Slow(JSContext *cx, const HandleValue v, int8_t *out)
{
    MOZ_ASSERT(!v.isInt32());
    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumberSlow(cx, v, &d))
            return false;
    }
    *out = ToInt8(d);
    return true;
}

/*
 * Convert a value to an int16_t, according to the WebIDL rules for short
 * conversion. Return converted value in *out on success, false on failure.
 */
JS_PUBLIC_API(bool)
js::ToInt16Slow(JSContext *cx, const HandleValue v, int16_t *out)
{
    MOZ_ASSERT(!v.isInt32());
    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumberSlow(cx, v, &d))
            return false;
    }
    *out = ToInt16(d);
    return true;
}

/*
 * Convert a value to an int64_t, according to the WebIDL rules for long long
 * conversion. Return converted value in *out on success, false on failure.
 */
JS_PUBLIC_API(bool)
js::ToInt64Slow(JSContext* cx, const HandleValue v, int64_t* out)
{
    MOZ_ASSERT(!v.isInt32());
    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumberSlow(cx, v, &d))
            return false;
    }
    *out = ToInt64(d);
    return true;
}

/*
 * Convert a value to an uint64_t, according to the WebIDL rules for unsigned long long
 * conversion. Return converted value in *out on success, false on failure.
 */
JS_PUBLIC_API(bool)
js::ToUint64Slow(JSContext* cx, const HandleValue v, uint64_t* out)
{
    MOZ_ASSERT(!v.isInt32());
    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumberSlow(cx, v, &d))
            return false;
    }
    *out = ToUint64(d);
    return true;
}

JS_PUBLIC_API(bool)
js::ToInt32Slow(JSContext* cx, const HandleValue v, int32_t* out)
{
    MOZ_ASSERT(!v.isInt32());
    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumberSlow(cx, v, &d))
            return false;
    }
    *out = ToInt32(d);
    return true;
}

JS_PUBLIC_API(bool)
js::ToUint32Slow(JSContext* cx, const HandleValue v, uint32_t* out)
{
    MOZ_ASSERT(!v.isInt32());
    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumberSlow(cx, v, &d))
            return false;
    }
    *out = ToUint32(d);
    return true;
}

JS_PUBLIC_API(bool)
js::ToUint16Slow(JSContext* cx, const HandleValue v, uint16_t* out)
{
    MOZ_ASSERT(!v.isInt32());
    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else if (!ToNumberSlow(cx, v, &d)) {
        return false;
    }

    if (d == 0 || !mozilla::IsFinite(d)) {
        *out = 0;
        return true;
    }

    uint16_t u = (uint16_t) d;
    if ((double)u == d) {
        *out = u;
        return true;
    }

    bool neg = (d < 0);
    d = floor(neg ? -d : d);
    d = neg ? -d : d;
    unsigned m = JS_BIT(16);
    d = fmod(d, (double) m);
    if (d < 0)
        d += m;
    *out = (uint16_t) d;
    return true;
}

template<typename T>
bool
js::ToLengthClamped(T* cx, HandleValue v, uint32_t* out, bool* overflow)
{
    if (v.isInt32()) {
        int32_t i = v.toInt32();
        *out = i < 0 ? 0 : i;
        return true;
    }
    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumber(cx, v, &d)) {
            *overflow = false;
            return false;
        }
    }
    d = JS::ToInteger(d);
    if (d <= 0.0) {
        *out = 0;
        return true;
    }
    if (d >= (double)0xFFFFFFFEU) {
        *overflow = true;
        return false;
    }
    *out = (uint32_t)d;
    return true;
}

template bool
js::ToLengthClamped<JSContext>(JSContext*, HandleValue, uint32_t*, bool*);
template bool
js::ToLengthClamped<ExclusiveContext>(ExclusiveContext*, HandleValue, uint32_t*, bool*);

template <typename CharT>
bool
js_strtod(ExclusiveContext* cx, const CharT* begin, const CharT* end, const CharT** dEnd,
          double* d)
{
    const CharT* s = SkipSpace(begin, end);
    size_t length = end - s;

    Vector<char, 32> chars(cx);
    if (!chars.growByUninitialized(length + 1))
        return false;

    size_t i = 0;
    for (; i < length; i++) {
        char16_t c = s[i];
        if (c >> 8)
            break;
        chars[i] = char(c);
    }
    chars[i] = 0;

    /* Try to parse +Infinity, -Infinity or Infinity. */
    {
        char* afterSign = chars.begin();
        bool negative = (*afterSign == '-');
        if (negative || *afterSign == '+')
            afterSign++;

        if (*afterSign == 'I' && !strncmp(afterSign, "Infinity", 8)) {
            *d = negative ? NegativeInfinity<double>() : PositiveInfinity<double>();
            *dEnd = s + (afterSign - chars.begin()) + 8;
            return true;
        }
    }

    /* Everything else. */
    int err;
    char* ep;
    *d = js_strtod_harder(cx->dtoaState(), chars.begin(), &ep, &err);

    MOZ_ASSERT(ep >= chars.begin());

    if (ep == chars.begin())
        *dEnd = begin;
    else
        *dEnd = s + (ep - chars.begin());

    return true;
}

template bool
js_strtod(ExclusiveContext* cx, const char16_t* begin, const char16_t* end, const char16_t** dEnd,
          double* d);

template bool
js_strtod(ExclusiveContext* cx, const Latin1Char* begin, const Latin1Char* end,
          const Latin1Char** dEnd, double* d);
