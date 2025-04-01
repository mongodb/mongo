/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/CharacterEncoding.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Latin1.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"
#include "mozilla/Span.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Utf8.h"

#ifndef XP_LINUX
// We still support libstd++ versions without codecvt support on Linux.
//
// When the minimum supported libstd++ version is bumped to 3.4.21, we can
// enable the codecvt code path for Linux, too. This should happen in 2024 when
// support for CentOS 7 is removed.
#  include <codecvt>
#endif
#include <cwchar>
#include <limits>
#include <locale>
#include <type_traits>

#include "frontend/FrontendContext.h"
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
using mozilla::Utf8Unit;

using JS::Latin1CharsZ;
using JS::TwoByteCharsZ;
using JS::UTF8Chars;
using JS::UTF8CharsZ;

using namespace js;
using namespace js::unicode;

Latin1CharsZ JS::LossyTwoByteCharsToNewLatin1CharsZ(
    JSContext* cx, const mozilla::Range<const char16_t>& tbchars) {
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
    char32_t v;
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
    auto [read, written] = ConvertLatin1toUtf8Partial(source, dst);
    (void)read;
    return written;
  }
  auto source = Span(src->twoByteChars(nogc), src->length());
  auto [read, written] = ConvertUtf16toUtf8Partial(source, dst);
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

template <typename CharT, typename Allocator>
UTF8CharsZ JS::CharsToNewUTF8CharsZ(Allocator* alloc,
                                    const mozilla::Range<CharT>& chars) {
  /* Get required buffer size. */
  const CharT* str = chars.begin().get();
  size_t len = ::GetDeflatedUTF8StringLength(str, chars.length());

  /* Allocate buffer. */
  char* utf8 = alloc->template pod_malloc<char>(len + 1);
  if (!utf8) {
    return UTF8CharsZ();
  }

  /* Encode to UTF8. */
  ::ConvertToUTF8(Span(str, chars.length()), Span(utf8, len));
  utf8[len] = '\0';

  return UTF8CharsZ(utf8, len);
}

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    JSContext* cx, const mozilla::Range<Latin1Char>& chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    JSContext* cx, const mozilla::Range<char16_t>& chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    JSContext* cx, const mozilla::Range<const Latin1Char>& chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    JSContext* cx, const mozilla::Range<const char16_t>& chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    FrontendAllocator* cx, const mozilla::Range<Latin1Char>& chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    FrontendAllocator* cx, const mozilla::Range<char16_t>& chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    FrontendAllocator* cx, const mozilla::Range<const Latin1Char>& chars);

template UTF8CharsZ JS::CharsToNewUTF8CharsZ(
    FrontendAllocator* cx, const mozilla::Range<const char16_t>& chars);

static constexpr uint32_t INVALID_UTF8 = std::numeric_limits<char32_t>::max();

/*
 * Convert a UTF-8 character sequence into a UCS-4 character and return that
 * character. It is assumed that the caller already checked that the sequence
 * is valid.
 */
static char32_t Utf8ToOneUcs4CharImpl(const uint8_t* utf8Buffer,
                                      int utf8Length) {
  MOZ_ASSERT(1 <= utf8Length && utf8Length <= 4);

  if (utf8Length == 1) {
    MOZ_ASSERT(!(*utf8Buffer & 0x80));
    return *utf8Buffer;
  }

  /* from Unicode 3.1, non-shortest form is illegal */
  static const char32_t minucs4Table[] = {0x80, 0x800, NonBMPMin};

  MOZ_ASSERT((*utf8Buffer & (0x100 - (1 << (7 - utf8Length)))) ==
             (0x100 - (1 << (8 - utf8Length))));
  char32_t ucs4Char = *utf8Buffer++ & ((1 << (7 - utf8Length)) - 1);
  char32_t minucs4Char = minucs4Table[utf8Length - 2];
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

char32_t JS::Utf8ToOneUcs4Char(const uint8_t* utf8Buffer, int utf8Length) {
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

inline bool IsInvalidSecondByte(uint32_t first, uint8_t second) {
  // Perform an extra check aginst the second byte.
  // From Unicode Standard v6.2, Table 3-7 Well-Formed UTF-8 Byte Sequences.
  //
  // The consumer should perform a followup check for second & 0xC0 == 0x80.
  return (first == 0xE0 && (second & 0xE0) != 0xA0) ||  // E0 A0~BF
         (first == 0xED && (second & 0xE0) != 0x80) ||  // ED 80~9F
         (first == 0xF0 && (second & 0xF0) == 0x80) ||  // F0 90~BF
         (first == 0xF4 && (second & 0xF0) != 0x80);    // F4 80~8F
}

// Scan UTF-8 input and (internally, at least) convert it to a series of UTF-16
// code units. But you can also do odd things like pass an empty lambda for
// `dst`, in which case the output is discarded entirely--the only effect of
// calling the template that way is error-checking.
template <OnUTF8Error ErrorAction, typename OutputFn>
static bool InflateUTF8ToUTF16(JSContext* cx, const UTF8Chars& src,
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

      // Non-ASCII code unit. Determine its length in bytes (n).
      //
      // Avoid undefined behavior from passing in 0
      // (https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html#index-_005f_005fbuiltin_005fclz)
      // by turning on the low bit so that 0xff will set n=31-24=7, which will
      // be detected as an invalid character.
      uint32_t n = mozilla::CountLeadingZeroes32(~int8_t(src[i]) | 0x1) - 24;

      // Check the leading byte.
      if (n < 2 || n > 4) {
        INVALID(ReportInvalidCharacter, i, 1);
      }

      // Check that |src| is large enough to hold an n-byte code unit.
      if (i + n > srclen) {
        // Check the second and continuation bytes, to replace maximal subparts
        // of an ill-formed subsequence with single U+FFFD.
        if (i + 2 > srclen) {
          INVALID(ReportBufferTooSmall, /* dummy = */ 0, 1);
        }

        if (IsInvalidSecondByte(v, (uint8_t)src[i + 1])) {
          INVALID(ReportInvalidCharacter, i, 1);
        }

        if ((src[i + 1] & 0xC0) != 0x80) {
          INVALID(ReportInvalidCharacter, i, 1);
        }

        if (n == 3) {
          INVALID(ReportInvalidCharacter, i, 2);
        } else {
          if (i + 3 > srclen) {
            INVALID(ReportBufferTooSmall, /* dummy = */ 0, 2);
          }
          if ((src[i + 2] & 0xC0) != 0x80) {
            INVALID(ReportInvalidCharacter, i, 2);
          }
          INVALID(ReportInvalidCharacter, i, 3);
        }
      }

      if (IsInvalidSecondByte(v, (uint8_t)src[i + 1])) {
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
static void CopyAndInflateUTF8IntoBuffer(JSContext* cx, const UTF8Chars& src,
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
}

template <OnUTF8Error ErrorAction, typename CharsT>
static CharsT InflateUTF8StringHelper(JSContext* cx, const UTF8Chars& src,
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
  dst[*outlen] = CharT('\0');

  return CharsT(dst, *outlen);
}

TwoByteCharsZ JS::UTF8CharsToNewTwoByteCharsZ(JSContext* cx,
                                              const UTF8Chars& utf8,
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
                                                   const JS::UTF8Chars& utf8,
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

JS::SmallestEncoding JS::FindSmallestEncoding(const UTF8Chars& utf8) {
  Span<const unsigned char> unsignedSpan = utf8;
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

Latin1CharsZ JS::UTF8CharsToNewLatin1CharsZ(JSContext* cx,
                                            const UTF8Chars& utf8,
                                            size_t* outlen,
                                            arena_id_t destArenaId) {
  return InflateUTF8StringHelper<OnUTF8Error::Throw, Latin1CharsZ>(
      cx, utf8, outlen, destArenaId);
}

Latin1CharsZ JS::LossyUTF8CharsToNewLatin1CharsZ(JSContext* cx,
                                                 const UTF8Chars& utf8,
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

bool GetUTF8AtomizationData(JSContext* cx, const JS::UTF8Chars& utf8,
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
bool UTF8EqualsChars(const JS::UTF8Chars& utfChars, const CharT* chars) {
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

template bool UTF8EqualsChars(const JS::UTF8Chars&, const char16_t*);
template bool UTF8EqualsChars(const JS::UTF8Chars&, const JS::Latin1Char*);

template <typename CharT>
void InflateUTF8CharsToBuffer(const JS::UTF8Chars& src, CharT* dst,
                              size_t dstLen, JS::SmallestEncoding encoding) {
  CopyAndInflateUTF8IntoBuffer<OnUTF8Error::Crash>(
      /* cx = */ nullptr, src, dst, dstLen,
      encoding == JS::SmallestEncoding::ASCII);
}

template void InflateUTF8CharsToBuffer(const UTF8Chars& src, char16_t* dst,
                                       size_t dstLen,
                                       JS::SmallestEncoding encoding);
template void InflateUTF8CharsToBuffer(const UTF8Chars& src,
                                       JS::Latin1Char* dst, size_t dstLen,
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
void JS::ConstUTF8CharsZ::validateWithoutLength() {
  MOZ_ASSERT(data_);
  validate(strlen(data_));
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

JS_PUBLIC_API JS::UniqueChars JS::EncodeNarrowToUtf8(JSContext* cx,
                                                     const char* chars) {
  // Convert the narrow multibyte character string to a wide string and then
  // use EncodeWideToUtf8() to convert the wide string to a UTF-8 string.

  std::mbstate_t mb{};

  // NOTE: The 2nd parameter is overwritten even if the 1st parameter is nullptr
  //       on Android NDK older than v16.  Use a temporary variable to save the
  //       `chars` for the subsequent call.  See bug 1492090.
  const char* tmpChars = chars;

  size_t wideLen = std::mbsrtowcs(nullptr, &tmpChars, 0, &mb);
  if (wideLen == size_t(-1)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_CONVERT_TO_WIDE);
    return nullptr;
  }
  MOZ_ASSERT(std::mbsinit(&mb),
             "multi-byte state is in its initial state when no conversion "
             "error occured");

  size_t bufLen = wideLen + 1;
  auto wideChars = cx->make_pod_array<wchar_t>(bufLen);
  if (!wideChars) {
    return nullptr;
  }

  mozilla::DebugOnly<size_t> actualLen =
      std::mbsrtowcs(wideChars.get(), &chars, bufLen, &mb);
  MOZ_ASSERT(wideLen == actualLen);
  MOZ_ASSERT(wideChars[actualLen] == '\0');

  return EncodeWideToUtf8(cx, wideChars.get());
}

JS_PUBLIC_API JS::UniqueChars JS::EncodeWideToUtf8(JSContext* cx,
                                                   const wchar_t* chars) {
  using CheckedSizeT = mozilla::CheckedInt<size_t>;

#ifndef XP_LINUX
  // Use the standard codecvt facet to convert a wide string to UTF-8.
  std::codecvt_utf8<wchar_t> cv;

  size_t len = std::wcslen(chars);
  CheckedSizeT utf8MaxLen = CheckedSizeT(len) * cv.max_length();
  CheckedSizeT utf8BufLen = utf8MaxLen + 1;
  if (!utf8BufLen.isValid()) {
    JS_ReportAllocationOverflow(cx);
    return nullptr;
  }
  auto utf8 = cx->make_pod_array<char>(utf8BufLen.value());
  if (!utf8) {
    return nullptr;
  }

  // STL returns |codecvt_base::partial| for empty strings.
  if (len == 0) {
    utf8[0] = '\0';  // Explicit null-termination required.
    return utf8;
  }

  std::mbstate_t mb{};
  const wchar_t* fromNext;
  char* toNext;
  std::codecvt_base::result result =
      cv.out(mb, chars, chars + len, fromNext, utf8.get(),
             utf8.get() + utf8MaxLen.value(), toNext);
  if (result != std::codecvt_base::ok) {
    MOZ_ASSERT(result == std::codecvt_base::error);
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_CONVERT_WIDE_TO_UTF8);
    return nullptr;
  }
  *toNext = '\0';  // Explicit null-termination required.

  // codecvt_utf8 doesn't validate its output and may produce WTF-8 instead
  // of UTF-8 on some platforms when the input contains unpaired surrogate
  // characters. We don't allow this.
  if (!mozilla::IsUtf8(
          mozilla::Span(utf8.get(), size_t(toNext - utf8.get())))) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_CONVERT_WIDE_TO_UTF8);
    return nullptr;
  }

  return utf8;
#else
  // Alternative code path for Linux, because we still support libstd++ versions
  // without codecvt support. See also the top comment where <codecvt> is
  // included.

  static_assert(sizeof(wchar_t) == 4,
                "Assume wchar_t is UTF-32 on Linux systems");

  constexpr size_t MaxUtf8CharLength = 4;

  size_t len = std::wcslen(chars);
  CheckedSizeT utf8MaxLen = CheckedSizeT(len) * MaxUtf8CharLength;
  CheckedSizeT utf8BufLen = utf8MaxLen + 1;
  if (!utf8BufLen.isValid()) {
    JS_ReportAllocationOverflow(cx);
    return nullptr;
  }
  auto utf8 = cx->make_pod_array<char>(utf8BufLen.value());
  if (!utf8) {
    return nullptr;
  }

  char* dst = utf8.get();
  for (size_t i = 0; i < len; i++) {
    uint8_t utf8buf[MaxUtf8CharLength];
    uint32_t utf8Len = OneUcs4ToUtf8Char(utf8buf, chars[i]);
    for (size_t j = 0; j < utf8Len; j++) {
      *dst++ = char(utf8buf[j]);
    }
  }
  *dst = '\0';

  return utf8;
#endif
}

JS_PUBLIC_API JS::UniqueChars JS::EncodeUtf8ToNarrow(JSContext* cx,
                                                     const char* chars) {
  // Convert the UTF-8 string to a wide string via EncodeUtf8ToWide() and
  // then convert the resulting wide string to a narrow multibyte character
  // string.

  auto wideChars = EncodeUtf8ToWide(cx, chars);
  if (!wideChars) {
    return nullptr;
  }

  const wchar_t* cWideChars = wideChars.get();
  std::mbstate_t mb{};
  size_t narrowLen = std::wcsrtombs(nullptr, &cWideChars, 0, &mb);
  if (narrowLen == size_t(-1)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_CONVERT_TO_NARROW);
    return nullptr;
  }
  MOZ_ASSERT(std::mbsinit(&mb),
             "multi-byte state is in its initial state when no conversion "
             "error occured");

  size_t bufLen = narrowLen + 1;
  auto narrow = cx->make_pod_array<char>(bufLen);
  if (!narrow) {
    return nullptr;
  }

  mozilla::DebugOnly<size_t> actualLen =
      std::wcsrtombs(narrow.get(), &cWideChars, bufLen, &mb);
  MOZ_ASSERT(narrowLen == actualLen);
  MOZ_ASSERT(narrow[actualLen] == '\0');

  return narrow;
}

JS_PUBLIC_API JS::UniqueWideChars JS::EncodeUtf8ToWide(JSContext* cx,
                                                       const char* chars) {
  // Only valid UTF-8 strings should be passed to this function.
  MOZ_ASSERT(mozilla::IsUtf8(mozilla::Span(chars, strlen(chars))));

#ifndef XP_LINUX
  // Use the standard codecvt facet to convert from UTF-8 to a wide string.
  std::codecvt_utf8<wchar_t> cv;

  size_t len = strlen(chars);
  auto wideChars = cx->make_pod_array<wchar_t>(len + 1);
  if (!wideChars) {
    return nullptr;
  }

  // STL returns |codecvt_base::partial| for empty strings.
  if (len == 0) {
    wideChars[0] = '\0';  // Explicit null-termination required.
    return wideChars;
  }

  std::mbstate_t mb{};
  const char* fromNext;
  wchar_t* toNext;
  std::codecvt_base::result result =
      cv.in(mb, chars, chars + len, fromNext, wideChars.get(),
            wideChars.get() + len, toNext);
  if (result != std::codecvt_base::ok) {
    MOZ_ASSERT(result == std::codecvt_base::error);
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_CONVERT_UTF8_TO_WIDE);
    return nullptr;
  }
  *toNext = '\0';  // Explicit null-termination required.

  return wideChars;
#else
  // Alternative code path for Linux, because we still support libstd++ versions
  // without codecvt support. See also the top comment where <codecvt> is
  // included.

  static_assert(sizeof(wchar_t) == 4,
                "Assume wchar_t is UTF-32 on Linux systems");

  size_t len = strlen(chars);
  auto wideChars = cx->make_pod_array<wchar_t>(len + 1);
  if (!wideChars) {
    return nullptr;
  }

  const auto* s = reinterpret_cast<const unsigned char*>(chars);
  const auto* const limit = s + len;

  wchar_t* dst = wideChars.get();
  while (s < limit) {
    unsigned char c = *s++;

    if (mozilla::IsAscii(c)) {
      *dst++ = wchar_t(c);
      continue;
    }

    mozilla::Utf8Unit utf8(c);
    mozilla::Maybe<char32_t> codePoint =
        mozilla::DecodeOneUtf8CodePoint(utf8, &s, limit);
    MOZ_ASSERT(codePoint.isSome());
    *dst++ = wchar_t(*codePoint);
  }
  *dst++ = '\0';

  return wideChars;
#endif
}

bool StringBuffer::append(const Utf8Unit* units, size_t len) {
  MOZ_ASSERT(maybeCx_);

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
  if (!InflateUTF8ToUTF16<OnUTF8Error::Throw>(maybeCx_, remainingUtf8,
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

  MOZ_ALWAYS_TRUE(InflateUTF8ToUTF16<OnUTF8Error::Throw>(
      maybeCx_, remainingUtf8, appendUtf16));
  MOZ_ASSERT(toFill == buf.end());
  return true;
}
