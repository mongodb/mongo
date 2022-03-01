/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef mozilla_intl_LineBreaker_h__
#define mozilla_intl_LineBreaker_h__

#include "nscore.h"
#include "nsISupports.h"

#define NS_LINEBREAKER_NEED_MORE_TEXT -1

namespace mozilla {
namespace intl {

class LineBreaker {
 public:
  NS_INLINE_DECL_REFCOUNTING(LineBreaker)

  enum class WordBreak : uint8_t {
    Normal = 0,    // default
    BreakAll = 1,  // break all
    KeepAll = 2    // always keep
  };

  enum class Strictness : uint8_t {
    Auto = 0,
    Loose = 1,
    Normal = 2,
    Strict = 3,
    Anywhere = 4
  };

  static already_AddRefed<LineBreaker> Create();

  int32_t Next(const char16_t* aText, uint32_t aLen, uint32_t aPos);

  int32_t Prev(const char16_t* aText, uint32_t aLen, uint32_t aPos);

  // Call this on a word with whitespace at either end. We will apply JISx4051
  // rules to find breaks inside the word. aBreakBefore is set to the break-
  // before status of each character; aBreakBefore[0] will always be false
  // because we never return a break before the first character.
  // aLength is the length of the aText array and also the length of the
  // aBreakBefore output array.
  void GetJISx4051Breaks(const char16_t* aText, uint32_t aLength,
                         WordBreak aWordBreak, Strictness aLevel,
                         bool aIsChineseOrJapanese, uint8_t* aBreakBefore);
  void GetJISx4051Breaks(const uint8_t* aText, uint32_t aLength,
                         WordBreak aWordBreak, Strictness aLevel,
                         bool aIsChineseOrJapanese, uint8_t* aBreakBefore);

 private:
  ~LineBreaker() = default;

  int32_t WordMove(const char16_t* aText, uint32_t aLen, uint32_t aPos,
                   int8_t aDirection);
};

static inline bool NS_IsSpace(char16_t u) {
  return u == 0x0020 ||                   // SPACE
         u == 0x0009 ||                   // CHARACTER TABULATION
         u == 0x000D ||                   // CARRIAGE RETURN
         (0x2000 <= u && u <= 0x2006) ||  // EN QUAD, EM QUAD, EN SPACE,
                                          // EM SPACE, THREE-PER-EM SPACE,
                                          // FOUR-PER-SPACE, SIX-PER-EM SPACE,
         (0x2008 <= u && u <= 0x200B) ||  // PUNCTUATION SPACE, THIN SPACE,
                                          // HAIR SPACE, ZERO WIDTH SPACE
         u == 0x1361 ||                   // ETHIOPIC WORDSPACE
         u == 0x1680 ||                   // OGHAM SPACE MARK
         u == 0x205F;                     // MEDIUM MATHEMATICAL SPACE
}

static inline bool NS_NeedsPlatformNativeHandling(char16_t aChar) {
  return
#if ANDROID  // Bug 1647377: no "platform native" support for Tibetan;
             // better to just use our class-based breaker.
      (0x0e01 <= aChar && aChar <= 0x0eff) ||  // Thai, Lao
#else
      (0x0e01 <= aChar && aChar <= 0x0fff) ||  // Thai, Lao, Tibetan
#endif
      (0x1780 <= aChar && aChar <= 0x17ff);  // Khmer
}

}  // namespace intl
}  // namespace mozilla

#endif /* mozilla_intl_LineBreaker_h__ */
