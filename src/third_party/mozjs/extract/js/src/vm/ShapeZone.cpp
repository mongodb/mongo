/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS symbol tables. */

#include "vm/ShapeZone.h"

#include "gc/Marking-inl.h"
#include "vm/Shape-inl.h"

using namespace js;
using namespace js::gc;

void ShapeZone::fixupPropMapShapeTableAfterMovingGC() {
  for (PropMapShapeSet::Enum e(propMapShapes); !e.empty(); e.popFront()) {
    SharedShape* shape = MaybeForwarded(e.front().unbarrieredGet());
    SharedPropMap* map = shape->propMapMaybeForwarded();
    BaseShape* base = MaybeForwarded(shape->base());

    PropMapShapeSet::Lookup lookup(base, shape->numFixedSlots(), map,
                                   shape->propMapLength(),
                                   shape->objectFlags());
    e.rekeyFront(lookup, shape);
  }
}

#ifdef JSGC_HASH_TABLE_CHECKS
void ShapeZone::checkTablesAfterMovingGC(JS::Zone* zone) {
  CheckTableAfterMovingGC(initialPropMaps, [zone](const auto& entry) {
    SharedPropMap* map = entry.unbarrieredGet();
    CheckGCThingAfterMovingGC(map, zone);
    PropertyKey key = map->getKey(0);
    if (key.isGCThing()) {
      CheckGCThingAfterMovingGC(key.toGCThing(), zone);
    }

    return InitialPropMapHasher::Lookup(key, map->getPropertyInfo(0));
  });

  CheckTableAfterMovingGC(baseShapes, [zone](const auto& entry) {
    BaseShape* base = entry.unbarrieredGet();
    CheckGCThingAfterMovingGC(base, zone);
    CheckProtoAfterMovingGC(base->proto(), zone);

    return BaseShapeHasher::Lookup(base->clasp(), base->realm(), base->proto());
  });

  CheckTableAfterMovingGC(initialShapes, [zone](const auto& entry) {
    SharedShape* shape = entry.unbarrieredGet();
    CheckGCThingAfterMovingGC(shape, zone);
    CheckProtoAfterMovingGC(shape->proto(), zone);

    return InitialShapeHasher::Lookup(shape->getObjectClass(), shape->realm(),
                                      shape->proto(), shape->numFixedSlots(),
                                      shape->objectFlags());
  });

  CheckTableAfterMovingGC(propMapShapes, [zone](const auto& entry) {
    SharedShape* shape = entry.unbarrieredGet();
    CheckGCThingAfterMovingGC(shape, zone);
    CheckGCThingAfterMovingGC(shape->base(), zone);
    CheckGCThingAfterMovingGC(shape->propMap(), zone);

    return PropMapShapeHasher::Lookup(shape->base(), shape->numFixedSlots(),
                                      shape->propMap(), shape->propMapLength(),
                                      shape->objectFlags());
  });

  CheckTableAfterMovingGC(proxyShapes, [zone](const auto& entry) {
    ProxyShape* shape = entry.unbarrieredGet();
    CheckGCThingAfterMovingGC(shape, zone);
    CheckProtoAfterMovingGC(shape->proto(), zone);

    return ProxyShapeHasher::Lookup(shape->getObjectClass(), shape->realm(),
                                    shape->proto(), shape->objectFlags());
  });

  CheckTableAfterMovingGC(wasmGCShapes, [zone](const auto& entry) {
    WasmGCShape* shape = entry.unbarrieredGet();
    CheckGCThingAfterMovingGC(shape, zone);
    CheckProtoAfterMovingGC(shape->proto(), zone);

    return WasmGCShapeHasher::Lookup(shape->getObjectClass(), shape->realm(),
                                     shape->proto(), shape->recGroup(),
                                     shape->objectFlags());
  });
}
#endif  // JSGC_HASH_TABLE_CHECKS

ShapeZone::ShapeZone(Zone* zone)
    : baseShapes(zone),
      initialPropMaps(zone),
      initialShapes(zone),
      propMapShapes(zone),
      proxyShapes(zone),
      wasmGCShapes(zone) {}

void ShapeZone::purgeShapeCaches(JS::GCContext* gcx) {
  for (Shape* shape : shapesWithCache) {
    MaybeForwarded(shape)->purgeCache(gcx);
  }
  shapesWithCache.clearAndFree();
}

void ShapeZone::addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                       size_t* initialPropMapTable,
                                       size_t* shapeTables) {
  *shapeTables += baseShapes.sizeOfExcludingThis(mallocSizeOf);
  *initialPropMapTable += initialPropMaps.sizeOfExcludingThis(mallocSizeOf);
  *shapeTables += initialShapes.sizeOfExcludingThis(mallocSizeOf);
  *shapeTables += propMapShapes.sizeOfExcludingThis(mallocSizeOf);
  *shapeTables += proxyShapes.sizeOfExcludingThis(mallocSizeOf);
  *shapeTables += wasmGCShapes.sizeOfExcludingThis(mallocSizeOf);
  *shapeTables += shapesWithCache.sizeOfExcludingThis(mallocSizeOf);
}
