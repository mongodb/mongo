/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC support for FinalizationRegistry and WeakRef objects.
 */

#include "gc/FinalizationObservers.h"

#include "mozilla/ScopeExit.h"

#include "builtin/FinalizationRegistryObject.h"
#include "builtin/WeakRefObject.h"
#include "gc/GCRuntime.h"
#include "gc/Zone.h"
#include "vm/JSContext.h"

#include "gc/WeakMap-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::gc;

FinalizationObservers::FinalizationObservers(Zone* zone)
    : zone(zone),
      registries(zone),
      recordMap(zone),
      crossZoneRecords(zone),
      weakRefMap(zone),
      crossZoneWeakRefs(zone) {}

FinalizationObservers::~FinalizationObservers() {
  MOZ_ASSERT(registries.empty());
  MOZ_ASSERT(recordMap.empty());
  MOZ_ASSERT(crossZoneRecords.empty());
  MOZ_ASSERT(crossZoneWeakRefs.empty());
}

bool GCRuntime::addFinalizationRegistry(
    JSContext* cx, Handle<FinalizationRegistryObject*> registry) {
  if (!cx->zone()->ensureFinalizationObservers() ||
      !cx->zone()->finalizationObservers()->addRegistry(registry)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool FinalizationObservers::addRegistry(
    Handle<FinalizationRegistryObject*> registry) {
  return registries.put(registry);
}

bool GCRuntime::registerWithFinalizationRegistry(JSContext* cx,
                                                 HandleObject target,
                                                 HandleObject record) {
  MOZ_ASSERT(!IsCrossCompartmentWrapper(target));
  MOZ_ASSERT(
      UncheckedUnwrapWithoutExpose(record)->is<FinalizationRecordObject>());
  MOZ_ASSERT(target->compartment() == record->compartment());

  Zone* zone = cx->zone();
  if (!zone->ensureFinalizationObservers() ||
      !zone->finalizationObservers()->addRecord(target, record)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool FinalizationObservers::addRecord(HandleObject target,
                                      HandleObject record) {
  // Add a record to the record map and clean up on failure.
  //
  // The following must be updated and kept in sync:
  //  - the zone's recordMap (to observe the target)
  //  - the registry's global objects's recordSet (to trace the record)
  //  - the count of cross zone records (to calculate sweep groups)

  MOZ_ASSERT(target->zone() == zone);

  FinalizationRecordObject* unwrappedRecord =
      &UncheckedUnwrapWithoutExpose(record)->as<FinalizationRecordObject>();

  Zone* registryZone = unwrappedRecord->zone();
  bool crossZone = registryZone != zone;
  if (crossZone && !addCrossZoneWrapper(crossZoneRecords, record)) {
    return false;
  }
  auto wrapperGuard = mozilla::MakeScopeExit([&] {
    if (crossZone) {
      removeCrossZoneWrapper(crossZoneRecords, record);
    }
  });

  GlobalObject* registryGlobal = &unwrappedRecord->global();
  auto* globalData = registryGlobal->getOrCreateFinalizationRegistryData();
  if (!globalData || !globalData->addRecord(unwrappedRecord)) {
    return false;
  }
  auto globalDataGuard = mozilla::MakeScopeExit(
      [&] { globalData->removeRecord(unwrappedRecord); });

  auto ptr = recordMap.lookupForAdd(target);
  if (!ptr && !recordMap.add(ptr, target, RecordVector(zone))) {
    return false;
  }

  if (!ptr->value().append(record)) {
    return false;
  }

  unwrappedRecord->setInRecordMap(true);

  globalDataGuard.release();
  wrapperGuard.release();
  return true;
}

bool FinalizationObservers::addCrossZoneWrapper(WrapperWeakSet& weakSet,
                                                JSObject* wrapper) {
  MOZ_ASSERT(IsCrossCompartmentWrapper(wrapper));
  MOZ_ASSERT(UncheckedUnwrapWithoutExpose(wrapper)->zone() != zone);

  auto ptr = weakSet.lookupForAdd(wrapper);
  MOZ_ASSERT(!ptr);
  return weakSet.add(ptr, wrapper, UndefinedValue());
}

void FinalizationObservers::removeCrossZoneWrapper(WrapperWeakSet& weakSet,
                                                   JSObject* wrapper) {
  MOZ_ASSERT(IsCrossCompartmentWrapper(wrapper));
  MOZ_ASSERT(UncheckedUnwrapWithoutExpose(wrapper)->zone() != zone);

  auto ptr = weakSet.lookupForAdd(wrapper);
  MOZ_ASSERT(ptr);
  weakSet.remove(ptr);
}

static FinalizationRecordObject* UnwrapFinalizationRecord(JSObject* obj) {
  obj = UncheckedUnwrapWithoutExpose(obj);
  if (!obj->is<FinalizationRecordObject>()) {
    MOZ_ASSERT(JS_IsDeadWrapper(obj));
    // CCWs between the compartments have been nuked. The
    // FinalizationRegistry's callback doesn't run in this case.
    return nullptr;
  }
  return &obj->as<FinalizationRecordObject>();
}

void FinalizationObservers::clearRecords() {
  // Clear table entries related to FinalizationRecordObjects, which are not
  // processed after the start of shutdown.
  //
  // WeakRefs are still updated during shutdown to avoid the possibility of
  // stale or dangling pointers.

#ifdef DEBUG
  checkTables();
#endif

  recordMap.clear();
  crossZoneRecords.clear();
}

void GCRuntime::traceWeakFinalizationObserverEdges(JSTracer* trc, Zone* zone) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(trc->runtime()));
  FinalizationObservers* observers = zone->finalizationObservers();
  if (observers) {
    observers->traceWeakEdges(trc);
  }
}

void FinalizationObservers::traceRoots(JSTracer* trc) {
  // The cross-zone wrapper weak maps are traced as roots; this does not keep
  // any of their entries alive by itself.
  crossZoneRecords.trace(trc);
  crossZoneWeakRefs.trace(trc);
}

void FinalizationObservers::traceWeakEdges(JSTracer* trc) {
  // Removing dead pointers from vectors may reorder live pointers to gray
  // things in the vector. This is OK.
  AutoTouchingGrayThings atgt;

  traceWeakWeakRefEdges(trc);
  traceWeakFinalizationRegistryEdges(trc);
}

void FinalizationObservers::traceWeakFinalizationRegistryEdges(JSTracer* trc) {
  // Sweep finalization registry data and queue finalization records for cleanup
  // for any entries whose target is dying and remove them from the map.

  GCRuntime* gc = &trc->runtime()->gc;

  for (RegistrySet::Enum e(registries); !e.empty(); e.popFront()) {
    auto result = TraceWeakEdge(trc, &e.mutableFront(), "FinalizationRegistry");
    if (result.isDead()) {
      auto* registry =
          &result.initialTarget()->as<FinalizationRegistryObject>();
      registry->queue()->setHasRegistry(false);
      e.removeFront();
    } else {
      result.finalTarget()->as<FinalizationRegistryObject>().traceWeak(trc);
    }
  }

  for (RecordMap::Enum e(recordMap); !e.empty(); e.popFront()) {
    RecordVector& records = e.front().value();

    // Sweep finalization records, updating any pointers moved by the GC and
    // remove if necessary.
    records.mutableEraseIf([&](HeapPtr<JSObject*>& heapPtr) {
      auto result = TraceWeakEdge(trc, &heapPtr, "FinalizationRecord");
      JSObject* obj =
          result.isLive() ? result.finalTarget() : result.initialTarget();
      FinalizationRecordObject* record = UnwrapFinalizationRecord(obj);
      MOZ_ASSERT_IF(record, record->isInRecordMap());

      bool shouldRemove = !result.isLive() || shouldRemoveRecord(record);
      if (shouldRemove && record && record->isInRecordMap()) {
        updateForRemovedRecord(obj, record);
      }

      return shouldRemove;
    });

#ifdef DEBUG
    for (JSObject* obj : records) {
      MOZ_ASSERT(UnwrapFinalizationRecord(obj)->isInRecordMap());
    }
#endif

    // Queue finalization records for targets that are dying.
    if (!TraceWeakEdge(trc, &e.front().mutableKey(),
                       "FinalizationRecord target")) {
      for (JSObject* obj : records) {
        FinalizationRecordObject* record = UnwrapFinalizationRecord(obj);
        FinalizationQueueObject* queue = record->queue();
        updateForRemovedRecord(obj, record);
        queue->queueRecordToBeCleanedUp(record);
        gc->queueFinalizationRegistryForCleanup(queue);
      }
      e.removeFront();
    }
  }
}

// static
bool FinalizationObservers::shouldRemoveRecord(
    FinalizationRecordObject* record) {
  // Records are removed from the target's vector for the following reasons:
  return !record ||                        // Nuked CCW to record.
         !record->isRegistered() ||        // Unregistered record.
         !record->queue()->hasRegistry();  // Dead finalization registry.
}

void FinalizationObservers::updateForRemovedRecord(
    JSObject* wrapper, FinalizationRecordObject* record) {
  // Remove other references to a record when it has been removed from the
  // zone's record map. See addRecord().
  MOZ_ASSERT(record->isInRecordMap());

  Zone* registryZone = record->zone();
  if (registryZone != zone) {
    removeCrossZoneWrapper(crossZoneRecords, wrapper);
  }

  GlobalObject* registryGlobal = &record->global();
  auto* globalData = registryGlobal->maybeFinalizationRegistryData();
  globalData->removeRecord(record);

  // The removed record may be gray, and that's OK.
  AutoTouchingGrayThings atgt;

  record->setInRecordMap(false);
}

void GCRuntime::nukeFinalizationRecordWrapper(
    JSObject* wrapper, FinalizationRecordObject* record) {
  // The target of the nuked wrapper may be gray, and that's OK.
  AutoTouchingGrayThings atgt;

  if (record->isInRecordMap()) {
    FinalizationRegistryObject::unregisterRecord(record);
    FinalizationObservers* observers = wrapper->zone()->finalizationObservers();
    observers->updateForRemovedRecord(wrapper, record);
  }
}

void GCRuntime::queueFinalizationRegistryForCleanup(
    FinalizationQueueObject* queue) {
  // Prod the embedding to call us back later to run the finalization callbacks,
  // if necessary.

  if (queue->isQueuedForCleanup()) {
    return;
  }

  JSObject* unwrappedHostDefineData = nullptr;

  if (JSObject* wrapped = queue->getHostDefinedData()) {
    unwrappedHostDefineData = UncheckedUnwrapWithoutExpose(wrapped);
    MOZ_ASSERT(unwrappedHostDefineData);
    // If the hostDefined object becomes a dead wrapper here, the target global
    // has already gone, and the finalization callback won't do anything to it
    // anyway.
    if (JS_IsDeadWrapper(unwrappedHostDefineData)) {
      return;
    }
  }

  callHostCleanupFinalizationRegistryCallback(queue->doCleanupFunction(),
                                              unwrappedHostDefineData);

  // The queue object may be gray, and that's OK.
  AutoTouchingGrayThings atgt;

  queue->setQueuedForCleanup(true);
}

// Insert a target -> weakRef mapping in the target's Zone so that a dying
// target will clear out the weakRef's target. If the weakRef is in a different
// Zone, then the crossZoneWeakRefs table will keep the weakRef alive. If the
// weakRef is in the same Zone, then it must be the actual WeakRefObject and
// not a cross-compartment wrapper, since nothing would keep that alive.
bool GCRuntime::registerWeakRef(HandleObject target, HandleObject weakRef) {
  MOZ_ASSERT(!IsCrossCompartmentWrapper(target));
  MOZ_ASSERT(UncheckedUnwrap(weakRef)->is<WeakRefObject>());
  MOZ_ASSERT_IF(target->zone() != weakRef->zone(),
                target->compartment() == weakRef->compartment());

  Zone* zone = target->zone();
  return zone->ensureFinalizationObservers() &&
         zone->finalizationObservers()->addWeakRefTarget(target, weakRef);
}

bool FinalizationObservers::addWeakRefTarget(HandleObject target,
                                             HandleObject weakRef) {
  WeakRefObject* unwrappedWeakRef =
      &UncheckedUnwrapWithoutExpose(weakRef)->as<WeakRefObject>();

  Zone* weakRefZone = unwrappedWeakRef->zone();
  bool crossZone = weakRefZone != zone;
  if (crossZone && !addCrossZoneWrapper(crossZoneWeakRefs, weakRef)) {
    return false;
  }
  auto wrapperGuard = mozilla::MakeScopeExit([&] {
    if (crossZone) {
      removeCrossZoneWrapper(crossZoneWeakRefs, weakRef);
    }
  });

  auto ptr = weakRefMap.lookupForAdd(target);
  if (!ptr && !weakRefMap.add(ptr, target, WeakRefHeapPtrVector(zone))) {
    return false;
  }

  if (!ptr->value().emplaceBack(weakRef)) {
    return false;
  }

  wrapperGuard.release();
  return true;
}

static WeakRefObject* UnwrapWeakRef(JSObject* obj) {
  MOZ_ASSERT(!JS_IsDeadWrapper(obj));
  obj = UncheckedUnwrapWithoutExpose(obj);
  return &obj->as<WeakRefObject>();
}

void FinalizationObservers::removeWeakRefTarget(
    Handle<JSObject*> target, Handle<WeakRefObject*> weakRef) {
  MOZ_ASSERT(target);

  WeakRefHeapPtrVector& weakRefs = weakRefMap.lookup(target)->value();
  JSObject* wrapper = nullptr;
  weakRefs.eraseIf([weakRef, &wrapper](JSObject* obj) {
    if (UnwrapWeakRef(obj) == weakRef) {
      wrapper = obj;
      return true;
    }
    return false;
  });

  MOZ_ASSERT(wrapper);
  updateForRemovedWeakRef(wrapper, weakRef);
}

void GCRuntime::nukeWeakRefWrapper(JSObject* wrapper, WeakRefObject* weakRef) {
  // WeakRef wrappers can exist independently of the ones we create for the
  // weakRefMap so don't assume |wrapper| is in the same zone as the WeakRef
  // target.
  JSObject* target = weakRef->target();
  if (!target) {
    return;
  }

  FinalizationObservers* observers = target->zone()->finalizationObservers();
  if (observers) {
    observers->unregisterWeakRefWrapper(wrapper, weakRef);
  }
}

void FinalizationObservers::unregisterWeakRefWrapper(JSObject* wrapper,
                                                     WeakRefObject* weakRef) {
  JSObject* target = weakRef->target();
  MOZ_ASSERT(target);

  bool removed = false;
  WeakRefHeapPtrVector& weakRefs = weakRefMap.lookup(target)->value();
  weakRefs.eraseIf([wrapper, &removed](JSObject* obj) {
    bool remove = obj == wrapper;
    if (remove) {
      removed = true;
    }
    return remove;
  });

  if (removed) {
    updateForRemovedWeakRef(wrapper, weakRef);
  }
}

void FinalizationObservers::updateForRemovedWeakRef(JSObject* wrapper,
                                                    WeakRefObject* weakRef) {
  weakRef->clearTarget();

  Zone* weakRefZone = weakRef->zone();
  if (weakRefZone != zone) {
    removeCrossZoneWrapper(crossZoneWeakRefs, wrapper);
  }
}

void FinalizationObservers::traceWeakWeakRefEdges(JSTracer* trc) {
  for (WeakRefMap::Enum e(weakRefMap); !e.empty(); e.popFront()) {
    // If target is dying, clear the target field of all weakRefs, and remove
    // the entry from the map.
    auto result = TraceWeakEdge(trc, &e.front().mutableKey(), "WeakRef target");
    if (result.isDead()) {
      for (JSObject* obj : e.front().value()) {
        updateForRemovedWeakRef(obj, UnwrapWeakRef(obj));
      }
      e.removeFront();
    } else {
      // Update the target field after compacting.
      traceWeakWeakRefVector(trc, e.front().value(), result.finalTarget());
    }
  }
}

void FinalizationObservers::traceWeakWeakRefVector(
    JSTracer* trc, WeakRefHeapPtrVector& weakRefs, JSObject* target) {
  weakRefs.mutableEraseIf([&](HeapPtr<JSObject*>& obj) -> bool {
    auto result = TraceWeakEdge(trc, &obj, "WeakRef");
    if (result.isDead()) {
      JSObject* wrapper = result.initialTarget();
      updateForRemovedWeakRef(wrapper, UnwrapWeakRef(wrapper));
    } else {
      UnwrapWeakRef(result.finalTarget())->setTargetUnbarriered(target);
    }
    return result.isDead();
  });
}

#ifdef DEBUG
void FinalizationObservers::checkTables() const {
  // Check all cross-zone wrappers are present in the appropriate table.
  size_t recordCount = 0;
  for (auto r = recordMap.all(); !r.empty(); r.popFront()) {
    for (JSObject* object : r.front().value()) {
      FinalizationRecordObject* record = UnwrapFinalizationRecord(object);
      if (record && record->isInRecordMap() && record->zone() != zone) {
        MOZ_ASSERT(crossZoneRecords.has(object));
        recordCount++;
      }
    }
  }
  MOZ_ASSERT(crossZoneRecords.count() == recordCount);

  size_t weakRefCount = 0;
  for (auto r = weakRefMap.all(); !r.empty(); r.popFront()) {
    for (JSObject* object : r.front().value()) {
      WeakRefObject* weakRef = UnwrapWeakRef(object);
      if (weakRef && weakRef->zone() != zone) {
        MOZ_ASSERT(crossZoneWeakRefs.has(object));
        weakRefCount++;
      }
    }
  }
  MOZ_ASSERT(crossZoneWeakRefs.count() == weakRefCount);
}
#endif

FinalizationRegistryGlobalData::FinalizationRegistryGlobalData(Zone* zone)
    : recordSet(zone) {}

bool FinalizationRegistryGlobalData::addRecord(
    FinalizationRecordObject* record) {
  return recordSet.putNew(record);
}

void FinalizationRegistryGlobalData::removeRecord(
    FinalizationRecordObject* record) {
  MOZ_ASSERT_IF(!record->runtimeFromMainThread()->gc.isShuttingDown(),
                recordSet.has(record));
  recordSet.remove(record);
}

void FinalizationRegistryGlobalData::trace(JSTracer* trc) {
  recordSet.trace(trc);
}
