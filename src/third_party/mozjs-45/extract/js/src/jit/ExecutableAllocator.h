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

#ifndef jit_ExecutableAllocator_h
#define jit_ExecutableAllocator_h

#include "mozilla/Maybe.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <limits>
#include <stddef.h> // for ptrdiff_t

#include "jsalloc.h"

#include "jit/arm/Simulator-arm.h"
#include "jit/mips32/Simulator-mips32.h"
#include "jit/mips64/Simulator-mips64.h"
#include "js/GCAPI.h"
#include "js/HashTable.h"
#include "js/Vector.h"

#ifdef JS_CPU_SPARC
#ifdef __linux__  // bugzilla 502369
static void sync_instruction_memory(caddr_t v, u_int len)
{
    caddr_t end = v + len;
    caddr_t p = v;
    while (p < end) {
        asm("flush %0" : : "r" (p));
        p += 32;
    }
}
#else
extern  "C" void sync_instruction_memory(caddr_t v, u_int len);
#endif
#endif

#if defined(__linux__) &&                                             \
     (defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)) &&    \
     (!defined(JS_SIMULATOR_MIPS32) && !defined(JS_SIMULATOR_MIPS64))
#include <sys/cachectl.h>
#endif

#if defined(JS_CODEGEN_ARM) && defined(XP_IOS)
#include <libkern/OSCacheControl.h>
#endif

namespace JS {
    struct CodeSizes;
} // namespace JS

namespace js {
namespace jit {
  enum CodeKind { ION_CODE = 0, BASELINE_CODE, REGEXP_CODE, OTHER_CODE };

  class ExecutableAllocator;

  // These are reference-counted. A new one starts with a count of 1.
  class ExecutablePool {

    friend class ExecutableAllocator;

  private:
    struct Allocation {
        char* pages;
        size_t size;
    };

    ExecutableAllocator* m_allocator;
    char* m_freePtr;
    char* m_end;
    Allocation m_allocation;

    // Reference count for automatic reclamation.
    unsigned m_refCount;

    // Number of bytes currently used for Method and Regexp JIT code.
    size_t m_ionCodeBytes;
    size_t m_baselineCodeBytes;
    size_t m_regexpCodeBytes;
    size_t m_otherCodeBytes;

  public:
    void release(bool willDestroy = false)
    {
        MOZ_ASSERT(m_refCount != 0);
        MOZ_ASSERT_IF(willDestroy, m_refCount == 1);
        if (--m_refCount == 0)
            js_delete(this);
    }
    void release(size_t n, CodeKind kind)
    {
        switch (kind) {
          case ION_CODE:
            m_ionCodeBytes -= n;
            MOZ_ASSERT(m_ionCodeBytes < m_allocation.size); // Shouldn't underflow.
            break;
          case BASELINE_CODE:
            m_baselineCodeBytes -= n;
            MOZ_ASSERT(m_baselineCodeBytes < m_allocation.size);
            break;
          case REGEXP_CODE:
            m_regexpCodeBytes -= n;
            MOZ_ASSERT(m_regexpCodeBytes < m_allocation.size);
            break;
          case OTHER_CODE:
            m_otherCodeBytes -= n;
            MOZ_ASSERT(m_otherCodeBytes < m_allocation.size);
            break;
          default:
            MOZ_CRASH("bad code kind");
        }

        release();
    }

    ExecutablePool(ExecutableAllocator* allocator, Allocation a)
      : m_allocator(allocator), m_freePtr(a.pages), m_end(m_freePtr + a.size), m_allocation(a),
        m_refCount(1), m_ionCodeBytes(0), m_baselineCodeBytes(0), m_regexpCodeBytes(0),
        m_otherCodeBytes(0)
    { }

    ~ExecutablePool();

  private:
    ExecutablePool(const ExecutablePool&) = delete;
    void operator=(const ExecutablePool&) = delete;

    // It should be impossible for us to roll over, because only small
    // pools have multiple holders, and they have one holder per chunk
    // of generated code, and they only hold 16KB or so of code.
    void addRef()
    {
        MOZ_ASSERT(m_refCount);
        ++m_refCount;
    }

    void* alloc(size_t n, CodeKind kind)
    {
        MOZ_ASSERT(n <= available());
        void* result = m_freePtr;
        m_freePtr += n;

        switch (kind) {
          case ION_CODE:      m_ionCodeBytes      += n;        break;
          case BASELINE_CODE: m_baselineCodeBytes += n;        break;
          case REGEXP_CODE:   m_regexpCodeBytes   += n;        break;
          case OTHER_CODE:    m_otherCodeBytes    += n;        break;
          default:            MOZ_CRASH("bad code kind");
        }
        return result;
    }

    size_t available() const {
        MOZ_ASSERT(m_end >= m_freePtr);
        return m_end - m_freePtr;
    }
};

class ExecutableAllocator
{
    typedef void (*DestroyCallback)(void* addr, size_t size);
    DestroyCallback destroyCallback;

#ifdef XP_WIN
    mozilla::Maybe<mozilla::non_crypto::XorShift128PlusRNG> randomNumberGenerator;
#endif

  public:
    enum ProtectionSetting { Writable, Executable };

    ExecutableAllocator()
      : destroyCallback(nullptr)
    {
        MOZ_ASSERT(m_smallPools.empty());
    }

    ~ExecutableAllocator()
    {
        for (size_t i = 0; i < m_smallPools.length(); i++)
            m_smallPools[i]->release(/* willDestroy = */true);

        // If this asserts we have a pool leak.
        MOZ_ASSERT_IF(m_pools.initialized(), m_pools.empty());
    }

    void purge() {
        for (size_t i = 0; i < m_smallPools.length(); i++)
            m_smallPools[i]->release();

        m_smallPools.clear();
    }

    // alloc() returns a pointer to some memory, and also (by reference) a
    // pointer to reference-counted pool. The caller owns a reference to the
    // pool; i.e. alloc() increments the count before returning the object.
    void* alloc(size_t n, ExecutablePool** poolp, CodeKind type)
    {
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
        return result;
    }

    void releasePoolPages(ExecutablePool* pool) {
        MOZ_ASSERT(pool->m_allocation.pages);
        if (destroyCallback) {
            // Do not allow GC during the page release callback.
            JS::AutoSuppressGCAnalysis nogc;
            destroyCallback(pool->m_allocation.pages, pool->m_allocation.size);
        }
        systemRelease(pool->m_allocation);
        MOZ_ASSERT(m_pools.initialized());

        // Pool may not be present in m_pools if we hit OOM during creation.
        auto ptr = m_pools.lookup(pool);
        if (ptr)
            m_pools.remove(ptr);
    }

    void addSizeOfCode(JS::CodeSizes* sizes) const;

    void setDestroyCallback(DestroyCallback destroyCallback) {
        this->destroyCallback = destroyCallback;
    }

    static void initStatic();

    static bool nonWritableJitCode;

  private:
    static size_t pageSize;
    static size_t largeAllocSize;

    static const size_t OVERSIZE_ALLOCATION = size_t(-1);

    static size_t roundUpAllocationSize(size_t request, size_t granularity)
    {
        // Something included via windows.h defines a macro with this name,
        // which causes the function below to fail to compile.
        #ifdef _MSC_VER
        # undef max
        #endif

        if ((std::numeric_limits<size_t>::max() - granularity) <= request)
            return OVERSIZE_ALLOCATION;

        // Round up to next page boundary
        size_t size = request + (granularity - 1);
        size = size & ~(granularity - 1);
        MOZ_ASSERT(size >= request);
        return size;
    }

    // On OOM, this will return an Allocation where pages is nullptr.
    ExecutablePool::Allocation systemAlloc(size_t n);
    static void systemRelease(const ExecutablePool::Allocation& alloc);
    void* computeRandomAllocationAddress();

    ExecutablePool* createPool(size_t n)
    {
        size_t allocSize = roundUpAllocationSize(n, pageSize);
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
            js_delete(pool);
            systemRelease(a);
            return nullptr;
        }

        return pool;
    }

  public:
    ExecutablePool* poolForSize(size_t n)
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
        if (n > largeAllocSize)
            return createPool(n);

        // Create a new allocator
        ExecutablePool* pool = createPool(largeAllocSize);
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
                if (m_smallPools[i]->available() <
                    m_smallPools[iMin]->available())
                {
                    iMin = i;
                }
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

    static void makeWritable(void* start, size_t size)
    {
        if (nonWritableJitCode)
            reprotectRegion(start, size, Writable);
    }

    static void makeExecutable(void* start, size_t size)
    {
        if (nonWritableJitCode)
            reprotectRegion(start, size, Executable);
    }

    static unsigned initialProtectionFlags(ProtectionSetting protection);

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    static void cacheFlush(void*, size_t)
    {
    }
#elif defined(JS_SIMULATOR_ARM) || defined(JS_SIMULATOR_MIPS32) || defined(JS_SIMULATOR_MIPS64)
    static void cacheFlush(void* code, size_t size)
    {
        js::jit::Simulator::FlushICache(code, size);
    }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    static void cacheFlush(void* code, size_t size)
    {
#if defined(_MIPS_ARCH_LOONGSON3A)
        // On Loongson3-CPUs, The cache flushed automatically
        // by hardware. Just need to execute an instruction hazard.
        uintptr_t tmp;
        asm volatile (
            ".set   push \n"
            ".set   noreorder \n"
            "move   %[tmp], $ra \n"
            "bal    1f \n"
            "daddiu $ra, 8 \n"
            "1: \n"
            "jr.hb  $ra \n"
            "move   $ra, %[tmp] \n"
            ".set   pop\n"
            :[tmp]"=&r"(tmp)
        );
#elif defined(__GNUC__)
        intptr_t end = reinterpret_cast<intptr_t>(code) + size;
        __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(end));
#else
        _flush_cache(reinterpret_cast<char*>(code), size, BCACHE);
#endif
    }
#elif defined(JS_CODEGEN_ARM) && (defined(__FreeBSD__) || defined(__NetBSD__))
    static void cacheFlush(void* code, size_t size)
    {
        __clear_cache(code, reinterpret_cast<char*>(code) + size);
    }
#elif defined(JS_CODEGEN_ARM) && defined(XP_IOS)
    static void cacheFlush(void* code, size_t size)
    {
        sys_icache_invalidate(code, size);
    }
#elif defined(JS_CODEGEN_ARM) && (defined(__linux__) || defined(ANDROID)) && defined(__GNUC__)
    static void cacheFlush(void* code, size_t size)
    {
        asm volatile (
            "push    {r7}\n"
            "mov     r0, %0\n"
            "mov     r1, %1\n"
            "mov     r7, #0xf0000\n"
            "add     r7, r7, #0x2\n"
            "mov     r2, #0x0\n"
            "svc     0x0\n"
            "pop     {r7}\n"
            :
            : "r" (code), "r" (reinterpret_cast<char*>(code) + size)
            : "r0", "r1", "r2");
    }
#elif JS_CPU_SPARC
    static void cacheFlush(void* code, size_t size)
    {
        sync_instruction_memory((caddr_t)code, size);
    }
#endif

  private:
    ExecutableAllocator(const ExecutableAllocator&) = delete;
    void operator=(const ExecutableAllocator&) = delete;

    static void reprotectRegion(void*, size_t, ProtectionSetting);

    // These are strong references;  they keep pools alive.
    static const size_t maxSmallPools = 4;
    typedef js::Vector<ExecutablePool*, maxSmallPools, js::SystemAllocPolicy> SmallExecPoolVector;
    SmallExecPoolVector m_smallPools;

    // All live pools are recorded here, just for stats purposes.  These are
    // weak references;  they don't keep pools alive.  When a pool is destroyed
    // its reference is removed from m_pools.
    typedef js::HashSet<ExecutablePool*, js::DefaultHasher<ExecutablePool*>, js::SystemAllocPolicy>
            ExecPoolHashSet;
    ExecPoolHashSet m_pools;    // All pools, just for stats purposes.

    static size_t determinePageSize();
};

extern void*
AllocateExecutableMemory(void* addr, size_t bytes, unsigned permissions, const char* tag,
                         size_t pageSize);

extern void
DeallocateExecutableMemory(void* addr, size_t bytes, size_t pageSize);

} // namespace jit
} // namespace js

#endif /* jit_ExecutableAllocator_h */
