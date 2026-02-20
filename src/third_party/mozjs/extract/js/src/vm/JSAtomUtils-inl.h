/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSAtomUtils_inl_h
#define vm_JSAtomUtils_inl_h

#include "vm/JSAtomUtils.h"

#include "mozilla/RangedPtr.h"

#include "jsnum.h"

#include "gc/MaybeRooted.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/StringType.h"

namespace js {

MOZ_ALWAYS_INLINE jsid AtomToId(JSAtom* atom) {
  static_assert(JS::PropertyKey::IntMin == 0);

  uint32_t index;
  if (atom->isIndex(&index) && index <= JS::PropertyKey::IntMax) {
    return JS::PropertyKey::Int(int32_t(index));
  }

  return JS::PropertyKey::NonIntAtom(atom);
}

// Use the NameToId method instead!
inline jsid AtomToId(PropertyName* name) = delete;

template <AllowGC allowGC>
extern bool PrimitiveValueToIdSlow(
    JSContext* cx, typename MaybeRooted<JS::Value, allowGC>::HandleType v,
    typename MaybeRooted<jsid, allowGC>::MutableHandleType idp);

template <AllowGC allowGC>
inline bool PrimitiveValueToId(
    JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType v,
    typename MaybeRooted<jsid, allowGC>::MutableHandleType idp) {
  // Non-primitive values should call ToPropertyKey.
  MOZ_ASSERT(v.isPrimitive());

  if (v.isString()) {
    JSAtom* atom = AtomizeString(cx, v.toString());
    if (!atom) {
      if constexpr (!allowGC) {
        cx->recoverFromOutOfMemory();
      }
      return false;
    }
    idp.set(AtomToId(atom));
    return true;
  }

  if (v.isInt32()) {
    if (PropertyKey::fitsInInt(v.toInt32())) {
      idp.set(PropertyKey::Int(v.toInt32()));
      return true;
    }
  } else if (v.isSymbol()) {
    idp.set(PropertyKey::Symbol(v.toSymbol()));
    return true;
  }

  return PrimitiveValueToIdSlow<allowGC>(cx, v, idp);
}

bool IndexToIdSlow(JSContext* cx, uint32_t index, MutableHandleId idp);

inline bool IndexToId(JSContext* cx, uint32_t index, MutableHandleId idp) {
  if (index <= PropertyKey::IntMax) {
    idp.set(PropertyKey::Int(index));
    return true;
  }

  return IndexToIdSlow(cx, index, idp);
}

static MOZ_ALWAYS_INLINE JSLinearString* IdToString(
    JSContext* cx, jsid id, gc::Heap heap = gc::Heap::Default) {
  if (id.isString()) {
    return id.toAtom();
  }

  if (MOZ_LIKELY(id.isInt())) {
    return Int32ToStringWithHeap<CanGC>(cx, id.toInt(), heap);
  }

  RootedValue idv(cx, IdToValue(id));
  JSString* str = ToStringSlow<CanGC>(cx, idv);
  if (!str) {
    return nullptr;
  }

  return str->ensureLinear(cx);
}

inline Handle<PropertyName*> TypeName(JSType type, const JSAtomState& names) {
  MOZ_ASSERT(type < JSTYPE_LIMIT);
  static_assert(offsetof(JSAtomState, undefined) +
                    JSTYPE_LIMIT * sizeof(ImmutableTenuredPtr<PropertyName*>) <=
                sizeof(JSAtomState));
  static_assert(JSTYPE_UNDEFINED == 0);
  return (&names.undefined)[type];
}

inline Handle<PropertyName*> ClassName(JSProtoKey key, JSAtomState& atomState) {
  MOZ_ASSERT(key < JSProto_LIMIT);
  static_assert(offsetof(JSAtomState, Null) +
                    JSProto_LIMIT *
                        sizeof(ImmutableTenuredPtr<PropertyName*>) <=
                sizeof(JSAtomState));
  static_assert(JSProto_Null == 0);
  return (&atomState.Null)[key];
}

}  // namespace js

#endif /* vm_JSAtomUtils_inl_h */
