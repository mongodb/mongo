/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsUnicharUtils.h"
#include "nsUTF8Utils.h"
#include "nsUnicodeProperties.h"
#include "mozilla/Likely.h"
#include "mozilla/HashFunctions.h"

// We map x -> x, except for upper-case letters,
// which we map to their lower-case equivalents.
static const uint8_t gASCIIToLower[128] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b,
    0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73,
    0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b,
    0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
};

// We want ToLowerCase(uint32_t) and ToLowerCaseASCII(uint32_t) to be fast
// when they're called from within the case-insensitive comparators, so we
// define inlined versions.
static MOZ_ALWAYS_INLINE uint32_t ToLowerCase_inline(uint32_t aChar) {
  if (IS_ASCII(aChar)) {
    return gASCIIToLower[aChar];
  }

  return mozilla::unicode::GetLowercase(aChar);
}

static MOZ_ALWAYS_INLINE uint32_t
ToLowerCaseASCII_inline(const uint32_t aChar) {
  if (IS_ASCII(aChar)) {
    return gASCIIToLower[aChar];
  }

  return aChar;
}

void ToLowerCase(nsAString& aString) {
  char16_t* buf = aString.BeginWriting();
  ToLowerCase(buf, buf, aString.Length());
}

void ToLowerCaseASCII(nsAString& aString) {
  char16_t* buf = aString.BeginWriting();
  ToLowerCaseASCII(buf, buf, aString.Length());
}

char ToLowerCaseASCII(char aChar) {
  if (aChar >= 'A' && aChar <= 'Z') {
    return aChar + 0x20;
  }
  return aChar;
}

char16_t ToLowerCaseASCII(char16_t aChar) {
  if (aChar >= 'A' && aChar <= 'Z') {
    return aChar + 0x20;
  }
  return aChar;
}

char32_t ToLowerCaseASCII(char32_t aChar) {
  if (aChar >= 'A' && aChar <= 'Z') {
    return aChar + 0x20;
  }
  return aChar;
}

char ToUpperCaseASCII(char aChar) {
  if (aChar >= 'a' && aChar <= 'z') {
    return aChar - 0x20;
  }
  return aChar;
}

char16_t ToUpperCaseASCII(char16_t aChar) {
  if (aChar >= 'a' && aChar <= 'z') {
    return aChar - 0x20;
  }
  return aChar;
}

char32_t ToUpperCaseASCII(char32_t aChar) {
  if (aChar >= 'a' && aChar <= 'z') {
    return aChar - 0x20;
  }
  return aChar;
}

void ToLowerCase(const nsAString& aSource, nsAString& aDest) {
  const char16_t* in = aSource.BeginReading();
  uint32_t len = aSource.Length();

  aDest.SetLength(len);
  char16_t* out = aDest.BeginWriting();

  ToLowerCase(in, out, len);
}

void ToLowerCaseASCII(const nsAString& aSource, nsAString& aDest) {
  const char16_t* in = aSource.BeginReading();
  uint32_t len = aSource.Length();

  aDest.SetLength(len);
  char16_t* out = aDest.BeginWriting();

  ToLowerCaseASCII(in, out, len);
}

uint32_t ToLowerCaseASCII(const uint32_t aChar) {
  return ToLowerCaseASCII_inline(aChar);
}

void ToUpperCase(nsAString& aString) {
  char16_t* buf = aString.BeginWriting();
  ToUpperCase(buf, buf, aString.Length());
}

void ToUpperCase(const nsAString& aSource, nsAString& aDest) {
  const char16_t* in = aSource.BeginReading();
  uint32_t len = aSource.Length();

  aDest.SetLength(len);
  char16_t* out = aDest.BeginWriting();

  ToUpperCase(in, out, len);
}

#ifdef MOZILLA_INTERNAL_API

uint32_t ToFoldedCase(uint32_t aChar) {
  if (IS_ASCII(aChar)) return gASCIIToLower[aChar];
  return mozilla::unicode::GetFoldedcase(aChar);
}

void ToFoldedCase(nsAString& aString) {
  char16_t* buf = aString.BeginWriting();
  ToFoldedCase(buf, buf, aString.Length());
}

void ToFoldedCase(const char16_t* aIn, char16_t* aOut, uint32_t aLen) {
  for (uint32_t i = 0; i < aLen; i++) {
    uint32_t ch = aIn[i];
    if (i < aLen - 1 && NS_IS_SURROGATE_PAIR(ch, aIn[i + 1])) {
      ch = mozilla::unicode::GetFoldedcase(SURROGATE_TO_UCS4(ch, aIn[i + 1]));
      NS_ASSERTION(!IS_IN_BMP(ch), "case mapping crossed BMP/SMP boundary!");
      aOut[i++] = H_SURROGATE(ch);
      aOut[i] = L_SURROGATE(ch);
      continue;
    }
    aOut[i] = ToFoldedCase(ch);
  }
}

uint32_t ToNaked(uint32_t aChar) {
  if (IS_ASCII(aChar)) {
    return aChar;
  }
  return mozilla::unicode::GetNaked(aChar);
}

void ToNaked(nsAString& aString) {
  uint32_t i = 0;
  while (i < aString.Length()) {
    uint32_t ch = aString[i];
    if (i < aString.Length() - 1 && NS_IS_SURROGATE_PAIR(ch, aString[i + 1])) {
      ch = SURROGATE_TO_UCS4(ch, aString[i + 1]);
      if (mozilla::unicode::IsCombiningDiacritic(ch)) {
        aString.Cut(i, 2);
      } else {
        ch = mozilla::unicode::GetNaked(ch);
        NS_ASSERTION(!IS_IN_BMP(ch), "stripping crossed BMP/SMP boundary!");
        aString.Replace(i++, 1, H_SURROGATE(ch));
        aString.Replace(i++, 1, L_SURROGATE(ch));
      }
      continue;
    }
    if (mozilla::unicode::IsCombiningDiacritic(ch)) {
      aString.Cut(i, 1);
    } else {
      aString.Replace(i++, 1, ToNaked(ch));
    }
  }
}

int32_t nsCaseInsensitiveStringComparator(const char16_t* lhs,
                                          const char16_t* rhs, uint32_t lLength,
                                          uint32_t rLength) {
  return (lLength == rLength)  ? CaseInsensitiveCompare(lhs, rhs, lLength)
         : (lLength > rLength) ? 1
                               : -1;
}

int32_t nsCaseInsensitiveUTF8StringComparator(const char* lhs, const char* rhs,
                                              uint32_t lLength,
                                              uint32_t rLength) {
  return CaseInsensitiveCompare(lhs, rhs, lLength, rLength);
}

int32_t nsASCIICaseInsensitiveStringComparator(const char16_t* lhs,
                                               const char16_t* rhs,
                                               uint32_t lLength,
                                               uint32_t rLength) {
  if (lLength != rLength) {
    if (lLength > rLength) return 1;
    return -1;
  }

  while (rLength) {
    // we don't care about surrogates here, because we're only
    // lowercasing the ASCII range
    char16_t l = *lhs++;
    char16_t r = *rhs++;
    if (l != r) {
      l = ToLowerCaseASCII_inline(l);
      r = ToLowerCaseASCII_inline(r);

      if (l > r)
        return 1;
      else if (r > l)
        return -1;
    }
    rLength--;
  }

  return 0;
}

#endif  // MOZILLA_INTERNAL_API

uint32_t ToLowerCase(uint32_t aChar) { return ToLowerCase_inline(aChar); }

void ToLowerCase(const char16_t* aIn, char16_t* aOut, uint32_t aLen) {
  for (uint32_t i = 0; i < aLen; i++) {
    uint32_t ch = aIn[i];
    if (i < aLen - 1 && NS_IS_SURROGATE_PAIR(ch, aIn[i + 1])) {
      ch = mozilla::unicode::GetLowercase(SURROGATE_TO_UCS4(ch, aIn[i + 1]));
      NS_ASSERTION(!IS_IN_BMP(ch), "case mapping crossed BMP/SMP boundary!");
      aOut[i++] = H_SURROGATE(ch);
      aOut[i] = L_SURROGATE(ch);
      continue;
    }
    aOut[i] = ToLowerCase(ch);
  }
}

void ToLowerCaseASCII(const char16_t* aIn, char16_t* aOut, uint32_t aLen) {
  for (uint32_t i = 0; i < aLen; i++) {
    char16_t ch = aIn[i];
    aOut[i] = IS_ASCII_UPPER(ch) ? (ch + 0x20) : ch;
  }
}

uint32_t ToUpperCase(uint32_t aChar) {
  if (IS_ASCII(aChar)) {
    if (IS_ASCII_LOWER(aChar)) {
      return aChar - 0x20;
    }
    return aChar;
  }

  return mozilla::unicode::GetUppercase(aChar);
}

void ToUpperCase(const char16_t* aIn, char16_t* aOut, uint32_t aLen) {
  for (uint32_t i = 0; i < aLen; i++) {
    uint32_t ch = aIn[i];
    if (i < aLen - 1 && NS_IS_SURROGATE_PAIR(ch, aIn[i + 1])) {
      ch = mozilla::unicode::GetUppercase(SURROGATE_TO_UCS4(ch, aIn[i + 1]));
      NS_ASSERTION(!IS_IN_BMP(ch), "case mapping crossed BMP/SMP boundary!");
      aOut[i++] = H_SURROGATE(ch);
      aOut[i] = L_SURROGATE(ch);
      continue;
    }
    aOut[i] = ToUpperCase(ch);
  }
}

uint32_t ToTitleCase(uint32_t aChar) {
  if (IS_ASCII(aChar)) {
    return ToUpperCase(aChar);
  }

  return mozilla::unicode::GetTitlecaseForLower(aChar);
}

int32_t CaseInsensitiveCompare(const char16_t* a, const char16_t* b,
                               uint32_t len) {
  NS_ASSERTION(a && b, "Do not pass in invalid pointers!");

  if (len) {
    do {
      uint32_t c1 = *a++;
      uint32_t c2 = *b++;

      // Unfortunately, we need to check for surrogates BEFORE we check
      // for equality, because we could have identical high surrogates
      // but non-identical characters, so we can't just skip them

      // If c1 isn't a surrogate, we don't bother to check c2;
      // in the case where it _is_ a surrogate, we're definitely going to get
      // a mismatch, and don't need to interpret and lowercase it

      if (len > 1 && NS_IS_SURROGATE_PAIR(c1, *a)) {
        c1 = SURROGATE_TO_UCS4(c1, *a++);
        if (NS_IS_SURROGATE_PAIR(c2, *b)) {
          c2 = SURROGATE_TO_UCS4(c2, *b++);
        }
        // If c2 wasn't a surrogate, decrementing len means we'd stop
        // short of the end of string b, but that doesn't actually matter
        // because we're going to find a mismatch and return early
        --len;
      }

      if (c1 != c2) {
        c1 = ToLowerCase_inline(c1);
        c2 = ToLowerCase_inline(c2);
        if (c1 != c2) {
          if (c1 < c2) {
            return -1;
          }
          return 1;
        }
      }
    } while (--len != 0);
  }
  return 0;
}

// Inlined definition of GetLowerUTF8Codepoint, which we use because we want
// to be fast when called from the case-insensitive comparators.
static MOZ_ALWAYS_INLINE uint32_t GetLowerUTF8Codepoint_inline(
    const char* aStr, const char* aEnd, const char** aNext) {
  // Convert to unsigned char so that stuffing chars into PRUint32s doesn't
  // sign extend.
  const unsigned char* str = (unsigned char*)aStr;

  if (UTF8traits::isASCII(str[0])) {
    // It's ASCII; just convert to lower-case and return it.
    *aNext = aStr + 1;
    return gASCIIToLower[*str];
  }
  if (UTF8traits::is2byte(str[0]) && MOZ_LIKELY(aStr + 1 < aEnd)) {
    // It's a two-byte sequence, so it looks like
    //  110XXXXX 10XXXXXX.
    // This is definitely in the BMP, so we can store straightaway into a
    // uint16_t.

    uint16_t c;
    c = (str[0] & 0x1F) << 6;
    c += (str[1] & 0x3F);

    // we don't go through ToLowerCase here, because we know this isn't
    // an ASCII character so the ASCII fast-path there is useless
    c = mozilla::unicode::GetLowercase(c);

    *aNext = aStr + 2;
    return c;
  }
  if (UTF8traits::is3byte(str[0]) && MOZ_LIKELY(aStr + 2 < aEnd)) {
    // It's a three-byte sequence, so it looks like
    //  1110XXXX 10XXXXXX 10XXXXXX.
    // This will just barely fit into 16-bits, so store into a uint16_t.

    uint16_t c;
    c = (str[0] & 0x0F) << 12;
    c += (str[1] & 0x3F) << 6;
    c += (str[2] & 0x3F);

    c = mozilla::unicode::GetLowercase(c);

    *aNext = aStr + 3;
    return c;
  }
  if (UTF8traits::is4byte(str[0]) && MOZ_LIKELY(aStr + 3 < aEnd)) {
    // It's a four-byte sequence, so it looks like
    //   11110XXX 10XXXXXX 10XXXXXX 10XXXXXX.

    uint32_t c;
    c = (str[0] & 0x07) << 18;
    c += (str[1] & 0x3F) << 12;
    c += (str[2] & 0x3F) << 6;
    c += (str[3] & 0x3F);

    c = mozilla::unicode::GetLowercase(c);

    *aNext = aStr + 4;
    return c;
  }

  // Hm, we don't understand this sequence.
  return -1;
}

uint32_t GetLowerUTF8Codepoint(const char* aStr, const char* aEnd,
                               const char** aNext) {
  return GetLowerUTF8Codepoint_inline(aStr, aEnd, aNext);
}

int32_t CaseInsensitiveCompare(const char* aLeft, const char* aRight,
                               uint32_t aLeftBytes, uint32_t aRightBytes) {
  const char* leftEnd = aLeft + aLeftBytes;
  const char* rightEnd = aRight + aRightBytes;

  while (aLeft < leftEnd && aRight < rightEnd) {
    uint32_t leftChar = GetLowerUTF8Codepoint_inline(aLeft, leftEnd, &aLeft);
    if (MOZ_UNLIKELY(leftChar == uint32_t(-1))) return -1;

    uint32_t rightChar =
        GetLowerUTF8Codepoint_inline(aRight, rightEnd, &aRight);
    if (MOZ_UNLIKELY(rightChar == uint32_t(-1))) return -1;

    // Now leftChar and rightChar are lower-case, so we can compare them.
    if (leftChar != rightChar) {
      if (leftChar > rightChar) return 1;
      return -1;
    }
  }

  // Make sure that if one string is longer than the other we return the
  // correct result.
  if (aLeft < leftEnd) return 1;
  if (aRight < rightEnd) return -1;

  return 0;
}

static MOZ_ALWAYS_INLINE uint32_t
GetLowerUTF8Codepoint_inline(const char* aStr, const char* aEnd,
                             const char** aNext, bool aMatchDiacritics) {
  uint32_t c;
  for (;;) {
    c = GetLowerUTF8Codepoint_inline(aStr, aEnd, aNext);
    if (aMatchDiacritics) {
      break;
    }
    if (!mozilla::unicode::IsCombiningDiacritic(c)) {
      break;
    }
    aStr = *aNext;
  }
  return c;
}

bool CaseInsensitiveUTF8CharsEqual(const char* aLeft, const char* aRight,
                                   const char* aLeftEnd, const char* aRightEnd,
                                   const char** aLeftNext,
                                   const char** aRightNext, bool* aErr,
                                   bool aMatchDiacritics) {
  NS_ASSERTION(aLeftNext, "Out pointer shouldn't be null.");
  NS_ASSERTION(aRightNext, "Out pointer shouldn't be null.");
  NS_ASSERTION(aErr, "Out pointer shouldn't be null.");
  NS_ASSERTION(aLeft < aLeftEnd, "aLeft must be less than aLeftEnd.");
  NS_ASSERTION(aRight < aRightEnd, "aRight must be less than aRightEnd.");

  uint32_t leftChar = GetLowerUTF8Codepoint_inline(aLeft, aLeftEnd, aLeftNext,
                                                   aMatchDiacritics);
  if (MOZ_UNLIKELY(leftChar == uint32_t(-1))) {
    *aErr = true;
    return false;
  }

  uint32_t rightChar = GetLowerUTF8Codepoint_inline(
      aRight, aRightEnd, aRightNext, aMatchDiacritics);
  if (MOZ_UNLIKELY(rightChar == uint32_t(-1))) {
    *aErr = true;
    return false;
  }

  // Can't have an error past this point.
  *aErr = false;

  if (!aMatchDiacritics) {
    leftChar = ToNaked(leftChar);
    rightChar = ToNaked(rightChar);
  }

  return leftChar == rightChar;
}

namespace mozilla {

uint32_t HashUTF8AsUTF16(const char* aUTF8, uint32_t aLength, bool* aErr) {
  uint32_t hash = 0;
  const char* s = aUTF8;
  const char* end = aUTF8 + aLength;

  *aErr = false;

  while (s < end) {
    uint32_t ucs4 = UTF8CharEnumerator::NextChar(&s, end, aErr);
    if (*aErr) {
      return 0;
    }

    if (ucs4 < PLANE1_BASE) {
      hash = AddToHash(hash, ucs4);
    } else {
      hash = AddToHash(hash, H_SURROGATE(ucs4), L_SURROGATE(ucs4));
    }
  }

  return hash;
}

bool IsSegmentBreakSkipChar(uint32_t u) {
  return unicode::IsEastAsianWidthFHWexcludingEmoji(u) &&
         unicode::GetScriptCode(u) != unicode::Script::HANGUL;
}

}  // namespace mozilla
