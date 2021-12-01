/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_Text_h
#define util_Text_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

#include "jsutil.h"
#include "NamespaceImports.h"

#include "js/Utility.h"
#include "util/Unicode.h"
#include "vm/Printer.h"

class JSLinearString;

/*
 * Shorthands for ASCII (7-bit) decimal and hex conversion.
 * Manually inline isdigit and isxdigit for performance; MSVC doesn't do this for us.
 */
#define JS7_ISDEC(c)    ((((unsigned)(c)) - '0') <= 9)
#define JS7_ISA2F(c)    ((((((unsigned)(c)) - 'a') <= 5) || (((unsigned)(c)) - 'A') <= 5))
#define JS7_UNDEC(c)    ((c) - '0')
#define JS7_ISOCT(c)    ((((unsigned)(c)) - '0') <= 7)
#define JS7_UNOCT(c)    (JS7_UNDEC(c))
#define JS7_ISHEX(c)    ((c) < 128 && (JS7_ISDEC(c) || JS7_ISA2F(c)))
#define JS7_UNHEX(c)    (unsigned)(JS7_ISDEC(c) ? (c) - '0' : 10 + tolower(c) - 'a')

static MOZ_ALWAYS_INLINE size_t
js_strlen(const char16_t* s)
{
    return std::char_traits<char16_t>::length(s);
}

template <typename CharT>
extern const CharT*
js_strchr_limit(const CharT* s, char16_t c, const CharT* limit);

extern int32_t
js_fputs(const char16_t* s, FILE* f);

namespace js {

class StringBuffer;

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

extern UniqueChars
DuplicateString(JSContext* cx, const char* s);

extern UniqueTwoByteChars
DuplicateString(JSContext* cx, const char16_t* s);

/*
 * These variants do not report OOMs, you must arrange for OOMs to be reported
 * yourself.
 */
extern UniqueChars
DuplicateString(const char* s);

extern UniqueChars
DuplicateString(const char* s, size_t n);

extern UniqueTwoByteChars
DuplicateString(const char16_t* s);

extern UniqueTwoByteChars
DuplicateString(const char16_t* s, size_t n);

/*
 * Inflate bytes in ASCII encoding to char16_t code units. Return null on error,
 * otherwise return the char16_t buffer that was malloc'ed. A null char is
 * appended.
 */
extern char16_t*
InflateString(JSContext* cx, const char* bytes, size_t length);

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
EncodeURI(JSContext* cx, StringBuffer& sb, const char* chars, size_t length);

} // namespace js

#endif // util_Text_h
