/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ICStubSpace_h
#define jit_ICStubSpace_h

#include "ds/LifoAlloc.h"

namespace js {
namespace jit {

// ICStubSpace is an abstraction for allocation policy and storage for stub data.
// There are two kinds of stubs: optimized stubs and fallback stubs (the latter
// also includes stubs that can make non-tail calls that can GC).
//
// Optimized stubs are allocated per-compartment and are always purged when
// JIT-code is discarded. Fallback stubs are allocated per BaselineScript and
// are only destroyed when the BaselineScript is destroyed.
class ICStubSpace
{
  protected:
    LifoAlloc allocator_;

    explicit ICStubSpace(size_t chunkSize)
      : allocator_(chunkSize)
    {}

  public:
    inline void* alloc(size_t size) {
        return allocator_.alloc(size);
    }

    JS_DECLARE_NEW_METHODS(allocate, alloc, inline)

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return allocator_.sizeOfExcludingThis(mallocSizeOf);
    }
};

// Space for optimized stubs. Every JitCompartment has a single
// OptimizedICStubSpace.
struct OptimizedICStubSpace : public ICStubSpace
{
    static const size_t STUB_DEFAULT_CHUNK_SIZE = 4 * 1024;

  public:
    OptimizedICStubSpace()
      : ICStubSpace(STUB_DEFAULT_CHUNK_SIZE)
    {}

    void free() {
        allocator_.freeAll();
    }
};

// Space for fallback stubs. Every BaselineScript has a
// FallbackICStubSpace.
struct FallbackICStubSpace : public ICStubSpace
{
    static const size_t STUB_DEFAULT_CHUNK_SIZE = 256;

  public:
    FallbackICStubSpace()
      : ICStubSpace(STUB_DEFAULT_CHUNK_SIZE)
    {}

    inline void adoptFrom(FallbackICStubSpace* other) {
        allocator_.steal(&(other->allocator_));
    }
};

} // namespace jit
} // namespace js

#endif /* jit_ICStubSpace_h */
