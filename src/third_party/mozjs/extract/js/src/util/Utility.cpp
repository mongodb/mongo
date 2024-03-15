/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Various JS utility functions. */

#include "js/Utility.h"

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Maybe.h"
#include "mozilla/ThreadLocal.h"

#include <stdio.h>

#include "jstypes.h"

#include "util/Poison.h"
#include "vm/HelperThreads.h"

using namespace js;

using mozilla::Maybe;

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
/* For OOM testing functionality in Utility.h. */
namespace js {

mozilla::Atomic<AutoEnterOOMUnsafeRegion*> AutoEnterOOMUnsafeRegion::owner_;

namespace oom {

JS_PUBLIC_DATA FailureSimulator simulator;
static MOZ_THREAD_LOCAL(uint32_t) threadType;

bool InitThreadType() { return threadType.init(); }

void SetThreadType(ThreadType type) { threadType.set(type); }

uint32_t GetThreadType(void) { return threadType.get(); }

static inline bool IsHelperThreadType(uint32_t thread) {
  return thread != THREAD_TYPE_NONE && thread != THREAD_TYPE_MAIN;
}

void FailureSimulator::simulateFailureAfter(Kind kind, uint64_t checks,
                                            uint32_t thread, bool always) {
  Maybe<AutoLockHelperThreadState> lock;
  if (IsHelperThreadType(targetThread_) || IsHelperThreadType(thread)) {
    lock.emplace();
    WaitForAllHelperThreads(lock.ref());
  }

  MOZ_ASSERT(counter_ + checks > counter_);
  MOZ_ASSERT(thread > js::THREAD_TYPE_NONE && thread < js::THREAD_TYPE_MAX);
  targetThread_ = thread;
  maxChecks_ = counter_ + checks;
  failAlways_ = always;
  kind_ = kind;
}

void FailureSimulator::reset() {
  Maybe<AutoLockHelperThreadState> lock;
  if (IsHelperThreadType(targetThread_)) {
    lock.emplace();
    WaitForAllHelperThreads(lock.ref());
  }

  targetThread_ = THREAD_TYPE_NONE;
  maxChecks_ = UINT64_MAX;
  failAlways_ = false;
  kind_ = Kind::Nothing;
}

}  // namespace oom
}  // namespace js
#endif  // defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

#if defined(FUZZING)
namespace js {
namespace oom {
JS_PUBLIC_DATA size_t largeAllocLimit = 0;
void InitLargeAllocLimit() {
  char* limitStr = getenv("MOZ_FUZZ_LARGE_ALLOC_LIMIT");
  if (limitStr) {
    largeAllocLimit = atoll(limitStr);
  }
}
}  // namespace oom
}  // namespace js
#endif

#if defined(JS_GC_ALLOW_EXTRA_POISONING)
#  if defined(DEBUG)
bool js::gExtraPoisoningEnabled = true;
#  else
bool js::gExtraPoisoningEnabled = false;
#  endif
#endif

JS_PUBLIC_DATA arena_id_t js::MallocArena;
JS_PUBLIC_DATA arena_id_t js::ArrayBufferContentsArena;
JS_PUBLIC_DATA arena_id_t js::StringBufferArena;

void js::InitMallocAllocator() {
  arena_params_t mallocArenaParams;
  mallocArenaParams.mMaxDirtyIncreaseOverride = 5;
  MallocArena = moz_create_arena_with_params(&mallocArenaParams);

  arena_params_t params;
  params.mMaxDirtyIncreaseOverride = 5;
  params.mFlags |= ARENA_FLAG_RANDOMIZE_SMALL_ENABLED;
  ArrayBufferContentsArena = moz_create_arena_with_params(&params);
  StringBufferArena = moz_create_arena_with_params(&params);
}

void js::ShutDownMallocAllocator() {
  // Until Bug 1364359 is fixed it is unsafe to call moz_dispose_arena.
  // moz_dispose_arena(MallocArena);
  // moz_dispose_arena(ArrayBufferContentsArena);
}

extern void js::AssertJSStringBufferInCorrectArena(const void* ptr) {
//  `jemalloc_ptr_info()` only exists if MOZ_MEMORY is defined, and it only
//  returns an arenaId if MOZ_DEBUG is defined. Otherwise, this function is
//  a no-op.
#if defined(MOZ_MEMORY) && defined(MOZ_DEBUG)
  if (ptr) {
    jemalloc_ptr_info_t ptrInfo{};
    jemalloc_ptr_info(ptr, &ptrInfo);
    MOZ_ASSERT(ptrInfo.tag != TagUnknown);
    MOZ_ASSERT(ptrInfo.arenaId == js::StringBufferArena);
  }
#endif
}

JS_PUBLIC_API void JS_Assert(const char* s, const char* file, int ln) {
  MOZ_ReportAssertionFailure(s, file, ln);
  MOZ_CRASH();
}

#ifdef __linux__

#  include <malloc.h>
#  include <stdlib.h>

namespace js {

// This function calls all the vanilla heap allocation functions.  It is never
// called, and exists purely to help config/check_vanilla_allocations.py.  See
// that script for more details.
extern MOZ_COLD void AllTheNonBasicVanillaNewAllocations() {
  // posix_memalign and aligned_alloc aren't available on all Linux
  // configurations.
  // valloc was deprecated in Android 5.0
  // char* q;
  // posix_memalign((void**)&q, 16, 16);

  intptr_t p = intptr_t(malloc(16)) + intptr_t(calloc(1, 16)) +
               intptr_t(realloc(nullptr, 16)) + intptr_t(new char) +
               intptr_t(new char) + intptr_t(new char) +
               intptr_t(new char[16]) + intptr_t(memalign(16, 16)) +
               // intptr_t(q) +
               // intptr_t(aligned_alloc(16, 16)) +
               // intptr_t(valloc(4096)) +
               intptr_t(strdup("dummy"));

  printf("%u\n", uint32_t(p));  // make sure |p| is not optimized away

  free((int*)p);  // this would crash if ever actually called

  MOZ_CRASH();
}

}  // namespace js

#endif  // __linux__
