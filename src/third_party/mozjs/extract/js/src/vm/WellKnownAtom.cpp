/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/WellKnownAtom.h"

#define DECLARE_CONST_CHAR_STR(IDPART, _, TEXT) char js_##IDPART##_str[] = TEXT;
FOR_EACH_COMMON_PROPERTYNAME(DECLARE_CONST_CHAR_STR)
#undef DECLARE_CONST_CHAR_STR

#define DECLARE_CONST_CHAR_STR(NAME, _) char js_##NAME##_str[] = #NAME;
JS_FOR_EACH_PROTOTYPE(DECLARE_CONST_CHAR_STR)
#undef DECLARE_CONST_CHAR_STR

#define DECLARE_CONST_CHAR_STR(NAME) char js_##NAME##_str[] = #NAME;
JS_FOR_EACH_WELL_KNOWN_SYMBOL(DECLARE_CONST_CHAR_STR)
#undef DECLARE_CONST_CHAR_STR

js::WellKnownAtomInfo js::wellKnownAtomInfos[] = {
#define ENUM_ENTRY_(IDPART, _, _2)                                \
  {uint32_t(sizeof(js_##IDPART##_str) - 1),                       \
   mozilla::HashStringKnownLength(js_##IDPART##_str,              \
                                  sizeof(js_##IDPART##_str) - 1), \
   js_##IDPART##_str},
    FOR_EACH_COMMON_PROPERTYNAME(ENUM_ENTRY_)
#undef ENUM_ENTRY_

#define ENUM_ENTRY_(NAME, _)                                    \
  {uint32_t(sizeof(js_##NAME##_str) - 1),                       \
   mozilla::HashStringKnownLength(js_##NAME##_str,              \
                                  sizeof(js_##NAME##_str) - 1), \
   js_##NAME##_str},
        JS_FOR_EACH_PROTOTYPE(ENUM_ENTRY_)
#undef ENUM_ENTRY_

#define ENUM_ENTRY_(NAME)                                       \
  {uint32_t(sizeof(js_##NAME##_str) - 1),                       \
   mozilla::HashStringKnownLength(js_##NAME##_str,              \
                                  sizeof(js_##NAME##_str) - 1), \
   js_##NAME##_str},
            JS_FOR_EACH_WELL_KNOWN_SYMBOL(ENUM_ENTRY_)
#undef ENUM_ENTRY_
};
