/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_WeakMapObject_inl_h
#define builtin_WeakMapObject_inl_h

#include "builtin/WeakMapObject.h"

#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Prefs.h"
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
    JSContext* cx, Handle<WeakCollectionObject*> obj, HandleValue key,
    HandleValue value) {
  ValueValueWeakMap* map = obj->getMap();
  if (!map) {
    auto newMap = cx->make_unique<ValueValueWeakMap>(cx, obj.get());
    if (!newMap) {
      return false;
    }
    map = newMap.release();
    InitReservedSlot(obj, WeakCollectionObject::DataSlot, map,
                     MemoryUse::WeakMapObject);
  }

  if (key.isObject()) {
    RootedObject keyObj(cx, &key.toObject());

    // Preserve wrapped native keys to prevent wrapper optimization.
    if (!TryPreserveReflector(cx, keyObj)) {
      return false;
    }

    RootedObject delegate(cx, UncheckedUnwrapWithoutExpose(keyObj));
    if (delegate && !TryPreserveReflector(cx, delegate)) {
      return false;
    }
  }

  MOZ_ASSERT_IF(key.isObject(),
                key.toObject().compartment() == obj->compartment());
  MOZ_ASSERT_IF(value.isGCThing(),
                gc::ToMarkable(value)->zoneFromAnyThread() == obj->zone() ||
                    gc::ToMarkable(value)->zoneFromAnyThread()->isAtomsZone());
  MOZ_ASSERT_IF(value.isObject(),
                value.toObject().compartment() == obj->compartment());
  if (!map->put(key, value)) {
    JS_ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

// https://tc39.es/ecma262/#sec-canbeheldweakly
static MOZ_ALWAYS_INLINE bool CanBeHeldWeakly(JSContext* cx,
                                              HandleValue value) {
  // 1. If v is an Object, return true.
  if (value.isObject()) {
    return true;
  }

#ifdef NIGHTLY_BUILD
  bool symbolsAsWeakMapKeysEnabled =
      JS::Prefs::experimental_symbols_as_weakmap_keys();

  // 2. If v is a Symbol and KeyForSymbol(v) is undefined, return true.
  if (symbolsAsWeakMapKeysEnabled && value.isSymbol() &&
      value.toSymbol()->code() != JS::SymbolCode::InSymbolRegistry) {
    return true;
  }
#endif

  // 3. Return false.
  return false;
}

static unsigned GetErrorNumber(bool isWeakMap) {
#ifdef NIGHTLY_BUILD
  bool symbolsAsWeakMapKeysEnabled =
      JS::Prefs::experimental_symbols_as_weakmap_keys();

  if (symbolsAsWeakMapKeysEnabled) {
    return isWeakMap ? JSMSG_WEAKMAP_KEY_CANT_BE_HELD_WEAKLY
                     : JSMSG_WEAKSET_VAL_CANT_BE_HELD_WEAKLY;
  }
#endif

  return isWeakMap ? JSMSG_WEAKMAP_KEY_MUST_BE_AN_OBJECT
                   : JSMSG_WEAKSET_VAL_MUST_BE_AN_OBJECT;
}

}  // namespace js

#endif /* builtin_WeakMapObject_inl_h */
