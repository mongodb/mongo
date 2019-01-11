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

#include "mozilla/EnumeratedArray.h"
#include "mozilla/Maybe.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <limits>
#include <stddef.h> // for ptrdiff_t

#ifdef JS_CODEGEN_ARM
# include "jit/arm/Architecture-arm.h"
#endif
#include "jit/arm/Simulator-arm.h"
#if defined(JS_CODEGEN_ARM64)
# include "jit/arm64/vixl/Cpu-vixl.h"
#endif
#include "jit/mips32/Simulator-mips32.h"
#include "jit/mips64/Simulator-mips64.h"
#include "jit/ProcessExecutableMemory.h"
#include "js/AllocPolicy.h"
#include "js/HashTable.h"
#include "js/TypeDecls.h"
#include "js/Vector.h"

#if defined(__sparc__)
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

enum class CodeKind : uint8_t {
    Ion,
    Baseline,
    RegExp,
    Other,
    Count
};

class ExecutableAllocator;
class JitRuntime;

// These are reference-counted. A new one starts with a count of 1.
class ExecutablePool
{
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
    unsigned m_refCount:31;

    // Flag that can be used by algorithms operating on pools.
    bool m_mark:1;

    // Number of bytes currently allocated for each CodeKind.
    mozilla::EnumeratedArray<CodeKind, CodeKind::Count, size_t> m_codeBytes;

  public:
    void release(bool willDestroy = false);
    void release(size_t n, CodeKind kind);

    void addRef();

    ExecutablePool(ExecutableAllocator* allocator, Allocation a)
      : m_allocator(allocator), m_freePtr(a.pages), m_end(m_freePtr + a.size), m_allocation(a),
        m_refCount(1), m_mark(false)
    {
        for (size_t& count : m_codeBytes)
            count = 0;
    }

    ~ExecutablePool();

    void mark() {
        MOZ_ASSERT(!m_mark);
        m_mark = true;
    }
    void unmark() {
        MOZ_ASSERT(m_mark);
        m_mark = false;
    }
    bool isMarked() const {
        return m_mark;
    }

  private:
    ExecutablePool(const ExecutablePool&) = delete;
    void operator=(const ExecutablePool&) = delete;

    void* alloc(size_t n, CodeKind kind);

    size_t available() const;

    // Returns the number of bytes that are currently in use (referenced by
    // live JitCode objects).
    size_t usedCodeBytes() const {
        size_t res = 0;
        for (size_t count : m_codeBytes)
            res += count;
        return res;
    }
};

struct JitPoisonRange
{
    jit::ExecutablePool* pool;
    void* start;
    size_t size;

    JitPoisonRange(jit::ExecutablePool* pool, void* start, size_t size)
      : pool(pool), start(start), size(size)
    {}
};

typedef Vector<JitPoisonRange, 0, SystemAllocPolicy> JitPoisonRangeVector;

class ExecutableAllocator
{
    JSRuntime* rt_;

  public:
    explicit ExecutableAllocator(JSRuntime* rt);
    ~ExecutableAllocator();

    void purge();

    // alloc() returns a pointer to some memory, and also (by reference) a
    // pointer to reference-counted pool. The caller owns a reference to the
    // pool; i.e. alloc() increments the count before returning the object.
    void* alloc(JSContext* cx, size_t n, ExecutablePool** poolp, CodeKind type);

    void releasePoolPages(ExecutablePool* pool);

    void addSizeOfCode(JS::CodeSizes* sizes) const;

  private:
    static const size_t OVERSIZE_ALLOCATION = size_t(-1);

    static size_t roundUpAllocationSize(size_t request, size_t granularity);

    // On OOM, this will return an Allocation where pages is nullptr.
    ExecutablePool::Allocation systemAlloc(size_t n);
    static void systemRelease(const ExecutablePool::Allocation& alloc);

    ExecutablePool* createPool(size_t n);
    ExecutablePool* poolForSize(size_t n);

    static void reprotectPool(JSRuntime* rt, ExecutablePool* pool, ProtectionSetting protection);

  public:
    MOZ_MUST_USE
    static bool makeWritable(void* start, size_t size)
    {
        return ReprotectRegion(start, size, ProtectionSetting::Writable);
    }

    MOZ_MUST_USE
    static bool makeExecutable(void* start, size_t size)
    {
        return ReprotectRegion(start, size, ProtectionSetting::Executable);
    }

    void makeAllWritable() {
        reprotectAll(ProtectionSetting::Writable);
    }
    void makeAllExecutable() {
        reprotectAll(ProtectionSetting::Executable);
    }

    static void poisonCode(JSRuntime* rt, JitPoisonRangeVector& ranges);

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64) || defined(JS_SIMULATOR_ARM64) || defined(JS_CODEGEN_NONE)
    static void cacheFlush(void*, size_t)
    {
    }
#elif defined(JS_SIMULATOR_ARM) || defined(JS_SIMULATOR_MIPS32) || defined(JS_SIMULATOR_MIPS64)
    static void cacheFlush(void* code, size_t size)
    {
        js::jit::SimulatorProcess::FlushICache(code, size);
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
#elif (defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)) && defined(XP_IOS)
    static void cacheFlush(void* code, size_t size)
    {
        sys_icache_invalidate(code, size);
    }
#elif defined(JS_CODEGEN_ARM) && (defined(__linux__) || defined(ANDROID)) && defined(__GNUC__)
    static void cacheFlush(void* code, size_t size)
    {
        void* end = (void*)(reinterpret_cast<char*>(code) + size);
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
            : "r" (code), "r" (end)
            : "r0", "r1", "r2");

        if (ForceDoubleCacheFlush()) {
            void* start = (void*)((uintptr_t)code + 1);
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
                : "r" (start), "r" (end)
                : "r0", "r1", "r2");
        }
    }
#elif defined(JS_CODEGEN_ARM64)
    static void cacheFlush(void* code, size_t size)
    {
        vixl::CPU::EnsureIAndDCacheCoherency(code, size);
    }
#elif defined(__sparc__)
    static void cacheFlush(void* code, size_t size)
    {
        sync_instruction_memory((caddr_t)code, size);
    }
#endif

  private:
    ExecutableAllocator(const ExecutableAllocator&) = delete;
    void operator=(const ExecutableAllocator&) = delete;

    void reprotectAll(ProtectionSetting);

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
};

} // namespace jit
} // namespace js

#endif /* jit_ExecutableAllocator_h */
