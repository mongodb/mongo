/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ICStubSpace_h
#define jit_ICStubSpace_h

#include "ds/LifoAlloc.h"

namespace js {
namespace jit {

// ICStubSpace is an abstraction for allocation policy and storage for CacheIR
// stub data. Each JitZone has a single ICStubSpace.
class ICStubSpace {
  static constexpr size_t DefaultChunkSize = 4096;
  LifoAlloc allocator_{DefaultChunkSize};

 public:
  inline void* alloc(size_t size) { return allocator_.alloc(size); }

  JS_DECLARE_NEW_METHODS(allocate, alloc, inline)

  void freeAllAfterMinorGC(JS::Zone* zone);

  void transferFrom(ICStubSpace& other) {
    allocator_.transferFrom(&other.allocator_);
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return allocator_.sizeOfExcludingThis(mallocSizeOf);
  }
};

}  // namespace jit
}  // namespace js

#endif /* jit_ICStubSpace_h */
