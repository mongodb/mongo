/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/RecordType.h"

#include "mozilla/Assertions.h"

#include "jsapi.h"

#include "gc/Nursery.h"
#include "js/Array.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "util/StringBuffer.h"
#include "vm/ArrayObject.h"
#include "vm/EqualityOperations.h"
#include "vm/JSAtomUtils.h"  // AtomizeString, EnsureAtomized
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/ObjectFlags.h"
#include "vm/PropertyInfo.h"
#include "vm/PropMap.h"
#include "vm/RecordTupleShared.h"
#include "vm/StringType.h"
#include "vm/ToSource.h"
#include "vm/TupleType.h"

#include "vm/JSAtomUtils-inl.h"  // AtomToId
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

static bool RecordConstructor(JSContext* cx, unsigned argc, Value* vp);

const JSClass RecordType::class_ = {"record",
                                    JSCLASS_HAS_RESERVED_SLOTS(SLOT_COUNT),
                                    JS_NULL_CLASS_OPS, &RecordType::classSpec_};

const ClassSpec RecordType::classSpec_ = {
    GenericCreateConstructor<RecordConstructor, 1, gc::AllocKind::FUNCTION>,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr};

Shape* RecordType::getInitialShape(JSContext* cx) {
  return SharedShape::getInitialShape(cx, &RecordType::class_, cx->realm(),
                                      TaggedProto(nullptr), SLOT_COUNT);
}

bool RecordType::copy(JSContext* cx, Handle<RecordType*> in,
                      MutableHandle<RecordType*> out) {
  uint32_t len = in->length();
  out.set(RecordType::createUninitialized(cx, len));
  if (!out) {
    return false;
  }
  RootedId k(cx);
  RootedValue v(cx), vCopy(cx);
  ArrayObject& sortedKeys = in->getFixedSlot(RecordType::SORTED_KEYS_SLOT)
                                .toObject()
                                .as<ArrayObject>();
  for (uint32_t i = 0; i < len; i++) {
    // Get the ith record key and convert it to a string, then to an id `k`
    Value kVal = sortedKeys.getDenseElement(i);
    MOZ_ASSERT(kVal.isString());
    k.set(AtomToId(&kVal.toString()->asAtom()));
    cx->markId(k);

    // Get the value corresponding to `k`
    MOZ_ALWAYS_TRUE(in->getOwnProperty(cx, k, &v));

    // Copy `v` for the new record
    if (!CopyRecordTupleElement(cx, v, &vCopy)) {
      return false;
    }

    // Set `k` to `v` in the new record
    if (!out->initializeNextProperty(cx, k, vCopy)) {
      return false;
    }
  }
  return out->finishInitialization(cx);
}

uint32_t RecordType::length() {
  ArrayObject& sortedKeys =
      getFixedSlot(SORTED_KEYS_SLOT).toObject().as<ArrayObject>();

  return sortedKeys.getDenseInitializedLength();
}

RecordType* RecordType::createUninitialized(JSContext* cx,
                                            uint32_t initialLength) {
  Rooted<Shape*> shape(cx, getInitialShape(cx));
  if (!shape) {
    return nullptr;
  }

  Rooted<RecordType*> rec(
      cx, cx->newCell<RecordType>(NewObjectGCKind(), gc::Heap::Default,
                                  &RecordType::class_));
  if (!rec) {
    return nullptr;
  }
  rec->initShape(shape);
  rec->setEmptyElements();
  rec->initEmptyDynamicSlots();
  rec->initFixedSlots(SLOT_COUNT);

  Rooted<ArrayObject*> sortedKeys(
      cx, NewDenseFullyAllocatedArray(cx, initialLength));
  if (!sortedKeys) {
    return nullptr;
  }

  rec->initFixedSlot(SORTED_KEYS_SLOT, ObjectValue(*sortedKeys));
  rec->initFixedSlot(IS_ATOMIZED_SLOT, BooleanValue(false));

  return rec;
}

bool RecordType::initializeNextProperty(JSContext* cx, HandleId key,
                                        HandleValue value) {
  if (key.isSymbol()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_RECORD_NO_SYMBOL_KEY);
    return false;
  }

  if (!value.isPrimitive()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_RECORD_TUPLE_NO_OBJECT);
    return false;
  }

  mozilla::Maybe<PropertyInfo> prop = lookupPure(key);

  if (prop.isSome()) {
    MOZ_ASSERT(prop.value().hasSlot());
    setSlot(prop.value().slot(), value);
    return true;
  }

  constexpr PropertyFlags propFlags = {PropertyFlag::Enumerable};
  Rooted<NativeObject*> target(cx, this);
  uint32_t slot;
  if (!NativeObject::addProperty(cx, target, key, propFlags, &slot)) {
    return false;
  }
  initSlot(slot, value);

  // Add the key to the SORTED_KEYS internal slot

  JSAtom* atomKey = key.isString() ? AtomizeString(cx, key.toString())
                                   : Int32ToAtom(cx, key.toInt());
  if (!atomKey) {
    return false;
  }

  ArrayObject* sortedKeys =
      &getFixedSlot(SORTED_KEYS_SLOT).toObject().as<ArrayObject>();
  uint32_t initializedLength = sortedKeys->getDenseInitializedLength();

  if (!sortedKeys->ensureElements(cx, initializedLength + 1)) {
    return false;
  }
  sortedKeys->setDenseInitializedLength(initializedLength + 1);
  sortedKeys->initDenseElement(initializedLength, StringValue(atomKey));

  return true;
}

bool RecordType::finishInitialization(JSContext* cx) {
  Rooted<NativeObject*> obj(cx, this);
  if (!JSObject::setFlag(cx, obj, ObjectFlag::NotExtensible)) {
    return false;
  }
  if (!ObjectElements::FreezeOrSeal(cx, obj, IntegrityLevel::Frozen)) {
    return false;
  }

  ArrayObject& sortedKeys =
      getFixedSlot(SORTED_KEYS_SLOT).toObject().as<ArrayObject>();
  uint32_t length = sortedKeys.getDenseInitializedLength();

  Rooted<JSLinearString*> tmpKey(cx);

  // Sort the keys. This is insertion sort - O(n^2) - but it's ok for now
  // becase records are probably not too big anyway.
  for (uint32_t i = 1, j; i < length; i++) {
#define KEY(index) sortedKeys.getDenseElement(index)
#define KEY_S(index) &KEY(index).toString()->asLinear()

    MOZ_ASSERT(KEY(i).isString());
    MOZ_ASSERT(KEY(i).toString()->isLinear());

    tmpKey = KEY_S(i);

    for (j = i; j > 0 && CompareStrings(KEY_S(j - 1), tmpKey) > 0; j--) {
      sortedKeys.setDenseElement(j, KEY(j - 1));
    }

    sortedKeys.setDenseElement(j, StringValue(tmpKey));

#undef KEY
#undef KEY_S
  }

  // We preallocate 1 element for each object spread. If spreads end up
  // introducing zero elements, we can then shrink the sortedKeys array.
  sortedKeys.setDenseInitializedLength(length);
  sortedKeys.setLength(length);
  sortedKeys.setNonWritableLength(cx);

  MOZ_ASSERT(sortedKeys.length() == length);

  return true;
}

bool RecordType::getOwnProperty(JSContext* cx, HandleId id,
                                MutableHandleValue vp) const {
  if (id.isSymbol()) {
    return false;
  }

  uint32_t index;

  // Check for a native dense element.
  if (id.isInt()) {
    index = id.toInt();
    if (containsDenseElement(index)) {
      vp.set(getDenseElement(index));
      return true;
    }
  }

  // Check for a native property.
  if (PropMap* map = shape()->lookup(cx, id, &index)) {
    PropertyInfo info = map->getPropertyInfo(index);
    MOZ_ASSERT(info.isDataProperty());
    vp.set(getSlot(info.slot()));
    return true;
  }

  return false;
}

js::HashNumber RecordType::hash(const RecordType::FieldHasher& hasher) {
  MOZ_ASSERT(isAtomized());

  ArrayObject& sortedKeys =
      getFixedSlot(SORTED_KEYS_SLOT).toObject().as<ArrayObject>();
  uint32_t length = sortedKeys.length();

  js::HashNumber h = mozilla::HashGeneric(length);
  for (uint32_t i = 0; i < length; i++) {
    JSAtom& key = sortedKeys.getDenseElement(i).toString()->asAtom();

    mozilla::Maybe<PropertyInfo> prop = lookupPure(AtomToId(&key));
    MOZ_ASSERT(prop.isSome() && prop.value().hasSlot());

    h = mozilla::AddToHash(h, key.hash(), hasher(getSlot(prop.value().slot())));
  }

  return h;
}

bool RecordType::ensureAtomized(JSContext* cx) {
  if (isAtomized()) {
    return true;
  }

  ArrayObject& sortedKeys =
      getFixedSlot(SORTED_KEYS_SLOT).toObject().as<ArrayObject>();
  uint32_t length = sortedKeys.length();

  RootedValue child(cx);
  bool updated;
  for (uint32_t i = 0; i < length; i++) {
    JSAtom& key = sortedKeys.getDenseElement(i).toString()->asAtom();

    mozilla::Maybe<PropertyInfo> prop = lookupPure(AtomToId(&key));
    MOZ_ASSERT(prop.isSome() && prop.value().hasSlot());
    uint32_t slot = prop.value().slot();

    child.set(getSlot(slot));

    if (!EnsureAtomized(cx, &child, &updated)) {
      return false;
    }
    if (updated) {
      setSlot(slot, child);
    }
  }

  setFixedSlot(IS_ATOMIZED_SLOT, BooleanValue(true));

  return true;
}

bool RecordType::sameValueZero(JSContext* cx, RecordType* lhs, RecordType* rhs,
                               bool* equal) {
  return sameValueWith<SameValueZero>(cx, lhs, rhs, equal);
}

bool RecordType::sameValue(JSContext* cx, RecordType* lhs, RecordType* rhs,
                           bool* equal) {
  return sameValueWith<SameValue>(cx, lhs, rhs, equal);
}

bool RecordType::sameValueZero(RecordType* lhs, RecordType* rhs) {
  MOZ_ASSERT(lhs->isAtomized());
  MOZ_ASSERT(rhs->isAtomized());

  if (lhs == rhs) {
    return true;
  }

  uint32_t length = lhs->length();

  if (rhs->length() != length) {
    return false;
  }

  ArrayObject& lhsSortedKeys =
      lhs->getFixedSlot(SORTED_KEYS_SLOT).toObject().as<ArrayObject>();
  ArrayObject& rhsSortedKeys =
      rhs->getFixedSlot(SORTED_KEYS_SLOT).toObject().as<ArrayObject>();

  Value v1, v2;

  for (uint32_t index = 0; index < length; index++) {
    JSAtom* key = &lhsSortedKeys.getDenseElement(index).toString()->asAtom();
    if (!EqualStrings(
            key, &rhsSortedKeys.getDenseElement(index).toString()->asAtom())) {
      return false;
    }

    {
      mozilla::Maybe<PropertyInfo> lhsProp = lhs->lookupPure(AtomToId(key));
      MOZ_ASSERT(lhsProp.isSome() && lhsProp.value().hasSlot());
      v1 = lhs->getSlot(lhsProp.value().slot());
    }

    {
      mozilla::Maybe<PropertyInfo> rhsProp = rhs->lookupPure(AtomToId(key));
      MOZ_ASSERT(rhsProp.isSome() && rhsProp.value().hasSlot());
      v2 = rhs->getSlot(rhsProp.value().slot());
    }

    if (!js::SameValueZeroLinear(v1, v2)) {
      return false;
    }
  }

  return true;
}

template <bool Comparator(JSContext*, HandleValue, HandleValue, bool*)>
bool RecordType::sameValueWith(JSContext* cx, RecordType* lhs, RecordType* rhs,
                               bool* equal) {
  if (lhs == rhs) {
    *equal = true;
    return true;
  }

  uint32_t length = lhs->length();

  if (rhs->length() != length) {
    *equal = false;
    return true;
  }

  *equal = true;
  RootedString k1(cx), k2(cx);
  RootedId id(cx);
  RootedValue v1(cx), v2(cx);

  Rooted<ArrayObject*> sortedKeysLHS(
      cx, &lhs->getFixedSlot(SORTED_KEYS_SLOT).toObject().as<ArrayObject>());
  Rooted<ArrayObject*> sortedKeysRHS(
      cx, &rhs->getFixedSlot(SORTED_KEYS_SLOT).toObject().as<ArrayObject>());

  for (uint32_t index = 0; index < length; index++) {
    k1.set(sortedKeysLHS->getDenseElement(index).toString());
    k2.set(sortedKeysRHS->getDenseElement(index).toString());

    if (!EqualStrings(cx, k1, k2, equal)) {
      return false;
    }
    if (!*equal) {
      return true;
    }

    if (!JS_StringToId(cx, k1, &id)) {
      return false;
    }

    // We already know that this is an own property of both records, so both
    // calls must return true.
    MOZ_ALWAYS_TRUE(lhs->getOwnProperty(cx, id, &v1));
    MOZ_ALWAYS_TRUE(rhs->getOwnProperty(cx, id, &v2));

    if (!Comparator(cx, v1, v2, equal)) {
      return false;
    }
    if (!*equal) {
      return true;
    }
  }

  return true;
}

// Record and Record proposal section 9.2.1
static bool RecordConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (args.isConstructing()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_CONSTRUCTOR, "Record");
    return false;
  }
  // Step 2.
  RootedObject obj(cx, ToObject(cx, args.get(0)));
  if (!obj) {
    return false;
  }

  // Step 3.
  RootedIdVector keys(cx);
  if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, &keys)) {
    return false;
  }

  size_t len = keys.length();

  Rooted<RecordType*> rec(cx, RecordType::createUninitialized(cx, len));

  if (!rec) {
    return false;
  }

  RootedId propKey(cx);
  RootedValue propValue(cx);
  for (size_t i = 0; i < len; i++) {
    propKey.set(keys[i]);
    MOZ_ASSERT(!propKey.isSymbol(), "symbols are filtered out at step 3");

    // Step 4.c.ii.1.
    if (MOZ_UNLIKELY(!GetProperty(cx, obj, obj, propKey, &propValue))) {
      return false;
    }

    if (MOZ_UNLIKELY(!rec->initializeNextProperty(cx, propKey, propValue))) {
      return false;
    }
  }

  if (MOZ_UNLIKELY(!rec->finishInitialization(cx))) {
    return false;
  }

  args.rval().setExtendedPrimitive(*rec);
  return true;
}

JSString* js::RecordToSource(JSContext* cx, RecordType* rec) {
  JSStringBuilder sb(cx);

  if (!sb.append("#{")) {
    return nullptr;
  }

  ArrayObject& sortedKeys = rec->getFixedSlot(RecordType::SORTED_KEYS_SLOT)
                                .toObject()
                                .as<ArrayObject>();

  uint32_t length = sortedKeys.length();

  Rooted<RecordType*> rootedRec(cx, rec);
  RootedValue value(cx);
  RootedString keyStr(cx);
  RootedId key(cx);
  JSString* str;
  for (uint32_t index = 0; index < length; index++) {
    value.set(sortedKeys.getDenseElement(index));
    MOZ_ASSERT(value.isString());

    str = ValueToSource(cx, value);
    if (!str) {
      return nullptr;
    }
    if (!sb.append(str)) {
      return nullptr;
    }

    if (!sb.append(": ")) {
      return nullptr;
    }

    keyStr.set(value.toString());
    if (!JS_StringToId(cx, keyStr, &key)) {
      return nullptr;
    }

    MOZ_ALWAYS_TRUE(rootedRec->getOwnProperty(cx, key, &value));

    str = ValueToSource(cx, value);
    if (!str) {
      return nullptr;
    }
    if (!sb.append(str)) {
      return nullptr;
    }

    if (index + 1 != length) {
      if (!sb.append(", ")) {
        return nullptr;
      }
    }
  }

  /* Finalize the buffer. */
  if (!sb.append('}')) {
    return nullptr;
  }

  return sb.finishString();
}
