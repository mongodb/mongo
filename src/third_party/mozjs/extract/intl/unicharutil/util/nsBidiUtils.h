/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsBidiUtils_h__
#define nsBidiUtils_h__

#include "nsString.h"
#include "encoding_rs_mem.h"

/**
 *  Read ftp://ftp.unicode.org/Public/UNIDATA/ReadMe-Latest.txt
 *  section BIDIRECTIONAL PROPERTIES
 *  for the detailed definition of the following categories
 *
 *  The values here must match the equivalents in %bidicategorycode in
 *  mozilla/intl/unicharutil/tools/genUnicodePropertyData.pl,
 *  and must also match the values used by ICU's UCharDirection.
 */

enum nsCharType {
  eCharType_LeftToRight = 0,
  eCharType_RightToLeft = 1,
  eCharType_EuropeanNumber = 2,
  eCharType_EuropeanNumberSeparator = 3,
  eCharType_EuropeanNumberTerminator = 4,
  eCharType_ArabicNumber = 5,
  eCharType_CommonNumberSeparator = 6,
  eCharType_BlockSeparator = 7,
  eCharType_SegmentSeparator = 8,
  eCharType_WhiteSpaceNeutral = 9,
  eCharType_OtherNeutral = 10,
  eCharType_LeftToRightEmbedding = 11,
  eCharType_LeftToRightOverride = 12,
  eCharType_RightToLeftArabic = 13,
  eCharType_RightToLeftEmbedding = 14,
  eCharType_RightToLeftOverride = 15,
  eCharType_PopDirectionalFormat = 16,
  eCharType_DirNonSpacingMark = 17,
  eCharType_BoundaryNeutral = 18,
  eCharType_FirstStrongIsolate = 19,
  eCharType_LeftToRightIsolate = 20,
  eCharType_RightToLeftIsolate = 21,
  eCharType_PopDirectionalIsolate = 22,
  eCharType_CharTypeCount
};

/**
 * This specifies the language directional property of a character set.
 */
typedef enum nsCharType nsCharType;

/**
 * Find the direction of an embedding level or paragraph level set by
 * the Unicode Bidi Algorithm. (Even levels are left-to-right, odd
 * levels right-to-left.
 */
#define IS_LEVEL_RTL(level) (((level)&1) == 1)

/**
 * Check whether two bidi levels have the same parity and thus the same
 * directionality
 */
#define IS_SAME_DIRECTION(level1, level2) (((level1 ^ level2) & 1) == 0)

/**
 * Convert from nsBidiLevel to nsBidiDirection
 */
#define DIRECTION_FROM_LEVEL(level) \
  ((IS_LEVEL_RTL(level)) ? NSBIDI_RTL : NSBIDI_LTR)

/**
 * definitions of bidirection character types by category
 */

#define CHARTYPE_IS_RTL(val) \
  (((val) == eCharType_RightToLeft) || ((val) == eCharType_RightToLeftArabic))

#define CHARTYPE_IS_WEAK(val)                       \
  (((val) == eCharType_EuropeanNumberSeparator) ||  \
   ((val) == eCharType_EuropeanNumberTerminator) || \
   (((val) > eCharType_ArabicNumber) &&             \
    ((val) != eCharType_RightToLeftArabic)))

/**
 * Inspects a Unichar, converting numbers to Arabic or Hindi forms and
 * returning them
 * @param aChar is the character
 * @param aPrevCharArabic is true if the previous character in the string is
 *        an Arabic char
 * @param aNumFlag specifies the conversion to perform:
 *        IBMBIDI_NUMERAL_NOMINAL:      don't do any conversion
 *        IBMBIDI_NUMERAL_HINDI:        convert to Hindi forms
 *                                        (Unicode 0660-0669)
 *        IBMBIDI_NUMERAL_ARABIC:       convert to Arabic forms
 *                                        (Unicode 0030-0039)
 *        IBMBIDI_NUMERAL_HINDICONTEXT: convert numbers in Arabic text to
 *                                      Hindi, otherwise to Arabic
 * @return the converted Unichar
 */
char16_t HandleNumberInChar(char16_t aChar, bool aPrevCharArabic,
                            uint32_t aNumFlag);

/**
 * Scan a Unichar string, converting numbers to Arabic or Hindi forms in
 * place
 * @param aBuffer is the string
 * @param aSize is the size of aBuffer
 * @param aNumFlag specifies the conversion to perform:
 *        IBMBIDI_NUMERAL_NOMINAL:      don't do any conversion
 *        IBMBIDI_NUMERAL_HINDI:        convert to Hindi forms
 *                                        (Unicode 0660-0669)
 *        IBMBIDI_NUMERAL_ARABIC:       convert to Arabic forms
 *                                        (Unicode 0030-0039)
 *        IBMBIDI_NUMERAL_HINDICONTEXT: convert numbers in Arabic text to
 *                                      Hindi, otherwise to Arabic
 */
nsresult HandleNumbers(char16_t* aBuffer, uint32_t aSize, uint32_t aNumFlag);

/**
 * Give a UTF-32 codepoint
 * return true if the codepoint is a Bidi control character (LRM, RLM, ALM;
 * LRE, RLE, PDF, LRO, RLO; LRI, RLI, FSI, PDI).
 * Return false, otherwise
 */
#define LRM_CHAR 0x200e
#define RLM_CHAR 0x200f

#define LRE_CHAR 0x202a
#define RLE_CHAR 0x202b
#define PDF_CHAR 0x202c
#define LRO_CHAR 0x202d
#define RLO_CHAR 0x202e

#define LRI_CHAR 0x2066
#define RLI_CHAR 0x2067
#define FSI_CHAR 0x2068
#define PDI_CHAR 0x2069

#define ALM_CHAR 0x061C
inline bool IsBidiControl(uint32_t aChar) {
  return ((LRE_CHAR <= aChar && aChar <= RLO_CHAR) ||
          (LRI_CHAR <= aChar && aChar <= PDI_CHAR) || (aChar == ALM_CHAR) ||
          (aChar & 0xfffffe) == LRM_CHAR);
}

/**
 * Give a UTF-32 codepoint
 * Return true if the codepoint is a Bidi control character that may result
 * in RTL directionality and therefore needs to trigger bidi resolution;
 * return false otherwise.
 */
inline bool IsBidiControlRTL(uint32_t aChar) {
  return aChar == RLM_CHAR || aChar == RLE_CHAR || aChar == RLO_CHAR ||
         aChar == RLI_CHAR || aChar == ALM_CHAR;
}

/**
 * Give a 16-bit (UTF-16) text buffer
 * @return true if the string contains right-to-left characters
 */
inline bool HasRTLChars(mozilla::Span<const char16_t> aBuffer) {
  // Span ensures we never pass a nullptr to Rust--even if the
  // length of the buffer is zero.
  return encoding_mem_is_utf16_bidi(aBuffer.Elements(), aBuffer.Length());
}

// These values are shared with Preferences dialog
//  ------------------
//  If Pref values are to be changed
//  in the XUL file of Prefs. the values
//  Must be changed here too..
//  ------------------
//
#define IBMBIDI_TEXTDIRECTION_STR "bidi.direction"
#define IBMBIDI_TEXTTYPE_STR "bidi.texttype"
#define IBMBIDI_NUMERAL_STR "bidi.numeral"

//  ------------------
//  Text Direction
//  ------------------
//  bidi.direction
#define IBMBIDI_TEXTDIRECTION_LTR 1  //  1 = directionLTRBidi *
#define IBMBIDI_TEXTDIRECTION_RTL 2  //  2 = directionRTLBidi
//  ------------------
//  Text Type
//  ------------------
//  bidi.texttype
#define IBMBIDI_TEXTTYPE_CHARSET 1  //  1 = charsettexttypeBidi *
#define IBMBIDI_TEXTTYPE_LOGICAL 2  //  2 = logicaltexttypeBidi
#define IBMBIDI_TEXTTYPE_VISUAL 3   //  3 = visualtexttypeBidi
//  ------------------
//  Numeral Style
//  ------------------
//  bidi.numeral
#define IBMBIDI_NUMERAL_NOMINAL 0         //  0 = nominalnumeralBidi *
#define IBMBIDI_NUMERAL_REGULAR 1         //  1 = regularcontextnumeralBidi
#define IBMBIDI_NUMERAL_HINDICONTEXT 2    //  2 = hindicontextnumeralBidi
#define IBMBIDI_NUMERAL_ARABIC 3          //  3 = arabicnumeralBidi
#define IBMBIDI_NUMERAL_HINDI 4           //  4 = hindinumeralBidi
#define IBMBIDI_NUMERAL_PERSIANCONTEXT 5  // 5 = persiancontextnumeralBidi
#define IBMBIDI_NUMERAL_PERSIAN 6         //  6 = persiannumeralBidi

#define IBMBIDI_DEFAULT_BIDI_OPTIONS                                    \
  ((IBMBIDI_TEXTDIRECTION_LTR << 0) | (IBMBIDI_TEXTTYPE_CHARSET << 4) | \
   (IBMBIDI_NUMERAL_NOMINAL << 8))

#define GET_BIDI_OPTION_DIRECTION(bo) \
  (((bo) >> 0) & 0x0000000F) /* 4 bits for DIRECTION */
#define GET_BIDI_OPTION_TEXTTYPE(bo) \
  (((bo) >> 4) & 0x0000000F) /* 4 bits for TEXTTYPE */
#define GET_BIDI_OPTION_NUMERAL(bo) \
  (((bo) >> 8) & 0x0000000F) /* 4 bits for NUMERAL */

#define SET_BIDI_OPTION_DIRECTION(bo, dir) \
  { (bo) = ((bo)&0xFFFFFFF0) | (((dir)&0x0000000F) << 0); }
#define SET_BIDI_OPTION_TEXTTYPE(bo, tt) \
  { (bo) = ((bo)&0xFFFFFF0F) | (((tt)&0x0000000F) << 4); }
#define SET_BIDI_OPTION_NUMERAL(bo, num) \
  { (bo) = ((bo)&0xFFFFF0FF) | (((num)&0x0000000F) << 8); }

/* Constants related to the position of numerics in the codepage */
#define START_HINDI_DIGITS 0x0660
#define END_HINDI_DIGITS 0x0669
#define START_ARABIC_DIGITS 0x0030
#define END_ARABIC_DIGITS 0x0039
#define START_FARSI_DIGITS 0x06f0
#define END_FARSI_DIGITS 0x06f9
#define IS_HINDI_DIGIT(u) \
  (((u) >= START_HINDI_DIGITS) && ((u) <= END_HINDI_DIGITS))
#define IS_ARABIC_DIGIT(u) \
  (((u) >= START_ARABIC_DIGITS) && ((u) <= END_ARABIC_DIGITS))
#define IS_FARSI_DIGIT(u) \
  (((u) >= START_FARSI_DIGITS) && ((u) <= END_FARSI_DIGITS))
/**
 * Arabic numeric separator and numeric formatting characters:
 *  U+0600;ARABIC NUMBER SIGN
 *  U+0601;ARABIC SIGN SANAH
 *  U+0602;ARABIC FOOTNOTE MARKER
 *  U+0603;ARABIC SIGN SAFHA
 *  U+066A;ARABIC PERCENT SIGN
 *  U+066B;ARABIC DECIMAL SEPARATOR
 *  U+066C;ARABIC THOUSANDS SEPARATOR
 *  U+06DD;ARABIC END OF AYAH
 */
#define IS_ARABIC_SEPARATOR(u)                                                 \
  ((/*(u) >= 0x0600 &&*/ (u) <= 0x0603) || ((u) >= 0x066A && (u) <= 0x066C) || \
   ((u) == 0x06DD))

#define IS_BIDI_DIACRITIC(u)                                                 \
  (((u) >= 0x0591 && (u) <= 0x05A1) || ((u) >= 0x05A3 && (u) <= 0x05B9) ||   \
   ((u) >= 0x05BB && (u) <= 0x05BD) || ((u) == 0x05BF) || ((u) == 0x05C1) || \
   ((u) == 0x05C2) || ((u) == 0x05C4) || ((u) >= 0x064B && (u) <= 0x0652) || \
   ((u) == 0x0670) || ((u) >= 0x06D7 && (u) <= 0x06E4) || ((u) == 0x06E7) || \
   ((u) == 0x06E8) || ((u) >= 0x06EA && (u) <= 0x06ED))

#define IS_HEBREW_CHAR(c) \
  (((0x0590 <= (c)) && ((c) <= 0x05FF)) || (((c) >= 0xfb1d) && ((c) <= 0xfb4f)))
#define IS_ARABIC_CHAR(c)              \
  ((0x0600 <= (c) && (c) <= 0x08FF) && \
   ((c) <= 0x06ff || ((c) >= 0x0750 && (c) <= 0x077f) || (c) >= 0x08a0))
#define IS_ARABIC_ALPHABETIC(c) \
  (IS_ARABIC_CHAR(c) &&         \
   !(IS_HINDI_DIGIT(c) || IS_FARSI_DIGIT(c) || IS_ARABIC_SEPARATOR(c)))

/**
 * The codepoint ranges in the following macros are based on the blocks
 *  allocated, or planned to be allocated, to right-to-left characters in the
 *  BMP (Basic Multilingual Plane) and SMP (Supplementary Multilingual Plane)
 *  according to
 *  http://unicode.org/Public/UNIDATA/extracted/DerivedBidiClass.txt and
 *  http://www.unicode.org/roadmaps/
 */

#define IS_IN_BMP_RTL_BLOCK(c) ((0x590 <= (c)) && ((c) <= 0x8ff))
#define IS_RTL_PRESENTATION_FORM(c) \
  (((0xfb1d <= (c)) && ((c) <= 0xfdff)) || ((0xfe70 <= (c)) && ((c) <= 0xfefe)))
#define IS_IN_SMP_RTL_BLOCK(c)               \
  (((0x10800 <= (c)) && ((c) <= 0x10fff)) || \
   ((0x1e800 <= (c)) && ((c) <= 0x1eFFF)))
// Due to the supplementary-plane RTL blocks being identifiable from the
// high surrogate without examining the low surrogate, it is correct to
// use this by-code-unit check on potentially astral text without doing
// the math to decode surrogate pairs into code points. However, unpaired
// high surrogates that are RTL high surrogates then count as RTL even
// though, if replaced by the REPLACEMENT CHARACTER, it would not be
// RTL.
#define UTF16_CODE_UNIT_IS_BIDI(c)                              \
  ((IS_IN_BMP_RTL_BLOCK(c)) || (IS_RTL_PRESENTATION_FORM(c)) || \
   (c) == 0xD802 || (c) == 0xD803 || (c) == 0xD83A || (c) == 0xD83B)
#define UTF32_CHAR_IS_BIDI(c)                                   \
  ((IS_IN_BMP_RTL_BLOCK(c)) || (IS_RTL_PRESENTATION_FORM(c)) || \
   (IS_IN_SMP_RTL_BLOCK(c)))
#endif /* nsBidiUtils_h__ */
