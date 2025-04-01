/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/WellKnownAtom.h"

js::WellKnownAtomInfo js::wellKnownAtomInfos[] = {
#define ENUM_ENTRY_(_, TEXT)   \
  {uint32_t(sizeof(TEXT) - 1), \
   mozilla::HashStringKnownLength(TEXT, sizeof(TEXT) - 1), TEXT},
    FOR_EACH_COMMON_PROPERTYNAME(ENUM_ENTRY_)
#undef ENUM_ENTRY_

#define ENUM_ENTRY_(NAME, _)    \
  {uint32_t(sizeof(#NAME) - 1), \
   mozilla::HashStringKnownLength(#NAME, sizeof(#NAME) - 1), #NAME},
        JS_FOR_EACH_PROTOTYPE(ENUM_ENTRY_)
#undef ENUM_ENTRY_

#define ENUM_ENTRY_(NAME)       \
  {uint32_t(sizeof(#NAME) - 1), \
   mozilla::HashStringKnownLength(#NAME, sizeof(#NAME) - 1), #NAME},
            JS_FOR_EACH_WELL_KNOWN_SYMBOL(ENUM_ENTRY_)
#undef ENUM_ENTRY_
};
