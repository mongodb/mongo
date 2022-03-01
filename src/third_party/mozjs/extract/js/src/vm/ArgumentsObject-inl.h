/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArgumentsObject_inl_h
#define vm_ArgumentsObject_inl_h

#include "vm/ArgumentsObject.h"

#include "vm/EnvironmentObject.h"

#include "vm/EnvironmentObject-inl.h"
#include "vm/JSScript-inl.h"

namespace js {

inline const Value& ArgumentsObject::element(uint32_t i) const {
  MOZ_ASSERT(!isElementDeleted(i));
  const Value& v = data()->args[i];
  if (IsMagicScopeSlotValue(v)) {
    CallObject& callobj =
        getFixedSlot(MAYBE_CALL_SLOT).toObject().as<CallObject>();
    return callobj.aliasedFormalFromArguments(v);
  }
  return v;
}

inline void ArgumentsObject::setElement(uint32_t i, const Value& v) {
  MOZ_ASSERT(!isElementDeleted(i));
  GCPtrValue& lhs = data()->args[i];
  if (IsMagicScopeSlotValue(lhs)) {
    uint32_t slot = SlotFromMagicScopeSlotValue(lhs);
    CallObject& callobj =
        getFixedSlot(MAYBE_CALL_SLOT).toObject().as<CallObject>();
    for (ShapePropertyIter<NoGC> iter(callobj.shape()); !iter.done(); iter++) {
      if (iter->slot() == slot) {
        callobj.setAliasedFormalFromArguments(lhs, v);
        return;
      }
    }
    MOZ_CRASH("Bad Arguments::setElement");
  }
  lhs = v;
}

inline bool ArgumentsObject::maybeGetElements(uint32_t start, uint32_t count,
                                              Value* vp) {
  MOZ_ASSERT(start + count >= start);

  uint32_t length = initialLength();
  if (start > length || start + count > length || isAnyElementDeleted()) {
    return false;
  }

  for (uint32_t i = start, end = start + count; i < end; ++i, ++vp) {
    *vp = element(i);
  }
  return true;
}

} /* namespace js */

#endif /* vm_ArgumentsObject_inl_h */
