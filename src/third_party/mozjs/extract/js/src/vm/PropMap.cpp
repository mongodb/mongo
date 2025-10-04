/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/PropMap-inl.h"

#include "mozilla/Sprintf.h"

#include "gc/HashUtil.h"
#include "js/GCVector.h"
#include "js/Printer.h"  // js::GenericPrinter, js::Fprinter
#include "vm/JSObject.h"
#include "vm/JSONPrinter.h"  // JSONPrinter

#include "gc/GCContext-inl.h"
#include "gc/Marking-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/ObjectFlags-inl.h"

using namespace js;

void PropMap::addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                     size_t* children, size_t* tables) const {
  if (isShared() && asShared()->hasChildrenSet()) {
    auto* set = asShared()->treeDataRef().children.toChildrenSet();
    *children += set->shallowSizeOfIncludingThis(mallocSizeOf);
  }
  if (canHaveTable() && asLinked()->hasTable()) {
    *tables += asLinked()->data_.table->sizeOfIncludingThis(mallocSizeOf);
  }
}

// static
SharedPropMap* SharedPropMap::create(JSContext* cx, Handle<SharedPropMap*> prev,
                                     HandleId id, PropertyInfo prop) {
  // If the first property has a slot number <= MaxSlotNumber, all properties
  // added later will have a slot number <= CompactPropertyInfo::MaxSlotNumber
  // so we can use a CompactPropMap.
  static constexpr size_t MaxFirstSlot =
      CompactPropertyInfo::MaxSlotNumber - (PropMap::Capacity - 1);

  if (!prev && prop.maybeSlot() <= MaxFirstSlot) {
    return cx->newCell<CompactPropMap>(id, prop);
  }

  return cx->newCell<NormalPropMap>(prev, id, prop);
}

// static
SharedPropMap* SharedPropMap::createInitial(JSContext* cx, HandleId id,
                                            PropertyInfo prop) {
  // Lookup or create a shared map based on the first property.

  using Lookup = InitialPropMapHasher::Lookup;

  auto& table = cx->zone()->shapeZone().initialPropMaps;

  auto p = MakeDependentAddPtr(cx, table, Lookup(id, prop));
  if (p) {
    return *p;
  }

  SharedPropMap* result = create(cx, /* prev = */ nullptr, id, prop);
  if (!result) {
    return nullptr;
  }

  Lookup lookup(id, prop);
  if (!p.add(cx, table, lookup, result)) {
    return nullptr;
  }

  return result;
}

// static
SharedPropMap* SharedPropMap::clone(JSContext* cx, Handle<SharedPropMap*> map,
                                    uint32_t length) {
  MOZ_ASSERT(length > 0);

  if (map->isCompact()) {
    Rooted<CompactPropMap*> prev(cx, map->asCompact());
    return cx->newCell<CompactPropMap>(prev, length);
  }

  Rooted<NormalPropMap*> prev(cx, map->asNormal());
  return cx->newCell<NormalPropMap>(prev, length);
}

// static
DictionaryPropMap* SharedPropMap::toDictionaryMap(JSContext* cx,
                                                  Handle<SharedPropMap*> map,
                                                  uint32_t length) {
  // Starting at the last map, clone each shared map to a new dictionary map.

  Rooted<DictionaryPropMap*> lastDictMap(cx);
  Rooted<DictionaryPropMap*> nextDictMap(cx);

  Rooted<SharedPropMap*> sharedMap(cx, map);
  uint32_t sharedLength = length;
  while (true) {
    sharedMap->setHadDictionaryConversion();

    DictionaryPropMap* dictMap;
    if (sharedMap->isCompact()) {
      Rooted<CompactPropMap*> prev(cx, sharedMap->asCompact());
      dictMap = cx->newCell<DictionaryPropMap>(prev, sharedLength);
    } else {
      Rooted<NormalPropMap*> prev(cx, sharedMap->asNormal());
      dictMap = cx->newCell<DictionaryPropMap>(prev, sharedLength);
    }
    if (!dictMap) {
      return nullptr;
    }

    if (!lastDictMap) {
      lastDictMap = dictMap;
    }

    if (nextDictMap) {
      nextDictMap->initPrevious(dictMap);
    }
    nextDictMap = dictMap;

    if (!sharedMap->hasPrevious()) {
      break;
    }
    sharedMap = sharedMap->asNormal()->previous();
    sharedLength = PropMap::Capacity;
  }

  return lastDictMap;
}

static MOZ_ALWAYS_INLINE SharedPropMap* PropMapChildReadBarrier(
    SharedPropMap* parent, SharedPropMap* child) {
  JS::Zone* zone = child->zone();
  if (zone->needsIncrementalBarrier()) {
    // We need a read barrier for the map tree, since these are weak
    // pointers.
    ReadBarrier(child);
    return child;
  }

  if (MOZ_UNLIKELY(zone->isGCSweeping() &&
                   IsAboutToBeFinalizedUnbarriered(child))) {
    // The map we've found is unreachable and due to be finalized, so
    // remove our weak reference to it and don't use it.
    MOZ_ASSERT(parent->isMarkedAny());
    parent->removeChild(zone->runtimeFromMainThread()->gcContext(), child);
    return nullptr;
  }

  // We don't yield to the mutator when the zone is in this state so we don't
  // need to account for it here.
  MOZ_ASSERT(!zone->isGCCompacting());

  return child;
}

SharedPropMap* SharedPropMap::lookupChild(uint32_t length, HandleId id,
                                          PropertyInfo prop) {
  MOZ_ASSERT(length > 0);

  SharedChildrenPtr children = treeDataRef().children;
  if (children.isNone()) {
    return nullptr;
  }

  if (!hasChildrenSet()) {
    SharedPropMapAndIndex prevChild = children.toSingleChild();
    if (prevChild.index() == length - 1) {
      SharedPropMap* child = prevChild.map();
      uint32_t newPropIndex = indexOfNextProperty(length - 1);
      if (child->matchProperty(newPropIndex, id, prop)) {
        return PropMapChildReadBarrier(this, child);
      }
    }
    return nullptr;
  }

  SharedChildrenSet* set = children.toChildrenSet();
  SharedChildrenHasher::Lookup lookup(id, prop, length - 1);
  if (auto p = set->lookup(lookup)) {
    MOZ_ASSERT(p->index() == length - 1);
    SharedPropMap* child = p->map();
    return PropMapChildReadBarrier(this, child);
  }
  return nullptr;
}

bool SharedPropMap::addChild(JSContext* cx, SharedPropMapAndIndex child,
                             HandleId id, PropertyInfo prop) {
  SharedPropMap* childMap = child.map();

#ifdef DEBUG
  // If the parent map was full, the child map must have the parent as
  // |previous| map. Else, the parent and child have the same |previous| map.
  if (childMap->hasPrevious()) {
    if (child.index() == PropMap::Capacity - 1) {
      MOZ_ASSERT(childMap->asLinked()->previous() == this);
    } else {
      MOZ_ASSERT(childMap->asLinked()->previous() == asLinked()->previous());
    }
  } else {
    MOZ_ASSERT(!hasPrevious());
  }
#endif

  SharedChildrenPtr& childrenRef = treeDataRef().children;

  if (childrenRef.isNone()) {
    childrenRef.setSingleChild(child);
    childMap->treeDataRef().setParent(this, child.index());
    return true;
  }

  SharedChildrenHasher::Lookup lookup(id, prop, child.index());

  if (hasChildrenSet()) {
    if (!childrenRef.toChildrenSet()->putNew(lookup, child)) {
      ReportOutOfMemory(cx);
      return false;
    }
  } else {
    auto hash = MakeUnique<SharedChildrenSet>();
    if (!hash || !hash->reserve(2)) {
      ReportOutOfMemory(cx);
      return false;
    }
    SharedPropMapAndIndex firstChild = childrenRef.toSingleChild();
    SharedPropMap* firstChildMap = firstChild.map();
    uint32_t firstChildIndex = indexOfNextProperty(firstChild.index());
    SharedChildrenHasher::Lookup lookupFirst(
        firstChildMap->getPropertyInfoWithKey(firstChildIndex),
        firstChild.index());
    hash->putNewInfallible(lookupFirst, firstChild);
    hash->putNewInfallible(lookup, child);

    childrenRef.setChildrenSet(hash.release());
    setHasChildrenSet();
    AddCellMemory(this, sizeof(SharedChildrenSet), MemoryUse::PropMapChildren);
  }

  childMap->treeDataRef().setParent(this, child.index());
  return true;
}

// static
bool SharedPropMap::addProperty(JSContext* cx, const JSClass* clasp,
                                MutableHandle<SharedPropMap*> map,
                                uint32_t* mapLength, HandleId id,
                                PropertyFlags flags, ObjectFlags* objectFlags,
                                uint32_t* slot) {
  MOZ_ASSERT(!flags.isCustomDataProperty());

  *slot = SharedPropMap::slotSpan(clasp, map, *mapLength);

  if (MOZ_UNLIKELY(*slot > SHAPE_MAXIMUM_SLOT)) {
    ReportAllocationOverflow(cx);
    return false;
  }

  *objectFlags =
      GetObjectFlagsForNewProperty(clasp, *objectFlags, id, flags, cx);

  PropertyInfo prop = PropertyInfo(flags, *slot);
  return addPropertyInternal(cx, map, mapLength, id, prop);
}

// static
bool SharedPropMap::addPropertyInReservedSlot(
    JSContext* cx, const JSClass* clasp, MutableHandle<SharedPropMap*> map,
    uint32_t* mapLength, HandleId id, PropertyFlags flags, uint32_t slot,
    ObjectFlags* objectFlags) {
  MOZ_ASSERT(!flags.isCustomDataProperty());

  MOZ_ASSERT(slot < JSCLASS_RESERVED_SLOTS(clasp));
  MOZ_ASSERT_IF(map, map->lastUsedSlot(*mapLength) < slot);

  *objectFlags =
      GetObjectFlagsForNewProperty(clasp, *objectFlags, id, flags, cx);

  PropertyInfo prop = PropertyInfo(flags, slot);
  return addPropertyInternal(cx, map, mapLength, id, prop);
}

// static
bool SharedPropMap::addPropertyWithKnownSlot(JSContext* cx,
                                             const JSClass* clasp,
                                             MutableHandle<SharedPropMap*> map,
                                             uint32_t* mapLength, HandleId id,
                                             PropertyFlags flags, uint32_t slot,
                                             ObjectFlags* objectFlags) {
  MOZ_ASSERT(!flags.isCustomDataProperty());

  if (MOZ_UNLIKELY(slot < JSCLASS_RESERVED_SLOTS(clasp))) {
    return addPropertyInReservedSlot(cx, clasp, map, mapLength, id, flags, slot,
                                     objectFlags);
  }

  MOZ_ASSERT(slot == SharedPropMap::slotSpan(clasp, map, *mapLength));
  MOZ_RELEASE_ASSERT(slot <= SHAPE_MAXIMUM_SLOT);

  *objectFlags =
      GetObjectFlagsForNewProperty(clasp, *objectFlags, id, flags, cx);

  PropertyInfo prop = PropertyInfo(flags, slot);
  return addPropertyInternal(cx, map, mapLength, id, prop);
}

// static
bool SharedPropMap::addCustomDataProperty(JSContext* cx, const JSClass* clasp,
                                          MutableHandle<SharedPropMap*> map,
                                          uint32_t* mapLength, HandleId id,
                                          PropertyFlags flags,
                                          ObjectFlags* objectFlags) {
  MOZ_ASSERT(flags.isCustomDataProperty());

  // Custom data properties don't have a slot. Copy the last property's slot
  // number to simplify the implementation of SharedPropMap::slotSpan.
  uint32_t slot = map ? map->lastUsedSlot(*mapLength) : SHAPE_INVALID_SLOT;

  *objectFlags =
      GetObjectFlagsForNewProperty(clasp, *objectFlags, id, flags, cx);

  PropertyInfo prop = PropertyInfo(flags, slot);
  return addPropertyInternal(cx, map, mapLength, id, prop);
}

// static
bool SharedPropMap::addPropertyInternal(JSContext* cx,
                                        MutableHandle<SharedPropMap*> map,
                                        uint32_t* mapLength, HandleId id,
                                        PropertyInfo prop) {
  if (!map) {
    // Adding the first property.
    MOZ_ASSERT(*mapLength == 0);
    map.set(SharedPropMap::createInitial(cx, id, prop));
    if (!map) {
      return false;
    }
    *mapLength = 1;
    return true;
  }

  MOZ_ASSERT(*mapLength > 0);

  if (*mapLength < PropMap::Capacity) {
    // Use the next map entry if possible.
    if (!map->hasKey(*mapLength)) {
      if (map->canHaveTable()) {
        JS::AutoCheckCannotGC nogc;
        if (PropMapTable* table = map->asLinked()->maybeTable(nogc)) {
          if (!table->add(cx, id, PropMapAndIndex(map, *mapLength))) {
            return false;
          }
        }
      }
      map->initProperty(*mapLength, id, prop);
      *mapLength += 1;
      return true;
    }
    if (map->matchProperty(*mapLength, id, prop)) {
      *mapLength += 1;
      return true;
    }

    // The next entry can't be used so look up or create a child map. The child
    // map is a clone of this map up to mapLength, with the new property stored
    // as the next entry.

    if (SharedPropMap* child = map->lookupChild(*mapLength, id, prop)) {
      map.set(child);
      *mapLength += 1;
      return true;
    }

    SharedPropMap* child = SharedPropMap::clone(cx, map, *mapLength);
    if (!child) {
      return false;
    }
    child->initProperty(*mapLength, id, prop);

    SharedPropMapAndIndex childEntry(child, *mapLength - 1);
    if (!map->addChild(cx, childEntry, id, prop)) {
      return false;
    }

    map.set(child);
    *mapLength += 1;
    return true;
  }

  // This map is full so look up or create a child map.
  MOZ_ASSERT(*mapLength == PropMap::Capacity);

  if (SharedPropMap* child = map->lookupChild(*mapLength, id, prop)) {
    map.set(child);
    *mapLength = 1;
    return true;
  }

  SharedPropMap* child = SharedPropMap::create(cx, map, id, prop);
  if (!child) {
    return false;
  }

  SharedPropMapAndIndex childEntry(child, PropMap::Capacity - 1);
  if (!map->addChild(cx, childEntry, id, prop)) {
    return false;
  }

  // As an optimization, pass the table to the new child map, unless adding the
  // property to it OOMs. Measurements indicate this gets rid of a large number
  // of PropMapTable allocations because we don't need to create a second table
  // when the parent map won't be used again as last map.
  if (map->canHaveTable()) {
    JS::AutoCheckCannotGC nogc;
    if (PropMapTable* table = map->asLinked()->maybeTable(nogc)) {
      // Trigger a pre-barrier on the parent map to appease the pre-barrier
      // verifier, because edges from the table are disappearing (even though
      // these edges are strictly redundant with the |previous| maps).
      gc::PreWriteBarrier(map.get());
      if (table->add(cx, id, PropMapAndIndex(child, 0))) {
        map->asLinked()->handOffTableTo(child->asLinked());
      } else {
        cx->recoverFromOutOfMemory();
      }
    }
  }

  map.set(child);
  *mapLength = 1;
  return true;
}

static PropertyFlags ComputeFlagsForSealOrFreeze(PropertyKey key,
                                                 PropertyFlags flags,
                                                 IntegrityLevel level) {
  // Private fields are not visible to SetIntegrityLevel.
  if (key.isSymbol() && key.toSymbol()->isPrivateName()) {
    return flags;
  }

  // Make all properties non-configurable; if freezing, make data properties
  // read-only.
  flags.clearFlag(PropertyFlag::Configurable);
  if (level == IntegrityLevel::Frozen && flags.isDataDescriptor()) {
    flags.clearFlag(PropertyFlag::Writable);
  }

  return flags;
}

// static
bool SharedPropMap::freezeOrSealProperties(JSContext* cx, IntegrityLevel level,
                                           const JSClass* clasp,
                                           MutableHandle<SharedPropMap*> map,
                                           uint32_t mapLength,
                                           ObjectFlags* objectFlags) {
  // Add all maps to a Vector so we can iterate over them in reverse order
  // (property definition order).
  JS::RootedVector<SharedPropMap*> maps(cx);
  {
    SharedPropMap* curMap = map;
    while (true) {
      if (!maps.append(curMap)) {
        return false;
      }
      if (!curMap->hasPrevious()) {
        break;
      }
      curMap = curMap->asNormal()->previous();
    }
  }

  // Build a new SharedPropMap by adding each property with the changed
  // attributes.
  Rooted<SharedPropMap*> newMap(cx);
  uint32_t newMapLength = 0;

  Rooted<PropertyKey> key(cx);
  Rooted<SharedPropMap*> curMap(cx);

  for (size_t i = maps.length(); i > 0; i--) {
    curMap = maps[i - 1];
    uint32_t len = (i == 1) ? mapLength : PropMap::Capacity;

    for (uint32_t j = 0; j < len; j++) {
      key = curMap->getKey(j);
      PropertyInfo prop = curMap->getPropertyInfo(j);
      PropertyFlags flags =
          ComputeFlagsForSealOrFreeze(key, prop.flags(), level);

      if (prop.isCustomDataProperty()) {
        if (!addCustomDataProperty(cx, clasp, &newMap, &newMapLength, key,
                                   flags, objectFlags)) {
          return false;
        }
      } else {
        if (!addPropertyWithKnownSlot(cx, clasp, &newMap, &newMapLength, key,
                                      flags, prop.slot(), objectFlags)) {
          return false;
        }
      }
    }
  }

  map.set(newMap);
  MOZ_ASSERT(newMapLength == mapLength);
  return true;
}

void LinkedPropMap::handOffTableTo(LinkedPropMap* next) {
  MOZ_ASSERT(hasTable());
  MOZ_ASSERT(!next->hasTable());

  next->data_.table = data_.table;
  data_.table = nullptr;

  // Note: for tables currently only sizeof(PropMapTable) is tracked.
  RemoveCellMemory(this, sizeof(PropMapTable), MemoryUse::PropMapTable);
  AddCellMemory(next, sizeof(PropMapTable), MemoryUse::PropMapTable);
}

void DictionaryPropMap::handOffLastMapStateTo(DictionaryPropMap* newLast) {
  // A dictionary object's last map contains the table, slot freeList, and
  // holeCount. These fields always have their initial values for non-last maps.

  MOZ_ASSERT(this != newLast);

  if (asLinked()->hasTable()) {
    asLinked()->handOffTableTo(newLast->asLinked());
  }

  MOZ_ASSERT(newLast->freeList_ == SHAPE_INVALID_SLOT);
  newLast->freeList_ = freeList_;
  freeList_ = SHAPE_INVALID_SLOT;

  MOZ_ASSERT(newLast->holeCount_ == 0);
  newLast->holeCount_ = holeCount_;
  holeCount_ = 0;
}

// static
bool DictionaryPropMap::addProperty(JSContext* cx, const JSClass* clasp,
                                    MutableHandle<DictionaryPropMap*> map,
                                    uint32_t* mapLength, HandleId id,
                                    PropertyFlags flags, uint32_t slot,
                                    ObjectFlags* objectFlags) {
  MOZ_ASSERT(map);

  *objectFlags =
      GetObjectFlagsForNewProperty(clasp, *objectFlags, id, flags, cx);
  PropertyInfo prop = PropertyInfo(flags, slot);

  if (*mapLength < PropMap::Capacity) {
    JS::AutoCheckCannotGC nogc;
    if (PropMapTable* table = map->asLinked()->maybeTable(nogc)) {
      if (!table->add(cx, id, PropMapAndIndex(map, *mapLength))) {
        return false;
      }
    }
    map->initProperty(*mapLength, id, prop);
    *mapLength += 1;
    return true;
  }

  DictionaryPropMap* newMap = cx->newCell<DictionaryPropMap>(map, id, prop);
  if (!newMap) {
    return false;
  }

  JS::AutoCheckCannotGC nogc;
  if (PropMapTable* table = map->asLinked()->maybeTable(nogc)) {
    if (!table->add(cx, id, PropMapAndIndex(newMap, 0))) {
      return false;
    }
  }

  MOZ_ASSERT(newMap->previous() == map);
  map->handOffLastMapStateTo(newMap);

  map.set(newMap);
  *mapLength = 1;
  return true;
}

void DictionaryPropMap::changeProperty(JSContext* cx, const JSClass* clasp,
                                       uint32_t index, PropertyFlags flags,
                                       uint32_t slot,
                                       ObjectFlags* objectFlags) {
  MOZ_ASSERT(hasKey(index));
  *objectFlags = GetObjectFlagsForNewProperty(clasp, *objectFlags,
                                              getKey(index), flags, cx);
  linkedData_.propInfos[index] = PropertyInfo(flags, slot);
}

void DictionaryPropMap::freezeOrSealProperties(JSContext* cx,
                                               IntegrityLevel level,
                                               const JSClass* clasp,
                                               uint32_t mapLength,
                                               ObjectFlags* objectFlags) {
  DictionaryPropMap* curMap = this;
  do {
    for (uint32_t i = 0; i < mapLength; i++) {
      if (!curMap->hasKey(i)) {
        continue;
      }
      PropertyKey key = curMap->getKey(i);
      PropertyFlags flags = curMap->getPropertyInfo(i).flags();
      flags = ComputeFlagsForSealOrFreeze(key, flags, level);
      curMap->changePropertyFlags(cx, clasp, i, flags, objectFlags);
    }
    curMap = curMap->previous();
    mapLength = PropMap::Capacity;
  } while (curMap);
}

// static
void DictionaryPropMap::skipTrailingHoles(MutableHandle<DictionaryPropMap*> map,
                                          uint32_t* mapLength) {
  // After removing a property, rewind map/mapLength so that the last property
  // is not a hole. This ensures accessing the last property of a map can always
  // be done without checking for holes.

  while (true) {
    MOZ_ASSERT(*mapLength > 0);
    do {
      if (map->hasKey(*mapLength - 1)) {
        return;
      }
      map->decHoleCount();
      *mapLength -= 1;
    } while (*mapLength > 0);

    if (!map->previous()) {
      // The dictionary map is empty, return the initial map with mapLength 0.
      MOZ_ASSERT(*mapLength == 0);
      MOZ_ASSERT(map->holeCount_ == 0);
      return;
    }

    map->handOffLastMapStateTo(map->previous());
    map.set(map->previous());
    *mapLength = PropMap::Capacity;
  }
}

// static
void DictionaryPropMap::removeProperty(JSContext* cx,
                                       MutableHandle<DictionaryPropMap*> map,
                                       uint32_t* mapLength, PropMapTable* table,
                                       PropMapTable::Ptr& ptr) {
  MOZ_ASSERT(map);
  MOZ_ASSERT(*mapLength > 0);

  JS::AutoCheckCannotGC nogc;
  MOZ_ASSERT(map->asLinked()->maybeTable(nogc) == table);

  bool removingLast = (map == ptr->map() && *mapLength - 1 == ptr->index());
  ptr->map()->asDictionary()->clearProperty(ptr->index());
  map->incHoleCount();
  table->remove(ptr);

  if (removingLast) {
    skipTrailingHoles(map, mapLength);
  }
  maybeCompact(cx, map, mapLength);
}

// static
void DictionaryPropMap::densifyElements(JSContext* cx,
                                        MutableHandle<DictionaryPropMap*> map,
                                        uint32_t* mapLength,
                                        NativeObject* obj) {
  MOZ_ASSERT(map);
  MOZ_ASSERT(*mapLength > 0);

  JS::AutoCheckCannotGC nogc;
  PropMapTable* table = map->asLinked()->maybeTable(nogc);

  DictionaryPropMap* currentMap = map;
  uint32_t currentLen = *mapLength;
  do {
    for (uint32_t i = 0; i < currentLen; i++) {
      PropertyKey key = currentMap->getKey(i);
      uint32_t index;
      if (!IdIsIndex(key, &index)) {
        continue;
      }

      // The caller must have checked all sparse elements are plain data
      // properties.
      PropertyInfo prop = currentMap->getPropertyInfo(i);
      MOZ_ASSERT(prop.flags() == PropertyFlags::defaultDataPropFlags);

      uint32_t slot = prop.slot();
      Value value = obj->getSlot(slot);
      obj->setDenseElement(index, value);
      obj->freeDictionarySlot(slot);

      if (table) {
        PropMapTable::Ptr p = table->lookupRaw(key);
        MOZ_ASSERT(p);
        table->remove(p);
      }

      currentMap->clearProperty(i);
      map->incHoleCount();
    }
    currentMap = currentMap->previous();
    currentLen = PropMap::Capacity;
  } while (currentMap);

  skipTrailingHoles(map, mapLength);
  maybeCompact(cx, map, mapLength);
}

void DictionaryPropMap::maybeCompact(JSContext* cx,
                                     MutableHandle<DictionaryPropMap*> map,
                                     uint32_t* mapLength) {
  // If there are no holes, there's nothing to compact.
  if (map->holeCount_ == 0) {
    return;
  }

  JS::AutoCheckCannotGC nogc;
  PropMapTable* table = map->asLinked()->ensureTable(cx, nogc);
  if (!table) {
    // Compacting is optional so just return.
    cx->recoverFromOutOfMemory();
    return;
  }

  // Heuristic: only compact if the number of holes >= the number of (non-hole)
  // entries.
  if (map->holeCount_ < table->entryCount()) {
    return;
  }

  // Add all dictionary maps to a Vector so that we can iterate over them in
  // reverse order (property definition order). If appending to the Vector OOMs,
  // just return because compacting is optional.
  Vector<DictionaryPropMap*, 32, SystemAllocPolicy> maps;
  for (DictionaryPropMap* curMap = map; curMap; curMap = curMap->previous()) {
    if (!maps.append(curMap)) {
      return;
    }
  }

  // Use two cursors: readMapCursor/readIndexCursor iterates over all properties
  // starting at the first one, to search for the next non-hole entry.
  // writeMapCursor/writeIndexCursor is used to write all non-hole keys.
  //
  // At the start of the loop, these cursors point to the next property slot to
  // read/write.

  size_t readMapCursorVectorIndex = maps.length() - 1;
  DictionaryPropMap* readMapCursor = maps[readMapCursorVectorIndex];
  uint32_t readIndexCursor = 0;

  size_t writeMapCursorVectorIndex = readMapCursorVectorIndex;
  DictionaryPropMap* writeMapCursor = readMapCursor;
  uint32_t writeIndexCursor = 0;

  mozilla::DebugOnly<uint32_t> numHoles = 0;

  while (true) {
    if (readMapCursor->hasKey(readIndexCursor)) {
      // Found a non-hole entry, copy it to its new position and update the
      // PropMapTable to point to this new entry. Only do this if the read and
      // write positions are different from each other.
      if (readMapCursor != writeMapCursor ||
          readIndexCursor != writeIndexCursor) {
        PropertyKey key = readMapCursor->getKey(readIndexCursor);
        auto p = table->lookupRaw(key);
        MOZ_ASSERT(p);
        MOZ_ASSERT(p->map() == readMapCursor);
        MOZ_ASSERT(p->index() == readIndexCursor);

        writeMapCursor->setKey(writeIndexCursor, key);
        writeMapCursor->linkedData_.propInfos[writeIndexCursor] =
            readMapCursor->linkedData_.propInfos[readIndexCursor];

        PropMapAndIndex newEntry(writeMapCursor, writeIndexCursor);
        table->replaceEntry(p, key, newEntry);
      }
      // Advance the write cursor.
      writeIndexCursor++;
      if (writeIndexCursor == PropMap::Capacity) {
        MOZ_ASSERT(writeMapCursorVectorIndex > 0);
        writeMapCursorVectorIndex--;
        writeMapCursor = maps[writeMapCursorVectorIndex];
        writeIndexCursor = 0;
      }
    } else {
      numHoles++;
    }
    // Advance the read cursor. If there are no more maps to read from, we're
    // done.
    readIndexCursor++;
    if (readIndexCursor == PropMap::Capacity) {
      if (readMapCursorVectorIndex == 0) {
        break;
      }
      readMapCursorVectorIndex--;
      readMapCursor = maps[readMapCursorVectorIndex];
      readIndexCursor = 0;
    }
  }

  // Sanity check: the read cursor skipped holes between properties and holes
  // at the end of the last map (these are not included in holeCount_).
  MOZ_ASSERT(map->holeCount_ + (PropMap::Capacity - *mapLength) == numHoles);

  // The write cursor points to the next available slot. If this is at the start
  // of a new map, use the previous map (which must be full) instead.
  if (writeIndexCursor == 0 && writeMapCursor->previous()) {
    writeMapCursor = writeMapCursor->previous();
    *mapLength = PropMap::Capacity;
  } else {
    *mapLength = writeIndexCursor;
  }

  // Ensure the last map does not have any keys in [mapLength, Capacity).
  for (uint32_t i = *mapLength; i < PropMap::Capacity; i++) {
    writeMapCursor->clearProperty(i);
  }

  if (writeMapCursor != map) {
    map->handOffLastMapStateTo(writeMapCursor);
    map.set(writeMapCursor);
  }
  map->holeCount_ = 0;

  MOZ_ASSERT(*mapLength <= PropMap::Capacity);
  MOZ_ASSERT_IF(*mapLength == 0, !map->previous());
  MOZ_ASSERT_IF(!map->previous(), table->entryCount() == *mapLength);
}

void SharedPropMap::fixupAfterMovingGC() {
  SharedChildrenPtr& childrenRef = treeDataRef().children;
  if (childrenRef.isNone()) {
    return;
  }

  if (!hasChildrenSet()) {
    SharedPropMapAndIndex child = childrenRef.toSingleChild();
    if (gc::IsForwarded(child.map())) {
      child = SharedPropMapAndIndex(gc::Forwarded(child.map()), child.index());
      childrenRef.setSingleChild(child);
    }
    return;
  }

  SharedChildrenSet* set = childrenRef.toChildrenSet();
  for (SharedChildrenSet::Enum e(*set); !e.empty(); e.popFront()) {
    SharedPropMapAndIndex child = e.front();
    if (IsForwarded(child.map())) {
      child = SharedPropMapAndIndex(Forwarded(child.map()), child.index());
      e.mutableFront() = child;
    }
  }
}

void SharedPropMap::removeChild(JS::GCContext* gcx, SharedPropMap* child) {
  SharedPropMapAndIndex& parentRef = child->treeDataRef().parent;
  MOZ_ASSERT(parentRef.map() == this);

  uint32_t index = parentRef.index();
  parentRef.setNone();

  SharedChildrenPtr& childrenRef = treeDataRef().children;
  MOZ_ASSERT(!childrenRef.isNone());

  if (!hasChildrenSet()) {
    MOZ_ASSERT(childrenRef.toSingleChild().map() == child);
    MOZ_ASSERT(childrenRef.toSingleChild().index() == index);
    childrenRef.setNone();
    return;
  }

  SharedChildrenSet* set = childrenRef.toChildrenSet();
  {
    uint32_t nextIndex = SharedPropMap::indexOfNextProperty(index);
    SharedChildrenHasher::Lookup lookup(
        child->getPropertyInfoWithKey(nextIndex), index);
    auto p = set->lookup(lookup);
    MOZ_ASSERT(p, "Child must be in children set");
    set->remove(p);
  }

  MOZ_ASSERT(set->count() >= 1);

  if (set->count() == 1) {
    // Convert from set form back to single child form.
    SharedChildrenSet::Range r = set->all();
    SharedPropMapAndIndex remainingChild = r.front();
    childrenRef.setSingleChild(remainingChild);
    clearHasChildrenSet();
    gcx->delete_(this, set, MemoryUse::PropMapChildren);
  }
}

void LinkedPropMap::purgeTable(JS::GCContext* gcx) {
  MOZ_ASSERT(hasTable());
  gcx->delete_(this, data_.table, MemoryUse::PropMapTable);
  data_.table = nullptr;
}

uint32_t PropMap::approximateEntryCount() const {
  // Returns a number that's guaranteed to tbe >= the exact number of properties
  // in this map (including previous maps). This is used to reserve space in the
  // HashSet when allocating a table for this map.

  const PropMap* map = this;
  uint32_t count = 0;
  JS::AutoCheckCannotGC nogc;
  while (true) {
    if (!map->hasPrevious()) {
      return count + PropMap::Capacity;
    }
    if (PropMapTable* table = map->asLinked()->maybeTable(nogc)) {
      return count + table->entryCount();
    }
    count += PropMap::Capacity;
    map = map->asLinked()->previous();
  }
}

bool PropMapTable::init(JSContext* cx, LinkedPropMap* map) {
  if (!set_.reserve(map->approximateEntryCount())) {
    ReportOutOfMemory(cx);
    return false;
  }

  PropMap* curMap = map;
  while (true) {
    for (uint32_t i = 0; i < PropMap::Capacity; i++) {
      if (curMap->hasKey(i)) {
        PropertyKey key = curMap->getKey(i);
        set_.putNewInfallible(key, PropMapAndIndex(curMap, i));
      }
    }
    if (!curMap->hasPrevious()) {
      break;
    }
    curMap = curMap->asLinked()->previous();
  }

  return true;
}

void PropMapTable::trace(JSTracer* trc) {
  purgeCache();

  for (Set::Enum e(set_); !e.empty(); e.popFront()) {
    PropMap* map = e.front().map();
    TraceManuallyBarrieredEdge(trc, &map, "PropMapTable map");
    if (map != e.front().map()) {
      e.mutableFront() = PropMapAndIndex(map, e.front().index());
    }
  }
}

#ifdef JSGC_HASH_TABLE_CHECKS
void PropMapTable::checkAfterMovingGC(JS::Zone* zone) {
  CheckTableAfterMovingGC(set_, [zone](const auto& entry) {
    PropMap* map = entry.map();
    MOZ_ASSERT(map);
    CheckGCThingAfterMovingGC(map, zone);

    PropertyKey key = map->getKey(entry.index());
    MOZ_RELEASE_ASSERT(!key.isVoid());
    if (key.isGCThing()) {
      CheckGCThingAfterMovingGC(key.toGCThing(), zone);
    }

    return key;
  });
}
#endif

#ifdef DEBUG
bool LinkedPropMap::canSkipMarkingTable() {
  if (!hasTable()) {
    return true;
  }

  PropMapTable* table = data_.table;
  uint32_t count = 0;

  PropMap* map = this;
  while (true) {
    for (uint32_t i = 0; i < Capacity; i++) {
      if (map->hasKey(i)) {
        PropertyKey key = map->getKey(i);
        PropMapTable::Ptr p = table->readonlyThreadsafeLookup(key);
        MOZ_ASSERT(*p == PropMapAndIndex(map, i));
        count++;
      }
    }
    if (!map->hasPrevious()) {
      break;
    }
    map = map->asLinked()->previous();
  }

  return count == table->entryCount();
}
#endif

bool LinkedPropMap::createTable(JSContext* cx) {
  MOZ_ASSERT(canHaveTable());
  MOZ_ASSERT(!hasTable());

  UniquePtr<PropMapTable> table = cx->make_unique<PropMapTable>();
  if (!table) {
    return false;
  }

  if (!table->init(cx, this)) {
    return false;
  }

  data_.table = table.release();
  // TODO: The contents of PropMapTable is not currently tracked, only the
  // object itself.
  AddCellMemory(this, sizeof(PropMapTable), MemoryUse::PropMapTable);
  return true;
}

#if defined(DEBUG) || defined(JS_JITSPEW)
void PropMap::dump() const {
  Fprinter out(stderr);
  dump(out);
}

void PropMap::dump(js::GenericPrinter& out) const {
  js::JSONPrinter json(out);
  dump(json);
  out.put("\n");
}

void PropMap::dump(js::JSONPrinter& json) const {
  json.beginObject();
  dumpFields(json);
  json.endObject();
}

template <typename KnownF, typename UnknownF>
void ForEachPropertyFlag(PropertyFlags flags, KnownF known, UnknownF unknown) {
  uint8_t raw = flags.toRaw();
  for (uint8_t i = 1; i; i = i << 1) {
    if (!(raw & i)) {
      continue;
    }
    switch (PropertyFlag(raw & i)) {
      case PropertyFlag::Configurable:
        known("Configurable");
        break;
      case PropertyFlag::Enumerable:
        known("Enumerable");
        break;
      case PropertyFlag::Writable:
        known("Writable");
        break;
      case PropertyFlag::AccessorProperty:
        known("AccessorProperty");
        break;
      case PropertyFlag::CustomDataProperty:
        known("UseWatchtowerTestingCallback");
        break;
      default:
        unknown(i);
        break;
    }
  }
}

template <typename KnownF, typename UnknownF>
/* static */
void PropMap::forEachPropMapFlag(uintptr_t flags, KnownF known,
                                 UnknownF unknown) {
  for (uintptr_t i = 1 << gc::CellFlagBitsReservedForGC;
       i < 1 << PropMap::NumPreviousMapsShift; i = i << 1) {
    if (!(flags & i)) {
      continue;
    }
    switch (flags & i) {
      case PropMap::Flags::IsCompactFlag:
        known("IsCompactFlag");
        break;
      case PropMap::Flags::HasPrevFlag:
        known("HasPrevFlag");
        break;
      case PropMap::Flags::IsDictionaryFlag:
        known("IsDictionaryFlag");
        break;
      case PropMap::Flags::CanHaveTableFlag:
        known("CanHaveTableFlag");
        break;
      case PropMap::Flags::HasChildrenSetFlag:
        known("HasChildrenSetFlag");
        break;
      case PropMap::Flags::HadDictionaryConversionFlag:
        known("HadDictionaryConversionFlag");
        break;
      default:
        unknown(i);
        break;
    }
  }
}

const char* PropMapTypeToString(const js::PropMap* map) {
  if (map->isLinked()) {
    return "js::LinkedPropMap";
  }

  if (map->isShared()) {
    if (map->isCompact()) {
      return "js::CompactPropMap";
    }
    return "js::NormalPropMap";
  }

  return "js::DictionaryPropMap";
}

void PropMap::dumpFields(js::JSONPrinter& json) const {
  json.formatProperty("address", "(%s*)0x%p", PropMapTypeToString(this), this);

  json.beginInlineListProperty("flags");
  forEachPropMapFlag(
      flags(), [&](const char* name) { json.value("%s", name); },
      [&](uint32_t value) { json.value("Unknown(%08x)", value); });
  json.endInlineList();

  if (isLinked()) {
    asLinked()->dumpOwnFields(json);
  } else if (isShared()) {
    asShared()->dumpOwnFields(json);
  } else {
    asDictionary()->dumpOwnFields(json);
  }

  json.beginObjectProperty("properties");
  for (uint32_t i = 0; i < Capacity; i++) {
    char name[64];
    SprintfLiteral(name, "%u", i);

    if (!hasKey(i)) {
      json.nullProperty(name);
      return;
    }

    json.beginObjectProperty(name);
    dumpFieldsAt(json, i);
    json.endObject();
  }
  json.endObject();
}

void LinkedPropMap::dumpOwnFields(js::JSONPrinter& json) const {
  if (hasPrevious()) {
    json.formatProperty("previous", "(%s*)0x%p",
                        PropMapTypeToString(previous()), previous());
  }

  if (canHaveTable()) {
    json.formatProperty("table", "(js::PropMapTable*)0x%p", data_.table);
  }
}

void SharedPropMap::dumpOwnFields(js::JSONPrinter& json) const {
  SharedPropMapAndIndex parent = treeDataRef().parent;
  if (parent.isNone()) {
    json.nullProperty("parent");
  } else {
    json.formatProperty("parent", "(%s*)0x%p [%u]",
                        PropMapTypeToString(parent.map()), parent.map(),
                        parent.index());
  }
}

void DictionaryPropMap::dumpOwnFields(js::JSONPrinter& json) const {
  json.property("freeList", freeList_);
  json.property("holeCount", holeCount_);
}

void PropMap::dumpFieldsAt(js::JSONPrinter& json, uint32_t index) const {
  PropertyKey key = getKey(index);
  js::GenericPrinter& out = json.beginStringProperty("key");
  key.dumpStringContent(out);
  json.endStringProperty();

  PropertyInfo prop = getPropertyInfo(index);
  json.beginInlineListProperty("flags");
  ForEachPropertyFlag(
      prop.flags(), [&](const char* name) { json.value("%s", name); },
      [&](uint8_t value) { json.value("Unknown(%02x)", value); });
  json.endInlineList();

  if (prop.hasSlot()) {
    json.property("slot", prop.slot());
  }
}

void PropMap::dumpDescriptorStringContentAt(js::GenericPrinter& out,
                                            uint32_t index) const {
  PropertyInfo prop = getPropertyInfo(index);

  out.printf("map=(%s*)0x%p, index=%u", PropMapTypeToString(this), this, index);

  if (prop.enumerable()) {
    out.put(", enumerable");
  }
  if (prop.configurable()) {
    out.put(", configurable");
  }
  if (prop.isDataDescriptor() && prop.writable()) {
    out.put(", writable");
  }

  if (prop.isCustomDataProperty()) {
    out.printf(", <custom-data-prop>");
  }

  if (prop.hasSlot()) {
    out.printf(", slot=%u", prop.slot());
  }
}

JS::UniqueChars PropMap::getPropertyNameAt(uint32_t index) const {
  Sprinter sp;
  if (!sp.init()) {
    return nullptr;
  }

  PropertyKey key = getKey(index);
  key.dumpPropertyName(sp);

  return sp.release();
}
#endif  // defined(DEBUG) || defined(JS_JITSPEW)

#ifdef DEBUG
void PropMap::checkConsistency(NativeObject* obj) const {
  const uint32_t mapLength = obj->shape()->propMapLength();
  MOZ_ASSERT(mapLength <= PropMap::Capacity);

  JS::AutoCheckCannotGC nogc;
  if (isDictionary()) {
    // Check dictionary slot free list.
    for (uint32_t fslot = asDictionary()->freeList();
         fslot != SHAPE_INVALID_SLOT;
         fslot = obj->getSlot(fslot).toPrivateUint32()) {
      MOZ_ASSERT(fslot < obj->slotSpan());
    }

    auto* table = asLinked()->maybeTable(nogc);
    const DictionaryPropMap* curMap = asDictionary();
    uint32_t numHoles = 0;
    do {
      // Some fields must only be set for the last dictionary map.
      if (curMap != this) {
        MOZ_ASSERT(!curMap->asLinked()->hasTable());
        MOZ_ASSERT(curMap->holeCount_ == 0);
        MOZ_ASSERT(curMap->freeList_ == SHAPE_INVALID_SLOT);
      }

      for (uint32_t i = 0; i < PropMap::Capacity; i++) {
        if (!curMap->hasKey(i)) {
          if (curMap != this || i < mapLength) {
            numHoles++;
          }
          continue;
        }

        // The last dictionary map must only have keys up to mapLength.
        MOZ_ASSERT_IF(curMap == this, i < mapLength);

        PropertyInfo prop = curMap->getPropertyInfo(i);
        MOZ_ASSERT_IF(prop.hasSlot(), prop.slot() < obj->slotSpan());

        // All properties must be in the table.
        if (table) {
          PropertyKey key = curMap->getKey(i);
          auto p = table->lookupRaw(key);
          MOZ_ASSERT(p->map() == curMap);
          MOZ_ASSERT(p->index() == i);
        }
      }
      curMap = curMap->previous();
    } while (curMap);

    MOZ_ASSERT(asDictionary()->holeCount_ == numHoles);
    return;
  }

  MOZ_ASSERT(mapLength > 0);

  const SharedPropMap* curMap = asShared();
  auto* table =
      curMap->canHaveTable() ? curMap->asLinked()->maybeTable(nogc) : nullptr;

  // Shared maps without a previous map never have a table.
  MOZ_ASSERT_IF(!curMap->hasPrevious(), !curMap->canHaveTable());

  const SharedPropMap* nextMap = nullptr;
  mozilla::Maybe<uint32_t> nextSlot;
  while (true) {
    // Verify numPreviousMaps is set correctly.
    MOZ_ASSERT_IF(nextMap && nextMap->numPreviousMaps() != NumPreviousMapsMax,
                  curMap->numPreviousMaps() + 1 == nextMap->numPreviousMaps());
    MOZ_ASSERT(curMap->hasPrevious() == (curMap->numPreviousMaps() > 0));

    // If a previous map also has a table, it must have fewer entries than the
    // last map's table.
    if (table && curMap != this && curMap->canHaveTable()) {
      if (auto* table2 = curMap->asLinked()->maybeTable(nogc)) {
        MOZ_ASSERT(table2->entryCount() < table->entryCount());
      }
    }

    for (int32_t i = PropMap::Capacity - 1; i >= 0; i--) {
      uint32_t index = uint32_t(i);

      // Only the last map can have holes, for entries following mapLength.
      if (!curMap->hasKey(index)) {
        MOZ_ASSERT(index > 0);
        MOZ_ASSERT(curMap == this);
        MOZ_ASSERT(index >= mapLength);
        continue;
      }

      // Check slot numbers are within slot span and never decreasing.
      PropertyInfo prop = curMap->getPropertyInfo(i);
      if (prop.hasSlot()) {
        MOZ_ASSERT_IF((curMap != this || index < mapLength),
                      prop.slot() < obj->slotSpan());
        MOZ_ASSERT_IF(nextSlot.isSome(), *nextSlot >= prop.slot());
        nextSlot = mozilla::Some(prop.slot());
      }

      // All properties must be in the table.
      if (table) {
        PropertyKey key = curMap->getKey(index);
        auto p = table->lookupRaw(key);
        MOZ_ASSERT(p->map() == curMap);
        MOZ_ASSERT(p->index() == index);
      }
    }

    if (!curMap->hasPrevious()) {
      break;
    }
    nextMap = curMap;
    curMap = curMap->asLinked()->previous()->asShared();
  }
}
#endif  // DEBUG

JS::ubi::Node::Size JS::ubi::Concrete<PropMap>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  Size size = js::gc::Arena::thingSize(get().asTenured().getAllocKind());
  size_t children = 0;
  size_t tables = 0;
  get().addSizeOfExcludingThis(mallocSizeOf, &children, &tables);
  return size + children + tables;
}
