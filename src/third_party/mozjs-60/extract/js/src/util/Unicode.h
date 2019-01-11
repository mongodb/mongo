/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_Unicode_h
#define util_Unicode_h

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
}

constexpr char16_t NO_BREAK_SPACE = 0x00A0;
constexpr char16_t MICRO_SIGN = 0x00B5;
constexpr char16_t LATIN_CAPITAL_LETTER_A_WITH_GRAVE = 0x00C0;
constexpr char16_t MULTIPLICATION_SIGN = 0x00D7;
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
constexpr char16_t BYTE_ORDER_MARK2 = 0xFFFE;

const char16_t LeadSurrogateMin = 0xD800;
const char16_t LeadSurrogateMax = 0xDBFF;
const char16_t TrailSurrogateMin = 0xDC00;
const char16_t TrailSurrogateMax = 0xDFFF;

const uint32_t UTF16Max = 0xFFFF;
const uint32_t NonBMPMin = 0x10000;
const uint32_t NonBMPMax = 0x10FFFF;

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

    inline bool isSpace() const {
        return flags & CharFlag::SPACE;
    }

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

inline const CharacterInfo&
CharInfo(char16_t code)
{
    const size_t shift = 6;
    size_t index = index1[code >> shift];
    index = index2[(index << shift) + (code & ((1 << shift) - 1))];

    return js_charinfo[index];
}

inline bool
IsIdentifierStart(char16_t ch)
{
    /*
     * ES2016 11.6 IdentifierStart
     *  $ (dollar sign)
     *  _ (underscore)
     *  or any character with the Unicode property «ID_Start».
     *
     * We use a lookup table for small and thus common characters for speed.
     */

    if (ch < 128)
        return js_isidstart[ch];

    return CharInfo(ch).isUnicodeIDStart();
}

bool
IsIdentifierStartNonBMP(uint32_t codePoint);

inline bool
IsIdentifierStart(uint32_t codePoint)
{
    if (MOZ_UNLIKELY(codePoint > UTF16Max))
        return IsIdentifierStartNonBMP(codePoint);
    return IsIdentifierStart(char16_t(codePoint));
}

inline bool
IsIdentifierPart(char16_t ch)
{
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

    if (ch < 128)
        return js_isident[ch];

    return CharInfo(ch).isUnicodeIDContinue();
}

bool
IsIdentifierPartNonBMP(uint32_t codePoint);

inline bool
IsIdentifierPart(uint32_t codePoint)
{
    if (MOZ_UNLIKELY(codePoint > UTF16Max))
        return IsIdentifierPartNonBMP(codePoint);
    return IsIdentifierPart(char16_t(codePoint));
}

inline bool
IsUnicodeIDStart(char16_t ch)
{
    return CharInfo(ch).isUnicodeIDStart();
}

bool
IsUnicodeIDStartNonBMP(uint32_t codePoint);

inline bool
IsUnicodeIDStart(uint32_t codePoint)
{
    if (MOZ_UNLIKELY(codePoint > UTF16Max))
        return IsIdentifierStartNonBMP(codePoint);
    return IsUnicodeIDStart(char16_t(codePoint));
}

inline bool
IsSpace(char16_t ch)
{
    /*
     * IsSpace checks if some character is included in the merged set
     * of WhiteSpace and LineTerminator, specified by ES2016 11.2 and 11.3.
     * We combined them, because in practice nearly every
     * calling function wants this, except some code in the tokenizer.
     *
     * We use a lookup table for ASCII-7 characters, because they are
     * very common and must be handled quickly in the tokenizer.
     * NO-BREAK SPACE is supposed to be the most common character not in
     * this range, so we inline this case, too.
     */

    if (ch < 128)
        return js_isspace[ch];

    if (ch == NO_BREAK_SPACE)
        return true;

    return CharInfo(ch).isSpace();
}

inline bool
IsSpaceOrBOM2(char16_t ch)
{
    if (ch < 128)
        return js_isspace[ch];

    /* We accept BOM2 (0xFFFE) for compatibility reasons in the parser. */
    if (ch == NO_BREAK_SPACE || ch == BYTE_ORDER_MARK2)
        return true;

    return CharInfo(ch).isSpace();
}

/*
 * Returns the simple upper case mapping (see CanUpperCaseSpecialCasing for
 * details) of the given UTF-16 code unit.
 */
inline char16_t
ToUpperCase(char16_t ch)
{
    if (ch < 128) {
        if (ch >= 'a' && ch <= 'z')
            return ch - ('a' - 'A');
        return ch;
    }

    const CharacterInfo& info = CharInfo(ch);

    return uint16_t(ch) + info.upperCase;
}

/*
 * Returns the simple lower case mapping (see CanUpperCaseSpecialCasing for
 * details) of the given UTF-16 code unit.
 */
inline char16_t
ToLowerCase(char16_t ch)
{
    if (ch < 128) {
        if (ch >= 'A' && ch <= 'Z')
            return ch + ('a' - 'A');
        return ch;
    }

    const CharacterInfo& info = CharInfo(ch);

    return uint16_t(ch) + info.lowerCase;
}

// Returns true iff ToUpperCase(ch) != ch.
inline bool
CanUpperCase(char16_t ch)
{
    if (ch < 128)
        return ch >= 'a' && ch <= 'z';
    return CharInfo(ch).upperCase != 0;
}

// Returns true iff ToUpperCase(ch) != ch.
inline bool
CanUpperCase(JS::Latin1Char ch)
{
    if (MOZ_LIKELY(ch < 128))
        return ch >= 'a' && ch <= 'z';

    // U+00B5 and U+00E0 to U+00FF, except U+00F7, have an uppercase form.
    bool canUpper = ch == MICRO_SIGN ||
                    (((ch & ~0x1F) == LATIN_SMALL_LETTER_A_WITH_GRAVE) && ch != DIVISION_SIGN);
    MOZ_ASSERT(canUpper == CanUpperCase(char16_t(ch)));
    return canUpper;
}

// Returns true iff ToLowerCase(ch) != ch.
inline bool
CanLowerCase(char16_t ch)
{
    if (ch < 128)
        return ch >= 'A' && ch <= 'Z';
    return CharInfo(ch).lowerCase != 0;
}

// Returns true iff ToLowerCase(ch) != ch.
inline bool
CanLowerCase(JS::Latin1Char ch)
{
    if (MOZ_LIKELY(ch < 128))
        return ch >= 'A' && ch <= 'Z';

    // U+00C0 to U+00DE, except U+00D7, have a lowercase form.
    bool canLower = ((ch & ~0x1F) == LATIN_CAPITAL_LETTER_A_WITH_GRAVE) &&
                    ((ch & MULTIPLICATION_SIGN) != MULTIPLICATION_SIGN);
    MOZ_ASSERT(canLower == CanLowerCase(char16_t(ch)));
    return canLower;
}

#define CHECK_RANGE(FROM, TO, LEAD, TRAIL_FROM, TRAIL_TO, DIFF) \
    if (lead == LEAD && trail >= TRAIL_FROM && trail <= TRAIL_TO) \
        return true;

inline bool
CanUpperCaseNonBMP(char16_t lead, char16_t trail)
{
    FOR_EACH_NON_BMP_UPPERCASE(CHECK_RANGE)
    return false;
}

inline bool
CanLowerCaseNonBMP(char16_t lead, char16_t trail)
{
    FOR_EACH_NON_BMP_LOWERCASE(CHECK_RANGE)
    return false;
}

#undef CHECK_RANGE

inline char16_t
ToUpperCaseNonBMPTrail(char16_t lead, char16_t trail)
{
#define CALC_TRAIL(FROM, TO, LEAD, TRAIL_FROM, TRAIL_TO, DIFF) \
    if (lead == LEAD && trail >= TRAIL_FROM && trail <= TRAIL_TO) \
        return trail + DIFF;
    FOR_EACH_NON_BMP_UPPERCASE(CALC_TRAIL)
#undef CALL_TRAIL

    return trail;
}

inline char16_t
ToLowerCaseNonBMPTrail(char16_t lead, char16_t trail)
{
#define CALC_TRAIL(FROM, TO, LEAD, TRAIL_FROM, TRAIL_TO, DIFF) \
    if (lead == LEAD && trail >= TRAIL_FROM && trail <= TRAIL_TO) \
        return trail + DIFF;
    FOR_EACH_NON_BMP_LOWERCASE(CALC_TRAIL)
#undef CALL_TRAIL

    return trail;
}

/*
 * Returns true if the given UTF-16 code unit has a language-independent,
 * unconditional or conditional special upper case mapping.
 *
 * Unicode defines two case mapping modes:
 * 1. "simple case mappings" for one-to-one mappings which are independent of
 *    context and language (defined in UnicodeData.txt).
 * 2. "special case mappings" for mappings which can increase or decrease the
 *    string length; or are dependent on context or locale (defined in
 *    SpecialCasing.txt).
 *
 * The CanUpperCase() method defined above only supports simple case mappings.
 * In order to support the full case mappings of all Unicode characters,
 * callers need to check this method in addition to CanUpperCase().
 *
 * NOTE: All special upper case mappings are unconditional in Unicode 9.
 */
bool
CanUpperCaseSpecialCasing(char16_t ch);

/*
 * Returns the length of the upper case mapping of |ch|.
 *
 * This function asserts if |ch| doesn't have a special upper case mapping.
 */
size_t
LengthUpperCaseSpecialCasing(char16_t ch);

/*
 * Appends the upper case mapping of |ch| to the given output buffer,
 * starting at the provided index.
 *
 * This function asserts if |ch| doesn't have a special upper case mapping.
 */
void
AppendUpperCaseSpecialCasing(char16_t ch, char16_t* elements, size_t* index);

/*
 * For a codepoint C, CodepointsWithSameUpperCaseInfo stores three offsets
 * from C to up to three codepoints with same uppercase (no codepoint in
 * UnicodeData.txt has more than three such codepoints).
 *
 * To illustrate, consider the codepoint U+0399 GREEK CAPITAL LETTER IOTA, the
 * uppercased form of these three codepoints:
 *
 *   U+03B9 GREEK SMALL LETTER IOTA
 *   U+1FBE GREEK PROSGEGRAMMENI
 *   U+0345 COMBINING GREEK YPOGEGRAMMENI
 *
 * For the CodepointsWithSameUpperCaseInfo corresponding to this codepoint,
 * delta{1,2,3} are 16-bit modular deltas from 0x0399 to each respective
 * codepoint:
 *   uint16_t(0x03B9 - 0x0399),
 *   uint16_t(0x1FBE - 0x0399),
 *   uint16_t(0x0345 - 0x0399)
 * in an unimportant order.
 *
 * If there are fewer than three other codepoints, some fields are zero.
 * Consider the codepoint U+03B9 above, the other two codepoints U+1FBE and
 * U+0345 have same uppercase (U+0399 is not).  For the
 * CodepointsWithSameUpperCaseInfo corresponding to this codepoint,
 * delta{1,2,3} are:
 *   uint16_t(0x1FBE - 0x03B9),
 *   uint16_t(0x0345 - 0x03B9),
 *   uint16_t(0)
 * in an unimportant order.
 *
 * Because multiple codepoints map to a single CodepointsWithSameUpperCaseInfo,
 * a CodepointsWithSameUpperCaseInfo and its delta{1,2,3} have no meaning
 * standing alone: they have meaning only with respect to a codepoint mapping
 * to that CodepointsWithSameUpperCaseInfo.
 */
class CodepointsWithSameUpperCaseInfo
{
  public:
    uint16_t delta1;
    uint16_t delta2;
    uint16_t delta3;
};

extern const uint8_t codepoints_with_same_upper_index1[];
extern const uint8_t codepoints_with_same_upper_index2[];
extern const CodepointsWithSameUpperCaseInfo js_codepoints_with_same_upper_info[];

class CodepointsWithSameUpperCase
{
    const CodepointsWithSameUpperCaseInfo& info_;
    const char16_t code_;

    static const CodepointsWithSameUpperCaseInfo& computeInfo(char16_t code) {
        const size_t shift = 6;
        size_t index = codepoints_with_same_upper_index1[code >> shift];
        index = codepoints_with_same_upper_index2[(index << shift) + (code & ((1 << shift) - 1))];
        return js_codepoints_with_same_upper_info[index];
    }

  public:
    explicit CodepointsWithSameUpperCase(char16_t code)
      : info_(computeInfo(code)),
        code_(code)
    {}

    char16_t other1() const { return uint16_t(code_) + info_.delta1; }
    char16_t other2() const { return uint16_t(code_) + info_.delta2; }
    char16_t other3() const { return uint16_t(code_) + info_.delta3; }
};

class FoldingInfo {
  public:
    uint16_t folding;
    uint16_t reverse1;
    uint16_t reverse2;
    uint16_t reverse3;
};

extern const uint8_t folding_index1[];
extern const uint8_t folding_index2[];
extern const FoldingInfo js_foldinfo[];

inline const FoldingInfo&
CaseFoldInfo(char16_t code)
{
    const size_t shift = 6;
    size_t index = folding_index1[code >> shift];
    index = folding_index2[(index << shift) + (code & ((1 << shift) - 1))];
    return js_foldinfo[index];
}

inline char16_t
FoldCase(char16_t ch)
{
    const FoldingInfo& info = CaseFoldInfo(ch);
    return uint16_t(ch) + info.folding;
}

inline char16_t
ReverseFoldCase1(char16_t ch)
{
    const FoldingInfo& info = CaseFoldInfo(ch);
    return uint16_t(ch) + info.reverse1;
}

inline char16_t
ReverseFoldCase2(char16_t ch)
{
    const FoldingInfo& info = CaseFoldInfo(ch);
    return uint16_t(ch) + info.reverse2;
}

inline char16_t
ReverseFoldCase3(char16_t ch)
{
    const FoldingInfo& info = CaseFoldInfo(ch);
    return uint16_t(ch) + info.reverse3;
}

inline bool
IsSupplementary(uint32_t codePoint)
{
    return codePoint >= NonBMPMin && codePoint <= NonBMPMax;
}

inline bool
IsLeadSurrogate(uint32_t codePoint)
{
    return codePoint >= LeadSurrogateMin && codePoint <= LeadSurrogateMax;
}

inline bool
IsTrailSurrogate(uint32_t codePoint)
{
    return codePoint >= TrailSurrogateMin && codePoint <= TrailSurrogateMax;
}

inline char16_t
LeadSurrogate(uint32_t codePoint)
{
    MOZ_ASSERT(IsSupplementary(codePoint));

    return char16_t((codePoint >> 10) + (LeadSurrogateMin - (NonBMPMin >> 10)));
}

inline char16_t
TrailSurrogate(uint32_t codePoint)
{
    MOZ_ASSERT(IsSupplementary(codePoint));

    return char16_t((codePoint & 0x3FF) | TrailSurrogateMin);
}

inline void
UTF16Encode(uint32_t codePoint, char16_t* lead, char16_t* trail)
{
    MOZ_ASSERT(IsSupplementary(codePoint));

    *lead = LeadSurrogate(codePoint);
    *trail = TrailSurrogate(codePoint);
}

inline void
UTF16Encode(uint32_t codePoint, char16_t* elements, unsigned* index)
{
    if (!IsSupplementary(codePoint)) {
        elements[(*index)++] = char16_t(codePoint);
    } else {
        elements[(*index)++] = LeadSurrogate(codePoint);
        elements[(*index)++] = TrailSurrogate(codePoint);
    }
}

inline uint32_t
UTF16Decode(char16_t lead, char16_t trail)
{
    MOZ_ASSERT(IsLeadSurrogate(lead));
    MOZ_ASSERT(IsTrailSurrogate(trail));

    return (lead << 10) + trail + (NonBMPMin - (LeadSurrogateMin << 10) - TrailSurrogateMin);
}

} /* namespace unicode */
} /* namespace js */

#endif /* util_Unicode_h */
