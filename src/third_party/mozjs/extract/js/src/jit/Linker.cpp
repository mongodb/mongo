/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Linker.h"

#include "jit/JitZone.h"
#include "util/Memory.h"

#include "gc/StoreBuffer-inl.h"

namespace js {
namespace jit {

JitCode* Linker::newCode(JSContext* cx, CodeKind kind) {
  JS::AutoAssertNoGC nogc(cx);
  if (masm.oom()) {
    return fail(cx);
  }

  static const size_t ExecutableAllocatorAlignment = sizeof(void*);
  static_assert(CodeAlignment >= ExecutableAllocatorAlignment,
                "Unexpected alignment requirements");

  // We require enough bytes for the code, header, and worst-case alignment
  // padding.
  size_t bytesNeeded = masm.bytesNeeded() + sizeof(JitCodeHeader) +
                       (CodeAlignment - ExecutableAllocatorAlignment);
  if (bytesNeeded >= MAX_BUFFER_SIZE) {
    return fail(cx);
  }

  // ExecutableAllocator requires bytesNeeded to be aligned.
  bytesNeeded = AlignBytes(bytesNeeded, ExecutableAllocatorAlignment);

  JitZone* jitZone = cx->zone()->getJitZone(cx);
  if (!jitZone) {
    // Note: don't call fail(cx) here, getJitZone reports OOM.
    return nullptr;
  }

  ExecutablePool* pool;
  uint8_t* result =
      (uint8_t*)jitZone->execAlloc().alloc(cx, bytesNeeded, &pool, kind);
  if (!result) {
    return fail(cx);
  }

  // The JitCodeHeader will be stored right before the code buffer.
  uint8_t* codeStart = result + sizeof(JitCodeHeader);

  // Bump the code up to a nice alignment.
  codeStart = (uint8_t*)AlignBytes((uintptr_t)codeStart, CodeAlignment);
  MOZ_ASSERT(codeStart + masm.bytesNeeded() <= result + bytesNeeded);
  uint32_t headerSize = codeStart - result;
  JitCode* code =
      JitCode::New<NoGC>(cx, codeStart, bytesNeeded, headerSize, pool, kind);
  if (!code) {
    return fail(cx);
  }
  if (masm.oom()) {
    return fail(cx);
  }
  awjcf.emplace(result, bytesNeeded);
  if (!awjcf->makeWritable()) {
    return fail(cx);
  }
  code->copyFrom(masm);
  masm.link(code);
  if (masm.embedsNurseryPointers()) {
    cx->runtime()->gc.storeBuffer().putWholeCell(code);
  }
  return code;
}

}  // namespace jit
}  // namespace js
