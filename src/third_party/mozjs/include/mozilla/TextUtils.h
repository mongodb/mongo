/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Character/text operations. */

#ifndef mozilla_TextUtils_h
#define mozilla_TextUtils_h

#include "mozilla/Assertions.h"
#include "mozilla/Latin1.h"

#if MOZ_HAS_JSRUST()
// Can't include mozilla/Encoding.h here.
extern "C" {
// Declared as uint8_t instead of char to match declaration in another header.
size_t encoding_ascii_valid_up_to(uint8_t const* buffer, size_t buffer_len);
}
#endif

namespace mozilla {

// See Utf8.h for IsUtf8() and conversions between UTF-8 and UTF-16.
// See Latin1.h for testing UTF-16 and UTF-8 for Latin1ness and
// for conversions to and from Latin1.

// The overloads below are not templated in order to make
// implicit conversions to span work as expected for the Span
// overloads.

/** Returns true iff |aChar| is ASCII, i.e. in the range [0, 0x80). */
constexpr bool IsAscii(unsigned char aChar) { return aChar < 0x80; }

/** Returns true iff |aChar| is ASCII, i.e. in the range [0, 0x80). */
constexpr bool IsAscii(signed char aChar) {
  return IsAscii(static_cast<unsigned char>(aChar));
}

/** Returns true iff |aChar| is ASCII, i.e. in the range [0, 0x80). */
constexpr bool IsAscii(char aChar) {
  return IsAscii(static_cast<unsigned char>(aChar));
}

#if defined(__cpp_char8_t) && __cpp_char8_t >= 201811

/** Returns true iff |aChar| is ASCII, i.e. in the range [0, 0x80). */
constexpr bool IsAscii(char8_t aChar) {
  return IsAscii(static_cast<unsigned char>(aChar));
}

#endif

/** Returns true iff |aChar| is ASCII, i.e. in the range [0, 0x80). */
constexpr bool IsAscii(char16_t aChar) { return aChar < 0x80; }

/** Returns true iff |aChar| is ASCII, i.e. in the range [0, 0x80). */
constexpr bool IsAscii(char32_t aChar) { return aChar < 0x80; }

/**
 * Returns |true| iff |aString| contains only ASCII characters, that is,
 * characters in the range [0x00, 0x80).
 *
 * @param aString a 8-bit wide string to scan
 */
inline bool IsAscii(mozilla::Span<const char> aString) {
#if MOZ_HAS_JSRUST()
  size_t length = aString.Length();
  const char* ptr = aString.Elements();
  // For short strings, avoid the function call, since, the SIMD
  // code won't have a chance to kick in anyway.
  if (length < mozilla::detail::kShortStringLimitForInlinePaths) {
    const uint8_t* uptr = reinterpret_cast<const uint8_t*>(ptr);
    uint8_t accu = 0;
    for (size_t i = 0; i < length; i++) {
      accu |= uptr[i];
    }
    return accu < 0x80;
  }
  return encoding_mem_is_ascii(ptr, length);
#else
  for (char c : aString) {
    if (!IsAscii(c)) {
      return false;
    }
  }
  return true;
#endif
}

/**
 * Returns |true| iff |aString| contains only ASCII characters, that is,
 * characters in the range [0x00, 0x80).
 *
 * @param aString a 16-bit wide string to scan
 */
inline bool IsAscii(mozilla::Span<const char16_t> aString) {
#if MOZ_HAS_JSRUST()
  size_t length = aString.Length();
  const char16_t* ptr = aString.Elements();
  // For short strings, calling into Rust is a pessimization, and the SIMD
  // code won't have a chance to kick in anyway.
  // 16 is a bit larger than logically necessary for this function alone,
  // but it's important that the limit here matches the limit used in
  // LossyConvertUtf16toLatin1!
  if (length < mozilla::detail::kShortStringLimitForInlinePaths) {
    char16_t accu = 0;
    for (size_t i = 0; i < length; i++) {
      accu |= ptr[i];
    }
    return accu < 0x80;
  }
  return encoding_mem_is_basic_latin(ptr, length);
#else
  for (char16_t c : aString) {
    if (!IsAscii(c)) {
      return false;
    }
  }
  return true;
#endif
}

/**
 * Returns true iff every character in the null-terminated string pointed to by
 * |aChar| is ASCII, i.e. in the range [0, 0x80).
 */
template <typename Char>
constexpr bool IsAsciiNullTerminated(const Char* aChar) {
  while (Char c = *aChar++) {
    if (!IsAscii(c)) {
      return false;
    }
  }
  return true;
}

#if MOZ_HAS_JSRUST()
/**
 * Returns the index of the first non-ASCII byte or
 * the length of the string if there are none.
 */
inline size_t AsciiValidUpTo(mozilla::Span<const char> aString) {
  return encoding_ascii_valid_up_to(
      reinterpret_cast<const uint8_t*>(aString.Elements()), aString.Length());
}

/**
 * Returns the index of the first unpaired surrogate or
 * the length of the string if there are none.
 */
inline size_t Utf16ValidUpTo(mozilla::Span<const char16_t> aString) {
  return encoding_mem_utf16_valid_up_to(aString.Elements(), aString.Length());
}

/**
 * Replaces unpaired surrogates with U+FFFD in the argument.
 *
 * Note: If you have an nsAString, use EnsureUTF16Validity() from
 * nsReadableUtils.h instead to avoid unsharing a valid shared
 * string.
 */
inline void EnsureUtf16ValiditySpan(mozilla::Span<char16_t> aString) {
  encoding_mem_ensure_utf16_validity(aString.Elements(), aString.Length());
}

/**
 * Convert ASCII to UTF-16. In debug builds, assert that the input is
 * ASCII.
 *
 * The length of aDest must not be less than the length of aSource.
 */
inline void ConvertAsciitoUtf16(mozilla::Span<const char> aSource,
                                mozilla::Span<char16_t> aDest) {
  MOZ_ASSERT(IsAscii(aSource));
  ConvertLatin1toUtf16(aSource, aDest);
}

#else  // The code below is implemented based on the equivalent specification in
       // `encoding_rs`.

/**
 * Returns the index of the first non-ASCII byte or
 * the length of the string if there are none.
 */
inline size_t AsciiValidUpTo(mozilla::Span<const char> aString) {
  size_t length = aString.Length();
  const char* ptr = aString.Elements();
  for (size_t i = 0; i < length; i++) {
    const uint8_t value = *(ptr + i);
    if (value > 127) {
      return i;
    }
  }
  return length;
}

/**
 * Returns the index of the first unpaired surrogate or
 * the length of the string if there are none.
 */
size_t Utf16ValidUpTo(mozilla::Span<const char16_t> aString);

/**
 * Replaces unpaired surrogates with U+FFFD in the argument.
 *
 * Note: If you have an nsAString, use EnsureUTF16Validity() from
 * nsReadableUtils.h instead to avoid unsharing a valid shared
 * string.
 */
inline void EnsureUtf16ValiditySpan(mozilla::Span<char16_t> aString) {
  size_t length = aString.Length();
  char16_t* ptr = aString.Elements();
  size_t offset = 0;
  while (true) {
    offset += Utf16ValidUpTo(aString.Subspan(offset));
    if (offset == length) {
      return;
    }
    ptr[offset] = 0xFFFD;
    offset += 1;
  }
}

/**
 * Convert ASCII to UTF-16. In debug builds, assert that the input is
 * ASCII.
 *
 * The length of aDest must not be less than the length of aSource.
 */
inline void ConvertAsciitoUtf16(mozilla::Span<const char> aSource,
                                mozilla::Span<char16_t> aDest) {
  MOZ_ASSERT(IsAscii(aSource));
  ConvertLatin1toUtf16(aSource, aDest);
}
#endif  // MOZ_HAS_JSRUST

/**
 * Returns true iff |aChar| matches Ascii Whitespace.
 *
 * This function is intended to match the Infra standard
 * (https://infra.spec.whatwg.org/#ascii-whitespace)
 */
template <typename Char>
constexpr bool IsAsciiWhitespace(Char aChar) {
  using UnsignedChar = typename detail::MakeUnsignedChar<Char>::Type;
  auto uc = static_cast<UnsignedChar>(aChar);
  return uc == 0x9 || uc == 0xA || uc == 0xC || uc == 0xD || uc == 0x20;
}

/**
 * Returns true iff |aChar| matches [a-z].
 *
 * This function is basically what you thought islower was, except its behavior
 * doesn't depend on the user's current locale.
 */
template <typename Char>
constexpr bool IsAsciiLowercaseAlpha(Char aChar) {
  using UnsignedChar = typename detail::MakeUnsignedChar<Char>::Type;
  auto uc = static_cast<UnsignedChar>(aChar);
  return 'a' <= uc && uc <= 'z';
}

/**
 * Returns true iff |aChar| matches [A-Z].
 *
 * This function is basically what you thought isupper was, except its behavior
 * doesn't depend on the user's current locale.
 */
template <typename Char>
constexpr bool IsAsciiUppercaseAlpha(Char aChar) {
  using UnsignedChar = typename detail::MakeUnsignedChar<Char>::Type;
  auto uc = static_cast<UnsignedChar>(aChar);
  return 'A' <= uc && uc <= 'Z';
}

/**
 * Returns true iff |aChar| matches [a-zA-Z].
 *
 * This function is basically what you thought isalpha was, except its behavior
 * doesn't depend on the user's current locale.
 */
template <typename Char>
constexpr bool IsAsciiAlpha(Char aChar) {
  return IsAsciiLowercaseAlpha(aChar) || IsAsciiUppercaseAlpha(aChar);
}

/**
 * Returns true iff |aChar| matches [0-9].
 *
 * This function is basically what you thought isdigit was, except its behavior
 * doesn't depend on the user's current locale.
 */
template <typename Char>
constexpr bool IsAsciiDigit(Char aChar) {
  using UnsignedChar = typename detail::MakeUnsignedChar<Char>::Type;
  auto uc = static_cast<UnsignedChar>(aChar);
  return '0' <= uc && uc <= '9';
}

/**
 * Returns true iff |aChar| matches [0-9a-fA-F].
 *
 * This function is basically isxdigit, but guaranteed to be only for ASCII.
 */
template <typename Char>
constexpr bool IsAsciiHexDigit(Char aChar) {
  using UnsignedChar = typename detail::MakeUnsignedChar<Char>::Type;
  auto uc = static_cast<UnsignedChar>(aChar);
  return ('0' <= uc && uc <= '9') || ('a' <= uc && uc <= 'f') ||
         ('A' <= uc && uc <= 'F');
}

/**
 * Returns true iff |aChar| matches [a-zA-Z0-9].
 *
 * This function is basically what you thought isalnum was, except its behavior
 * doesn't depend on the user's current locale.
 */
template <typename Char>
constexpr bool IsAsciiAlphanumeric(Char aChar) {
  return IsAsciiDigit(aChar) || IsAsciiAlpha(aChar);
}

/**
 * Converts an ASCII alphanumeric digit [0-9a-zA-Z] to number as if in base-36.
 * (This function therefore works for decimal, hexadecimal, etc.).
 */
template <typename Char>
uint8_t AsciiAlphanumericToNumber(Char aChar) {
  using UnsignedChar = typename detail::MakeUnsignedChar<Char>::Type;
  auto uc = static_cast<UnsignedChar>(aChar);

  if ('0' <= uc && uc <= '9') {
    return uc - '0';
  }

  if ('A' <= uc && uc <= 'Z') {
    return uc - 'A' + 10;
  }

  // Ideally this function would be constexpr, but unfortunately gcc at least as
  // of 6.4 forbids non-constexpr function calls in unevaluated constexpr
  // function calls.  See bug 1453456.  So for now, just assert and leave the
  // entire function non-constexpr.
  MOZ_ASSERT('a' <= uc && uc <= 'z',
             "non-ASCII alphanumeric character can't be converted to number");
  return uc - 'a' + 10;
}

}  // namespace mozilla

#endif /* mozilla_TextUtils_h */
