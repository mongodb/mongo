/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/CharacterEncoding.h"

#include "mozilla/Range.h"
#include "mozilla/Sprintf.h"

#include <algorithm>
#include <type_traits>

#include "vm/JSContext.h"

using namespace js;

Latin1CharsZ
JS::LossyTwoByteCharsToNewLatin1CharsZ(JSContext* cx,
                                       const mozilla::Range<const char16_t> tbchars)
{
    MOZ_ASSERT(cx);
    size_t len = tbchars.length();
    unsigned char* latin1 = cx->pod_malloc<unsigned char>(len + 1);
    if (!latin1)
        return Latin1CharsZ();
    for (size_t i = 0; i < len; ++i)
        latin1[i] = static_cast<unsigned char>(tbchars[i]);
    latin1[len] = '\0';
    return Latin1CharsZ(latin1, len);
}

template <typename CharT>
static size_t
GetDeflatedUTF8StringLength(const CharT* chars, size_t nchars)
{
    size_t nbytes = nchars;
    for (const CharT* end = chars + nchars; chars < end; chars++) {
        char16_t c = *chars;
        if (c < 0x80)
            continue;
        uint32_t v;
        if (0xD800 <= c && c <= 0xDFFF) {
            /* nbytes sets 1 length since this is surrogate pair. */
            if (c >= 0xDC00 || (chars + 1) == end) {
                nbytes += 2; /* Bad Surrogate */
                continue;
            }
            char16_t c2 = chars[1];
            if (c2 < 0xDC00 || c2 > 0xDFFF) {
                nbytes += 2; /* Bad Surrogate */
                continue;
            }
            v = ((c - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
            nbytes--;
            chars++;
        } else {
            v = c;
        }
        v >>= 11;
        nbytes++;
        while (v) {
            v >>= 5;
            nbytes++;
        }
    }
    return nbytes;
}

JS_PUBLIC_API(size_t)
JS::GetDeflatedUTF8StringLength(JSFlatString* s)
{
    JS::AutoCheckCannotGC nogc;
    return s->hasLatin1Chars()
           ? ::GetDeflatedUTF8StringLength(s->latin1Chars(nogc), s->length())
           : ::GetDeflatedUTF8StringLength(s->twoByteChars(nogc), s->length());
}

static const char16_t UTF8_REPLACEMENT_CHAR = 0xFFFD;

template <typename CharT>
static void
DeflateStringToUTF8Buffer(const CharT* src, size_t srclen, mozilla::RangedPtr<char> dst,
                          size_t* dstlenp = nullptr, size_t* numcharsp = nullptr)
{
    size_t capacity = 0;
    if (dstlenp) {
        capacity = *dstlenp;
        *dstlenp = 0;
    }
    if (numcharsp)
        *numcharsp = 0;

    while (srclen) {
        uint32_t v;
        char16_t c = *src++;
        srclen--;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            v = UTF8_REPLACEMENT_CHAR;
        } else if (c < 0xD800 || c > 0xDBFF) {
            v = c;
        } else {
            if (srclen < 1) {
                v = UTF8_REPLACEMENT_CHAR;
            } else {
                char16_t c2 = *src;
                if (c2 < 0xDC00 || c2 > 0xDFFF) {
                    v = UTF8_REPLACEMENT_CHAR;
                } else {
                    src++;
                    srclen--;
                    v = ((c - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
                }
            }
        }

        size_t utf8Len;
        if (v < 0x0080) {
            /* no encoding necessary - performance hack */
            if (dstlenp && *dstlenp + 1 > capacity)
                return;
            *dst++ = char(v);
            utf8Len = 1;
        } else {
            uint8_t utf8buf[4];
            utf8Len = OneUcs4ToUtf8Char(utf8buf, v);
            if (dstlenp && *dstlenp + utf8Len > capacity)
                return;
            for (size_t i = 0; i < utf8Len; i++)
                *dst++ = char(utf8buf[i]);
        }

        if (dstlenp)
            *dstlenp += utf8Len;
        if (numcharsp)
            (*numcharsp)++;
    }
}

JS_PUBLIC_API(void)
JS::DeflateStringToUTF8Buffer(JSFlatString* src, mozilla::RangedPtr<char> dst,
                              size_t* dstlenp, size_t* numcharsp)
{
    JS::AutoCheckCannotGC nogc;
    return src->hasLatin1Chars()
           ? ::DeflateStringToUTF8Buffer(src->latin1Chars(nogc), src->length(), dst,
                                         dstlenp, numcharsp)
           : ::DeflateStringToUTF8Buffer(src->twoByteChars(nogc), src->length(), dst,
                                         dstlenp, numcharsp);
}

template <typename CharT>
UTF8CharsZ
JS::CharsToNewUTF8CharsZ(JSContext* maybeCx, const mozilla::Range<CharT> chars)
{
    /* Get required buffer size. */
    const CharT* str = chars.begin().get();
    size_t len = ::GetDeflatedUTF8StringLength(str, chars.length());

    /* Allocate buffer. */
    char* utf8;
    if (maybeCx)
        utf8 = maybeCx->pod_malloc<char>(len + 1);
    else
        utf8 = js_pod_malloc<char>(len + 1);
    if (!utf8)
        return UTF8CharsZ();

    /* Encode to UTF8. */
    ::DeflateStringToUTF8Buffer(str, chars.length(), mozilla::RangedPtr<char>(utf8, len));
    utf8[len] = '\0';

    return UTF8CharsZ(utf8, len);
}

template UTF8CharsZ
JS::CharsToNewUTF8CharsZ(JSContext* maybeCx,
                         const mozilla::Range<Latin1Char> chars);

template UTF8CharsZ
JS::CharsToNewUTF8CharsZ(JSContext* maybeCx,
                         const mozilla::Range<char16_t> chars);

template UTF8CharsZ
JS::CharsToNewUTF8CharsZ(JSContext* maybeCx,
                         const mozilla::Range<const Latin1Char> chars);

template UTF8CharsZ
JS::CharsToNewUTF8CharsZ(JSContext* maybeCx,
                         const mozilla::Range<const char16_t> chars);

static const uint32_t INVALID_UTF8 = UINT32_MAX;

/*
 * Convert a utf8 character sequence into a UCS-4 character and return that
 * character.  It is assumed that the caller already checked that the sequence
 * is valid.
 */
uint32_t
JS::Utf8ToOneUcs4Char(const uint8_t* utf8Buffer, int utf8Length)
{
    MOZ_ASSERT(1 <= utf8Length && utf8Length <= 4);

    if (utf8Length == 1) {
        MOZ_ASSERT(!(*utf8Buffer & 0x80));
        return *utf8Buffer;
    }

    /* from Unicode 3.1, non-shortest form is illegal */
    static const uint32_t minucs4Table[] = { 0x80, 0x800, 0x10000 };

    MOZ_ASSERT((*utf8Buffer & (0x100 - (1 << (7 - utf8Length)))) ==
               (0x100 - (1 << (8 - utf8Length))));
    uint32_t ucs4Char = *utf8Buffer++ & ((1 << (7 - utf8Length)) - 1);
    uint32_t minucs4Char = minucs4Table[utf8Length - 2];
    while (--utf8Length) {
        MOZ_ASSERT((*utf8Buffer & 0xC0) == 0x80);
        ucs4Char = (ucs4Char << 6) | (*utf8Buffer++ & 0x3F);
    }

    if (MOZ_UNLIKELY(ucs4Char < minucs4Char || (ucs4Char >= 0xD800 && ucs4Char <= 0xDFFF)))
        return INVALID_UTF8;

    return ucs4Char;
}

static void
ReportInvalidCharacter(JSContext* cx, uint32_t offset)
{
    char buffer[10];
    SprintfLiteral(buffer, "%u", offset);
    JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
                                      JSMSG_MALFORMED_UTF8_CHAR, buffer);
}

static void
ReportBufferTooSmall(JSContext* cx, uint32_t dummy)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BUFFER_TOO_SMALL);
}

static void
ReportTooBigCharacter(JSContext* cx, uint32_t v)
{
    char buffer[10];
    SprintfLiteral(buffer, "0x%x", v + 0x10000);
    JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
                                      JSMSG_UTF8_CHAR_TOO_LARGE, buffer);
}

enum InflateUTF8Action {
    CountAndReportInvalids,
    CountAndIgnoreInvalids,
    AssertNoInvalids,
    Copy,
    FindEncoding
};

static const char16_t REPLACE_UTF8 = 0xFFFD;
static const Latin1Char REPLACE_UTF8_LATIN1 = '?';

// If making changes to this algorithm, make sure to also update
// LossyConvertUTF8toUTF16() in dom/wifi/WifiUtils.cpp
template <InflateUTF8Action Action, typename CharT, class ContextT>
static bool
InflateUTF8StringToBuffer(ContextT* cx, const UTF8Chars src, CharT* dst, size_t* dstlenp,
                          JS::SmallestEncoding *smallestEncoding)
{
    if (Action != AssertNoInvalids)
        *smallestEncoding = JS::SmallestEncoding::ASCII;
    auto RequireLatin1 = [&smallestEncoding]{
        *smallestEncoding = std::max(JS::SmallestEncoding::Latin1, *smallestEncoding);
    };
    auto RequireUTF16 = [&smallestEncoding]{
        *smallestEncoding = JS::SmallestEncoding::UTF16;
    };

    // Count how many code units need to be in the inflated string.
    // |i| is the index into |src|, and |j| is the the index into |dst|.
    size_t srclen = src.length();
    uint32_t j = 0;
    for (uint32_t i = 0; i < srclen; i++, j++) {
        uint32_t v = uint32_t(src[i]);
        if (!(v & 0x80)) {
            // ASCII code unit.  Simple copy.
            if (Action == Copy)
                dst[j] = CharT(v);

        } else {
            // Non-ASCII code unit.  Determine its length in bytes (n).
            uint32_t n = 1;
            while (v & (0x80 >> n))
                n++;

        #define INVALID(report, arg, n2)                                \
            do {                                                        \
                if (Action == CountAndReportInvalids) {                 \
                    report(cx, arg);                                    \
                    return false;                                       \
                } else if (Action == AssertNoInvalids) {                \
                    MOZ_CRASH("invalid UTF-8 string: " # report);       \
                } else {                                                \
                    if (Action == Copy) {                               \
                        if (std::is_same<decltype(dst[0]), Latin1Char>::value) \
                            dst[j] = CharT(REPLACE_UTF8_LATIN1);        \
                        else                                            \
                            dst[j] = CharT(REPLACE_UTF8);               \
                    } else {                                            \
                        MOZ_ASSERT(Action == CountAndIgnoreInvalids ||  \
                                   Action == FindEncoding);             \
                    }                                                   \
                    n = n2;                                             \
                    goto invalidMultiByteCodeUnit;                      \
                }                                                       \
            } while (0)

            // Check the leading byte.
            if (n < 2 || n > 4)
                INVALID(ReportInvalidCharacter, i, 1);

            // Check that |src| is large enough to hold an n-byte code unit.
            if (i + n > srclen)
                INVALID(ReportBufferTooSmall, /* dummy = */ 0, 1);

            // Check the second byte.  From Unicode Standard v6.2, Table 3-7
            // Well-Formed UTF-8 Byte Sequences.
            if ((v == 0xE0 && ((uint8_t)src[i + 1] & 0xE0) != 0xA0) ||  // E0 A0~BF
                (v == 0xED && ((uint8_t)src[i + 1] & 0xE0) != 0x80) ||  // ED 80~9F
                (v == 0xF0 && ((uint8_t)src[i + 1] & 0xF0) == 0x80) ||  // F0 90~BF
                (v == 0xF4 && ((uint8_t)src[i + 1] & 0xF0) != 0x80))    // F4 80~8F
            {
                INVALID(ReportInvalidCharacter, i, 1);
            }

            // Check the continuation bytes.
            for (uint32_t m = 1; m < n; m++) {
                if ((src[i + m] & 0xC0) != 0x80)
                    INVALID(ReportInvalidCharacter, i, m);
            }

            // Determine the code unit's length in CharT and act accordingly.
            v = JS::Utf8ToOneUcs4Char((uint8_t*)&src[i], n);
            if (Action != AssertNoInvalids) {
                if (v > 0xff) {
                    RequireUTF16();
                    if (Action == FindEncoding) {
                        MOZ_ASSERT(dst == nullptr);
                        return true;
                    }
                } else {
                    RequireLatin1();
                }
            }
            if (v < 0x10000) {
                // The n-byte UTF8 code unit will fit in a single CharT.
                if (Action == Copy)
                    dst[j] = CharT(v);
            } else {
                v -= 0x10000;
                if (v <= 0xFFFFF) {
                    // The n-byte UTF8 code unit will fit in two CharT units.
                    if (Action == Copy)
                        dst[j] = CharT((v >> 10) + 0xD800);
                    j++;
                    if (Action == Copy)
                        dst[j] = CharT((v & 0x3FF) + 0xDC00);

                } else {
                    // The n-byte UTF8 code unit won't fit in two CharT units.
                    INVALID(ReportTooBigCharacter, v, 1);
                }
            }

          invalidMultiByteCodeUnit:
            // Move i to the last byte of the multi-byte code unit;  the loop
            // header will do the final i++ to move to the start of the next
            // code unit.
            i += n - 1;
            if (Action != AssertNoInvalids)
                RequireUTF16();
        }
    }

    if (Action != AssertNoInvalids && Action != FindEncoding)
        *dstlenp = j;

    return true;
}

template <InflateUTF8Action Action, typename CharsT, class ContextT>
static CharsT
InflateUTF8StringHelper(ContextT* cx, const UTF8Chars src, size_t* outlen)
{
    using CharT = typename CharsT::CharT;
    *outlen = 0;

    JS::SmallestEncoding encoding;
    if (!InflateUTF8StringToBuffer<Action, CharT>(cx, src, /* dst = */ nullptr, outlen, &encoding))
        return CharsT();

    CharT* dst = cx->template pod_malloc<CharT>(*outlen + 1);  // +1 for NUL
    if (!dst) {
        ReportOutOfMemory(cx);
        return CharsT();
    }

    if (encoding == JS::SmallestEncoding::ASCII) {
        size_t srclen = src.length();
        MOZ_ASSERT(*outlen == srclen);
        for (uint32_t i = 0; i < srclen; i++)
            dst[i] = CharT(src[i]);
    } else {
        MOZ_ALWAYS_TRUE((InflateUTF8StringToBuffer<Copy, CharT>(cx, src, dst, outlen, &encoding)));
    }

    dst[*outlen] = 0;    // NUL char

    return CharsT(dst, *outlen);
}

TwoByteCharsZ
JS::UTF8CharsToNewTwoByteCharsZ(JSContext* cx, const UTF8Chars utf8, size_t* outlen)
{
    return InflateUTF8StringHelper<CountAndReportInvalids, TwoByteCharsZ>(cx, utf8, outlen);
}

TwoByteCharsZ
JS::UTF8CharsToNewTwoByteCharsZ(JSContext* cx, const ConstUTF8CharsZ& utf8, size_t* outlen)
{
    UTF8Chars chars(utf8.c_str(), strlen(utf8.c_str()));
    return InflateUTF8StringHelper<CountAndReportInvalids, TwoByteCharsZ>(cx, chars, outlen);
}

TwoByteCharsZ
JS::LossyUTF8CharsToNewTwoByteCharsZ(JSContext* cx, const JS::UTF8Chars utf8, size_t* outlen)
{
    return InflateUTF8StringHelper<CountAndIgnoreInvalids, TwoByteCharsZ>(cx, utf8, outlen);
}

TwoByteCharsZ
JS::LossyUTF8CharsToNewTwoByteCharsZ(JSContext* cx, const JS::ConstUTF8CharsZ& utf8, size_t* outlen)
{
    UTF8Chars chars(utf8.c_str(), strlen(utf8.c_str()));
    return InflateUTF8StringHelper<CountAndIgnoreInvalids, TwoByteCharsZ>(cx, chars, outlen);
}

JS::SmallestEncoding
JS::FindSmallestEncoding(UTF8Chars utf8)
{
    JS::SmallestEncoding encoding;
    MOZ_ALWAYS_TRUE((InflateUTF8StringToBuffer<FindEncoding, char16_t, JSContext>(
                         /* cx = */ nullptr,
                         utf8,
                         /* dst = */ nullptr,
                         /* dstlen = */ nullptr,
                         &encoding)));
    return encoding;
}

Latin1CharsZ
JS::UTF8CharsToNewLatin1CharsZ(JSContext* cx, const UTF8Chars utf8, size_t* outlen)
{
    return InflateUTF8StringHelper<CountAndReportInvalids, Latin1CharsZ>(cx, utf8, outlen);
}

Latin1CharsZ
JS::LossyUTF8CharsToNewLatin1CharsZ(JSContext* cx, const UTF8Chars utf8, size_t* outlen)
{
    return InflateUTF8StringHelper<CountAndIgnoreInvalids, Latin1CharsZ>(cx, utf8, outlen);
}

#ifdef DEBUG
void
JS::ConstUTF8CharsZ::validate(size_t aLength)
{
    MOZ_ASSERT(data_);
    UTF8Chars chars(data_, aLength);
    InflateUTF8StringToBuffer<AssertNoInvalids, char16_t, JSContext>(
        /* cx = */ nullptr,
        chars,
        /* dst = */ nullptr,
        /* dstlen = */ nullptr,
        /* smallestEncoding = */ nullptr);
}
#endif

bool
JS::StringIsASCII(const char* s)
{
    while (*s) {
        if (*s & 0x80)
            return false;
        s++;
    }
    return true;
}

bool
JS::StringIsUTF8(const uint8_t* s, uint32_t length)
{
    const uint8_t* limit = s + length;
    while (s < limit) {
        uint32_t len;
        uint32_t min;
        uint32_t n = *s;
        if ((n & 0x80) == 0) {
            len = 1;
            min = 0;
        } else if ((n & 0xE0) == 0xC0) {
            len = 2;
            min = 0x80;
            n &= 0x1F;
        } else if ((n & 0xF0) == 0xE0) {
            len = 3;
            min = 0x800;
            n &= 0x0F;
        } else if ((n & 0xF8) == 0xF0) {
            len = 4;
            min = 0x10000;
            n &= 0x07;
        } else {
            return false;
        }
        if (s + len > limit)
            return false;
        for (uint32_t i = 1; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80)
                return false;
            n = (n << 6) | (s[i] & 0x3F);
        }
        if (n < min || (0xD800 <= n && n < 0xE000) || n >= 0x110000)
            return false;
        s += len;
    }
    return true;
}
