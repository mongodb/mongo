/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ObjectFlags_inl_h
#define vm_ObjectFlags_inl_h

#include "vm/ObjectFlags.h"

#include "builtin/Array.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"
#include "vm/PropertyInfo.h"

namespace js {

MOZ_ALWAYS_INLINE ObjectFlags
GetObjectFlagsForNewProperty(const JSClass* clasp, ObjectFlags flags, jsid id,
                             PropertyFlags propFlags, JSContext* cx) {
  uint32_t index;
  if (IdIsIndex(id, &index)) {
    flags.setFlag(ObjectFlag::Indexed);
  } else if (id.isSymbol() && id.toSymbol()->isInterestingSymbol()) {
    flags.setFlag(ObjectFlag::HasInterestingSymbol);
  }

  if ((!propFlags.isDataProperty() || !propFlags.writable()) &&
      clasp == &PlainObject::class_ && !id.isAtom(cx->names().proto)) {
    flags.setFlag(ObjectFlag::HasNonWritableOrAccessorPropExclProto);
  }

  if (propFlags.enumerable()) {
    flags.setFlag(ObjectFlag::HasEnumerable);
  }

  return flags;
}

// When reusing another shape's PropMap, we need to copy the object flags that
// are based on property information. This is equivalent to (but faster than)
// calling GetObjectFlagsForNewProperty for all properties in the map.
inline ObjectFlags CopyPropMapObjectFlags(ObjectFlags dest,
                                          ObjectFlags source) {
  if (source.hasFlag(ObjectFlag::Indexed)) {
    dest.setFlag(ObjectFlag::Indexed);
  }
  if (source.hasFlag(ObjectFlag::HasInterestingSymbol)) {
    dest.setFlag(ObjectFlag::HasInterestingSymbol);
  }
  if (source.hasFlag(ObjectFlag::HasNonWritableOrAccessorPropExclProto)) {
    dest.setFlag(ObjectFlag::HasNonWritableOrAccessorPropExclProto);
  }
  return dest;
}

}  // namespace js

#endif /* vm_ObjectFlags_inl_h */
