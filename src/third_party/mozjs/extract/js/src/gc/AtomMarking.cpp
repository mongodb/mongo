/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/AtomMarking-inl.h"

#include <type_traits>

#include "gc/PublicIterators.h"

#include "gc/GC-inl.h"
#include "gc/Heap-inl.h"
#include "gc/PrivateIterators-inl.h"

namespace js {
namespace gc {

// [SMDOC] GC Atom Marking
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

void AtomMarkingRuntime::registerArena(Arena* arena, const AutoLockGC& lock) {
  MOZ_ASSERT(arena->getThingSize() != 0);
  MOZ_ASSERT(arena->getThingSize() % CellAlignBytes == 0);
  MOZ_ASSERT(arena->zone->isAtomsZone());

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

void AtomMarkingRuntime::unregisterArena(Arena* arena, const AutoLockGC& lock) {
  MOZ_ASSERT(arena->zone->isAtomsZone());

  // Leak these atom bits if we run out of memory.
  (void)freeArenaIndexes.ref().emplaceBack(arena->atomBitmapStart());
}

void AtomMarkingRuntime::refineZoneBitmapsForCollectedZones(
    GCRuntime* gc, size_t collectedZones) {
  // If there is more than one zone to update, copy the chunk mark bits into a
  // bitmap and AND that into the atom marking bitmap for each zone.
  DenseBitmap marked;
  if (collectedZones > 1 && computeBitmapFromChunkMarkBits(gc, marked)) {
    for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
      refineZoneBitmapForCollectedZone(zone, marked);
    }
    return;
  }

  // If there's only one zone (or on OOM), AND the mark bits for each arena into
  // the zones' atom marking bitmaps directly.
  for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
    if (zone->isAtomsZone()) {
      continue;
    }

    for (auto thingKind : AllAllocKinds()) {
      for (ArenaIterInGC aiter(gc->atomsZone(), thingKind); !aiter.done();
           aiter.next()) {
        Arena* arena = aiter.get();
        MarkBitmapWord* chunkWords = arena->chunk()->markBits.arenaBits(arena);
        zone->markedAtoms().bitwiseAndRangeWith(arena->atomBitmapStart(),
                                                ArenaBitmapWords, chunkWords);
      }
    }
  }
}

bool AtomMarkingRuntime::computeBitmapFromChunkMarkBits(GCRuntime* gc,
                                                        DenseBitmap& bitmap) {
  MOZ_ASSERT(CurrentThreadIsPerformingGC());

  if (!bitmap.ensureSpace(allocatedWords)) {
    return false;
  }

  Zone* atomsZone = gc->atomsZone();
  for (auto thingKind : AllAllocKinds()) {
    for (ArenaIterInGC aiter(atomsZone, thingKind); !aiter.done();
         aiter.next()) {
      Arena* arena = aiter.get();
      MarkBitmapWord* chunkWords = arena->chunk()->markBits.arenaBits(arena);
      bitmap.copyBitsFrom(arena->atomBitmapStart(), ArenaBitmapWords,
                          chunkWords);
    }
  }

  return true;
}

void AtomMarkingRuntime::refineZoneBitmapForCollectedZone(
    Zone* zone, const DenseBitmap& bitmap) {
  MOZ_ASSERT(zone->isCollectingFromAnyThread());

  if (zone->isAtomsZone()) {
    return;
  }

  // Take the bitwise and between the two mark bitmaps to get the best new
  // overapproximation we can. |bitmap| might include bits that are not in
  // the zone's mark bitmap, if additional zones were collected by the GC.
  zone->markedAtoms().bitwiseAndWith(bitmap);
}

// Set any bits in the chunk mark bitmaps for atoms which are marked in bitmap.
template <typename Bitmap>
static void BitwiseOrIntoChunkMarkBits(Zone* atomsZone, Bitmap& bitmap) {
  // Make sure that by copying the mark bits for one arena in word sizes we
  // do not affect the mark bits for other arenas.
  static_assert(ArenaBitmapBits == ArenaBitmapWords * JS_BITS_PER_WORD,
                "ArenaBitmapWords must evenly divide ArenaBitmapBits");

  for (auto thingKind : AllAllocKinds()) {
    for (ArenaIterInGC aiter(atomsZone, thingKind); !aiter.done();
         aiter.next()) {
      Arena* arena = aiter.get();
      MarkBitmapWord* chunkWords = arena->chunk()->markBits.arenaBits(arena);
      bitmap.bitwiseOrRangeInto(arena->atomBitmapStart(), ArenaBitmapWords,
                                chunkWords);
    }
  }
}

void AtomMarkingRuntime::markAtomsUsedByUncollectedZones(
    GCRuntime* gc, size_t uncollectedZones) {
  MOZ_ASSERT(CurrentThreadIsPerformingGC());

  // If there are no uncollected non-atom zones then there's no work to do.
  if (uncollectedZones == 0) {
    return;
  }

  // If there is more than one zone then try to compute a simple union of the
  // zone atom bitmaps before updating the chunk mark bitmaps. If there is only
  // one zone or this allocation fails then update the chunk mark bitmaps
  // separately for each zone.

  DenseBitmap markedUnion;
  if (uncollectedZones == 1 || !markedUnion.ensureSpace(allocatedWords)) {
    for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
      if (!zone->isCollecting()) {
        BitwiseOrIntoChunkMarkBits(gc->atomsZone(), zone->markedAtoms());
      }
    }
    return;
  }

  for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
    // We only need to update the chunk mark bits for zones which were
    // not collected in the current GC. Atoms which are referenced by
    // collected zones have already been marked.
    if (!zone->isCollecting()) {
      zone->markedAtoms().bitwiseOrInto(markedUnion);
    }
  }

  BitwiseOrIntoChunkMarkBits(gc->atomsZone(), markedUnion);
}

template <typename T>
void AtomMarkingRuntime::markAtom(JSContext* cx, T* thing) {
  return inlinedMarkAtom(cx, thing);
}

template void AtomMarkingRuntime::markAtom(JSContext* cx, JSAtom* thing);
template void AtomMarkingRuntime::markAtom(JSContext* cx, JS::Symbol* thing);

void AtomMarkingRuntime::markId(JSContext* cx, jsid id) {
  if (id.isAtom()) {
    markAtom(cx, id.toAtom());
    return;
  }
  if (id.isSymbol()) {
    markAtom(cx, id.toSymbol());
    return;
  }
  MOZ_ASSERT(!id.isGCThing());
}

void AtomMarkingRuntime::markAtomValue(JSContext* cx, const Value& value) {
  if (value.isString()) {
    if (value.toString()->isAtom()) {
      markAtom(cx, &value.toString()->asAtom());
    }
    return;
  }
  if (value.isSymbol()) {
    markAtom(cx, value.toSymbol());
    return;
  }
  MOZ_ASSERT_IF(value.isGCThing(), value.isObject() ||
                                       value.isPrivateGCThing() ||
                                       value.isBigInt());
}

#ifdef DEBUG
template <typename T>
bool AtomMarkingRuntime::atomIsMarked(Zone* zone, T* thing) {
  static_assert(std::is_same_v<T, JSAtom> || std::is_same_v<T, JS::Symbol>,
                "Should only be called with JSAtom* or JS::Symbol* argument");

  MOZ_ASSERT(thing);
  MOZ_ASSERT(!IsInsideNursery(thing));
  MOZ_ASSERT(thing->zoneFromAnyThread()->isAtomsZone());

  if (!zone->runtimeFromAnyThread()->permanentAtomsPopulated()) {
    return true;
  }

  if (thing->isPermanentAndMayBeShared()) {
    return true;
  }

  if constexpr (std::is_same_v<T, JSAtom>) {
    if (thing->isPinned()) {
      return true;
    }
  }

  size_t bit = GetAtomBit(&thing->asTenured());
  return zone->markedAtoms().readonlyThreadsafeGetBit(bit);
}

template bool AtomMarkingRuntime::atomIsMarked(Zone* zone, JSAtom* thing);
template bool AtomMarkingRuntime::atomIsMarked(Zone* zone, JS::Symbol* thing);

template <>
bool AtomMarkingRuntime::atomIsMarked(Zone* zone, TenuredCell* thing) {
  if (!thing) {
    return true;
  }

  if (thing->is<JSString>()) {
    JSString* str = thing->as<JSString>();
    if (!str->isAtom()) {
      return true;
    }
    return atomIsMarked(zone, &str->asAtom());
  }

  if (thing->is<JS::Symbol>()) {
    return atomIsMarked(zone, thing->as<JS::Symbol>());
  }

  return true;
}

bool AtomMarkingRuntime::idIsMarked(Zone* zone, jsid id) {
  if (id.isAtom()) {
    return atomIsMarked(zone, id.toAtom());
  }

  if (id.isSymbol()) {
    return atomIsMarked(zone, id.toSymbol());
  }

  MOZ_ASSERT(!id.isGCThing());
  return true;
}

bool AtomMarkingRuntime::valueIsMarked(Zone* zone, const Value& value) {
  if (value.isString()) {
    if (value.toString()->isAtom()) {
      return atomIsMarked(zone, &value.toString()->asAtom());
    }
    return true;
  }

  if (value.isSymbol()) {
    return atomIsMarked(zone, value.toSymbol());
  }

  MOZ_ASSERT_IF(value.isGCThing(), value.hasObjectPayload() ||
                                       value.isPrivateGCThing() ||
                                       value.isBigInt());
  return true;
}

#endif  // DEBUG

}  // namespace gc

#ifdef DEBUG

bool AtomIsMarked(Zone* zone, JSAtom* atom) {
  return zone->runtimeFromAnyThread()->gc.atomMarking.atomIsMarked(zone, atom);
}

bool AtomIsMarked(Zone* zone, jsid id) {
  return zone->runtimeFromAnyThread()->gc.atomMarking.idIsMarked(zone, id);
}

bool AtomIsMarked(Zone* zone, const Value& value) {
  return zone->runtimeFromAnyThread()->gc.atomMarking.valueIsMarked(zone,
                                                                    value);
}

#endif  // DEBUG

}  // namespace js
