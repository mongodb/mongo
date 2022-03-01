/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PropMap_inl_h
#define vm_PropMap_inl_h

#include "vm/PropMap.h"

#include "gc/Allocator.h"
#include "vm/Interpreter.h"
#include "vm/JSObject.h"
#include "vm/TypedArrayObject.h"

#include "gc/FreeOp-inl.h"
#include "gc/Marking-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSContext-inl.h"

namespace js {

inline AutoKeepPropMapTables::AutoKeepPropMapTables(JSContext* cx)
    : cx_(cx), prev_(cx->zone()->keepPropMapTables()) {
  cx->zone()->setKeepPropMapTables(true);
}

inline AutoKeepPropMapTables::~AutoKeepPropMapTables() {
  cx_->zone()->setKeepPropMapTables(prev_);
}

// static
MOZ_ALWAYS_INLINE PropMap* PropMap::lookupLinear(uint32_t mapLength,
                                                 PropertyKey key,
                                                 uint32_t* index) {
  MOZ_ASSERT(mapLength > 0);
  MOZ_ASSERT(mapLength <= Capacity);

  // This function is very hot, so we use a macro to manually unroll the lookups
  // below. Some compilers are able to unroll the equivalent loops, but they're
  // not very consistent about this. The code below results in reasonable code
  // with all compilers we tested.

  static_assert(PropMap::Capacity == 8,
                "Code below needs to change when capacity changes");

#define LOOKUP_KEY(idx)                        \
  if (mapLength > idx && getKey(idx) == key) { \
    *index = idx;                              \
    return this;                               \
  }
  LOOKUP_KEY(0);
  LOOKUP_KEY(1);
  LOOKUP_KEY(2);
  LOOKUP_KEY(3);
  LOOKUP_KEY(4);
  LOOKUP_KEY(5);
  LOOKUP_KEY(6);
  LOOKUP_KEY(7);
#undef LOOKUP_KEY

  PropMap* map = this;
  while (map->hasPrevious()) {
    map = map->asLinked()->previous();
#define LOOKUP_KEY(idx)          \
  if (map->getKey(idx) == key) { \
    *index = idx;                \
    return map;                  \
  }
    LOOKUP_KEY(0);
    LOOKUP_KEY(1);
    LOOKUP_KEY(2);
    LOOKUP_KEY(3);
    LOOKUP_KEY(4);
    LOOKUP_KEY(5);
    LOOKUP_KEY(6);
    LOOKUP_KEY(7);
#undef LOOKUP_INDEX
  }

  return nullptr;
}

MOZ_ALWAYS_INLINE PropMap* PropMapTable::lookup(PropMap* map,
                                                uint32_t mapLength,
                                                PropertyKey key,
                                                uint32_t* index) {
  JS::AutoCheckCannotGC nogc;
  MOZ_ASSERT(map->asLinked()->maybeTable(nogc) == this);

  PropMapAndIndex entry;
  if (lookupInCache(key, &entry)) {
    if (entry.isNone()) {
      return nullptr;
    }
  } else {
    auto p = lookupRaw(key);
    addToCache(key, p);
    if (!p) {
      return nullptr;
    }
    entry = *p;
  }

  // For the last map, only properties in [0, mapLength) are part of the object.
  if (entry.map() == map && entry.index() >= mapLength) {
    return nullptr;
  }

  *index = entry.index();
  return entry.map();
}

// static
MOZ_ALWAYS_INLINE PropMap* PropMap::lookupPure(uint32_t mapLength,
                                               PropertyKey key,
                                               uint32_t* index) {
  if (canHaveTable()) {
    JS::AutoCheckCannotGC nogc;
    if (PropMapTable* table = asLinked()->maybeTable(nogc)) {
      return table->lookup(this, mapLength, key, index);
    }
  }

  return lookupLinear(mapLength, key, index);
}

// static
MOZ_ALWAYS_INLINE PropMap* PropMap::lookup(JSContext* cx, uint32_t mapLength,
                                           PropertyKey key, uint32_t* index) {
  if (canHaveTable()) {
    JS::AutoCheckCannotGC nogc;
    if (PropMapTable* table = asLinked()->ensureTable(cx, nogc);
        MOZ_LIKELY(table)) {
      return table->lookup(this, mapLength, key, index);
    }
    // OOM. Do a linear lookup.
    cx->recoverFromOutOfMemory();
  }

  return lookupLinear(mapLength, key, index);
}

// static
inline void SharedPropMap::getPrevious(MutableHandle<SharedPropMap*> map,
                                       uint32_t* mapLength) {
  // Update the map/mapLength pointers to "remove" the last property. In most
  // cases we can simply decrement *mapLength, but if *mapLength is 1 we have to
  // either start at the previous map or set map/mapLength to nullptr/zero
  // (if there is just one property).

  MOZ_ASSERT(map);
  MOZ_ASSERT(*mapLength > 0);

  if (*mapLength > 1) {
    *mapLength -= 1;
    return;
  }

  if (map->hasPrevious()) {
    map.set(map->asNormal()->previous());
    *mapLength = PropMap::Capacity;
    return;
  }

  map.set(nullptr);
  *mapLength = 0;
}

// static
inline bool PropMap::lookupForRemove(JSContext* cx, PropMap* map,
                                     uint32_t mapLength, PropertyKey key,
                                     const AutoKeepPropMapTables& keep,
                                     PropMap** propMap, uint32_t* propIndex,
                                     PropMapTable** table,
                                     PropMapTable::Ptr* ptr) {
  if (map->isDictionary()) {
    *table = map->asLinked()->ensureTable(cx, keep);
    if (!*table) {
      return false;
    }
    *ptr = (*table)->lookupRaw(key);
    *propMap = *ptr ? (*ptr)->map() : nullptr;
    *propIndex = *ptr ? (*ptr)->index() : 0;
    return true;
  }

  *table = nullptr;
  *propMap = map->lookup(cx, mapLength, key, propIndex);
  return true;
}

MOZ_ALWAYS_INLINE bool SharedPropMap::shouldConvertToDictionaryForAdd() const {
  if (MOZ_LIKELY(numPreviousMaps() < NumPrevMapsConsiderDictionary)) {
    return false;
  }
  if (numPreviousMaps() >= NumPrevMapsAlwaysDictionary) {
    return true;
  }

  // More heuristics: if one of the last two maps has had a dictionary
  // conversion before, or is branchy (indicated by parent != previous), convert
  // to dictionary.
  const SharedPropMap* curMap = this;
  for (size_t i = 0; i < 2; i++) {
    if (curMap->hadDictionaryConversion()) {
      return true;
    }
    if (curMap->treeDataRef().parent.map() != curMap->asNormal()->previous()) {
      return true;
    }
    curMap = curMap->asNormal()->previous();
  }
  return false;
}

inline void SharedPropMap::sweep(JSFreeOp* fop) {
  // We detach the child from the parent if the parent is reachable.
  //
  // This test depends on PropMap arenas not being freed until after we finish
  // incrementally sweeping them. If that were not the case the parent pointer
  // could point to a marked cell that had been deallocated and then
  // reallocated, since allocating a cell in a zone that is being marked will
  // set the mark bit for that cell.

  MOZ_ASSERT(zone()->isGCSweeping());
  MOZ_ASSERT_IF(hasPrevious(), asLinked()->previous()->zone() == zone());

  SharedPropMapAndIndex parent = treeDataRef().parent;
  if (!parent.isNone() && parent.map()->isMarkedAny()) {
    parent.map()->removeChild(fop, this);
  }
}

inline void SharedPropMap::finalize(JSFreeOp* fop) {
  if (canHaveTable() && asLinked()->hasTable()) {
    asLinked()->purgeTable(fop);
  }
  if (hasChildrenSet()) {
    SharedChildrenPtr& childrenRef = treeDataRef().children;
    fop->delete_(this, childrenRef.toChildrenSet(), MemoryUse::PropMapChildren);
    childrenRef.setNone();
  }
}

inline void DictionaryPropMap::finalize(JSFreeOp* fop) {
  if (asLinked()->hasTable()) {
    asLinked()->purgeTable(fop);
  }
}

}  // namespace js

#endif /* vm_PropMap_inl_h */
