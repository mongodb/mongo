/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsstr.h"

#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Range.h"
#include "mozilla/TypeTraits.h"
#include "mozilla/UniquePtr.h"

#include <ctype.h>
#include <string.h>

#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsgc.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jstypes.h"
#include "jsutil.h"

#include "builtin/Intl.h"
#include "builtin/RegExp.h"
#include "jit/InlinableNatives.h"
#include "js/Conversions.h"
#if ENABLE_INTL_API
#include "unicode/unorm.h"
#endif
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/Opcodes.h"
#include "vm/Printer.h"
#include "vm/RegExpObject.h"
#include "vm/RegExpStatics.h"
#include "vm/ScopeObject.h"
#include "vm/StringBuffer.h"

#include "vm/Interpreter-inl.h"
#include "vm/String-inl.h"
#include "vm/StringObject-inl.h"
#include "vm/TypeInference-inl.h"

using namespace js;
using namespace js::gc;
using namespace js::unicode;

using JS::Symbol;
using JS::SymbolCode;
using JS::ToInt32;
using JS::ToUint32;

using mozilla::AssertedCast;
using mozilla::CheckedInt;
using mozilla::IsNaN;
using mozilla::IsNegativeZero;
using mozilla::IsSame;
using mozilla::Move;
using mozilla::PodCopy;
using mozilla::PodEqual;
using mozilla::RangedPtr;
using mozilla::UniquePtr;

using JS::AutoCheckCannotGC;

static JSLinearString*
ArgToRootedString(JSContext* cx, const CallArgs& args, unsigned argno)
{
    if (argno >= args.length())
        return cx->names().undefined;

    JSString* str = ToString<CanGC>(cx, args[argno]);
    if (!str)
        return nullptr;

    args[argno].setString(str);
    return str->ensureLinear(cx);
}

/*
 * Forward declarations for URI encode/decode and helper routines
 */
static bool
str_decodeURI(JSContext* cx, unsigned argc, Value* vp);

static bool
str_decodeURI_Component(JSContext* cx, unsigned argc, Value* vp);

static bool
str_encodeURI(JSContext* cx, unsigned argc, Value* vp);

static bool
str_encodeURI_Component(JSContext* cx, unsigned argc, Value* vp);

/*
 * Global string methods
 */


/* ES5 B.2.1 */
template <typename CharT>
static Latin1Char*
Escape(JSContext* cx, const CharT* chars, uint32_t length, uint32_t* newLengthOut)
{
    static const uint8_t shouldPassThrough[128] = {
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
         0,0,0,0,0,0,0,0,0,0,1,1,0,1,1,1,       /*    !"#$%&'()*+,-./  */
         1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,       /*   0123456789:;<=>?  */
         1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,       /*   @ABCDEFGHIJKLMNO  */
         1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,1,       /*   PQRSTUVWXYZ[\]^_  */
         0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,       /*   `abcdefghijklmno  */
         1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,       /*   pqrstuvwxyz{\}~  DEL */
    };

    /* Take a first pass and see how big the result string will need to be. */
    uint32_t newLength = length;
    for (size_t i = 0; i < length; i++) {
        char16_t ch = chars[i];
        if (ch < 128 && shouldPassThrough[ch])
            continue;

        /* The character will be encoded as %XX or %uXXXX. */
        newLength += (ch < 256) ? 2 : 5;

        /*
         * newlength is incremented by at most 5 on each iteration, so worst
         * case newlength == length * 6. This can't overflow.
         */
        static_assert(JSString::MAX_LENGTH < UINT32_MAX / 6,
                      "newlength must not overflow");
    }

    Latin1Char* newChars = cx->pod_malloc<Latin1Char>(newLength + 1);
    if (!newChars)
        return nullptr;

    static const char digits[] = "0123456789ABCDEF";

    size_t i, ni;
    for (i = 0, ni = 0; i < length; i++) {
        char16_t ch = chars[i];
        if (ch < 128 && shouldPassThrough[ch]) {
            newChars[ni++] = ch;
        } else if (ch < 256) {
            newChars[ni++] = '%';
            newChars[ni++] = digits[ch >> 4];
            newChars[ni++] = digits[ch & 0xF];
        } else {
            newChars[ni++] = '%';
            newChars[ni++] = 'u';
            newChars[ni++] = digits[ch >> 12];
            newChars[ni++] = digits[(ch & 0xF00) >> 8];
            newChars[ni++] = digits[(ch & 0xF0) >> 4];
            newChars[ni++] = digits[ch & 0xF];
        }
    }
    MOZ_ASSERT(ni == newLength);
    newChars[newLength] = 0;

    *newLengthOut = newLength;
    return newChars;
}

static bool
str_escape(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSLinearString* str = ArgToRootedString(cx, args, 0);
    if (!str)
        return false;

    ScopedJSFreePtr<Latin1Char> newChars;
    uint32_t newLength = 0;  // initialize to silence GCC warning
    if (str->hasLatin1Chars()) {
        AutoCheckCannotGC nogc;
        newChars = Escape(cx, str->latin1Chars(nogc), str->length(), &newLength);
    } else {
        AutoCheckCannotGC nogc;
        newChars = Escape(cx, str->twoByteChars(nogc), str->length(), &newLength);
    }

    if (!newChars)
        return false;

    JSString* res = NewString<CanGC>(cx, newChars.get(), newLength);
    if (!res)
        return false;

    newChars.forget();
    args.rval().setString(res);
    return true;
}

template <typename CharT>
static inline bool
Unhex4(const RangedPtr<const CharT> chars, char16_t* result)
{
    char16_t a = chars[0],
             b = chars[1],
             c = chars[2],
             d = chars[3];

    if (!(JS7_ISHEX(a) && JS7_ISHEX(b) && JS7_ISHEX(c) && JS7_ISHEX(d)))
        return false;

    *result = (((((JS7_UNHEX(a) << 4) + JS7_UNHEX(b)) << 4) + JS7_UNHEX(c)) << 4) + JS7_UNHEX(d);
    return true;
}

template <typename CharT>
static inline bool
Unhex2(const RangedPtr<const CharT> chars, char16_t* result)
{
    char16_t a = chars[0],
             b = chars[1];

    if (!(JS7_ISHEX(a) && JS7_ISHEX(b)))
        return false;

    *result = (JS7_UNHEX(a) << 4) + JS7_UNHEX(b);
    return true;
}

template <typename CharT>
static bool
Unescape(StringBuffer& sb, const mozilla::Range<const CharT> chars)
{
    /*
     * NB: use signed integers for length/index to allow simple length
     * comparisons without unsigned-underflow hazards.
     */
    static_assert(JSString::MAX_LENGTH <= INT_MAX, "String length must fit in a signed integer");
    int length = AssertedCast<int>(chars.length());

    /*
     * Note that the spec algorithm has been optimized to avoid building
     * a string in the case where no escapes are present.
     */

    /* Step 4. */
    int k = 0;
    bool building = false;

    /* Step 5. */
    while (k < length) {
        /* Step 6. */
        char16_t c = chars[k];

        /* Step 7. */
        if (c != '%')
            goto step_18;

        /* Step 8. */
        if (k > length - 6)
            goto step_14;

        /* Step 9. */
        if (chars[k + 1] != 'u')
            goto step_14;

#define ENSURE_BUILDING                                      \
        do {                                                 \
            if (!building) {                                 \
                building = true;                             \
                if (!sb.reserve(length))                     \
                    return false;                            \
                sb.infallibleAppend(chars.start().get(), k); \
            }                                                \
        } while(false);

        /* Step 10-13. */
        if (Unhex4(chars.start() + k + 2, &c)) {
            ENSURE_BUILDING;
            k += 5;
            goto step_18;
        }

      step_14:
        /* Step 14. */
        if (k > length - 3)
            goto step_18;

        /* Step 15-17. */
        if (Unhex2(chars.start() + k + 1, &c)) {
            ENSURE_BUILDING;
            k += 2;
        }

      step_18:
        if (building && !sb.append(c))
            return false;

        /* Step 19. */
        k += 1;
    }

    return true;
#undef ENSURE_BUILDING
}

/* ES5 B.2.2 */
static bool
str_unescape(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedLinearString str(cx, ArgToRootedString(cx, args, 0));
    if (!str)
        return false;

    /* Step 3. */
    StringBuffer sb(cx);
    if (str->hasTwoByteChars() && !sb.ensureTwoByteChars())
        return false;

    if (str->hasLatin1Chars()) {
        AutoCheckCannotGC nogc;
        if (!Unescape(sb, str->latin1Range(nogc)))
            return false;
    } else {
        AutoCheckCannotGC nogc;
        if (!Unescape(sb, str->twoByteRange(nogc)))
            return false;
    }

    JSLinearString* result;
    if (!sb.empty()) {
        result = sb.finishString();
        if (!result)
            return false;
    } else {
        result = str;
    }

    args.rval().setString(result);
    return true;
}

#if JS_HAS_UNEVAL
static bool
str_uneval(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JSString* str = ValueToSource(cx, args.get(0));
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}
#endif

static const JSFunctionSpec string_functions[] = {
    JS_FN(js_escape_str,             str_escape,                1, JSPROP_RESOLVING),
    JS_FN(js_unescape_str,           str_unescape,              1, JSPROP_RESOLVING),
#if JS_HAS_UNEVAL
    JS_FN(js_uneval_str,             str_uneval,                1, JSPROP_RESOLVING),
#endif
    JS_FN(js_decodeURI_str,          str_decodeURI,             1, JSPROP_RESOLVING),
    JS_FN(js_encodeURI_str,          str_encodeURI,             1, JSPROP_RESOLVING),
    JS_FN(js_decodeURIComponent_str, str_decodeURI_Component,   1, JSPROP_RESOLVING),
    JS_FN(js_encodeURIComponent_str, str_encodeURI_Component,   1, JSPROP_RESOLVING),

    JS_FS_END
};

static const unsigned STRING_ELEMENT_ATTRS = JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT;

static bool
str_enumerate(JSContext* cx, HandleObject obj)
{
    RootedString str(cx, obj->as<StringObject>().unbox());
    RootedValue value(cx);
    for (size_t i = 0, length = str->length(); i < length; i++) {
        JSString* str1 = NewDependentString(cx, str, i, 1);
        if (!str1)
            return false;
        value.setString(str1);
        if (!DefineElement(cx, obj, i, value, nullptr, nullptr,
                           STRING_ELEMENT_ATTRS | JSPROP_RESOLVING))
        {
            return false;
        }
    }

    return true;
}

static bool
str_mayResolve(const JSAtomState&, jsid id, JSObject*)
{
    // str_resolve ignores non-integer ids.
    return JSID_IS_INT(id);
}

static bool
str_resolve(JSContext* cx, HandleObject obj, HandleId id, bool* resolvedp)
{
    if (!JSID_IS_INT(id))
        return true;

    RootedString str(cx, obj->as<StringObject>().unbox());

    int32_t slot = JSID_TO_INT(id);
    if ((size_t)slot < str->length()) {
        JSString* str1 = cx->staticStrings().getUnitStringForElement(cx, str, size_t(slot));
        if (!str1)
            return false;
        RootedValue value(cx, StringValue(str1));
        if (!DefineElement(cx, obj, uint32_t(slot), value, nullptr, nullptr,
                           STRING_ELEMENT_ATTRS | JSPROP_RESOLVING))
        {
            return false;
        }
        *resolvedp = true;
    }
    return true;
}

const Class StringObject::class_ = {
    js_String_str,
    JSCLASS_HAS_RESERVED_SLOTS(StringObject::RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_String),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    str_enumerate,
    str_resolve,
    str_mayResolve
};

/*
 * Returns a JSString * for the |this| value associated with 'call', or throws
 * a TypeError if |this| is null or undefined.  This algorithm is the same as
 * calling CheckObjectCoercible(this), then returning ToString(this), as all
 * String.prototype.* methods do (other than toString and valueOf).
 */
static MOZ_ALWAYS_INLINE JSString*
ThisToStringForStringProto(JSContext* cx, CallReceiver call)
{
    JS_CHECK_RECURSION(cx, return nullptr);

    if (call.thisv().isString())
        return call.thisv().toString();

    if (call.thisv().isObject()) {
        RootedObject obj(cx, &call.thisv().toObject());
        if (obj->is<StringObject>()) {
            StringObject* nobj = &obj->as<StringObject>();
            Rooted<jsid> id(cx, NameToId(cx->names().toString));
            if (ClassMethodIsNative(cx, nobj, &StringObject::class_, id, str_toString)) {
                JSString* str = nobj->unbox();
                call.setThis(StringValue(str));
                return str;
            }
        }
    } else if (call.thisv().isNullOrUndefined()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                             call.thisv().isNull() ? "null" : "undefined", "object");
        return nullptr;
    }

    JSString* str = ToStringSlow<CanGC>(cx, call.thisv());
    if (!str)
        return nullptr;

    call.setThis(StringValue(str));
    return str;
}

MOZ_ALWAYS_INLINE bool
IsString(HandleValue v)
{
    return v.isString() || (v.isObject() && v.toObject().is<StringObject>());
}

#if JS_HAS_TOSOURCE

MOZ_ALWAYS_INLINE bool
str_toSource_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsString(args.thisv()));

    Rooted<JSString*> str(cx, ToString<CanGC>(cx, args.thisv()));
    if (!str)
        return false;

    str = QuoteString(cx, str, '"');
    if (!str)
        return false;

    StringBuffer sb(cx);
    if (!sb.append("(new String(") || !sb.append(str) || !sb.append("))"))
        return false;

    str = sb.finishString();
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static bool
str_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsString, str_toSource_impl>(cx, args);
}

#endif /* JS_HAS_TOSOURCE */

MOZ_ALWAYS_INLINE bool
str_toString_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsString(args.thisv()));

    args.rval().setString(args.thisv().isString()
                              ? args.thisv().toString()
                              : args.thisv().toObject().as<StringObject>().unbox());
    return true;
}

bool
js::str_toString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsString, str_toString_impl>(cx, args);
}

/*
 * Java-like string native methods.
 */

JSString*
js::SubstringKernel(JSContext* cx, HandleString str, int32_t beginInt, int32_t lengthInt)
{
    MOZ_ASSERT(0 <= beginInt);
    MOZ_ASSERT(0 <= lengthInt);
    MOZ_ASSERT(uint32_t(beginInt) <= str->length());
    MOZ_ASSERT(uint32_t(lengthInt) <= str->length() - beginInt);

    uint32_t begin = beginInt;
    uint32_t len = lengthInt;

    /*
     * Optimization for one level deep ropes.
     * This is common for the following pattern:
     *
     * while() {
     *   text = text.substr(0, x) + "bla" + text.substr(x)
     *   test.charCodeAt(x + 1)
     * }
     */
    if (str->isRope()) {
        JSRope* rope = &str->asRope();

        /* Substring is totally in leftChild of rope. */
        if (begin + len <= rope->leftChild()->length())
            return NewDependentString(cx, rope->leftChild(), begin, len);

        /* Substring is totally in rightChild of rope. */
        if (begin >= rope->leftChild()->length()) {
            begin -= rope->leftChild()->length();
            return NewDependentString(cx, rope->rightChild(), begin, len);
        }

        /*
         * Requested substring is partly in the left and partly in right child.
         * Create a rope of substrings for both childs.
         */
        MOZ_ASSERT(begin < rope->leftChild()->length() &&
                   begin + len > rope->leftChild()->length());

        size_t lhsLength = rope->leftChild()->length() - begin;
        size_t rhsLength = begin + len - rope->leftChild()->length();

        Rooted<JSRope*> ropeRoot(cx, rope);
        RootedString lhs(cx, NewDependentString(cx, ropeRoot->leftChild(), begin, lhsLength));
        if (!lhs)
            return nullptr;

        RootedString rhs(cx, NewDependentString(cx, ropeRoot->rightChild(), 0, rhsLength));
        if (!rhs)
            return nullptr;

        return JSRope::new_<CanGC>(cx, lhs, rhs, len);
    }

    return NewDependentString(cx, str, begin, len);
}

template <typename CharT>
static JSString*
ToLowerCase(JSContext* cx, JSLinearString* str)
{
    // Unlike toUpperCase, toLowerCase has the nice invariant that if the input
    // is a Latin1 string, the output is also a Latin1 string.
    UniquePtr<CharT[], JS::FreePolicy> newChars;
    size_t length = str->length();
    {
        AutoCheckCannotGC nogc;
        const CharT* chars = str->chars<CharT>(nogc);

        // Look for the first upper case character.
        size_t i = 0;
        for (; i < length; i++) {
            char16_t c = chars[i];
            if (unicode::CanLowerCase(c))
                break;
        }

        // If all characters are lower case, return the input string.
        if (i == length)
            return str;

        newChars = cx->make_pod_array<CharT>(length + 1);
        if (!newChars)
            return nullptr;

        PodCopy(newChars.get(), chars, i);

        for (; i < length; i++) {
            char16_t c = unicode::ToLowerCase(chars[i]);
            MOZ_ASSERT_IF((IsSame<CharT, Latin1Char>::value), c <= JSString::MAX_LATIN1_CHAR);
            newChars[i] = c;
        }

        newChars[length] = 0;
    }

    JSString* res = NewStringDontDeflate<CanGC>(cx, newChars.get(), length);
    if (!res)
        return nullptr;

    newChars.release();
    return res;
}

static inline bool
ToLowerCaseHelper(JSContext* cx, CallReceiver call)
{
    RootedString str(cx, ThisToStringForStringProto(cx, call));
    if (!str)
        return false;

    JSLinearString* linear = str->ensureLinear(cx);
    if (!linear)
        return false;

    if (linear->hasLatin1Chars())
        str = ToLowerCase<Latin1Char>(cx, linear);
    else
        str = ToLowerCase<char16_t>(cx, linear);
    if (!str)
        return false;

    call.rval().setString(str);
    return true;
}

bool
js::str_toLowerCase(JSContext* cx, unsigned argc, Value* vp)
{
    return ToLowerCaseHelper(cx, CallArgsFromVp(argc, vp));
}

static bool
str_toLocaleLowerCase(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /*
     * Forcefully ignore the first (or any) argument and return toLowerCase(),
     * ECMA has reserved that argument, presumably for defining the locale.
     */
    if (cx->runtime()->localeCallbacks && cx->runtime()->localeCallbacks->localeToLowerCase) {
        RootedString str(cx, ThisToStringForStringProto(cx, args));
        if (!str)
            return false;

        RootedValue result(cx);
        if (!cx->runtime()->localeCallbacks->localeToLowerCase(cx, str, &result))
            return false;

        args.rval().set(result);
        return true;
    }

    return ToLowerCaseHelper(cx, args);
}

template <typename DestChar, typename SrcChar>
static void
ToUpperCaseImpl(DestChar* destChars, const SrcChar* srcChars, size_t firstLowerCase, size_t length)
{
    MOZ_ASSERT(firstLowerCase < length);

    for (size_t i = 0; i < firstLowerCase; i++)
        destChars[i] = srcChars[i];

    for (size_t i = firstLowerCase; i < length; i++) {
        char16_t c = unicode::ToUpperCase(srcChars[i]);
        MOZ_ASSERT_IF((IsSame<DestChar, Latin1Char>::value), c <= JSString::MAX_LATIN1_CHAR);
        destChars[i] = c;
    }

    destChars[length] = '\0';
}

template <typename CharT>
static JSString*
ToUpperCase(JSContext* cx, JSLinearString* str)
{
    typedef UniquePtr<Latin1Char[], JS::FreePolicy> Latin1CharPtr;
    typedef UniquePtr<char16_t[], JS::FreePolicy> TwoByteCharPtr;

    mozilla::MaybeOneOf<Latin1CharPtr, TwoByteCharPtr> newChars;
    size_t length = str->length();
    {
        AutoCheckCannotGC nogc;
        const CharT* chars = str->chars<CharT>(nogc);

        // Look for the first lower case character.
        size_t i = 0;
        for (; i < length; i++) {
            char16_t c = chars[i];
            if (unicode::CanUpperCase(c))
                break;
        }

        // If all characters are upper case, return the input string.
        if (i == length)
            return str;

        // If the string is Latin1, check if it contains the MICRO SIGN (0xb5)
        // or SMALL LETTER Y WITH DIAERESIS (0xff) character. The corresponding
        // upper case characters are not in the Latin1 range.
        bool resultIsLatin1;
        if (IsSame<CharT, Latin1Char>::value) {
            resultIsLatin1 = true;
            for (size_t j = i; j < length; j++) {
                Latin1Char c = chars[j];
                if (c == 0xb5 || c == 0xff) {
                    MOZ_ASSERT(unicode::ToUpperCase(c) > JSString::MAX_LATIN1_CHAR);
                    resultIsLatin1 = false;
                    break;
                } else {
                    MOZ_ASSERT(unicode::ToUpperCase(c) <= JSString::MAX_LATIN1_CHAR);
                }
            }
        } else {
            resultIsLatin1 = false;
        }

        if (resultIsLatin1) {
            Latin1CharPtr buf = cx->make_pod_array<Latin1Char>(length + 1);
            if (!buf)
                return nullptr;

            ToUpperCaseImpl(buf.get(), chars, i, length);
            newChars.construct<Latin1CharPtr>(Move(buf));
        } else {
            TwoByteCharPtr buf = cx->make_pod_array<char16_t>(length + 1);
            if (!buf)
                return nullptr;

            ToUpperCaseImpl(buf.get(), chars, i, length);
            newChars.construct<TwoByteCharPtr>(Move(buf));
        }
    }

    JSString* res;
    if (newChars.constructed<Latin1CharPtr>()) {
        res = NewStringDontDeflate<CanGC>(cx, newChars.ref<Latin1CharPtr>().get(), length);
        if (!res)
            return nullptr;

        newChars.ref<Latin1CharPtr>().release();
    } else {
        res = NewStringDontDeflate<CanGC>(cx, newChars.ref<TwoByteCharPtr>().get(), length);
        if (!res)
            return nullptr;

        newChars.ref<TwoByteCharPtr>().release();
    }

    return res;
}

static bool
ToUpperCaseHelper(JSContext* cx, CallReceiver call)
{
    RootedString str(cx, ThisToStringForStringProto(cx, call));
    if (!str)
        return false;

    JSLinearString* linear = str->ensureLinear(cx);
    if (!linear)
        return false;

    if (linear->hasLatin1Chars())
        str = ToUpperCase<Latin1Char>(cx, linear);
    else
        str = ToUpperCase<char16_t>(cx, linear);
    if (!str)
        return false;

    call.rval().setString(str);
    return true;
}

bool
js::str_toUpperCase(JSContext* cx, unsigned argc, Value* vp)
{
    return ToUpperCaseHelper(cx, CallArgsFromVp(argc, vp));
}

static bool
str_toLocaleUpperCase(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /*
     * Forcefully ignore the first (or any) argument and return toUpperCase(),
     * ECMA has reserved that argument, presumably for defining the locale.
     */
    if (cx->runtime()->localeCallbacks && cx->runtime()->localeCallbacks->localeToUpperCase) {
        RootedString str(cx, ThisToStringForStringProto(cx, args));
        if (!str)
            return false;

        RootedValue result(cx);
        if (!cx->runtime()->localeCallbacks->localeToUpperCase(cx, str, &result))
            return false;

        args.rval().set(result);
        return true;
    }

    return ToUpperCaseHelper(cx, args);
}

#if !EXPOSE_INTL_API
static bool
str_localeCompare(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedString str(cx, ThisToStringForStringProto(cx, args));
    if (!str)
        return false;

    RootedString thatStr(cx, ToString<CanGC>(cx, args.get(0)));
    if (!thatStr)
        return false;

    if (cx->runtime()->localeCallbacks && cx->runtime()->localeCallbacks->localeCompare) {
        RootedValue result(cx);
        if (!cx->runtime()->localeCallbacks->localeCompare(cx, str, thatStr, &result))
            return false;

        args.rval().set(result);
        return true;
    }

    int32_t result;
    if (!CompareStrings(cx, str, thatStr, &result))
        return false;

    args.rval().setInt32(result);
    return true;
}
#endif

#if EXPOSE_INTL_API
/* ES6 20140210 draft 21.1.3.12. */
static bool
str_normalize(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1-3.
    RootedString str(cx, ThisToStringForStringProto(cx, args));
    if (!str)
        return false;

    // Step 4.
    UNormalizationMode form;
    if (!args.hasDefined(0)) {
        form = UNORM_NFC;
    } else {
        // Steps 5-6.
        RootedLinearString formStr(cx, ArgToRootedString(cx, args, 0));
        if (!formStr)
            return false;

        // Step 7.
        if (EqualStrings(formStr, cx->names().NFC)) {
            form = UNORM_NFC;
        } else if (EqualStrings(formStr, cx->names().NFD)) {
            form = UNORM_NFD;
        } else if (EqualStrings(formStr, cx->names().NFKC)) {
            form = UNORM_NFKC;
        } else if (EqualStrings(formStr, cx->names().NFKD)) {
            form = UNORM_NFKD;
        } else {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                 JSMSG_INVALID_NORMALIZE_FORM);
            return false;
        }
    }

    // Step 8.
    AutoStableStringChars stableChars(cx);
    if (!str->ensureFlat(cx) || !stableChars.initTwoByte(cx, str))
        return false;

    static const size_t INLINE_CAPACITY = 32;

    const UChar* srcChars = Char16ToUChar(stableChars.twoByteRange().start().get());
    int32_t srcLen = AssertedCast<int32_t>(str->length());
    Vector<char16_t, INLINE_CAPACITY> chars(cx);
    if (!chars.resize(INLINE_CAPACITY))
        return false;

    UErrorCode status = U_ZERO_ERROR;
    int32_t size = unorm_normalize(srcChars, srcLen, form, 0,
                                   Char16ToUChar(chars.begin()), INLINE_CAPACITY,
                                   &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        if (!chars.resize(size))
            return false;
        status = U_ZERO_ERROR;
#ifdef DEBUG
        int32_t finalSize =
#endif
        unorm_normalize(srcChars, srcLen, form, 0,
                        Char16ToUChar(chars.begin()), size,
                        &status);
        MOZ_ASSERT(size == finalSize || U_FAILURE(status), "unorm_normalize behaved inconsistently");
    }
    if (U_FAILURE(status))
        return false;

    JSString* ns = NewStringCopyN<CanGC>(cx, chars.begin(), size);
    if (!ns)
        return false;

    // Step 9.
    args.rval().setString(ns);
    return true;
}
#endif

bool
js::str_charAt(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedString str(cx);
    size_t i;
    if (args.thisv().isString() && args.length() != 0 && args[0].isInt32()) {
        str = args.thisv().toString();
        i = size_t(args[0].toInt32());
        if (i >= str->length())
            goto out_of_range;
    } else {
        str = ThisToStringForStringProto(cx, args);
        if (!str)
            return false;

        double d = 0.0;
        if (args.length() > 0 && !ToInteger(cx, args[0], &d))
            return false;

        if (d < 0 || str->length() <= d)
            goto out_of_range;
        i = size_t(d);
    }

    str = cx->staticStrings().getUnitStringForElement(cx, str, i);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;

  out_of_range:
    args.rval().setString(cx->runtime()->emptyString);
    return true;
}

bool
js::str_charCodeAt_impl(JSContext* cx, HandleString string, HandleValue index, MutableHandleValue res)
{
    RootedString str(cx);
    size_t i;
    if (index.isInt32()) {
        i = index.toInt32();
        if (i >= string->length())
            goto out_of_range;
    } else {
        double d = 0.0;
        if (!ToInteger(cx, index, &d))
            return false;
        // check whether d is negative as size_t is unsigned
        if (d < 0 || string->length() <= d )
            goto out_of_range;
        i = size_t(d);
    }
    char16_t c;
    if (!string->getChar(cx, i , &c))
        return false;
    res.setInt32(c);
    return true;

out_of_range:
    res.setNaN();
    return true;
}

bool
js::str_charCodeAt(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedString str(cx);
    RootedValue index(cx);
    if (args.thisv().isString()) {
        str = args.thisv().toString();
    } else {
        str = ThisToStringForStringProto(cx, args);
        if (!str)
            return false;
    }
    if (args.length() != 0)
        index = args[0];
    else
        index.setInt32(0);

    return js::str_charCodeAt_impl(cx, str, index, args.rval());
}

/*
 * Boyer-Moore-Horspool superlinear search for pat:patlen in text:textlen.
 * The patlen argument must be positive and no greater than sBMHPatLenMax.
 *
 * Return the index of pat in text, or -1 if not found.
 */
static const uint32_t sBMHCharSetSize = 256; /* ISO-Latin-1 */
static const uint32_t sBMHPatLenMax   = 255; /* skip table element is uint8_t */
static const int      sBMHBadPattern  = -2;  /* return value if pat is not ISO-Latin-1 */

template <typename TextChar, typename PatChar>
static int
BoyerMooreHorspool(const TextChar* text, uint32_t textLen, const PatChar* pat, uint32_t patLen)
{
    MOZ_ASSERT(0 < patLen && patLen <= sBMHPatLenMax);

    uint8_t skip[sBMHCharSetSize];
    for (uint32_t i = 0; i < sBMHCharSetSize; i++)
        skip[i] = uint8_t(patLen);

    uint32_t patLast = patLen - 1;
    for (uint32_t i = 0; i < patLast; i++) {
        char16_t c = pat[i];
        if (c >= sBMHCharSetSize)
            return sBMHBadPattern;
        skip[c] = uint8_t(patLast - i);
    }

    for (uint32_t k = patLast; k < textLen; ) {
        for (uint32_t i = k, j = patLast; ; i--, j--) {
            if (text[i] != pat[j])
                break;
            if (j == 0)
                return static_cast<int>(i);  /* safe: max string size */
        }

        char16_t c = text[k];
        k += (c >= sBMHCharSetSize) ? patLen : skip[c];
    }
    return -1;
}

template <typename TextChar, typename PatChar>
struct MemCmp {
    typedef uint32_t Extent;
    static MOZ_ALWAYS_INLINE Extent computeExtent(const PatChar*, uint32_t patLen) {
        return (patLen - 1) * sizeof(PatChar);
    }
    static MOZ_ALWAYS_INLINE bool match(const PatChar* p, const TextChar* t, Extent extent) {
        MOZ_ASSERT(sizeof(TextChar) == sizeof(PatChar));
        return memcmp(p, t, extent) == 0;
    }
};

template <typename TextChar, typename PatChar>
struct ManualCmp {
    typedef const PatChar* Extent;
    static MOZ_ALWAYS_INLINE Extent computeExtent(const PatChar* pat, uint32_t patLen) {
        return pat + patLen;
    }
    static MOZ_ALWAYS_INLINE bool match(const PatChar* p, const TextChar* t, Extent extent) {
        for (; p != extent; ++p, ++t) {
            if (*p != *t)
                return false;
        }
        return true;
    }
};

template <typename TextChar, typename PatChar>
static const TextChar*
FirstCharMatcherUnrolled(const TextChar* text, uint32_t n, const PatChar pat)
{
    const TextChar* textend = text + n;
    const TextChar* t = text;

    switch ((textend - t) & 7) {
        case 0: if (*t++ == pat) return t - 1;
        case 7: if (*t++ == pat) return t - 1;
        case 6: if (*t++ == pat) return t - 1;
        case 5: if (*t++ == pat) return t - 1;
        case 4: if (*t++ == pat) return t - 1;
        case 3: if (*t++ == pat) return t - 1;
        case 2: if (*t++ == pat) return t - 1;
        case 1: if (*t++ == pat) return t - 1;
    }
    while (textend != t) {
        if (t[0] == pat) return t;
        if (t[1] == pat) return t + 1;
        if (t[2] == pat) return t + 2;
        if (t[3] == pat) return t + 3;
        if (t[4] == pat) return t + 4;
        if (t[5] == pat) return t + 5;
        if (t[6] == pat) return t + 6;
        if (t[7] == pat) return t + 7;
        t += 8;
    }
    return nullptr;
}

static const char*
FirstCharMatcher8bit(const char* text, uint32_t n, const char pat)
{
#if  defined(__clang__)
    return FirstCharMatcherUnrolled<char, char>(text, n, pat);
#else
    return reinterpret_cast<const char*>(memchr(text, pat, n));
#endif
}

static const char16_t*
FirstCharMatcher16bit(const char16_t* text, uint32_t n, const char16_t pat)
{
#if defined(XP_DARWIN) || defined(XP_WIN)
    /*
     * Performance of memchr is horrible in OSX. Windows is better,
     * but it is still better to use UnrolledMatcher.
     */
    return FirstCharMatcherUnrolled<char16_t, char16_t>(text, n, pat);
#else
    /*
     * For linux the best performance is obtained by slightly hacking memchr.
     * memchr works only on 8bit char but char16_t is 16bit. So we treat char16_t
     * in blocks of 8bit and use memchr.
     */

    const char* text8 = (const char*) text;
    const char* pat8 = reinterpret_cast<const char*>(&pat);

    MOZ_ASSERT(n < UINT32_MAX/2);
    n *= 2;

    uint32_t i = 0;
    while (i < n) {
        /* Find the first 8 bits of 16bit character in text. */
        const char* pos8 = FirstCharMatcher8bit(text8 + i, n - i, pat8[0]);
        if (pos8 == nullptr)
            return nullptr;
        i = static_cast<uint32_t>(pos8 - text8);

        /* Incorrect match if it matches the last 8 bits of 16bit char. */
        if (i % 2 != 0) {
            i++;
            continue;
        }

        /* Test if last 8 bits match last 8 bits of 16bit char. */
        if (pat8[1] == text8[i + 1])
            return (text + (i/2));

        i += 2;
    }
    return nullptr;
#endif
}

template <class InnerMatch, typename TextChar, typename PatChar>
static int
Matcher(const TextChar* text, uint32_t textlen, const PatChar* pat, uint32_t patlen)
{
    const typename InnerMatch::Extent extent = InnerMatch::computeExtent(pat, patlen);

    uint32_t i = 0;
    uint32_t n = textlen - patlen + 1;
    while (i < n) {
        const TextChar* pos;

        if (sizeof(TextChar) == 2 && sizeof(PatChar) == 2)
            pos = (TextChar*) FirstCharMatcher16bit((char16_t*)text + i, n - i, pat[0]);
        else if (sizeof(TextChar) == 1 && sizeof(PatChar) == 1)
            pos = (TextChar*) FirstCharMatcher8bit((char*) text + i, n - i, pat[0]);
        else
            pos = (TextChar*) FirstCharMatcherUnrolled<TextChar, PatChar>(text + i, n - i, pat[0]);

        if (pos == nullptr)
            return -1;

        i = static_cast<uint32_t>(pos - text);
        if (InnerMatch::match(pat + 1, text + i + 1, extent))
            return i;

        i += 1;
     }
     return -1;
 }


template <typename TextChar, typename PatChar>
static MOZ_ALWAYS_INLINE int
StringMatch(const TextChar* text, uint32_t textLen, const PatChar* pat, uint32_t patLen)
{
    if (patLen == 0)
        return 0;
    if (textLen < patLen)
        return -1;

#if defined(__i386__) || defined(_M_IX86) || defined(__i386)
    /*
     * Given enough registers, the unrolled loop below is faster than the
     * following loop. 32-bit x86 does not have enough registers.
     */
    if (patLen == 1) {
        const PatChar p0 = *pat;
        const TextChar* end = text + textLen;
        for (const TextChar* c = text; c != end; ++c) {
            if (*c == p0)
                return c - text;
        }
        return -1;
    }
#endif

    /*
     * If the text or pattern string is short, BMH will be more expensive than
     * the basic linear scan due to initialization cost and a more complex loop
     * body. While the correct threshold is input-dependent, we can make a few
     * conservative observations:
     *  - When |textLen| is "big enough", the initialization time will be
     *    proportionally small, so the worst-case slowdown is minimized.
     *  - When |patLen| is "too small", even the best case for BMH will be
     *    slower than a simple scan for large |textLen| due to the more complex
     *    loop body of BMH.
     * From this, the values for "big enough" and "too small" are determined
     * empirically. See bug 526348.
     */
    if (textLen >= 512 && patLen >= 11 && patLen <= sBMHPatLenMax) {
        int index = BoyerMooreHorspool(text, textLen, pat, patLen);
        if (index != sBMHBadPattern)
            return index;
    }

    /*
     * For big patterns with large potential overlap we want the SIMD-optimized
     * speed of memcmp. For small patterns, a simple loop is faster. We also can't
     * use memcmp if one of the strings is TwoByte and the other is Latin1.
     *
     * FIXME: Linux memcmp performance is sad and the manual loop is faster.
     */
    return
#if !defined(__linux__)
        (patLen > 128 && IsSame<TextChar, PatChar>::value)
            ? Matcher<MemCmp<TextChar, PatChar>, TextChar, PatChar>(text, textLen, pat, patLen)
            :
#endif
              Matcher<ManualCmp<TextChar, PatChar>, TextChar, PatChar>(text, textLen, pat, patLen);
}

static int32_t
StringMatch(JSLinearString* text, JSLinearString* pat, uint32_t start = 0)
{
    MOZ_ASSERT(start <= text->length());
    uint32_t textLen = text->length() - start;
    uint32_t patLen = pat->length();

    int match;
    AutoCheckCannotGC nogc;
    if (text->hasLatin1Chars()) {
        const Latin1Char* textChars = text->latin1Chars(nogc) + start;
        if (pat->hasLatin1Chars())
            match = StringMatch(textChars, textLen, pat->latin1Chars(nogc), patLen);
        else
            match = StringMatch(textChars, textLen, pat->twoByteChars(nogc), patLen);
    } else {
        const char16_t* textChars = text->twoByteChars(nogc) + start;
        if (pat->hasLatin1Chars())
            match = StringMatch(textChars, textLen, pat->latin1Chars(nogc), patLen);
        else
            match = StringMatch(textChars, textLen, pat->twoByteChars(nogc), patLen);
    }

    return (match == -1) ? -1 : start + match;
}

static const size_t sRopeMatchThresholdRatioLog2 = 5;

bool
js::StringHasPattern(JSLinearString* text, const char16_t* pat, uint32_t patLen)
{
    AutoCheckCannotGC nogc;
    return text->hasLatin1Chars()
           ? StringMatch(text->latin1Chars(nogc), text->length(), pat, patLen) != -1
           : StringMatch(text->twoByteChars(nogc), text->length(), pat, patLen) != -1;
}

int
js::StringFindPattern(JSLinearString* text, JSLinearString* pat, size_t start)
{
    return StringMatch(text, pat, start);
}

// When an algorithm does not need a string represented as a single linear
// array of characters, this range utility may be used to traverse the string a
// sequence of linear arrays of characters. This avoids flattening ropes.
class StringSegmentRange
{
    // If malloc() shows up in any profiles from this vector, we can add a new
    // StackAllocPolicy which stashes a reusable freed-at-gc buffer in the cx.
    Rooted<StringVector> stack;
    RootedLinearString cur;

    bool settle(JSString* str) {
        while (str->isRope()) {
            JSRope& rope = str->asRope();
            if (!stack.append(rope.rightChild()))
                return false;
            str = rope.leftChild();
        }
        cur = &str->asLinear();
        return true;
    }

  public:
    explicit StringSegmentRange(JSContext* cx)
      : stack(cx, StringVector(cx)), cur(cx)
    {}

    MOZ_WARN_UNUSED_RESULT bool init(JSString* str) {
        MOZ_ASSERT(stack.empty());
        return settle(str);
    }

    bool empty() const {
        return cur == nullptr;
    }

    JSLinearString* front() const {
        MOZ_ASSERT(!cur->isRope());
        return cur;
    }

    MOZ_WARN_UNUSED_RESULT bool popFront() {
        MOZ_ASSERT(!empty());
        if (stack.empty()) {
            cur = nullptr;
            return true;
        }
        return settle(stack.popCopy());
    }
};

typedef Vector<JSLinearString*, 16, SystemAllocPolicy> LinearStringVector;

template <typename TextChar, typename PatChar>
static int
RopeMatchImpl(const AutoCheckCannotGC& nogc, LinearStringVector& strings,
              const PatChar* pat, size_t patLen)
{
    /* Absolute offset from the beginning of the logical text string. */
    int pos = 0;

    for (JSLinearString** outerp = strings.begin(); outerp != strings.end(); ++outerp) {
        /* Try to find a match within 'outer'. */
        JSLinearString* outer = *outerp;
        const TextChar* chars = outer->chars<TextChar>(nogc);
        size_t len = outer->length();
        int matchResult = StringMatch(chars, len, pat, patLen);
        if (matchResult != -1) {
            /* Matched! */
            return pos + matchResult;
        }

        /* Try to find a match starting in 'outer' and running into other nodes. */
        const TextChar* const text = chars + (patLen > len ? 0 : len - patLen + 1);
        const TextChar* const textend = chars + len;
        const PatChar p0 = *pat;
        const PatChar* const p1 = pat + 1;
        const PatChar* const patend = pat + patLen;
        for (const TextChar* t = text; t != textend; ) {
            if (*t++ != p0)
                continue;

            JSLinearString** innerp = outerp;
            const TextChar* ttend = textend;
            const TextChar* tt = t;
            for (const PatChar* pp = p1; pp != patend; ++pp, ++tt) {
                while (tt == ttend) {
                    if (++innerp == strings.end())
                        return -1;

                    JSLinearString* inner = *innerp;
                    tt = inner->chars<TextChar>(nogc);
                    ttend = tt + inner->length();
                }
                if (*pp != *tt)
                    goto break_continue;
            }

            /* Matched! */
            return pos + (t - chars) - 1;  /* -1 because of *t++ above */

          break_continue:;
        }

        pos += len;
    }

    return -1;
}

/*
 * RopeMatch takes the text to search and the pattern to search for in the text.
 * RopeMatch returns false on OOM and otherwise returns the match index through
 * the 'match' outparam (-1 for not found).
 */
static bool
RopeMatch(JSContext* cx, JSRope* text, JSLinearString* pat, int* match)
{
    uint32_t patLen = pat->length();
    if (patLen == 0) {
        *match = 0;
        return true;
    }
    if (text->length() < patLen) {
        *match = -1;
        return true;
    }

    /*
     * List of leaf nodes in the rope. If we run out of memory when trying to
     * append to this list, we can still fall back to StringMatch, so use the
     * system allocator so we don't report OOM in that case.
     */
    LinearStringVector strings;

    /*
     * We don't want to do rope matching if there is a poor node-to-char ratio,
     * since this means spending a lot of time in the match loop below. We also
     * need to build the list of leaf nodes. Do both here: iterate over the
     * nodes so long as there are not too many.
     *
     * We also don't use rope matching if the rope contains both Latin1 and
     * TwoByte nodes, to simplify the match algorithm.
     */
    {
        size_t threshold = text->length() >> sRopeMatchThresholdRatioLog2;
        StringSegmentRange r(cx);
        if (!r.init(text))
            return false;

        bool textIsLatin1 = text->hasLatin1Chars();
        while (!r.empty()) {
            if (threshold-- == 0 ||
                r.front()->hasLatin1Chars() != textIsLatin1 ||
                !strings.append(r.front()))
            {
                JSLinearString* linear = text->ensureLinear(cx);
                if (!linear)
                    return false;

                *match = StringMatch(linear, pat);
                return true;
            }
            if (!r.popFront())
                return false;
        }
    }

    AutoCheckCannotGC nogc;
    if (text->hasLatin1Chars()) {
        if (pat->hasLatin1Chars())
            *match = RopeMatchImpl<Latin1Char>(nogc, strings, pat->latin1Chars(nogc), patLen);
        else
            *match = RopeMatchImpl<Latin1Char>(nogc, strings, pat->twoByteChars(nogc), patLen);
    } else {
        if (pat->hasLatin1Chars())
            *match = RopeMatchImpl<char16_t>(nogc, strings, pat->latin1Chars(nogc), patLen);
        else
            *match = RopeMatchImpl<char16_t>(nogc, strings, pat->twoByteChars(nogc), patLen);
    }

    return true;
}

/* ES6 draft rc4 21.1.3.7. */
static bool
str_includes(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1, 2, and 3
    RootedString str(cx, ThisToStringForStringProto(cx, args));
    if (!str)
        return false;

    // Steps 4 and 5
    bool isRegExp;
    if (!IsRegExp(cx, args.get(0), &isRegExp))
        return false;

    // Step 6
    if (isRegExp) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INVALID_ARG_TYPE,
                             "first", "", "Regular Expression");
        return false;
    }

    // Steps 7 and 8
    RootedLinearString searchStr(cx, ArgToRootedString(cx, args, 0));
    if (!searchStr)
        return false;

    // Steps 9 and 10
    uint32_t pos = 0;
    if (args.hasDefined(1)) {
        if (args[1].isInt32()) {
            int i = args[1].toInt32();
            pos = (i < 0) ? 0U : uint32_t(i);
        } else {
            double d;
            if (!ToInteger(cx, args[1], &d))
                return false;
            pos = uint32_t(Min(Max(d, 0.0), double(UINT32_MAX)));
        }
    }

    // Step 11
    uint32_t textLen = str->length();

    // Step 12
    uint32_t start = Min(Max(pos, 0U), textLen);

    // Steps 13 and 14
    JSLinearString* text = str->ensureLinear(cx);
    if (!text)
        return false;

    args.rval().setBoolean(StringMatch(text, searchStr, start) != -1);
    return true;
}

/* TODO: remove String.prototype.contains (bug 1103588) */
static bool
str_contains(JSContext *cx, unsigned argc, Value *vp)
{
#ifndef RELEASE_BUILD
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject callee(cx, &args.callee());
    if (!GlobalObject::warnOnceAboutStringContains(cx, callee))
        return false;
#endif
    return str_includes(cx, argc, vp);
}

/* ES6 20120927 draft 15.5.4.7. */
bool
js::str_indexOf(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1, 2, and 3
    RootedString str(cx, ThisToStringForStringProto(cx, args));
    if (!str)
        return false;

    // Steps 4 and 5
    RootedLinearString searchStr(cx, ArgToRootedString(cx, args, 0));
    if (!searchStr)
        return false;

    // Steps 6 and 7
    uint32_t pos = 0;
    if (args.hasDefined(1)) {
        if (args[1].isInt32()) {
            int i = args[1].toInt32();
            pos = (i < 0) ? 0U : uint32_t(i);
        } else {
            double d;
            if (!ToInteger(cx, args[1], &d))
                return false;
            pos = uint32_t(Min(Max(d, 0.0), double(UINT32_MAX)));
        }
    }

   // Step 8
    uint32_t textLen = str->length();

    // Step 9
    uint32_t start = Min(Max(pos, 0U), textLen);

    // Steps 10 and 11
    JSLinearString* text = str->ensureLinear(cx);
    if (!text)
        return false;

    args.rval().setInt32(StringMatch(text, searchStr, start));
    return true;
}

template <typename TextChar, typename PatChar>
static int32_t
LastIndexOfImpl(const TextChar* text, size_t textLen, const PatChar* pat, size_t patLen,
                size_t start)
{
    MOZ_ASSERT(patLen > 0);
    MOZ_ASSERT(patLen <= textLen);
    MOZ_ASSERT(start <= textLen - patLen);

    const PatChar p0 = *pat;
    const PatChar* patNext = pat + 1;
    const PatChar* patEnd = pat + patLen;

    for (const TextChar* t = text + start; t >= text; --t) {
        if (*t == p0) {
            const TextChar* t1 = t + 1;
            for (const PatChar* p1 = patNext; p1 < patEnd; ++p1, ++t1) {
                if (*t1 != *p1)
                    goto break_continue;
            }

            return static_cast<int32_t>(t - text);
        }
      break_continue:;
    }

    return -1;
}

bool
js::str_lastIndexOf(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedString textstr(cx, ThisToStringForStringProto(cx, args));
    if (!textstr)
        return false;

    RootedLinearString pat(cx, ArgToRootedString(cx, args, 0));
    if (!pat)
        return false;

    size_t textLen = textstr->length();
    size_t patLen = pat->length();
    int start = textLen - patLen; // Start searching here
    if (start < 0) {
        args.rval().setInt32(-1);
        return true;
    }

    if (args.hasDefined(1)) {
        if (args[1].isInt32()) {
            int i = args[1].toInt32();
            if (i <= 0)
                start = 0;
            else if (i < start)
                start = i;
        } else {
            double d;
            if (!ToNumber(cx, args[1], &d))
                return false;
            if (!IsNaN(d)) {
                d = JS::ToInteger(d);
                if (d <= 0)
                    start = 0;
                else if (d < start)
                    start = int(d);
            }
        }
    }

    if (patLen == 0) {
        args.rval().setInt32(start);
        return true;
    }

    JSLinearString* text = textstr->ensureLinear(cx);
    if (!text)
        return false;

    int32_t res;
    AutoCheckCannotGC nogc;
    if (text->hasLatin1Chars()) {
        const Latin1Char* textChars = text->latin1Chars(nogc);
        if (pat->hasLatin1Chars())
            res = LastIndexOfImpl(textChars, textLen, pat->latin1Chars(nogc), patLen, start);
        else
            res = LastIndexOfImpl(textChars, textLen, pat->twoByteChars(nogc), patLen, start);
    } else {
        const char16_t* textChars = text->twoByteChars(nogc);
        if (pat->hasLatin1Chars())
            res = LastIndexOfImpl(textChars, textLen, pat->latin1Chars(nogc), patLen, start);
        else
            res = LastIndexOfImpl(textChars, textLen, pat->twoByteChars(nogc), patLen, start);
    }

    args.rval().setInt32(res);
    return true;
}

bool
js::HasSubstringAt(JSLinearString* text, JSLinearString* pat, size_t start)
{
    MOZ_ASSERT(start + pat->length() <= text->length());

    size_t patLen = pat->length();

    AutoCheckCannotGC nogc;
    if (text->hasLatin1Chars()) {
        const Latin1Char* textChars = text->latin1Chars(nogc) + start;
        if (pat->hasLatin1Chars())
            return PodEqual(textChars, pat->latin1Chars(nogc), patLen);

        return EqualChars(textChars, pat->twoByteChars(nogc), patLen);
    }

    const char16_t* textChars = text->twoByteChars(nogc) + start;
    if (pat->hasTwoByteChars())
        return PodEqual(textChars, pat->twoByteChars(nogc), patLen);

    return EqualChars(pat->latin1Chars(nogc), textChars, patLen);
}

/* ES6 draft rc3 21.1.3.18. */
bool
js::str_startsWith(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1, 2, and 3
    RootedString str(cx, ThisToStringForStringProto(cx, args));
    if (!str)
        return false;

    // Steps 4 and 5
    bool isRegExp;
    if (!IsRegExp(cx, args.get(0), &isRegExp))
        return false;

    // Step 6
    if (isRegExp) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INVALID_ARG_TYPE,
                             "first", "", "Regular Expression");
        return false;
    }

    // Steps 7 and 8
    RootedLinearString searchStr(cx, ArgToRootedString(cx, args, 0));
    if (!searchStr)
        return false;

    // Steps 9 and 10
    uint32_t pos = 0;
    if (args.hasDefined(1)) {
        if (args[1].isInt32()) {
            int i = args[1].toInt32();
            pos = (i < 0) ? 0U : uint32_t(i);
        } else {
            double d;
            if (!ToInteger(cx, args[1], &d))
                return false;
            pos = uint32_t(Min(Max(d, 0.0), double(UINT32_MAX)));
        }
    }

    // Step 11
    uint32_t textLen = str->length();

    // Step 12
    uint32_t start = Min(Max(pos, 0U), textLen);

    // Step 13
    uint32_t searchLen = searchStr->length();

    // Step 14
    if (searchLen + start < searchLen || searchLen + start > textLen) {
        args.rval().setBoolean(false);
        return true;
    }

    // Steps 15 and 16
    JSLinearString* text = str->ensureLinear(cx);
    if (!text)
        return false;

    args.rval().setBoolean(HasSubstringAt(text, searchStr, start));
    return true;
}

/* ES6 draft rc3 21.1.3.6. */
static bool
str_endsWith(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1, 2, and 3
    RootedString str(cx, ThisToStringForStringProto(cx, args));
    if (!str)
        return false;

    // Steps 4 and 5
    bool isRegExp;
    if (!IsRegExp(cx, args.get(0), &isRegExp))
        return false;

    // Step 6
    if (isRegExp) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INVALID_ARG_TYPE,
                             "first", "", "Regular Expression");
        return false;
    }

    // Steps 7 and 8
    RootedLinearString searchStr(cx, ArgToRootedString(cx, args, 0));
    if (!searchStr)
        return false;

    // Step 9
    uint32_t textLen = str->length();

    // Steps 10 and 11
    uint32_t pos = textLen;
    if (args.hasDefined(1)) {
        if (args[1].isInt32()) {
            int i = args[1].toInt32();
            pos = (i < 0) ? 0U : uint32_t(i);
        } else {
            double d;
            if (!ToInteger(cx, args[1], &d))
                return false;
            pos = uint32_t(Min(Max(d, 0.0), double(UINT32_MAX)));
        }
    }

    // Step 12
    uint32_t end = Min(Max(pos, 0U), textLen);

    // Step 13
    uint32_t searchLen = searchStr->length();

    // Step 15 (reordered)
    if (searchLen > end) {
        args.rval().setBoolean(false);
        return true;
    }

    // Step 14
    uint32_t start = end - searchLen;

    // Steps 16 and 17
    JSLinearString* text = str->ensureLinear(cx);
    if (!text)
        return false;

    args.rval().setBoolean(HasSubstringAt(text, searchStr, start));
    return true;
}

template <typename CharT>
static void
TrimString(const CharT* chars, bool trimLeft, bool trimRight, size_t length,
           size_t* pBegin, size_t* pEnd)
{
    size_t begin = 0, end = length;

    if (trimLeft) {
        while (begin < length && unicode::IsSpace(chars[begin]))
            ++begin;
    }

    if (trimRight) {
        while (end > begin && unicode::IsSpace(chars[end - 1]))
            --end;
    }

    *pBegin = begin;
    *pEnd = end;
}

static bool
TrimString(JSContext* cx, Value* vp, bool trimLeft, bool trimRight)
{
    CallReceiver call = CallReceiverFromVp(vp);
    RootedString str(cx, ThisToStringForStringProto(cx, call));
    if (!str)
        return false;

    JSLinearString* linear = str->ensureLinear(cx);
    if (!linear)
        return false;

    size_t length = linear->length();
    size_t begin, end;
    if (linear->hasLatin1Chars()) {
        AutoCheckCannotGC nogc;
        TrimString(linear->latin1Chars(nogc), trimLeft, trimRight, length, &begin, &end);
    } else {
        AutoCheckCannotGC nogc;
        TrimString(linear->twoByteChars(nogc), trimLeft, trimRight, length, &begin, &end);
    }

    str = NewDependentString(cx, str, begin, end - begin);
    if (!str)
        return false;

    call.rval().setString(str);
    return true;
}

static bool
str_trim(JSContext* cx, unsigned argc, Value* vp)
{
    return TrimString(cx, vp, true, true);
}

static bool
str_trimLeft(JSContext* cx, unsigned argc, Value* vp)
{
    return TrimString(cx, vp, true, false);
}

static bool
str_trimRight(JSContext* cx, unsigned argc, Value* vp)
{
    return TrimString(cx, vp, false, true);
}

/*
 * Perl-inspired string functions.
 */

namespace {

/* Result of a successfully performed flat match. */
class FlatMatch
{
    RootedAtom pat_;
    int32_t match_;

    friend class StringRegExpGuard;

  public:
    explicit FlatMatch(JSContext* cx) : pat_(cx) {}
    JSLinearString* pattern() const { return pat_; }
    size_t patternLength() const { return pat_->length(); }

    /*
     * Note: The match is -1 when the match is performed successfully,
     * but no match is found.
     */
    int32_t match() const { return match_; }
};

} /* anonymous namespace */

static inline bool
IsRegExpMetaChar(char16_t c)
{
    switch (c) {
      /* Taken from the PatternCharacter production in 15.10.1. */
      case '^': case '$': case '\\': case '.': case '*': case '+':
      case '?': case '(': case ')': case '[': case ']': case '{':
      case '}': case '|':
        return true;
      default:
        return false;
    }
}

template <typename CharT>
bool
js::HasRegExpMetaChars(const CharT* chars, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (IsRegExpMetaChar(chars[i]))
            return true;
    }
    return false;
}

template bool
js::HasRegExpMetaChars<Latin1Char>(const Latin1Char* chars, size_t length);

template bool
js::HasRegExpMetaChars<char16_t>(const char16_t* chars, size_t length);

bool
js::StringHasRegExpMetaChars(JSLinearString* str)
{
    AutoCheckCannotGC nogc;
    if (str->hasLatin1Chars())
        return HasRegExpMetaChars(str->latin1Chars(nogc), str->length());

    return HasRegExpMetaChars(str->twoByteChars(nogc), str->length());
}

namespace {

/*
 * StringRegExpGuard factors logic out of String regexp operations.
 *
 * |optarg| indicates in which argument position RegExp flags will be found, if
 * present. This is a Mozilla extension and not part of any ECMA spec.
 */
class MOZ_STACK_CLASS StringRegExpGuard
{
    RegExpGuard re_;
    FlatMatch   fm;
    RootedObject obj_;

    /*
     * Upper bound on the number of characters we are willing to potentially
     * waste on searching for RegExp meta-characters.
     */
    static const size_t MAX_FLAT_PAT_LEN = 256;

    template <typename CharT>
    static bool
    flattenPattern(StringBuffer& sb, const CharT* chars, size_t len)
    {
        static const char ESCAPE_CHAR = '\\';
        for (const CharT* it = chars; it < chars + len; ++it) {
            if (IsRegExpMetaChar(*it)) {
                if (!sb.append(ESCAPE_CHAR) || !sb.append(*it))
                    return false;
            } else {
                if (!sb.append(*it))
                    return false;
            }
        }
        return true;
    }

    static JSAtom*
    flattenPattern(JSContext* cx, JSAtom* pat)
    {
        StringBuffer sb(cx);
        if (!sb.reserve(pat->length()))
            return nullptr;

        if (pat->hasLatin1Chars()) {
            AutoCheckCannotGC nogc;
            if (!flattenPattern(sb, pat->latin1Chars(nogc), pat->length()))
                return nullptr;
        } else {
            AutoCheckCannotGC nogc;
            if (!flattenPattern(sb, pat->twoByteChars(nogc), pat->length()))
                return nullptr;
        }

        return sb.finishAtom();
    }

  public:
    explicit StringRegExpGuard(JSContext* cx)
      : re_(cx), fm(cx), obj_(cx)
    { }

    /* init must succeed in order to call tryFlatMatch or normalizeRegExp. */
    bool init(JSContext* cx, const CallArgs& args, bool convertVoid = false)
    {
        if (args.length() != 0) {
            ESClassValue cls;
            if (!GetClassOfValue(cx, args[0], &cls))
                return false;

            if (cls == ESClass_RegExp)
                return initRegExp(cx, &args[0].toObject());
        }

        if (convertVoid && !args.hasDefined(0)) {
            fm.pat_ = cx->runtime()->emptyString;
            return true;
        }

        JSString* arg = ArgToRootedString(cx, args, 0);
        if (!arg)
            return false;

        fm.pat_ = AtomizeString(cx, arg);
        if (!fm.pat_)
            return false;

        return true;
    }

    bool initRegExp(JSContext* cx, JSObject* regexp) {
        obj_ = regexp;
        return RegExpToShared(cx, obj_, &re_);
    }

    bool init(JSContext* cx, HandleString pattern) {
        fm.pat_ = AtomizeString(cx, pattern);
        if (!fm.pat_)
            return false;
        return true;
    }

    /*
     * Attempt to match |patstr| to |textstr|. A flags argument, metachars in
     * the pattern string, or a lengthy pattern string can thwart this process.
     *
     * |checkMetaChars| looks for regexp metachars in the pattern string.
     *
     * Return whether flat matching could be used.
     *
     * N.B. tryFlatMatch returns nullptr on OOM, so the caller must check
     * cx->isExceptionPending().
     */
    const FlatMatch*
    tryFlatMatch(JSContext* cx, JSString* text, unsigned optarg, unsigned argc,
                 bool checkMetaChars = true)
    {
        if (re_.initialized())
            return nullptr;

        if (optarg < argc)
            return nullptr;

        size_t patLen = fm.pat_->length();
        if (checkMetaChars && (patLen > MAX_FLAT_PAT_LEN || StringHasRegExpMetaChars(fm.pat_)))
            return nullptr;

        /*
         * |text| could be a rope, so we want to avoid flattening it for as
         * long as possible.
         */
        if (text->isRope()) {
            if (!RopeMatch(cx, &text->asRope(), fm.pat_, &fm.match_))
                return nullptr;
        } else {
            fm.match_ = StringMatch(&text->asLinear(), fm.pat_, 0);
        }

        return &fm;
    }

    /* If the pattern is not already a regular expression, make it so. */
    bool normalizeRegExp(JSContext* cx, bool flat, unsigned optarg, const CallArgs& args)
    {
        if (re_.initialized())
            return true;

        /* Build RegExp from pattern string. */
        RootedString opt(cx);
        if (optarg < args.length()) {
            if (JSScript* script = cx->currentScript()) {
                const char* filename = script->filename();
                cx->compartment()->addTelemetry(filename, JSCompartment::DeprecatedFlagsArgument);
            }

            if (!cx->compartment()->warnedAboutFlagsArgument) {
                if (!JS_ReportErrorFlagsAndNumber(cx, JSREPORT_WARNING, GetErrorMessage, nullptr,
                                                  JSMSG_DEPRECATED_FLAGS_ARG))
                    return false;
                cx->compartment()->warnedAboutFlagsArgument = true;
            }

            opt = ToString<CanGC>(cx, args[optarg]);
            if (!opt)
                return false;
        } else {
            opt = nullptr;
        }

        Rooted<JSAtom*> pat(cx);
        if (flat) {
            pat = flattenPattern(cx, fm.pat_);
            if (!pat)
                return false;
        } else {
            pat = fm.pat_;
        }
        MOZ_ASSERT(pat);

        return cx->compartment()->regExps.get(cx, pat, opt, &re_);
    }

    bool zeroLastIndex(JSContext* cx) {
        if (!regExpIsObject())
            return true;

        // Use a fast path for same-global RegExp objects with writable
        // lastIndex.
        if (obj_->is<RegExpObject>()) {
            RegExpObject* nobj = &obj_->as<RegExpObject>();
            if (nobj->lookup(cx, cx->names().lastIndex)->writable()) {
                nobj->zeroLastIndex();
                return true;
            }
        }

        // Handle everything else generically (including throwing if .lastIndex is non-writable).
        RootedValue zero(cx, Int32Value(0));
        return SetProperty(cx, obj_, cx->names().lastIndex, zero);
    }

    RegExpShared& regExp() { return *re_; }

    bool regExpIsObject() { return obj_ != nullptr; }
    HandleObject regExpObject() {
        MOZ_ASSERT(regExpIsObject());
        return obj_;
    }

  private:
    StringRegExpGuard(const StringRegExpGuard&) = delete;
    void operator=(const StringRegExpGuard&) = delete;
};

} /* anonymous namespace */

static bool
DoMatchLocal(JSContext* cx, const CallArgs& args, RegExpStatics* res, HandleLinearString input,
             RegExpShared& re)
{
    ScopedMatchPairs matches(&cx->tempLifoAlloc());
    RegExpRunStatus status = re.execute(cx, input, 0, &matches);
    if (status == RegExpRunStatus_Error)
        return false;

    if (status == RegExpRunStatus_Success_NotFound) {
        args.rval().setNull();
        return true;
    }

    if (!res->updateFromMatchPairs(cx, input, matches))
        return false;

    RootedValue rval(cx);
    if (!CreateRegExpMatchResult(cx, input, matches, &rval))
        return false;

    args.rval().set(rval);
    return true;
}

/* ES5 15.5.4.10 step 8. */
static bool
DoMatchGlobal(JSContext* cx, const CallArgs& args, RegExpStatics* res, HandleLinearString input,
              StringRegExpGuard& g)
{
    // Step 8a.
    //
    // This single zeroing of "lastIndex" covers all "lastIndex" changes in the
    // rest of String.prototype.match, particularly in steps 8f(i) and
    // 8f(iii)(2)(a).  Here's why.
    //
    // The inputs to the calls to RegExp.prototype.exec are a RegExp object
    // whose .global is true and a string.  The only side effect of a call in
    // these circumstances is that the RegExp's .lastIndex will be modified to
    // the next starting index after the discovered match (or to 0 if there's
    // no remaining match).  Because .lastIndex is a non-configurable data
    // property and no script-controllable code executes after step 8a, passing
    // step 8a implies *every* .lastIndex set succeeds.  String.prototype.match
    // calls RegExp.prototype.exec repeatedly, and the last call doesn't match,
    // so the final value of .lastIndex is 0: exactly the state after step 8a
    // succeeds.  No spec step lets script observe intermediate .lastIndex
    // values.
    //
    // The arrays returned by RegExp.prototype.exec always have a string at
    // index 0, for which [[Get]]s have no side effects.
    //
    // Filling in a new array using [[DefineOwnProperty]] is unobservable.
    //
    // This is a tricky point, because after this set, our implementation *can*
    // fail.  The key is that script can't distinguish these failure modes from
    // one where, in spec terms, we fail immediately after step 8a.  That *in
    // reality* we might have done extra matching work, or created a partial
    // results array to return, or hit an interrupt, is irrelevant.  The
    // script can't tell we did any of those things but didn't update
    // .lastIndex.  Thus we can optimize steps 8b onward however we want,
    // including eliminating intermediate .lastIndex sets, as long as we don't
    // add ways for script to observe the intermediate states.
    //
    // In short: it's okay to cheat (by setting .lastIndex to 0, once) because
    // we can't get caught.
    if (!g.zeroLastIndex(cx))
        return false;

    // Step 8b.
    AutoValueVector elements(cx);

    size_t lastSuccessfulStart = 0;

    // The loop variables from steps 8c-e aren't needed, as we use different
    // techniques from the spec to implement step 8f's loop.

    // Step 8f.
    ScopedMatchPairs matches(&cx->tempLifoAlloc());
    size_t charsLen = input->length();
    RegExpShared& re = g.regExp();
    for (size_t searchIndex = 0; searchIndex <= charsLen; ) {
        if (!CheckForInterrupt(cx))
            return false;

        // Steps 8f(i-ii), minus "lastIndex" updates (see above).
        RegExpRunStatus status = re.execute(cx, input, searchIndex, &matches);
        if (status == RegExpRunStatus_Error)
            return false;

        // Step 8f(ii).
        if (status == RegExpRunStatus_Success_NotFound)
            break;

        lastSuccessfulStart = searchIndex;
        MatchPair& match = matches[0];

        // Steps 8f(iii)(1-3).
        searchIndex = match.isEmpty() ? match.limit + 1 : match.limit;

        // Step 8f(iii)(4-5).
        JSLinearString* str = NewDependentString(cx, input, match.start, match.length());
        if (!str)
            return false;
        if (!elements.append(StringValue(str)))
            return false;
    }

    // Step 8g.
    if (elements.empty()) {
        args.rval().setNull();
        return true;
    }

    // The last *successful* match updates the RegExpStatics. (Interestingly,
    // this implies that String.prototype.match's semantics aren't those
    // implied by the RegExp.prototype.exec calls in the ES5 algorithm.)
    res->updateLazily(cx, input, &re, lastSuccessfulStart);

    // Steps 8b, 8f(iii)(5-6), 8h.
    JSObject* array = NewDenseCopiedArray(cx, elements.length(), elements.begin());
    if (!array)
        return false;

    args.rval().setObject(*array);
    return true;
}

static bool
BuildFlatMatchArray(JSContext* cx, HandleString textstr, const FlatMatch& fm, CallArgs* args)
{
    if (fm.match() < 0) {
        args->rval().setNull();
        return true;
    }

    /* Get the templateObject that defines the shape and type of the output object */
    JSObject* templateObject = cx->compartment()->regExps.getOrCreateMatchResultTemplateObject(cx);
    if (!templateObject)
        return false;

    RootedArrayObject arr(cx, NewDenseFullyAllocatedArrayWithTemplate(cx, 1, templateObject));
    if (!arr)
        return false;

    /* Store a Value for each pair. */
    arr->setDenseInitializedLength(1);
    arr->initDenseElement(0, StringValue(fm.pattern()));

    /* Set the |index| property. (TemplateObject positions it in slot 0) */
    arr->setSlot(0, Int32Value(fm.match()));

    /* Set the |input| property. (TemplateObject positions it in slot 1) */
    arr->setSlot(1, StringValue(textstr));

#ifdef DEBUG
    RootedValue test(cx);
    RootedId id(cx, NameToId(cx->names().index));
    if (!NativeGetProperty(cx, arr, id, &test))
        return false;
    MOZ_ASSERT(test == arr->getSlot(0));
    id = NameToId(cx->names().input);
    if (!NativeGetProperty(cx, arr, id, &test))
        return false;
    MOZ_ASSERT(test == arr->getSlot(1));
#endif

    args->rval().setObject(*arr);
    return true;
}

/* ES5 15.5.4.10. */
bool
js::str_match(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Steps 1-2. */
    RootedString str(cx, ThisToStringForStringProto(cx, args));
    if (!str)
        return false;

    /* Steps 3-4, plus the trailing-argument "flags" extension. */
    StringRegExpGuard g(cx);
    if (!g.init(cx, args, true))
        return false;

    /* Fast path when the search pattern can be searched for as a string. */
    if (const FlatMatch* fm = g.tryFlatMatch(cx, str, 1, args.length()))
        return BuildFlatMatchArray(cx, str, *fm, &args);

    /* Return if there was an error in tryFlatMatch. */
    if (cx->isExceptionPending())
        return false;

    /* Create regular-expression internals as needed to perform the match. */
    if (!g.normalizeRegExp(cx, false, 1, args))
        return false;

    RegExpStatics* res = cx->global()->getRegExpStatics(cx);
    if (!res)
        return false;

    RootedLinearString linearStr(cx, str->ensureLinear(cx));
    if (!linearStr)
        return false;

    /* Steps 5-6, 7. */
    if (!g.regExp().global())
        return DoMatchLocal(cx, args, res, linearStr, g.regExp());

    /* Steps 6, 8. */
    return DoMatchGlobal(cx, args, res, linearStr, g);
}

bool
js::str_search(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedString str(cx, ThisToStringForStringProto(cx, args));
    if (!str)
        return false;

    StringRegExpGuard g(cx);
    if (!g.init(cx, args, true))
        return false;
    if (const FlatMatch* fm = g.tryFlatMatch(cx, str, 1, args.length())) {
        args.rval().setInt32(fm->match());
        return true;
    }

    if (cx->isExceptionPending())  /* from tryFlatMatch */
        return false;

    if (!g.normalizeRegExp(cx, false, 1, args))
        return false;

    RootedLinearString linearStr(cx, str->ensureLinear(cx));
    if (!linearStr)
        return false;

    RegExpStatics* res = cx->global()->getRegExpStatics(cx);
    if (!res)
        return false;

    /* Per ECMAv5 15.5.4.12 (5) The last index property is ignored and left unchanged. */
    ScopedMatchPairs matches(&cx->tempLifoAlloc());
    RegExpRunStatus status = g.regExp().execute(cx, linearStr, 0, &matches);
    if (status == RegExpRunStatus_Error)
        return false;

    if (status == RegExpRunStatus_Success)
        res->updateLazily(cx, linearStr, &g.regExp(), 0);

    args.rval().setInt32(status == RegExpRunStatus_Success_NotFound ? -1 : matches[0].start);
    return true;
}

// Utility for building a rope (lazy concatenation) of strings.
class RopeBuilder {
    JSContext* cx;
    RootedString res;

    RopeBuilder(const RopeBuilder& other) = delete;
    void operator=(const RopeBuilder& other) = delete;

  public:
    explicit RopeBuilder(JSContext* cx)
      : cx(cx), res(cx, cx->runtime()->emptyString)
    {}

    inline bool append(HandleString str) {
        res = ConcatStrings<CanGC>(cx, res, str);
        return !!res;
    }

    inline JSString* result() {
        return res;
    }
};

namespace {

template <typename CharT>
static uint32_t
FindDollarIndex(const CharT* chars, size_t length)
{
    if (const CharT* p = js_strchr_limit(chars, '$', chars + length)) {
        uint32_t dollarIndex = p - chars;
        MOZ_ASSERT(dollarIndex < length);
        return dollarIndex;
    }
    return UINT32_MAX;
}

struct ReplaceData
{
    explicit ReplaceData(JSContext* cx)
      : str(cx), g(cx), lambda(cx), elembase(cx), repstr(cx),
        fig(cx, NullValue()), sb(cx)
    {}

    inline void setReplacementString(JSLinearString* string) {
        MOZ_ASSERT(string);
        lambda = nullptr;
        elembase = nullptr;
        repstr = string;

        AutoCheckCannotGC nogc;
        dollarIndex = string->hasLatin1Chars()
                      ? FindDollarIndex(string->latin1Chars(nogc), string->length())
                      : FindDollarIndex(string->twoByteChars(nogc), string->length());
    }

    inline void setReplacementFunction(JSObject* func) {
        MOZ_ASSERT(func);
        lambda = func;
        elembase = nullptr;
        repstr = nullptr;
        dollarIndex = UINT32_MAX;
    }

    RootedString       str;            /* 'this' parameter object as a string */
    StringRegExpGuard  g;              /* regexp parameter object and private data */
    RootedObject       lambda;         /* replacement function object or null */
    RootedNativeObject elembase;       /* object for function(a){return b[a]} replace */
    RootedLinearString repstr;         /* replacement string */
    uint32_t           dollarIndex;    /* index of first $ in repstr, or UINT32_MAX */
    int                leftIndex;      /* left context index in str->chars */
    bool               calledBack;     /* record whether callback has been called */
    FastInvokeGuard    fig;            /* used for lambda calls, also holds arguments */
    StringBuffer       sb;             /* buffer built during DoMatch */
};

} /* anonymous namespace */

static bool
ReplaceRegExp(JSContext* cx, RegExpStatics* res, ReplaceData& rdata);

static bool
DoMatchForReplaceLocal(JSContext* cx, RegExpStatics* res, HandleLinearString linearStr,
                       RegExpShared& re, ReplaceData& rdata, size_t* rightContextOffset)
{
    ScopedMatchPairs matches(&cx->tempLifoAlloc());
    RegExpRunStatus status = re.execute(cx, linearStr, 0, &matches);
    if (status == RegExpRunStatus_Error)
        return false;

    if (status == RegExpRunStatus_Success_NotFound)
        return true;

    MatchPair& match = matches[0];
    *rightContextOffset = match.limit;

    if (!res->updateFromMatchPairs(cx, linearStr, matches))
        return false;

    return ReplaceRegExp(cx, res, rdata);
}

static bool
DoMatchForReplaceGlobal(JSContext* cx, RegExpStatics* res, HandleLinearString linearStr,
                        RegExpShared& re, ReplaceData& rdata, size_t* rightContextOffset)
{
    size_t charsLen = linearStr->length();
    ScopedMatchPairs matches(&cx->tempLifoAlloc());
    for (size_t count = 0, searchIndex = 0; searchIndex <= charsLen; ++count) {
        if (!CheckForInterrupt(cx))
            return false;

        RegExpRunStatus status = re.execute(cx, linearStr, searchIndex, &matches);
        if (status == RegExpRunStatus_Error)
            return false;

        if (status == RegExpRunStatus_Success_NotFound)
            break;

        MatchPair& match = matches[0];
        searchIndex = match.isEmpty() ? match.limit + 1 : match.limit;
        *rightContextOffset = match.limit;

        if (!res->updateFromMatchPairs(cx, linearStr, matches))
            return false;

        if (!ReplaceRegExp(cx, res, rdata))
            return false;
    }

    return true;
}

template <typename CharT>
static bool
InterpretDollar(RegExpStatics* res, const CharT* bp, const CharT* dp, const CharT* ep,
                ReplaceData& rdata, JSSubString* out, size_t* skip)
{
    MOZ_ASSERT(*dp == '$');

    /* If there is only a dollar, bail now */
    if (dp + 1 >= ep)
        return false;

    /* Interpret all Perl match-induced dollar variables. */
    char16_t dc = dp[1];
    if (JS7_ISDEC(dc)) {
        /* ECMA-262 Edition 3: 1-9 or 01-99 */
        unsigned num = JS7_UNDEC(dc);
        if (num > res->getMatches().parenCount())
            return false;

        const CharT* cp = dp + 2;
        if (cp < ep && (dc = *cp, JS7_ISDEC(dc))) {
            unsigned tmp = 10 * num + JS7_UNDEC(dc);
            if (tmp <= res->getMatches().parenCount()) {
                cp++;
                num = tmp;
            }
        }
        if (num == 0)
            return false;

        *skip = cp - dp;

        MOZ_ASSERT(num <= res->getMatches().parenCount());

        /*
         * Note: we index to get the paren with the (1-indexed) pair
         * number, as opposed to a (0-indexed) paren number.
         */
        res->getParen(num, out);
        return true;
    }

    *skip = 2;
    switch (dc) {
      case '$':
        out->init(rdata.repstr, dp - bp, 1);
        return true;
      case '&':
        res->getLastMatch(out);
        return true;
      case '+':
        res->getLastParen(out);
        return true;
      case '`':
        res->getLeftContext(out);
        return true;
      case '\'':
        res->getRightContext(out);
        return true;
    }
    return false;
}

template <typename CharT>
static bool
FindReplaceLengthString(JSContext* cx, RegExpStatics* res, ReplaceData& rdata, size_t* sizep)
{
    JSLinearString* repstr = rdata.repstr;
    CheckedInt<uint32_t> replen = repstr->length();

    if (rdata.dollarIndex != UINT32_MAX) {
        AutoCheckCannotGC nogc;
        MOZ_ASSERT(rdata.dollarIndex < repstr->length());
        const CharT* bp = repstr->chars<CharT>(nogc);
        const CharT* dp = bp + rdata.dollarIndex;
        const CharT* ep = bp + repstr->length();
        do {
            JSSubString sub;
            size_t skip;
            if (InterpretDollar(res, bp, dp, ep, rdata, &sub, &skip)) {
                if (sub.length > skip)
                    replen += sub.length - skip;
                else
                    replen -= skip - sub.length;
                dp += skip;
            } else {
                dp++;
            }

            dp = js_strchr_limit(dp, '$', ep);
        } while (dp);
    }

    if (!replen.isValid()) {
        ReportAllocationOverflow(cx);
        return false;
    }

    *sizep = replen.value();
    return true;
}

static bool
FindReplaceLength(JSContext* cx, RegExpStatics* res, ReplaceData& rdata, size_t* sizep)
{
    if (rdata.elembase) {
        /*
         * The base object is used when replace was passed a lambda which looks like
         * 'function(a) { return b[a]; }' for the base object b.  b will not change
         * in the course of the replace unless we end up making a scripted call due
         * to accessing a scripted getter or a value with a scripted toString.
         */
        MOZ_ASSERT(rdata.lambda);
        MOZ_ASSERT(!rdata.elembase->getOps()->lookupProperty);
        MOZ_ASSERT(!rdata.elembase->getOps()->getProperty);

        RootedValue match(cx);
        if (!res->createLastMatch(cx, &match))
            return false;
        JSAtom* atom = ToAtom<CanGC>(cx, match);
        if (!atom)
            return false;

        RootedValue v(cx);
        if (HasDataProperty(cx, rdata.elembase, AtomToId(atom), v.address()) && v.isString()) {
            rdata.repstr = v.toString()->ensureLinear(cx);
            if (!rdata.repstr)
                return false;
            *sizep = rdata.repstr->length();
            return true;
        }

        /*
         * Couldn't handle this property, fall through and despecialize to the
         * general lambda case.
         */
        rdata.elembase = nullptr;
    }

    if (rdata.lambda) {
        RootedObject lambda(cx, rdata.lambda);

        /*
         * In the lambda case, not only do we find the replacement string's
         * length, we compute repstr and return it via rdata for use within
         * DoReplace.  The lambda is called with arguments ($&, $1, $2, ...,
         * index, input), i.e., all the properties of a regexp match array.
         * For $&, etc., we must create string jsvals from cx->regExpStatics.
         * We grab up stack space to keep the newborn strings GC-rooted.
         */
        unsigned p = res->getMatches().parenCount();
        unsigned argc = 1 + p + 2;

        InvokeArgs& args = rdata.fig.args();
        if (!args.init(argc))
            return false;

        args.setCallee(ObjectValue(*lambda));
        args.setThis(UndefinedValue());

        /* Push $&, $1, $2, ... */
        unsigned argi = 0;
        if (!res->createLastMatch(cx, args[argi++]))
            return false;

        for (size_t i = 0; i < res->getMatches().parenCount(); ++i) {
            if (!res->createParen(cx, i + 1, args[argi++]))
                return false;
        }

        /* Push match index and input string. */
        args[argi++].setInt32(res->getMatches()[0].start);
        args[argi].setString(rdata.str);

        if (!rdata.fig.invoke(cx))
            return false;

        /* root repstr: rdata is on the stack, so scanned by conservative gc. */
        JSString* repstr = ToString<CanGC>(cx, args.rval());
        if (!repstr)
            return false;
        rdata.repstr = repstr->ensureLinear(cx);
        if (!rdata.repstr)
            return false;
        *sizep = rdata.repstr->length();
        return true;
    }

    return rdata.repstr->hasLatin1Chars()
           ? FindReplaceLengthString<Latin1Char>(cx, res, rdata, sizep)
           : FindReplaceLengthString<char16_t>(cx, res, rdata, sizep);
}

/*
 * Precondition: |rdata.sb| already has necessary growth space reserved (as
 * derived from FindReplaceLength), and has been inflated to TwoByte if
 * necessary.
 */
template <typename CharT>
static void
DoReplace(RegExpStatics* res, ReplaceData& rdata)
{
    AutoCheckCannotGC nogc;
    JSLinearString* repstr = rdata.repstr;
    const CharT* bp = repstr->chars<CharT>(nogc);
    const CharT* cp = bp;

    if (rdata.dollarIndex != UINT32_MAX) {
        MOZ_ASSERT(rdata.dollarIndex < repstr->length());
        const CharT* dp = bp + rdata.dollarIndex;
        const CharT* ep = bp + repstr->length();
        do {
            /* Move one of the constant portions of the replacement value. */
            size_t len = dp - cp;
            rdata.sb.infallibleAppend(cp, len);
            cp = dp;

            JSSubString sub;
            size_t skip;
            if (InterpretDollar(res, bp, dp, ep, rdata, &sub, &skip)) {
                rdata.sb.infallibleAppendSubstring(sub.base, sub.offset, sub.length);
                cp += skip;
                dp += skip;
            } else {
                dp++;
            }

            dp = js_strchr_limit(dp, '$', ep);
        } while (dp);
    }
    rdata.sb.infallibleAppend(cp, repstr->length() - (cp - bp));
}

static bool
ReplaceRegExp(JSContext* cx, RegExpStatics* res, ReplaceData& rdata)
{

    const MatchPair& match = res->getMatches()[0];
    MOZ_ASSERT(!match.isUndefined());
    MOZ_ASSERT(match.limit >= match.start && match.limit >= 0);

    rdata.calledBack = true;
    size_t leftoff = rdata.leftIndex;
    size_t leftlen = match.start - leftoff;
    rdata.leftIndex = match.limit;

    size_t replen = 0;  /* silence 'unused' warning */
    if (!FindReplaceLength(cx, res, rdata, &replen))
        return false;

    CheckedInt<uint32_t> newlen(rdata.sb.length());
    newlen += leftlen;
    newlen += replen;
    if (!newlen.isValid()) {
        ReportAllocationOverflow(cx);
        return false;
    }

    /*
     * Inflate the buffer now if needed, to avoid (fallible) Latin1 to TwoByte
     * inflation later on.
     */
    JSLinearString& str = rdata.str->asLinear();  /* flattened for regexp */
    if (str.hasTwoByteChars() || rdata.repstr->hasTwoByteChars()) {
        if (!rdata.sb.ensureTwoByteChars())
            return false;
    }

    if (!rdata.sb.reserve(newlen.value()))
        return false;

    /* Append skipped-over portion of the search value. */
    rdata.sb.infallibleAppendSubstring(&str, leftoff, leftlen);

    if (rdata.repstr->hasLatin1Chars())
        DoReplace<Latin1Char>(res, rdata);
    else
        DoReplace<char16_t>(res, rdata);
    return true;
}

static JSString*
BuildFlatReplacement(JSContext* cx, HandleString textstr, HandleString repstr,
                     const FlatMatch& fm)
{
    RopeBuilder builder(cx);
    size_t match = fm.match();
    size_t matchEnd = match + fm.patternLength();

    if (textstr->isRope()) {
        /*
         * If we are replacing over a rope, avoid flattening it by iterating
         * through it, building a new rope.
         */
        StringSegmentRange r(cx);
        if (!r.init(textstr))
            return nullptr;

        size_t pos = 0;
        while (!r.empty()) {
            RootedString str(cx, r.front());
            size_t len = str->length();
            size_t strEnd = pos + len;
            if (pos < matchEnd && strEnd > match) {
                /*
                 * We need to special-case any part of the rope that overlaps
                 * with the replacement string.
                 */
                if (match >= pos) {
                    /*
                     * If this part of the rope overlaps with the left side of
                     * the pattern, then it must be the only one to overlap with
                     * the first character in the pattern, so we include the
                     * replacement string here.
                     */
                    RootedString leftSide(cx, NewDependentString(cx, str, 0, match - pos));
                    if (!leftSide ||
                        !builder.append(leftSide) ||
                        !builder.append(repstr))
                    {
                        return nullptr;
                    }
                }

                /*
                 * If str runs off the end of the matched string, append the
                 * last part of str.
                 */
                if (strEnd > matchEnd) {
                    RootedString rightSide(cx, NewDependentString(cx, str, matchEnd - pos,
                                                                  strEnd - matchEnd));
                    if (!rightSide || !builder.append(rightSide))
                        return nullptr;
                }
            } else {
                if (!builder.append(str))
                    return nullptr;
            }
            pos += str->length();
            if (!r.popFront())
                return nullptr;
        }
    } else {
        RootedString leftSide(cx, NewDependentString(cx, textstr, 0, match));
        if (!leftSide)
            return nullptr;
        RootedString rightSide(cx);
        rightSide = NewDependentString(cx, textstr, match + fm.patternLength(),
                                       textstr->length() - match - fm.patternLength());
        if (!rightSide ||
            !builder.append(leftSide) ||
            !builder.append(repstr) ||
            !builder.append(rightSide))
        {
            return nullptr;
        }
    }

    return builder.result();
}

template <typename CharT>
static bool
AppendDollarReplacement(StringBuffer& newReplaceChars, size_t firstDollarIndex,
                        const FlatMatch& fm, JSLinearString* text,
                        const CharT* repChars, size_t repLength)
{
    MOZ_ASSERT(firstDollarIndex < repLength);

    size_t matchStart = fm.match();
    size_t matchLimit = matchStart + fm.patternLength();

    /* Move the pre-dollar chunk in bulk. */
    newReplaceChars.infallibleAppend(repChars, firstDollarIndex);

    /* Move the rest char-by-char, interpreting dollars as we encounter them. */
    const CharT* repLimit = repChars + repLength;
    for (const CharT* it = repChars + firstDollarIndex; it < repLimit; ++it) {
        if (*it != '$' || it == repLimit - 1) {
            if (!newReplaceChars.append(*it))
                return false;
            continue;
        }

        switch (*(it + 1)) {
          case '$': /* Eat one of the dollars. */
            if (!newReplaceChars.append(*it))
                return false;
            break;
          case '&':
            if (!newReplaceChars.appendSubstring(text, matchStart, matchLimit - matchStart))
                return false;
            break;
          case '`':
            if (!newReplaceChars.appendSubstring(text, 0, matchStart))
                return false;
            break;
          case '\'':
            if (!newReplaceChars.appendSubstring(text, matchLimit, text->length() - matchLimit))
                return false;
            break;
          default: /* The dollar we saw was not special (no matter what its mother told it). */
            if (!newReplaceChars.append(*it))
                return false;
            continue;
        }
        ++it; /* We always eat an extra char in the above switch. */
    }

    return true;
}

/*
 * Perform a linear-scan dollar substitution on the replacement text,
 * constructing a result string that looks like:
 *
 *      newstring = string[:matchStart] + dollarSub(replaceValue) + string[matchLimit:]
 */
static JSString*
BuildDollarReplacement(JSContext* cx, JSString* textstrArg, JSLinearString* repstr,
                       uint32_t firstDollarIndex, const FlatMatch& fm)
{
    RootedLinearString textstr(cx, textstrArg->ensureLinear(cx));
    if (!textstr)
        return nullptr;

    size_t matchStart = fm.match();
    size_t matchLimit = matchStart + fm.patternLength();

    /*
     * Most probably:
     *
     *      len(newstr) >= len(orig) - len(match) + len(replacement)
     *
     * Note that dollar vars _could_ make the resulting text smaller than this.
     */
    StringBuffer newReplaceChars(cx);
    if (repstr->hasTwoByteChars() && !newReplaceChars.ensureTwoByteChars())
        return nullptr;

    if (!newReplaceChars.reserve(textstr->length() - fm.patternLength() + repstr->length()))
        return nullptr;

    bool res;
    if (repstr->hasLatin1Chars()) {
        AutoCheckCannotGC nogc;
        res = AppendDollarReplacement(newReplaceChars, firstDollarIndex, fm, textstr,
                                      repstr->latin1Chars(nogc), repstr->length());
    } else {
        AutoCheckCannotGC nogc;
        res = AppendDollarReplacement(newReplaceChars, firstDollarIndex, fm, textstr,
                                      repstr->twoByteChars(nogc), repstr->length());
    }
    if (!res)
        return nullptr;

    RootedString leftSide(cx, NewDependentString(cx, textstr, 0, matchStart));
    if (!leftSide)
        return nullptr;

    RootedString newReplace(cx, newReplaceChars.finishString());
    if (!newReplace)
        return nullptr;

    MOZ_ASSERT(textstr->length() >= matchLimit);
    RootedString rightSide(cx, NewDependentString(cx, textstr, matchLimit,
                                                  textstr->length() - matchLimit));
    if (!rightSide)
        return nullptr;

    RopeBuilder builder(cx);
    if (!builder.append(leftSide) || !builder.append(newReplace) || !builder.append(rightSide))
        return nullptr;

    return builder.result();
}

struct StringRange
{
    size_t start;
    size_t length;

    StringRange(size_t s, size_t l)
      : start(s), length(l)
    { }
};

template <typename CharT>
static void
CopySubstringsToFatInline(JSFatInlineString* dest, const CharT* src, const StringRange* ranges,
                          size_t rangesLen, size_t outputLen)
{
    CharT* buf = dest->init<CharT>(outputLen);
    size_t pos = 0;
    for (size_t i = 0; i < rangesLen; i++) {
        PodCopy(buf + pos, src + ranges[i].start, ranges[i].length);
        pos += ranges[i].length;
    }

    MOZ_ASSERT(pos == outputLen);
    buf[outputLen] = 0;
}

static inline JSFatInlineString*
FlattenSubstrings(JSContext* cx, HandleLinearString str, const StringRange* ranges,
                  size_t rangesLen, size_t outputLen)
{
    JSFatInlineString* result = Allocate<JSFatInlineString>(cx);
    if (!result)
        return nullptr;

    AutoCheckCannotGC nogc;
    if (str->hasLatin1Chars())
        CopySubstringsToFatInline(result, str->latin1Chars(nogc), ranges, rangesLen, outputLen);
    else
        CopySubstringsToFatInline(result, str->twoByteChars(nogc), ranges, rangesLen, outputLen);
    return result;
}

static JSString*
AppendSubstrings(JSContext* cx, HandleLinearString str, const StringRange* ranges,
                 size_t rangesLen)
{
    MOZ_ASSERT(rangesLen);

    /* For single substrings, construct a dependent string. */
    if (rangesLen == 1)
        return NewDependentString(cx, str, ranges[0].start, ranges[0].length);

    bool isLatin1 = str->hasLatin1Chars();
    uint32_t fatInlineMaxLength = JSFatInlineString::MAX_LENGTH_TWO_BYTE;
    if (isLatin1)
        fatInlineMaxLength = JSFatInlineString::MAX_LENGTH_LATIN1;

    /* Collect substrings into a rope */
    size_t i = 0;
    RopeBuilder rope(cx);
    RootedString part(cx, nullptr);
    while (i < rangesLen) {

        /* Find maximum range that fits in JSFatInlineString */
        size_t substrLen = 0;
        size_t end = i;
        for (; end < rangesLen; end++) {
            if (substrLen + ranges[end].length > fatInlineMaxLength)
                break;
            substrLen += ranges[end].length;
        }

        if (i == end) {
            /* Not even one range fits JSFatInlineString, use DependentString */
            const StringRange& sr = ranges[i++];
            part = NewDependentString(cx, str, sr.start, sr.length);
        } else {
            /* Copy the ranges (linearly) into a JSFatInlineString */
            part = FlattenSubstrings(cx, str, ranges + i, end - i, substrLen);
            i = end;
        }

        if (!part)
            return nullptr;

        /* Appending to the rope permanently roots the substring. */
        if (!rope.append(part))
            return nullptr;
    }

    return rope.result();
}

static JSString*
StrReplaceRegexpRemove(JSContext* cx, HandleString str, RegExpShared& re)
{
    RootedLinearString linearStr(cx, str->ensureLinear(cx));
    if (!linearStr)
        return nullptr;

    Vector<StringRange, 16, SystemAllocPolicy> ranges;

    size_t charsLen = linearStr->length();

    ScopedMatchPairs matches(&cx->tempLifoAlloc());
    size_t startIndex = 0; /* Index used for iterating through the string. */
    size_t lastIndex = 0;  /* Index after last successful match. */
    size_t lazyIndex = 0;  /* Index before last successful match. */

    /* Accumulate StringRanges for unmatched substrings. */
    while (startIndex <= charsLen) {
        if (!CheckForInterrupt(cx))
            return nullptr;

        RegExpRunStatus status = re.execute(cx, linearStr, startIndex, &matches);
        if (status == RegExpRunStatus_Error)
            return nullptr;
        if (status == RegExpRunStatus_Success_NotFound)
            break;
        MatchPair& match = matches[0];

        /* Include the latest unmatched substring. */
        if (size_t(match.start) > lastIndex) {
            if (!ranges.append(StringRange(lastIndex, match.start - lastIndex)))
                return nullptr;
        }

        lazyIndex = lastIndex;
        lastIndex = match.limit;

        startIndex = match.isEmpty() ? match.limit + 1 : match.limit;

        /* Non-global removal executes at most once. */
        if (!re.global())
            break;
    }

    RegExpStatics* res;

    /* If unmatched, return the input string. */
    if (!lastIndex) {
        if (startIndex > 0) {
            res = cx->global()->getRegExpStatics(cx);
            if (!res)
                return nullptr;
            res->updateLazily(cx, linearStr, &re, lazyIndex);
        }

        return str;
    }

    /* The last successful match updates the RegExpStatics. */
    res = cx->global()->getRegExpStatics(cx);
    if (!res)
        return nullptr;

    res->updateLazily(cx, linearStr, &re, lazyIndex);

    /* Include any remaining part of the string. */
    if (lastIndex < charsLen) {
        if (!ranges.append(StringRange(lastIndex, charsLen - lastIndex)))
            return nullptr;
    }

    /* Handle the empty string before calling .begin(). */
    if (ranges.empty())
        return cx->runtime()->emptyString;

    return AppendSubstrings(cx, linearStr, ranges.begin(), ranges.length());
}

static inline JSString*
StrReplaceRegExp(JSContext* cx, ReplaceData& rdata)
{
    rdata.leftIndex = 0;
    rdata.calledBack = false;

    RegExpStatics* res = cx->global()->getRegExpStatics(cx);
    if (!res)
        return nullptr;

    RegExpShared& re = rdata.g.regExp();

    // The spec doesn't describe this function very clearly, so we go ahead and
    // assume that when the input to String.prototype.replace is a global
    // RegExp, calling the replacer function (assuming one was provided) takes
    // place only after the matching is done. See the comment at the beginning
    // of DoMatchGlobal explaining why we can zero the the RegExp object's
    // lastIndex property here.
    if (re.global() && !rdata.g.zeroLastIndex(cx))
        return nullptr;

    /* Optimize removal. */
    if (rdata.repstr && rdata.repstr->length() == 0) {
        MOZ_ASSERT(!rdata.lambda && !rdata.elembase && rdata.dollarIndex == UINT32_MAX);
        return StrReplaceRegexpRemove(cx, rdata.str, re);
    }

    RootedLinearString linearStr(cx, rdata.str->ensureLinear(cx));
    if (!linearStr)
        return nullptr;

    size_t rightContextOffset = 0;
    if (re.global()) {
        if (!DoMatchForReplaceGlobal(cx, res, linearStr, re, rdata, &rightContextOffset))
            return nullptr;
    } else {
        if (!DoMatchForReplaceLocal(cx, res, linearStr, re, rdata, &rightContextOffset))
            return nullptr;
    }

    if (!rdata.calledBack) {
        /* Didn't match, so the string is unmodified. */
        return rdata.str;
    }

    MOZ_ASSERT(rightContextOffset <= rdata.str->length());
    size_t length = rdata.str->length() - rightContextOffset;
    if (!rdata.sb.appendSubstring(rdata.str, rightContextOffset, length))
        return nullptr;

    return rdata.sb.finishString();
}

static inline bool
str_replace_regexp(JSContext* cx, const CallArgs& args, ReplaceData& rdata)
{
    if (!rdata.g.normalizeRegExp(cx, true, 2, args))
        return false;

    JSString* res = StrReplaceRegExp(cx, rdata);
    if (!res)
        return false;

    args.rval().setString(res);
    return true;
}

JSString*
js::str_replace_regexp_raw(JSContext* cx, HandleString string, Handle<RegExpObject*> regexp,
                           HandleString replacement)
{
    /* Optimize removal, so we don't have to create ReplaceData */
    if (replacement->length() == 0) {
        StringRegExpGuard guard(cx);
        if (!guard.initRegExp(cx, regexp))
            return nullptr;

        RegExpShared& re = guard.regExp();
        return StrReplaceRegexpRemove(cx, string, re);
    }

    ReplaceData rdata(cx);
    rdata.str = string;

    JSLinearString* repl = replacement->ensureLinear(cx);
    if (!repl)
        return nullptr;

    rdata.setReplacementString(repl);

    if (!rdata.g.initRegExp(cx, regexp))
        return nullptr;

    return StrReplaceRegExp(cx, rdata);
}

static JSString*
StrReplaceString(JSContext* cx, ReplaceData& rdata, const FlatMatch& fm)
{
    /*
     * Note: we could optimize the text.length == pattern.length case if we wanted,
     * even in the presence of dollar metachars.
     */
    if (rdata.dollarIndex != UINT32_MAX)
        return BuildDollarReplacement(cx, rdata.str, rdata.repstr, rdata.dollarIndex, fm);
    return BuildFlatReplacement(cx, rdata.str, rdata.repstr, fm);
}

static const uint32_t ReplaceOptArg = 2;

JSString*
js::str_replace_string_raw(JSContext* cx, HandleString string, HandleString pattern,
                          HandleString replacement)
{
    ReplaceData rdata(cx);

    rdata.str = string;
    JSLinearString* repl = replacement->ensureLinear(cx);
    if (!repl)
        return nullptr;
    rdata.setReplacementString(repl);

    if (!rdata.g.init(cx, pattern))
        return nullptr;
    const FlatMatch* fm = rdata.g.tryFlatMatch(cx, rdata.str, ReplaceOptArg, ReplaceOptArg, false);

    if (fm->match() < 0)
        return string;

    return StrReplaceString(cx, rdata, *fm);
}

static inline bool
str_replace_flat_lambda(JSContext* cx, const CallArgs& outerArgs, ReplaceData& rdata,
                        const FlatMatch& fm)
{
    RootedString matchStr(cx, NewDependentString(cx, rdata.str, fm.match(), fm.patternLength()));
    if (!matchStr)
        return false;

    /* lambda(matchStr, matchStart, textstr) */
    static const uint32_t lambdaArgc = 3;
    if (!rdata.fig.args().init(lambdaArgc))
        return false;

    CallArgs& args = rdata.fig.args();
    args.setCallee(ObjectValue(*rdata.lambda));
    args.setThis(UndefinedValue());

    Value* sp = args.array();
    sp[0].setString(matchStr);
    sp[1].setInt32(fm.match());
    sp[2].setString(rdata.str);

    if (!rdata.fig.invoke(cx))
        return false;

    RootedString repstr(cx, ToString<CanGC>(cx, args.rval()));
    if (!repstr)
        return false;

    RootedString leftSide(cx, NewDependentString(cx, rdata.str, 0, fm.match()));
    if (!leftSide)
        return false;

    size_t matchLimit = fm.match() + fm.patternLength();
    RootedString rightSide(cx, NewDependentString(cx, rdata.str, matchLimit,
                                                  rdata.str->length() - matchLimit));
    if (!rightSide)
        return false;

    RopeBuilder builder(cx);
    if (!(builder.append(leftSide) &&
          builder.append(repstr) &&
          builder.append(rightSide))) {
        return false;
    }

    outerArgs.rval().setString(builder.result());
    return true;
}

/*
 * Pattern match the script to check if it is is indexing into a particular
 * object, e.g. 'function(a) { return b[a]; }'. Avoid calling the script in
 * such cases, which are used by javascript packers (particularly the popular
 * Dean Edwards packer) to efficiently encode large scripts. We only handle the
 * code patterns generated by such packers here.
 */
static bool
LambdaIsGetElem(JSContext* cx, JSObject& lambda, MutableHandleNativeObject pobj)
{
    if (!lambda.is<JSFunction>())
        return true;

    RootedFunction fun(cx, &lambda.as<JSFunction>());
    if (!fun->isInterpreted() || fun->isClassConstructor())
        return true;

    JSScript* script = fun->getOrCreateScript(cx);
    if (!script)
        return false;

    jsbytecode* pc = script->code();

    /*
     * JSOP_GETALIASEDVAR tells us exactly where to find the base object 'b'.
     * Rule out the (unlikely) possibility of a function with a call object
     * since it would make our scope walk off by 1.
     */
    if (JSOp(*pc) != JSOP_GETALIASEDVAR || fun->needsCallObject())
        return true;
    ScopeCoordinate sc(pc);
    ScopeObject* scope = &fun->environment()->as<ScopeObject>();
    for (unsigned i = 0; i < sc.hops(); ++i)
        scope = &scope->enclosingScope().as<ScopeObject>();
    Value b = scope->aliasedVar(sc);
    pc += JSOP_GETALIASEDVAR_LENGTH;

    /* Look for 'a' to be the lambda's first argument. */
    if (JSOp(*pc) != JSOP_GETARG || GET_ARGNO(pc) != 0)
        return true;
    pc += JSOP_GETARG_LENGTH;

    /* 'b[a]' */
    if (JSOp(*pc) != JSOP_GETELEM)
        return true;
    pc += JSOP_GETELEM_LENGTH;

    /* 'return b[a]' */
    if (JSOp(*pc) != JSOP_RETURN)
        return true;

    /* 'b' must behave like a normal object. */
    if (!b.isObject())
        return true;

    JSObject& bobj = b.toObject();
    const Class* clasp = bobj.getClass();
    if (!clasp->isNative() || clasp->ops.lookupProperty || clasp->ops.getProperty)
        return true;

    pobj.set(&bobj.as<NativeObject>());
    return true;
}

bool
js::str_replace(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    ReplaceData rdata(cx);
    rdata.str = ThisToStringForStringProto(cx, args);
    if (!rdata.str)
        return false;

    if (!rdata.g.init(cx, args))
        return false;

    /* Extract replacement string/function. */
    if (args.length() >= ReplaceOptArg && IsCallable(args[1])) {
        rdata.setReplacementFunction(&args[1].toObject());

        if (!LambdaIsGetElem(cx, *rdata.lambda, &rdata.elembase))
            return false;
    } else {
        JSLinearString* string = ArgToRootedString(cx, args, 1);
        if (!string)
            return false;

        rdata.setReplacementString(string);
    }

    rdata.fig.initFunction(ObjectOrNullValue(rdata.lambda));

    /*
     * Unlike its |String.prototype| brethren, |replace| doesn't convert
     * its input to a regular expression. (Even if it contains metachars.)
     *
     * However, if the user invokes our (non-standard) |flags| argument
     * extension then we revert to creating a regular expression. Note that
     * this is observable behavior through the side-effect mutation of the
     * |RegExp| statics.
     */

    const FlatMatch* fm = rdata.g.tryFlatMatch(cx, rdata.str, ReplaceOptArg, args.length(), false);

    if (!fm) {
        if (cx->isExceptionPending())  /* oom in RopeMatch in tryFlatMatch */
            return false;
        return str_replace_regexp(cx, args, rdata);
    }

    if (fm->match() < 0) {
        args.rval().setString(rdata.str);
        return true;
    }

    if (rdata.lambda)
        return str_replace_flat_lambda(cx, args, rdata, *fm);

    JSString* res = StrReplaceString(cx, rdata, *fm);
    if (!res)
        return false;

    args.rval().setString(res);
    return true;
}

namespace {

class SplitMatchResult {
    size_t endIndex_;
    size_t length_;

  public:
    void setFailure() {
        JS_STATIC_ASSERT(SIZE_MAX > JSString::MAX_LENGTH);
        endIndex_ = SIZE_MAX;
    }
    bool isFailure() const {
        return endIndex_ == SIZE_MAX;
    }
    size_t endIndex() const {
        MOZ_ASSERT(!isFailure());
        return endIndex_;
    }
    size_t length() const {
        MOZ_ASSERT(!isFailure());
        return length_;
    }
    void setResult(size_t length, size_t endIndex) {
        length_ = length;
        endIndex_ = endIndex;
    }
};

} /* anonymous namespace */

template<class Matcher>
static JSObject*
SplitHelper(JSContext* cx, HandleLinearString str, uint32_t limit, const Matcher& splitMatch,
            HandleObjectGroup group)
{
    size_t strLength = str->length();
    SplitMatchResult result;

    /* Step 11. */
    if (strLength == 0) {
        if (!splitMatch(cx, str, 0, &result))
            return nullptr;

        /*
         * NB: Unlike in the non-empty string case, it's perfectly fine
         *     (indeed the spec requires it) if we match at the end of the
         *     string.  Thus these cases should hold:
         *
         *   var a = "".split("");
         *   assertEq(a.length, 0);
         *   var b = "".split(/.?/);
         *   assertEq(b.length, 0);
         */
        if (!result.isFailure())
            return NewFullyAllocatedArrayTryUseGroup(cx, group, 0);

        RootedValue v(cx, StringValue(str));
        return NewCopiedArrayTryUseGroup(cx, group, v.address(), 1);
    }

    /* Step 12. */
    size_t lastEndIndex = 0;
    size_t index = 0;

    /* Step 13. */
    AutoValueVector splits(cx);

    while (index < strLength) {
        /* Step 13(a). */
        if (!splitMatch(cx, str, index, &result))
            return nullptr;

        /*
         * Step 13(b).
         *
         * Our match algorithm differs from the spec in that it returns the
         * next index at which a match happens.  If no match happens we're
         * done.
         *
         * But what if the match is at the end of the string (and the string is
         * not empty)?  Per 13(c)(ii) this shouldn't be a match, so we have to
         * specially exclude it.  Thus this case should hold:
         *
         *   var a = "abc".split(/\b/);
         *   assertEq(a.length, 1);
         *   assertEq(a[0], "abc");
         */
        if (result.isFailure())
            break;

        /* Step 13(c)(i). */
        size_t sepLength = result.length();
        size_t endIndex = result.endIndex();
        if (sepLength == 0 && endIndex == strLength)
            break;

        /* Step 13(c)(ii). */
        if (endIndex == lastEndIndex) {
            index++;
            continue;
        }

        /* Step 13(c)(iii). */
        MOZ_ASSERT(lastEndIndex < endIndex);
        MOZ_ASSERT(sepLength <= strLength);
        MOZ_ASSERT(lastEndIndex + sepLength <= endIndex);

        /* Steps 13(c)(iii)(1-3). */
        size_t subLength = size_t(endIndex - sepLength - lastEndIndex);
        JSString* sub = NewDependentString(cx, str, lastEndIndex, subLength);
        if (!sub || !splits.append(StringValue(sub)))
            return nullptr;

        /* Step 13(c)(iii)(4). */
        if (splits.length() == limit)
            return NewCopiedArrayTryUseGroup(cx, group, splits.begin(), splits.length());

        /* Step 13(c)(iii)(5). */
        lastEndIndex = endIndex;

        /* Step 13(c)(iii)(6-7). */
        if (Matcher::returnsCaptures) {
            RegExpStatics* res = cx->global()->getRegExpStatics(cx);
            if (!res)
                return nullptr;

            const MatchPairs& matches = res->getMatches();
            for (size_t i = 0; i < matches.parenCount(); i++) {
                /* Steps 13(c)(iii)(7)(a-c). */
                if (!matches[i + 1].isUndefined()) {
                    JSSubString parsub;
                    res->getParen(i + 1, &parsub);
                    sub = NewDependentString(cx, parsub.base, parsub.offset, parsub.length);
                    if (!sub || !splits.append(StringValue(sub)))
                        return nullptr;
                } else {
                    if (!splits.append(UndefinedValue()))
                        return nullptr;
                }

                /* Step 13(c)(iii)(7)(d). */
                if (splits.length() == limit)
                    return NewCopiedArrayTryUseGroup(cx, group, splits.begin(), splits.length());
            }
        }

        /* Step 13(c)(iii)(8). */
        index = lastEndIndex;
    }

    /* Steps 14-15. */
    JSString* sub = NewDependentString(cx, str, lastEndIndex, strLength - lastEndIndex);
    if (!sub || !splits.append(StringValue(sub)))
        return nullptr;

    /* Step 16. */
    return NewCopiedArrayTryUseGroup(cx, group, splits.begin(), splits.length());
}

// Fast-path for splitting a string into a character array via split("").
static JSObject*
CharSplitHelper(JSContext* cx, HandleLinearString str, uint32_t limit, HandleObjectGroup group)
{
    size_t strLength = str->length();
    if (strLength == 0)
        return NewFullyAllocatedArrayTryUseGroup(cx, group, 0);

    js::StaticStrings& staticStrings = cx->staticStrings();
    uint32_t resultlen = (limit < strLength ? limit : strLength);

    AutoValueVector splits(cx);
    if (!splits.reserve(resultlen))
        return nullptr;

    for (size_t i = 0; i < resultlen; ++i) {
        JSString* sub = staticStrings.getUnitStringForElement(cx, str, i);
        if (!sub)
            return nullptr;
        splits.infallibleAppend(StringValue(sub));
    }

    return NewCopiedArrayTryUseGroup(cx, group, splits.begin(), splits.length());
}

namespace {

/*
 * The SplitMatch operation from ES5 15.5.4.14 is implemented using different
 * paths for regular expression and string separators.
 *
 * The algorithm differs from the spec in that the we return the next index at
 * which a match happens.
 */
class SplitRegExpMatcher
{
    RegExpShared& re;
    RegExpStatics* res;

  public:
    SplitRegExpMatcher(RegExpShared& re, RegExpStatics* res) : re(re), res(res) {}

    static const bool returnsCaptures = true;

    bool operator()(JSContext* cx, HandleLinearString str, size_t index,
                    SplitMatchResult* result) const
    {
        ScopedMatchPairs matches(&cx->tempLifoAlloc());
        RegExpRunStatus status = re.execute(cx, str, index, &matches);
        if (status == RegExpRunStatus_Error)
            return false;

        if (status == RegExpRunStatus_Success_NotFound) {
            result->setFailure();
            return true;
        }

        if (!res->updateFromMatchPairs(cx, str, matches))
            return false;

        JSSubString sep;
        res->getLastMatch(&sep);

        result->setResult(sep.length, matches[0].limit);
        return true;
    }
};

class SplitStringMatcher
{
    RootedLinearString sep;

  public:
    SplitStringMatcher(JSContext* cx, HandleLinearString sep)
      : sep(cx, sep)
    {}

    static const bool returnsCaptures = false;

    bool operator()(JSContext* cx, JSLinearString* str, size_t index, SplitMatchResult* res) const
    {
        MOZ_ASSERT(index == 0 || index < str->length());
        int match = StringMatch(str, sep, index);
        if (match == -1)
            res->setFailure();
        else
            res->setResult(sep->length(), match + sep->length());
        return true;
    }
};

} /* anonymous namespace */

/* ES5 15.5.4.14 */
bool
js::str_split(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Steps 1-2. */
    RootedString str(cx, ThisToStringForStringProto(cx, args));
    if (!str)
        return false;

    RootedObjectGroup group(cx, ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array));
    if (!group)
        return false;

    /* Step 5: Use the second argument as the split limit, if given. */
    uint32_t limit;
    if (args.hasDefined(1)) {
        double d;
        if (!ToNumber(cx, args[1], &d))
            return false;
        limit = ToUint32(d);
    } else {
        limit = UINT32_MAX;
    }

    /* Step 8. */
    RegExpGuard re(cx);
    RootedLinearString sepstr(cx);
    bool sepDefined = args.hasDefined(0);
    if (sepDefined) {
        ESClassValue cls;
        if (!GetClassOfValue(cx, args[0], &cls))
            return false;

        if (cls == ESClass_RegExp) {
            RootedObject obj(cx, &args[0].toObject());
            if (!RegExpToShared(cx, obj, &re))
                return false;
        } else {
            sepstr = ArgToRootedString(cx, args, 0);
            if (!sepstr)
                return false;
        }
    }

    /* Step 9. */
    if (limit == 0) {
        JSObject* aobj = NewFullyAllocatedArrayTryUseGroup(cx, group, 0);
        if (!aobj)
            return false;
        args.rval().setObject(*aobj);
        return true;
    }

    /* Step 10. */
    if (!sepDefined) {
        RootedValue v(cx, StringValue(str));
        JSObject* aobj = NewCopiedArrayTryUseGroup(cx, group, v.address(), 1);
        if (!aobj)
            return false;
        args.rval().setObject(*aobj);
        return true;
    }
    RootedLinearString linearStr(cx, str->ensureLinear(cx));
    if (!linearStr)
        return false;

    /* Steps 11-15. */
    RootedObject aobj(cx);
    if (!re.initialized()) {
        if (sepstr->length() == 0) {
            aobj = CharSplitHelper(cx, linearStr, limit, group);
        } else {
            SplitStringMatcher matcher(cx, sepstr);
            aobj = SplitHelper(cx, linearStr, limit, matcher, group);
        }
    } else {
        RegExpStatics* res = cx->global()->getRegExpStatics(cx);
        if (!res)
            return false;
        SplitRegExpMatcher matcher(*re, res);
        aobj = SplitHelper(cx, linearStr, limit, matcher, group);
    }
    if (!aobj)
        return false;

    /* Step 16. */
    MOZ_ASSERT(aobj->group() == group);
    args.rval().setObject(*aobj);
    return true;
}

JSObject*
js::str_split_string(JSContext* cx, HandleObjectGroup group, HandleString str, HandleString sep)
{
    RootedLinearString linearStr(cx, str->ensureLinear(cx));
    if (!linearStr)
        return nullptr;

    RootedLinearString linearSep(cx, sep->ensureLinear(cx));
    if (!linearSep)
        return nullptr;

    uint32_t limit = UINT32_MAX;

    if (linearSep->length() == 0)
        return CharSplitHelper(cx, linearStr, limit, group);

    SplitStringMatcher matcher(cx, linearSep);
    return SplitHelper(cx, linearStr, limit, matcher, group);
}

/*
 * Python-esque sequence operations.
 */
static bool
str_concat(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JSString* str = ThisToStringForStringProto(cx, args);
    if (!str)
        return false;

    for (unsigned i = 0; i < args.length(); i++) {
        JSString* argStr = ToString<NoGC>(cx, args[i]);
        if (!argStr) {
            RootedString strRoot(cx, str);
            argStr = ToString<CanGC>(cx, args[i]);
            if (!argStr)
                return false;
            str = strRoot;
        }

        JSString* next = ConcatStrings<NoGC>(cx, str, argStr);
        if (next) {
            str = next;
        } else {
            RootedString strRoot(cx, str), argStrRoot(cx, argStr);
            str = ConcatStrings<CanGC>(cx, strRoot, argStrRoot);
            if (!str)
                return false;
        }
    }

    args.rval().setString(str);
    return true;
}

static const JSFunctionSpec string_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,     str_toSource,          0,0),
#endif

    /* Java-like methods. */
    JS_FN(js_toString_str,     str_toString,          0,0),
    JS_FN(js_valueOf_str,      str_toString,          0,0),
    JS_FN("toLowerCase",       str_toLowerCase,       0,JSFUN_GENERIC_NATIVE),
    JS_FN("toUpperCase",       str_toUpperCase,       0,JSFUN_GENERIC_NATIVE),
    JS_INLINABLE_FN("charAt",  str_charAt,            1,JSFUN_GENERIC_NATIVE, StringCharAt),
    JS_INLINABLE_FN("charCodeAt", str_charCodeAt,     1,JSFUN_GENERIC_NATIVE, StringCharCodeAt),
    JS_SELF_HOSTED_FN("substring", "String_substring", 2,0),
    JS_SELF_HOSTED_FN("codePointAt", "String_codePointAt", 1,0),
    JS_FN("includes",          str_includes,          1,JSFUN_GENERIC_NATIVE),
    JS_FN("contains",          str_contains,          1,JSFUN_GENERIC_NATIVE),
    JS_FN("indexOf",           str_indexOf,           1,JSFUN_GENERIC_NATIVE),
    JS_FN("lastIndexOf",       str_lastIndexOf,       1,JSFUN_GENERIC_NATIVE),
    JS_FN("startsWith",        str_startsWith,        1,JSFUN_GENERIC_NATIVE),
    JS_FN("endsWith",          str_endsWith,          1,JSFUN_GENERIC_NATIVE),
    JS_FN("trim",              str_trim,              0,JSFUN_GENERIC_NATIVE),
    JS_FN("trimLeft",          str_trimLeft,          0,JSFUN_GENERIC_NATIVE),
    JS_FN("trimRight",         str_trimRight,         0,JSFUN_GENERIC_NATIVE),
    JS_FN("toLocaleLowerCase", str_toLocaleLowerCase, 0,JSFUN_GENERIC_NATIVE),
    JS_FN("toLocaleUpperCase", str_toLocaleUpperCase, 0,JSFUN_GENERIC_NATIVE),
#if EXPOSE_INTL_API
    JS_SELF_HOSTED_FN("localeCompare", "String_localeCompare", 1,0),
#else
    JS_FN("localeCompare",     str_localeCompare,     1,JSFUN_GENERIC_NATIVE),
#endif
    JS_SELF_HOSTED_FN("repeat", "String_repeat",      1,0),
#if EXPOSE_INTL_API
    JS_FN("normalize",         str_normalize,         0,JSFUN_GENERIC_NATIVE),
#endif

    /* Perl-ish methods (search is actually Python-esque). */
    JS_FN("match",             str_match,             1,JSFUN_GENERIC_NATIVE),
    JS_FN("search",            str_search,            1,JSFUN_GENERIC_NATIVE),
    JS_INLINABLE_FN("replace", str_replace,           2,JSFUN_GENERIC_NATIVE, StringReplace),
    JS_INLINABLE_FN("split",   str_split,             2,JSFUN_GENERIC_NATIVE, StringSplit),
    JS_SELF_HOSTED_FN("substr", "String_substr",      2,0),

    /* Python-esque sequence methods. */
    JS_FN("concat",            str_concat,            1,JSFUN_GENERIC_NATIVE),
    JS_SELF_HOSTED_FN("slice", "String_slice",        2,0),

    /* HTML string methods. */
    JS_SELF_HOSTED_FN("bold",     "String_bold",       0,0),
    JS_SELF_HOSTED_FN("italics",  "String_italics",    0,0),
    JS_SELF_HOSTED_FN("fixed",    "String_fixed",      0,0),
    JS_SELF_HOSTED_FN("strike",   "String_strike",     0,0),
    JS_SELF_HOSTED_FN("small",    "String_small",      0,0),
    JS_SELF_HOSTED_FN("big",      "String_big",        0,0),
    JS_SELF_HOSTED_FN("blink",    "String_blink",      0,0),
    JS_SELF_HOSTED_FN("sup",      "String_sup",        0,0),
    JS_SELF_HOSTED_FN("sub",      "String_sub",        0,0),
    JS_SELF_HOSTED_FN("anchor",   "String_anchor",     1,0),
    JS_SELF_HOSTED_FN("link",     "String_link",       1,0),
    JS_SELF_HOSTED_FN("fontcolor","String_fontcolor",  1,0),
    JS_SELF_HOSTED_FN("fontsize", "String_fontsize",   1,0),

    JS_SELF_HOSTED_SYM_FN(iterator, "String_iterator", 0,0),
    JS_FS_END
};

// ES6 rev 27 (2014 Aug 24) 21.1.1
bool
js::StringConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedString str(cx);
    if (args.length() > 0) {
        if (!args.isConstructing() && args[0].isSymbol())
            return js::SymbolDescriptiveString(cx, args[0].toSymbol(), args.rval());

        str = ToString<CanGC>(cx, args[0]);
        if (!str)
            return false;
    } else {
        str = cx->runtime()->emptyString;
    }

    if (args.isConstructing()) {
        RootedObject proto(cx);
        RootedObject newTarget(cx, &args.newTarget().toObject());
        if (!GetPrototypeFromConstructor(cx, newTarget, &proto))
            return false;

        StringObject* strobj = StringObject::create(cx, str, proto);
        if (!strobj)
            return false;
        args.rval().setObject(*strobj);
        return true;
    }

    args.rval().setString(str);
    return true;
}

static bool
str_fromCharCode_few_args(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(args.length() <= JSFatInlineString::MAX_LENGTH_TWO_BYTE);

    char16_t chars[JSFatInlineString::MAX_LENGTH_TWO_BYTE];
    for (unsigned i = 0; i < args.length(); i++) {
        uint16_t code;
        if (!ToUint16(cx, args[i], &code))
            return false;
        chars[i] = char16_t(code);
    }
    JSString* str = NewStringCopyN<CanGC>(cx, chars, args.length());
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

bool
js::str_fromCharCode(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    MOZ_ASSERT(args.length() <= ARGS_LENGTH_MAX);

    // Optimize the single-char case.
    if (args.length() == 1)
        return str_fromCharCode_one_arg(cx, args[0], args.rval());

    // Optimize the case where the result will definitely fit in an inline
    // string (thin or fat) and so we don't need to malloc the chars. (We could
    // cover some cases where args.length() goes up to
    // JSFatInlineString::MAX_LENGTH_LATIN1 if we also checked if the chars are
    // all Latin1, but it doesn't seem worth the effort.)
    if (args.length() <= JSFatInlineString::MAX_LENGTH_TWO_BYTE)
        return str_fromCharCode_few_args(cx, args);

    char16_t* chars = cx->pod_malloc<char16_t>(args.length() + 1);
    if (!chars)
        return false;
    for (unsigned i = 0; i < args.length(); i++) {
        uint16_t code;
        if (!ToUint16(cx, args[i], &code)) {
            js_free(chars);
            return false;
        }
        chars[i] = char16_t(code);
    }
    chars[args.length()] = 0;
    JSString* str = NewString<CanGC>(cx, chars, args.length());
    if (!str) {
        js_free(chars);
        return false;
    }

    args.rval().setString(str);
    return true;
}

bool
js::str_fromCharCode_one_arg(JSContext* cx, HandleValue code, MutableHandleValue rval)
{
    uint16_t ucode;

    if (!ToUint16(cx, code, &ucode))
        return false;

    if (StaticStrings::hasUnit(ucode)) {
        rval.setString(cx->staticStrings().getUnit(ucode));
        return true;
    }

    char16_t c = char16_t(ucode);
    JSString* str = NewStringCopyN<CanGC>(cx, &c, 1);
    if (!str)
        return false;

    rval.setString(str);
    return true;
}

static const JSFunctionSpec string_static_methods[] = {
    JS_INLINABLE_FN("fromCharCode", js::str_fromCharCode, 1, 0, StringFromCharCode),

    JS_SELF_HOSTED_FN("fromCodePoint",   "String_static_fromCodePoint", 1,0),
    JS_SELF_HOSTED_FN("raw",             "String_static_raw",           2,0),
    JS_SELF_HOSTED_FN("substring",       "String_static_substring",     3,0),
    JS_SELF_HOSTED_FN("substr",          "String_static_substr",        3,0),
    JS_SELF_HOSTED_FN("slice",           "String_static_slice",         3,0),

    // This must be at the end because of bug 853075: functions listed after
    // self-hosted methods aren't available in self-hosted code.
#if EXPOSE_INTL_API
    JS_SELF_HOSTED_FN("localeCompare",   "String_static_localeCompare", 2,0),
#endif
    JS_FS_END
};

/* static */ Shape*
StringObject::assignInitialShape(ExclusiveContext* cx, Handle<StringObject*> obj)
{
    MOZ_ASSERT(obj->empty());

    return obj->addDataProperty(cx, cx->names().length, LENGTH_SLOT,
                                JSPROP_PERMANENT | JSPROP_READONLY);
}

JSObject*
js::InitStringClass(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->isNative());

    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());

    Rooted<JSString*> empty(cx, cx->runtime()->emptyString);
    RootedObject proto(cx, global->createBlankPrototype(cx, &StringObject::class_));
    if (!proto || !proto->as<StringObject>().init(cx, empty))
        return nullptr;

    /* Now create the String function. */
    RootedFunction ctor(cx);
    ctor = global->createConstructor(cx, StringConstructor, cx->names().String, 1,
                                     AllocKind::FUNCTION, &jit::JitInfo_String);
    if (!ctor)
        return nullptr;

    if (!GlobalObject::initBuiltinConstructor(cx, global, JSProto_String, ctor, proto))
        return nullptr;

    if (!LinkConstructorAndPrototype(cx, ctor, proto))
        return nullptr;

    if (!DefinePropertiesAndFunctions(cx, proto, nullptr, string_methods) ||
        !DefinePropertiesAndFunctions(cx, ctor, nullptr, string_static_methods))
    {
        return nullptr;
    }

    /*
     * Define escape/unescape, the URI encode/decode functions, and maybe
     * uneval on the global object.
     */
    if (!JS_DefineFunctions(cx, global, string_functions))
        return nullptr;

    return proto;
}

const char*
js::ValueToPrintable(JSContext* cx, const Value& vArg, JSAutoByteString* bytes, bool asSource)
{
    RootedValue v(cx, vArg);
    JSString* str;
    if (asSource)
        str = ValueToSource(cx, v);
    else
        str = ToString<CanGC>(cx, v);
    if (!str)
        return nullptr;
    str = QuoteString(cx, str, 0);
    if (!str)
        return nullptr;
    return bytes->encodeLatin1(cx, str);
}

template <AllowGC allowGC>
JSString*
js::ToStringSlow(ExclusiveContext* cx, typename MaybeRooted<Value, allowGC>::HandleType arg)
{
    /* As with ToObjectSlow, callers must verify that |arg| isn't a string. */
    MOZ_ASSERT(!arg.isString());

    Value v = arg;
    if (!v.isPrimitive()) {
        if (!cx->shouldBeJSContext() || !allowGC)
            return nullptr;
        RootedValue v2(cx, v);
        if (!ToPrimitive(cx->asJSContext(), JSTYPE_STRING, &v2))
            return nullptr;
        v = v2;
    }

    JSString* str;
    if (v.isString()) {
        str = v.toString();
    } else if (v.isInt32()) {
        str = Int32ToString<allowGC>(cx, v.toInt32());
    } else if (v.isDouble()) {
        str = NumberToString<allowGC>(cx, v.toDouble());
    } else if (v.isBoolean()) {
        str = BooleanToString(cx, v.toBoolean());
    } else if (v.isNull()) {
        str = cx->names().null;
    } else if (v.isSymbol()) {
        if (cx->shouldBeJSContext() && allowGC) {
            JS_ReportErrorNumber(cx->asJSContext(), GetErrorMessage, nullptr,
                                 JSMSG_SYMBOL_TO_STRING);
        }
        return nullptr;
    } else {
        MOZ_ASSERT(v.isUndefined());
        str = cx->names().undefined;
    }
    return str;
}

template JSString*
js::ToStringSlow<CanGC>(ExclusiveContext* cx, HandleValue arg);

template JSString*
js::ToStringSlow<NoGC>(ExclusiveContext* cx, Value arg);

JS_PUBLIC_API(JSString*)
js::ToStringSlow(JSContext* cx, HandleValue v)
{
    return ToStringSlow<CanGC>(cx, v);
}

static JSString*
SymbolToSource(JSContext* cx, Symbol* symbol)
{
    RootedString desc(cx, symbol->description());
    SymbolCode code = symbol->code();
    if (code != SymbolCode::InSymbolRegistry && code != SymbolCode::UniqueSymbol) {
        // Well-known symbol.
        MOZ_ASSERT(uint32_t(code) < JS::WellKnownSymbolLimit);
        return desc;
    }

    StringBuffer buf(cx);
    if (code == SymbolCode::InSymbolRegistry ? !buf.append("Symbol.for(") : !buf.append("Symbol("))
        return nullptr;
    if (desc) {
        desc = StringToSource(cx, desc);
        if (!desc || !buf.append(desc))
            return nullptr;
    }
    if (!buf.append(')'))
        return nullptr;
    return buf.finishString();
}

JSString*
js::ValueToSource(JSContext* cx, HandleValue v)
{
    JS_CHECK_RECURSION(cx, return nullptr);
    assertSameCompartment(cx, v);

    if (v.isUndefined())
        return cx->names().void0;
    if (v.isString())
        return StringToSource(cx, v.toString());
    if (v.isSymbol())
        return SymbolToSource(cx, v.toSymbol());
    if (v.isPrimitive()) {
        /* Special case to preserve negative zero, _contra_ toString. */
        if (v.isDouble() && IsNegativeZero(v.toDouble())) {
            /* NB: _ucNstr rather than _ucstr to indicate non-terminated. */
            static const char16_t js_negzero_ucNstr[] = {'-', '0'};

            return NewStringCopyN<CanGC>(cx, js_negzero_ucNstr, 2);
        }
        return ToString<CanGC>(cx, v);
    }

    RootedValue fval(cx);
    RootedObject obj(cx, &v.toObject());
    if (!GetProperty(cx, obj, obj, cx->names().toSource, &fval))
        return nullptr;
    if (IsCallable(fval)) {
        RootedValue rval(cx);
        if (!Invoke(cx, ObjectValue(*obj), fval, 0, nullptr, &rval))
            return nullptr;
        return ToString<CanGC>(cx, rval);
    }

    return ObjectToSource(cx, obj);
}

JSString*
js::StringToSource(JSContext* cx, JSString* str)
{
    return QuoteString(cx, str, '"');
}

bool
js::EqualChars(JSLinearString* str1, JSLinearString* str2)
{
    MOZ_ASSERT(str1->length() == str2->length());

    size_t len = str1->length();

    AutoCheckCannotGC nogc;
    if (str1->hasTwoByteChars()) {
        if (str2->hasTwoByteChars())
            return PodEqual(str1->twoByteChars(nogc), str2->twoByteChars(nogc), len);

        return EqualChars(str2->latin1Chars(nogc), str1->twoByteChars(nogc), len);
    }

    if (str2->hasLatin1Chars())
        return PodEqual(str1->latin1Chars(nogc), str2->latin1Chars(nogc), len);

    return EqualChars(str1->latin1Chars(nogc), str2->twoByteChars(nogc), len);
}

bool
js::EqualStrings(JSContext* cx, JSString* str1, JSString* str2, bool* result)
{
    if (str1 == str2) {
        *result = true;
        return true;
    }

    size_t length1 = str1->length();
    if (length1 != str2->length()) {
        *result = false;
        return true;
    }

    JSLinearString* linear1 = str1->ensureLinear(cx);
    if (!linear1)
        return false;
    JSLinearString* linear2 = str2->ensureLinear(cx);
    if (!linear2)
        return false;

    *result = EqualChars(linear1, linear2);
    return true;
}

bool
js::EqualStrings(JSLinearString* str1, JSLinearString* str2)
{
    if (str1 == str2)
        return true;

    size_t length1 = str1->length();
    if (length1 != str2->length())
        return false;

    return EqualChars(str1, str2);
}

static int32_t
CompareStringsImpl(JSLinearString* str1, JSLinearString* str2)
{
    size_t len1 = str1->length();
    size_t len2 = str2->length();

    AutoCheckCannotGC nogc;
    if (str1->hasLatin1Chars()) {
        const Latin1Char* chars1 = str1->latin1Chars(nogc);
        return str2->hasLatin1Chars()
               ? CompareChars(chars1, len1, str2->latin1Chars(nogc), len2)
               : CompareChars(chars1, len1, str2->twoByteChars(nogc), len2);
    }

    const char16_t* chars1 = str1->twoByteChars(nogc);
    return str2->hasLatin1Chars()
           ? CompareChars(chars1, len1, str2->latin1Chars(nogc), len2)
           : CompareChars(chars1, len1, str2->twoByteChars(nogc), len2);
}

int32_t
js::CompareChars(const char16_t* s1, size_t len1, JSLinearString* s2)
{
    AutoCheckCannotGC nogc;
    return s2->hasLatin1Chars()
           ? CompareChars(s1, len1, s2->latin1Chars(nogc), s2->length())
           : CompareChars(s1, len1, s2->twoByteChars(nogc), s2->length());
}

bool
js::CompareStrings(JSContext* cx, JSString* str1, JSString* str2, int32_t* result)
{
    MOZ_ASSERT(str1);
    MOZ_ASSERT(str2);

    if (str1 == str2) {
        *result = 0;
        return true;
    }

    JSLinearString* linear1 = str1->ensureLinear(cx);
    if (!linear1)
        return false;

    JSLinearString* linear2 = str2->ensureLinear(cx);
    if (!linear2)
        return false;

    *result = CompareStringsImpl(linear1, linear2);
    return true;
}

int32_t
js::CompareAtoms(JSAtom* atom1, JSAtom* atom2)
{
    return CompareStringsImpl(atom1, atom2);
}

bool
js::StringEqualsAscii(JSLinearString* str, const char* asciiBytes)
{
    size_t length = strlen(asciiBytes);
#ifdef DEBUG
    for (size_t i = 0; i != length; ++i)
        MOZ_ASSERT(unsigned(asciiBytes[i]) <= 127);
#endif
    if (length != str->length())
        return false;

    const Latin1Char* latin1 = reinterpret_cast<const Latin1Char*>(asciiBytes);

    AutoCheckCannotGC nogc;
    return str->hasLatin1Chars()
           ? PodEqual(latin1, str->latin1Chars(nogc), length)
           : EqualChars(latin1, str->twoByteChars(nogc), length);
}

size_t
js_strlen(const char16_t* s)
{
    const char16_t* t;

    for (t = s; *t != 0; t++)
        continue;
    return (size_t)(t - s);
}

int32_t
js_strcmp(const char16_t* lhs, const char16_t* rhs)
{
    while (true) {
        if (*lhs != *rhs)
            return int32_t(*lhs) - int32_t(*rhs);
        if (*lhs == 0)
            return 0;
        ++lhs, ++rhs;
    }
}

UniquePtr<char[], JS::FreePolicy>
js::DuplicateString(js::ExclusiveContext* cx, const char* s)
{
    size_t n = strlen(s) + 1;
    auto ret = cx->make_pod_array<char>(n);
    if (!ret)
        return ret;
    PodCopy(ret.get(), s, n);
    return ret;
}

UniquePtr<char16_t[], JS::FreePolicy>
js::DuplicateString(js::ExclusiveContext* cx, const char16_t* s)
{
    size_t n = js_strlen(s) + 1;
    auto ret = cx->make_pod_array<char16_t>(n);
    if (!ret)
        return ret;
    PodCopy(ret.get(), s, n);
    return ret;
}

UniquePtr<char16_t[], JS::FreePolicy>
js::DuplicateString(const char16_t* s)
{
    size_t n = js_strlen(s) + 1;
    UniquePtr<char16_t[], JS::FreePolicy> ret(js_pod_malloc<char16_t>(n));
    if (!ret)
        return nullptr;
    PodCopy(ret.get(), s, n);
    return ret;
}

template <typename CharT>
const CharT*
js_strchr_limit(const CharT* s, char16_t c, const CharT* limit)
{
    while (s < limit) {
        if (*s == c)
            return s;
        s++;
    }
    return nullptr;
}

template const Latin1Char*
js_strchr_limit(const Latin1Char* s, char16_t c, const Latin1Char* limit);

template const char16_t*
js_strchr_limit(const char16_t* s, char16_t c, const char16_t* limit);

char16_t*
js::InflateString(ExclusiveContext* cx, const char* bytes, size_t* lengthp)
{
    size_t nchars;
    char16_t* chars;
    size_t nbytes = *lengthp;

    nchars = nbytes;
    chars = cx->pod_malloc<char16_t>(nchars + 1);
    if (!chars)
        goto bad;
    for (size_t i = 0; i < nchars; i++)
        chars[i] = (unsigned char) bytes[i];
    *lengthp = nchars;
    chars[nchars] = 0;
    return chars;

  bad:
    // For compatibility with callers of JS_DecodeBytes we must zero lengthp
    // on errors.
    *lengthp = 0;
    return nullptr;
}

template <typename CharT>
bool
js::DeflateStringToBuffer(JSContext* maybecx, const CharT* src, size_t srclen,
                          char* dst, size_t* dstlenp)
{
    size_t dstlen = *dstlenp;
    if (srclen > dstlen) {
        for (size_t i = 0; i < dstlen; i++)
            dst[i] = char(src[i]);
        if (maybecx) {
            AutoSuppressGC suppress(maybecx);
            JS_ReportErrorNumber(maybecx, GetErrorMessage, nullptr,
                                 JSMSG_BUFFER_TOO_SMALL);
        }
        return false;
    }
    for (size_t i = 0; i < srclen; i++)
        dst[i] = char(src[i]);
    *dstlenp = srclen;
    return true;
}

template bool
js::DeflateStringToBuffer(JSContext* maybecx, const Latin1Char* src, size_t srclen,
                          char* dst, size_t* dstlenp);

template bool
js::DeflateStringToBuffer(JSContext* maybecx, const char16_t* src, size_t srclen,
                          char* dst, size_t* dstlenp);

#define ____ false

/*
 * Identifier start chars:
 * -      36:    $
 * -  65..90: A..Z
 * -      95:    _
 * - 97..122: a..z
 */
const bool js_isidstart[] = {
/*       0     1     2     3     4     5     6     7     8     9  */
/*  0 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  1 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  2 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  3 */ ____, ____, ____, ____, ____, ____, true, ____, ____, ____,
/*  4 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  5 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  6 */ ____, ____, ____, ____, ____, true, true, true, true, true,
/*  7 */ true, true, true, true, true, true, true, true, true, true,
/*  8 */ true, true, true, true, true, true, true, true, true, true,
/*  9 */ true, ____, ____, ____, ____, true, ____, true, true, true,
/* 10 */ true, true, true, true, true, true, true, true, true, true,
/* 11 */ true, true, true, true, true, true, true, true, true, true,
/* 12 */ true, true, true, ____, ____, ____, ____, ____
};

/*
 * Identifier chars:
 * -      36:    $
 * -  48..57: 0..9
 * -  65..90: A..Z
 * -      95:    _
 * - 97..122: a..z
 */
const bool js_isident[] = {
/*       0     1     2     3     4     5     6     7     8     9  */
/*  0 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  1 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  2 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  3 */ ____, ____, ____, ____, ____, ____, true, ____, ____, ____,
/*  4 */ ____, ____, ____, ____, ____, ____, ____, ____, true, true,
/*  5 */ true, true, true, true, true, true, true, true, ____, ____,
/*  6 */ ____, ____, ____, ____, ____, true, true, true, true, true,
/*  7 */ true, true, true, true, true, true, true, true, true, true,
/*  8 */ true, true, true, true, true, true, true, true, true, true,
/*  9 */ true, ____, ____, ____, ____, true, ____, true, true, true,
/* 10 */ true, true, true, true, true, true, true, true, true, true,
/* 11 */ true, true, true, true, true, true, true, true, true, true,
/* 12 */ true, true, true, ____, ____, ____, ____, ____
};

/* Whitespace chars: '\t', '\n', '\v', '\f', '\r', ' '. */
const bool js_isspace[] = {
/*       0     1     2     3     4     5     6     7     8     9  */
/*  0 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, true,
/*  1 */ true, true, true, true, ____, ____, ____, ____, ____, ____,
/*  2 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  3 */ ____, ____, true, ____, ____, ____, ____, ____, ____, ____,
/*  4 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  5 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  6 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  7 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  8 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  9 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/* 10 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/* 11 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/* 12 */ ____, ____, ____, ____, ____, ____, ____, ____
};

/*
 * Uri reserved chars + #:
 * - 35: #
 * - 36: $
 * - 38: &
 * - 43: +
 * - 44: ,
 * - 47: /
 * - 58: :
 * - 59: ;
 * - 61: =
 * - 63: ?
 * - 64: @
 */
static const bool js_isUriReservedPlusPound[] = {
/*       0     1     2     3     4     5     6     7     8     9  */
/*  0 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  1 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  2 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  3 */ ____, ____, ____, ____, ____, true, true, ____, true, ____,
/*  4 */ ____, ____, ____, true, true, ____, ____, true, ____, ____,
/*  5 */ ____, ____, ____, ____, ____, ____, ____, ____, true, true,
/*  6 */ ____, true, ____, true, true, ____, ____, ____, ____, ____,
/*  7 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  8 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  9 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/* 10 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/* 11 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/* 12 */ ____, ____, ____, ____, ____, ____, ____, ____
};

/*
 * Uri unescaped chars:
 * -      33: !
 * -      39: '
 * -      40: (
 * -      41: )
 * -      42: *
 * -      45: -
 * -      46: .
 * -  48..57: 0-9
 * -  65..90: A-Z
 * -      95: _
 * - 97..122: a-z
 * -     126: ~
 */
static const bool js_isUriUnescaped[] = {
/*       0     1     2     3     4     5     6     7     8     9  */
/*  0 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  1 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  2 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  3 */ ____, ____, ____, true, ____, ____, ____, ____, ____, true,
/*  4 */ true, true, true, ____, ____, true, true, ____, true, true,
/*  5 */ true, true, true, true, true, true, true, true, ____, ____,
/*  6 */ ____, ____, ____, ____, ____, true, true, true, true, true,
/*  7 */ true, true, true, true, true, true, true, true, true, true,
/*  8 */ true, true, true, true, true, true, true, true, true, true,
/*  9 */ true, ____, ____, ____, ____, true, ____, true, true, true,
/* 10 */ true, true, true, true, true, true, true, true, true, true,
/* 11 */ true, true, true, true, true, true, true, true, true, true,
/* 12 */ true, true, true, ____, ____, ____, true, ____
};

#undef ____

#define URI_CHUNK 64U

static inline bool
TransferBufferToString(StringBuffer& sb, MutableHandleValue rval)
{
    JSString* str = sb.finishString();
    if (!str)
        return false;
    rval.setString(str);
    return true;
}

/*
 * ECMA 3, 15.1.3 URI Handling Function Properties
 *
 * The following are implementations of the algorithms
 * given in the ECMA specification for the hidden functions
 * 'Encode' and 'Decode'.
 */
enum EncodeResult { Encode_Failure, Encode_BadUri, Encode_Success };

template <typename CharT>
static EncodeResult
Encode(StringBuffer& sb, const CharT* chars, size_t length,
       const bool* unescapedSet, const bool* unescapedSet2)
{
    static const char HexDigits[] = "0123456789ABCDEF"; /* NB: uppercase */

    char16_t hexBuf[4];
    hexBuf[0] = '%';
    hexBuf[3] = 0;

    for (size_t k = 0; k < length; k++) {
        char16_t c = chars[k];
        if (c < 128 && (unescapedSet[c] || (unescapedSet2 && unescapedSet2[c]))) {
            if (!sb.append(c))
                return Encode_Failure;
        } else {
            if (c >= 0xDC00 && c <= 0xDFFF)
                return Encode_BadUri;

            uint32_t v;
            if (c < 0xD800 || c > 0xDBFF) {
                v = c;
            } else {
                k++;
                if (k == length)
                    return Encode_BadUri;

                char16_t c2 = chars[k];
                if (c2 < 0xDC00 || c2 > 0xDFFF)
                    return Encode_BadUri;

                v = ((c - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
            }
            uint8_t utf8buf[4];
            size_t L = OneUcs4ToUtf8Char(utf8buf, v);
            for (size_t j = 0; j < L; j++) {
                hexBuf[1] = HexDigits[utf8buf[j] >> 4];
                hexBuf[2] = HexDigits[utf8buf[j] & 0xf];
                if (!sb.append(hexBuf, 3))
                    return Encode_Failure;
            }
        }
    }

    return Encode_Success;
}

static bool
Encode(JSContext* cx, HandleLinearString str, const bool* unescapedSet,
       const bool* unescapedSet2, MutableHandleValue rval)
{
    size_t length = str->length();
    if (length == 0) {
        rval.setString(cx->runtime()->emptyString);
        return true;
    }

    StringBuffer sb(cx);
    if (!sb.reserve(length))
        return false;

    EncodeResult res;
    if (str->hasLatin1Chars()) {
        AutoCheckCannotGC nogc;
        res = Encode(sb, str->latin1Chars(nogc), str->length(), unescapedSet, unescapedSet2);
    } else {
        AutoCheckCannotGC nogc;
        res = Encode(sb, str->twoByteChars(nogc), str->length(), unescapedSet, unescapedSet2);
    }

    if (res == Encode_Failure)
        return false;

    if (res == Encode_BadUri) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_URI, nullptr);
        return false;
    }

    MOZ_ASSERT(res == Encode_Success);
    return TransferBufferToString(sb, rval);
}

enum DecodeResult { Decode_Failure, Decode_BadUri, Decode_Success };

template <typename CharT>
static DecodeResult
Decode(StringBuffer& sb, const CharT* chars, size_t length, const bool* reservedSet)
{
    for (size_t k = 0; k < length; k++) {
        char16_t c = chars[k];
        if (c == '%') {
            size_t start = k;
            if ((k + 2) >= length)
                return Decode_BadUri;

            if (!JS7_ISHEX(chars[k+1]) || !JS7_ISHEX(chars[k+2]))
                return Decode_BadUri;

            uint32_t B = JS7_UNHEX(chars[k+1]) * 16 + JS7_UNHEX(chars[k+2]);
            k += 2;
            if (!(B & 0x80)) {
                c = char16_t(B);
            } else {
                int n = 1;
                while (B & (0x80 >> n))
                    n++;

                if (n == 1 || n > 4)
                    return Decode_BadUri;

                uint8_t octets[4];
                octets[0] = (uint8_t)B;
                if (k + 3 * (n - 1) >= length)
                    return Decode_BadUri;

                for (int j = 1; j < n; j++) {
                    k++;
                    if (chars[k] != '%')
                        return Decode_BadUri;

                    if (!JS7_ISHEX(chars[k+1]) || !JS7_ISHEX(chars[k+2]))
                        return Decode_BadUri;

                    B = JS7_UNHEX(chars[k+1]) * 16 + JS7_UNHEX(chars[k+2]);
                    if ((B & 0xC0) != 0x80)
                        return Decode_BadUri;

                    k += 2;
                    octets[j] = char(B);
                }
                uint32_t v = JS::Utf8ToOneUcs4Char(octets, n);
                if (v >= 0x10000) {
                    v -= 0x10000;
                    if (v > 0xFFFFF)
                        return Decode_BadUri;

                    c = char16_t((v & 0x3FF) + 0xDC00);
                    char16_t H = char16_t((v >> 10) + 0xD800);
                    if (!sb.append(H))
                        return Decode_Failure;
                } else {
                    c = char16_t(v);
                }
            }
            if (c < 128 && reservedSet && reservedSet[c]) {
                if (!sb.append(chars + start, k - start + 1))
                    return Decode_Failure;
            } else {
                if (!sb.append(c))
                    return Decode_Failure;
            }
        } else {
            if (!sb.append(c))
                return Decode_Failure;
        }
    }

    return Decode_Success;
}

static bool
Decode(JSContext* cx, HandleLinearString str, const bool* reservedSet, MutableHandleValue rval)
{
    size_t length = str->length();
    if (length == 0) {
        rval.setString(cx->runtime()->emptyString);
        return true;
    }

    StringBuffer sb(cx);

    DecodeResult res;
    if (str->hasLatin1Chars()) {
        AutoCheckCannotGC nogc;
        res = Decode(sb, str->latin1Chars(nogc), str->length(), reservedSet);
    } else {
        AutoCheckCannotGC nogc;
        res = Decode(sb, str->twoByteChars(nogc), str->length(), reservedSet);
    }

    if (res == Decode_Failure)
        return false;

    if (res == Decode_BadUri) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_URI);
        return false;
    }

    MOZ_ASSERT(res == Decode_Success);
    return TransferBufferToString(sb, rval);
}

static bool
str_decodeURI(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedLinearString str(cx, ArgToRootedString(cx, args, 0));
    if (!str)
        return false;

    return Decode(cx, str, js_isUriReservedPlusPound, args.rval());
}

static bool
str_decodeURI_Component(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedLinearString str(cx, ArgToRootedString(cx, args, 0));
    if (!str)
        return false;

    return Decode(cx, str, nullptr, args.rval());
}

static bool
str_encodeURI(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedLinearString str(cx, ArgToRootedString(cx, args, 0));
    if (!str)
        return false;

    return Encode(cx, str, js_isUriUnescaped, js_isUriReservedPlusPound, args.rval());
}

static bool
str_encodeURI_Component(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedLinearString str(cx, ArgToRootedString(cx, args, 0));
    if (!str)
        return false;

    return Encode(cx, str, js_isUriUnescaped, nullptr, args.rval());
}

/*
 * Convert one UCS-4 char and write it into a UTF-8 buffer, which must be at
 * least 4 bytes long.  Return the number of UTF-8 bytes of data written.
 */
uint32_t
js::OneUcs4ToUtf8Char(uint8_t* utf8Buffer, uint32_t ucs4Char)
{
    MOZ_ASSERT(ucs4Char <= 0x10FFFF);

    if (ucs4Char < 0x80) {
        utf8Buffer[0] = uint8_t(ucs4Char);
        return 1;
    }

    uint32_t a = ucs4Char >> 11;
    uint32_t utf8Length = 2;
    while (a) {
        a >>= 5;
        utf8Length++;
    }

    MOZ_ASSERT(utf8Length <= 4);

    uint32_t i = utf8Length;
    while (--i) {
        utf8Buffer[i] = uint8_t((ucs4Char & 0x3F) | 0x80);
        ucs4Char >>= 6;
    }

    utf8Buffer[0] = uint8_t(0x100 - (1 << (8 - utf8Length)) + ucs4Char);
    return utf8Length;
}

size_t
js::PutEscapedStringImpl(char* buffer, size_t bufferSize, GenericPrinter* out, JSLinearString* str,
                         uint32_t quote)
{
    size_t len = str->length();
    AutoCheckCannotGC nogc;
    return str->hasLatin1Chars()
           ? PutEscapedStringImpl(buffer, bufferSize, out, str->latin1Chars(nogc), len, quote)
           : PutEscapedStringImpl(buffer, bufferSize, out, str->twoByteChars(nogc), len, quote);
}

template <typename CharT>
size_t
js::PutEscapedStringImpl(char* buffer, size_t bufferSize, GenericPrinter* out, const CharT* chars,
                         size_t length, uint32_t quote)
{
    enum {
        STOP, FIRST_QUOTE, LAST_QUOTE, CHARS, ESCAPE_START, ESCAPE_MORE
    } state;

    MOZ_ASSERT(quote == 0 || quote == '\'' || quote == '"');
    MOZ_ASSERT_IF(!buffer, bufferSize == 0);
    MOZ_ASSERT_IF(out, !buffer);

    if (bufferSize == 0)
        buffer = nullptr;
    else
        bufferSize--;

    const CharT* charsEnd = chars + length;
    size_t n = 0;
    state = FIRST_QUOTE;
    unsigned shift = 0;
    unsigned hex = 0;
    unsigned u = 0;
    char c = 0;  /* to quell GCC warnings */

    for (;;) {
        switch (state) {
          case STOP:
            goto stop;
          case FIRST_QUOTE:
            state = CHARS;
            goto do_quote;
          case LAST_QUOTE:
            state = STOP;
          do_quote:
            if (quote == 0)
                continue;
            c = (char)quote;
            break;
          case CHARS:
            if (chars == charsEnd) {
                state = LAST_QUOTE;
                continue;
            }
            u = *chars++;
            if (u < ' ') {
                if (u != 0) {
                    const char* escape = strchr(js_EscapeMap, (int)u);
                    if (escape) {
                        u = escape[1];
                        goto do_escape;
                    }
                }
                goto do_hex_escape;
            }
            if (u < 127) {
                if (u == quote || u == '\\')
                    goto do_escape;
                c = (char)u;
            } else if (u < 0x100) {
                goto do_hex_escape;
            } else {
                shift = 16;
                hex = u;
                u = 'u';
                goto do_escape;
            }
            break;
          do_hex_escape:
            shift = 8;
            hex = u;
            u = 'x';
          do_escape:
            c = '\\';
            state = ESCAPE_START;
            break;
          case ESCAPE_START:
            MOZ_ASSERT(' ' <= u && u < 127);
            c = (char)u;
            state = ESCAPE_MORE;
            break;
          case ESCAPE_MORE:
            if (shift == 0) {
                state = CHARS;
                continue;
            }
            shift -= 4;
            u = 0xF & (hex >> shift);
            c = (char)(u + (u < 10 ? '0' : 'A' - 10));
            break;
        }
        if (buffer) {
            MOZ_ASSERT(n <= bufferSize);
            if (n != bufferSize) {
                buffer[n] = c;
            } else {
                buffer[n] = '\0';
                buffer = nullptr;
            }
        } else if (out) {
            if (out->put(&c, 1) < 0)
                return size_t(-1);
        }
        n++;
    }
  stop:
    if (buffer)
        buffer[n] = '\0';
    return n;
}

template size_t
js::PutEscapedStringImpl(char* buffer, size_t bufferSize, GenericPrinter* out, const Latin1Char* chars,
                         size_t length, uint32_t quote);

template size_t
js::PutEscapedStringImpl(char* buffer, size_t bufferSize, GenericPrinter* out, const char* chars,
                         size_t length, uint32_t quote);

template size_t
js::PutEscapedStringImpl(char* buffer, size_t bufferSize, GenericPrinter* out, const char16_t* chars,
                         size_t length, uint32_t quote);

template size_t
js::PutEscapedString(char* buffer, size_t bufferSize, const Latin1Char* chars, size_t length,
                     uint32_t quote);

template size_t
js::PutEscapedString(char* buffer, size_t bufferSize, const char16_t* chars, size_t length,
                     uint32_t quote);
