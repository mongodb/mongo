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
    JSAtom* atom;
    if (v.toString()->isAtom()) {
      atom = &v.toString()->asAtom();
    } else {
      atom = AtomizeString(cx, v.toString());
      if (!atom) {
        if constexpr (!allowGC) {
          cx->recoverFromOutOfMemory();
        }
        return false;
      }
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

/*
 * Write out character representing |index| to the memory just before |end|.
 * Thus |*end| is not touched, but |end[-1]| and earlier are modified as
 * appropriate.  There must be at least js::UINT32_CHAR_BUFFER_LENGTH elements
 * before |end| to avoid buffer underflow.  The start of the characters written
 * is returned and is necessarily before |end|.
 */
template <typename T>
inline mozilla::RangedPtr<T> BackfillIndexInCharBuffer(
    uint32_t index, mozilla::RangedPtr<T> end) {
#ifdef DEBUG
  /*
   * Assert that the buffer we're filling will hold as many characters as we
   * could write out, by dereferencing the index that would hold the most
   * significant digit.
   */
  (void)*(end - UINT32_CHAR_BUFFER_LENGTH);
#endif

  do {
    uint32_t next = index / 10, digit = index % 10;
    *--end = '0' + digit;
    index = next;
  } while (index > 0);

  return end;
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
