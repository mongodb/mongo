/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Linker.h"

#include "gc/GC.h"

#include "gc/StoreBuffer-inl.h"

namespace js {
namespace jit {

JitCode*
Linker::newCode(JSContext* cx, CodeKind kind, bool hasPatchableBackedges /* = false */)
{
    MOZ_ASSERT_IF(hasPatchableBackedges, kind == CodeKind::Ion);

    JS::AutoAssertNoGC nogc(cx);
    if (masm.oom())
        return fail(cx);

    masm.performPendingReadBarriers();

    static const size_t ExecutableAllocatorAlignment = sizeof(void*);
    static_assert(CodeAlignment >= ExecutableAllocatorAlignment,
                  "Unexpected alignment requirements");

    // We require enough bytes for the code, header, and worst-case alignment padding.
    size_t bytesNeeded = masm.bytesNeeded() +
                         sizeof(JitCodeHeader) +
                         (CodeAlignment - ExecutableAllocatorAlignment);
    if (bytesNeeded >= MAX_BUFFER_SIZE)
        return fail(cx);

    // ExecutableAllocator requires bytesNeeded to be aligned.
    bytesNeeded = AlignBytes(bytesNeeded, ExecutableAllocatorAlignment);

    ExecutableAllocator& execAlloc = hasPatchableBackedges
                                     ? cx->runtime()->jitRuntime()->backedgeExecAlloc()
                                     : cx->runtime()->jitRuntime()->execAlloc();

    ExecutablePool* pool;
    uint8_t* result = (uint8_t*)execAlloc.alloc(cx, bytesNeeded, &pool, kind);
    if (!result)
        return fail(cx);

    // The JitCodeHeader will be stored right before the code buffer.
    uint8_t* codeStart = result + sizeof(JitCodeHeader);

    // Bump the code up to a nice alignment.
    codeStart = (uint8_t*)AlignBytes((uintptr_t)codeStart, CodeAlignment);
    MOZ_ASSERT(codeStart + masm.bytesNeeded() <= result + bytesNeeded);
    uint32_t headerSize = codeStart - result;
    JitCode* code = JitCode::New<NoGC>(cx, codeStart, bytesNeeded - headerSize,
                                       headerSize, pool, kind);
    if (!code)
        return fail(cx);
    if (masm.oom())
        return fail(cx);
    awjc.emplace(result, bytesNeeded);
    code->copyFrom(masm);
    masm.link(code);
    if (masm.embedsNurseryPointers())
        cx->zone()->group()->storeBuffer().putWholeCell(code);
    return code;
}

} // namespace jit
} // namespace js
