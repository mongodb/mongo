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
      clasp == &PlainObject::class_ && !id.isAtom(cx->names().proto_)) {
    flags.setFlag(ObjectFlag::HasNonWritableOrAccessorPropExclProto);
  }

  // https://tc39.es/ecma262/multipage/ordinary-and-exotic-objects-behaviours.html#sec-proxy-object-internal-methods-and-internal-slots-get-p-receiver
  // Proxy.[[Get]] or [[Set]] Step 9
  if (!propFlags.configurable()) {
    MOZ_ASSERT(clasp->isNativeObject());
    // NOTE: there is a hole which this flag does not cover, which is if the
    // class has a resolve hook which could lazily define a non-configurable
    // non-writable property. We can just look this up directly though in the
    // JIT.
    if (propFlags.isDataProperty() && !propFlags.writable()) {
      flags.setFlag(ObjectFlag::NeedsProxyGetSetResultValidation);
    } else if (propFlags.isAccessorProperty()) {
      // This will cover us for both get trap validation and set trap
      // validation. We could be more aggressive, because what we really
      // care about is if there is a getter but not a setter and vice
      // versa, but the first pass at doing that resulted in test
      // failures. We'll need to work on that as a follow-up if it is
      // important.
      flags.setFlag(ObjectFlag::NeedsProxyGetSetResultValidation);
    }
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
  if (source.hasFlag(ObjectFlag::NeedsProxyGetSetResultValidation)) {
    dest.setFlag(ObjectFlag::NeedsProxyGetSetResultValidation);
  }
  return dest;
}

}  // namespace js

#endif /* vm_ObjectFlags_inl_h */
