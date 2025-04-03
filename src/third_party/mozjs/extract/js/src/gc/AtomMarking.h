/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_AtomMarking_h
#define gc_AtomMarking_h

#include "NamespaceImports.h"
#include "js/Vector.h"
#include "threading/ProtectedData.h"

namespace js {

class AutoLockGC;
class DenseBitmap;

namespace gc {

class Arena;

// This class manages state used for marking atoms during GCs.
// See AtomMarking.cpp for details.
class AtomMarkingRuntime {
  // Unused arena atom bitmap indexes. Protected by the GC lock.
  js::GCLockData<Vector<size_t, 0, SystemAllocPolicy>> freeArenaIndexes;

  inline void markChildren(JSContext* cx, JSAtom*);
  inline void markChildren(JSContext* cx, JS::Symbol* symbol);

 public:
  // The extent of all allocated and free words in atom mark bitmaps.
  // This monotonically increases and may be read from without locking.
  mozilla::Atomic<size_t, mozilla::SequentiallyConsistent> allocatedWords;

  AtomMarkingRuntime() : allocatedWords(0) {}

  // Mark an arena as holding things in the atoms zone.
  void registerArena(Arena* arena, const AutoLockGC& lock);

  // Mark an arena as no longer holding things in the atoms zone.
  void unregisterArena(Arena* arena, const AutoLockGC& lock);

  // Fill |bitmap| with an atom marking bitmap based on the things that are
  // currently marked in the chunks used by atoms zone arenas. This returns
  // false on an allocation failure (but does not report an exception).
  bool computeBitmapFromChunkMarkBits(JSRuntime* runtime, DenseBitmap& bitmap);

  // Update the atom marking bitmap in |zone| according to another
  // overapproximation of the reachable atoms in |bitmap|.
  void refineZoneBitmapForCollectedZone(Zone* zone, const DenseBitmap& bitmap);

  // Set any bits in the chunk mark bitmaps for atoms which are marked in any
  // uncollected zone in the runtime.
  void markAtomsUsedByUncollectedZones(JSRuntime* runtime);

  // Mark an atom or id as being newly reachable by the context's zone.
  template <typename T>
  void markAtom(JSContext* cx, T* thing);

  // Version of markAtom that's always inlined, for performance-sensitive
  // callers.
  template <typename T, bool Fallible>
  MOZ_ALWAYS_INLINE bool inlinedMarkAtomInternal(JSContext* cx, T* thing);
  template <typename T>
  MOZ_ALWAYS_INLINE void inlinedMarkAtom(JSContext* cx, T* thing);
  template <typename T>
  MOZ_ALWAYS_INLINE bool inlinedMarkAtomFallible(JSContext* cx, T* thing);

  void markId(JSContext* cx, jsid id);
  void markAtomValue(JSContext* cx, const Value& value);

#ifdef DEBUG
  // Return whether |thing/id| is in the atom marking bitmap for |zone|.
  template <typename T>
  bool atomIsMarked(Zone* zone, T* thing);
  bool idIsMarked(Zone* zone, jsid id);
  bool valueIsMarked(Zone* zone, const Value& value);
#endif
};

}  // namespace gc
}  // namespace js

#endif  // gc_AtomMarking_h
