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
void ShapeZone::checkTablesAfterMovingGC() {
  // Assert that the moving GC worked and that nothing is left in the tables
  // that points into the nursery, and that the hash table entries are
  // discoverable.

  for (auto r = initialPropMaps.all(); !r.empty(); r.popFront()) {
    SharedPropMap* map = r.front().unbarrieredGet();
    CheckGCThingAfterMovingGC(map);

    InitialPropMapHasher::Lookup lookup(map->getKey(0),
                                        map->getPropertyInfo(0));
    InitialPropMapSet::Ptr ptr = initialPropMaps.lookup(lookup);
    MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
  }

  for (auto r = baseShapes.all(); !r.empty(); r.popFront()) {
    BaseShape* base = r.front().unbarrieredGet();
    CheckGCThingAfterMovingGC(base);

    BaseShapeHasher::Lookup lookup(base->clasp(), base->realm(), base->proto());
    BaseShapeSet::Ptr ptr = baseShapes.lookup(lookup);
    MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
  }

  for (auto r = initialShapes.all(); !r.empty(); r.popFront()) {
    SharedShape* shape = r.front().unbarrieredGet();
    CheckGCThingAfterMovingGC(shape);

    using Lookup = InitialShapeHasher::Lookup;
    Lookup lookup(shape->getObjectClass(), shape->realm(), shape->proto(),
                  shape->numFixedSlots(), shape->objectFlags());
    InitialShapeSet::Ptr ptr = initialShapes.lookup(lookup);
    MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
  }

  for (auto r = propMapShapes.all(); !r.empty(); r.popFront()) {
    SharedShape* shape = r.front().unbarrieredGet();
    CheckGCThingAfterMovingGC(shape);

    using Lookup = PropMapShapeHasher::Lookup;
    Lookup lookup(shape->base(), shape->numFixedSlots(), shape->propMap(),
                  shape->propMapLength(), shape->objectFlags());
    PropMapShapeSet::Ptr ptr = propMapShapes.lookup(lookup);
    MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
  }

  for (auto r = proxyShapes.all(); !r.empty(); r.popFront()) {
    ProxyShape* shape = r.front().unbarrieredGet();
    CheckGCThingAfterMovingGC(shape);

    using Lookup = ProxyShapeHasher::Lookup;
    Lookup lookup(shape->getObjectClass(), shape->realm(), shape->proto(),
                  shape->objectFlags());
    ProxyShapeSet::Ptr ptr = proxyShapes.lookup(lookup);
    MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
  }

  for (auto r = wasmGCShapes.all(); !r.empty(); r.popFront()) {
    WasmGCShape* shape = r.front().unbarrieredGet();
    CheckGCThingAfterMovingGC(shape);

    using Lookup = WasmGCShapeHasher::Lookup;
    Lookup lookup(shape->getObjectClass(), shape->realm(), shape->proto(),
                  shape->recGroup(), shape->objectFlags());
    WasmGCShapeSet::Ptr ptr = wasmGCShapes.lookup(lookup);
    MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
  }
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
