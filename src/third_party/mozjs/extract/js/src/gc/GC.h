/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS engine garbage collector API.
 */

#ifndef gc_GC_h
#define gc_GC_h

#include "gc/GCEnum.h"
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/RealmIterators.h"
#include "js/TraceKind.h"

class JSTracer;

namespace JS {
class RealmOptions;
}

namespace js {

class Nursery;

namespace gc {

class Arena;
class TenuredChunk;

} /* namespace gc */

// Define name, key and writability for the GC parameters.
#define FOR_EACH_GC_PARAM(_)                                                \
  _("maxBytes", JSGC_MAX_BYTES, true)                                       \
  _("minNurseryBytes", JSGC_MIN_NURSERY_BYTES, true)                        \
  _("maxNurseryBytes", JSGC_MAX_NURSERY_BYTES, true)                        \
  _("gcBytes", JSGC_BYTES, false)                                           \
  _("nurseryBytes", JSGC_NURSERY_BYTES, false)                              \
  _("gcNumber", JSGC_NUMBER, false)                                         \
  _("majorGCNumber", JSGC_MAJOR_GC_NUMBER, false)                           \
  _("minorGCNumber", JSGC_MINOR_GC_NUMBER, false)                           \
  _("incrementalGCEnabled", JSGC_INCREMENTAL_GC_ENABLED, true)              \
  _("perZoneGCEnabled", JSGC_PER_ZONE_GC_ENABLED, true)                     \
  _("unusedChunks", JSGC_UNUSED_CHUNKS, false)                              \
  _("totalChunks", JSGC_TOTAL_CHUNKS, false)                                \
  _("sliceTimeBudgetMS", JSGC_SLICE_TIME_BUDGET_MS, true)                   \
  _("highFrequencyTimeLimit", JSGC_HIGH_FREQUENCY_TIME_LIMIT, true)         \
  _("smallHeapSizeMax", JSGC_SMALL_HEAP_SIZE_MAX, true)                     \
  _("largeHeapSizeMin", JSGC_LARGE_HEAP_SIZE_MIN, true)                     \
  _("highFrequencySmallHeapGrowth", JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH,  \
    true)                                                                   \
  _("highFrequencyLargeHeapGrowth", JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH,  \
    true)                                                                   \
  _("lowFrequencyHeapGrowth", JSGC_LOW_FREQUENCY_HEAP_GROWTH, true)         \
  _("balancedHeapLimitsEnabled", JSGC_BALANCED_HEAP_LIMITS_ENABLED, true)   \
  _("heapGrowthFactor", JSGC_HEAP_GROWTH_FACTOR, true)                      \
  _("allocationThreshold", JSGC_ALLOCATION_THRESHOLD, true)                 \
  _("smallHeapIncrementalLimit", JSGC_SMALL_HEAP_INCREMENTAL_LIMIT, true)   \
  _("largeHeapIncrementalLimit", JSGC_LARGE_HEAP_INCREMENTAL_LIMIT, true)   \
  _("minEmptyChunkCount", JSGC_MIN_EMPTY_CHUNK_COUNT, true)                 \
  _("maxEmptyChunkCount", JSGC_MAX_EMPTY_CHUNK_COUNT, true)                 \
  _("compactingEnabled", JSGC_COMPACTING_ENABLED, true)                     \
  _("parallelMarkingEnabled", JSGC_PARALLEL_MARKING_ENABLED, true)          \
  _("parallelMarkingThresholdKB", JSGC_PARALLEL_MARKING_THRESHOLD_KB, true) \
  _("minLastDitchGCPeriod", JSGC_MIN_LAST_DITCH_GC_PERIOD, true)            \
  _("nurseryFreeThresholdForIdleCollection",                                \
    JSGC_NURSERY_FREE_THRESHOLD_FOR_IDLE_COLLECTION, true)                  \
  _("nurseryFreeThresholdForIdleCollectionPercent",                         \
    JSGC_NURSERY_FREE_THRESHOLD_FOR_IDLE_COLLECTION_PERCENT, true)          \
  _("nurseryTimeoutForIdleCollectionMS",                                    \
    JSGC_NURSERY_TIMEOUT_FOR_IDLE_COLLECTION_MS, true)                      \
  _("pretenureThreshold", JSGC_PRETENURE_THRESHOLD, true)                   \
  _("zoneAllocDelayKB", JSGC_ZONE_ALLOC_DELAY_KB, true)                     \
  _("mallocThresholdBase", JSGC_MALLOC_THRESHOLD_BASE, true)                \
  _("urgentThreshold", JSGC_URGENT_THRESHOLD_MB, true)                      \
  _("chunkBytes", JSGC_CHUNK_BYTES, false)                                  \
  _("helperThreadRatio", JSGC_HELPER_THREAD_RATIO, true)                    \
  _("maxHelperThreads", JSGC_MAX_HELPER_THREADS, true)                      \
  _("helperThreadCount", JSGC_HELPER_THREAD_COUNT, false)                   \
  _("markingThreadCount", JSGC_MARKING_THREAD_COUNT, true)                  \
  _("systemPageSizeKB", JSGC_SYSTEM_PAGE_SIZE_KB, false)

// Get the key and writability give a GC parameter name.
extern bool GetGCParameterInfo(const char* name, JSGCParamKey* keyOut,
                               bool* writableOut);

extern void TraceRuntime(JSTracer* trc);

// Trace roots but don't evict the nursery first; used from DumpHeap.
extern void TraceRuntimeWithoutEviction(JSTracer* trc);

extern void ReleaseAllJITCode(JS::GCContext* gcx);

extern void PrepareForDebugGC(JSRuntime* rt);

/* Functions for managing cross compartment gray pointers. */

extern void NotifyGCNukeWrapper(JSContext* cx, JSObject* wrapper);

extern unsigned NotifyGCPreSwap(JSObject* a, JSObject* b);

extern void NotifyGCPostSwap(JSObject* a, JSObject* b, unsigned preResult);

using IterateChunkCallback = void (*)(JSRuntime*, void*, gc::TenuredChunk*,
                                      const JS::AutoRequireNoGC&);
using IterateZoneCallback = void (*)(JSRuntime*, void*, JS::Zone*,
                                     const JS::AutoRequireNoGC&);
using IterateArenaCallback = void (*)(JSRuntime*, void*, gc::Arena*,
                                      JS::TraceKind, size_t,
                                      const JS::AutoRequireNoGC&);
using IterateCellCallback = void (*)(JSRuntime*, void*, JS::GCCellPtr, size_t,
                                     const JS::AutoRequireNoGC&);

/*
 * This function calls |zoneCallback| on every zone, |realmCallback| on
 * every realm, |arenaCallback| on every in-use arena, and |cellCallback|
 * on every in-use cell in the GC heap.
 *
 * Note that no read barrier is triggered on the cells passed to cellCallback,
 * so no these pointers must not escape the callback.
 */
extern void IterateHeapUnbarriered(JSContext* cx, void* data,
                                   IterateZoneCallback zoneCallback,
                                   JS::IterateRealmCallback realmCallback,
                                   IterateArenaCallback arenaCallback,
                                   IterateCellCallback cellCallback);

/*
 * This function is like IterateHeapUnbarriered, but does it for a single zone.
 */
extern void IterateHeapUnbarrieredForZone(
    JSContext* cx, JS::Zone* zone, void* data, IterateZoneCallback zoneCallback,
    JS::IterateRealmCallback realmCallback, IterateArenaCallback arenaCallback,
    IterateCellCallback cellCallback);

/*
 * Invoke chunkCallback on every in-use chunk.
 */
extern void IterateChunks(JSContext* cx, void* data,
                          IterateChunkCallback chunkCallback);

using IterateScriptCallback = void (*)(JSRuntime*, void*, BaseScript*,
                                       const JS::AutoRequireNoGC&);

/*
 * Invoke scriptCallback on every in-use script for the given realm or for all
 * realms if it is null. The scripts may or may not have bytecode.
 */
extern void IterateScripts(JSContext* cx, JS::Realm* realm, void* data,
                           IterateScriptCallback scriptCallback);

JS::Realm* NewRealm(JSContext* cx, JSPrincipals* principals,
                    const JS::RealmOptions& options);

namespace gc {

void FinishGC(JSContext* cx, JS::GCReason = JS::GCReason::FINISH_GC);

void WaitForBackgroundTasks(JSContext* cx);

enum VerifierType { PreBarrierVerifier };

#ifdef JS_GC_ZEAL

extern const char ZealModeHelpText[];

/* Check that write barriers have been used correctly. See gc/Verifier.cpp. */
void VerifyBarriers(JSRuntime* rt, VerifierType type);

void MaybeVerifyBarriers(JSContext* cx, bool always = false);

void DumpArenaInfo();

#else

static inline void VerifyBarriers(JSRuntime* rt, VerifierType type) {}

static inline void MaybeVerifyBarriers(JSContext* cx, bool always = false) {}

#endif

/*
 * Instances of this class prevent GC from happening while they are live. If an
 * allocation causes a heap threshold to be exceeded, no GC will be performed
 * and the allocation will succeed. Allocation may still fail for other reasons.
 *
 * Use of this class is highly discouraged, since without GC system memory can
 * become exhausted and this can cause crashes at places where we can't handle
 * allocation failure.
 *
 * Use of this is permissible in situations where it would be impossible (or at
 * least very difficult) to tolerate GC and where only a fixed number of objects
 * are allocated, such as:
 *
 *  - error reporting
 *  - JIT bailout handling
 *  - brain transplants (JSObject::swap)
 *  - debugging utilities not exposed to the browser
 *
 * This works by updating the |JSContext::suppressGC| counter which is checked
 * at the start of GC.
 */
class MOZ_RAII JS_HAZ_GC_SUPPRESSED AutoSuppressGC
    : public JS::AutoRequireNoGC {
  int32_t& suppressGC_;

 public:
  explicit AutoSuppressGC(JSContext* cx);

  ~AutoSuppressGC() { suppressGC_--; }
};

const char* StateName(State state);

} /* namespace gc */

/* Use this to avoid assertions when manipulating the wrapper map. */
class MOZ_RAII AutoDisableProxyCheck {
 public:
#ifdef DEBUG
  AutoDisableProxyCheck();
  ~AutoDisableProxyCheck();
#else
  AutoDisableProxyCheck() {}
#endif
};

struct MOZ_RAII AutoDisableCompactingGC {
  explicit AutoDisableCompactingGC(JSContext* cx);
  ~AutoDisableCompactingGC();

 private:
  JSContext* cx;
};

} /* namespace js */

#endif /* gc_GC_h */
