/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_WeakMapObject_inl_h
#define builtin_WeakMapObject_inl_h

#include "builtin/WeakMapObject.h"

#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Wrapper.h"
#include "gc/WeakMap-inl.h"
#include "vm/JSObject-inl.h"

namespace js {

static bool TryPreserveReflector(JSContext* cx, HandleObject obj) {
  if (!MaybePreserveDOMWrapper(cx, obj)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_WEAKMAP_KEY);
    return false;
  }

  return true;
}

static MOZ_ALWAYS_INLINE bool WeakCollectionPutEntryInternal(
    JSContext* cx, Handle<WeakCollectionObject*> obj, HandleObject key,
    HandleValue value) {
  ObjectValueWeakMap* map = obj->getMap();
  if (!map) {
    auto newMap = cx->make_unique<ObjectValueWeakMap>(cx, obj.get());
    if (!newMap) {
      return false;
    }
    map = newMap.release();
    InitReservedSlot(obj, WeakCollectionObject::DataSlot, map,
                     MemoryUse::WeakMapObject);
  }

  // Preserve wrapped native keys to prevent wrapper optimization.
  if (!TryPreserveReflector(cx, key)) {
    return false;
  }

  RootedObject delegate(cx, UncheckedUnwrapWithoutExpose(key));
  if (delegate && !TryPreserveReflector(cx, delegate)) {
    return false;
  }

  MOZ_ASSERT(key->compartment() == obj->compartment());
  MOZ_ASSERT_IF(value.isObject(),
                value.toObject().compartment() == obj->compartment());
  if (!map->put(key, value)) {
    JS_ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

}  // namespace js

#endif /* builtin_WeakMapObject_inl_h */
