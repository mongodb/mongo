/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitAllocPolicy_h
#define jit_JitAllocPolicy_h

#include "mozilla/GuardObjects.h"
#include "mozilla/TypeTraits.h"

#include "jscntxt.h"

#include "ds/LifoAlloc.h"
#include "jit/InlineList.h"
#include "jit/Ion.h"

namespace js {
namespace jit {

class TempAllocator
{
    LifoAllocScope lifoScope_;

  public:
    // Most infallible JIT allocations are small, so we use a ballast of 16
    // KiB. And with a ballast of 16 KiB, a chunk size of 32 KiB works well,
    // because TempAllocators with a peak allocation size of less than 16 KiB
    // (which is most of them) only have to allocate a single chunk.
    static const size_t BallastSize;            // 16 KiB
    static const size_t PreferredLifoChunkSize; // 32 KiB

    explicit TempAllocator(LifoAlloc* lifoAlloc)
      : lifoScope_(lifoAlloc)
    { }

    void* allocateInfallible(size_t bytes)
    {
        return lifoScope_.alloc().allocInfallible(bytes);
    }

    void* allocate(size_t bytes)
    {
        void* p = lifoScope_.alloc().alloc(bytes);
        if (!ensureBallast())
            return nullptr;
        return p;
    }

    template <typename T>
    T* allocateArray(size_t n)
    {
        size_t bytes;
        if (MOZ_UNLIKELY(!CalculateAllocSize<T>(n, &bytes)))
            return nullptr;
        T* p = static_cast<T*>(lifoScope_.alloc().alloc(bytes));
        if (MOZ_UNLIKELY(!ensureBallast()))
            return nullptr;
        return p;
    }

    LifoAlloc* lifoAlloc()
    {
        return &lifoScope_.alloc();
    }

    bool ensureBallast() {
        return lifoScope_.alloc().ensureUnusedApproximate(BallastSize);
    }
};

class JitAllocPolicy
{
    TempAllocator& alloc_;

  public:
    MOZ_IMPLICIT JitAllocPolicy(TempAllocator& alloc)
      : alloc_(alloc)
    {}
    template <typename T>
    T* pod_malloc(size_t numElems) {
        size_t bytes;
        if (MOZ_UNLIKELY(!CalculateAllocSize<T>(numElems, &bytes)))
            return nullptr;
        return static_cast<T*>(alloc_.allocate(bytes));
    }
    template <typename T>
    T* pod_calloc(size_t numElems) {
        T* p = pod_malloc<T>(numElems);
        if (MOZ_LIKELY(p))
            memset(p, 0, numElems * sizeof(T));
        return p;
    }
    template <typename T>
    T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
        T* n = pod_malloc<T>(newSize);
        if (MOZ_UNLIKELY(!n))
            return n;
        MOZ_ASSERT(!(oldSize & mozilla::tl::MulOverflowMask<sizeof(T)>::value));
        memcpy(n, p, Min(oldSize * sizeof(T), newSize * sizeof(T)));
        return n;
    }
    void free_(void* p) {
    }
    void reportAllocOverflow() const {
    }
};

class OldJitAllocPolicy
{
  public:
    OldJitAllocPolicy()
    {}
    template <typename T>
    T* pod_malloc(size_t numElems) {
        size_t bytes;
        if (MOZ_UNLIKELY(!CalculateAllocSize<T>(numElems, &bytes)))
            return nullptr;
        return static_cast<T*>(GetJitContext()->temp->allocate(bytes));
    }
    void free_(void* p) {
    }
    void reportAllocOverflow() const {
    }
};

class AutoJitContextAlloc
{
    TempAllocator tempAlloc_;
    JitContext* jcx_;
    TempAllocator* prevAlloc_;

  public:
    explicit AutoJitContextAlloc(JSContext* cx)
      : tempAlloc_(&cx->tempLifoAlloc()),
        jcx_(GetJitContext()),
        prevAlloc_(jcx_->temp)
    {
        jcx_->temp = &tempAlloc_;
    }

    ~AutoJitContextAlloc() {
        MOZ_ASSERT(jcx_->temp == &tempAlloc_);
        jcx_->temp = prevAlloc_;
    }
};

struct TempObject
{
    inline void* operator new(size_t nbytes, TempAllocator& alloc) {
        return alloc.allocateInfallible(nbytes);
    }
    template <class T>
    inline void* operator new(size_t nbytes, T* pos) {
        static_assert(mozilla::IsConvertible<T*, TempObject*>::value,
                      "Placement new argument type must inherit from TempObject");
        return pos;
    }
};

template <typename T>
class TempObjectPool
{
    TempAllocator* alloc_;
    InlineForwardList<T> freed_;

  public:
    TempObjectPool()
      : alloc_(nullptr)
    {}
    void setAllocator(TempAllocator& alloc) {
        MOZ_ASSERT(freed_.empty());
        alloc_ = &alloc;
    }
    T* allocate() {
        MOZ_ASSERT(alloc_);
        if (freed_.empty())
            return new(*alloc_) T();
        return freed_.popFront();
    }
    void free(T* obj) {
        freed_.pushFront(obj);
    }
    void clear() {
        freed_.clear();
    }
};

} // namespace jit
} // namespace js

#endif /* jit_JitAllocPolicy_h */
