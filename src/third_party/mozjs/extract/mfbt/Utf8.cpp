/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Latin1.h"
#include "mozilla/Maybe.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Types.h"
#include "mozilla/Utf8.h"

#include <functional>  // for std::function
#include <stddef.h>
#include <stdint.h>

////////////////////////////////////////////////////////////
// Utf8.h
////////////////////////////////////////////////////////////

MFBT_API bool mozilla::detail::IsValidUtf8(const void* aCodeUnits,
                                           size_t aCount) {
  const auto* s = reinterpret_cast<const unsigned char*>(aCodeUnits);
  const auto* const limit = s + aCount;

  while (s < limit) {
    unsigned char c = *s++;

    // If the first byte is ASCII, it's the only one in the code point.  Have a
    // fast path that avoids all the rest of the work and looping in that case.
    if (IsAscii(c)) {
      continue;
    }

    Maybe<char32_t> maybeCodePoint =
        DecodeOneUtf8CodePoint(Utf8Unit(c), &s, limit);
    if (maybeCodePoint.isNothing()) {
      return false;
    }
  }

  MOZ_ASSERT(s == limit);
  return true;
}

#if !MOZ_HAS_JSRUST()
#  include <memory>          // for std::shared_ptr
#  include "unicode/ucnv.h"  // for UConverter

mozilla::Tuple<UConverter*, UErrorCode> _getUConverter() {
  static thread_local UErrorCode uConverterErr = U_ZERO_ERROR;
  static thread_local std::shared_ptr<UConverter> utf8Cnv(
      ucnv_open("UTF-8", &uConverterErr), ucnv_close);
  return mozilla::MakeTuple(utf8Cnv.get(), uConverterErr);
}

mozilla::Tuple<size_t, size_t> mozilla::ConvertUtf16toUtf8Partial(
    mozilla::Span<const char16_t> aSource, mozilla::Span<char> aDest) {
  const char16_t* srcOrigPtr = aSource.Elements();
  const char16_t* srcPtr = srcOrigPtr;
  const char16_t* srcLimit = srcPtr + aSource.Length();
  char* dstOrigPtr = aDest.Elements();
  char* dstPtr = dstOrigPtr;
  const char* dstLimit = dstPtr + aDest.Length();

  // Thread-local instance of a UTF-8 converter
  UConverter* utf8Conv;
  UErrorCode uConverterErr;
  Tie(utf8Conv, uConverterErr) = _getUConverter();

  if (MOZ_LIKELY(U_SUCCESS(uConverterErr) && utf8Conv != NULL)) {
    UErrorCode err = U_ZERO_ERROR;
    do {
      ucnv_fromUnicode(utf8Conv, &dstPtr, dstLimit, &srcPtr, srcLimit, nullptr,
                       true, &err);
      ucnv_reset(utf8Conv); /* ucnv_fromUnicode is a stateful operation */
      if (MOZ_UNLIKELY(U_FAILURE(err))) {
        if (err == U_BUFFER_OVERFLOW_ERROR) {
          const size_t firstInvalid =
              Utf8ValidUpToIndex(Span(dstOrigPtr, dstPtr));
          MOZ_ASSERT(static_cast<size_t>(srcPtr - srcOrigPtr) >= 0);
          MOZ_ASSERT(static_cast<size_t>(dstLimit - dstOrigPtr) >=
                     firstInvalid);
          const size_t incorrectCharLen =
              static_cast<size_t>(dstLimit - dstOrigPtr) - firstInvalid;
          char* ptr = dstOrigPtr + firstInvalid;
          switch (incorrectCharLen) {
            case 3:
              // TRIPLE_BYTE_REPLACEMENT_CHAR
              *ptr++ = 0xEF;
              *ptr++ = 0xBF;
              *ptr++ = 0xBD;
              break;
            case 2:
              // DOUBLE_BYTE_REPLACEMENT_CHAR
              *ptr++ = 0xC2;
              *ptr++ = 0xBF;
              break;
            case 1:
              // SINGLE_BYTE_REPLACEMENT_CHAR
            default:
              for (; ptr < dstLimit; ++ptr) {
                *ptr = '?';  // REPLACEMENT CHAR
              }
              break;
            case 0:
              break;
          }
          return mozilla::MakeTuple(static_cast<size_t>(srcPtr - srcOrigPtr),
                                    static_cast<size_t>(dstPtr - dstOrigPtr));
        } else {
          // We do not need to handle it, as the problematic character will be
          // replaced with a REPLACEMENT CHARACTER.
        }
      }

      if (MOZ_UNLIKELY(srcPtr < srcLimit && dstPtr < dstLimit)) {
        ++srcPtr;
        *dstPtr = '?';  // REPLACEMENT CHAR
        ++dstPtr;
      }
    } while (srcPtr < srcLimit && dstPtr < dstLimit);
  }

  return mozilla::MakeTuple(static_cast<size_t>(srcPtr - srcOrigPtr),
                            static_cast<size_t>(dstPtr - dstOrigPtr));
}

size_t mozilla::ConvertUtf16toUtf8(mozilla::Span<const char16_t> aSource,
                                   mozilla::Span<char> aDest) {
  MOZ_ASSERT(aDest.Length() >= aSource.Length() * 3);
  size_t read;
  size_t written;
  Tie(read, written) = mozilla::ConvertUtf16toUtf8Partial(aSource, aDest);
  MOZ_ASSERT(read == aSource.Length());
  return written;
}

size_t mozilla::ConvertUtf8toUtf16(mozilla::Span<const char> aSource,
                                   mozilla::Span<char16_t> aDest) {
  MOZ_ASSERT(aDest.Length() > aSource.Length());

  const char* srcOrigPtr = aSource.Elements();
  const char* srcPtr = srcOrigPtr;
  const char* srcLimit = srcPtr + aSource.Length();
  char16_t* dstOrigPtr = aDest.Elements();
  char16_t* dstPtr = dstOrigPtr;
  const char16_t* dstLimit = dstPtr + aDest.Length();

  // Thread-local instance of a UTF-8 converter
  UConverter* utf8Conv;
  UErrorCode uConverterErr;
  Tie(utf8Conv, uConverterErr) = _getUConverter();

  if (MOZ_LIKELY(U_SUCCESS(uConverterErr) && utf8Conv != NULL)) {
    UErrorCode err = U_ZERO_ERROR;
    do {
      ucnv_toUnicode(utf8Conv, &dstPtr, dstLimit, &srcPtr, srcLimit, nullptr,
                     true, &err);
      if (MOZ_UNLIKELY(U_FAILURE(err))) {
        // We do not need to handle it, as the problematic character will be
        // replaced with a REPLACEMENT CHARACTER.
      }

      if (MOZ_UNLIKELY(srcPtr < srcLimit && dstPtr < dstLimit)) {
        ++srcPtr;
        *dstPtr = '?';  // REPLACEMENT CHAR
        ++dstPtr;
      }
    } while (srcPtr < srcLimit && dstPtr < dstLimit);
  }
  return static_cast<size_t>(dstPtr - dstOrigPtr);
}

size_t mozilla::UnsafeConvertValidUtf8toUtf16(mozilla::Span<const char> aSource,
                                              mozilla::Span<char16_t> aDest) {
  const char* srcOrigPtr = aSource.Elements();
  const char* srcPtr = srcOrigPtr;
  size_t srcLen = aSource.Length();
  const char* srcLimit = srcPtr + srcLen;
  char16_t* dstOrigPtr = aDest.Elements();
  char16_t* dstPtr = dstOrigPtr;
  size_t dstLen = aDest.Length();
  const char16_t* dstLimit = dstPtr + dstLen;

  MOZ_ASSERT(dstLen >= srcLen);

  // Thread-local instance of a UTF-8 converter
  UConverter* utf8Conv;
  UErrorCode uConverterErr;
  Tie(utf8Conv, uConverterErr) = _getUConverter();

  if (MOZ_LIKELY(U_SUCCESS(uConverterErr) && utf8Conv != NULL)) {
    UErrorCode err = U_ZERO_ERROR;

    ucnv_toUnicode(utf8Conv, &dstPtr, dstLimit, &srcPtr, srcLimit, nullptr,
                   true, &err);
    MOZ_ASSERT(!U_FAILURE(err));

    MOZ_ASSERT(srcPtr == srcLimit);
  }

  return static_cast<size_t>(dstPtr - dstOrigPtr);
}

////////////////////////////////////////////////////////////
// TextUtils.h
////////////////////////////////////////////////////////////

size_t mozilla::Utf16ValidUpTo(mozilla::Span<const char16_t> aString) {
  size_t length = aString.Length();
  const char16_t* ptr = aString.Elements();
  if (!length) {
    return 0;
  }
  size_t offset = 0;
  while (true) {
    char16_t unit = ptr[offset];
    size_t next = offset + 1;

    char16_t unit_minus_surrogate_start = (unit - 0xD800);
    if (unit_minus_surrogate_start > (0xDFFF - 0xD800)) {
      // Not a surrogate
      offset = next;
      if (offset == length) {
        return offset;
      }
      continue;
    }

    if (unit_minus_surrogate_start <= (0xDBFF - 0xD800)) {
      // high surrogate
      if (next < length) {
        char16_t second = ptr[next];
        char16_t second_minus_low_surrogate_start = (second - 0xDC00);
        if (second_minus_low_surrogate_start <= (0xDFFF - 0xDC00)) {
          // The next code unit is a low surrogate. Advance position.
          offset = next + 1;
          if (offset == length) {
            return offset;
          }
          continue;
        }
        // The next code unit is not a low surrogate. Don't advance
        // position and treat the high surrogate as unpaired.
        // fall through
      }
      // Unpaired, fall through
    }
    // Unpaired surrogate
    return offset;
  }
  return offset;
}

////////////////////////////////////////////////////////////
// Latin1.h
////////////////////////////////////////////////////////////

size_t mozilla::Utf8ValidUpToIndex(mozilla::Span<const char> aString) {
  size_t length = aString.Length();
  const char* string = aString.Elements();
  if (!length) return 0;

  size_t i = 0;
  while (i < length) {
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(string + i);
    if (  // ASCII
        bytes[0] <= 0x7F) {
      i += 1;
      continue;
    }

    if (length - i > 1 && (  // non-overlong 2-byte
                              (0xC2 <= bytes[0] && bytes[0] <= 0xDF) &&
                              (0x80 <= bytes[1] && bytes[1] <= 0xBF))) {
      i += 2;
      continue;
    }

    if (length - i > 2 &&
        ((  // excluding overlongs
             bytes[0] == 0xE0 && (0xA0 <= bytes[1] && bytes[1] <= 0xBF) &&
             (0x80 <= bytes[2] && bytes[2] <= 0xBF)) ||
         (  // straight 3-byte
             ((0xE1 <= bytes[0] && bytes[0] <= 0xEC) || bytes[0] == 0xEE ||
              bytes[0] == 0xEF) &&
             (0x80 <= bytes[1] && bytes[1] <= 0xBF) &&
             (0x80 <= bytes[2] && bytes[2] <= 0xBF)) ||
         (  // excluding surrogates
             bytes[0] == 0xED && (0x80 <= bytes[1] && bytes[1] <= 0x9F) &&
             (0x80 <= bytes[2] && bytes[2] <= 0xBF)))) {
      i += 3;
      continue;
    }

    if (length - i > 3 &&
        ((  // planes 1-3
             bytes[0] == 0xF0 && (0x90 <= bytes[1] && bytes[1] <= 0xBF) &&
             (0x80 <= bytes[2] && bytes[2] <= 0xBF) &&
             (0x80 <= bytes[3] && bytes[3] <= 0xBF)) ||
         (  // planes 4-15
             (0xF1 <= bytes[0] && bytes[0] <= 0xF3) &&
             (0x80 <= bytes[1] && bytes[1] <= 0xBF) &&
             (0x80 <= bytes[2] && bytes[2] <= 0xBF) &&
             (0x80 <= bytes[3] && bytes[3] <= 0xBF)) ||
         (  // plane 16
             bytes[0] == 0xF4 && (0x80 <= bytes[1] && bytes[1] <= 0x8F) &&
             (0x80 <= bytes[2] && bytes[2] <= 0xBF) &&
             (0x80 <= bytes[3] && bytes[3] <= 0xBF)))) {
      i += 4;
      continue;
    }

    return i;
  }

  return length;
}

#endif  // !MOZ_HAS_JSRUST()
