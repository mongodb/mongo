/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Unicode_h
#define vm_Unicode_h

#include "jspubtd.h"

extern const bool js_isidstart[];
extern const bool js_isident[];
extern const bool js_isspace[];

namespace js {
namespace unicode {

/*
 * This enum contains the all the knowledge required to handle
 * Unicode in JavaScript.
 *
 * SPACE
 *   Every character that is either in the ECMA-262 5th Edition
 *   class WhiteSpace or LineTerminator.
 *
 *   WhiteSpace
 *    \u0009, \u000B, \u000C, \u0020, \u00A0 and \uFEFF
 *    and every other Unicode character with the General Category "Zs".
 *    In pratice this is every character with the value "Zs" as the third
 *    field (after the char code in hex, and the name) called General_Category
 *    (see http://www.unicode.org/reports/tr44/#UnicodeData.txt)
 *     in the file UnicodeData.txt.
 *
 *   LineTerminator
 *    \u000A, \u000D, \u2028, \u2029
 *
 * LETTER
 *   This are all characters included UnicodeLetter from ECMA-262.
 *   This includes the category 'Lu', 'Ll', 'Lt', 'Lm', 'Lo', 'Nl'
 *
 * IDENTIFIER_PART
 *   This is UnicodeCombiningMark, UnicodeDigit, UnicodeConnectorPunctuation.
 *   Aka categories Mn/Mc, Md, Nd, Pc
 *   And <ZWNJ> and <ZWJ>.
 *   Attention: FLAG_LETTER is _not_ IdentifierStart, but you could build
 *   a matcher for the real IdentifierPart like this:
 *
 *   if isEscapeSequence():
 *      handleEscapeSequence()
 *      return True
 *   if char in ['$', '_']:
 *      return True
 *   if GetFlag(char) & (FLAG_IDENTIFIER_PART | FLAG_LETTER):
 *      return True
 *
 */

struct CharFlag {
    enum temp {
        SPACE  = 1 << 0,
        LETTER = 1 << 1,
        IDENTIFIER_PART = 1 << 2,
    };
};

const char16_t BYTE_ORDER_MARK2 = 0xFFFE;
const char16_t NO_BREAK_SPACE  = 0x00A0;

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

    inline bool isLetter() const {
        return flags & CharFlag::LETTER;
    }

    inline bool isIdentifierPart() const {
        return flags & (CharFlag::IDENTIFIER_PART | CharFlag::LETTER);
    }
};

extern const uint8_t index1[];
extern const uint8_t index2[];
extern const CharacterInfo js_charinfo[];

inline const CharacterInfo&
CharInfo(char16_t code)
{
    const size_t shift = 5;
    size_t index = index1[code >> shift];
    index = index2[(index << shift) + (code & ((1 << shift) - 1))];

    return js_charinfo[index];
}

inline bool
IsIdentifierStart(char16_t ch)
{
    /*
     * ES5 7.6 IdentifierStart
     *  $ (dollar sign)
     *  _ (underscore)
     *  or any UnicodeLetter.
     *
     * We use a lookup table for small and thus common characters for speed.
     */

    if (ch < 128)
        return js_isidstart[ch];

    return CharInfo(ch).isLetter();
}

inline bool
IsIdentifierPart(char16_t ch)
{
    /* Matches ES5 7.6 IdentifierPart. */

    if (ch < 128)
        return js_isident[ch];

    return CharInfo(ch).isIdentifierPart();
}

inline bool
IsLetter(char16_t ch)
{
    return CharInfo(ch).isLetter();
}

inline bool
IsSpace(char16_t ch)
{
    /*
     * IsSpace checks if some character is included in the merged set
     * of WhiteSpace and LineTerminator, specified by ES5 7.2 and 7.3.
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

// Returns true iff ToLowerCase(ch) != ch.
inline bool
CanLowerCase(char16_t ch)
{
    if (ch < 128)
        return ch >= 'A' && ch <= 'Z';
    return CharInfo(ch).lowerCase != 0;
}

} /* namespace unicode */
} /* namespace js */

#endif /* vm_Unicode_h */
