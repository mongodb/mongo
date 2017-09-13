/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsstr_h
#define jsstr_h

#include "mozilla/HashFunctions.h"
#include "mozilla/PodOperations.h"
#include "mozilla/UniquePtr.h"

#include "jsutil.h"
#include "NamespaceImports.h"

#include "gc/Rooting.h"
#include "js/RootingAPI.h"
#include "vm/Printer.h"
#include "vm/Unicode.h"

class JSAutoByteString;
class JSLinearString;

namespace js {

class StringBuffer;

template <AllowGC allowGC>
extern JSString*
ConcatStrings(ExclusiveContext* cx,
              typename MaybeRooted<JSString*, allowGC>::HandleType left,
              typename MaybeRooted<JSString*, allowGC>::HandleType right);

// Return s advanced past any Unicode white space characters.
template <typename CharT>
static inline const CharT*
SkipSpace(const CharT* s, const CharT* end)
{
    MOZ_ASSERT(s <= end);

    while (s < end && unicode::IsSpace(*s))
        s++;

    return s;
}

// Return less than, equal to, or greater than zero depending on whether
// s1 is less than, equal to, or greater than s2.
template <typename Char1, typename Char2>
inline int32_t
CompareChars(const Char1* s1, size_t len1, const Char2* s2, size_t len2)
{
    size_t n = Min(len1, len2);
    for (size_t i = 0; i < n; i++) {
        if (int32_t cmp = s1[i] - s2[i])
            return cmp;
    }

    return int32_t(len1 - len2);
}

extern int32_t
CompareChars(const char16_t* s1, size_t len1, JSLinearString* s2);

}  /* namespace js */

struct JSSubString {
    JSLinearString* base;
    size_t          offset;
    size_t          length;

    JSSubString() { mozilla::PodZero(this); }

    void initEmpty(JSLinearString* base) {
        this->base = base;
        offset = length = 0;
    }
    void init(JSLinearString* base, size_t offset, size_t length) {
        this->base = base;
        this->offset = offset;
        this->length = length;
    }
};

/*
 * Shorthands for ASCII (7-bit) decimal and hex conversion.
 * Manually inline isdigit for performance; MSVC doesn't do this for us.
 */
#define JS7_ISDEC(c)    ((((unsigned)(c)) - '0') <= 9)
#define JS7_UNDEC(c)    ((c) - '0')
#define JS7_ISOCT(c)    ((((unsigned)(c)) - '0') <= 7)
#define JS7_UNOCT(c)    (JS7_UNDEC(c))
#define JS7_ISHEX(c)    ((c) < 128 && isxdigit(c))
#define JS7_UNHEX(c)    (unsigned)(JS7_ISDEC(c) ? (c) - '0' : 10 + tolower(c) - 'a')
#define JS7_ISLET(c)    ((c) < 128 && isalpha(c))

extern size_t
js_strlen(const char16_t* s);

extern int32_t
js_strcmp(const char16_t* lhs, const char16_t* rhs);

template <typename CharT>
extern const CharT*
js_strchr_limit(const CharT* s, char16_t c, const CharT* limit);

static MOZ_ALWAYS_INLINE void
js_strncpy(char16_t* dst, const char16_t* src, size_t nelem)
{
    return mozilla::PodCopy(dst, src, nelem);
}

namespace js {

/* Initialize the String class, returning its prototype object. */
extern JSObject*
InitStringClass(JSContext* cx, HandleObject obj);

/*
 * Convert a value to a printable C string.
 */
extern const char*
ValueToPrintable(JSContext* cx, const Value&, JSAutoByteString* bytes, bool asSource = false);

extern mozilla::UniquePtr<char[], JS::FreePolicy>
DuplicateString(ExclusiveContext* cx, const char* s);

extern mozilla::UniquePtr<char16_t[], JS::FreePolicy>
DuplicateString(ExclusiveContext* cx, const char16_t* s);

// This variant does not report OOMs, you must arrange for OOMs to be reported
// yourself.
extern mozilla::UniquePtr<char16_t[], JS::FreePolicy>
DuplicateString(const char16_t* s);

/*
 * Convert a non-string value to a string, returning null after reporting an
 * error, otherwise returning a new string reference.
 */
template <AllowGC allowGC>
extern JSString*
ToStringSlow(ExclusiveContext* cx, typename MaybeRooted<Value, allowGC>::HandleType arg);

/*
 * Convert the given value to a string.  This method includes an inline
 * fast-path for the case where the value is already a string; if the value is
 * known not to be a string, use ToStringSlow instead.
 */
template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE JSString*
ToString(JSContext* cx, JS::HandleValue v)
{
    if (v.isString())
        return v.toString();
    return ToStringSlow<allowGC>(cx, v);
}

/*
 * This function implements E-262-3 section 9.8, toString. Convert the given
 * value to a string of characters appended to the given buffer. On error, the
 * passed buffer may have partial results appended.
 */
inline bool
ValueToStringBuffer(JSContext* cx, const Value& v, StringBuffer& sb);

/*
 * Convert a value to its source expression, returning null after reporting
 * an error, otherwise returning a new string reference.
 */
extern JSString*
ValueToSource(JSContext* cx, HandleValue v);

/*
 * Convert a JSString to its source expression; returns null after reporting an
 * error, otherwise returns a new string reference. No Handle needed since the
 * input is dead after the GC.
 */
extern JSString*
StringToSource(JSContext* cx, JSString* str);

/*
 * Test if strings are equal. The caller can call the function even if str1
 * or str2 are not GC-allocated things.
 */
extern bool
EqualStrings(JSContext* cx, JSString* str1, JSString* str2, bool* result);

/* Use the infallible method instead! */
extern bool
EqualStrings(JSContext* cx, JSLinearString* str1, JSLinearString* str2, bool* result) = delete;

/* EqualStrings is infallible on linear strings. */
extern bool
EqualStrings(JSLinearString* str1, JSLinearString* str2);

extern bool
EqualChars(JSLinearString* str1, JSLinearString* str2);

/*
 * Return less than, equal to, or greater than zero depending on whether
 * str1 is less than, equal to, or greater than str2.
 */
extern bool
CompareStrings(JSContext* cx, JSString* str1, JSString* str2, int32_t* result);

/*
 * Same as CompareStrings but for atoms.  Don't use this to just test
 * for equality; use this when you need an ordering on atoms.
 */
extern int32_t
CompareAtoms(JSAtom* atom1, JSAtom* atom2);

/*
 * Return true if the string matches the given sequence of ASCII bytes.
 */
extern bool
StringEqualsAscii(JSLinearString* str, const char* asciiBytes);

/* Return true if the string contains a pattern anywhere inside it. */
extern bool
StringHasPattern(JSLinearString* text, const char16_t* pat, uint32_t patlen);

extern int
StringFindPattern(JSLinearString* text, JSLinearString* pat, size_t start);

/* Return true if the string contains a pattern at |start|. */
extern bool
HasSubstringAt(JSLinearString* text, JSLinearString* pat, size_t start);

template <typename CharT>
extern bool
HasRegExpMetaChars(const CharT* chars, size_t length);

extern bool
StringHasRegExpMetaChars(JSLinearString* str);

template <typename Char1, typename Char2>
inline bool
EqualChars(const Char1* s1, const Char2* s2, size_t len);

template <typename Char1>
inline bool
EqualChars(const Char1* s1, const Char1* s2, size_t len)
{
    return mozilla::PodEqual(s1, s2, len);
}

template <typename Char1, typename Char2>
inline bool
EqualChars(const Char1* s1, const Char2* s2, size_t len)
{
    for (const Char1* s1end = s1 + len; s1 < s1end; s1++, s2++) {
        if (*s1 != *s2)
            return false;
    }
    return true;
}

/*
 * Computes |str|'s substring for the range [beginInt, beginInt + lengthInt).
 * Negative, overlarge, swapped, etc. |beginInt| and |lengthInt| are forbidden
 * and constitute API misuse.
 */
JSString*
SubstringKernel(JSContext* cx, HandleString str, int32_t beginInt, int32_t lengthInt);

/*
 * Inflate bytes in ASCII encoding to char16_t code units. Return null on error,
 * otherwise return the char16_t buffer that was malloc'ed. length is updated to
 * the length of the new string (in char16_t code units). A null char is
 * appended, but it is not included in the length.
 */
extern char16_t*
InflateString(ExclusiveContext* cx, const char* bytes, size_t* length);

/*
 * Inflate bytes to JS chars in an existing buffer. 'dst' must be large
 * enough for 'srclen' char16_t code units. The buffer is NOT null-terminated.
 */
inline void
CopyAndInflateChars(char16_t* dst, const char* src, size_t srclen)
{
    for (size_t i = 0; i < srclen; i++)
        dst[i] = (unsigned char) src[i];
}

inline void
CopyAndInflateChars(char16_t* dst, const JS::Latin1Char* src, size_t srclen)
{
    for (size_t i = 0; i < srclen; i++)
        dst[i] = src[i];
}

/*
 * Deflate JS chars to bytes into a buffer. 'bytes' must be large enough for
 * 'length chars. The buffer is NOT null-terminated. The destination length
 * must to be initialized with the buffer size and will contain on return the
 * number of copied bytes.
 */
template <typename CharT>
extern bool
DeflateStringToBuffer(JSContext* maybecx, const CharT* chars,
                      size_t charsLength, char* bytes, size_t* length);

/*
 * The String.prototype.replace fast-native entry point is exported for joined
 * function optimization in js{interp,tracer}.cpp.
 */
extern bool
str_replace(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
str_fromCharCode(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_fromCharCode_one_arg(JSContext* cx, HandleValue code, MutableHandleValue rval);

/* String methods exposed so they can be installed in the self-hosting global. */

extern bool
str_indexOf(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_lastIndexOf(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_startsWith(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_toLowerCase(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_toUpperCase(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_toString(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_charAt(JSContext* cx, unsigned argc, Value* vp);

extern bool
str_charCodeAt_impl(JSContext* cx, HandleString string, HandleValue index, MutableHandleValue res);

extern bool
str_charCodeAt(JSContext* cx, unsigned argc, Value* vp);
/*
 * Convert one UCS-4 char and write it into a UTF-8 buffer, which must be at
 * least 4 bytes long.  Return the number of UTF-8 bytes of data written.
 */
extern uint32_t
OneUcs4ToUtf8Char(uint8_t* utf8Buffer, uint32_t ucs4Char);

extern size_t
PutEscapedStringImpl(char* buffer, size_t size, GenericPrinter* out, JSLinearString* str,
                     uint32_t quote);

template <typename CharT>
extern size_t
PutEscapedStringImpl(char* buffer, size_t bufferSize, GenericPrinter* out, const CharT* chars,
                     size_t length, uint32_t quote);

/*
 * Write str into buffer escaping any non-printable or non-ASCII character
 * using \escapes for JS string literals.
 * Guarantees that a NUL is at the end of the buffer unless size is 0. Returns
 * the length of the written output, NOT including the NUL. Thus, a return
 * value of size or more means that the output was truncated. If buffer
 * is null, just returns the length of the output. If quote is not 0, it must
 * be a single or double quote character that will quote the output.
*/
inline size_t
PutEscapedString(char* buffer, size_t size, JSLinearString* str, uint32_t quote)
{
    size_t n = PutEscapedStringImpl(buffer, size, nullptr, str, quote);

    /* PutEscapedStringImpl can only fail with a file. */
    MOZ_ASSERT(n != size_t(-1));
    return n;
}

template <typename CharT>
inline size_t
PutEscapedString(char* buffer, size_t bufferSize, const CharT* chars, size_t length, uint32_t quote)
{
    size_t n = PutEscapedStringImpl(buffer, bufferSize, nullptr, chars, length, quote);

    /* PutEscapedStringImpl can only fail with a file. */
    MOZ_ASSERT(n != size_t(-1));
    return n;
}

inline bool
EscapedStringPrinter(GenericPrinter& out, JSLinearString* str, uint32_t quote)
{
    return PutEscapedStringImpl(nullptr, 0, &out, str, quote) != size_t(-1);
}

inline bool
EscapedStringPrinter(GenericPrinter& out, const char* chars, size_t length, uint32_t quote)
{
    return PutEscapedStringImpl(nullptr, 0, &out, chars, length, quote) != size_t(-1);
}

/*
 * Write str into file escaping any non-printable or non-ASCII character.
 * If quote is not 0, it must be a single or double quote character that
 * will quote the output.
*/
inline bool
FileEscapedString(FILE* fp, JSLinearString* str, uint32_t quote)
{
    Fprinter out(fp);
    bool res = EscapedStringPrinter(out, str, quote);
    out.finish();
    return res;
}

inline bool
FileEscapedString(FILE* fp, const char* chars, size_t length, uint32_t quote)
{
    Fprinter out(fp);
    bool res = EscapedStringPrinter(out, chars, length, quote);
    out.finish();
    return res;
}

bool
str_match(JSContext* cx, unsigned argc, Value* vp);

bool
str_search(JSContext* cx, unsigned argc, Value* vp);

bool
str_split(JSContext* cx, unsigned argc, Value* vp);

JSObject*
str_split_string(JSContext* cx, HandleObjectGroup group, HandleString str, HandleString sep);

JSString*
str_replace_string_raw(JSContext* cx, HandleString string, HandleString pattern,
                       HandleString replacement);

extern bool
StringConstructor(JSContext* cx, unsigned argc, Value* vp);

} /* namespace js */

#endif /* jsstr_h */
