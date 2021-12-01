/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/AtomMarking.h"

#include "vm/JSCompartment.h"

#include "gc/Heap-inl.h"

namespace js {
namespace gc {

inline size_t
GetAtomBit(TenuredCell* thing)
{
    MOZ_ASSERT(thing->zoneFromAnyThread()->isAtomsZone());
    Arena* arena = thing->arena();
    size_t arenaBit = (reinterpret_cast<uintptr_t>(thing) - arena->address()) / CellBytesPerMarkBit;
    return arena->atomBitmapStart() * JS_BITS_PER_WORD + arenaBit;
}

inline bool
ThingIsPermanent(JSAtom* atom)
{
    return atom->isPermanentAtom();
}

inline bool
ThingIsPermanent(JS::Symbol* symbol)
{
    return symbol->isWellKnownSymbol();
}

template <typename T>
MOZ_ALWAYS_INLINE void
AtomMarkingRuntime::inlinedMarkAtom(JSContext* cx, T* thing)
{
    static_assert(mozilla::IsSame<T, JSAtom>::value ||
                  mozilla::IsSame<T, JS::Symbol>::value,
                  "Should only be called with JSAtom* or JS::Symbol* argument");

    MOZ_ASSERT(thing);
    js::gc::TenuredCell* cell = &thing->asTenured();
    MOZ_ASSERT(cell->zoneFromAnyThread()->isAtomsZone());

    // The context's zone will be null during initialization of the runtime.
    if (!cx->zone())
        return;
    MOZ_ASSERT(!cx->zone()->isAtomsZone());

    if (ThingIsPermanent(thing))
        return;

    size_t bit = GetAtomBit(cell);
    MOZ_ASSERT(bit / JS_BITS_PER_WORD < allocatedWords);

    cx->zone()->markedAtoms().setBit(bit);

    if (!cx->helperThread()) {
        // Trigger a read barrier on the atom, in case there is an incremental
        // GC in progress. This is necessary if the atom is being marked
        // because a reference to it was obtained from another zone which is
        // not being collected by the incremental GC.
        T::readBarrier(thing);
    }

    // Children of the thing also need to be marked in the context's zone.
    // We don't have a JSTracer for this so manually handle the cases in which
    // an atom can reference other atoms.
    markChildren(cx, thing);
}

} // namespace gc
} // namespace js
