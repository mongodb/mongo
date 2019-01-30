/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Defines, Enums, Functions, and Types extract from include/js/Utility.h
 * to support MongoDB's jscustomallocator.h
 */

#pragma once

#include <cstdlib>

namespace js {

enum ThreadType {
    THREAD_TYPE_NONE = 0,       // 0
    THREAD_TYPE_COOPERATING,    // 1
    THREAD_TYPE_WASM,           // 2
    THREAD_TYPE_ION,            // 3
    THREAD_TYPE_PARSE,          // 4
    THREAD_TYPE_COMPRESS,       // 5
    THREAD_TYPE_GCHELPER,       // 6
    THREAD_TYPE_GCPARALLEL,     // 7
    THREAD_TYPE_PROMISE_TASK,   // 8
    THREAD_TYPE_ION_FREE,       // 9
    THREAD_TYPE_WASM_TIER2,     // 10
    THREAD_TYPE_WORKER,         // 11
    THREAD_TYPE_MAX             // Used to check shell function arguments
};

namespace oom {

# if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
extern bool InitThreadType(void);
extern void SetThreadType(ThreadType);
extern uint32_t GetThreadType(void);
# else
inline bool InitThreadType(void) { return true; }
inline void SetThreadType(ThreadType t) {};
inline uint32_t GetThreadType(void) { return 0; }
# endif
} /* namespace oom */
} /* namespace js */

# if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

/*
 * In order to test OOM conditions, when the testing function
 * oomAfterAllocations COUNT is passed, we fail continuously after the NUM'th
 * allocation from now.
 */
extern JS_PUBLIC_DATA(uint32_t) OOM_maxAllocations; /* set in builtin/TestingFunctions.cpp */
extern JS_PUBLIC_DATA(uint32_t) OOM_counter; /* data race, who cares. */
extern JS_PUBLIC_DATA(bool) OOM_failAlways;

#ifdef JS_OOM_BREAKPOINT
static MOZ_NEVER_INLINE void js_failedAllocBreakpoint() { asm(""); }
#define JS_OOM_CALL_BP_FUNC() js_failedAllocBreakpoint()
#else
#define JS_OOM_CALL_BP_FUNC() do {} while(0)
#endif

namespace js {
namespace oom {

extern JS_PUBLIC_DATA(uint32_t) targetThread;

static inline bool
IsThreadSimulatingOOM()
{
    return false;
}

static inline bool
IsSimulatedOOMAllocation()
{
    return false;
}

static inline bool
ShouldFailWithOOM()
{
    return false;
}

# else

namespace js {
namespace oom {
static inline bool IsSimulatedOOMAllocation() { return false; }
static inline bool ShouldFailWithOOM() { return false; }

# endif /* DEBUG || JS_OOM_BREAKPOINT */

} /* namespace oom */
} /* namespace js */

#  define JS_OOM_POSSIBLY_FAIL() do {} while(0)
#  define JS_OOM_POSSIBLY_FAIL_BOOL() do {} while(0)

namespace js {

/* Disable OOM testing in sections which are not OOM safe. */
struct MOZ_RAII AutoEnterOOMUnsafeRegion
{
    MOZ_NORETURN MOZ_COLD void crash(const char* reason);
    MOZ_NORETURN MOZ_COLD void crash(size_t size, const char* reason);

    using AnnotateOOMAllocationSizeCallback = void(*)(size_t);
    static AnnotateOOMAllocationSizeCallback annotateOOMSizeCallback;

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    AutoEnterOOMUnsafeRegion()
      : oomEnabled_(oom::IsThreadSimulatingOOM() && OOM_maxAllocations != UINT32_MAX),
        oomAfter_(0)
    {
        if (oomEnabled_) {
            oomAfter_ = int64_t(OOM_maxAllocations) - OOM_counter;
            OOM_maxAllocations = UINT32_MAX;
        }
    }

    ~AutoEnterOOMUnsafeRegion() {
        if (oomEnabled_) {
            MOZ_ASSERT(OOM_maxAllocations == UINT32_MAX);
            int64_t maxAllocations = OOM_counter + oomAfter_;
            MOZ_ASSERT(maxAllocations >= 0 && maxAllocations < UINT32_MAX,
                       "alloc count + oom limit exceeds range, your oom limit is probably too large");
            OOM_maxAllocations = uint32_t(maxAllocations);
        }
    }

  private:
    bool oomEnabled_;
    int64_t oomAfter_;
#endif

};
} /* namespace js */
