/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_FinalizationObservers_h
#define gc_FinalizationObservers_h

#include "gc/Barrier.h"
#include "gc/WeakMap.h"
#include "gc/ZoneAllocator.h"
#include "js/GCHashTable.h"
#include "js/GCVector.h"

namespace js {

class FinalizationRegistryObject;
class FinalizationRecordObject;
class WeakRefObject;

namespace gc {

// Per-zone data structures to support FinalizationRegistry and WeakRef.
class FinalizationObservers {
  Zone* const zone;

  // The set of all finalization registries in the associated zone. These are
  // direct pointers and are not wrapped.
  using RegistrySet =
      GCHashSet<HeapPtr<JSObject*>, StableCellHasher<HeapPtr<JSObject*>>,
                ZoneAllocPolicy>;
  RegistrySet registries;

  // A vector of FinalizationRecord objects, or CCWs to them.
  using RecordVector = GCVector<HeapPtr<JSObject*>, 1, ZoneAllocPolicy>;

  // A map from finalization registry targets to a vector of finalization
  // records representing registries that the target is registered with and
  // their associated held values. The records may be in other zones and are
  // wrapped appropriately.
  using RecordMap =
      GCHashMap<HeapPtr<JSObject*>, RecordVector,
                StableCellHasher<HeapPtr<JSObject*>>, ZoneAllocPolicy>;
  RecordMap recordMap;

  // A weak map used as a set of cross-zone wrappers. This is used for both
  // finalization registries and weak refs. For the former it has wrappers to
  // finalization record objects and for the latter wrappers to weak refs.
  //
  // The weak map marking rules keep the wrappers alive while their targets are
  // alive and ensure that they are both swept in the same sweep group.
  using WrapperWeakSet = ObjectValueWeakMap;
  WrapperWeakSet crossZoneRecords;

  // A map of weak ref targets to a vector of weak refs that are observing the
  // target. The weak refs may be in other zones and are wrapped appropriately.
  using WeakRefHeapPtrVector =
      GCVector<js::HeapPtr<JSObject*>, 1, js::ZoneAllocPolicy>;
  using WeakRefMap =
      GCHashMap<HeapPtr<JSObject*>, WeakRefHeapPtrVector,
                StableCellHasher<HeapPtr<JSObject*>>, ZoneAllocPolicy>;
  WeakRefMap weakRefMap;

  // A weak map used as a set of cross-zone weak refs wrappers.
  WrapperWeakSet crossZoneWeakRefs;

 public:
  explicit FinalizationObservers(Zone* zone);
  ~FinalizationObservers();

  // FinalizationRegistry support:
  bool addRegistry(Handle<FinalizationRegistryObject*> registry);
  bool addRecord(HandleObject target, HandleObject record);
  void clearRecords();

  void updateForRemovedRecord(JSObject* wrapper,
                              FinalizationRecordObject* record);

  // WeakRef support:
  bool addWeakRefTarget(Handle<JSObject*> target, Handle<JSObject*> weakRef);
  void removeWeakRefTarget(Handle<JSObject*> target,
                           Handle<WeakRefObject*> weakRef);

  void unregisterWeakRefWrapper(JSObject* wrapper, WeakRefObject* weakRef);

  void traceRoots(JSTracer* trc);
  void traceWeakEdges(JSTracer* trc);

#ifdef DEBUG
  void checkTables() const;
#endif

 private:
  bool addCrossZoneWrapper(WrapperWeakSet& weakSet, JSObject* wrapper);
  void removeCrossZoneWrapper(WrapperWeakSet& weakSet, JSObject* wrapper);

  void updateForRemovedWeakRef(JSObject* wrapper, WeakRefObject* weakRef);

  void traceWeakFinalizationRegistryEdges(JSTracer* trc);
  void traceWeakWeakRefEdges(JSTracer* trc);
  void traceWeakWeakRefVector(JSTracer* trc, WeakRefHeapPtrVector& weakRefs,
                              JSObject* target);

  static bool shouldRemoveRecord(FinalizationRecordObject* record);
};

// Per-global data structures to support FinalizationRegistry.
class FinalizationRegistryGlobalData {
  // Set of finalization records for finalization registries in this
  // realm. These are traced as part of the realm's global.
  using RecordSet =
      GCHashSet<HeapPtr<JSObject*>, StableCellHasher<HeapPtr<JSObject*>>,
                ZoneAllocPolicy>;
  RecordSet recordSet;

 public:
  explicit FinalizationRegistryGlobalData(Zone* zone);

  bool addRecord(FinalizationRecordObject* record);
  void removeRecord(FinalizationRecordObject* record);

  void trace(JSTracer* trc);
};

}  // namespace gc
}  // namespace js

#endif  // gc_FinalizationObservers_h
