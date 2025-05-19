/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_StringObject_inl_h
#define vm_StringObject_inl_h

#include "vm/StringObject.h"

#include "vm/JSObject-inl.h"
#include "vm/Shape-inl.h"

namespace js {

/* static */ inline bool StringObject::init(JSContext* cx,
                                            Handle<StringObject*> obj,
                                            HandleString str) {
  MOZ_ASSERT(obj->numFixedSlots() == 2);

  if (!SharedShape::ensureInitialCustomShape<StringObject>(cx, obj)) {
    return false;
  }

  MOZ_ASSERT(obj->lookup(cx, NameToId(cx->names().length))->slot() ==
             LENGTH_SLOT);

  obj->setStringThis(str);

  return true;
}

/* static */ inline StringObject* StringObject::create(JSContext* cx,
                                                       HandleString str,
                                                       HandleObject proto,
                                                       NewObjectKind newKind) {
  Rooted<StringObject*> obj(
      cx, NewObjectWithClassProtoAndKind<StringObject>(
              cx, proto, newKind,
              ObjectFlags({ObjectFlag::NeedsProxyGetSetResultValidation})));
  if (!obj) {
    return nullptr;
  }
  if (!StringObject::init(cx, obj, str)) {
    return nullptr;
  }
  return obj;
}

}  // namespace js

#endif /* vm_StringObject_inl_h */
