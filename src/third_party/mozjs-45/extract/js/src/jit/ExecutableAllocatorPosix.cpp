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

#include "mozilla/DebugOnly.h"
#include "mozilla/TaggedAnonymousMemory.h"

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include "jit/ExecutableAllocator.h"
#include "js/Utility.h"

using namespace js::jit;

size_t
ExecutableAllocator::determinePageSize()
{
    return getpagesize();
}

void*
js::jit::AllocateExecutableMemory(void* addr, size_t bytes, unsigned permissions, const char* tag,
                                  size_t pageSize)
{
    MOZ_ASSERT(bytes % pageSize == 0);
    void* p = MozTaggedAnonymousMmap(addr, bytes, permissions, MAP_PRIVATE | MAP_ANON, -1, 0, tag);
    return p == MAP_FAILED ? nullptr : p;
}

void
js::jit::DeallocateExecutableMemory(void* addr, size_t bytes, size_t pageSize)
{
    MOZ_ASSERT(bytes % pageSize == 0);
    mozilla::DebugOnly<int> result = munmap(addr, bytes);
    MOZ_ASSERT(!result || errno == ENOMEM);
}

ExecutablePool::Allocation
ExecutableAllocator::systemAlloc(size_t n)
{
    void* allocation = AllocateExecutableMemory(nullptr, n, initialProtectionFlags(Executable),
                                                "js-jit-code", pageSize);
    ExecutablePool::Allocation alloc = { reinterpret_cast<char*>(allocation), n };
    return alloc;
}

void
ExecutableAllocator::systemRelease(const ExecutablePool::Allocation& alloc)
{
    DeallocateExecutableMemory(alloc.pages, alloc.size, pageSize);
}

static const unsigned FLAGS_RW = PROT_READ | PROT_WRITE;
static const unsigned FLAGS_RX = PROT_READ | PROT_EXEC;

void
ExecutableAllocator::reprotectRegion(void* start, size_t size, ProtectionSetting setting)
{
    MOZ_ASSERT(nonWritableJitCode);
    MOZ_ASSERT(pageSize);

    // Calculate the start of the page containing this region,
    // and account for this extra memory within size.
    intptr_t startPtr = reinterpret_cast<intptr_t>(start);
    intptr_t pageStartPtr = startPtr & ~(pageSize - 1);
    void* pageStart = reinterpret_cast<void*>(pageStartPtr);
    size += (startPtr - pageStartPtr);

    // Round size up
    size += (pageSize - 1);
    size &= ~(pageSize - 1);

    mprotect(pageStart, size, (setting == Writable) ? FLAGS_RW : FLAGS_RX);
}

/* static */ unsigned
ExecutableAllocator::initialProtectionFlags(ProtectionSetting protection)
{
    if (!nonWritableJitCode)
        return FLAGS_RW | FLAGS_RX;

    return (protection == Writable) ? FLAGS_RW : FLAGS_RX;
}
