/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_StableCellHasher_inl_h
#define gc_StableCellHasher_inl_h

#include "gc/StableCellHasher.h"

#include "mozilla/HashFunctions.h"

#include "gc/Cell.h"
#include "gc/Marking.h"
#include "gc/Zone.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/Runtime.h"

namespace js {
namespace gc {

extern uint64_t NextCellUniqueId(JSRuntime* rt);

inline bool MaybeGetUniqueId(Cell* cell, uint64_t* uidp) {
  MOZ_ASSERT(uidp);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cell->runtimeFromAnyThread()) ||
             CurrentThreadIsPerformingGC());

  if (cell->is<JSObject>()) {
    JSObject* obj = cell->as<JSObject>();
    if (obj->is<NativeObject>()) {
      auto* nobj = &obj->as<NativeObject>();
      if (!nobj->hasUniqueId()) {
        return false;
      }

      *uidp = nobj->uniqueId();
      return true;
    }
  }

  // Get an existing uid, if one has been set.
  auto p = cell->zone()->uniqueIds().readonlyThreadsafeLookup(cell);
  if (!p) {
    return false;
  }

  *uidp = p->value();

  return true;
}

extern bool CreateUniqueIdForNativeObject(NativeObject* obj, uint64_t* uidp);
extern bool CreateUniqueIdForNonNativeObject(Cell* cell, UniqueIdMap::AddPtr,
                                             uint64_t* uidp);

inline bool GetOrCreateUniqueId(Cell* cell, uint64_t* uidp) {
  MOZ_ASSERT(uidp);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cell->runtimeFromAnyThread()) ||
             CurrentThreadIsPerformingGC());

  if (cell->is<JSObject>()) {
    JSObject* obj = cell->as<JSObject>();
    if (obj->is<NativeObject>()) {
      auto* nobj = &obj->as<NativeObject>();
      if (nobj->hasUniqueId()) {
        *uidp = nobj->uniqueId();
        return true;
      }

      return CreateUniqueIdForNativeObject(nobj, uidp);
    }
  }

  // Get an existing uid, if one has been set.
  auto p = cell->zone()->uniqueIds().lookupForAdd(cell);
  if (p) {
    *uidp = p->value();
    return true;
  }

  return CreateUniqueIdForNonNativeObject(cell, p, uidp);
}

inline bool SetOrUpdateUniqueId(JSContext* cx, Cell* cell, uint64_t uid) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cell->runtimeFromAnyThread()));

  if (cell->is<JSObject>()) {
    JSObject* obj = cell->as<JSObject>();
    if (obj->is<NativeObject>()) {
      auto* nobj = &obj->as<NativeObject>();
      return nobj->setOrUpdateUniqueId(cx, uid);
    }
  }

  // If the cell was in the nursery, hopefully unlikely, then we need to
  // tell the nursery about it so that it can sweep the uid if the thing
  // does not get tenured.
  JSRuntime* runtime = cell->runtimeFromMainThread();
  if (IsInsideNursery(cell) &&
      !runtime->gc.nursery().addedUniqueIdToCell(cell)) {
    return false;
  }

  return cell->zone()->uniqueIds().put(cell, uid);
}

inline uint64_t GetUniqueIdInfallible(Cell* cell) {
  uint64_t uid;
  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!GetOrCreateUniqueId(cell, &uid)) {
    oomUnsafe.crash("failed to allocate uid");
  }
  return uid;
}

inline bool HasUniqueId(Cell* cell) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cell->runtimeFromAnyThread()) ||
             CurrentThreadIsPerformingGC());

  if (cell->is<JSObject>()) {
    JSObject* obj = cell->as<JSObject>();
    if (obj->is<NativeObject>()) {
      return obj->as<NativeObject>().hasUniqueId();
    }
  }

  return cell->zone()->uniqueIds().has(cell);
}

inline void TransferUniqueId(Cell* tgt, Cell* src) {
  MOZ_ASSERT(src != tgt);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(tgt->runtimeFromAnyThread()));
  MOZ_ASSERT(src->zone() == tgt->zone());

  Zone* zone = tgt->zone();
  MOZ_ASSERT_IF(zone->uniqueIds().has(src), !zone->uniqueIds().has(tgt));
  zone->uniqueIds().rekeyIfMoved(src, tgt);
}

inline void RemoveUniqueId(Cell* cell) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cell->runtimeFromAnyThread()));
  // The cell may no longer be in the hash table if it was swapped with a
  // NativeObject.
  cell->zone()->uniqueIds().remove(cell);
}

}  // namespace gc

static inline js::HashNumber UniqueIdToHash(uint64_t uid) {
  // This uses the default hasher which returns the lower 32 bits of 64 bit
  // integers as the hash code. This is OK because he result will be scrambled
  // later by ScrambleHashCode().
  return DefaultHasher<uint64_t>::hash(uid);
}

template <typename T>
/* static */ bool StableCellHasher<T>::maybeGetHash(const Lookup& l,
                                                    HashNumber* hashOut) {
  if (!l) {
    *hashOut = 0;
    return true;
  }

  uint64_t uid;
  if (!gc::MaybeGetUniqueId(l, &uid)) {
    return false;
  }

  *hashOut = UniqueIdToHash(uid);
  return true;
}

template <typename T>
/* static */ bool StableCellHasher<T>::ensureHash(const Lookup& l,
                                                  HashNumber* hashOut) {
  if (!l) {
    *hashOut = 0;
    return true;
  }

  uint64_t uid;
  if (!gc::GetOrCreateUniqueId(l, &uid)) {
    return false;
  }

  *hashOut = UniqueIdToHash(uid);
  return true;
}

template <typename T>
/* static */ HashNumber StableCellHasher<T>::hash(const Lookup& l) {
  if (!l) {
    return 0;
  }

  // We have to access the zone from-any-thread here: a worker thread may be
  // cloning a self-hosted object from the main runtime's self- hosting zone
  // into another runtime. The zone's uid lock will protect against multiple
  // workers doing this simultaneously.
  MOZ_ASSERT(CurrentThreadCanAccessZone(l->zoneFromAnyThread()) ||
             CurrentThreadIsPerformingGC());

  return UniqueIdToHash(gc::GetUniqueIdInfallible(l));
}

template <typename T>
/* static */ bool StableCellHasher<T>::match(const Key& k, const Lookup& l) {
  if (k == l) {
    return true;
  }

  if (!k || !l) {
    return false;
  }

  MOZ_ASSERT(CurrentThreadCanAccessZone(l->zoneFromAnyThread()) ||
             CurrentThreadIsPerformingGC());

#ifdef DEBUG
  // Incremental table sweeping means that existing table entries may no
  // longer have unique IDs. We fail the match in that case and the entry is
  // removed from the table later on.
  if (!gc::HasUniqueId(k)) {
    Key key = k;
    MOZ_ASSERT(key->zoneFromAnyThread()->needsIncrementalBarrier() &&
               !key->isMarkedAny());
  }
  MOZ_ASSERT(gc::HasUniqueId(l));
#endif

  uint64_t keyId;
  if (!gc::MaybeGetUniqueId(k, &keyId)) {
    // Key is dead and cannot match lookup which must be live.
    return false;
  }

  return keyId == gc::GetUniqueIdInfallible(l);
}

}  // namespace js

#endif  // gc_StableCellHasher_inl_h
