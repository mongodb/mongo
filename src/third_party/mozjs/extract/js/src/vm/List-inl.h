/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_List_inl_h
#define vm_List_inl_h

#include "vm/List.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stdint.h>  // uint32_t

#include "js/RootingAPI.h"    // JS::Handle, JS::Rooted
#include "js/Value.h"         // JS::Value, JS::ObjectValue
#include "vm/JSContext.h"     // JSContext
#include "vm/NativeObject.h"  // js::NativeObject

#include "vm/Compartment-inl.h"   // JS::Compartment::wrap
#include "vm/JSObject-inl.h"      // js::NewObjectWithGivenProto
#include "vm/NativeObject-inl.h"  // js::NativeObject::*
#include "vm/Realm-inl.h"         // js::AutoRealm

inline /* static */ js::ListObject* js::ListObject::create(JSContext* cx) {
  return NewObjectWithGivenProto<ListObject>(cx, nullptr);
}

inline bool js::ListObject::append(JSContext* cx, JS::Handle<JS::Value> value) {
  uint32_t len = length();

  if (!ensureElements(cx, len + 1)) {
    return false;
  }

  ensureDenseInitializedLength(len, 1);
  setDenseElement(len, value);
  return true;
}

inline bool js::ListObject::appendValueAndSize(JSContext* cx,
                                               JS::Handle<JS::Value> value,
                                               double size) {
  uint32_t len = length();

  if (!ensureElements(cx, len + 2)) {
    return false;
  }

  ensureDenseInitializedLength(len, 2);
  setDenseElement(len, value);
  setDenseElement(len + 1, JS::DoubleValue(size));
  return true;
}

inline JS::Value js::ListObject::popFirst(JSContext* cx) {
  uint32_t len = length();
  MOZ_ASSERT(len > 0);

  JS::Value entry = get(0);
  if (!tryShiftDenseElements(1)) {
    moveDenseElements(0, 1, len - 1);
    setDenseInitializedLength(len - 1);
    shrinkElements(cx, len - 1);
  }

  MOZ_ASSERT(length() == len - 1);
  return entry;
}

inline void js::ListObject::popFirstPair(JSContext* cx) {
  uint32_t len = length();
  MOZ_ASSERT(len > 0);
  MOZ_ASSERT((len % 2) == 0);

  if (!tryShiftDenseElements(2)) {
    moveDenseElements(0, 2, len - 2);
    setDenseInitializedLength(len - 2);
    shrinkElements(cx, len - 2);
  }

  MOZ_ASSERT(length() == len - 2);
}

template <class T>
inline T& js::ListObject::popFirstAs(JSContext* cx) {
  return popFirst(cx).toObject().as<T>();
}

namespace js {

/**
 * Stores an empty ListObject in the given fixed slot of |obj|.
 */
[[nodiscard]] inline bool StoreNewListInFixedSlot(JSContext* cx,
                                                  JS::Handle<NativeObject*> obj,
                                                  uint32_t slot) {
  AutoRealm ar(cx, obj);
  ListObject* list = ListObject::create(cx);
  if (!list) {
    return false;
  }

  obj->setFixedSlot(slot, JS::ObjectValue(*list));
  return true;
}

/**
 * Given an object |obj| whose fixed slot |slot| contains a ListObject, append
 * |toAppend| to that list.
 */
[[nodiscard]] inline bool AppendToListInFixedSlot(
    JSContext* cx, JS::Handle<NativeObject*> obj, uint32_t slot,
    JS::Handle<JSObject*> toAppend) {
  JS::Rooted<ListObject*> list(
      cx, &obj->getFixedSlot(slot).toObject().as<ListObject>());

  AutoRealm ar(cx, list);
  JS::Rooted<JS::Value> val(cx, JS::ObjectValue(*toAppend));
  if (!cx->compartment()->wrap(cx, &val)) {
    return false;
  }
  return list->append(cx, val);
}

}  // namespace js

#endif  // vm_List_inl_h
