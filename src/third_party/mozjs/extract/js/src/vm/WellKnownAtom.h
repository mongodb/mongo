/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_WellKnownAtom_h
#define vm_WellKnownAtom_h

#include "mozilla/HashFunctions.h"  // mozilla::HashNumber, mozilla::HashStringKnownLength

#include <stdint.h>  // uint32_t

#include "js/ProtoKey.h"             // JS_FOR_EACH_PROTOTYPE
#include "js/Symbol.h"               // JS_FOR_EACH_WELL_KNOWN_SYMBOL
#include "vm/CommonPropertyNames.h"  // FOR_EACH_COMMON_PROPERTYNAME

namespace js {

// An index for well-known atoms.
//
// GetWellKnownAtom in ParserAtom.cpp relies on the fact that
// JSAtomState fields and this enum variants use the same order.
enum class WellKnownAtomId : uint32_t {
#define ENUM_ENTRY_(NAME, _) NAME,
  FOR_EACH_COMMON_PROPERTYNAME(ENUM_ENTRY_)
#undef ENUM_ENTRY_

#define ENUM_ENTRY_(NAME, _) NAME,
      JS_FOR_EACH_PROTOTYPE(ENUM_ENTRY_)
#undef ENUM_ENTRY_

#define ENUM_ENTRY_(NAME) NAME,
          JS_FOR_EACH_WELL_KNOWN_SYMBOL(ENUM_ENTRY_)
#undef ENUM_ENTRY_

              Limit,
};

struct WellKnownAtomInfo {
  uint32_t length;
  mozilla::HashNumber hash;
  const char* content;
};

extern WellKnownAtomInfo wellKnownAtomInfos[];

inline const WellKnownAtomInfo& GetWellKnownAtomInfo(WellKnownAtomId atomId) {
  return wellKnownAtomInfos[uint32_t(atomId)];
}

} /* namespace js */

#endif  // vm_WellKnownAtom_h
