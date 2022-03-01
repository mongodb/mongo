/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/intl/LineBreaker.h"

#include "jisx4051class.h"
#include "nsComplexBreaker.h"
#include "nsTArray.h"
#include "nsUnicodeProperties.h"
#include "mozilla/ArrayUtils.h"

using namespace mozilla::unicode;
using namespace mozilla::intl;

/*static*/
already_AddRefed<LineBreaker> LineBreaker::Create() {
  return RefPtr<LineBreaker>(new LineBreaker()).forget();
}

/*

   Simplification of Pair Table in JIS X 4051

   1. The Origion Table - in 4.1.3

   In JIS x 4051. The pair table is defined as below

   Class of
   Leading    Class of Trailing Char Class
   Char

              1  2  3  4  5  6  7  8  9 10 11 12 13 13 14 14 15 16 17 18 19 20
                                                 *  #  *  #
        1     X  X  X  X  X  X  X  X  X  X  X  X  X  X  X  X  X  X  X  X  X  E
        2        X  X  X  X  X                                               X
        3        X  X  X  X  X                                               X
        4        X  X  X  X  X                                               X
        5        X  X  X  X  X                                               X
        6        X  X  X  X  X                                               X
        7        X  X  X  X  X  X                                            X
        8        X  X  X  X  X                                X              E
        9        X  X  X  X  X                                               X
       10        X  X  X  X  X                                               X
       11        X  X  X  X  X                                               X
       12        X  X  X  X  X                                               X
       13        X  X  X  X  X                    X                          X
       14        X  X  X  X  X                          X                    X
       15        X  X  X  X  X        X                       X        X     X
       16        X  X  X  X  X                                   X     X     X
       17        X  X  X  X  X                                               E
       18        X  X  X  X  X                                X  X     X     X
       19     X  E  E  E  E  E  X  X  X  X  X  X  X  X  X  X  X  X  E  X  E  E
       20        X  X  X  X  X                                               E

   * Same Char
   # Other Char

   X Cannot Break

   The classes mean:
      1: Open parenthesis
      2: Close parenthesis
      3: Prohibit a line break before
      4: Punctuation for sentence end (except Full stop, e.g., "!" and "?")
      5: Middle dot (e.g., U+30FB KATAKANA MIDDLE DOT)
      6: Full stop
      7: Non-breakable between same characters
      8: Prefix (e.g., "$", "NO.")
      9: Postfix (e.g., "%")
     10: Ideographic space
     11: Hiragana
     12: Japanese characters (except class 11)
     13: Subscript
     14: Ruby
     15: Numeric
     16: Alphabet
     17: Space for Western language
     18: Western characters (except class 17)
     19: Split line note (Warichu) begin quote
     20: Split line note (Warichu) end quote

   2. Simplified by remove the class which we do not care

   However, since we do not care about class 13(Subscript), 14(Ruby),
   16 (Aphabet), 19(split line note begin quote), and 20(split line note end
   quote) we can simplify this par table into the following

   Class of
   Leading    Class of Trailing Char Class
   Char

              1  2  3  4  5  6  7  8  9 10 11 12 15 17 18

        1     X  X  X  X  X  X  X  X  X  X  X  X  X  X  X
        2        X  X  X  X  X
        3        X  X  X  X  X
        4        X  X  X  X  X
        5        X  X  X  X  X
        6        X  X  X  X  X
        7        X  X  X  X  X  X
        8        X  X  X  X  X                    X
        9        X  X  X  X  X
       10        X  X  X  X  X
       11        X  X  X  X  X
       12        X  X  X  X  X
       15        X  X  X  X  X        X           X     X
       17        X  X  X  X  X
       18        X  X  X  X  X                    X     X

   3. Simplified by merged classes

   After the 2 simplification, the pair table have some duplication
   a. class 2, 3, 4, 5, 6,  are the same- we can merged them
   b. class 10, 11, 12, 17  are the same- we can merged them

   We introduce an extra non-breaking pair at [b]/7 to better match
   the expectations of CSS line-breaking as tested by WPT tests.
   This added entry is marked as * in the tables below.

   Class of
   Leading    Class of Trailing Char Class
   Char

              1 [a] 7  8  9 [b]15 18

        1     X  X  X  X  X  X  X  X
      [a]        X
        7        X  X
        8        X              X
        9        X
      [b]        X  *
       15        X        X     X  X
       18        X              X  X


   4. We add COMPLEX characters and make it breakable w/ all ther class
      except after class 1 and before class [a]

   Class of
   Leading    Class of Trailing Char Class
   Char

              1 [a] 7  8  9 [b]15 18 COMPLEX

        1     X  X  X  X  X  X  X  X  X
      [a]        X
        7        X  X
        8        X              X
        9        X
      [b]        X  *
       15        X        X     X  X
       18        X              X  X
  COMPLEX        X                    T

     T : need special handling


   5. However, we need two special class for some punctuations/parentheses,
      theirs breaking rules like character class (18), see bug 389056.
      And also we need character like punctuation that is same behavior with 18,
      but the characters are not letters of all languages. (e.g., '_')
      [c]. Based on open parenthesis class (1), but it is not breakable after
           character class (18) or numeric class (15).
      [d]. Based on close parenthesis (or punctuation) class (2), but it is not
           breakable before character class (18) or numeric class (15).

   Class of
   Leading    Class of Trailing Char Class
   Char

              1 [a] 7  8  9 [b]15 18 COMPLEX [c] [d]

        1     X  X  X  X  X  X  X  X  X       X    X
      [a]        X                            X    X
        7        X  X
        8        X              X
        9        X
      [b]        X  *                              X
       15        X        X     X  X          X    X
       18        X              X  X          X    X
  COMPLEX        X                    T
      [c]     X  X  X  X  X  X  X  X  X       X    X
      [d]        X              X  X               X


   6. And Unicode has "NON-BREAK" characters. The lines should be broken around
      them. But in JIS X 4051, such class is not, therefore, we create [e].

   Class of
   Leading    Class of Trailing Char Class
   Char

              1 [a] 7  8  9 [b]15 18 COMPLEX [c] [d] [e]

        1     X  X  X  X  X  X  X  X  X       X    X   X
      [a]        X                                 X   X
        7        X  X                                  X
        8        X              X                      X
        9        X                                     X
      [b]        X  *                              X   X
       15        X        X     X  X          X    X   X
       18        X              X  X          X    X   X
  COMPLEX        X                    T                X
      [c]     X  X  X  X  X  X  X  X  X       X    X   X
      [d]        X              X  X               X   X
      [e]     X  X  X  X  X  X  X  X  X       X    X   X


   7. Now we use one bit to encode whether it is breakable, and use 2 bytes
      for one row, then the bit table will look like:

                 18    <-   1

       1  0000 1111 1111 1111  = 0x0FFF
      [a] 0000 1100 0000 0010  = 0x0C02
       7  0000 1000 0000 0110  = 0x0806
       8  0000 1000 0100 0010  = 0x0842
       9  0000 1000 0000 0010  = 0x0802
      [b] 0000 1100 0000 0110  = 0x0C06
      15  0000 1110 1101 0010  = 0x0ED2
      18  0000 1110 1100 0010  = 0x0EC2
 COMPLEX  0000 1001 0000 0010  = 0x0902
      [c] 0000 1111 1111 1111  = 0x0FFF
      [d] 0000 1100 1100 0010  = 0x0CC2
      [e] 0000 1111 1111 1111  = 0x0FFF
*/

#define MAX_CLASSES 12

static const uint16_t gPair[MAX_CLASSES] = {0x0FFF, 0x0C02, 0x0806, 0x0842,
                                            0x0802, 0x0C06, 0x0ED2, 0x0EC2,
                                            0x0902, 0x0FFF, 0x0CC2, 0x0FFF};

/*

   8. And if the character is not enough far from word start, word end and
      another break point, we should not break in non-CJK languages.
      I.e., Don't break around 15, 18, [c] and [d], but don't change
      that if they are related to [b].

   Class of
   Leading    Class of Trailing Char Class
   Char

              1 [a] 7  8  9 [b]15 18 COMPLEX [c] [d] [e]

        1     X  X  X  X  X  X  X  X  X       X    X   X
      [a]        X              X  X          X    X   X
        7        X  X           X  X          X    X   X
        8        X              X  X          X    X   X
        9        X              X  X          X    X   X
      [b]        X  *                              X   X
       15     X  X  X  X  X     X  X  X       X    X   X
       18     X  X  X  X  X     X  X  X       X    X   X
  COMPLEX        X              X  X  T       X    X   X
      [c]     X  X  X  X  X  X  X  X  X       X    X   X
      [d]     X  X  X  X  X     X  X  X       X    X   X
      [e]     X  X  X  X  X  X  X  X  X       X    X   X

                 18    <-   1

       1  0000 1111 1111 1111  = 0x0FFF
      [a] 0000 1110 1100 0010  = 0x0EC2
       7  0000 1110 1100 0110  = 0x0EC6
       8  0000 1110 1100 0010  = 0x0EC2
       9  0000 1110 1100 0010  = 0x0EC2
      [b] 0000 1100 0000 0110  = 0x0C06
      15  0000 1111 1101 1111  = 0x0FDF
      18  0000 1111 1101 1111  = 0x0FDF
 COMPLEX  0000 1111 1100 0010  = 0x0FC2
      [c] 0000 1111 1111 1111  = 0x0FFF
      [d] 0000 1111 1101 1111  = 0x0FDF
      [e] 0000 1111 1111 1111  = 0x0FFF
*/

static const uint16_t gPairConservative[MAX_CLASSES] = {
    0x0FFF, 0x0EC2, 0x0EC6, 0x0EC2, 0x0EC2, 0x0C06,
    0x0FDF, 0x0FDF, 0x0FC2, 0x0FFF, 0x0FDF, 0x0FFF};

/*

   9. Now we map the class to number

      0: 1
      1: [a]- 2, 3, 4, 5, 6
      2: 7
      3: 8
      4: 9
      5: [b]- 10, 11, 12, 17
      6: 15
      7: 18
      8: COMPLEX
      9: [c]
      A: [d]
      B: [e]

    and they mean:
      0: Open parenthesis
      1: Punctuation that prohibits break before
      2: Non-breakable between same classes
      3: Prefix
      4: Postfix
      5: Breakable character (Spaces and Most Japanese characters)
      6: Numeric
      7: Characters
      8: Need special handling characters (E.g., Thai)
      9: Open parentheses like Character (See bug 389056)
      A: Close parenthese (or punctuations) like Character (See bug 389056)
      B: Non breakable (See bug 390920)

*/

#define CLASS_NONE INT8_MAX

#define CLASS_OPEN 0x00
#define CLASS_CLOSE 0x01
#define CLASS_NON_BREAKABLE_BETWEEN_SAME_CLASS 0x02
#define CLASS_PREFIX 0x03
#define CLASS_POSTFFIX 0x04
#define CLASS_BREAKABLE 0x05
#define CLASS_NUMERIC 0x06
#define CLASS_CHARACTER 0x07
#define CLASS_COMPLEX 0x08
#define CLASS_OPEN_LIKE_CHARACTER 0x09
#define CLASS_CLOSE_LIKE_CHARACTER 0x0A
#define CLASS_NON_BREAKABLE 0x0B

#define U_NULL char16_t(0x0000)
#define U_SLASH char16_t('/')
#define U_SPACE char16_t(' ')
#define U_HYPHEN char16_t('-')
#define U_EQUAL char16_t('=')
#define U_PERCENT char16_t('%')
#define U_AMPERSAND char16_t('&')
#define U_SEMICOLON char16_t(';')
#define U_BACKSLASH char16_t('\\')
#define U_OPEN_SINGLE_QUOTE char16_t(0x2018)
#define U_OPEN_DOUBLE_QUOTE char16_t(0x201C)
#define U_OPEN_GUILLEMET char16_t(0x00AB)

#define NEED_CONTEXTUAL_ANALYSIS(c)                                            \
  (IS_HYPHEN(c) || (c) == U_SLASH || (c) == U_PERCENT || (c) == U_AMPERSAND || \
   (c) == U_SEMICOLON || (c) == U_BACKSLASH || (c) == U_OPEN_SINGLE_QUOTE ||   \
   (c) == U_OPEN_DOUBLE_QUOTE || (c) == U_OPEN_GUILLEMET)

#define IS_ASCII_DIGIT(u) (0x0030 <= (u) && (u) <= 0x0039)

static inline int GETCLASSFROMTABLE(const uint32_t* t, uint16_t l) {
  return ((((t)[(l >> 3)]) >> ((l & 0x0007) << 2)) & 0x000f);
}

static inline int IS_HALFWIDTH_IN_JISx4051_CLASS3(char16_t u) {
  return ((0xff66 <= (u)) && ((u) <= 0xff70));
}

static inline int IS_CJK_CHAR(char32_t u) {
  return (
      (0x1100 <= (u) && (u) <= 0x11ff) || (0x2e80 <= (u) && (u) <= 0xd7ff) ||
      (0xf900 <= (u) && (u) <= 0xfaff) || (0xff00 <= (u) && (u) <= 0xffef) ||
      (0x20000 <= (u) && (u) <= 0x2fffd));
}

static inline bool IS_NONBREAKABLE_SPACE(char16_t u) {
  return u == 0x00A0 || u == 0x2007;  // NO-BREAK SPACE, FIGURE SPACE
}

static inline bool IS_HYPHEN(char16_t u) {
  return (u == U_HYPHEN || u == 0x2010 ||  // HYPHEN
          u == 0x2012 ||                   // FIGURE DASH
          u == 0x2013 ||                   // EN DASH
#if ANDROID
          /* Bug 1647377: On Android, we don't have a "platform" backend
           * that supports Tibetan (nsRuleBreaker.cpp only knows about
           * Thai), so instead we just treat the TSHEG like a hyphen to
           * provide basic line-breaking possibilities.
           */
          u == 0x0F0B ||  // TIBETAN MARK INTERSYLLABIC TSHEG
#endif
          u == 0x058A);  // ARMENIAN HYPHEN
}

static int8_t GetClass(uint32_t u, LineBreaker::Strictness aLevel,
                       bool aIsChineseOrJapanese) {
  // Mapping for Unicode LineBreak.txt classes to the (simplified) set of
  // character classes used here.
  // XXX The mappings here were derived by comparing the Unicode LineBreak
  //     values of BMP characters to the classes our existing GetClass returns
  //     for the same codepoints; in cases where characters with the same
  //     LineBreak class mapped to various classes here, I picked what seemed
  //     the most prevalent equivalence.
  //     Some of these are unclear to me, but currently they are ONLY used
  //     for characters not handled by the old code below, so all the JISx405
  //     special cases should already be accounted for.
  static const int8_t sUnicodeLineBreakToClass[] = {
      /* UNKNOWN = 0,                       [XX] */ CLASS_CHARACTER,
      /* AMBIGUOUS = 1,                     [AI] */ CLASS_CHARACTER,
      /* ALPHABETIC = 2,                    [AL] */ CLASS_CHARACTER,
      /* BREAK_BOTH = 3,                    [B2] */ CLASS_CHARACTER,
      /* BREAK_AFTER = 4,                   [BA] */ CLASS_CHARACTER,
      /* BREAK_BEFORE = 5,                  [BB] */ CLASS_OPEN_LIKE_CHARACTER,
      /* MANDATORY_BREAK = 6,               [BK] */ CLASS_CHARACTER,
      /* CONTINGENT_BREAK = 7,              [CB] */ CLASS_CHARACTER,
      /* CLOSE_PUNCTUATION = 8,             [CL] */ CLASS_CHARACTER,
      /* COMBINING_MARK = 9,                [CM] */ CLASS_CHARACTER,
      /* CARRIAGE_RETURN = 10,              [CR] */ CLASS_BREAKABLE,
      /* EXCLAMATION = 11,                  [EX] */ CLASS_CHARACTER,
      /* GLUE = 12,                         [GL] */ CLASS_NON_BREAKABLE,
      /* HYPHEN = 13,                       [HY] */ CLASS_CHARACTER,
      /* IDEOGRAPHIC = 14,                  [ID] */ CLASS_BREAKABLE,
      /* INSEPARABLE = 15,                  [IN] */ CLASS_CLOSE_LIKE_CHARACTER,
      /* INFIX_NUMERIC = 16,                [IS] */ CLASS_CHARACTER,
      /* LINE_FEED = 17,                    [LF] */ CLASS_BREAKABLE,
      /* NONSTARTER = 18,                   [NS] */ CLASS_CLOSE_LIKE_CHARACTER,
      /* NUMERIC = 19,                      [NU] */ CLASS_NUMERIC,
      /* OPEN_PUNCTUATION = 20,             [OP] */ CLASS_CHARACTER,
      /* POSTFIX_NUMERIC = 21,              [PO] */ CLASS_CHARACTER,
      /* PREFIX_NUMERIC = 22,               [PR] */ CLASS_CHARACTER,
      /* QUOTATION = 23,                    [QU] */ CLASS_CHARACTER,
      /* COMPLEX_CONTEXT = 24,              [SA] */ CLASS_CHARACTER,
      /* SURROGATE = 25,                    [SG] */ CLASS_CHARACTER,
      /* SPACE = 26,                        [SP] */ CLASS_BREAKABLE,
      /* BREAK_SYMBOLS = 27,                [SY] */ CLASS_CHARACTER,
      /* ZWSPACE = 28,                      [ZW] */ CLASS_BREAKABLE,
      /* NEXT_LINE = 29,                    [NL] */ CLASS_CHARACTER,
      /* WORD_JOINER = 30,                  [WJ] */ CLASS_NON_BREAKABLE,
      /* H2 = 31,                           [H2] */ CLASS_BREAKABLE,
      /* H3 = 32,                           [H3] */ CLASS_BREAKABLE,
      /* JL = 33,                           [JL] */ CLASS_CHARACTER,
      /* JT = 34,                           [JT] */ CLASS_CHARACTER,
      /* JV = 35,                           [JV] */ CLASS_CHARACTER,
      /* CLOSE_PARENTHESIS = 36,            [CP] */ CLASS_CLOSE_LIKE_CHARACTER,
      /* CONDITIONAL_JAPANESE_STARTER = 37, [CJ] */ CLASS_CLOSE,
      /* HEBREW_LETTER = 38,                [HL] */ CLASS_CHARACTER,
      /* REGIONAL_INDICATOR = 39,           [RI] */ CLASS_CHARACTER,
      /* E_BASE = 40,                       [EB] */ CLASS_BREAKABLE,
      /* E_MODIFIER = 41,                   [EM] */ CLASS_CHARACTER,
      /* ZWJ = 42,                          [ZWJ]*/ CLASS_CHARACTER};

  static_assert(U_LB_COUNT == mozilla::ArrayLength(sUnicodeLineBreakToClass),
                "Gecko vs ICU LineBreak class mismatch");

  auto cls = GetLineBreakClass(u);
  MOZ_ASSERT(cls < mozilla::ArrayLength(sUnicodeLineBreakToClass));

  // Overrides based on rules for the different line-break values given in
  // https://drafts.csswg.org/css-text-3/#line-break-property
  switch (aLevel) {
    case LineBreaker::Strictness::Auto:
      // For now, just use legacy Gecko behavior.
      // XXX Possible enhancement - vary strictness according to line width
      // or other criteria.
      break;
    case LineBreaker::Strictness::Strict:
      if (cls == U_LB_CONDITIONAL_JAPANESE_STARTER ||
          (u == 0x3095 || u == 0x3096 || u == 0x30f5 || u == 0x30f6)) {
        return CLASS_CLOSE;
      }
      if (cls == U_LB_INSEPARABLE) {
        return CLASS_NON_BREAKABLE_BETWEEN_SAME_CLASS;
      }
      if (u == 0x3005 || u == 0x303B || u == 0x309D || u == 0x309E ||
          u == 0x30FD || u == 0x30FE) {
        return CLASS_CLOSE_LIKE_CHARACTER;
      }
      if (aIsChineseOrJapanese) {
        if (cls == U_LB_POSTFIX_NUMERIC && IsEastAsianWidthAFW(u)) {
          return CLASS_CLOSE_LIKE_CHARACTER;
        }
        if (cls == U_LB_PREFIX_NUMERIC && IsEastAsianWidthAFW(u)) {
          return CLASS_OPEN_LIKE_CHARACTER;
        }
        if (u == 0x2010 || u == 0x2013 || u == 0x301C || u == 0x30A0) {
          return CLASS_CLOSE_LIKE_CHARACTER;
        }
      }
      break;
    case LineBreaker::Strictness::Normal:
      if (cls == U_LB_CONDITIONAL_JAPANESE_STARTER) {
        return CLASS_BREAKABLE;
      }
      if (cls == U_LB_INSEPARABLE) {
        return CLASS_NON_BREAKABLE_BETWEEN_SAME_CLASS;
      }
      if (u == 0x3005 || u == 0x303B || u == 0x309D || u == 0x309E ||
          u == 0x30FD || u == 0x30FE) {
        return CLASS_CLOSE_LIKE_CHARACTER;
      }
      if (aIsChineseOrJapanese) {
        if (cls == U_LB_POSTFIX_NUMERIC && IsEastAsianWidthAFW(u)) {
          return CLASS_CLOSE_LIKE_CHARACTER;
        }
        if (cls == U_LB_PREFIX_NUMERIC && IsEastAsianWidthAFW(u)) {
          return CLASS_OPEN_LIKE_CHARACTER;
        }
        if (u == 0x2010 || u == 0x2013 || u == 0x301C || u == 0x30A0) {
          return CLASS_BREAKABLE;
        }
      }
      break;
    case LineBreaker::Strictness::Loose:
      if (cls == U_LB_CONDITIONAL_JAPANESE_STARTER) {
        return CLASS_BREAKABLE;
      }
      if (u == 0x3005 || u == 0x303B || u == 0x309D || u == 0x309E ||
          u == 0x30FD || u == 0x30FE) {
        return CLASS_BREAKABLE;
      }
      if (cls == U_LB_INSEPARABLE) {
        return CLASS_BREAKABLE;
      }
      if (aIsChineseOrJapanese) {
        if (u == 0x30FB || u == 0xFF1A || u == 0xFF1B || u == 0xFF65 ||
            u == 0x203C || u == 0x2047 || u == 0x2048 || u == 0x2049 ||
            u == 0xFF01 || u == 0xFF1F) {
          return CLASS_BREAKABLE;
        }
        if (cls == U_LB_POSTFIX_NUMERIC && IsEastAsianWidthAFW(u)) {
          return CLASS_BREAKABLE;
        }
        if (cls == U_LB_PREFIX_NUMERIC && IsEastAsianWidthAFW(u)) {
          return CLASS_BREAKABLE;
        }
        if (u == 0x2010 || u == 0x2013 || u == 0x301C || u == 0x30A0) {
          return CLASS_BREAKABLE;
        }
      }
      break;
    case LineBreaker::Strictness::Anywhere:
      MOZ_ASSERT_UNREACHABLE("should have been handled already");
      break;
  }

  if (u < 0x10000) {
    uint16_t h = u & 0xFF00;
    uint16_t l = u & 0x00ff;

    // Handle 3 range table first
    if (0x0000 == h) {
      return GETCLASSFROMTABLE(gLBClass00, l);
    }
    if (0x1700 == h) {
      return GETCLASSFROMTABLE(gLBClass17, l);
    }
    if (NS_NeedsPlatformNativeHandling(u)) {
      return CLASS_COMPLEX;
    }
    if (0x0E00 == h) {
      return GETCLASSFROMTABLE(gLBClass0E, l);
    }
    if (0x2000 == h) {
      return GETCLASSFROMTABLE(gLBClass20, l);
    }
    if (0x2100 == h) {
      return GETCLASSFROMTABLE(gLBClass21, l);
    }
    if (0x3000 == h) {
      return GETCLASSFROMTABLE(gLBClass30, l);
    }
    if (0xff00 == h) {
      if (l <= 0x0060) {  // Fullwidth ASCII variant
        // Fullwidth comma and period are exceptions to our map-to-ASCII
        // behavior: https://bugzilla.mozilla.org/show_bug.cgi?id=1595428
        if (l + 0x20 == ',' || l + 0x20 == '.') {
          return CLASS_CLOSE;
        }
        // Also special-case fullwidth left/right white parenthesis,
        // which do not fit the pattern of mapping to the ASCII block
        if (l == 0x005f) {
          return CLASS_OPEN;
        }
        if (l == 0x0060) {
          return CLASS_CLOSE;
        }
        return GETCLASSFROMTABLE(gLBClass00, (l + 0x20));
      }
      if (l < 0x00a0) {  // Halfwidth Katakana variants
        switch (l) {
          case 0x61:
            return GetClass(0x3002, aLevel, aIsChineseOrJapanese);
          case 0x62:
            return GetClass(0x300c, aLevel, aIsChineseOrJapanese);
          case 0x63:
            return GetClass(0x300d, aLevel, aIsChineseOrJapanese);
          case 0x64:
            return GetClass(0x3001, aLevel, aIsChineseOrJapanese);
          case 0x65:
            return GetClass(0x30fb, aLevel, aIsChineseOrJapanese);
          case 0x9e:
            return GetClass(0x309b, aLevel, aIsChineseOrJapanese);
          case 0x9f:
            return GetClass(0x309c, aLevel, aIsChineseOrJapanese);
          default:
            if (IS_HALFWIDTH_IN_JISx4051_CLASS3(u)) {
              return CLASS_CLOSE;  // jis x4051 class 3
            }
            return CLASS_BREAKABLE;  // jis x4051 class 11
        }
      }
      if (l < 0x00e0) {
        return CLASS_CHARACTER;  // Halfwidth Hangul variants
      }
      if (l < 0x00f0) {
        static char16_t NarrowFFEx[16] = {
            0x00A2, 0x00A3, 0x00AC, 0x00AF, 0x00A6, 0x00A5, 0x20A9, 0x0000,
            0x2502, 0x2190, 0x2191, 0x2192, 0x2193, 0x25A0, 0x25CB, 0x0000};
        return GetClass(NarrowFFEx[l - 0x00e0], aLevel, aIsChineseOrJapanese);
      }
    } else if (0x3100 == h) {
      if (l <= 0xbf) {  // Hangul Compatibility Jamo, Bopomofo, Kanbun
                        // XXX: This is per UAX #14, but UAX #14 may change
                        // the line breaking rules about Kanbun and Bopomofo.
        return CLASS_BREAKABLE;
      }
      if (l >= 0xf0) {  // Katakana small letters for Ainu
        return CLASS_CLOSE;
      }
    } else if (0x0300 == h) {
      if (0x4F == l || (0x5C <= l && l <= 0x62)) {
        return CLASS_NON_BREAKABLE;
      }
    } else if (0x0500 == h) {
      // ARMENIAN HYPHEN (for "Breaking Hyphens" of UAX#14)
      if (l == 0x8A) {
        return GETCLASSFROMTABLE(gLBClass00, uint16_t(U_HYPHEN));
      }
    } else if (0x0F00 == h) {
      // Tibetan chars with class = BA
      if (0x34 == l || 0x7f == l || 0x85 == l || 0xbe == l || 0xbf == l ||
          0xd2 == l) {
        return CLASS_BREAKABLE;
      }
    } else if (0x1800 == h) {
      if (0x0E == l) {
        return CLASS_NON_BREAKABLE;
      }
    } else if (0x1600 == h) {
      if (0x80 == l) {  // U+1680 OGHAM SPACE MARK
        return CLASS_BREAKABLE;
      }
    } else if (u == 0xfeff) {
      return CLASS_NON_BREAKABLE;
    }
  }

  return sUnicodeLineBreakToClass[cls];
}

static bool GetPair(int8_t c1, int8_t c2) {
  NS_ASSERTION(c1 < MAX_CLASSES, "illegal classes 1");
  NS_ASSERTION(c2 < MAX_CLASSES, "illegal classes 2");

  return (0 == ((gPair[c1] >> c2) & 0x0001));
}

static bool GetPairConservative(int8_t c1, int8_t c2) {
  NS_ASSERTION(c1 < MAX_CLASSES, "illegal classes 1");
  NS_ASSERTION(c2 < MAX_CLASSES, "illegal classes 2");

  return (0 == ((gPairConservative[c1] >> c2) & 0x0001));
}

class ContextState {
 public:
  ContextState(const char16_t* aText, uint32_t aLength)
      : mUniText(aText), mText(nullptr), mLength(aLength) {
    Init();
  }

  ContextState(const uint8_t* aText, uint32_t aLength)
      : mUniText(nullptr), mText(aText), mLength(aLength) {
    Init();
  }

  uint32_t Length() const { return mLength; }
  uint32_t Index() const { return mIndex; }

  // This gets a single code unit of the text, without checking for surrogates
  // (in the case of a 16-bit text buffer). That's OK if we're only checking for
  // specific characters that are known to be BMP values.
  char16_t GetCodeUnitAt(uint32_t aIndex) const {
    MOZ_ASSERT(aIndex < mLength, "Out of range!");
    return mUniText ? mUniText[aIndex] : char16_t(mText[aIndex]);
  }

  // This gets a 32-bit Unicode character (codepoint), handling surrogate pairs
  // as necessary. It must ONLY be called for 16-bit text, not 8-bit.
  char32_t GetUnicodeCharAt(uint32_t aIndex) const {
    MOZ_ASSERT(mUniText, "Only for 16-bit text!");
    MOZ_ASSERT(aIndex < mLength, "Out of range!");
    char32_t c = mUniText[aIndex];
    if (aIndex + 1 < mLength && NS_IS_SURROGATE_PAIR(c, mUniText[aIndex + 1])) {
      c = SURROGATE_TO_UCS4(c, mUniText[aIndex + 1]);
    }
    return c;
  }

  void AdvanceIndex() { ++mIndex; }

  void NotifyBreakBefore() { mLastBreakIndex = mIndex; }

  // A word of western language should not be broken. But even if the word has
  // only ASCII characters, non-natural context words should be broken, e.g.,
  // URL and file path. For protecting the natural words, we should use
  // conservative breaking rules at following conditions:
  //   1. at near the start of word
  //   2. at near the end of word
  //   3. at near the latest broken point
  // CONSERVATIVE_RANGE_{LETTER,OTHER} define the 'near' in characters,
  // which varies depending whether we are looking at a letter or a non-letter
  // character: for non-letters, we use an extended "conservative" range.

#define CONSERVATIVE_RANGE_LETTER 2
#define CONSERVATIVE_RANGE_OTHER 6

  bool UseConservativeBreaking(uint32_t aOffset = 0) const {
    if (mHasCJKChar) return false;
    uint32_t index = mIndex + aOffset;

    // If the character at index is a letter (rather than various punctuation
    // characters, etc) then we want a shorter "conservative" range
    uint32_t conservativeRangeStart, conservativeRangeEnd;
    if (index < mLength &&
        nsUGenCategory::kLetter ==
            (mText ? GetGenCategory(mText[index])
                   : GetGenCategory(GetUnicodeCharAt(index)))) {
      // Primarily for hyphenated word prefixes/suffixes; we add 1 to Start
      // to get more balanced behavior (if we break off a 2-letter prefix,
      // that means the break will actually be three letters from start of
      // word, to include the hyphen; whereas a 2-letter suffix will be
      // broken only two letters from end of word).
      conservativeRangeEnd = CONSERVATIVE_RANGE_LETTER;
      conservativeRangeStart = CONSERVATIVE_RANGE_LETTER + 1;
    } else {
      conservativeRangeEnd = conservativeRangeStart = CONSERVATIVE_RANGE_OTHER;
    }

    bool result = (index < conservativeRangeStart ||
                   mLength - index < conservativeRangeEnd ||
                   index - mLastBreakIndex < conservativeRangeStart);
    if (result || !mHasNonbreakableSpace) return result;

    // This text has no-breakable space, we need to check whether the index
    // is near it.

    // Note that index is always larger than conservativeRange here.
    for (uint32_t i = index; index - conservativeRangeStart < i; --i) {
      if (IS_NONBREAKABLE_SPACE(GetCodeUnitAt(i - 1))) return true;
    }
    // Note that index is always less than mLength - conservativeRange.
    for (uint32_t i = index + 1; i < index + conservativeRangeEnd; ++i) {
      if (IS_NONBREAKABLE_SPACE(GetCodeUnitAt(i))) return true;
    }
    return false;
  }

  bool HasPreviousEqualsSign() const { return mHasPreviousEqualsSign; }
  void NotifySeenEqualsSign() { mHasPreviousEqualsSign = true; }

  bool HasPreviousSlash() const { return mHasPreviousSlash; }
  void NotifySeenSlash() { mHasPreviousSlash = true; }

  bool HasPreviousBackslash() const { return mHasPreviousBackslash; }
  void NotifySeenBackslash() { mHasPreviousBackslash = true; }

  uint32_t GetPreviousNonHyphenCharacter() const {
    return mPreviousNonHyphenCharacter;
  }
  void NotifyNonHyphenCharacter(uint32_t ch) {
    mPreviousNonHyphenCharacter = ch;
  }

 private:
  void Init() {
    mIndex = 0;
    mLastBreakIndex = 0;
    mPreviousNonHyphenCharacter = U_NULL;
    mHasCJKChar = false;
    mHasNonbreakableSpace = false;
    mHasPreviousEqualsSign = false;
    mHasPreviousSlash = false;
    mHasPreviousBackslash = false;

    if (mText) {
      // 8-bit text: we only need to check for &nbsp;
      for (uint32_t i = 0; i < mLength; ++i) {
        if (IS_NONBREAKABLE_SPACE(mText[i])) {
          mHasNonbreakableSpace = true;
          break;
        }
      }
    } else {
      // 16-bit text: handle surrogates and check for CJK as well as &nbsp;
      for (uint32_t i = 0; i < mLength; ++i) {
        char32_t u = GetUnicodeCharAt(i);
        if (!mHasNonbreakableSpace && IS_NONBREAKABLE_SPACE(u)) {
          mHasNonbreakableSpace = true;
          if (mHasCJKChar) {
            break;
          }
        } else if (!mHasCJKChar && IS_CJK_CHAR(u)) {
          mHasCJKChar = true;
          if (mHasNonbreakableSpace) {
            break;
          }
        }
        if (u > 0xFFFFu) {
          ++i;  // step over trailing low surrogate
        }
      }
    }
  }

  const char16_t* const mUniText;
  const uint8_t* const mText;

  uint32_t mIndex;
  const uint32_t mLength;  // length of text
  uint32_t mLastBreakIndex;
  char32_t mPreviousNonHyphenCharacter;  // The last character we have seen
                                         // which is not U_HYPHEN
  bool mHasCJKChar;             // if the text has CJK character, this is true.
  bool mHasNonbreakableSpace;   // if the text has no-breakable space,
                                // this is true.
  bool mHasPreviousEqualsSign;  // True if we have seen a U_EQUAL
  bool mHasPreviousSlash;       // True if we have seen a U_SLASH
  bool mHasPreviousBackslash;   // True if we have seen a U_BACKSLASH
};

static int8_t ContextualAnalysis(char32_t prev, char32_t cur, char32_t next,
                                 ContextState& aState,
                                 LineBreaker::Strictness aLevel,
                                 bool aIsChineseOrJapanese) {
  // Don't return CLASS_OPEN/CLASS_CLOSE if aState.UseJISX4051 is FALSE.

  if (IS_HYPHEN(cur)) {
    // If next character is hyphen, we don't need to break between them.
    if (IS_HYPHEN(next)) return CLASS_CHARACTER;
    // If prev and next characters are numeric, it may be in Math context.
    // So, we should not break here.
    bool prevIsNum = IS_ASCII_DIGIT(prev);
    bool nextIsNum = IS_ASCII_DIGIT(next);
    if (prevIsNum && nextIsNum) return CLASS_NUMERIC;
    // If one side is numeric and the other is a character, or if both sides are
    // characters, the hyphen should be breakable.
    if (!aState.UseConservativeBreaking(1)) {
      char32_t prevOfHyphen = aState.GetPreviousNonHyphenCharacter();
      if (prevOfHyphen && next) {
        int8_t prevClass = GetClass(prevOfHyphen, aLevel, aIsChineseOrJapanese);
        int8_t nextClass = GetClass(next, aLevel, aIsChineseOrJapanese);
        bool prevIsNumOrCharOrClose =
            prevIsNum ||
            (prevClass == CLASS_CHARACTER &&
             !NEED_CONTEXTUAL_ANALYSIS(prevOfHyphen)) ||
            prevClass == CLASS_CLOSE || prevClass == CLASS_CLOSE_LIKE_CHARACTER;
        bool nextIsNumOrCharOrOpen =
            nextIsNum ||
            (nextClass == CLASS_CHARACTER && !NEED_CONTEXTUAL_ANALYSIS(next)) ||
            nextClass == CLASS_OPEN || nextClass == CLASS_OPEN_LIKE_CHARACTER ||
            next == U_OPEN_SINGLE_QUOTE || next == U_OPEN_DOUBLE_QUOTE ||
            next == U_OPEN_GUILLEMET;
        if (prevIsNumOrCharOrClose && nextIsNumOrCharOrOpen) {
          return CLASS_CLOSE;
        }
      }
    }
  } else {
    aState.NotifyNonHyphenCharacter(cur);
    if (cur == U_SLASH || cur == U_BACKSLASH) {
      // If this is immediately after same char, we should not break here.
      if (prev == cur) return CLASS_CHARACTER;
      // If this text has two or more (BACK)SLASHs, this may be file path or
      // URL. Make sure to compute shouldReturn before we notify on this slash.
      bool shouldReturn = !aState.UseConservativeBreaking() &&
                          (cur == U_SLASH ? aState.HasPreviousSlash()
                                          : aState.HasPreviousBackslash());

      if (cur == U_SLASH) {
        aState.NotifySeenSlash();
      } else {
        aState.NotifySeenBackslash();
      }

      if (shouldReturn) return CLASS_OPEN;
    } else if (cur == U_PERCENT) {
      // If this is a part of the param of URL, we should break before.
      if (!aState.UseConservativeBreaking()) {
        if (aState.Index() >= 3 &&
            aState.GetCodeUnitAt(aState.Index() - 3) == U_PERCENT)
          return CLASS_OPEN;
        if (aState.Index() + 3 < aState.Length() &&
            aState.GetCodeUnitAt(aState.Index() + 3) == U_PERCENT)
          return CLASS_OPEN;
      }
    } else if (cur == U_AMPERSAND || cur == U_SEMICOLON) {
      // If this may be a separator of params of URL, we should break after.
      if (!aState.UseConservativeBreaking(1) && aState.HasPreviousEqualsSign())
        return CLASS_CLOSE;
    } else if (cur == U_OPEN_SINGLE_QUOTE || cur == U_OPEN_DOUBLE_QUOTE ||
               cur == U_OPEN_GUILLEMET) {
      // for CJK usage, we treat these as openers to allow a break before them,
      // but otherwise treat them as normal characters because quote mark usage
      // in various Western languages varies too much; see bug #450088
      // discussion.
      if (!aState.UseConservativeBreaking() && IS_CJK_CHAR(next))
        return CLASS_OPEN;
    } else {
      NS_ERROR("Forgot to handle the current character!");
    }
  }
  return GetClass(cur, aLevel, aIsChineseOrJapanese);
}

int32_t LineBreaker::WordMove(const char16_t* aText, uint32_t aLen,
                              uint32_t aPos, int8_t aDirection) {
  bool textNeedsJISx4051 = false;
  int32_t begin, end;

  for (begin = aPos; begin > 0 && !NS_IsSpace(aText[begin - 1]); --begin) {
    if (IS_CJK_CHAR(aText[begin]) ||
        NS_NeedsPlatformNativeHandling(aText[begin])) {
      textNeedsJISx4051 = true;
    }
  }
  for (end = aPos + 1; end < int32_t(aLen) && !NS_IsSpace(aText[end]); ++end) {
    if (IS_CJK_CHAR(aText[end]) || NS_NeedsPlatformNativeHandling(aText[end])) {
      textNeedsJISx4051 = true;
    }
  }

  int32_t ret;
  AutoTArray<uint8_t, 2000> breakState;
  if (!textNeedsJISx4051) {
    // No complex text character, do not try to do complex line break.
    // (This is required for serializers. See Bug #344816.)
    if (aDirection < 0) {
      ret = (begin == int32_t(aPos)) ? begin - 1 : begin;
    } else {
      ret = end;
    }
  } else {
    // XXX(Bug 1631371) Check if this should use a fallible operation as it
    // pretended earlier.
    breakState.AppendElements(end - begin);
    GetJISx4051Breaks(aText + begin, end - begin, WordBreak::Normal,
                      Strictness::Auto, false, breakState.Elements());

    ret = aPos;
    do {
      ret += aDirection;
    } while (begin < ret && ret < end && !breakState[ret - begin]);
  }

  return ret;
}

int32_t LineBreaker::Next(const char16_t* aText, uint32_t aLen, uint32_t aPos) {
  NS_ASSERTION(aText, "aText shouldn't be null");
  NS_ASSERTION(aLen > aPos,
               "Bad position passed to nsJISx4051LineBreaker::Next");

  int32_t nextPos = WordMove(aText, aLen, aPos, 1);
  return nextPos < int32_t(aLen) ? nextPos : NS_LINEBREAKER_NEED_MORE_TEXT;
}

int32_t LineBreaker::Prev(const char16_t* aText, uint32_t aLen, uint32_t aPos) {
  NS_ASSERTION(aText, "aText shouldn't be null");
  NS_ASSERTION(aLen >= aPos && aPos > 0,
               "Bad position passed to nsJISx4051LineBreaker::Prev");

  int32_t prevPos = WordMove(aText, aLen, aPos, -1);
  return prevPos > 0 ? prevPos : NS_LINEBREAKER_NEED_MORE_TEXT;
}

static bool SuppressBreakForKeepAll(uint32_t aPrev, uint32_t aCh) {
  auto affectedByKeepAll = [](uint8_t aLBClass) {
    switch (aLBClass) {
      // Per https://drafts.csswg.org/css-text-3/#valdef-word-break-keep-all:
      // "implicit soft wrap opportunities between typographic letter units
      // (or other typographic character units belonging to the NU, AL, AI,
      // or ID Unicode line breaking classes [UAX14]) are suppressed..."
      case U_LB_ALPHABETIC:
      case U_LB_AMBIGUOUS:
      case U_LB_NUMERIC:
      case U_LB_IDEOGRAPHIC:
      // Additional classes that should be treated similarly, but have been
      // broken out as separate classes in newer Unicode versions:
      case U_LB_H2:
      case U_LB_H3:
      case U_LB_JL:
      case U_LB_JV:
      case U_LB_JT:
      case U_LB_CONDITIONAL_JAPANESE_STARTER:
        return true;
      default:
        return false;
    }
  };
  return affectedByKeepAll(GetLineBreakClass(aPrev)) &&
         affectedByKeepAll(GetLineBreakClass(aCh));
}

void LineBreaker::GetJISx4051Breaks(const char16_t* aChars, uint32_t aLength,
                                    WordBreak aWordBreak, Strictness aLevel,
                                    bool aIsChineseOrJapanese,
                                    uint8_t* aBreakBefore) {
  uint32_t cur;
  int8_t lastClass = CLASS_NONE;
  ContextState state(aChars, aLength);

  for (cur = 0; cur < aLength; ++cur, state.AdvanceIndex()) {
    char32_t ch = state.GetUnicodeCharAt(cur);
    uint32_t chLen = ch > 0xFFFFu ? 2 : 1;
    int8_t cl;

    auto prev = [=]() -> char32_t {
      if (!cur) {
        return 0;
      }
      char32_t c = aChars[cur - 1];
      if (cur > 1 && NS_IS_SURROGATE_PAIR(aChars[cur - 2], c)) {
        c = SURROGATE_TO_UCS4(aChars[cur - 2], c);
      }
      return c;
    };

    if (NEED_CONTEXTUAL_ANALYSIS(ch)) {
      char32_t next;
      if (cur + chLen < aLength) {
        next = state.GetUnicodeCharAt(cur + chLen);
      } else {
        next = 0;
      }
      cl = ContextualAnalysis(prev(), ch, next, state, aLevel,
                              aIsChineseOrJapanese);
    } else {
      if (ch == U_EQUAL) state.NotifySeenEqualsSign();
      state.NotifyNonHyphenCharacter(ch);
      cl = GetClass(ch, aLevel, aIsChineseOrJapanese);
    }

    // To implement word-break:break-all, we overwrite the line-break class of
    // alphanumeric characters so they are treated the same as ideographic.
    // The relevant characters will have been assigned CLASS_CHARACTER, _CLOSE,
    // _CLOSE_LIKE_CHARACTER, or _NUMERIC by GetClass(), but those classes also
    // include others that we don't want to touch here, so we re-check the
    // Unicode line-break class to determine which ones to modify.
    if (aWordBreak == WordBreak::BreakAll &&
        (cl == CLASS_CHARACTER || cl == CLASS_CLOSE ||
         cl == CLASS_CLOSE_LIKE_CHARACTER || cl == CLASS_NUMERIC)) {
      auto cls = GetLineBreakClass(ch);
      if (cls == U_LB_ALPHABETIC || cls == U_LB_NUMERIC ||
          cls == U_LB_AMBIGUOUS || cls == U_LB_COMPLEX_CONTEXT ||
          /* Additional Japanese and Korean LB classes; CSS Text spec doesn't
             explicitly mention these, but this appears to give expected
             behavior (spec issue?) */
          cls == U_LB_CONDITIONAL_JAPANESE_STARTER ||
          (cls >= U_LB_H2 && cls <= U_LB_JV)) {
        cl = CLASS_BREAKABLE;
      }
    }

    bool allowBreak = false;
    if (cur > 0) {
      NS_ASSERTION(CLASS_COMPLEX != lastClass || CLASS_COMPLEX != cl,
                   "Loop should have prevented adjacent complex chars here");
      allowBreak =
          (state.UseConservativeBreaking() ? GetPairConservative(lastClass, cl)
                                           : GetPair(lastClass, cl));
      // Special cases where a normally-allowed break is suppressed:
      if (allowBreak) {
        // word-break:keep-all suppresses breaks between certain line-break
        // classes.
        if (aWordBreak == WordBreak::KeepAll &&
            SuppressBreakForKeepAll(prev(), ch)) {
          allowBreak = false;
        }
        // We also don't allow a break within a run of U+3000 chars unless
        // word-break:break-all is in effect.
        if (ch == 0x3000 && prev() == 0x3000 &&
            aWordBreak != WordBreak::BreakAll) {
          allowBreak = false;
        }
      }
    }
    aBreakBefore[cur] = allowBreak;
    if (allowBreak) state.NotifyBreakBefore();
    lastClass = cl;
    if (CLASS_COMPLEX == cl) {
      uint32_t end = cur + chLen;

      while (end < aLength) {
        char32_t c = state.GetUnicodeCharAt(end);
        if (CLASS_COMPLEX != GetClass(c, aLevel, false)) {
          break;
        }
        ++end;
        if (c > 0xFFFFU) {  // it was a surrogate pair
          ++end;
        }
      }

      if (aWordBreak == WordBreak::BreakAll) {
        // For break-all, we don't need to run a dictionary-based breaking
        // algorithm, we just allow breaks between all grapheme clusters.
        ClusterIterator ci(aChars + cur, end - cur);
        while (!ci.AtEnd()) {
          ci.Next();
          aBreakBefore[ci - aChars] = true;
        }
      } else {
        NS_GetComplexLineBreaks(aChars + cur, end - cur, aBreakBefore + cur);
        // restore breakability at chunk begin, which was always set to false
        // by the complex line breaker
        aBreakBefore[cur] = allowBreak;
      }

      cur = end - 1;
    }

    if (chLen == 2) {
      // Supplementary-plane character: mark that we cannot break before the
      // trailing low surrogate, and advance past it.
      ++cur;
      aBreakBefore[cur] = false;
      state.AdvanceIndex();
    }
  }
}

void LineBreaker::GetJISx4051Breaks(const uint8_t* aChars, uint32_t aLength,
                                    WordBreak aWordBreak, Strictness aLevel,
                                    bool aIsChineseOrJapanese,
                                    uint8_t* aBreakBefore) {
  uint32_t cur;
  int8_t lastClass = CLASS_NONE;
  ContextState state(aChars, aLength);

  for (cur = 0; cur < aLength; ++cur, state.AdvanceIndex()) {
    char32_t ch = aChars[cur];
    int8_t cl;

    if (NEED_CONTEXTUAL_ANALYSIS(ch)) {
      cl = ContextualAnalysis(cur > 0 ? aChars[cur - 1] : U_NULL, ch,
                              cur + 1 < aLength ? aChars[cur + 1] : U_NULL,
                              state, aLevel, aIsChineseOrJapanese);
    } else {
      if (ch == U_EQUAL) state.NotifySeenEqualsSign();
      state.NotifyNonHyphenCharacter(ch);
      cl = GetClass(ch, aLevel, aIsChineseOrJapanese);
    }
    if (aWordBreak == WordBreak::BreakAll &&
        (cl == CLASS_CHARACTER || cl == CLASS_CLOSE ||
         cl == CLASS_CLOSE_LIKE_CHARACTER || cl == CLASS_NUMERIC)) {
      auto cls = GetLineBreakClass(ch);
      // Don't need to check additional Japanese/Korean classes in 8-bit
      if (cls == U_LB_ALPHABETIC || cls == U_LB_NUMERIC ||
          cls == U_LB_COMPLEX_CONTEXT) {
        cl = CLASS_BREAKABLE;
      }
    }

    bool allowBreak = false;
    if (cur > 0) {
      allowBreak =
          (state.UseConservativeBreaking() ? GetPairConservative(lastClass, cl)
                                           : GetPair(lastClass, cl)) &&
          (aWordBreak != WordBreak::KeepAll ||
           !SuppressBreakForKeepAll(aChars[cur - 1], ch));
    }
    aBreakBefore[cur] = allowBreak;
    if (allowBreak) state.NotifyBreakBefore();
    lastClass = cl;
  }
}
