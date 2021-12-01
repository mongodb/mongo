/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Various JS utility functions. */

#include "jsutil.h"

#include "mozilla/Assertions.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"
#include "mozilla/ThreadLocal.h"

#include <stdio.h>

#include "jstypes.h"

#include "js/Utility.h"
#include "util/Windows.h"
#include "vm/HelperThreads.h"

using namespace js;

using mozilla::CeilingLog2Size;
using mozilla::PodArrayZero;

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
/* For OOM testing functionality in Utility.h. */
namespace js {

mozilla::Atomic<AutoEnterOOMUnsafeRegion*> AutoEnterOOMUnsafeRegion::owner_;

namespace oom {

JS_PUBLIC_DATA(uint32_t) targetThread = 0;
MOZ_THREAD_LOCAL(uint32_t) threadType;
JS_PUBLIC_DATA(uint64_t) maxAllocations = UINT64_MAX;
JS_PUBLIC_DATA(uint64_t) counter = 0;
JS_PUBLIC_DATA(bool) failAlways = true;

JS_PUBLIC_DATA(uint32_t) stackTargetThread = 0;
JS_PUBLIC_DATA(uint64_t) maxStackChecks = UINT64_MAX;
JS_PUBLIC_DATA(uint64_t) stackCheckCounter = 0;
JS_PUBLIC_DATA(bool) stackCheckFailAlways = true;

JS_PUBLIC_DATA(uint32_t) interruptTargetThread = 0;
JS_PUBLIC_DATA(uint64_t) maxInterruptChecks = UINT64_MAX;
JS_PUBLIC_DATA(uint64_t) interruptCheckCounter = 0;
JS_PUBLIC_DATA(bool) interruptCheckFailAlways = true;

bool
InitThreadType(void) {
    return threadType.init();
}

void
SetThreadType(ThreadType type) {
    threadType.set(type);
}

uint32_t
GetThreadType(void) {
    return threadType.get();
}

static inline bool
IsHelperThreadType(uint32_t thread)
{
    return thread != THREAD_TYPE_NONE && thread != THREAD_TYPE_COOPERATING;
}

void
SimulateOOMAfter(uint64_t allocations, uint32_t thread, bool always)
{
    Maybe<AutoLockHelperThreadState> lock;
    if (IsHelperThreadType(targetThread) || IsHelperThreadType(thread)) {
        lock.emplace();
        HelperThreadState().waitForAllThreadsLocked(lock.ref());
    }

    MOZ_ASSERT(counter + allocations > counter);
    MOZ_ASSERT(thread > js::THREAD_TYPE_NONE && thread < js::THREAD_TYPE_MAX);
    targetThread = thread;
    maxAllocations = counter + allocations;
    failAlways = always;
}

void
ResetSimulatedOOM()
{
    Maybe<AutoLockHelperThreadState> lock;
    if (IsHelperThreadType(targetThread)) {
        lock.emplace();
        HelperThreadState().waitForAllThreadsLocked(lock.ref());
    }

    targetThread = THREAD_TYPE_NONE;
    maxAllocations = UINT64_MAX;
    failAlways = false;
}

void
SimulateStackOOMAfter(uint64_t checks, uint32_t thread, bool always)
{
    Maybe<AutoLockHelperThreadState> lock;
    if (IsHelperThreadType(stackTargetThread) || IsHelperThreadType(thread)) {
        lock.emplace();
        HelperThreadState().waitForAllThreadsLocked(lock.ref());
    }

    MOZ_ASSERT(stackCheckCounter + checks > stackCheckCounter);
    MOZ_ASSERT(thread > js::THREAD_TYPE_NONE && thread < js::THREAD_TYPE_MAX);
    stackTargetThread = thread;
    maxStackChecks = stackCheckCounter + checks;
    stackCheckFailAlways = always;
}

void
ResetSimulatedStackOOM()
{
    Maybe<AutoLockHelperThreadState> lock;
    if (IsHelperThreadType(stackTargetThread)) {
        lock.emplace();
        HelperThreadState().waitForAllThreadsLocked(lock.ref());
    }

    stackTargetThread = THREAD_TYPE_NONE;
    maxStackChecks = UINT64_MAX;
    stackCheckFailAlways = false;
}

void
SimulateInterruptAfter(uint64_t checks, uint32_t thread, bool always)
{
    Maybe<AutoLockHelperThreadState> lock;
    if (IsHelperThreadType(interruptTargetThread) || IsHelperThreadType(thread)) {
        lock.emplace();
        HelperThreadState().waitForAllThreadsLocked(lock.ref());
    }

    MOZ_ASSERT(interruptCheckCounter + checks > interruptCheckCounter);
    MOZ_ASSERT(thread > js::THREAD_TYPE_NONE && thread < js::THREAD_TYPE_MAX);
    interruptTargetThread = thread;
    maxInterruptChecks = interruptCheckCounter + checks;
    interruptCheckFailAlways = always;
}

void
ResetSimulatedInterrupt()
{
    Maybe<AutoLockHelperThreadState> lock;
    if (IsHelperThreadType(interruptTargetThread)) {
        lock.emplace();
        HelperThreadState().waitForAllThreadsLocked(lock.ref());
    }

    interruptTargetThread = THREAD_TYPE_NONE;
    maxInterruptChecks = UINT64_MAX;
    interruptCheckFailAlways = false;
}

} // namespace oom
} // namespace js
#endif // defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

JS_PUBLIC_DATA(arena_id_t) js::MallocArena;

void
js::InitMallocAllocator()
{
    MallocArena = moz_create_arena();
}

void
js::ShutDownMallocAllocator()
{
    // Until Bug 1364359 is fixed it is unsafe to call moz_dispose_arena.
    // moz_dispose_arena(MallocArena);
}

JS_PUBLIC_API(void)
JS_Assert(const char* s, const char* file, int ln)
{
    MOZ_ReportAssertionFailure(s, file, ln);
    MOZ_CRASH();
}

#ifdef __linux__

#include <malloc.h>
#include <stdlib.h>

namespace js {

// This function calls all the vanilla heap allocation functions.  It is never
// called, and exists purely to help config/check_vanilla_allocations.py.  See
// that script for more details.
extern MOZ_COLD void
AllTheNonBasicVanillaNewAllocations()
{
    // posix_memalign and aligned_alloc aren't available on all Linux
    // configurations.
    // valloc was deprecated in Android 5.0
    //char* q;
    //posix_memalign((void**)&q, 16, 16);

    intptr_t p =
        intptr_t(malloc(16)) +
        intptr_t(calloc(1, 16)) +
        intptr_t(realloc(nullptr, 16)) +
        intptr_t(new char) +
        intptr_t(new char) +
        intptr_t(new char) +
        intptr_t(new char[16]) +
        intptr_t(memalign(16, 16)) +
        //intptr_t(q) +
        //intptr_t(aligned_alloc(16, 16)) +
        //intptr_t(valloc(4096)) +
        intptr_t(strdup("dummy"));

    printf("%u\n", uint32_t(p));  // make sure |p| is not optimized away

    free((int*)p);      // this would crash if ever actually called

    MOZ_CRASH();
}

} // namespace js

#endif // __linux__

#ifdef JS_BASIC_STATS

#include <math.h>

/*
 * Histogram bins count occurrences of values <= the bin label, as follows:
 *
 *   linear:  0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10 or more
 *     2**x:  0,   1,   2,   4,   8,  16,  32,  64, 128, 256, 512 or more
 *    10**x:  0,   1,  10, 100, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9 or more
 *
 * We wish to count occurrences of 0 and 1 values separately, always.
 */
static uint32_t
BinToVal(unsigned logscale, unsigned bin)
{
    MOZ_ASSERT(bin <= 10);
    if (bin <= 1 || logscale == 0)
        return bin;
    --bin;
    if (logscale == 2)
        return JS_BIT(bin);
    MOZ_ASSERT(logscale == 10);
    return uint32_t(pow(10.0, (double) bin));
}

static unsigned
ValToBin(unsigned logscale, uint32_t val)
{
    unsigned bin;

    if (val <= 1)
        return val;
    bin = (logscale == 10)
        ? (unsigned) ceil(log10((double) val))
        : (logscale == 2)
        ? (unsigned) CeilingLog2Size(val)
        : val;
    return Min(bin, 10U);
}

void
JS_BasicStatsAccum(JSBasicStats* bs, uint32_t val)
{
    unsigned oldscale, newscale, bin;
    double mean;

    ++bs->num;
    if (bs->max < val)
        bs->max = val;
    bs->sum += val;
    bs->sqsum += (double)val * val;

    oldscale = bs->logscale;
    if (oldscale != 10) {
        mean = bs->sum / bs->num;
        if (bs->max > 16 && mean > 8) {
            newscale = (bs->max > 1e6 && mean > 1000) ? 10 : 2;
            if (newscale != oldscale) {
                uint32_t newhist[11], newbin;

                PodArrayZero(newhist);
                for (bin = 0; bin <= 10; bin++) {
                    newbin = ValToBin(newscale, BinToVal(oldscale, bin));
                    newhist[newbin] += bs->hist[bin];
                }
                js_memcpy(bs->hist, newhist, sizeof bs->hist);
                bs->logscale = newscale;
            }
        }
    }

    bin = ValToBin(bs->logscale, val);
    ++bs->hist[bin];
}

double
JS_MeanAndStdDev(uint32_t num, double sum, double sqsum, double* sigma)
{
    double var;

    if (num == 0 || sum == 0) {
        *sigma = 0;
        return 0;
    }

    var = num * sqsum - sum * sum;
    if (var < 0 || num == 1)
        var = 0;
    else
        var /= (double)num * (num - 1);

    /* Windows says sqrt(0.0) is "-1.#J" (?!) so we must test. */
    *sigma = (var != 0) ? sqrt(var) : 0;
    return sum / num;
}

void
JS_DumpBasicStats(JSBasicStats* bs, const char* title, FILE* fp)
{
    double mean, sigma;

    mean = JS_MeanAndStdDevBS(bs, &sigma);
    fprintf(fp, "\nmean %s %g, std. deviation %g, max %lu\n",
            title, mean, sigma, (unsigned long) bs->max);
    JS_DumpHistogram(bs, fp);
}

void
JS_DumpHistogram(JSBasicStats* bs, FILE* fp)
{
    unsigned bin;
    uint32_t cnt, max;
    double sum, mean;

    for (bin = 0, max = 0, sum = 0; bin <= 10; bin++) {
        cnt = bs->hist[bin];
        if (max < cnt)
            max = cnt;
        sum += cnt;
    }
    mean = sum / cnt;
    for (bin = 0; bin <= 10; bin++) {
        unsigned val = BinToVal(bs->logscale, bin);
        unsigned end = (bin == 10) ? 0 : BinToVal(bs->logscale, bin + 1);
        cnt = bs->hist[bin];
        if (val + 1 == end)
            fprintf(fp, "        [%6u]", val);
        else if (end != 0)
            fprintf(fp, "[%6u, %6u]", val, end - 1);
        else
            fprintf(fp, "[%6u,   +inf]", val);
        fprintf(fp, ": %8u ", cnt);
        if (cnt != 0) {
            if (max > 1e6 && mean > 1e3)
                cnt = uint32_t(ceil(log10((double) cnt)));
            else if (max > 16 && mean > 8)
                cnt = CeilingLog2Size(cnt);
            for (unsigned i = 0; i < cnt; i++)
                putc('*', fp);
        }
        putc('\n', fp);
    }
}

#endif /* JS_BASIC_STATS */
