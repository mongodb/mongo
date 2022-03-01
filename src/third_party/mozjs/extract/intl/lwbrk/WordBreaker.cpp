/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/intl/WordBreaker.h"
#include "mozilla/StaticPrefs_layout.h"
#include "nsComplexBreaker.h"
#include "nsUnicodeProperties.h"

using mozilla::intl::WordBreakClass;
using mozilla::intl::WordBreaker;
using mozilla::intl::WordRange;
using mozilla::unicode::GetScriptCode;

/*static*/
already_AddRefed<WordBreaker> WordBreaker::Create() {
  return RefPtr<WordBreaker>(new WordBreaker()).forget();
}

bool WordBreaker::BreakInBetween(const char16_t* aText1, uint32_t aTextLen1,
                                 const char16_t* aText2, uint32_t aTextLen2) {
  MOZ_ASSERT(nullptr != aText1, "null ptr");
  MOZ_ASSERT(nullptr != aText2, "null ptr");

  if (!aText1 || !aText2 || (0 == aTextLen1) || (0 == aTextLen2)) return false;

  uint8_t c1 = GetClass(aText1[aTextLen1 - 1]);
  uint8_t c2 = GetClass(aText2[0]);

  if (c1 == c2 && kWbClassScriptioContinua == c1) {
    nsAutoString text(aText1, aTextLen1);
    text.Append(aText2, aTextLen2);
    AutoTArray<uint8_t, 256> breakBefore;
    breakBefore.SetLength(aTextLen1 + aTextLen2);
    NS_GetComplexLineBreaks(text.get(), text.Length(), breakBefore.Elements());
    bool ret = breakBefore[aTextLen1];
    return ret;
  }

  return (c1 != c2);
}

#define IS_ASCII(c) (0 == (0xFF80 & (c)))
#define ASCII_IS_ALPHA(c) \
  ((('a' <= (c)) && ((c) <= 'z')) || (('A' <= (c)) && ((c) <= 'Z')))
#define ASCII_IS_DIGIT(c) (('0' <= (c)) && ((c) <= '9'))
#define ASCII_IS_SPACE(c) \
  ((' ' == (c)) || ('\t' == (c)) || ('\r' == (c)) || ('\n' == (c)))
#define IS_ALPHABETICAL_SCRIPT(c) ((c) < 0x2E80)

// we change the beginning of IS_HAN from 0x4e00 to 0x3400 to relfect
// Unicode 3.0
#define IS_HAN(c) \
  ((0x3400 <= (c)) && ((c) <= 0x9fff)) || ((0xf900 <= (c)) && ((c) <= 0xfaff))
#define IS_KATAKANA(c) ((0x30A0 <= (c)) && ((c) <= 0x30FF))
#define IS_HIRAGANA(c) ((0x3040 <= (c)) && ((c) <= 0x309F))
#define IS_HALFWIDTHKATAKANA(c) ((0xFF60 <= (c)) && ((c) <= 0xFF9F))

// Return true if aChar belongs to a SEAsian script that is written without
// word spaces, so we need to use the "complex breaker" to find possible word
// boundaries. (https://en.wikipedia.org/wiki/Scriptio_continua)
// (How well this works depends on the level of platform support for finding
// possible line breaks - or possible word boundaries - in the particular
// script. Thai, at least, works pretty well on the major desktop OSes. If
// the script is not supported by the platform, we just won't find any useful
// boundaries.)
static bool IsScriptioContinua(char16_t aChar) {
  Script sc = GetScriptCode(aChar);
  return sc == Script::THAI || sc == Script::MYANMAR || sc == Script::KHMER ||
         sc == Script::JAVANESE || sc == Script::BALINESE ||
         sc == Script::SUNDANESE || sc == Script::LAO;
}

/* static */
WordBreakClass WordBreaker::GetClass(char16_t c) {
  // begin of the hack

  if (IS_ALPHABETICAL_SCRIPT(c)) {
    if (IS_ASCII(c)) {
      if (ASCII_IS_SPACE(c)) {
        return kWbClassSpace;
      }
      if (ASCII_IS_ALPHA(c) || ASCII_IS_DIGIT(c) ||
          (c == '_' && !StaticPrefs::layout_word_select_stop_at_underscore())) {
        return kWbClassAlphaLetter;
      }
      return kWbClassPunct;
    }
    if (c == 0x00A0 /*NBSP*/) {
      return kWbClassSpace;
    }
    if (GetGenCategory(c) == nsUGenCategory::kPunctuation) {
      return kWbClassPunct;
    }
    if (IsScriptioContinua(c)) {
      return kWbClassScriptioContinua;
    }
    return kWbClassAlphaLetter;
  }
  if (IS_HAN(c)) {
    return kWbClassHanLetter;
  }
  if (IS_KATAKANA(c)) {
    return kWbClassKatakanaLetter;
  }
  if (IS_HIRAGANA(c)) {
    return kWbClassHiraganaLetter;
  }
  if (IS_HALFWIDTHKATAKANA(c)) {
    return kWbClassHWKatakanaLetter;
  }
  if (GetGenCategory(c) == nsUGenCategory::kPunctuation) {
    return kWbClassPunct;
  }
  if (IsScriptioContinua(c)) {
    return kWbClassScriptioContinua;
  }
  return kWbClassAlphaLetter;
}

WordRange WordBreaker::FindWord(const char16_t* aText, uint32_t aTextLen,
                                uint32_t aOffset) {
  WordRange range;
  MOZ_ASSERT(nullptr != aText, "null ptr");
  MOZ_ASSERT(0 != aTextLen, "len = 0");
  MOZ_ASSERT(aOffset <= aTextLen, "aOffset > aTextLen");

  range.mBegin = aTextLen + 1;
  range.mEnd = aTextLen + 1;

  if (!aText || aOffset > aTextLen) return range;

  WordBreakClass c = GetClass(aText[aOffset]);
  uint32_t i;
  // Scan forward
  range.mEnd--;
  for (i = aOffset + 1; i <= aTextLen; i++) {
    if (c != GetClass(aText[i])) {
      range.mEnd = i;
      break;
    }
  }

  // Scan backward
  range.mBegin = 0;
  for (i = aOffset; i > 0; i--) {
    if (c != GetClass(aText[i - 1])) {
      range.mBegin = i;
      break;
    }
  }

  if (kWbClassScriptioContinua == c) {
    // we pass the whole text segment to the complex word breaker to find a
    // shorter answer
    AutoTArray<uint8_t, 256> breakBefore;
    breakBefore.SetLength(range.mEnd - range.mBegin);
    NS_GetComplexLineBreaks(aText + range.mBegin, range.mEnd - range.mBegin,
                            breakBefore.Elements());

    // Scan forward
    for (i = aOffset + 1; i < range.mEnd; i++) {
      if (breakBefore[i - range.mBegin]) {
        range.mEnd = i;
        break;
      }
    }

    // Scan backward
    for (i = aOffset; i > range.mBegin; i--) {
      if (breakBefore[i - range.mBegin]) {
        range.mBegin = i;
        break;
      }
    }
  }
  return range;
}

int32_t WordBreaker::NextWord(const char16_t* aText, uint32_t aLen,
                              uint32_t aPos) {
  WordBreakClass c1, c2;
  uint32_t cur = aPos;
  if (cur == aLen) {
    return NS_WORDBREAKER_NEED_MORE_TEXT;
  }
  c1 = GetClass(aText[cur]);

  for (cur++; cur < aLen; cur++) {
    c2 = GetClass(aText[cur]);
    if (c2 != c1) {
      break;
    }
  }

  if (kWbClassScriptioContinua == c1) {
    // we pass the whole text segment to the complex word breaker to find a
    // shorter answer
    AutoTArray<uint8_t, 256> breakBefore;
    breakBefore.SetLength(aLen - aPos);
    NS_GetComplexLineBreaks(aText + aPos, aLen - aPos, breakBefore.Elements());
    uint32_t i = 1;
    while (i < cur - aPos && !breakBefore[i]) {
      i++;
    }
    if (i < cur - aPos) {
      return aPos + i;
    }
  }

  if (cur == aLen) {
    return NS_WORDBREAKER_NEED_MORE_TEXT;
  }

  MOZ_ASSERT(cur != aPos);
  return cur;
}
