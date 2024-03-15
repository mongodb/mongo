/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Heap_inl_h
#define gc_Heap_inl_h

#include "gc/Heap.h"

#include "gc/StoreBuffer.h"
#include "gc/Zone.h"
#include "util/Poison.h"
#include "vm/Runtime.h"

inline void js::gc::Arena::init(JS::Zone* zoneArg, AllocKind kind,
                                const AutoLockGC& lock) {
#ifdef DEBUG
  MOZ_MAKE_MEM_DEFINED(&zone, sizeof(zone));
  MOZ_ASSERT((uintptr_t(zone) & 0xff) == JS_FREED_ARENA_PATTERN);
#endif

  MOZ_ASSERT(firstFreeSpan.isEmpty());
  MOZ_ASSERT(!allocated());
  MOZ_ASSERT(!onDelayedMarkingList_);
  MOZ_ASSERT(!hasDelayedBlackMarking_);
  MOZ_ASSERT(!hasDelayedGrayMarking_);
  MOZ_ASSERT(!nextDelayedMarkingArena_);

  MOZ_MAKE_MEM_UNDEFINED(this, ArenaSize);

  zone = zoneArg;
  allocKind = kind;
  isNewlyCreated_ = 1;
  onDelayedMarkingList_ = 0;
  hasDelayedBlackMarking_ = 0;
  hasDelayedGrayMarking_ = 0;
  nextDelayedMarkingArena_ = 0;
  if (zone->isAtomsZone()) {
    zone->runtimeFromAnyThread()->gc.atomMarking.registerArena(this, lock);
  } else {
    bufferedCells() = &ArenaCellSet::Empty;
  }

  setAsFullyUnused();
}

inline void js::gc::Arena::release(const AutoLockGC& lock) {
  if (zone->isAtomsZone()) {
    zone->runtimeFromAnyThread()->gc.atomMarking.unregisterArena(this, lock);
  }
  setAsNotAllocated();
}

inline js::gc::ArenaCellSet*& js::gc::Arena::bufferedCells() {
  MOZ_ASSERT(zone && !zone->isAtomsZone());
  return bufferedCells_;
}

inline size_t& js::gc::Arena::atomBitmapStart() {
  MOZ_ASSERT(zone && zone->isAtomsZone());
  return atomBitmapStart_;
}

inline js::gc::NurseryCellHeader::NurseryCellHeader(AllocSite* site,
                                                    JS::TraceKind kind)
    : allocSiteAndTraceKind(MakeValue(site, kind)) {}

#endif
