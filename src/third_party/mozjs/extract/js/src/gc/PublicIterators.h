/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Iterators for various data structures.
 */

#ifndef gc_PublicIterators_h
#define gc_PublicIterators_h

#include "jstypes.h"
#include "gc/GCRuntime.h"
#include "gc/IteratorUtils.h"
#include "gc/Zone.h"
#include "vm/Compartment.h"
#include "vm/Runtime.h"

namespace JS {
class JS_PUBLIC_API Realm;
}

namespace js {

enum ZoneSelector { WithAtoms, SkipAtoms };

// Iterate over all zones in the runtime. May or may not include the atoms zone.
class ZonesIter {
  gc::AutoEnterIteration iterMarker;
  JS::Zone** it;
  JS::Zone** const end;

 public:
  ZonesIter(gc::GCRuntime* gc, ZoneSelector selector)
      : iterMarker(gc), it(gc->zones().begin()), end(gc->zones().end()) {
    if (selector == SkipAtoms) {
      MOZ_ASSERT(get()->isAtomsZone());
      next();
    }
  }
  ZonesIter(JSRuntime* rt, ZoneSelector selector)
      : ZonesIter(&rt->gc, selector) {}

  bool done() const { return it == end; }

  void next() {
    MOZ_ASSERT(!done());
    it++;
  }

  JS::Zone* get() const {
    MOZ_ASSERT(!done());
    return *it;
  }

  operator JS::Zone*() const { return get(); }
  JS::Zone* operator->() const { return get(); }
};

// Iterate over all zones in the runtime apart from the atoms zone.
class NonAtomZonesIter : public ZonesIter {
 public:
  explicit NonAtomZonesIter(gc::GCRuntime* gc) : ZonesIter(gc, SkipAtoms) {}
  explicit NonAtomZonesIter(JSRuntime* rt) : NonAtomZonesIter(&rt->gc) {}
};

// Iterate over all zones in the runtime, except those which may be in use by
// parse threads.
class AllZonesIter : public ZonesIter {
 public:
  explicit AllZonesIter(gc::GCRuntime* gc) : ZonesIter(gc, WithAtoms) {}
  explicit AllZonesIter(JSRuntime* rt) : AllZonesIter(&rt->gc) {}
};

struct CompartmentsInZoneIter {
  explicit CompartmentsInZoneIter(JS::Zone* zone) : zone(zone) {
    it = zone->compartments().begin();
  }

  bool done() const {
    MOZ_ASSERT(it);
    return it < zone->compartments().begin() ||
           it >= zone->compartments().end();
  }
  void next() {
    MOZ_ASSERT(!done());
    it++;
  }

  JS::Compartment* get() const {
    MOZ_ASSERT(it);
    return *it;
  }

  operator JS::Compartment*() const { return get(); }
  JS::Compartment* operator->() const { return get(); }

 private:
  JS::Zone* zone;
  JS::Compartment** it;
};

class RealmsInCompartmentIter {
  JS::Compartment* comp;
  JS::Realm** it;

 public:
  explicit RealmsInCompartmentIter(JS::Compartment* comp) : comp(comp) {
    it = comp->realms().begin();
    MOZ_ASSERT(!done(), "Compartments must have at least one realm");
  }

  bool done() const {
    MOZ_ASSERT(it);
    return it < comp->realms().begin() || it >= comp->realms().end();
  }
  void next() {
    MOZ_ASSERT(!done());
    it++;
  }

  JS::Realm* get() const {
    MOZ_ASSERT(!done());
    return *it;
  }

  operator JS::Realm*() const { return get(); }
  JS::Realm* operator->() const { return get(); }
};

using RealmsInZoneIter =
    NestedIterator<CompartmentsInZoneIter, RealmsInCompartmentIter>;

// This iterator iterates over all the compartments or realms in a given set of
// zones. The set of zones is determined by iterating ZoneIterT. The set of
// compartments or realms is determined by InnerIterT.
template <class ZonesIterT, class InnerIterT>
class CompartmentsOrRealmsIterT
    : public NestedIterator<ZonesIterT, InnerIterT> {
  gc::AutoEnterIteration iterMarker;

 public:
  explicit CompartmentsOrRealmsIterT(gc::GCRuntime* gc)
      : NestedIterator<ZonesIterT, InnerIterT>(gc), iterMarker(gc) {}
  explicit CompartmentsOrRealmsIterT(JSRuntime* rt)
      : CompartmentsOrRealmsIterT(&rt->gc) {}
};

using CompartmentsIter =
    CompartmentsOrRealmsIterT<NonAtomZonesIter, CompartmentsInZoneIter>;
using RealmsIter =
    CompartmentsOrRealmsIterT<NonAtomZonesIter, RealmsInZoneIter>;

}  // namespace js

#endif  // gc_PublicIterators_h
