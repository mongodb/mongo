/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/CharacterEncoding.h"

#include "mozilla/Latin1.h"
#include "mozilla/Range.h"
#include "mozilla/Span.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Utf8.h"

#include <algorithm>
#include <type_traits>

#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "util/StringBuffer.h"
#include "util/Unicode.h"  // unicode::REPLACEMENT_CHARACTER
#include "vm/JSContext.h"

using mozilla::AsChars;
using mozilla::AsciiValidUpTo;
using mozilla::AsWritableChars;
using mozilla::ConvertLatin1toUtf8Partial;
using mozilla::ConvertUtf16toUtf8Partial;
using mozilla::IsAscii;
using mozilla::IsUtf8Latin1;
using mozilla::LossyConvertUtf16toLatin1;
using mozilla::Span;
using mozilla::Tie;
using mozilla::Tuple;
using mozilla::Utf8Unit;

using JS::Latin1CharsZ;
using JS::TwoByteCharsZ;
using JS::UTF8Chars;
using JS::UTF8CharsZ;

using namespace js;
using namespace js::unicode;

Latin1CharsZ JS::LossyTwoByteCharsToNewLatin1CharsZ(
    JSContext* cx, const mozilla::Range<const char16_t> tbchars) {
  MOZ_ASSERT(cx);
  size_t len = tbchars.length();
  unsigned char* latin1 = cx->pod_malloc<unsigned char>(len + 1);
  if (!latin1) {
    return Latin1CharsZ();
  }
  LossyConvertUtf16toLatin1(tbchars, AsWritableChars(Span(latin1, len)));
  latin1[len] = '\0';
  return Latin1CharsZ(latin1, len);
}

template <typename CharT>
static size_t GetDeflatedUTF8StringLength(const CharT* chars, size_t nchars) {
  size_t nbytes = nchars;
  for (const CharT* end = chars + nchars; chars < end; chars++) {
    char16_t c = *chars;
    if (c < 0x80) {
      continue;
    }
    uint32_t v;
    if (IsSurrogate(c)) {
      /* nbytes sets 1 length since this is surrogate pair. */
      if (IsTrailSurrogate(c) || (chars + 1) == end) {
        nbytes += 2; /* Bad Surrogate */
        continue;
      }
      char16_t c2 = chars[1];
      if (!IsTrailSurrogate(c2)) {
        nbytes += 2; /* Bad Surrogate */
        continue;
      }
      v = UTF16Decode(c, c2);
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

JS_PUBLIC_API size_t JS::GetDeflatedUTF8StringLength(JSLinearString* s) {
  JS::AutoCheckCannotGC nogc;
  return s->hasLatin1Chars()
             ? ::GetDeflatedUTF8StringLength(s->latin1Chars(nogc), s->length())
             : ::GetDeflatedUTF8StringLength(s->twoByteChars(nogc),
                                             s->length());
}

JS_PUBLIC_API size_t JS::DeflateStringToUTF8Buffer(JSLinearString* src,
                                                   mozilla::Span<char> dst) {
  JS::AutoCheckCannotGC nogc;
  if (src->hasLatin1Chars()) {
    auto source = AsChars(Span(src->latin1Chars(nogc), src->length()));
    size_t read;
    size_t written;
    Tie(read, written) = ConvertLatin1toUtf8Partial(source, dst);
    (void)read;
    return written;
  }
  auto source = Span(src->twoByteChars(nogc), src->length());
  size_t read;
  size_t written;
  Tie(read, written) = ConvertUtf16toUtf8Partial(source, dst);
  (void)read;
  return written;
}

template <typename CharT>
void ConvertToUTF8(mozilla::Span<CharT> src, mozilla::Span<char> dst);

template <>
void ConvertToUTF8<const char16_t>(mozilla::Span<const char16_t> src,
                                   mozilla::Span<char> dst) {
  (void)ConvertUtf16toUtf8Partial(src, dst);
}

template <>
void ConvertToUTF8<const Latin1Char>(mozilla::Span<const Latin1Char> src,
                                     mozilla::Span<char> dst) {
  (void)ConvertLatin1toUtf8Partial(AsChars(src), dst);
}

template <typename CharT>
UTF8CharsZ JS::CharsToNewUTF8CharsZ(JSContext* cx,
                                    const mozilla::Range<CharT> chars) {
  /* Get required buffer size. */
  const CharT* str = chars.begin().get();
  size_t len = ::GetDeflatedUTF8StringLength(str, chars.length());

  /* Allocate buffer. */
  char* utf8 = cx->pod_malloc<char>(len + 1);
  if (!utf8) {
    return UTF8CharsZ();
  }

  /* Encode to UTF8. */
  ::ConvertToUTF8(Span(str, chars.length()), Span(utf8, len));
  utf8[len] = '\0';

  return UTF8CharsZ(utf8, len);
}

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    JSContext* cx, const mozilla::Range<Latin1Char> chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    JSContext* cx, const mozilla::Range<char16_t> chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    JSContext* cx, const mozilla::Range<const Latin1Char> chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    JSContext* cx, const mozilla::Range<const char16_t> chars);

static const uint32_t INVALID_UTF8 = UINT32_MAX;

/*
 * Convert a UTF-8 character sequence into a UCS-4 character and return that
 * character. It is assumed that the caller already checked that the sequence
 * is valid.
 */
static uint32_t Utf8ToOneUcs4CharImpl(const uint8_t* utf8Buffer,
                                      int utf8Length) {
  MOZ_ASSERT(1 <= utf8Length && utf8Length <= 4);

  if (utf8Length == 1) {
    MOZ_ASSERT(!(*utf8Buffer & 0x80));
    return *utf8Buffer;
  }

  /* from Unicode 3.1, non-shortest form is illegal */
  static const uint32_t minucs4Table[] = {0x80, 0x800, NonBMPMin};

  MOZ_ASSERT((*utf8Buffer & (0x100 - (1 << (7 - utf8Length)))) ==
             (0x100 - (1 << (8 - utf8Length))));
  uint32_t ucs4Char = *utf8Buffer++ & ((1 << (7 - utf8Length)) - 1);
  uint32_t minucs4Char = minucs4Table[utf8Length - 2];
  while (--utf8Length) {
    MOZ_ASSERT((*utf8Buffer & 0xC0) == 0x80);
    ucs4Char = (ucs4Char << 6) | (*utf8Buffer++ & 0x3F);
  }

  if (MOZ_UNLIKELY(ucs4Char < minucs4Char)) {
    return INVALID_UTF8;
  }

  if (MOZ_UNLIKELY(IsSurrogate(ucs4Char))) {
    return INVALID_UTF8;
  }

  return ucs4Char;
}

uint32_t JS::Utf8ToOneUcs4Char(const uint8_t* utf8Buffer, int utf8Length) {
  return Utf8ToOneUcs4CharImpl(utf8Buffer, utf8Length);
}

static void ReportInvalidCharacter(JSContext* cx, uint32_t offset) {
  char buffer[10];
  SprintfLiteral(buffer, "%u", offset);
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_MALFORMED_UTF8_CHAR, buffer);
}

static void ReportBufferTooSmall(JSContext* cx, uint32_t dummy) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_BUFFER_TOO_SMALL);
}

static void ReportTooBigCharacter(JSContext* cx, uint32_t v) {
  char buffer[11];
  SprintfLiteral(buffer, "0x%x", v);
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_UTF8_CHAR_TOO_LARGE, buffer);
}

enum class LoopDisposition {
  Break,
  Continue,
};

enum class OnUTF8Error {
  InsertReplacementCharacter,
  InsertQuestionMark,
  Throw,
  Crash,
};

// Scan UTF-8 input and (internally, at least) convert it to a series of UTF-16
// code units. But you can also do odd things like pass an empty lambda for
// `dst`, in which case the output is discarded entirely--the only effect of
// calling the template that way is error-checking.
template <OnUTF8Error ErrorAction, typename OutputFn>
static bool InflateUTF8ToUTF16(JSContext* cx, const UTF8Chars src,
                               OutputFn dst) {
  size_t srclen = src.length();
  for (uint32_t i = 0; i < srclen; i++) {
    uint32_t v = uint32_t(src[i]);
    if (!(v & 0x80)) {
      // ASCII code unit.  Simple copy.
      if (dst(uint16_t(v)) == LoopDisposition::Break) {
        break;
      }
    } else {
      // Non-ASCII code unit.  Determine its length in bytes (n).
      uint32_t n = 1;
      while (v & (0x80 >> n)) {
        n++;
      }

#define INVALID(report, arg, n2)                                    \
  do {                                                              \
    if (ErrorAction == OnUTF8Error::Throw) {                        \
      report(cx, arg);                                              \
      return false;                                                 \
    } else if (ErrorAction == OnUTF8Error::Crash) {                 \
      MOZ_CRASH("invalid UTF-8 string: " #report);                  \
    } else {                                                        \
      char16_t replacement;                                         \
      if (ErrorAction == OnUTF8Error::InsertReplacementCharacter) { \
        replacement = REPLACEMENT_CHARACTER;                        \
      } else {                                                      \
        MOZ_ASSERT(ErrorAction == OnUTF8Error::InsertQuestionMark); \
        replacement = '?';                                          \
      }                                                             \
      if (dst(replacement) == LoopDisposition::Break) {             \
        break;                                                      \
      }                                                             \
      n = n2;                                                       \
      goto invalidMultiByteCodeUnit;                                \
    }                                                               \
  } while (0)

      // Check the leading byte.
      if (n < 2 || n > 4) {
        INVALID(ReportInvalidCharacter, i, 1);
      }

      // Check that |src| is large enough to hold an n-byte code unit.
      if (i + n > srclen) {
        INVALID(ReportBufferTooSmall, /* dummy = */ 0, 1);
      }

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
        if ((src[i + m] & 0xC0) != 0x80) {
          INVALID(ReportInvalidCharacter, i, m);
        }
      }

      // Determine the code unit's length in CharT and act accordingly.
      v = Utf8ToOneUcs4CharImpl((uint8_t*)&src[i], n);
      if (v < NonBMPMin) {
        // The n-byte UTF8 code unit will fit in a single CharT.
        if (dst(char16_t(v)) == LoopDisposition::Break) {
          break;
        }
      } else if (v <= NonBMPMax) {
        // The n-byte UTF8 code unit will fit in two CharT units.
        if (dst(LeadSurrogate(v)) == LoopDisposition::Break) {
          break;
        }
        if (dst(TrailSurrogate(v)) == LoopDisposition::Break) {
          break;
        }
      } else {
        // The n-byte UTF8 code unit won't fit in two CharT units.
        INVALID(ReportTooBigCharacter, v, 1);
      }

    invalidMultiByteCodeUnit:
      // Move i to the last byte of the multi-byte code unit; the loop
      // header will do the final i++ to move to the start of the next
      // code unit.
      i += n - 1;
    }
  }

  return true;
}

template <OnUTF8Error ErrorAction, typename CharT>
static void CopyAndInflateUTF8IntoBuffer(JSContext* cx, const UTF8Chars src,
                                         CharT* dst, size_t outlen,
                                         bool allASCII) {
  if (allASCII) {
    size_t srclen = src.length();
    MOZ_ASSERT(outlen == srclen);
    for (uint32_t i = 0; i < srclen; i++) {
      dst[i] = CharT(src[i]);
    }
  } else {
    size_t j = 0;
    auto push = [dst, &j](char16_t c) -> LoopDisposition {
      dst[j++] = CharT(c);
      return LoopDisposition::Continue;
    };
    MOZ_ALWAYS_TRUE((InflateUTF8ToUTF16<ErrorAction>(cx, src, push)));
    MOZ_ASSERT(j == outlen);
  }
  dst[outlen] = CharT('\0');  // NUL char
}

template <OnUTF8Error ErrorAction, typename CharsT>
static CharsT InflateUTF8StringHelper(JSContext* cx, const UTF8Chars src,
                                      size_t* outlen, arena_id_t destArenaId) {
  using CharT = typename CharsT::CharT;
  static_assert(
      std::is_same_v<CharT, char16_t> || std::is_same_v<CharT, Latin1Char>,
      "bad CharT");

  *outlen = 0;

  size_t len = 0;
  bool allASCII = true;
  auto count = [&len, &allASCII](char16_t c) -> LoopDisposition {
    len++;
    allASCII &= (c < 0x80);
    return LoopDisposition::Continue;
  };
  if (!InflateUTF8ToUTF16<ErrorAction>(cx, src, count)) {
    return CharsT();
  }
  *outlen = len;

  CharT* dst = cx->pod_arena_malloc<CharT>(destArenaId,
                                           *outlen + 1);  // +1 for NUL

  if (!dst) {
    ReportOutOfMemory(cx);
    return CharsT();
  }

  constexpr OnUTF8Error errorMode =
      std::is_same_v<CharT, Latin1Char>
          ? OnUTF8Error::InsertQuestionMark
          : OnUTF8Error::InsertReplacementCharacter;
  CopyAndInflateUTF8IntoBuffer<errorMode>(cx, src, dst, *outlen, allASCII);

  return CharsT(dst, *outlen);
}

TwoByteCharsZ JS::UTF8CharsToNewTwoByteCharsZ(JSContext* cx,
                                              const UTF8Chars utf8,
                                              size_t* outlen,
                                              arena_id_t destArenaId) {
  return InflateUTF8StringHelper<OnUTF8Error::Throw, TwoByteCharsZ>(
      cx, utf8, outlen, destArenaId);
}

TwoByteCharsZ JS::UTF8CharsToNewTwoByteCharsZ(JSContext* cx,
                                              const ConstUTF8CharsZ& utf8,
                                              size_t* outlen,
                                              arena_id_t destArenaId) {
  UTF8Chars chars(utf8.c_str(), strlen(utf8.c_str()));
  return InflateUTF8StringHelper<OnUTF8Error::Throw, TwoByteCharsZ>(
      cx, chars, outlen, destArenaId);
}

TwoByteCharsZ JS::LossyUTF8CharsToNewTwoByteCharsZ(JSContext* cx,
                                                   const JS::UTF8Chars utf8,
                                                   size_t* outlen,
                                                   arena_id_t destArenaId) {
  return InflateUTF8StringHelper<OnUTF8Error::InsertReplacementCharacter,
                                 TwoByteCharsZ>(cx, utf8, outlen, destArenaId);
}

TwoByteCharsZ JS::LossyUTF8CharsToNewTwoByteCharsZ(
    JSContext* cx, const JS::ConstUTF8CharsZ& utf8, size_t* outlen,
    arena_id_t destArenaId) {
  UTF8Chars chars(utf8.c_str(), strlen(utf8.c_str()));
  return InflateUTF8StringHelper<OnUTF8Error::InsertReplacementCharacter,
                                 TwoByteCharsZ>(cx, chars, outlen, destArenaId);
}

static void UpdateSmallestEncodingForChar(char16_t c,
                                          JS::SmallestEncoding* encoding) {
  JS::SmallestEncoding newEncoding = JS::SmallestEncoding::ASCII;
  if (c >= 0x80) {
    if (c < 0x100) {
      newEncoding = JS::SmallestEncoding::Latin1;
    } else {
      newEncoding = JS::SmallestEncoding::UTF16;
    }
  }
  if (newEncoding > *encoding) {
    *encoding = newEncoding;
  }
}

JS::SmallestEncoding JS::FindSmallestEncoding(UTF8Chars utf8) {
  Span<unsigned char> unsignedSpan = utf8;
  auto charSpan = AsChars(unsignedSpan);
  size_t upTo = AsciiValidUpTo(charSpan);
  if (upTo == charSpan.Length()) {
    return SmallestEncoding::ASCII;
  }
  if (IsUtf8Latin1(charSpan.From(upTo))) {
    return SmallestEncoding::Latin1;
  }
  return SmallestEncoding::UTF16;
}

Latin1CharsZ JS::UTF8CharsToNewLatin1CharsZ(JSContext* cx, const UTF8Chars utf8,
                                            size_t* outlen,
                                            arena_id_t destArenaId) {
  return InflateUTF8StringHelper<OnUTF8Error::Throw, Latin1CharsZ>(
      cx, utf8, outlen, destArenaId);
}

Latin1CharsZ JS::LossyUTF8CharsToNewLatin1CharsZ(JSContext* cx,
                                                 const UTF8Chars utf8,
                                                 size_t* outlen,
                                                 arena_id_t destArenaId) {
  return InflateUTF8StringHelper<OnUTF8Error::InsertQuestionMark, Latin1CharsZ>(
      cx, utf8, outlen, destArenaId);
}

/**
 * Atomization Helpers.
 *
 * These functions are extremely single-use, and are not intended for general
 * consumption.
 */

bool GetUTF8AtomizationData(JSContext* cx, const JS::UTF8Chars utf8,
                            size_t* outlen, JS::SmallestEncoding* encoding,
                            HashNumber* hashNum) {
  *outlen = 0;
  *encoding = JS::SmallestEncoding::ASCII;
  *hashNum = 0;

  auto getMetadata = [outlen, encoding,
                      hashNum](char16_t c) -> LoopDisposition {
    (*outlen)++;
    UpdateSmallestEncodingForChar(c, encoding);
    *hashNum = mozilla::AddToHash(*hashNum, c);
    return LoopDisposition::Continue;
  };
  if (!InflateUTF8ToUTF16<OnUTF8Error::Throw>(cx, utf8, getMetadata)) {
    return false;
  }

  return true;
}

template <typename CharT>
bool UTF8EqualsChars(const JS::UTF8Chars utfChars, const CharT* chars) {
  size_t ind = 0;
  bool isEqual = true;

  auto checkEqual = [&isEqual, &ind, chars](char16_t c) -> LoopDisposition {
#ifdef DEBUG
    JS::SmallestEncoding encoding = JS::SmallestEncoding::ASCII;
    UpdateSmallestEncodingForChar(c, &encoding);
    if (std::is_same_v<CharT, JS::Latin1Char>) {
      MOZ_ASSERT(encoding <= JS::SmallestEncoding::Latin1);
    } else if (!std::is_same_v<CharT, char16_t>) {
      MOZ_CRASH("Invalid character type in UTF8EqualsChars");
    }
#endif

    if (CharT(c) != chars[ind]) {
      isEqual = false;
      return LoopDisposition::Break;
    }

    ind++;
    return LoopDisposition::Continue;
  };

  // To get here, you must have checked your work.
  InflateUTF8ToUTF16<OnUTF8Error::Crash>(/* cx = */ nullptr, utfChars,
                                         checkEqual);

  return isEqual;
}

template bool UTF8EqualsChars(const JS::UTF8Chars, const char16_t*);
template bool UTF8EqualsChars(const JS::UTF8Chars, const JS::Latin1Char*);

template <typename CharT>
void InflateUTF8CharsToBufferAndTerminate(const JS::UTF8Chars src, CharT* dst,
                                          size_t dstLen,
                                          JS::SmallestEncoding encoding) {
  CopyAndInflateUTF8IntoBuffer<OnUTF8Error::Crash>(
      /* cx = */ nullptr, src, dst, dstLen,
      encoding == JS::SmallestEncoding::ASCII);
}

template void InflateUTF8CharsToBufferAndTerminate(
    const UTF8Chars src, char16_t* dst, size_t dstLen,
    JS::SmallestEncoding encoding);
template void InflateUTF8CharsToBufferAndTerminate(
    const UTF8Chars src, JS::Latin1Char* dst, size_t dstLen,
    JS::SmallestEncoding encoding);

#ifdef DEBUG
void JS::ConstUTF8CharsZ::validate(size_t aLength) {
  MOZ_ASSERT(data_);
  UTF8Chars chars(data_, aLength);
  auto nop = [](char16_t) -> LoopDisposition {
    return LoopDisposition::Continue;
  };
  InflateUTF8ToUTF16<OnUTF8Error::Crash>(/* cx = */ nullptr, chars, nop);
}
#endif

bool JS::StringIsASCII(const char* s) {
  while (*s) {
    if (*s & 0x80) {
      return false;
    }
    s++;
  }
  return true;
}

bool JS::StringIsASCII(Span<const char> s) { return IsAscii(s); }

bool StringBuffer::append(const Utf8Unit* units, size_t len) {
  if (isLatin1()) {
    Latin1CharBuffer& latin1 = latin1Chars();

    while (len > 0) {
      if (!IsAscii(*units)) {
        break;
      }

      if (!latin1.append(units->toUnsignedChar())) {
        return false;
      }

      ++units;
      --len;
    }
    if (len == 0) {
      return true;
    }

    // Non-ASCII doesn't *necessarily* mean we couldn't keep appending to
    // |latin1|, but it's only possible for [U+0080, U+0100) code points,
    // and handling the full complexity of UTF-8 only for that very small
    // additional range isn't worth it.  Inflate to two-byte storage before
    // appending the remaining code points.
    if (!inflateChars()) {
      return false;
    }
  }

  UTF8Chars remainingUtf8(units, len);

  // Determine how many UTF-16 code units are required to represent the
  // remaining units.
  size_t utf16Len = 0;
  auto countInflated = [&utf16Len](char16_t c) -> LoopDisposition {
    utf16Len++;
    return LoopDisposition::Continue;
  };
  if (!InflateUTF8ToUTF16<OnUTF8Error::Throw>(cx_, remainingUtf8,
                                              countInflated)) {
    return false;
  }

  TwoByteCharBuffer& buf = twoByteChars();

  size_t i = buf.length();
  if (!buf.growByUninitialized(utf16Len)) {
    return false;
  }
  MOZ_ASSERT(i + utf16Len == buf.length(),
             "growByUninitialized assumed to increase length immediately");

  char16_t* toFill = &buf[i];
  auto appendUtf16 = [&toFill](char16_t unit) {
    *toFill++ = unit;
    return LoopDisposition::Continue;
  };

  MOZ_ALWAYS_TRUE(
      InflateUTF8ToUTF16<OnUTF8Error::Throw>(cx_, remainingUtf8, appendUtf16));
  MOZ_ASSERT(toFill == buf.end());
  return true;
}
