/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_Unicode_h
#define util_Unicode_h

#include "mozilla/Casting.h"  // mozilla::AssertedCast

#include "jspubtd.h"

#include "util/UnicodeNonBMP.h"

namespace js {
namespace unicode {

extern const bool js_isidstart[];
extern const bool js_isident[];
extern const bool js_isspace[];

/*
 * This namespace contains all the knowledge required to handle Unicode
 * characters in JavaScript.
 *
 * SPACE
 *   Every character that is either in the ECMAScript class WhiteSpace
 *   (ES2016, § 11.2) or in LineTerminator (ES2016, § 11.3).
 *
 *   WhiteSpace
 *    \u0009, \u000B, \u000C, \u0020, \u00A0 and \uFEFF
 *    and every other Unicode character with the General Category "Zs".
 *    See <http://www.unicode.org/reports/tr44/#UnicodeData.txt> for more
 *    information about General Categories and the UnicodeData.txt file.
 *
 *   LineTerminator
 *    \u000A, \u000D, \u2028, \u2029
 *
 * UNICODE_ID_START
 *   These are all characters with the Unicode property «ID_Start».
 *
 * UNICODE_ID_CONTINUE_ONLY
 *   These are all characters with the Unicode property «ID_Continue» minus all
 *   characters with the Unicode property «ID_Start».
 *   And additionally <ZWNJ> and <ZWJ>. (ES2016, § 11.6)
 *
 * UNICODE_ID_CONTINUE
 *   These are all characters with the Unicode property «ID_Continue».
 *   And additionally <ZWNJ> and <ZWJ>. (ES2016, § 11.6)
 *
 *   Attention: UNICODE_ID_START is _not_ IdentifierStart, but you could build
 *   a matcher for the real IdentifierPart like this:
 *
 *   if char in ['$', '_']:
 *      return True
 *   if GetFlag(char) & UNICODE_ID_CONTINUE:
 *      return True
 *
 */

namespace CharFlag {
const uint8_t SPACE = 1 << 0;
const uint8_t UNICODE_ID_START = 1 << 1;
const uint8_t UNICODE_ID_CONTINUE_ONLY = 1 << 2;
const uint8_t UNICODE_ID_CONTINUE = UNICODE_ID_START + UNICODE_ID_CONTINUE_ONLY;
}  // namespace CharFlag

constexpr char16_t NO_BREAK_SPACE = 0x00A0;
constexpr char16_t MICRO_SIGN = 0x00B5;
constexpr char16_t LATIN_SMALL_LETTER_SHARP_S = 0x00DF;
constexpr char16_t LATIN_SMALL_LETTER_A_WITH_GRAVE = 0x00E0;
constexpr char16_t DIVISION_SIGN = 0x00F7;
constexpr char16_t LATIN_SMALL_LETTER_Y_WITH_DIAERESIS = 0x00FF;
constexpr char16_t LATIN_CAPITAL_LETTER_I_WITH_DOT_ABOVE = 0x0130;
constexpr char16_t COMBINING_DOT_ABOVE = 0x0307;
constexpr char16_t GREEK_CAPITAL_LETTER_SIGMA = 0x03A3;
constexpr char16_t GREEK_SMALL_LETTER_FINAL_SIGMA = 0x03C2;
constexpr char16_t GREEK_SMALL_LETTER_SIGMA = 0x03C3;
constexpr char16_t LINE_SEPARATOR = 0x2028;
constexpr char16_t PARA_SEPARATOR = 0x2029;
constexpr char16_t REPLACEMENT_CHARACTER = 0xFFFD;

const char16_t LeadSurrogateMin = 0xD800;
const char16_t LeadSurrogateMax = 0xDBFF;
const char16_t TrailSurrogateMin = 0xDC00;
const char16_t TrailSurrogateMax = 0xDFFF;

const char32_t UTF16Max = 0xFFFF;
const char32_t NonBMPMin = 0x10000;
const char32_t NonBMPMax = 0x10FFFF;

class CharacterInfo {
  /*
   * upperCase and lowerCase normally store the delta between two
   * letters. For example the lower case alpha (a) has the char code
   * 97, and the upper case alpha (A) has 65. So for "a" we would
   * store -32 in upperCase (97 + (-32) = 65) and 0 in lowerCase,
   * because this char is already in lower case.
   * Well, not -32 exactly, but (2**16 - 32) to induce
   * unsigned overflow with identical mathematical behavior.
   * For upper case alpha, we would store 0 in upperCase and 32 in
   * lowerCase (65 + 32 = 97).
   *
   * We use deltas to reuse information for multiple characters. For
   * example the whole lower case latin alphabet fits into one entry,
   * because it's always a UnicodeLetter and upperCase contains
   * -32.
   */
 public:
  uint16_t upperCase;
  uint16_t lowerCase;
  uint8_t flags;

  inline bool isSpace() const { return flags & CharFlag::SPACE; }

  inline bool isUnicodeIDStart() const {
    return flags & CharFlag::UNICODE_ID_START;
  }

  inline bool isUnicodeIDContinue() const {
    // Also matches <ZWNJ> and <ZWJ>!
    return flags & CharFlag::UNICODE_ID_CONTINUE;
  }
};

extern const uint8_t index1[];
extern const uint8_t index2[];
extern const CharacterInfo js_charinfo[];

constexpr size_t CharInfoShift = 6;

inline const CharacterInfo& CharInfo(char16_t code) {
  const size_t shift = CharInfoShift;
  size_t index = index1[code >> shift];
  index = index2[(index << shift) + (code & ((1 << shift) - 1))];

  return js_charinfo[index];
}

inline bool IsIdentifierStart(char16_t ch) {
  /*
   * ES2016 11.6 IdentifierStart
   *  $ (dollar sign)
   *  _ (underscore)
   *  or any character with the Unicode property «ID_Start».
   *
   * We use a lookup table for small and thus common characters for speed.
   */

  if (ch < 128) {
    return js_isidstart[ch];
  }

  return CharInfo(ch).isUnicodeIDStart();
}

inline bool IsIdentifierStartASCII(char ch) {
  MOZ_ASSERT(uint8_t(ch) < 128);
  return js_isidstart[uint8_t(ch)];
}

bool IsIdentifierStartNonBMP(char32_t codePoint);

inline bool IsIdentifierStart(char32_t codePoint) {
  if (MOZ_UNLIKELY(codePoint > UTF16Max)) {
    return IsIdentifierStartNonBMP(codePoint);
  }
  return IsIdentifierStart(char16_t(codePoint));
}

inline bool IsIdentifierPart(char16_t ch) {
  /*
   * ES2016 11.6 IdentifierPart
   *  $ (dollar sign)
   *  _ (underscore)
   *  <ZWNJ>
   *  <ZWJ>
   *  or any character with the Unicode property «ID_Continue».
   *
   * We use a lookup table for small and thus common characters for speed.
   */

  if (ch < 128) {
    return js_isident[ch];
  }

  return CharInfo(ch).isUnicodeIDContinue();
}

inline bool IsIdentifierPartASCII(char ch) {
  MOZ_ASSERT(uint8_t(ch) < 128);
  return js_isident[uint8_t(ch)];
}

bool IsIdentifierPartNonBMP(char32_t codePoint);

inline bool IsIdentifierPart(char32_t codePoint) {
  if (MOZ_UNLIKELY(codePoint > UTF16Max)) {
    return IsIdentifierPartNonBMP(codePoint);
  }
  return IsIdentifierPart(char16_t(codePoint));
}

inline bool IsUnicodeIDStart(char16_t ch) {
  return CharInfo(ch).isUnicodeIDStart();
}

bool IsUnicodeIDStartNonBMP(char32_t codePoint);

inline bool IsUnicodeIDStart(char32_t codePoint) {
  if (MOZ_UNLIKELY(codePoint > UTF16Max)) {
    return IsIdentifierStartNonBMP(codePoint);
  }
  return IsUnicodeIDStart(char16_t(codePoint));
}

// IsSpace checks if a code point is included in the merged set of WhiteSpace
// and LineTerminator specified by #sec-white-space and #sec-line-terminators.
// We combine them because nearly every calling function wants this, excepting
// only some tokenizer code that necessarily handles LineTerminator specially
// due to UTF-8/UTF-16 template specialization.
inline bool IsSpace(char16_t ch) {
  // ASCII code points are very common and must be handled quickly, so use a
  // lookup table for them.
  if (ch < 128) {
    return js_isspace[ch];
  }

  // NO-BREAK SPACE is supposed to be the most common non-ASCII WhiteSpace code
  // point, so inline its handling too.
  if (ch == NO_BREAK_SPACE) {
    return true;
  }

  return CharInfo(ch).isSpace();
}

inline bool IsSpace(JS::Latin1Char ch) {
  if (ch < 128) {
    return js_isspace[ch];
  }

  if (ch == NO_BREAK_SPACE) {
    return true;
  }

  MOZ_ASSERT(!CharInfo(ch).isSpace());
  return false;
}

inline bool IsSpace(char ch) {
  return IsSpace(static_cast<JS::Latin1Char>(ch));
}

// IsSpace(char32_t) must additionally exclude everything non-BMP.
inline bool IsSpace(char32_t ch) {
  if (ch < 128) {
    return js_isspace[ch];
  }

  if (ch == NO_BREAK_SPACE) {
    return true;
  }

  // An assertion in make_unicode.py:make_unicode_file guarantees that there are
  // no Space_Separator (Zs) code points outside the BMP.
  if (ch >= NonBMPMin) {
    return false;
  }

  return CharInfo(mozilla::AssertedCast<char16_t>(ch)).isSpace();
}

/*
 * Returns the simple upper case mapping (possibly the identity mapping; see
 * ChangesWhenUpperCasedSpecialCasing for details) of the given UTF-16 code
 * unit.
 */
inline char16_t ToUpperCase(char16_t ch) {
  if (ch < 128) {
    if (ch >= 'a' && ch <= 'z') {
      return ch - ('a' - 'A');
    }
    return ch;
  }

  const CharacterInfo& info = CharInfo(ch);

  return uint16_t(ch) + info.upperCase;
}

/*
 * Returns the simple lower case mapping (possibly the identity mapping; see
 * ChangesWhenUpperCasedSpecialCasing for details) of the given UTF-16 code
 * unit.
 */
inline char16_t ToLowerCase(char16_t ch) {
  if (ch < 128) {
    if (ch >= 'A' && ch <= 'Z') {
      return ch + ('a' - 'A');
    }
    return ch;
  }

  const CharacterInfo& info = CharInfo(ch);

  return uint16_t(ch) + info.lowerCase;
}

extern const JS::Latin1Char latin1ToLowerCaseTable[];

/*
 * Returns the simple lower case mapping (possibly the identity mapping; see
 * ChangesWhenUpperCasedSpecialCasing for details) of the given Latin-1 code
 * point.
 */
inline JS::Latin1Char ToLowerCase(JS::Latin1Char ch) {
  return latin1ToLowerCaseTable[ch];
}

/*
 * Returns the simple lower case mapping (possibly the identity mapping; see
 * ChangesWhenUpperCasedSpecialCasing for details) of the given ASCII code
 * point.
 */
inline char ToLowerCase(char ch) {
  MOZ_ASSERT(static_cast<unsigned char>(ch) < 128);
  return latin1ToLowerCaseTable[uint8_t(ch)];
}

/**
 * Returns true iff ToUpperCase(ch) != ch.
 *
 * This function isn't guaranteed to correctly handle code points for which
 * |ChangesWhenUpperCasedSpecialCasing| returns true, so it is *not* always the
 * same as the value of the Changes_When_Uppercased Unicode property value for
 * the code point.
 */
inline bool ChangesWhenUpperCased(char16_t ch) {
  if (ch < 128) {
    return ch >= 'a' && ch <= 'z';
  }
  return CharInfo(ch).upperCase != 0;
}

/**
 * Returns true iff ToUpperCase(ch) != ch.
 *
 * This function isn't guaranteed to correctly handle code points for which
 * |ChangesWhenUpperCasedSpecialCasing| returns true, so it is *not* always the
 * same as the value of the Changes_When_Uppercased Unicode property value for
 * the code point.
 */
inline bool ChangesWhenUpperCased(JS::Latin1Char ch) {
  if (MOZ_LIKELY(ch < 128)) {
    return ch >= 'a' && ch <= 'z';
  }

  // U+00B5 and U+00E0 to U+00FF, except U+00F7, have an uppercase form.
  bool hasUpper =
      ch == MICRO_SIGN || (((ch & ~0x1F) == LATIN_SMALL_LETTER_A_WITH_GRAVE) &&
                           ch != DIVISION_SIGN);
  MOZ_ASSERT(hasUpper == ChangesWhenUpperCased(char16_t(ch)));
  return hasUpper;
}

// Returns true iff ToLowerCase(ch) != ch.
inline bool ChangesWhenLowerCased(char16_t ch) {
  if (ch < 128) {
    return ch >= 'A' && ch <= 'Z';
  }
  return CharInfo(ch).lowerCase != 0;
}

// Returns true iff ToLowerCase(ch) != ch.
inline bool ChangesWhenLowerCased(JS::Latin1Char ch) {
  return latin1ToLowerCaseTable[ch] != ch;
}

#define CHECK_RANGE(FROM, TO, LEAD, TRAIL_FROM, TRAIL_TO, DIFF) \
  if (lead == LEAD && trail >= TRAIL_FROM && trail <= TRAIL_TO) return true;

inline bool ChangesWhenUpperCasedNonBMP(char16_t lead, char16_t trail) {
  FOR_EACH_NON_BMP_UPPERCASE(CHECK_RANGE)
  return false;
}

inline bool ChangesWhenLowerCasedNonBMP(char16_t lead, char16_t trail) {
  FOR_EACH_NON_BMP_LOWERCASE(CHECK_RANGE)
  return false;
}

#undef CHECK_RANGE

inline char16_t ToUpperCaseNonBMPTrail(char16_t lead, char16_t trail) {
#define CALC_TRAIL(FROM, TO, LEAD, TRAIL_FROM, TRAIL_TO, DIFF)  \
  if (lead == LEAD && trail >= TRAIL_FROM && trail <= TRAIL_TO) \
    return trail + DIFF;
  FOR_EACH_NON_BMP_UPPERCASE(CALC_TRAIL)
#undef CALL_TRAIL

  return trail;
}

inline char16_t ToLowerCaseNonBMPTrail(char16_t lead, char16_t trail) {
#define CALC_TRAIL(FROM, TO, LEAD, TRAIL_FROM, TRAIL_TO, DIFF)  \
  if (lead == LEAD && trail >= TRAIL_FROM && trail <= TRAIL_TO) \
    return trail + DIFF;
  FOR_EACH_NON_BMP_LOWERCASE(CALC_TRAIL)
#undef CALL_TRAIL

  return trail;
}

/*
 * Returns true if, independent of language/locale, the given UTF-16 code unit
 * has a special upper case mapping.
 *
 * Unicode defines two case mapping modes:
 *
 *   1. "simple case mappings" (defined in UnicodeData.txt) for one-to-one
 *      mappings that are always the same regardless of locale or context
 *      within a string (e.g. "a"→"A").
 *   2. "special case mappings" (defined in SpecialCasing.txt) for mappings
 *      that alter string length (e.g. uppercasing "ß"→"SS") or where different
 *      mappings occur depending on language/locale (e.g. uppercasing "i"→"I"
 *      usually but "i"→"İ" in Turkish) or context within the string (e.g.
 *      lowercasing "Σ" U+03A3 GREEK CAPITAL LETTER SIGMA to "ς" U+03C2 GREEK
 *      SMALL LETTER FINAL SIGMA when the sigma appears [roughly speaking] at
 *      the end of a word but "ς" U+03C3 GREEK SMALL LETTER SIGMA anywhere
 *      else).
 *
 * The ChangesWhenUpperCased*() functions defined above will return true for
 * code points that have simple case mappings, but they may not return the
 * right result for code points that have special case mappings.  To correctly
 * support full case mappings for all code points, callers must determine
 * whether this function returns true or false for the code point, then use
 * AppendUpperCaseSpecialCasing in the former case and ToUpperCase in the
 * latter.
 *
 * NOTE: All special upper case mappings are unconditional (that is, they don't
 *       depend on language/locale or context within the string) in Unicode 10.
 */
bool ChangesWhenUpperCasedSpecialCasing(char16_t ch);

/*
 * Returns the length of the upper case mapping of |ch|.
 *
 * This function asserts if |ch| doesn't have a special upper case mapping.
 */
size_t LengthUpperCaseSpecialCasing(char16_t ch);

/*
 * Appends the upper case mapping of |ch| to the given output buffer,
 * starting at the provided index.
 *
 * This function asserts if |ch| doesn't have a special upper case mapping.
 */
void AppendUpperCaseSpecialCasing(char16_t ch, char16_t* elements,
                                  size_t* index);

class FoldingInfo {
 public:
  uint16_t folding;
};

extern const uint8_t folding_index1[];
extern const uint8_t folding_index2[];
extern const FoldingInfo js_foldinfo[];

inline const FoldingInfo& CaseFoldInfo(char16_t code) {
  const size_t shift = 5;
  size_t index = folding_index1[code >> shift];
  index = folding_index2[(index << shift) + (code & ((1 << shift) - 1))];
  return js_foldinfo[index];
}

inline char16_t FoldCase(char16_t ch) {
  const FoldingInfo& info = CaseFoldInfo(ch);
  return uint16_t(ch) + info.folding;
}

inline bool IsSupplementary(char32_t codePoint) {
  return codePoint >= NonBMPMin && codePoint <= NonBMPMax;
}

inline bool IsLeadSurrogate(char32_t codePoint) {
  return codePoint >= LeadSurrogateMin && codePoint <= LeadSurrogateMax;
}

inline bool IsTrailSurrogate(char32_t codePoint) {
  return codePoint >= TrailSurrogateMin && codePoint <= TrailSurrogateMax;
}

/**
 * True iff the given value is a UTF-16 surrogate.
 *
 * This function is intended for use in contexts where 32-bit values may need
 * to be tested to see if they reside in the surrogate range, so it doesn't
 * just take char16_t.
 */
inline bool IsSurrogate(char32_t codePoint) {
  return LeadSurrogateMin <= codePoint && codePoint <= TrailSurrogateMax;
}

inline char16_t LeadSurrogate(char32_t codePoint) {
  MOZ_ASSERT(IsSupplementary(codePoint));

  return char16_t((codePoint >> 10) + (LeadSurrogateMin - (NonBMPMin >> 10)));
}

inline char16_t TrailSurrogate(char32_t codePoint) {
  MOZ_ASSERT(IsSupplementary(codePoint));

  return char16_t((codePoint & 0x3FF) | TrailSurrogateMin);
}

inline void UTF16Encode(char32_t codePoint, char16_t* lead, char16_t* trail) {
  MOZ_ASSERT(IsSupplementary(codePoint));

  *lead = LeadSurrogate(codePoint);
  *trail = TrailSurrogate(codePoint);
}

inline void UTF16Encode(char32_t codePoint, char16_t* elements,
                        unsigned* index) {
  if (!IsSupplementary(codePoint)) {
    elements[(*index)++] = char16_t(codePoint);
  } else {
    elements[(*index)++] = LeadSurrogate(codePoint);
    elements[(*index)++] = TrailSurrogate(codePoint);
  }
}

inline char32_t UTF16Decode(char16_t lead, char16_t trail) {
  MOZ_ASSERT(IsLeadSurrogate(lead));
  MOZ_ASSERT(IsTrailSurrogate(trail));

  return (lead << 10) + trail +
         (NonBMPMin - (LeadSurrogateMin << 10) - TrailSurrogateMin);
}

} /* namespace unicode */
} /* namespace js */

#endif /* util_Unicode_h */
