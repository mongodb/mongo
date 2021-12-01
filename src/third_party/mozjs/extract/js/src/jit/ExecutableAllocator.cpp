/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "jit/ExecutableAllocator.h"

#include "jit/JitCompartment.h"
#include "js/MemoryMetrics.h"

using namespace js::jit;

ExecutablePool::~ExecutablePool()
{
#ifdef DEBUG
    for (size_t bytes : m_codeBytes)
        MOZ_ASSERT(bytes == 0);
#endif

    MOZ_ASSERT(!isMarked());

    m_allocator->releasePoolPages(this);
}

void
ExecutablePool::release(bool willDestroy)
{
    MOZ_ASSERT(m_refCount != 0);
    MOZ_ASSERT_IF(willDestroy, m_refCount == 1);
    if (--m_refCount == 0)
        js_delete(this);
}

void
ExecutablePool::release(size_t n, CodeKind kind)
{
    m_codeBytes[kind] -= n;
    MOZ_ASSERT(m_codeBytes[kind] < m_allocation.size); // Shouldn't underflow.

    release();
}

void
ExecutablePool::addRef()
{
    // It should be impossible for us to roll over, because only small
    // pools have multiple holders, and they have one holder per chunk
    // of generated code, and they only hold 16KB or so of code.
    MOZ_ASSERT(m_refCount);
    ++m_refCount;
    MOZ_ASSERT(m_refCount, "refcount overflow");
}

void*
ExecutablePool::alloc(size_t n, CodeKind kind)
{
    MOZ_ASSERT(n <= available());
    void* result = m_freePtr;
    m_freePtr += n;

    m_codeBytes[kind] += n;

    return result;
}

size_t
ExecutablePool::available() const
{
    MOZ_ASSERT(m_end >= m_freePtr);
    return m_end - m_freePtr;
}

ExecutableAllocator::ExecutableAllocator(JSRuntime* rt)
  : rt_(rt)
{
    MOZ_ASSERT(m_smallPools.empty());
}

ExecutableAllocator::~ExecutableAllocator()
{
    for (size_t i = 0; i < m_smallPools.length(); i++)
        m_smallPools[i]->release(/* willDestroy = */true);

    // If this asserts we have a pool leak.
    MOZ_ASSERT_IF(m_pools.initialized() && rt_->gc.shutdownCollectedEverything(),
                  m_pools.empty());
}

ExecutablePool*
ExecutableAllocator::poolForSize(size_t n)
{
    // Try to fit in an existing small allocator.  Use the pool with the
    // least available space that is big enough (best-fit).  This is the
    // best strategy because (a) it maximizes the chance of the next
    // allocation fitting in a small pool, and (b) it minimizes the
    // potential waste when a small pool is next abandoned.
    ExecutablePool* minPool = nullptr;
    for (size_t i = 0; i < m_smallPools.length(); i++) {
        ExecutablePool* pool = m_smallPools[i];
        if (n <= pool->available() && (!minPool || pool->available() < minPool->available()))
            minPool = pool;
    }
    if (minPool) {
        minPool->addRef();
        return minPool;
    }

    // If the request is large, we just provide a unshared allocator
    if (n > ExecutableCodePageSize)
        return createPool(n);

    // Create a new allocator
    ExecutablePool* pool = createPool(ExecutableCodePageSize);
    if (!pool)
        return nullptr;
    // At this point, local |pool| is the owner.

    if (m_smallPools.length() < maxSmallPools) {
        // We haven't hit the maximum number of live pools; add the new pool.
        // If append() OOMs, we just return an unshared allocator.
        if (m_smallPools.append(pool))
            pool->addRef();
    } else {
        // Find the pool with the least space.
        int iMin = 0;
        for (size_t i = 1; i < m_smallPools.length(); i++) {
            if (m_smallPools[i]->available() < m_smallPools[iMin]->available())
                iMin = i;
        }

        // If the new allocator will result in more free space than the small
        // pool with the least space, then we will use it instead
        ExecutablePool* minPool = m_smallPools[iMin];
        if ((pool->available() - n) > minPool->available()) {
            minPool->release();
            m_smallPools[iMin] = pool;
            pool->addRef();
        }
    }

    // Pass ownership to the caller.
    return pool;
}

/* static */ size_t
ExecutableAllocator::roundUpAllocationSize(size_t request, size_t granularity)
{
    if ((std::numeric_limits<size_t>::max() - granularity) <= request)
        return OVERSIZE_ALLOCATION;

    // Round up to next page boundary
    size_t size = request + (granularity - 1);
    size = size & ~(granularity - 1);
    MOZ_ASSERT(size >= request);
    return size;
}

ExecutablePool*
ExecutableAllocator::createPool(size_t n)
{
    MOZ_ASSERT(rt_->jitRuntime()->preventBackedgePatching());

    size_t allocSize = roundUpAllocationSize(n, ExecutableCodePageSize);
    if (allocSize == OVERSIZE_ALLOCATION)
        return nullptr;

    if (!m_pools.initialized() && !m_pools.init())
        return nullptr;

    ExecutablePool::Allocation a = systemAlloc(allocSize);
    if (!a.pages)
        return nullptr;

    ExecutablePool* pool = js_new<ExecutablePool>(this, a);
    if (!pool) {
        systemRelease(a);
        return nullptr;
    }

    if (!m_pools.put(pool)) {
        // Note: this will call |systemRelease(a)|.
        js_delete(pool);
        return nullptr;
    }

    return pool;
}

void*
ExecutableAllocator::alloc(JSContext* cx, size_t n, ExecutablePool** poolp, CodeKind type)
{
    // Don't race with reprotectAll called from the signal handler.
    JitRuntime::AutoPreventBackedgePatching apbp(rt_);

    // Caller must ensure 'n' is word-size aligned. If all allocations are
    // of word sized quantities, then all subsequent allocations will be
    // aligned.
    MOZ_ASSERT(roundUpAllocationSize(n, sizeof(void*)) == n);

    if (n == OVERSIZE_ALLOCATION) {
        *poolp = nullptr;
        return nullptr;
    }

    *poolp = poolForSize(n);
    if (!*poolp)
        return nullptr;

    // This alloc is infallible because poolForSize() just obtained
    // (found, or created if necessary) a pool that had enough space.
    void* result = (*poolp)->alloc(n, type);
    MOZ_ASSERT(result);

    cx->zone()->updateJitCodeMallocBytes(n);

    return result;
}

void
ExecutableAllocator::releasePoolPages(ExecutablePool* pool)
{
    // Don't race with reprotectAll called from the signal handler.
    JitRuntime::AutoPreventBackedgePatching apbp(rt_);

    MOZ_ASSERT(pool->m_allocation.pages);
    systemRelease(pool->m_allocation);

    MOZ_ASSERT(m_pools.initialized());

    // Pool may not be present in m_pools if we hit OOM during creation.
    if (auto ptr = m_pools.lookup(pool))
        m_pools.remove(ptr);
}

void
ExecutableAllocator::purge()
{
    // Don't race with reprotectAll called from the signal handler.
    JitRuntime::AutoPreventBackedgePatching apbp(rt_);

    for (size_t i = 0; i < m_smallPools.length(); ) {
        ExecutablePool* pool = m_smallPools[i];
        if (pool->m_refCount > 1) {
            // Releasing this pool is not going to deallocate it, so we might as
            // well hold on to it and reuse it for future allocations.
            i++;
            continue;
        }

        MOZ_ASSERT(pool->m_refCount == 1);
        pool->release();
        m_smallPools.erase(&m_smallPools[i]);
    }
}

void
ExecutableAllocator::addSizeOfCode(JS::CodeSizes* sizes) const
{
    if (m_pools.initialized()) {
        for (ExecPoolHashSet::Range r = m_pools.all(); !r.empty(); r.popFront()) {
            ExecutablePool* pool = r.front();
            sizes->ion      += pool->m_codeBytes[CodeKind::Ion];
            sizes->baseline += pool->m_codeBytes[CodeKind::Baseline];
            sizes->regexp   += pool->m_codeBytes[CodeKind::RegExp];
            sizes->other    += pool->m_codeBytes[CodeKind::Other];
            sizes->unused   += pool->m_allocation.size - pool->usedCodeBytes();
        }
    }
}

void
ExecutableAllocator::reprotectAll(ProtectionSetting protection)
{
    if (!m_pools.initialized())
        return;

    for (ExecPoolHashSet::Range r = m_pools.all(); !r.empty(); r.popFront())
        reprotectPool(rt_, r.front(), protection);
}

/* static */ void
ExecutableAllocator::reprotectPool(JSRuntime* rt, ExecutablePool* pool, ProtectionSetting protection)
{
    // Don't race with reprotectAll called from the signal handler.
    MOZ_ASSERT(rt->jitRuntime()->preventBackedgePatching() ||
               rt->activeContext()->handlingJitInterrupt());

    char* start = pool->m_allocation.pages;
    if (!ReprotectRegion(start, pool->m_freePtr - start, protection))
        MOZ_CRASH();
}

/* static */ void
ExecutableAllocator::poisonCode(JSRuntime* rt, JitPoisonRangeVector& ranges)
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

    // Don't race with reprotectAll called from the signal handler.
    JitRuntime::AutoPreventBackedgePatching apbp(rt);

#ifdef DEBUG
    // Make sure no pools have the mark bit set.
    for (size_t i = 0; i < ranges.length(); i++)
        MOZ_ASSERT(!ranges[i].pool->isMarked());
#endif

    for (size_t i = 0; i < ranges.length(); i++) {
        ExecutablePool* pool = ranges[i].pool;
        if (pool->m_refCount == 1) {
            // This is the last reference so the release() call below will
            // unmap the memory. Don't bother poisoning it.
            continue;
        }

        MOZ_ASSERT(pool->m_refCount > 1);

        // Use the pool's mark bit to indicate we made the pool writable.
        // This avoids reprotecting a pool multiple times.
        if (!pool->isMarked()) {
            reprotectPool(rt, pool, ProtectionSetting::Writable);
            pool->mark();
        }

        memset(ranges[i].start, JS_SWEPT_CODE_PATTERN, ranges[i].size);
    }

    // Make the pools executable again and drop references.
    for (size_t i = 0; i < ranges.length(); i++) {
        ExecutablePool* pool = ranges[i].pool;
        if (pool->isMarked()) {
            reprotectPool(rt, pool, ProtectionSetting::Executable);
            pool->unmark();
        }
        pool->release();
    }
}

ExecutablePool::Allocation
ExecutableAllocator::systemAlloc(size_t n)
{
    void* allocation = AllocateExecutableMemory(n, ProtectionSetting::Executable);
    ExecutablePool::Allocation alloc = { reinterpret_cast<char*>(allocation), n };
    return alloc;
}

void
ExecutableAllocator::systemRelease(const ExecutablePool::Allocation& alloc)
{
    DeallocateExecutableMemory(alloc.pages, alloc.size);
}
