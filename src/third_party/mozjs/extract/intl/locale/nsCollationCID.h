/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsCollationCID_h__
#define nsCollationCID_h__

#include "nscore.h"
#include "nsISupports.h"

// {A1B72850-A999-11d2-9119-006008A6EDF6}
#define NS_COLLATIONFACTORY_CID                    \
  {                                                \
    0xa1b72850, 0xa999, 0x11d2, {                  \
      0x91, 0x19, 0x0, 0x60, 0x8, 0xa6, 0xed, 0xf6 \
    }                                              \
  }

#define NS_COLLATIONFACTORY_CONTRACTID "@mozilla.org/intl/collation-factory;1"

#endif  // nsCollationCID_h__
