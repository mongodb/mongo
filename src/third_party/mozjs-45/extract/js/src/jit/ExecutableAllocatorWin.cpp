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

#include "mozilla/WindowsVersion.h"

#include "jsfriendapi.h"
#include "jsmath.h"
#include "jswin.h"

#include "jit/ExecutableAllocator.h"

using namespace js::jit;

size_t
ExecutableAllocator::determinePageSize()
{
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    return system_info.dwPageSize;
}

void*
ExecutableAllocator::computeRandomAllocationAddress()
{
    /*
     * Inspiration is V8's OS::Allocate in platform-win32.cc.
     *
     * VirtualAlloc takes 64K chunks out of the virtual address space, so we
     * keep 16b alignment.
     *
     * x86: V8 comments say that keeping addresses in the [64MiB, 1GiB) range
     * tries to avoid system default DLL mapping space. In the end, we get 13
     * bits of randomness in our selection.
     * x64: [2GiB, 4TiB), with 25 bits of randomness.
     */
#ifdef JS_CPU_X64
    static const uintptr_t base = 0x0000000080000000;
    static const uintptr_t mask = 0x000003ffffff0000;
#elif defined(JS_CPU_X86)
    static const uintptr_t base = 0x04000000;
    static const uintptr_t mask = 0x3fff0000;
#else
# error "Unsupported architecture"
#endif

    if (randomNumberGenerator.isNothing()) {
        mozilla::Array<uint64_t, 2> seed;
        js::GenerateXorShift128PlusSeed(seed);
        randomNumberGenerator.emplace(seed[0], seed[1]);
    }

    uint64_t rand = randomNumberGenerator.ref().next();
    return (void*) (base | (rand & mask));
}

static bool
RandomizeIsBrokenImpl()
{
    // We disable everything before Vista, for now.
    return !mozilla::IsVistaOrLater();
}

static bool
RandomizeIsBroken()
{
    // Use the compiler's intrinsic guards for |static type value = expr| to avoid some potential
    // races if runtimes are created from multiple threads.
    static int result = RandomizeIsBrokenImpl();
    return !!result;
}

#ifdef JS_CPU_X64
static js::JitExceptionHandler sJitExceptionHandler;

JS_FRIEND_API(void)
js::SetJitExceptionHandler(JitExceptionHandler handler)
{
    MOZ_ASSERT(!sJitExceptionHandler);
    sJitExceptionHandler = handler;
}

// From documentation for UNWIND_INFO on
// http://msdn.microsoft.com/en-us/library/ddssxxy8.aspx
struct UnwindInfo
{
    uint8_t version : 3;
    uint8_t flags : 5;
    uint8_t sizeOfPrologue;
    uint8_t countOfUnwindCodes;
    uint8_t frameRegister : 4;
    uint8_t frameOffset : 4;
    ULONG exceptionHandler;
};

static const unsigned ThunkLength = 12;

struct ExceptionHandlerRecord
{
    RUNTIME_FUNCTION runtimeFunction;
    UnwindInfo unwindInfo;
    uint8_t thunk[ThunkLength];
};

// This function must match the function pointer type PEXCEPTION_HANDLER
// mentioned in:
//   http://msdn.microsoft.com/en-us/library/ssa62fwe.aspx.
// This type is rather elusive in documentation; Wine is the best I've found:
//   http://source.winehq.org/source/include/winnt.h
static DWORD
ExceptionHandler(PEXCEPTION_RECORD exceptionRecord, _EXCEPTION_REGISTRATION_RECORD*,
                 PCONTEXT context, _EXCEPTION_REGISTRATION_RECORD**)
{
    return sJitExceptionHandler(exceptionRecord, context);
}

// For an explanation of the problem being solved here, see
// SetJitExceptionFilter in jsfriendapi.h.
static bool
RegisterExecutableMemory(void* p, size_t bytes, size_t pageSize)
{
    ExceptionHandlerRecord* r = reinterpret_cast<ExceptionHandlerRecord*>(p);

    // All these fields are specified to be offsets from the base of the
    // executable code (which is 'p'), even if they have 'Address' in their
    // names. In particular, exceptionHandler is a ULONG offset which is a
    // 32-bit integer. Since 'p' can be farther than INT32_MAX away from
    // sJitExceptionHandler, we must generate a little thunk inside the
    // record. The record is put on its own page so that we can take away write
    // access to protect against accidental clobbering.

    r->runtimeFunction.BeginAddress = pageSize;
    r->runtimeFunction.EndAddress = (DWORD)bytes;
    r->runtimeFunction.UnwindData = offsetof(ExceptionHandlerRecord, unwindInfo);

    r->unwindInfo.version = 1;
    r->unwindInfo.flags = UNW_FLAG_EHANDLER;
    r->unwindInfo.sizeOfPrologue = 0;
    r->unwindInfo.countOfUnwindCodes = 0;
    r->unwindInfo.frameRegister = 0;
    r->unwindInfo.frameOffset = 0;
    r->unwindInfo.exceptionHandler = offsetof(ExceptionHandlerRecord, thunk);

    // mov imm64, rax
    r->thunk[0]  = 0x48;
    r->thunk[1]  = 0xb8;
    void* handler = JS_FUNC_TO_DATA_PTR(void*, ExceptionHandler);
    memcpy(&r->thunk[2], &handler, 8);

    // jmp rax
    r->thunk[10] = 0xff;
    r->thunk[11] = 0xe0;

    DWORD oldProtect;
    if (!VirtualProtect(p, pageSize, PAGE_EXECUTE_READ, &oldProtect))
        return false;

    return RtlAddFunctionTable(&r->runtimeFunction, 1, reinterpret_cast<DWORD64>(p));
}

static void
UnregisterExecutableMemory(void* p, size_t bytes, size_t pageSize)
{
    ExceptionHandlerRecord* r = reinterpret_cast<ExceptionHandlerRecord*>(p);
    RtlDeleteFunctionTable(&r->runtimeFunction);
}
#endif

void*
js::jit::AllocateExecutableMemory(void* addr, size_t bytes, unsigned permissions, const char* tag,
                                  size_t pageSize)
{
    MOZ_ASSERT(bytes % pageSize == 0);

#ifdef JS_CPU_X64
    if (sJitExceptionHandler)
        bytes += pageSize;
#endif

    void* p = VirtualAlloc(addr, bytes, MEM_COMMIT | MEM_RESERVE, permissions);
    if (!p)
        return nullptr;

#ifdef JS_CPU_X64
    if (sJitExceptionHandler) {
        if (!RegisterExecutableMemory(p, bytes, pageSize)) {
            VirtualFree(p, 0, MEM_RELEASE);
            return nullptr;
        }

        p = (uint8_t*)p + pageSize;
    }
#endif

    return p;
}

void
js::jit::DeallocateExecutableMemory(void* addr, size_t bytes, size_t pageSize)
{
    MOZ_ASSERT(bytes % pageSize == 0);

#ifdef JS_CPU_X64
    if (sJitExceptionHandler) {
        addr = (uint8_t*)addr - pageSize;
        UnregisterExecutableMemory(addr, bytes, pageSize);
    }
#endif

    VirtualFree(addr, 0, MEM_RELEASE);
}

ExecutablePool::Allocation
ExecutableAllocator::systemAlloc(size_t n)
{
    void* allocation = nullptr;
    if (!RandomizeIsBroken()) {
        void* randomAddress = computeRandomAllocationAddress();
        allocation = AllocateExecutableMemory(randomAddress, n, initialProtectionFlags(Executable),
                                              "js-jit-code", pageSize);
    }
    if (!allocation) {
        allocation = AllocateExecutableMemory(nullptr, n, initialProtectionFlags(Executable),
                                              "js-jit-code", pageSize);
    }
    ExecutablePool::Allocation alloc = { reinterpret_cast<char*>(allocation), n };
    return alloc;
}

void
ExecutableAllocator::systemRelease(const ExecutablePool::Allocation& alloc)
{
    DeallocateExecutableMemory(alloc.pages, alloc.size, pageSize);
}

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

    DWORD oldProtect;
    int flags = (setting == Writable) ? PAGE_READWRITE : PAGE_EXECUTE_READ;
    if (!VirtualProtect(pageStart, size, flags, &oldProtect))
        MOZ_CRASH();
}

/* static */ unsigned
ExecutableAllocator::initialProtectionFlags(ProtectionSetting protection)
{
    if (!nonWritableJitCode)
        return PAGE_EXECUTE_READWRITE;

    return (protection == Writable) ? PAGE_READWRITE : PAGE_EXECUTE_READ;
}
