/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ShapeZone_h
#define vm_ShapeZone_h

#include "mozilla/MemoryReporting.h"

#include "gc/Barrier.h"
#include "js/GCHashTable.h"
#include "vm/PropertyKey.h"
#include "vm/PropMap.h"
#include "vm/Shape.h"
#include "vm/TaggedProto.h"

namespace js {

// Hash policy for the per-zone baseShapes set.
struct BaseShapeHasher {
  struct Lookup {
    const JSClass* clasp;
    JS::Realm* realm;
    TaggedProto proto;

    Lookup(const JSClass* clasp, JS::Realm* realm, TaggedProto proto)
        : clasp(clasp), realm(realm), proto(proto) {}
  };

  static HashNumber hash(const Lookup& lookup) {
    HashNumber hash = MovableCellHasher<TaggedProto>::hash(lookup.proto);
    return mozilla::AddToHash(hash, lookup.clasp, lookup.realm);
  }
  static bool match(const WeakHeapPtr<BaseShape*>& key, const Lookup& lookup) {
    return key.unbarrieredGet()->clasp() == lookup.clasp &&
           key.unbarrieredGet()->realm() == lookup.realm &&
           key.unbarrieredGet()->proto() == lookup.proto;
  }
};
using BaseShapeSet = JS::WeakCache<
    JS::GCHashSet<WeakHeapPtr<BaseShape*>, BaseShapeHasher, SystemAllocPolicy>>;

// Hash policy for the per-zone initialPropMaps set, mapping property key + info
// to a shared property map.
struct InitialPropMapHasher {
  struct Lookup {
    PropertyKey key;
    PropertyInfo prop;

    Lookup(PropertyKey key, PropertyInfo prop) : key(key), prop(prop) {}
  };
  static HashNumber hash(const Lookup& lookup) {
    HashNumber hash = HashPropertyKey(lookup.key);
    return mozilla::AddToHash(hash, lookup.prop.toRaw());
  }
  static bool match(const WeakHeapPtr<SharedPropMap*>& key,
                    const Lookup& lookup) {
    const SharedPropMap* map = key.unbarrieredGet();
    return map->matchProperty(0, lookup.key, lookup.prop);
  }
};
using InitialPropMapSet =
    JS::WeakCache<JS::GCHashSet<WeakHeapPtr<SharedPropMap*>,
                                InitialPropMapHasher, SystemAllocPolicy>>;

// Hash policy for the per-zone initialShapes set storing initial shapes for
// objects in the zone.
//
// These are empty shapes, except for certain classes (e.g. String, RegExp)
// which may add certain baked-in properties. See insertInitialShape.
struct InitialShapeHasher {
  struct Lookup {
    const JSClass* clasp;
    JS::Realm* realm;
    TaggedProto proto;
    uint32_t nfixed;
    ObjectFlags objectFlags;

    Lookup(const JSClass* clasp, JS::Realm* realm, const TaggedProto& proto,
           uint32_t nfixed, ObjectFlags objectFlags)
        : clasp(clasp),
          realm(realm),
          proto(proto),
          nfixed(nfixed),
          objectFlags(objectFlags) {}
  };

  static HashNumber hash(const Lookup& lookup) {
    HashNumber hash = MovableCellHasher<TaggedProto>::hash(lookup.proto);
    return mozilla::AddToHash(hash, lookup.clasp, lookup.realm, lookup.nfixed,
                              lookup.objectFlags.toRaw());
  }
  static bool match(const WeakHeapPtr<Shape*>& key, const Lookup& lookup) {
    const Shape* shape = key.unbarrieredGet();
    return lookup.clasp == shape->getObjectClass() &&
           lookup.realm == shape->realm() && lookup.proto == shape->proto() &&
           lookup.nfixed == shape->numFixedSlots() &&
           lookup.objectFlags == shape->objectFlags();
  }
};
using InitialShapeSet = JS::WeakCache<
    JS::GCHashSet<WeakHeapPtr<Shape*>, InitialShapeHasher, SystemAllocPolicy>>;

// Hash policy for the per-zone propMapShapes set storing shared shapes with
// shared property maps.
struct PropMapShapeHasher {
  struct Lookup {
    BaseShape* base;
    SharedPropMap* map;
    uint32_t mapLength;
    uint32_t nfixed;
    ObjectFlags objectFlags;

    Lookup(BaseShape* base, uint32_t nfixed, SharedPropMap* map,
           uint32_t mapLength, ObjectFlags objectFlags)
        : base(base),
          map(map),
          mapLength(mapLength),
          nfixed(nfixed),
          objectFlags(objectFlags) {}
  };

  static HashNumber hash(const Lookup& lookup) {
    return mozilla::HashGeneric(lookup.base, lookup.map, lookup.mapLength,
                                lookup.nfixed, lookup.objectFlags.toRaw());
  }
  static bool match(const WeakHeapPtr<Shape*>& key, const Lookup& lookup) {
    const Shape* shape = key.unbarrieredGet();
    return lookup.base == shape->base() &&
           lookup.nfixed == shape->numFixedSlots() &&
           lookup.map == shape->propMap() &&
           lookup.mapLength == shape->propMapLength() &&
           lookup.objectFlags == shape->objectFlags();
  }
  static void rekey(WeakHeapPtr<Shape*>& k, const WeakHeapPtr<Shape*>& newKey) {
    k = newKey;
  }
};
using PropMapShapeSet = JS::WeakCache<
    JS::GCHashSet<WeakHeapPtr<Shape*>, PropMapShapeHasher, SystemAllocPolicy>>;

struct ShapeZone {
  // Set of all base shapes in the Zone.
  BaseShapeSet baseShapes;

  // Set used to look up a shared property map based on the first property's
  // PropertyKey and PropertyInfo.
  InitialPropMapSet initialPropMaps;

  // Set of initial shapes in the Zone.
  InitialShapeSet initialShapes;

  // Set of SharedPropMapShapes in the Zone.
  PropMapShapeSet propMapShapes;

  using ShapeWithCacheVector = js::Vector<js::Shape*, 0, js::SystemAllocPolicy>;
  ShapeWithCacheVector shapesWithCache;

  explicit ShapeZone(Zone* zone);

  void clearTables(JSFreeOp* fop);
  void purgeShapeCaches(JSFreeOp* fop);

  void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              size_t* initialPropMapTable, size_t* shapeTables);

  void fixupPropMapShapeTableAfterMovingGC();

#ifdef JSGC_HASH_TABLE_CHECKS
  void checkTablesAfterMovingGC();
#endif
};

}  // namespace js

#endif /* vm_ShapeZone_h */
