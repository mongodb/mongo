/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/AtomMarking.h"

#include "mozilla/Assertions.h"

#include <type_traits>

#include "vm/JSContext.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"

#include "gc/Heap-inl.h"

namespace js {
namespace gc {

inline size_t GetAtomBit(TenuredCell* thing) {
  MOZ_ASSERT(thing->zoneFromAnyThread()->isAtomsZone());
  Arena* arena = thing->arena();
  size_t arenaBit = (reinterpret_cast<uintptr_t>(thing) - arena->address()) /
                    CellBytesPerMarkBit;
  return arena->atomBitmapStart() * JS_BITS_PER_WORD + arenaBit;
}

template <typename T, bool Fallible>
MOZ_ALWAYS_INLINE bool AtomMarkingRuntime::inlinedMarkAtomInternal(
    JSContext* cx, T* thing) {
  static_assert(std::is_same_v<T, JSAtom> || std::is_same_v<T, JS::Symbol>,
                "Should only be called with JSAtom* or JS::Symbol* argument");

  MOZ_ASSERT(cx->zone());
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  MOZ_ASSERT(thing);
  js::gc::TenuredCell* cell = &thing->asTenured();
  MOZ_ASSERT(cell->zoneFromAnyThread()->isAtomsZone());

  // This doesn't check for pinned atoms since that might require taking a
  // lock. This is not required for correctness.
  if (thing->isPermanentAndMayBeShared()) {
    return true;
  }

  size_t bit = GetAtomBit(cell);
  MOZ_ASSERT(bit / JS_BITS_PER_WORD < allocatedWords);

  if (Fallible) {
    if (!cx->zone()->markedAtoms().setBitFallible(bit)) {
      return false;
    }
  } else {
    cx->zone()->markedAtoms().setBit(bit);
  }

  // Trigger a read barrier on the atom, in case there is an incremental
  // GC in progress. This is necessary if the atom is being marked
  // because a reference to it was obtained from another zone which is
  // not being collected by the incremental GC.
  ReadBarrier(thing);

  // Children of the thing also need to be marked in the context's zone.
  // We don't have a JSTracer for this so manually handle the cases in which
  // an atom can reference other atoms.
  markChildren(cx, thing);

  return true;
}

void AtomMarkingRuntime::markChildren(JSContext* cx, JSAtom*) {}

void AtomMarkingRuntime::markChildren(JSContext* cx, JS::Symbol* symbol) {
  if (JSAtom* description = symbol->description()) {
    markAtom(cx, description);
  }
}

template <typename T>
MOZ_ALWAYS_INLINE void AtomMarkingRuntime::inlinedMarkAtom(JSContext* cx,
                                                           T* thing) {
  MOZ_ALWAYS_TRUE((inlinedMarkAtomInternal<T, false>(cx, thing)));
}

template <typename T>
MOZ_ALWAYS_INLINE bool AtomMarkingRuntime::inlinedMarkAtomFallible(
    JSContext* cx, T* thing) {
  return inlinedMarkAtomInternal<T, true>(cx, thing);
}

}  // namespace gc
}  // namespace js
