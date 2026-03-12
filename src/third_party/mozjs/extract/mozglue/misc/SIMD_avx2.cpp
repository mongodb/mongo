/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/SIMD.h"

#include "mozilla/SSE.h"
#include "mozilla/Assertions.h"

// Restricting to x86_64 simplifies things, and we're not particularly
// worried about slightly degraded performance on 32 bit processors which
// support AVX2, as this should be quite a minority.
#if defined(MOZILLA_MAY_SUPPORT_AVX2) && defined(__x86_64__)

#  include <cstring>
#  include <immintrin.h>
#  include <stdint.h>
#  include <type_traits>

#  include "mozilla/EndianUtils.h"

namespace mozilla {

const __m256i* Cast256(uintptr_t ptr) {
  return reinterpret_cast<const __m256i*>(ptr);
}

template <typename T>
T GetAs(uintptr_t ptr) {
  return *reinterpret_cast<const T*>(ptr);
}

uintptr_t AlignDown32(uintptr_t ptr) { return ptr & ~0x1f; }

uintptr_t AlignUp32(uintptr_t ptr) { return AlignDown32(ptr + 0x1f); }

template <typename TValue>
__m128i CmpEq128(__m128i a, __m128i b) {
  static_assert(sizeof(TValue) == 1 || sizeof(TValue) == 2);
  if (sizeof(TValue) == 1) {
    return _mm_cmpeq_epi8(a, b);
  }
  return _mm_cmpeq_epi16(a, b);
}

template <typename TValue>
__m256i CmpEq256(__m256i a, __m256i b) {
  static_assert(sizeof(TValue) == 1 || sizeof(TValue) == 2 ||
                sizeof(TValue) == 4 || sizeof(TValue) == 8);
  if (sizeof(TValue) == 1) {
    return _mm256_cmpeq_epi8(a, b);
  }
  if (sizeof(TValue) == 2) {
    return _mm256_cmpeq_epi16(a, b);
  }
  if (sizeof(TValue) == 4) {
    return _mm256_cmpeq_epi32(a, b);
  }

  return _mm256_cmpeq_epi64(a, b);
}

#  if defined(__GNUC__) && !defined(__clang__)

// See the comment in SIMD.cpp over Load32BitsIntoXMM. This is just adapted
// from that workaround. Testing this, it also yields the correct instructions
// across all tested compilers.
__m128i Load64BitsIntoXMM(uintptr_t ptr) {
  int64_t tmp;
  memcpy(&tmp, reinterpret_cast<const void*>(ptr), sizeof(tmp));
  return _mm_cvtsi64_si128(tmp);
}

#  else

__m128i Load64BitsIntoXMM(uintptr_t ptr) {
  return _mm_loadu_si64(reinterpret_cast<const __m128i*>(ptr));
}

#  endif

template <typename TValue>
const TValue* Check4x8Bytes(__m128i needle, uintptr_t a, uintptr_t b,
                            uintptr_t c, uintptr_t d) {
  __m128i haystackA = Load64BitsIntoXMM(a);
  __m128i cmpA = CmpEq128<TValue>(needle, haystackA);
  __m128i haystackB = Load64BitsIntoXMM(b);
  __m128i cmpB = CmpEq128<TValue>(needle, haystackB);
  __m128i haystackC = Load64BitsIntoXMM(c);
  __m128i cmpC = CmpEq128<TValue>(needle, haystackC);
  __m128i haystackD = Load64BitsIntoXMM(d);
  __m128i cmpD = CmpEq128<TValue>(needle, haystackD);
  __m128i or_ab = _mm_or_si128(cmpA, cmpB);
  __m128i or_cd = _mm_or_si128(cmpC, cmpD);
  __m128i or_abcd = _mm_or_si128(or_ab, or_cd);
  int orMask = _mm_movemask_epi8(or_abcd);
  if (orMask & 0xff) {
    int cmpMask;
    cmpMask = _mm_movemask_epi8(cmpA);
    if (cmpMask & 0xff) {
      return reinterpret_cast<const TValue*>(a + __builtin_ctz(cmpMask));
    }
    cmpMask = _mm_movemask_epi8(cmpB);
    if (cmpMask & 0xff) {
      return reinterpret_cast<const TValue*>(b + __builtin_ctz(cmpMask));
    }
    cmpMask = _mm_movemask_epi8(cmpC);
    if (cmpMask & 0xff) {
      return reinterpret_cast<const TValue*>(c + __builtin_ctz(cmpMask));
    }
    cmpMask = _mm_movemask_epi8(cmpD);
    if (cmpMask & 0xff) {
      return reinterpret_cast<const TValue*>(d + __builtin_ctz(cmpMask));
    }
  }

  return nullptr;
}

template <typename TValue>
const TValue* Check4x32Bytes(__m256i needle, uintptr_t a, uintptr_t b,
                             uintptr_t c, uintptr_t d) {
  __m256i haystackA = _mm256_loadu_si256(Cast256(a));
  __m256i cmpA = CmpEq256<TValue>(needle, haystackA);
  __m256i haystackB = _mm256_loadu_si256(Cast256(b));
  __m256i cmpB = CmpEq256<TValue>(needle, haystackB);
  __m256i haystackC = _mm256_loadu_si256(Cast256(c));
  __m256i cmpC = CmpEq256<TValue>(needle, haystackC);
  __m256i haystackD = _mm256_loadu_si256(Cast256(d));
  __m256i cmpD = CmpEq256<TValue>(needle, haystackD);
  __m256i or_ab = _mm256_or_si256(cmpA, cmpB);
  __m256i or_cd = _mm256_or_si256(cmpC, cmpD);
  __m256i or_abcd = _mm256_or_si256(or_ab, or_cd);
  int orMask = _mm256_movemask_epi8(or_abcd);
  if (orMask) {
    int cmpMask;
    cmpMask = _mm256_movemask_epi8(cmpA);
    if (cmpMask) {
      return reinterpret_cast<const TValue*>(a + __builtin_ctz(cmpMask));
    }
    cmpMask = _mm256_movemask_epi8(cmpB);
    if (cmpMask) {
      return reinterpret_cast<const TValue*>(b + __builtin_ctz(cmpMask));
    }
    cmpMask = _mm256_movemask_epi8(cmpC);
    if (cmpMask) {
      return reinterpret_cast<const TValue*>(c + __builtin_ctz(cmpMask));
    }
    cmpMask = _mm256_movemask_epi8(cmpD);
    if (cmpMask) {
      return reinterpret_cast<const TValue*>(d + __builtin_ctz(cmpMask));
    }
  }

  return nullptr;
}

template <typename TValue>
const TValue* FindInBufferAVX2(const TValue* ptr, TValue value, size_t length) {
  static_assert(sizeof(TValue) == 1 || sizeof(TValue) == 2 ||
                sizeof(TValue) == 4 || sizeof(TValue) == 8);
  static_assert(std::is_unsigned<TValue>::value);

  // Load our needle into a 32-byte register
  __m256i needle;
  if (sizeof(TValue) == 1) {
    needle = _mm256_set1_epi8(value);
  } else if (sizeof(TValue) == 2) {
    needle = _mm256_set1_epi16(value);
  } else if (sizeof(TValue) == 4) {
    needle = _mm256_set1_epi32(value);
  } else {
    needle = _mm256_set1_epi64x(value);
  }

  size_t numBytes = length * sizeof(TValue);
  uintptr_t cur = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t end = cur + numBytes;

  if (numBytes < 8 || (sizeof(TValue) >= 4 && numBytes < 32)) {
    while (cur < end) {
      if (GetAs<TValue>(cur) == value) {
        return reinterpret_cast<const TValue*>(cur);
      }
      cur += sizeof(TValue);
    }
    return nullptr;
  }

  if constexpr (sizeof(TValue) < 4) {
    if (numBytes < 32) {
      __m128i needle_narrow;
      if (sizeof(TValue) == 1) {
        needle_narrow = _mm_set1_epi8(value);
      } else {
        needle_narrow = _mm_set1_epi16(value);
      }
      uintptr_t a = cur;
      uintptr_t b = cur + ((numBytes & 16) >> 1);
      uintptr_t c = end - 8 - ((numBytes & 16) >> 1);
      uintptr_t d = end - 8;
      return Check4x8Bytes<TValue>(needle_narrow, a, b, c, d);
    }
  }

  if (numBytes < 128) {
    // NOTE: here and below, we have some bit fiddling which could look a
    // little weird. The important thing to note though is it's just a trick
    // for getting the number 32 if numBytes is greater than or equal to 64,
    // and 0 otherwise. This lets us fully cover the range without any
    // branching for the case where numBytes is in [32,64), and [64,128). We get
    // four ranges from this - if numbytes > 64, we get:
    //   [0,32), [32,64], [end - 64), [end - 32)
    // and if numbytes < 64, we get
    //   [0,32), [0,32), [end - 32), [end - 32)
    uintptr_t a = cur;
    uintptr_t b = cur + ((numBytes & 64) >> 1);
    uintptr_t c = end - 32 - ((numBytes & 64) >> 1);
    uintptr_t d = end - 32;
    return Check4x32Bytes<TValue>(needle, a, b, c, d);
  }

  // Get the initial unaligned load out of the way. This will overlap with the
  // aligned stuff below, but the overlapped part should effectively be free
  // (relative to a mispredict from doing a byte-by-byte loop).
  __m256i haystack = _mm256_loadu_si256(Cast256(cur));
  __m256i cmp = CmpEq256<TValue>(needle, haystack);
  int cmpMask = _mm256_movemask_epi8(cmp);
  if (cmpMask) {
    return reinterpret_cast<const TValue*>(cur + __builtin_ctz(cmpMask));
  }

  // Now we're working with aligned memory. Hooray! \o/
  cur = AlignUp32(cur);

  uintptr_t tailStartPtr = AlignDown32(end - 96);
  uintptr_t tailEndPtr = end - 32;

  while (cur < tailStartPtr) {
    uintptr_t a = cur;
    uintptr_t b = cur + 32;
    uintptr_t c = cur + 64;
    uintptr_t d = cur + 96;
    const TValue* result = Check4x32Bytes<TValue>(needle, a, b, c, d);
    if (result) {
      return result;
    }
    cur += 128;
  }

  uintptr_t a = tailStartPtr;
  uintptr_t b = tailStartPtr + 32;
  uintptr_t c = tailStartPtr + 64;
  uintptr_t d = tailEndPtr;
  return Check4x32Bytes<TValue>(needle, a, b, c, d);
}

const char* SIMD::memchr8AVX2(const char* ptr, char value, size_t length) {
  const unsigned char* uptr = reinterpret_cast<const unsigned char*>(ptr);
  unsigned char uvalue = static_cast<unsigned char>(value);
  const unsigned char* uresult =
      FindInBufferAVX2<unsigned char>(uptr, uvalue, length);
  return reinterpret_cast<const char*>(uresult);
}

const char16_t* SIMD::memchr16AVX2(const char16_t* ptr, char16_t value,
                                   size_t length) {
  return FindInBufferAVX2<char16_t>(ptr, value, length);
}

const uint32_t* SIMD::memchr32AVX2(const uint32_t* ptr, uint32_t value,
                                   size_t length) {
  return FindInBufferAVX2<uint32_t>(ptr, value, length);
}

const uint64_t* SIMD::memchr64AVX2(const uint64_t* ptr, uint64_t value,
                                   size_t length) {
  return FindInBufferAVX2<uint64_t>(ptr, value, length);
}

}  // namespace mozilla

#else

namespace mozilla {

const char* SIMD::memchr8AVX2(const char* ptr, char value, size_t length) {
  MOZ_RELEASE_ASSERT(false, "AVX2 not supported in this binary.");
}

const char16_t* SIMD::memchr16AVX2(const char16_t* ptr, char16_t value,
                                   size_t length) {
  MOZ_RELEASE_ASSERT(false, "AVX2 not supported in this binary.");
}

const uint32_t* SIMD::memchr32AVX2(const uint32_t* ptr, uint32_t value,
                                   size_t length) {
  MOZ_RELEASE_ASSERT(false, "AVX2 not supported in this binary.");
}

const uint64_t* SIMD::memchr64AVX2(const uint64_t* ptr, uint64_t value,
                                   size_t length) {
  MOZ_RELEASE_ASSERT(false, "AVX2 not supported in this binary.");
}

}  // namespace mozilla

#endif
