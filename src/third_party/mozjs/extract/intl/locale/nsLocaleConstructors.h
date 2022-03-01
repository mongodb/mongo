/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsLocaleConstructors_h__
#define nsLocaleConstructors_h__

#include "nsCollation.h"
#include "nsCollationCID.h"
#include "mozilla/ModuleUtils.h"
#include "LocaleService.h"
#include "OSPreferences.h"

#define NSLOCALE_MAKE_CTOR(ctor_, iface_, func_)                              \
  static nsresult ctor_(nsISupports* aOuter, REFNSIID aIID, void** aResult) { \
    *aResult = nullptr;                                                       \
    if (aOuter) return NS_ERROR_NO_AGGREGATION;                               \
    iface_* inst;                                                             \
    nsresult rv = func_(&inst);                                               \
    if (NS_SUCCEEDED(rv)) {                                                   \
      rv = inst->QueryInterface(aIID, aResult);                               \
      NS_RELEASE(inst);                                                       \
    }                                                                         \
    return rv;                                                                \
  }

NS_GENERIC_FACTORY_CONSTRUCTOR(nsCollationFactory)

namespace mozilla {
namespace intl {
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(LocaleService,
                                         LocaleService::GetInstanceAddRefed)
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(OSPreferences,
                                         OSPreferences::GetInstanceAddRefed)
}  // namespace intl
}  // namespace mozilla

#endif
