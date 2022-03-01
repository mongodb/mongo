/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=4 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NS_UNICODEPROPERTIES_H
#define NS_UNICODEPROPERTIES_H

#include "nsBidiUtils.h"
#include "nsUGenCategory.h"
#include "nsUnicodeScriptCodes.h"
#include "harfbuzz/hb.h"

#include "unicode/uchar.h"
#include "unicode/uscript.h"

const nsCharProps2& GetCharProps2(uint32_t aCh);

namespace mozilla {

namespace unicode {

extern const nsUGenCategory sDetailedToGeneralCategory[];

/* This MUST match the values assigned by genUnicodePropertyData.pl! */
enum VerticalOrientation {
  VERTICAL_ORIENTATION_U = 0,
  VERTICAL_ORIENTATION_R = 1,
  VERTICAL_ORIENTATION_Tu = 2,
  VERTICAL_ORIENTATION_Tr = 3
};

/* This MUST match the values assigned by genUnicodePropertyData.pl! */
enum PairedBracketType {
  PAIRED_BRACKET_TYPE_NONE = 0,
  PAIRED_BRACKET_TYPE_OPEN = 1,
  PAIRED_BRACKET_TYPE_CLOSE = 2
};

/* Flags for Unicode security IdentifierType.txt attributes. Only a subset
   of these are currently checked by Gecko, so we only define flags for the
   ones we need. */
enum IdentifierType {
  IDTYPE_RESTRICTED = 0,
  IDTYPE_ALLOWED = 1,
};

enum EmojiPresentation { TextOnly = 0, TextDefault = 1, EmojiDefault = 2 };

const uint32_t kVariationSelector15 = 0xFE0E;  // text presentation
const uint32_t kVariationSelector16 = 0xFE0F;  // emoji presentation

// Unicode values for EMOJI MODIFIER FITZPATRICK TYPE-*
const uint32_t kEmojiSkinToneFirst = 0x1f3fb;
const uint32_t kEmojiSkinToneLast = 0x1f3ff;

extern const hb_unicode_general_category_t sICUtoHBcategory[];

inline uint32_t GetMirroredChar(uint32_t aCh) { return u_charMirror(aCh); }

inline bool HasMirroredChar(uint32_t aCh) { return u_isMirrored(aCh); }

inline uint8_t GetCombiningClass(uint32_t aCh) {
  return u_getCombiningClass(aCh);
}

inline uint8_t GetGeneralCategory(uint32_t aCh) {
  return sICUtoHBcategory[u_charType(aCh)];
}

inline nsCharType GetBidiCat(uint32_t aCh) {
  return nsCharType(u_charDirection(aCh));
}

inline int8_t GetNumericValue(uint32_t aCh) {
  UNumericType type =
      UNumericType(u_getIntPropertyValue(aCh, UCHAR_NUMERIC_TYPE));
  return type == U_NT_DECIMAL || type == U_NT_DIGIT
             ? int8_t(u_getNumericValue(aCh))
             : -1;
}

inline uint8_t GetLineBreakClass(uint32_t aCh) {
  return u_getIntPropertyValue(aCh, UCHAR_LINE_BREAK);
}

inline Script GetScriptCode(uint32_t aCh) {
  UErrorCode err = U_ZERO_ERROR;
  return Script(uscript_getScript(aCh, &err));
}

inline bool HasScript(uint32_t aCh, Script aScript) {
  return uscript_hasScript(aCh, UScriptCode(aScript));
}

inline uint32_t GetScriptTagForCode(Script aScriptCode) {
  const char* tag = uscript_getShortName(UScriptCode(aScriptCode));
  if (tag) {
    return HB_TAG(tag[0], tag[1], tag[2], tag[3]);
  }
  // return UNKNOWN script tag (running with older ICU?)
  return HB_SCRIPT_UNKNOWN;
}

inline PairedBracketType GetPairedBracketType(uint32_t aCh) {
  return PairedBracketType(
      u_getIntPropertyValue(aCh, UCHAR_BIDI_PAIRED_BRACKET_TYPE));
}

inline uint32_t GetPairedBracket(uint32_t aCh) {
  return u_getBidiPairedBracket(aCh);
}

inline uint32_t GetUppercase(uint32_t aCh) { return u_toupper(aCh); }

inline uint32_t GetLowercase(uint32_t aCh) { return u_tolower(aCh); }

inline uint32_t GetTitlecaseForLower(
    uint32_t aCh)  // maps LC to titlecase, UC unchanged
{
  return u_isULowercase(aCh) ? u_totitle(aCh) : aCh;
}

inline uint32_t GetTitlecaseForAll(
    uint32_t aCh)  // maps both UC and LC to titlecase
{
  return u_totitle(aCh);
}

inline uint32_t GetFoldedcase(uint32_t aCh) {
  // Handle dotted capital I and dotless small i specially because we want to
  // use a combination of ordinary case-folding rules and Turkish case-folding
  // rules.
  if (aCh == 0x0130 || aCh == 0x0131) {
    return 'i';
  }
  return u_foldCase(aCh, U_FOLD_CASE_DEFAULT);
}

inline bool IsEastAsianWidthFHWexcludingEmoji(uint32_t aCh) {
  switch (u_getIntPropertyValue(aCh, UCHAR_EAST_ASIAN_WIDTH)) {
    case U_EA_FULLWIDTH:
    case U_EA_HALFWIDTH:
      return true;
    case U_EA_WIDE:
      return u_hasBinaryProperty(aCh, UCHAR_EMOJI) ? false : true;
    case U_EA_AMBIGUOUS:
    case U_EA_NARROW:
    case U_EA_NEUTRAL:
      return false;
  }
  return false;
}

inline bool IsEastAsianWidthAFW(uint32_t aCh) {
  switch (u_getIntPropertyValue(aCh, UCHAR_EAST_ASIAN_WIDTH)) {
    case U_EA_AMBIGUOUS:
    case U_EA_FULLWIDTH:
    case U_EA_WIDE:
      return true;
    case U_EA_HALFWIDTH:
    case U_EA_NARROW:
    case U_EA_NEUTRAL:
      return false;
  }
  return false;
}

inline bool IsDefaultIgnorable(uint32_t aCh) {
  return u_hasBinaryProperty(aCh, UCHAR_DEFAULT_IGNORABLE_CODE_POINT);
}

inline EmojiPresentation GetEmojiPresentation(uint32_t aCh) {
  if (!u_hasBinaryProperty(aCh, UCHAR_EMOJI)) {
    return TextOnly;
  }

  if (u_hasBinaryProperty(aCh, UCHAR_EMOJI_PRESENTATION)) {
    return EmojiDefault;
  }
  return TextDefault;
}

// returns the simplified Gen Category as defined in nsUGenCategory
inline nsUGenCategory GetGenCategory(uint32_t aCh) {
  return sDetailedToGeneralCategory[GetGeneralCategory(aCh)];
}

inline VerticalOrientation GetVerticalOrientation(uint32_t aCh) {
  return VerticalOrientation(GetCharProps2(aCh).mVertOrient);
}

inline IdentifierType GetIdentifierType(uint32_t aCh) {
  return IdentifierType(GetCharProps2(aCh).mIdType);
}

uint32_t GetFullWidth(uint32_t aCh);
// This is the reverse function of GetFullWidth which guarantees that
// for every codepoint c, GetFullWidthInverse(GetFullWidth(c)) == c.
// Note that, this function does not guarantee to convert all wide
// form characters to their possible narrow form.
uint32_t GetFullWidthInverse(uint32_t aCh);

bool IsClusterExtender(uint32_t aCh, uint8_t aCategory);

inline bool IsClusterExtender(uint32_t aCh) {
  return IsClusterExtender(aCh, GetGeneralCategory(aCh));
}

// A simple iterator for a string of char16_t codepoints that advances
// by Unicode grapheme clusters
class ClusterIterator {
 public:
  ClusterIterator(const char16_t* aText, uint32_t aLength)
      : mPos(aText),
        mLimit(aText + aLength)
#ifdef DEBUG
        ,
        mText(aText)
#endif
  {
  }

  operator const char16_t*() const { return mPos; }

  bool AtEnd() const { return mPos >= mLimit; }

  void Next();

 private:
  const char16_t* mPos;
  const char16_t* mLimit;
#ifdef DEBUG
  const char16_t* mText;
#endif
};

// Count the number of grapheme clusters in the given string
uint32_t CountGraphemeClusters(const char16_t* aText, uint32_t aLength);

// Determine whether a character is a "combining diacritic" for the purpose
// of diacritic-insensitive text search. Examples of such characters include
// European accents and Hebrew niqqud, but not Hangul components or Thaana
// vowels, even though Thaana vowels are combining nonspacing marks that could
// be considered diacritics.
// As an exception to strictly following Unicode properties, we exclude the
// Japanese kana voicing marks
//   3099;COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK;Mn;8;NSM
//   309A;COMBINING KATAKANA-HIRAGANA SEMI-VOICED SOUND MARK;Mn;8;NSM
// which users report should not be ignored (bug 1624244).
// Keep this function in sync with is_combining_diacritic in base_chars.py.
inline bool IsCombiningDiacritic(uint32_t aCh) {
  uint8_t cc = u_getCombiningClass(aCh);
  return cc != HB_UNICODE_COMBINING_CLASS_NOT_REORDERED &&
         cc != HB_UNICODE_COMBINING_CLASS_KANA_VOICING &&
         cc != HB_UNICODE_COMBINING_CLASS_VIRAMA && cc != 91 && cc != 129 &&
         cc != 130 && cc != 132;
}

// Keep this function in sync with is_math_symbol in base_chars.py.
inline bool IsMathOrMusicSymbol(uint32_t aCh) {
  return u_charType(aCh) == U_MATH_SYMBOL || u_charType(aCh) == U_OTHER_SYMBOL;
}

// Remove diacritics from a character
uint32_t GetNaked(uint32_t aCh);

// A simple reverse iterator for a string of char16_t codepoints that
// advances by Unicode grapheme clusters
class ClusterReverseIterator {
 public:
  ClusterReverseIterator(const char16_t* aText, uint32_t aLength)
      : mPos(aText + aLength), mLimit(aText) {}

  operator const char16_t*() const { return mPos; }

  bool AtEnd() const { return mPos <= mLimit; }

  void Next();

 private:
  const char16_t* mPos;
  const char16_t* mLimit;
};

}  // end namespace unicode

}  // end namespace mozilla

#endif /* NS_UNICODEPROPERTIES_H */
