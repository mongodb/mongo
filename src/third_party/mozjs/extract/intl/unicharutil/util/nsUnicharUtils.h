/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsUnicharUtils_h__
#define nsUnicharUtils_h__

#include "nsString.h"

/* (0x3131u <= (u) && (u) <= 0x318eu) => Hangul Compatibility Jamo */
/* (0xac00u <= (u) && (u) <= 0xd7a3u) => Hangul Syllables          */
#define IS_CJ_CHAR(u)                                                          \
  ((0x2e80u <= (u) && (u) <= 0x312fu) || (0x3190u <= (u) && (u) <= 0xabffu) || \
   (0xf900u <= (u) && (u) <= 0xfaffu) || (0xff00u <= (u) && (u) <= 0xffefu))

#define IS_ZERO_WIDTH_SPACE(u) ((u) == 0x200B)

#define IS_ASCII(u) ((u) < 0x80)
#define IS_ASCII_UPPER(u) (('A' <= (u)) && ((u) <= 'Z'))
#define IS_ASCII_LOWER(u) (('a' <= (u)) && ((u) <= 'z'))
#define IS_ASCII_ALPHA(u) (IS_ASCII_UPPER(u) || IS_ASCII_LOWER(u))
#define IS_ASCII_SPACE(u) (' ' == (u))

void ToLowerCase(nsAString& aString);
void ToLowerCaseASCII(nsAString& aString);
void ToUpperCase(nsAString& aString);

void ToLowerCase(const nsAString& aSource, nsAString& aDest);
void ToLowerCaseASCII(const nsAString& aSource, nsAString& aDest);
void ToUpperCase(const nsAString& aSource, nsAString& aDest);

uint32_t ToLowerCase(uint32_t aChar);
uint32_t ToUpperCase(uint32_t aChar);
uint32_t ToTitleCase(uint32_t aChar);

void ToLowerCase(const char16_t* aIn, char16_t* aOut, uint32_t aLen);
void ToLowerCaseASCII(const char16_t* aIn, char16_t* aOut, uint32_t aLen);
void ToUpperCase(const char16_t* aIn, char16_t* aOut, uint32_t aLen);

char ToLowerCaseASCII(const char aChar);
char16_t ToLowerCaseASCII(const char16_t aChar);
char32_t ToLowerCaseASCII(const char32_t aChar);

char ToUpperCaseASCII(const char aChar);
char16_t ToUpperCaseASCII(const char16_t aChar);
char32_t ToUpperCaseASCII(const char32_t aChar);

inline bool IsUpperCase(uint32_t c) { return ToLowerCase(c) != c; }

inline bool IsLowerCase(uint32_t c) { return ToUpperCase(c) != c; }

#ifdef MOZILLA_INTERNAL_API

uint32_t ToFoldedCase(uint32_t aChar);
void ToFoldedCase(nsAString& aString);
void ToFoldedCase(const char16_t* aIn, char16_t* aOut, uint32_t aLen);

uint32_t ToNaked(uint32_t aChar);
void ToNaked(nsAString& aString);

int32_t nsCaseInsensitiveStringComparator(const char16_t*, const char16_t*,
                                          uint32_t, uint32_t);

int32_t nsCaseInsensitiveUTF8StringComparator(const char*, const char*,
                                              uint32_t, uint32_t);

class nsCaseInsensitiveStringArrayComparator {
 public:
  template <class A, class B>
  bool Equals(const A& a, const B& b) const {
    return a.Equals(b, nsCaseInsensitiveStringComparator);
  }
};

int32_t nsASCIICaseInsensitiveStringComparator(const char16_t*, const char16_t*,
                                               uint32_t, uint32_t);

inline bool CaseInsensitiveFindInReadable(
    const nsAString& aPattern, nsAString::const_iterator& aSearchStart,
    nsAString::const_iterator& aSearchEnd) {
  return FindInReadable(aPattern, aSearchStart, aSearchEnd,
                        nsCaseInsensitiveStringComparator);
}

inline bool CaseInsensitiveFindInReadable(const nsAString& aPattern,
                                          const nsAString& aHay) {
  nsAString::const_iterator searchBegin, searchEnd;
  return FindInReadable(aPattern, aHay.BeginReading(searchBegin),
                        aHay.EndReading(searchEnd),
                        nsCaseInsensitiveStringComparator);
}

#endif  // MOZILLA_INTERNAL_API

int32_t CaseInsensitiveCompare(const char16_t* a, const char16_t* b,
                               uint32_t len);

int32_t CaseInsensitiveCompare(const char* aLeft, const char* aRight,
                               uint32_t aLeftBytes, uint32_t aRightBytes);

/**
 * Calculates the lower-case of the codepoint of the UTF8 sequence starting at
 * aStr.  Sets aNext to the byte following the end of the sequence.
 *
 * If the sequence is invalid, or if computing the codepoint would take us off
 * the end of the string (as marked by aEnd), returns -1 and does not set
 * aNext.  Note that this function doesn't check that aStr < aEnd -- it assumes
 * you've done that already.
 */
uint32_t GetLowerUTF8Codepoint(const char* aStr, const char* aEnd,
                               const char** aNext);

/**
 * This function determines whether the UTF-8 sequence pointed to by aLeft is
 * case insensitively equal to the UTF-8 sequence pointed to by aRight (or
 * optionally, case and diacritic insensitively equal), as defined by having
 * matching (naked) lower-cased codepoints.
 *
 * aLeftEnd marks the first memory location past aLeft that is not part of
 * aLeft; aRightEnd similarly marks the end of aRight.
 *
 * The function assumes that aLeft < aLeftEnd and aRight < aRightEnd.
 *
 * The function stores the addresses of the next characters in the sequence
 * into aLeftNext and aRightNext.  It's up to the caller to make sure that the
 * returned pointers are valid -- i.e. the function may return aLeftNext >=
 * aLeftEnd or aRightNext >= aRightEnd.
 *
 * If the function encounters invalid text, it sets aErr to true and returns
 * false, possibly leaving aLeftNext and aRightNext uninitialized.  If the
 * function returns true, aErr is guaranteed to be false and both aLeftNext and
 * aRightNext are guaranteed to be initialized.
 *
 * If aMatchDiacritics is false, the comparison is neither case-sensitive nor
 * diacritic-sensitive.
 */
bool CaseInsensitiveUTF8CharsEqual(const char* aLeft, const char* aRight,
                                   const char* aLeftEnd, const char* aRightEnd,
                                   const char** aLeftNext,
                                   const char** aRightNext, bool* aErr,
                                   bool aMatchDiacritics = true);

namespace mozilla {

/**
 * Hash a UTF8 string as though it were a UTF16 string.
 *
 * The value returned is the same as if we converted the string to UTF16 and
 * then ran HashString() on the result.
 *
 * The given |length| is in bytes.
 */
uint32_t HashUTF8AsUTF16(const char* aUTF8, uint32_t aLength, bool* aErr);

bool IsSegmentBreakSkipChar(uint32_t u);

}  // namespace mozilla

#endif /* nsUnicharUtils_h__ */
