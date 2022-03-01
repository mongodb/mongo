/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsComplexBreaker.h"

#include <pango/pango-break.h>
#include "nsUTF8Utils.h"
#include "nsString.h"
#include "nsTArray.h"

void NS_GetComplexLineBreaks(const char16_t* aText, uint32_t aLength,
                             uint8_t* aBreakBefore) {
  NS_ASSERTION(aText, "aText shouldn't be null");

  memset(aBreakBefore, false, aLength * sizeof(uint8_t));

  AutoTArray<PangoLogAttr, 2000> attrBuffer;
  // XXX(Bug 1631371) Check if this should use a fallible operation as it
  // pretended earlier.
  attrBuffer.AppendElements(aLength + 1);

  NS_ConvertUTF16toUTF8 aUTF8(aText, aLength);

  const gchar* p = aUTF8.Data();
  const gchar* end = p + aUTF8.Length();
  uint32_t u16Offset = 0;

  static PangoLanguage* language = pango_language_from_string("en");

  while (p < end) {
    PangoLogAttr* attr = attrBuffer.Elements();
    pango_get_log_attrs(p, end - p, -1, language, attr, attrBuffer.Length());

    while (p < end) {
      aBreakBefore[u16Offset] = attr->is_line_break;
      if (NS_IS_LOW_SURROGATE(aText[u16Offset]))
        aBreakBefore[++u16Offset] = false;  // Skip high surrogate
      ++u16Offset;

      // We're iterating over text obtained from NS_ConvertUTF16toUTF8,
      // so we know we have valid UTF-8 and don't need to check for
      // errors.
      uint32_t ch = UTF8CharEnumerator::NextChar(&p, end);
      ++attr;

      if (!ch) {
        // pango_break (pango 1.16.2) only analyses text before the
        // first NUL (but sets one extra attr). Workaround loop to call
        // pango_break again to analyse after the NUL is done somewhere else
        // (gfx/thebes/gfxFontconfigFonts.cpp: SetupClusterBoundaries()).
        // So, we do the same here for pango_get_log_attrs.
        break;
      }
    }
  }
}
