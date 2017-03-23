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

#include "js/MemoryMetrics.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

using namespace js::jit;

size_t ExecutableAllocator::pageSize = 0;
size_t ExecutableAllocator::largeAllocSize = 0;

ExecutablePool::~ExecutablePool()
{
    MOZ_ASSERT(m_ionCodeBytes == 0);
    MOZ_ASSERT(m_baselineCodeBytes == 0);
    MOZ_ASSERT(m_regexpCodeBytes == 0);
    MOZ_ASSERT(m_otherCodeBytes == 0);

    m_allocator->releasePoolPages(this);
}

/* static */ void
ExecutableAllocator::initStatic()
{
    if (!pageSize) {
        pageSize = determinePageSize();
        // On Windows, VirtualAlloc effectively allocates in 64K chunks.
        // (Technically, it allocates in page chunks, but the starting
        // address is always a multiple of 64K, so each allocation uses up
        // 64K of address space.)  So a size less than that would be
        // pointless.  But it turns out that 64KB is a reasonable size for
        // all platforms.  (This assumes 4KB pages.) On 64-bit windows,
        // AllocateExecutableMemory prepends an extra page for structured
        // exception handling data (see comments in function) onto whatever
        // is passed in, so subtract one page here.
#if defined(JS_CPU_X64) && defined(XP_WIN)
        largeAllocSize = pageSize * 15;
#else
        largeAllocSize = pageSize * 16;
#endif
    }
}

void
ExecutableAllocator::addSizeOfCode(JS::CodeSizes* sizes) const
{
    if (m_pools.initialized()) {
        for (ExecPoolHashSet::Range r = m_pools.all(); !r.empty(); r.popFront()) {
            ExecutablePool* pool = r.front();
            sizes->ion      += pool->m_ionCodeBytes;
            sizes->baseline += pool->m_baselineCodeBytes;
            sizes->regexp   += pool->m_regexpCodeBytes;
            sizes->other    += pool->m_otherCodeBytes;
            sizes->unused   += pool->m_allocation.size - pool->m_ionCodeBytes
                                                       - pool->m_baselineCodeBytes
                                                       - pool->m_regexpCodeBytes
                                                       - pool->m_otherCodeBytes;
        }
    }
}

#if TARGET_OS_IPHONE
bool ExecutableAllocator::nonWritableJitCode = true;
#else
bool ExecutableAllocator::nonWritableJitCode = false;
#endif
