/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSAtom_inl_h
#define vm_JSAtom_inl_h

#include "vm/JSAtom.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/RangedPtr.h"

#include "jsnum.h"

#include "gc/MaybeRooted.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"

namespace js {

MOZ_ALWAYS_INLINE jsid AtomToId(JSAtom* atom) {
  static_assert(JSID_INT_MIN == 0);

  uint32_t index;
  if (atom->isIndex(&index) && index <= JSID_INT_MAX) {
    return INT_TO_JSID(int32_t(index));
  }

  return JS::PropertyKey::fromNonIntAtom(atom);
}

// Use the NameToId method instead!
inline jsid AtomToId(PropertyName* name) = delete;

MOZ_ALWAYS_INLINE bool ValueToIntId(const Value& v, jsid* id) {
  int32_t i;
  if (v.isInt32()) {
    i = v.toInt32();
  } else if (!v.isDouble() || !mozilla::NumberEqualsInt32(v.toDouble(), &i)) {
    return false;
  }

  if (!INT_FITS_IN_JSID(i)) {
    return false;
  }

  *id = INT_TO_JSID(i);
  return true;
}

inline bool ValueToIdPure(const Value& v, jsid* id) {
  if (v.isString()) {
    if (v.toString()->isAtom()) {
      *id = AtomToId(&v.toString()->asAtom());
      return true;
    }
    return false;
  }

  if (ValueToIntId(v, id)) {
    return true;
  }

  if (v.isSymbol()) {
    *id = SYMBOL_TO_JSID(v.toSymbol());
    return true;
  }

  return false;
}

template <AllowGC allowGC>
inline bool PrimitiveValueToId(
    JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType v,
    typename MaybeRooted<jsid, allowGC>::MutableHandleType idp) {
  // Non-primitive values should call ToPropertyKey.
  MOZ_ASSERT(v.isPrimitive());

  if (v.isString()) {
    if (v.toString()->isAtom()) {
      idp.set(AtomToId(&v.toString()->asAtom()));
      return true;
    }
  } else {
    if (ValueToIntId(v, idp.address())) {
      return true;
    }

    if (v.isSymbol()) {
      idp.set(SYMBOL_TO_JSID(v.toSymbol()));
      return true;
    }
  }

  JSAtom* atom = ToAtom<allowGC>(cx, v);
  if (!atom) {
    return false;
  }

  idp.set(AtomToId(atom));
  return true;
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
  if (index <= JSID_INT_MAX) {
    idp.set(INT_TO_JSID(index));
    return true;
  }

  return IndexToIdSlow(cx, index, idp);
}

static MOZ_ALWAYS_INLINE JSLinearString* IdToString(JSContext* cx, jsid id) {
  if (JSID_IS_STRING(id)) {
    return id.toAtom();
  }

  if (MOZ_LIKELY(JSID_IS_INT(id))) {
    return Int32ToString<CanGC>(cx, JSID_TO_INT(id));
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
                    JSTYPE_LIMIT * sizeof(ImmutablePropertyNamePtr) <=
                sizeof(JSAtomState));
  static_assert(JSTYPE_UNDEFINED == 0);
  return (&names.undefined)[type];
}

inline Handle<PropertyName*> ClassName(JSProtoKey key, JSAtomState& atomState) {
  MOZ_ASSERT(key < JSProto_LIMIT);
  static_assert(offsetof(JSAtomState, Null) +
                    JSProto_LIMIT * sizeof(ImmutablePropertyNamePtr) <=
                sizeof(JSAtomState));
  static_assert(JSProto_Null == 0);
  return (&atomState.Null)[key];
}

}  // namespace js

#endif /* vm_JSAtom_inl_h */
