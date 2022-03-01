
/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsCollationFactory_h__
#define nsCollationFactory_h__

#include "nsICollation.h"
#include "nsCOMPtr.h"
#include "mozilla/Attributes.h"

// Create a collation interface for the current app's locale.
//
class nsCollationFactory final : public nsICollationFactory {
  ~nsCollationFactory() = default;

 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSICOLLATIONFACTORY

  nsCollationFactory() = default;
};

#endif /* nsCollationFactory_h__ */
