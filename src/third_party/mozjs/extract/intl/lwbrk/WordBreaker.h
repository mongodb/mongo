/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef mozilla_intl_WordBreaker_h__
#define mozilla_intl_WordBreaker_h__

#include "nscore.h"
#include "nsISupports.h"

#define NS_WORDBREAKER_NEED_MORE_TEXT -1

namespace mozilla {
namespace intl {

typedef struct {
  uint32_t mBegin;
  uint32_t mEnd;
} WordRange;

enum WordBreakClass : uint8_t {
  kWbClassSpace = 0,
  kWbClassAlphaLetter,
  kWbClassPunct,
  kWbClassHanLetter,
  kWbClassKatakanaLetter,
  kWbClassHiraganaLetter,
  kWbClassHWKatakanaLetter,
  kWbClassScriptioContinua
};

class WordBreaker {
 public:
  NS_INLINE_DECL_REFCOUNTING(WordBreaker)

  static already_AddRefed<WordBreaker> Create();

  bool BreakInBetween(const char16_t* aText1, uint32_t aTextLen1,
                      const char16_t* aText2, uint32_t aTextLen2);
  WordRange FindWord(const char16_t* aText1, uint32_t aTextLen1,
                     uint32_t aOffset);
  int32_t NextWord(const char16_t* aText, uint32_t aLen, uint32_t aPos);

  static WordBreakClass GetClass(char16_t aChar);

 private:
  ~WordBreaker() = default;
};

}  // namespace intl
}  // namespace mozilla

#endif /* mozilla_intl_WordBreaker_h__ */
