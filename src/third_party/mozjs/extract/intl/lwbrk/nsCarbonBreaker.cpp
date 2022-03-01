/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include "nsDebug.h"
#include "nscore.h"

void NS_GetComplexLineBreaks(const char16_t* aText, uint32_t aLength,
                             uint8_t* aBreakBefore) {
  NS_ASSERTION(aText, "aText shouldn't be null");

  memset(aBreakBefore, 0, aLength * sizeof(uint8_t));

  CFStringRef str = ::CFStringCreateWithCharactersNoCopy(
      kCFAllocatorDefault, reinterpret_cast<const UniChar*>(aText), aLength,
      kCFAllocatorNull);
  if (!str) {
    return;
  }

  CFStringTokenizerRef st = ::CFStringTokenizerCreate(
      kCFAllocatorDefault, str, ::CFRangeMake(0, aLength),
      kCFStringTokenizerUnitLineBreak, nullptr);
  if (!st) {
    ::CFRelease(str);
    return;
  }

  CFStringTokenizerTokenType tt = ::CFStringTokenizerAdvanceToNextToken(st);
  while (tt != kCFStringTokenizerTokenNone) {
    CFRange r = ::CFStringTokenizerGetCurrentTokenRange(st);
    if (r.location != 0) {  // Ignore leading edge
      aBreakBefore[r.location] = true;
    }
    tt = CFStringTokenizerAdvanceToNextToken(st);
  }

  ::CFRelease(st);
  ::CFRelease(str);
}
