/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsComplexBreaker.h"

#define TH_UNICODE
#include "rulebrk.h"

void NS_GetComplexLineBreaks(const char16_t* aText, uint32_t aLength,
                             uint8_t* aBreakBefore) {
  NS_ASSERTION(aText, "aText shouldn't be null");

  for (uint32_t i = 0; i < aLength; i++)
    aBreakBefore[i] = (0 == TrbWordBreakPos(aText, i, aText + i, aLength - i));
}
