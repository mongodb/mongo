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
// stub data. There are two kinds of Baseline CacheIR stubs:
//
// (1) CacheIR stubs that can make non-tail calls that can GC. These are
//     allocated in a LifoAlloc stored in JitScript.
//     See JitScriptICStubSpace.
//
// (2) Other CacheIR stubs (aka optimized IC stubs). Allocated in a per-Zone
//     LifoAlloc and purged when JIT-code is discarded.
//     See OptimizedICStubSpace.
class ICStubSpace {
 protected:
  LifoAlloc allocator_;

  explicit ICStubSpace(size_t chunkSize) : allocator_(chunkSize) {}

 public:
  inline void* alloc(size_t size) { return allocator_.alloc(size); }

  JS_DECLARE_NEW_METHODS(allocate, alloc, inline)

  void freeAllAfterMinorGC(JS::Zone* zone);

#ifdef DEBUG
  bool isEmpty() const { return allocator_.isEmpty(); }
#endif

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return allocator_.sizeOfExcludingThis(mallocSizeOf);
  }
};

// Space for optimized stubs. Every JitZone has a single OptimizedICStubSpace.
struct OptimizedICStubSpace : public ICStubSpace {
  static const size_t STUB_DEFAULT_CHUNK_SIZE = 4096;

 public:
  OptimizedICStubSpace() : ICStubSpace(STUB_DEFAULT_CHUNK_SIZE) {}
};

// Space for Can-GC stubs. Every JitScript has a JitScriptICStubSpace.
struct JitScriptICStubSpace : public ICStubSpace {
  static const size_t STUB_DEFAULT_CHUNK_SIZE = 4096;

 public:
  JitScriptICStubSpace() : ICStubSpace(STUB_DEFAULT_CHUNK_SIZE) {}
};

}  // namespace jit
}  // namespace js

#endif /* jit_ICStubSpace_h */
