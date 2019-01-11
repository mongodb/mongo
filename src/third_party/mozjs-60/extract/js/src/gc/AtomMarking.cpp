/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/AtomMarking-inl.h"

#include "gc/PublicIterators.h"
#include "vm/JSCompartment.h"

#include "gc/GC-inl.h"
#include "gc/Heap-inl.h"

namespace js {
namespace gc {

// Atom Marking Overview
//
// Things in the atoms zone (which includes atomized strings and other things,
// all of which we will refer to as 'atoms' here) may be pointed to freely by
// things in other zones. To avoid the need to perform garbage collections of
// the entire runtime to collect atoms, we compute a separate atom mark bitmap
// for each zone that is always an overapproximation of the atoms that zone is
// using. When an atom is not in the mark bitmap for any zone, it can be
// destroyed.
//
// To minimize interference with the rest of the GC, atom marking and sweeping
// is done by manipulating the mark bitmaps in the chunks used for the atoms.
// When the atoms zone is being collected, the mark bitmaps for the chunk(s)
// used by the atoms are updated normally during marking. After marking
// finishes, the chunk mark bitmaps are translated to a more efficient atom mark
// bitmap (see below) that is stored on the zones which the GC collected
// (computeBitmapFromChunkMarkBits). Before sweeping begins, the chunk mark
// bitmaps are updated with any atoms that might be referenced by zones which
// weren't collected (markAtomsUsedByUncollectedZones). The GC sweeping will
// then release all atoms which are not marked by any zone.
//
// The representation of atom mark bitmaps is as follows:
//
// Each arena in the atoms zone has an atomBitmapStart() value indicating the
// word index into the bitmap of the first thing in the arena. Each arena uses
// ArenaBitmapWords of data to store its bitmap, which uses the same
// representation as chunk mark bitmaps: one bit is allocated per Cell, with
// bits for space between things being unused when things are larger than a
// single Cell.

void
AtomMarkingRuntime::registerArena(Arena* arena)
{
    MOZ_ASSERT(arena->getThingSize() != 0);
    MOZ_ASSERT(arena->getThingSize() % CellAlignBytes == 0);
    MOZ_ASSERT(arena->zone->isAtomsZone());
    MOZ_ASSERT(arena->zone->runtimeFromAnyThread()->currentThreadHasExclusiveAccess());

    // We need to find a range of bits from the atoms bitmap for this arena.

    // Look for a free range of bits compatible with this arena.
    if (freeArenaIndexes.ref().length()) {
        arena->atomBitmapStart() = freeArenaIndexes.ref().popCopy();
        return;
    }

    // Allocate a range of bits from the end for this arena.
    arena->atomBitmapStart() = allocatedWords;
    allocatedWords += ArenaBitmapWords;
}

void
AtomMarkingRuntime::unregisterArena(Arena* arena)
{
    MOZ_ASSERT(arena->zone->isAtomsZone());

    // Leak these atom bits if we run out of memory.
    mozilla::Unused << freeArenaIndexes.ref().emplaceBack(arena->atomBitmapStart());
}

bool
AtomMarkingRuntime::computeBitmapFromChunkMarkBits(JSRuntime* runtime, DenseBitmap& bitmap)
{
    MOZ_ASSERT(runtime->currentThreadHasExclusiveAccess());

    if (!bitmap.ensureSpace(allocatedWords))
        return false;

    Zone* atomsZone = runtime->unsafeAtomsCompartment()->zone();
    for (auto thingKind : AllAllocKinds()) {
        for (ArenaIter aiter(atomsZone, thingKind); !aiter.done(); aiter.next()) {
            Arena* arena = aiter.get();
            uintptr_t* chunkWords = arena->chunk()->bitmap.arenaBits(arena);
            bitmap.copyBitsFrom(arena->atomBitmapStart(), ArenaBitmapWords, chunkWords);
        }
    }

    return true;
}

void
AtomMarkingRuntime::refineZoneBitmapForCollectedZone(Zone* zone, const DenseBitmap& bitmap)
{
    MOZ_ASSERT(zone->isCollectingFromAnyThread());

    if (zone->isAtomsZone())
        return;

    // Take the bitwise and between the two mark bitmaps to get the best new
    // overapproximation we can. |bitmap| might include bits that are not in
    // the zone's mark bitmap, if additional zones were collected by the GC.
    zone->markedAtoms().bitwiseAndWith(bitmap);
}

// Set any bits in the chunk mark bitmaps for atoms which are marked in bitmap.
template <typename Bitmap>
static void
BitwiseOrIntoChunkMarkBits(JSRuntime* runtime, Bitmap& bitmap)
{
    // Make sure that by copying the mark bits for one arena in word sizes we
    // do not affect the mark bits for other arenas.
    static_assert(ArenaBitmapBits == ArenaBitmapWords * JS_BITS_PER_WORD,
                  "ArenaBitmapWords must evenly divide ArenaBitmapBits");

    Zone* atomsZone = runtime->unsafeAtomsCompartment()->zone();
    for (auto thingKind : AllAllocKinds()) {
        for (ArenaIter aiter(atomsZone, thingKind); !aiter.done(); aiter.next()) {
            Arena* arena = aiter.get();
            uintptr_t* chunkWords = arena->chunk()->bitmap.arenaBits(arena);
            bitmap.bitwiseOrRangeInto(arena->atomBitmapStart(), ArenaBitmapWords, chunkWords);
        }
    }
}

void
AtomMarkingRuntime::markAtomsUsedByUncollectedZones(JSRuntime* runtime)
{
    MOZ_ASSERT(runtime->currentThreadHasExclusiveAccess());

    // Try to compute a simple union of the zone atom bitmaps before updating
    // the chunk mark bitmaps. If this allocation fails then fall back to
    // updating the chunk mark bitmaps separately for each zone.
    DenseBitmap markedUnion;
    if (markedUnion.ensureSpace(allocatedWords)) {
        for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
            // We only need to update the chunk mark bits for zones which were
            // not collected in the current GC. Atoms which are referenced by
            // collected zones have already been marked.
            if (!zone->isCollectingFromAnyThread())
                zone->markedAtoms().bitwiseOrInto(markedUnion);
        }
        BitwiseOrIntoChunkMarkBits(runtime, markedUnion);
    } else {
        for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
            if (!zone->isCollectingFromAnyThread())
                BitwiseOrIntoChunkMarkBits(runtime, zone->markedAtoms());
        }
    }
}

template <typename T>
void
AtomMarkingRuntime::markAtom(JSContext* cx, T* thing)
{
    return inlinedMarkAtom(cx, thing);
}

template void AtomMarkingRuntime::markAtom(JSContext* cx, JSAtom* thing);
template void AtomMarkingRuntime::markAtom(JSContext* cx, JS::Symbol* thing);

void
AtomMarkingRuntime::markId(JSContext* cx, jsid id)
{
    if (JSID_IS_ATOM(id)) {
        markAtom(cx, JSID_TO_ATOM(id));
        return;
    }
    if (JSID_IS_SYMBOL(id)) {
        markAtom(cx, JSID_TO_SYMBOL(id));
        return;
    }
    MOZ_ASSERT(!JSID_IS_GCTHING(id));
}

void
AtomMarkingRuntime::markAtomValue(JSContext* cx, const Value& value)
{
    if (value.isString()) {
        if (value.toString()->isAtom())
            markAtom(cx, &value.toString()->asAtom());
        return;
    }
    if (value.isSymbol()) {
        markAtom(cx, value.toSymbol());
        return;
    }
    MOZ_ASSERT_IF(value.isGCThing(), value.isObject() || value.isPrivateGCThing());
}

void
AtomMarkingRuntime::adoptMarkedAtoms(Zone* target, Zone* source)
{
    MOZ_ASSERT(target->runtimeFromAnyThread()->currentThreadHasExclusiveAccess());
    target->markedAtoms().bitwiseOrWith(source->markedAtoms());
}

#ifdef DEBUG
template <typename T>
bool
AtomMarkingRuntime::atomIsMarked(Zone* zone, T* thing)
{
    static_assert(mozilla::IsSame<T, JSAtom>::value ||
                  mozilla::IsSame<T, JS::Symbol>::value,
                  "Should only be called with JSAtom* or JS::Symbol* argument");

    MOZ_ASSERT(thing);
    MOZ_ASSERT(!IsInsideNursery(thing));
    MOZ_ASSERT(thing->zoneFromAnyThread()->isAtomsZone());

    if (!zone->runtimeFromAnyThread()->permanentAtoms)
        return true;

    if (ThingIsPermanent(thing))
        return true;

    if (mozilla::IsSame<T, JSAtom>::value) {
        JSAtom* atom = reinterpret_cast<JSAtom*>(thing);
        if (AtomIsPinnedInRuntime(zone->runtimeFromAnyThread(), atom))
            return true;
    }

    size_t bit = GetAtomBit(&thing->asTenured());
    return zone->markedAtoms().getBit(bit);
}

template bool AtomMarkingRuntime::atomIsMarked(Zone* zone, JSAtom* thing);
template bool AtomMarkingRuntime::atomIsMarked(Zone* zone, JS::Symbol* thing);

template<>
bool
AtomMarkingRuntime::atomIsMarked(Zone* zone, TenuredCell* thing)
{
    if (!thing)
        return true;

    if (thing->is<JSString>()) {
        JSString* str = thing->as<JSString>();
        if (!str->isAtom())
            return true;
        return atomIsMarked(zone, &str->asAtom());
    }

    if (thing->is<JS::Symbol>())
        return atomIsMarked(zone, thing->as<JS::Symbol>());

    return true;
}

bool
AtomMarkingRuntime::idIsMarked(Zone* zone, jsid id)
{
    if (JSID_IS_ATOM(id))
        return atomIsMarked(zone, JSID_TO_ATOM(id));

    if (JSID_IS_SYMBOL(id))
        return atomIsMarked(zone, JSID_TO_SYMBOL(id));

    MOZ_ASSERT(!JSID_IS_GCTHING(id));
    return true;
}

bool
AtomMarkingRuntime::valueIsMarked(Zone* zone, const Value& value)
{
    if (value.isString()) {
        if (value.toString()->isAtom())
            return atomIsMarked(zone, &value.toString()->asAtom());
        return true;
    }

    if (value.isSymbol())
        return atomIsMarked(zone, value.toSymbol());

    MOZ_ASSERT_IF(value.isGCThing(), value.isObject() || value.isPrivateGCThing());
    return true;
}

#endif // DEBUG

} // namespace gc

#ifdef DEBUG

bool
AtomIsMarked(Zone* zone, JSAtom* atom)
{
    return zone->runtimeFromAnyThread()->gc.atomMarking.atomIsMarked(zone, atom);
}

bool
AtomIsMarked(Zone* zone, jsid id)
{
    return zone->runtimeFromAnyThread()->gc.atomMarking.idIsMarked(zone, id);
}

bool
AtomIsMarked(Zone* zone, const Value& value)
{
    return zone->runtimeFromAnyThread()->gc.atomMarking.valueIsMarked(zone, value);
}

#endif // DEBUG

} // namespace js
