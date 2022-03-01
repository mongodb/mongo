/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/AtomMarking-inl.h"

#include <type_traits>

#include "gc/PublicIterators.h"
#include "vm/Realm.h"

#include "gc/GC-inl.h"
#include "gc/Heap-inl.h"

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

bool AtomMarkingRuntime::computeBitmapFromChunkMarkBits(JSRuntime* runtime,
                                                        DenseBitmap& bitmap) {
  MOZ_ASSERT(CurrentThreadIsPerformingGC());
  MOZ_ASSERT(!runtime->hasHelperThreadZones());

  if (!bitmap.ensureSpace(allocatedWords)) {
    return false;
  }

  Zone* atomsZone = runtime->unsafeAtomsZone();
  for (auto thingKind : AllAllocKinds()) {
    for (ArenaIter aiter(atomsZone, thingKind); !aiter.done(); aiter.next()) {
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
static void BitwiseOrIntoChunkMarkBits(JSRuntime* runtime, Bitmap& bitmap) {
  // Make sure that by copying the mark bits for one arena in word sizes we
  // do not affect the mark bits for other arenas.
  static_assert(ArenaBitmapBits == ArenaBitmapWords * JS_BITS_PER_WORD,
                "ArenaBitmapWords must evenly divide ArenaBitmapBits");

  Zone* atomsZone = runtime->unsafeAtomsZone();
  for (auto thingKind : AllAllocKinds()) {
    for (ArenaIter aiter(atomsZone, thingKind); !aiter.done(); aiter.next()) {
      Arena* arena = aiter.get();
      MarkBitmapWord* chunkWords = arena->chunk()->markBits.arenaBits(arena);
      bitmap.bitwiseOrRangeInto(arena->atomBitmapStart(), ArenaBitmapWords,
                                chunkWords);
    }
  }
}

void AtomMarkingRuntime::markAtomsUsedByUncollectedZones(JSRuntime* runtime) {
  MOZ_ASSERT(CurrentThreadIsPerformingGC());
  MOZ_ASSERT(!runtime->hasHelperThreadZones());

  // Try to compute a simple union of the zone atom bitmaps before updating
  // the chunk mark bitmaps. If this allocation fails then fall back to
  // updating the chunk mark bitmaps separately for each zone.
  DenseBitmap markedUnion;
  if (markedUnion.ensureSpace(allocatedWords)) {
    for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
      // We only need to update the chunk mark bits for zones which were
      // not collected in the current GC. Atoms which are referenced by
      // collected zones have already been marked.
      if (!zone->isCollectingFromAnyThread()) {
        zone->markedAtoms().bitwiseOrInto(markedUnion);
      }
    }
    BitwiseOrIntoChunkMarkBits(runtime, markedUnion);
  } else {
    for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
      if (!zone->isCollectingFromAnyThread()) {
        BitwiseOrIntoChunkMarkBits(runtime, zone->markedAtoms());
      }
    }
  }
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

void AtomMarkingRuntime::adoptMarkedAtoms(Zone* target, Zone* source) {
  MOZ_ASSERT(CurrentThreadCanAccessZone(source));
  MOZ_ASSERT(CurrentThreadCanAccessZone(target));
  target->markedAtoms().bitwiseOrWith(source->markedAtoms());
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
    JSRuntime* rt = zone->runtimeFromAnyThread();
    if (rt->atoms().atomIsPinned(rt, thing)) {
      return true;
    }
  }

  size_t bit = GetAtomBit(&thing->asTenured());
  return zone->markedAtoms().getBit(bit);
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

  MOZ_ASSERT_IF(value.isGCThing(), value.isObject() ||
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
