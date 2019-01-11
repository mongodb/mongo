/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Iterators for various data structures.
 */

#ifndef gc_PublicIterators_h
#define gc_PublicIterators_h

#include "gc/Zone.h"

namespace js {

// Iterate over all zone groups except those which may be in use by helper
// thread parse tasks.
class ZoneGroupsIter
{
    gc::AutoEnterIteration iterMarker;
    ZoneGroup** it;
    ZoneGroup** end;

  public:
    explicit ZoneGroupsIter(JSRuntime* rt) : iterMarker(&rt->gc) {
        it = rt->gc.groups().begin();
        end = rt->gc.groups().end();

        if (!done() && (*it)->usedByHelperThread())
            next();
    }

    bool done() const { return it == end; }

    void next() {
        MOZ_ASSERT(!done());
        do {
            it++;
        } while (!done() && (*it)->usedByHelperThread());
    }

    ZoneGroup* get() const {
        MOZ_ASSERT(!done());
        return *it;
    }

    operator ZoneGroup*() const { return get(); }
    ZoneGroup* operator->() const { return get(); }
};

// Using the atoms zone without holding the exclusive access lock is dangerous
// because worker threads may be using it simultaneously. Therefore, it's
// better to skip the atoms zone when iterating over zones. If you need to
// iterate over the atoms zone, consider taking the exclusive access lock first.
enum ZoneSelector {
    WithAtoms,
    SkipAtoms
};

// Iterate over all zones in one zone group.
class ZonesInGroupIter
{
    gc::AutoEnterIteration iterMarker;
    JS::Zone** it;
    JS::Zone** end;

  public:
    explicit ZonesInGroupIter(ZoneGroup* group) : iterMarker(&group->runtime->gc) {
        it = group->zones().begin();
        end = group->zones().end();
    }

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

// Iterate over all zones in the runtime, except those which may be in use by
// parse threads.
class ZonesIter
{
    ZoneGroupsIter group;
    Maybe<ZonesInGroupIter> zone;
    JS::Zone* atomsZone;

  public:
    ZonesIter(JSRuntime* rt, ZoneSelector selector)
      : group(rt), atomsZone(selector == WithAtoms ? rt->gc.atomsZone.ref() : nullptr)
    {
        if (!atomsZone && !done())
            next();
    }

    bool done() const { return !atomsZone && group.done(); }

    void next() {
        MOZ_ASSERT(!done());
        if (atomsZone)
            atomsZone = nullptr;
        while (!group.done()) {
            if (zone.isSome())
                zone.ref().next();
            else
                zone.emplace(group);
            if (zone.ref().done()) {
                zone.reset();
                group.next();
            } else {
                break;
            }
        }
    }

    JS::Zone* get() const {
        MOZ_ASSERT(!done());
        return atomsZone ? atomsZone : zone.ref().get();
    }

    operator JS::Zone*() const { return get(); }
    JS::Zone* operator->() const { return get(); }
};

struct CompartmentsInZoneIter
{
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

    JSCompartment* get() const {
        MOZ_ASSERT(it);
        return *it;
    }

    operator JSCompartment*() const { return get(); }
    JSCompartment* operator->() const { return get(); }

  private:
    JS::Zone* zone;
    JSCompartment** it;

    CompartmentsInZoneIter()
      : zone(nullptr), it(nullptr)
    {}

    // This is for the benefit of CompartmentsIterT::comp.
    friend class mozilla::Maybe<CompartmentsInZoneIter>;
};

// This iterator iterates over all the compartments in a given set of zones. The
// set of zones is determined by iterating ZoneIterT.
template<class ZonesIterT>
class CompartmentsIterT
{
    gc::AutoEnterIteration iterMarker;
    ZonesIterT zone;
    mozilla::Maybe<CompartmentsInZoneIter> comp;

  public:
    explicit CompartmentsIterT(JSRuntime* rt)
      : iterMarker(&rt->gc), zone(rt)
    {
        if (zone.done())
            comp.emplace();
        else
            comp.emplace(zone);
    }

    CompartmentsIterT(JSRuntime* rt, ZoneSelector selector)
      : iterMarker(&rt->gc), zone(rt, selector)
    {
        if (zone.done())
            comp.emplace();
        else
            comp.emplace(zone);
    }

    bool done() const { return zone.done(); }

    void next() {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(!comp.ref().done());
        comp->next();
        if (comp->done()) {
            comp.reset();
            zone.next();
            if (!zone.done())
                comp.emplace(zone);
        }
    }

    JSCompartment* get() const {
        MOZ_ASSERT(!done());
        return *comp;
    }

    operator JSCompartment*() const { return get(); }
    JSCompartment* operator->() const { return get(); }
};

typedef CompartmentsIterT<ZonesIter> CompartmentsIter;

} // namespace js

#endif // gc_PublicIterators_h
