/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/WeakMap-inl.h"

#include <string.h>

#include "gc/PublicIterators.h"
#include "vm/JSObject.h"

#include "gc/Marking-inl.h"

using namespace js;
using namespace js::gc;

WeakMapBase::WeakMapBase(JSObject* memOf, Zone* zone)
    : memberOf(memOf), zone_(zone) {
  MOZ_ASSERT_IF(memberOf, memberOf->compartment()->zone() == zone);
  MOZ_ASSERT(!IsMarked(mapColor()));
}

WeakMapBase::~WeakMapBase() {
  MOZ_ASSERT(CurrentThreadIsGCFinalizing() ||
             CurrentThreadCanAccessZone(zone_));
}

void WeakMapBase::unmarkZone(JS::Zone* zone) {
  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!zone->gcEphemeronEdges().clear()) {
    oomUnsafe.crash("clearing ephemeron edges table");
  }
  MOZ_ASSERT(zone->gcNurseryEphemeronEdges().count() == 0);

  for (WeakMapBase* m : zone->gcWeakMapList()) {
    m->setMapColor(CellColor::White);
  }
}

void Zone::traceWeakMaps(JSTracer* trc) {
  MOZ_ASSERT(trc->weakMapAction() != JS::WeakMapTraceAction::Skip);
  for (WeakMapBase* m : gcWeakMapList()) {
    m->trace(trc);
    TraceNullableEdge(trc, &m->memberOf, "memberOf");
  }
}

bool WeakMapBase::markMap(MarkColor markColor) {
  // We may be marking in parallel here so use a compare exchange loop to handle
  // concurrent updates to the map color.
  //
  // The color increases monotonically; we don't downgrade from black to gray.
  //
  // We can attempt to mark gray after marking black when a barrier pushes the
  // map object onto the black mark stack when it's already present on the
  // gray mark stack, since this is marked later.

  uint32_t targetColor = uint32_t(markColor);

  for (;;) {
    uint32_t currentColor = mapColor_;

    if (currentColor >= targetColor) {
      return false;
    }

    if (mapColor_.compareExchange(currentColor, targetColor)) {
      return true;
    }
  }
}

bool WeakMapBase::addEphemeronEdgesForEntry(MarkColor mapColor, Cell* key,
                                            Cell* delegate,
                                            TenuredCell* value) {
  if (delegate && !addEphemeronEdge(mapColor, delegate, key)) {
    return false;
  }

  if (value && !addEphemeronEdge(mapColor, key, value)) {
    return false;
  }

  return true;
}

bool WeakMapBase::addEphemeronEdge(MarkColor color, gc::Cell* src,
                                   gc::Cell* dst) {
  // Add an implicit edge from |src| to |dst|.

  auto& edgeTable = src->zone()->gcEphemeronEdges(src);
  auto* ptr = edgeTable.getOrAdd(src);
  return ptr && ptr->value.emplaceBack(color, dst);
}

#if defined(JS_GC_ZEAL) || defined(DEBUG)
bool WeakMapBase::checkMarkingForZone(JS::Zone* zone) {
  // This is called at the end of marking.
  MOZ_ASSERT(zone->isGCMarking());

  bool ok = true;
  for (WeakMapBase* m : zone->gcWeakMapList()) {
    if (IsMarked(m->mapColor()) && !m->checkMarking()) {
      ok = false;
    }
  }

  return ok;
}
#endif

#ifdef JSGC_HASH_TABLE_CHECKS
/* static */
void WeakMapBase::checkWeakMapsAfterMovingGC(JS::Zone* zone) {
  for (WeakMapBase* map : zone->gcWeakMapList()) {
    map->checkAfterMovingGC();
  }
}
#endif

bool WeakMapBase::markZoneIteratively(JS::Zone* zone, GCMarker* marker) {
  bool markedAny = false;
  for (WeakMapBase* m : zone->gcWeakMapList()) {
    if (IsMarked(m->mapColor()) && m->markEntries(marker)) {
      markedAny = true;
    }
  }
  return markedAny;
}

bool WeakMapBase::findSweepGroupEdgesForZone(JS::Zone* zone) {
  for (WeakMapBase* m : zone->gcWeakMapList()) {
    if (!m->findSweepGroupEdges()) {
      return false;
    }
  }
  return true;
}

void Zone::sweepWeakMaps(JSTracer* trc) {
  for (WeakMapBase* m = gcWeakMapList().getFirst(); m;) {
    WeakMapBase* next = m->getNext();
    if (IsMarked(m->mapColor())) {
      m->traceWeakEdges(trc);
    } else {
      m->clearAndCompact();
      m->removeFrom(gcWeakMapList());
    }
    m = next;
  }

#ifdef DEBUG
  for (WeakMapBase* m : gcWeakMapList()) {
    MOZ_ASSERT(m->isInList() && IsMarked(m->mapColor()));
  }
#endif
}

void WeakMapBase::traceAllMappings(WeakMapTracer* tracer) {
  JSRuntime* rt = tracer->runtime;
  for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
    for (WeakMapBase* m : zone->gcWeakMapList()) {
      // The WeakMapTracer callback is not allowed to GC.
      JS::AutoSuppressGCAnalysis nogc;
      m->traceMappings(tracer);
    }
  }
}

bool WeakMapBase::saveZoneMarkedWeakMaps(JS::Zone* zone,
                                         WeakMapColors& markedWeakMaps) {
  for (WeakMapBase* m : zone->gcWeakMapList()) {
    if (IsMarked(m->mapColor()) && !markedWeakMaps.put(m, m->mapColor())) {
      return false;
    }
  }
  return true;
}

void WeakMapBase::restoreMarkedWeakMaps(WeakMapColors& markedWeakMaps) {
  for (WeakMapColors::Range r = markedWeakMaps.all(); !r.empty();
       r.popFront()) {
    WeakMapBase* map = r.front().key();
    MOZ_ASSERT(map->zone()->isGCMarking());
    MOZ_ASSERT(!IsMarked(map->mapColor()));
    map->setMapColor(r.front().value());
  }
}

ObjectWeakMap::ObjectWeakMap(JSContext* cx) : map(cx, nullptr) {}

JSObject* ObjectWeakMap::lookup(const JSObject* obj) {
  if (ObjectValueWeakMap::Ptr p = map.lookup(const_cast<JSObject*>(obj))) {
    return &p->value().toObject();
  }
  return nullptr;
}

bool ObjectWeakMap::add(JSContext* cx, JSObject* obj, JSObject* target) {
  MOZ_ASSERT(obj && target);

  Value targetVal(ObjectValue(*target));
  if (!map.putNew(obj, targetVal)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

void ObjectWeakMap::remove(JSObject* key) {
  MOZ_ASSERT(key);
  map.remove(key);
}

void ObjectWeakMap::clear() { map.clear(); }

void ObjectWeakMap::trace(JSTracer* trc) { map.trace(trc); }

size_t ObjectWeakMap::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
  return map.shallowSizeOfExcludingThis(mallocSizeOf);
}
