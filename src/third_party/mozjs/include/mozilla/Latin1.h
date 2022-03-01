/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Latin-1 operations (i.e. a byte is the corresponding code point).
 * (Note: this is *not* the same as the encoding of windows-1252 or
 * latin1 content on the web. In Web terms, this encoding
 * corresponds to "isomorphic decode" / "isomorphic encoding" from
 * the Infra Standard.)
 */

#ifndef mozilla_Latin1_h
#define mozilla_Latin1_h

#include <type_traits>

#include "mozilla/JsRust.h"
#include "mozilla/Span.h"
#include "mozilla/Tuple.h"

#if MOZ_HAS_JSRUST()
#  include "encoding_rs_mem.h"
#endif

namespace mozilla {

namespace detail {

// It's important for optimizations that Latin1ness checks
// and inflation/deflation function use the same short
// string limit. The limit is 16, because that's the shortest
// that inflates/deflates using SIMD.
constexpr size_t kShortStringLimitForInlinePaths = 16;

template <typename Char>
class MakeUnsignedChar {
 public:
  using Type = std::make_unsigned_t<Char>;
};

template <>
class MakeUnsignedChar<char16_t> {
 public:
  using Type = char16_t;
};

template <>
class MakeUnsignedChar<char32_t> {
 public:
  using Type = char32_t;
};

}  // namespace detail

/**
 * Returns true iff |aChar| is Latin-1 but not ASCII, i.e. in the range
 * [0x80, 0xFF].
 */
template <typename Char>
constexpr bool IsNonAsciiLatin1(Char aChar) {
  using UnsignedChar = typename detail::MakeUnsignedChar<Char>::Type;
  auto uc = static_cast<UnsignedChar>(aChar);
  return uc >= 0x80 && uc <= 0xFF;
}

#if MOZ_HAS_JSRUST()

/**
 * Returns |true| iff |aString| contains only Latin1 characters, that is,
 * characters in the range [U+0000, U+00FF].
 *
 * @param aString a potentially-invalid UTF-16 string to scan
 */
inline bool IsUtf16Latin1(mozilla::Span<const char16_t> aString) {
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
    return accu < 0x100;
  }
  return encoding_mem_is_utf16_latin1(ptr, length);
}

/**
 * Returns |true| iff |aString| is valid UTF-8 containing only Latin-1
 * characters.
 *
 * If you know that the argument is always absolutely guaranteed to be valid
 * UTF-8, use the faster UnsafeIsValidUtf8Latin1() instead.
 *
 * @param aString potentially-invalid UTF-8 string to scan
 */
inline bool IsUtf8Latin1(mozilla::Span<const char> aString) {
  return encoding_mem_is_utf8_latin1(aString.Elements(), aString.Length());
}

/**
 * Returns |true| iff |aString|, which MUST be valid UTF-8, contains only
 * Latin1 characters, that is, characters in the range [U+0000, U+00FF].
 * (If |aString| might not be valid UTF-8, use |IsUtf8Latin1| instead.)
 *
 * @param aString known-valid UTF-8 string to scan
 */
inline bool UnsafeIsValidUtf8Latin1(mozilla::Span<const char> aString) {
  return encoding_mem_is_str_latin1(aString.Elements(), aString.Length());
}

/**
 * Returns the index of first byte that starts an invalid byte
 * sequence or a non-Latin1 byte sequence in a potentially-invalid UTF-8
 * string, or the length of the string if there are neither.
 *
 * If you know that the argument is always absolutely guaranteed to be valid
 * UTF-8, use the faster UnsafeValidUtf8Lati1UpTo() instead.
 *
 * @param aString potentially-invalid UTF-8 string to scan
 */
inline size_t Utf8Latin1UpTo(mozilla::Span<const char> aString) {
  return encoding_mem_utf8_latin1_up_to(aString.Elements(), aString.Length());
}

/**
 * Returns the index of first byte that starts a non-Latin1 byte
 * sequence in a known-valid UTF-8 string, or the length of the
 * string if there are none. (If the string might not be valid
 * UTF-8, use Utf8Latin1UpTo() instead.)
 *
 * @param aString known-valid UTF-8 string to scan
 */
inline size_t UnsafeValidUtf8Lati1UpTo(mozilla::Span<const char> aString) {
  return encoding_mem_str_latin1_up_to(aString.Elements(), aString.Length());
}

/**
 * If all the code points in the input are below U+0100, converts to Latin1,
 * i.e. unsigned byte value is Unicode scalar value. If there are code points
 * above U+00FF, produces unspecified garbage in a memory-safe way. The
 * nature of the garbage must not be relied upon.
 *
 * The length of aDest must not be less than the length of aSource.
 */
inline void LossyConvertUtf16toLatin1(mozilla::Span<const char16_t> aSource,
                                      mozilla::Span<char> aDest) {
  const char16_t* srcPtr = aSource.Elements();
  size_t srcLen = aSource.Length();
  char* dstPtr = aDest.Elements();
  size_t dstLen = aDest.Length();
  // Avoid function call overhead when SIMD isn't used anyway
  // If you change the length limit here, be sure to change
  // IsUtf16Latin1 and IsAscii to match so that optimizations don't
  // fail!
  if (srcLen < mozilla::detail::kShortStringLimitForInlinePaths) {
    MOZ_ASSERT(dstLen >= srcLen);
    uint8_t* unsignedPtr = reinterpret_cast<uint8_t*>(dstPtr);
    const char16_t* end = srcPtr + srcLen;
    while (srcPtr < end) {
      *unsignedPtr = static_cast<uint8_t>(*srcPtr);
      ++srcPtr;
      ++unsignedPtr;
    }
    return;
  }
  encoding_mem_convert_utf16_to_latin1_lossy(srcPtr, srcLen, dstPtr, dstLen);
}

/**
 * If all the code points in the input are below U+0100, converts to Latin1,
 * i.e. unsigned byte value is Unicode scalar value. If there are code points
 * above U+00FF, produces unspecified garbage in a memory-safe way. The
 * nature of the garbage must not be relied upon.
 *
 * Returns the number of code units written.
 *
 * The length of aDest must not be less than the length of aSource.
 */
inline size_t LossyConvertUtf8toLatin1(mozilla::Span<const char> aSource,
                                       mozilla::Span<char> aDest) {
  return encoding_mem_convert_utf8_to_latin1_lossy(
      aSource.Elements(), aSource.Length(), aDest.Elements(), aDest.Length());
}

/**
 * Converts each byte of |aSource|, interpreted as a Unicode scalar value
 * having that unsigned value, to its UTF-8 representation in |aDest|.
 *
 * Returns the number of code units written.
 *
 * The length of aDest must be at least twice the length of aSource.
 */
inline size_t ConvertLatin1toUtf8(mozilla::Span<const char> aSource,
                                  mozilla::Span<char> aDest) {
  return encoding_mem_convert_latin1_to_utf8(
      aSource.Elements(), aSource.Length(), aDest.Elements(), aDest.Length());
}

/**
 * Converts bytes whose unsigned value is interpreted as Unicode code point
 * (i.e. U+0000 to U+00FF, inclusive) to UTF-8 with potentially insufficient
 * output space.
 *
 * Returns the number of bytes read and the number of bytes written.
 *
 * If the output isn't large enough, not all input is consumed.
 *
 * The conversion is guaranteed to be complete if the length of aDest is
 * at least the length of aSource times two.
 *
 * The output is always valid UTF-8 ending on scalar value boundary
 * even in the case of partial conversion.
 *
 * The semantics of this function match the semantics of
 * TextEncoder.encodeInto.
 * https://encoding.spec.whatwg.org/#dom-textencoder-encodeinto
 */
inline mozilla::Tuple<size_t, size_t> ConvertLatin1toUtf8Partial(
    mozilla::Span<const char> aSource, mozilla::Span<char> aDest) {
  size_t srcLen = aSource.Length();
  size_t dstLen = aDest.Length();
  encoding_mem_convert_latin1_to_utf8_partial(aSource.Elements(), &srcLen,
                                              aDest.Elements(), &dstLen);
  return mozilla::MakeTuple(srcLen, dstLen);
}

/**
 * Converts Latin-1 code points (i.e. each byte is the identical code
 * point) from |aSource| to UTF-16 code points in |aDest|.
 *
 * The length of aDest must not be less than the length of aSource.
 */
inline void ConvertLatin1toUtf16(mozilla::Span<const char> aSource,
                                 mozilla::Span<char16_t> aDest) {
  const char* srcPtr = aSource.Elements();
  size_t srcLen = aSource.Length();
  char16_t* dstPtr = aDest.Elements();
  size_t dstLen = aDest.Length();
  // Avoid function call overhead when SIMD isn't used anyway
  if (srcLen < mozilla::detail::kShortStringLimitForInlinePaths) {
    MOZ_ASSERT(dstLen >= srcLen);
    const uint8_t* unsignedPtr = reinterpret_cast<const uint8_t*>(srcPtr);
    const uint8_t* end = unsignedPtr + srcLen;
    while (unsignedPtr < end) {
      *dstPtr = *unsignedPtr;
      ++unsignedPtr;
      ++dstPtr;
    }
    return;
  }
  encoding_mem_convert_latin1_to_utf16(srcPtr, srcLen, dstPtr, dstLen);
}
#else  // The code below is implemented based on the equivalent specification in
       // `encoding_rs`.

/**
 * Returns |true| iff |aString| contains only Latin1 characters, that is,
 * characters in the range [U+0000, U+00FF].
 *
 * @param aString a potentially-invalid UTF-16 string to scan
 */
inline bool IsUtf16Latin1(mozilla::Span<const char16_t> aString) {
  size_t length = aString.Length();
  const char16_t* ptr = aString.Elements();
  for (size_t i = 0; i < length; i++) {
    if (!(ptr[i] < 0x100)) {
      return false;
    }
  }
  return true;
}

/**
 * Returns the index of first byte that starts a non-Latin1 byte
 * sequence in a known-valid UTF-8 string, or the length of the
 * string if there are none. (If the string might not be valid
 * UTF-8, use Utf8Latin1UpTo() instead.)
 *
 * @param aString known-valid UTF-8 string to scan
 */
inline size_t UnsafeValidUtf8Lati1UpTo(mozilla::Span<const char> aString) {
  size_t length = aString.Length();
  const char* ptr = aString.Elements();
  for (size_t i = 0; i < length; ++i) {
    const uint8_t value = *(ptr + i);
    if (value <= 0x7F) {
      continue;
    }
    if (value > 0xC3) {
      return i;
    } else {
      // skip the second byte of the current Latin1 character
      ++i;
    }
  }
  return length;
}

/**
 * Returns the index of first byte that starts an invalid byte sequence in a
 * potentially-invalid UTF-8 string, or the length of the string if there are
 * neither.
 *
 * @param aString potentially-invalid UTF-8 string to scan
 */
size_t Utf8ValidUpToIndex(mozilla::Span<const char> aString);

/**
 * Returns the index of first byte that starts an invalid byte
 * sequence or a non-Latin1 byte sequence in a potentially-invalid UTF-8
 * string, or the length of the string if there are neither.
 *
 * If you know that the argument is always absolutely guaranteed to be valid
 * UTF-8, use the faster UnsafeValidUtf8Lati1UpTo() instead.
 *
 * @param aString potentially-invalid UTF-8 string to scan
 */
inline size_t Utf8Latin1UpTo(mozilla::Span<const char> aString) {
  size_t utf8ValidUpToResult = Utf8ValidUpToIndex(aString);
  if (utf8ValidUpToResult != aString.Length()) {
    return utf8ValidUpToResult;
  }

  return UnsafeValidUtf8Lati1UpTo(aString);
}

/**
 * Returns |true| iff |aString|, which MUST be valid UTF-8, contains only
 * Latin1 characters, that is, characters in the range [U+0000, U+00FF].
 * (If |aString| might not be valid UTF-8, use |IsUtf8Latin1| instead.)
 *
 * @param aString known-valid UTF-8 string to scan
 */
inline bool UnsafeIsValidUtf8Latin1(mozilla::Span<const char> aString) {
  return UnsafeValidUtf8Lati1UpTo(aString) == aString.Length();
}

/**
 * Returns |true| iff |aString| is valid UTF-8 containing only Latin-1
 * characters.
 *
 * If you know that the argument is always absolutely guaranteed to be valid
 * UTF-8, use the faster UnsafeIsValidUtf8Latin1() instead.
 *
 * @param aString potentially-invalid UTF-8 string to scan
 */
inline bool IsUtf8Latin1(mozilla::Span<const char> aString) {
  return Utf8Latin1UpTo(aString) == aString.Length();
}

/**
 * If all the code points in the input are below U+0100, converts to Latin1,
 * i.e. unsigned byte value is Unicode scalar value. If there are code points
 * above U+00FF, produces unspecified garbage in a memory-safe way. The
 * nature of the garbage must not be relied upon.
 *
 * The length of aDest must not be less than the length of aSource.
 */
inline void LossyConvertUtf16toLatin1(mozilla::Span<const char16_t> aSource,
                                      mozilla::Span<char> aDest) {
  const char16_t* srcPtr = aSource.Elements();
  size_t srcLen = aSource.Length();
  char* dstPtr = aDest.Elements();
  MOZ_ASSERT(aDest.Length() >= srcLen);
  uint8_t* unsignedPtr = reinterpret_cast<uint8_t*>(dstPtr);
  const char16_t* end = srcPtr + srcLen;
  while (srcPtr < end) {
    *unsignedPtr = static_cast<uint8_t>(*srcPtr);
    ++srcPtr;
    ++unsignedPtr;
  }
}

template <typename Iterator>
inline size_t _GetIteratorLength(Iterator p) {
  unsigned char c = static_cast<unsigned char>(*p);
  if (c < 0x80)
    return 1;
  else if (!(c & 0x20))
    return 2;
  else if (!(c & 0x10))
    return 3;
  else if (!(c & 0x08))
    return 4;
  else if (!(c & 0x04))
    return 5;
  else
    return 6;
}

template <typename Iterator>
inline uint32_t _GetIteratorValue(Iterator& p, Iterator ptrEnd) {
  MOZ_ASSERT(p < ptrEnd);

  size_t utf8CharLen = _GetIteratorLength(p);

  if (utf8CharLen == 1) {
    return *(p++);
  }
  uint32_t res =
      static_cast<unsigned char>(*(p++) & (0xff >> (utf8CharLen + 1)))
      << ((utf8CharLen - 1) * 6);

  for (--utf8CharLen; utf8CharLen && p < ptrEnd; --utf8CharLen) {
    res |= (static_cast<unsigned char>(*(p++)) - 0x80)
           << ((utf8CharLen - 1) * 6);
  }

  return res;
}

/**
 * If all the code points in the input are below U+0100, converts to Latin1,
 * i.e. unsigned byte value is Unicode scalar value. If there are code points
 * above U+00FF, produces unspecified garbage in a memory-safe way. The
 * nature of the garbage must not be relied upon.
 *
 * Returns the number of code units written.
 *
 * The length of aDest must not be less than the length of aSource.
 */
inline size_t LossyConvertUtf8toLatin1(mozilla::Span<const char> aSource,
                                       mozilla::Span<char> aDest) {
  const char* srcPtr = aSource.Elements();
  size_t srcLen = aSource.Length();
  char* dstPtr = aDest.Elements();
  MOZ_ASSERT(aDest.Length() >= srcLen);
  uint8_t* unsignedPtr = reinterpret_cast<uint8_t*>(dstPtr);
  const char* end = srcPtr + srcLen;
  while (srcPtr < end) {
    *unsignedPtr = _GetIteratorValue<const char*>(srcPtr, end);
    ++unsignedPtr;
  }
  return unsignedPtr - reinterpret_cast<uint8_t*>(dstPtr);
}

/**
 * Converts bytes whose unsigned value is interpreted as Unicode code point
 * (i.e. U+0000 to U+00FF, inclusive) to UTF-8 with potentially insufficient
 * output space.
 *
 * Returns the number of bytes read and the number of bytes written.
 *
 * If the output isn't large enough, not all input is consumed.
 *
 * The conversion is guaranteed to be complete if the length of aDest is
 * at least the length of aSource times two.
 *
 * The output is always valid UTF-8 ending on scalar value boundary
 * even in the case of partial conversion.
 *
 * The semantics of this function match the semantics of
 * TextEncoder.encodeInto.
 * https://encoding.spec.whatwg.org/#dom-textencoder-encodeinto
 */
inline mozilla::Tuple<size_t, size_t> ConvertLatin1toUtf8Partial(
    mozilla::Span<const char> aSource, mozilla::Span<char> aDest) {
  const char* srcPtr = aSource.Elements();
  size_t srcLen = aSource.Length();
  char* dstPtr = aDest.Elements();
  size_t dstLen = aDest.Length();
  const uint8_t* unsignedSrcPtr = reinterpret_cast<const uint8_t*>(srcPtr);
  uint8_t* unsignedDstPtr = reinterpret_cast<uint8_t*>(dstPtr);
  const uint8_t* srcEnd = unsignedSrcPtr + srcLen;
  const uint8_t* dstEnd = unsignedDstPtr + dstLen;
  while (unsignedSrcPtr < srcEnd && unsignedDstPtr < dstEnd) {
    if (*unsignedSrcPtr <= 127) {
      *(unsignedDstPtr++) = *(unsignedSrcPtr++);
    } else if (unsignedDstPtr + 1 < dstEnd) {
      uint8_t nonAscii = *(unsignedSrcPtr++);
      *(unsignedDstPtr++) = (nonAscii >> 6) | 0xC0;
      *(unsignedDstPtr++) = (nonAscii & 0x3F) | 0x80;
    } else {
      break;
    }
  }
  return mozilla::MakeTuple(
      static_cast<size_t>(reinterpret_cast<const char*>(unsignedSrcPtr) -
                          srcPtr),
      static_cast<size_t>(reinterpret_cast<const char*>(unsignedDstPtr) -
                          dstPtr));
}

/**
 * Converts each byte of |aSource|, interpreted as a Unicode scalar value
 * having that unsigned value, to its UTF-8 representation in |aDest|.
 *
 * Returns the number of code units written.
 *
 * The length of aDest must be at least twice the length of aSource.
 */
inline size_t ConvertLatin1toUtf8(mozilla::Span<const char> aSource,
                                  mozilla::Span<char> aDest) {
  MOZ_ASSERT(aDest.Length() >= aSource.Length() * 2);
  size_t read;
  size_t written;
  Tie(read, written) = ConvertLatin1toUtf8Partial(aSource, aDest);
  MOZ_ASSERT(read == aSource.Length());
  return written;
}

/**
 * Converts Latin-1 code points (i.e. each byte is the identical code
 * point) from |aSource| to UTF-16 code points in |aDest|.
 *
 * The length of aDest must not be less than the length of aSource.
 */
inline void ConvertLatin1toUtf16(mozilla::Span<const char> aSource,
                                 mozilla::Span<char16_t> aDest) {
  const char* srcPtr = aSource.Elements();
  size_t srcLen = aSource.Length();
  char16_t* dstPtr = aDest.Elements();
  // Avoid function call overhead when SIMD isn't used anyway
  MOZ_ASSERT(aDest.Length() >= srcLen);
  const uint8_t* unsignedPtr = reinterpret_cast<const uint8_t*>(srcPtr);
  const uint8_t* end = unsignedPtr + srcLen;
  while (unsignedPtr < end) {
    *dstPtr = *unsignedPtr;
    ++unsignedPtr;
    ++dstPtr;
  }
}
#endif  // MOZ_HAS_JSRUST()

};  // namespace mozilla

#endif  // mozilla_Latin1_h
