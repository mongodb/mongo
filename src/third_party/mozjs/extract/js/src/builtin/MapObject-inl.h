/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_MapObject_inl_h
#define builtin_MapObject_inl_h

#include "builtin/MapObject.h"

#include "vm/JSObject.h"
#include "vm/NativeObject.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

namespace js {

enum class MapOrSet { Map, Set };

template <MapOrSet IsMapOrSet>
[[nodiscard]] static bool IsOptimizableArrayForMapOrSetCtor(JSObject* iterable,
                                                            JSContext* cx) {
  if (!IsArrayWithDefaultIterator<MustBePacked::Yes>(iterable, cx)) {
    return false;
  }

  // For the Map and WeakMap constructors, ensure the elements are also packed
  // arrays with at least two elements (key and value).
  //
  // Limit this to relatively short arrays to avoid adding overhead for large
  // arrays in the worst case, when this check fails for one of the last
  // elements.
  if constexpr (IsMapOrSet == MapOrSet::Map) {
    ArrayObject* array = &iterable->as<ArrayObject>();
    size_t len = array->length();
    static constexpr size_t MaxLength = 100;
    if (len > MaxLength) {
      return false;
    }
    for (size_t i = 0; i < len; i++) {
      Value elem = array->getDenseElement(i);
      if (!elem.isObject()) {
        return false;
      }
      JSObject* obj = &elem.toObject();
      if (!IsPackedArray(obj) || obj->as<ArrayObject>().length() < 2) {
        return false;
      }
    }
  }

  return true;
}

template <JSProtoKey ProtoKey>
[[nodiscard]] static bool CanOptimizeMapOrSetCtorWithIterable(
    JSNative addOrSetNative, NativeObject* mapOrSetObject, JSContext* cx) {
  constexpr bool isMap = ProtoKey == JSProto_Map || ProtoKey == JSProto_WeakMap;
  constexpr bool isSet = ProtoKey == JSProto_Set || ProtoKey == JSProto_WeakSet;
  static_assert(isMap != isSet, "must be either a Map or a Set");

  // Ensures mapOrSetObject's prototype is the canonical prototype.
  JSObject* proto = mapOrSetObject->staticPrototype();
  MOZ_ASSERT(proto);
  if (proto != cx->global()->maybeGetPrototype(ProtoKey)) {
    return false;
  }

  // Ensure the 'add' or 'set' method on the prototype is unchanged. Use a fast
  // path based on fuses.
  switch (ProtoKey) {
    case JSProto_Map:
      if (cx->realm()->realmFuses.optimizeMapPrototypeSetFuse.intact()) {
        return true;
      }
      break;
    case JSProto_Set:
      if (cx->realm()->realmFuses.optimizeSetPrototypeAddFuse.intact()) {
        return true;
      }
      break;
    case JSProto_WeakMap:
      if (cx->realm()->realmFuses.optimizeWeakMapPrototypeSetFuse.intact()) {
        return true;
      }
      break;
    case JSProto_WeakSet:
      if (cx->realm()->realmFuses.optimizeWeakSetPrototypeAddFuse.intact()) {
        return true;
      }
      break;
    default:
      MOZ_CRASH("Unexpected ProtoKey");
  }

  // Look up the 'add' or 'set' property on the prototype object.
  auto* nproto = &proto->as<NativeObject>();
  PropertyName* propName = isSet ? cx->names().add : cx->names().set;
  mozilla::Maybe<PropertyInfo> prop = nproto->lookup(cx, propName);
  if (prop.isNothing() || !prop->isDataProperty()) {
    return false;
  }

  // Get the property value and ensure it's the canonical 'add' or 'set'
  // function.
  Value propVal = nproto->getSlot(prop->slot());
  return IsNativeFunction(propVal, addOrSetNative);
}

}  // namespace js

#endif /* builtin_MapObject_inl_h */
