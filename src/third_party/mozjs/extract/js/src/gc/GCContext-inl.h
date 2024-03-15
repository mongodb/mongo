/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCContext_inl_h
#define gc_GCContext_inl_h

#include "gc/GCContext.h"

#include "gc/ZoneAllocator.h"

inline void JS::GCContext::free_(Cell* cell, void* p, size_t nbytes,
                                 MemoryUse use) {
  if (p) {
    removeCellMemory(cell, nbytes, use);
    js_free(p);
  }
}

template <class T>
inline void JS::GCContext::release(Cell* cell, T* p, size_t nbytes,
                                   MemoryUse use) {
  if (p) {
    removeCellMemory(cell, nbytes, use);
    p->Release();
  }
}

inline void JS::GCContext::removeCellMemory(Cell* cell, size_t nbytes,
                                            MemoryUse use) {
  // This may or may not be called as part of GC.
  if (nbytes && cell->isTenured()) {
    auto zone = js::ZoneAllocator::from(cell->asTenured().zoneFromAnyThread());
    zone->removeCellMemory(cell, nbytes, use, isFinalizing());
  }
}

#endif  // gc_GCContext_inl_h
