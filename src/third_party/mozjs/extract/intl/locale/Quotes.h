/* -*- Mode: C++; tab-width: 2; indent-tabs-mode:nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_Quotes_h__
#define mozilla_intl_Quotes_h__

#include "nsAtom.h"

namespace mozilla {
namespace intl {

// Currently, all the quotation characters provided by CLDR are single BMP
// codepoints, so they fit into char16_t fields. If there are ever multi-
// character strings or non-BMP codepoints in a future version, we'll need
// to extend this to a larger/more flexible structure, but for now it's
// deliberately kept simple and lightweight.
struct Quotes {
  // Entries in order [open, close, alternativeOpen, alternativeClose]
  char16_t mChars[4];
};

/**
 * Return a pointer to the Quotes record for the given locale (lang attribute),
 * or nullptr if none available.
 * The returned value points to a hashtable entry, but will remain valid until
 * shutdown begins, as the table is not modified after initialization.
 */
const Quotes* QuotesForLang(const nsAtom* aLang);

}  // namespace intl
}  // namespace mozilla

#endif  // mozilla_intl_Quotes_h__
