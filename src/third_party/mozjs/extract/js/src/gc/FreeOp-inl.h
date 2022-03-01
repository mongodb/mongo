/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_FreeOp_inl_h
#define gc_FreeOp_inl_h

#include "gc/FreeOp.h"

#include "gc/ZoneAllocator.h"
#include "js/RefCounted.h"

inline void JSFreeOp::free_(Cell* cell, void* p, size_t nbytes, MemoryUse use) {
  if (p) {
    removeCellMemory(cell, nbytes, use);
    js_free(p);
  }
}

template <class T>
inline void JSFreeOp::release(Cell* cell, T* p, size_t nbytes, MemoryUse use) {
  if (p) {
    removeCellMemory(cell, nbytes, use);
    p->Release();
  }
}

inline void JSFreeOp::removeCellMemory(Cell* cell, size_t nbytes,
                                       MemoryUse use) {
  RemoveCellMemory(cell, nbytes, use, isCollecting());
}

#endif  // gc_JSFreeOp_inl_h
