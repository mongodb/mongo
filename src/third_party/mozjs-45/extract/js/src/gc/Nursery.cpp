/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Nursery-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Move.h"

#include "jscompartment.h"
#include "jsfriendapi.h"
#include "jsgc.h"
#include "jsutil.h"

#include "gc/GCInternals.h"
#include "gc/Memory.h"
#include "jit/JitFrames.h"
#include "vm/ArrayObject.h"
#include "vm/Debugger.h"
#if defined(DEBUG)
#include "vm/ScopeObject.h"
#endif
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"
#include "vm/TypeInference.h"

#include "jsobjinlines.h"

#include "vm/NativeObject-inl.h"

using namespace js;
using namespace gc;

using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::PodCopy;
using mozilla::PodZero;

struct js::Nursery::FreeMallocedBuffersTask : public GCParallelTask
{
    explicit FreeMallocedBuffersTask(FreeOp* fop) : fop_(fop) {}
    bool init() { return buffers_.init(); }
    void transferBuffersToFree(MallocedBuffersSet& buffersToFree);
    ~FreeMallocedBuffersTask() override { join(); }

  private:
    FreeOp* fop_;
    MallocedBuffersSet buffers_;

    virtual void run() override;
};

bool
js::Nursery::init(uint32_t maxNurseryBytes)
{
    /* maxNurseryBytes parameter is rounded down to a multiple of chunk size. */
    numNurseryChunks_ = maxNurseryBytes >> ChunkShift;

    /* If no chunks are specified then the nursery is permenantly disabled. */
    if (numNurseryChunks_ == 0)
        return true;

    if (!mallocedBuffers.init())
        return false;

    if (!cellsWithUid_.init())
        return false;

    void* heap = MapAlignedPages(nurserySize(), Alignment);
    if (!heap)
        return false;

    freeMallocedBuffersTask = js_new<FreeMallocedBuffersTask>(runtime()->defaultFreeOp());
    if (!freeMallocedBuffersTask || !freeMallocedBuffersTask->init())
        return false;

    heapStart_ = uintptr_t(heap);
    heapEnd_ = heapStart_ + nurserySize();
    currentStart_ = start();
    numActiveChunks_ = 1;
    JS_POISON(heap, JS_FRESH_NURSERY_PATTERN, nurserySize());
    setCurrentChunk(0);
    updateDecommittedRegion();

    char* env = getenv("JS_GC_PROFILE_NURSERY");
    if (env) {
        if (0 == strcmp(env, "help")) {
            fprintf(stderr, "JS_GC_PROFILE_NURSERY=N\n\n"
                    "\tReport minor GC's taking more than N microseconds.");
            exit(0);
        }
        enableProfiling_ = true;
        profileThreshold_ = atoi(env);
    }

    MOZ_ASSERT(isEnabled());
    return true;
}

js::Nursery::~Nursery()
{
    if (start())
        UnmapPages((void*)start(), nurserySize());

    js_delete(freeMallocedBuffersTask);
}

void
js::Nursery::updateDecommittedRegion()
{
#ifndef JS_GC_ZEAL
    if (numActiveChunks_ < numNurseryChunks_) {
        // Bug 994054: madvise on MacOS is too slow to make this
        //             optimization worthwhile.
# ifndef XP_DARWIN
        uintptr_t decommitStart = chunk(numActiveChunks_).start();
        uintptr_t decommitSize = heapEnd() - decommitStart;
        MOZ_ASSERT(decommitStart == AlignBytes(decommitStart, Alignment));
        MOZ_ASSERT(decommitSize == AlignBytes(decommitStart, Alignment));
        MarkPagesUnused((void*)decommitStart, decommitSize);
# endif
    }
#endif
}

void
js::Nursery::enable()
{
    MOZ_ASSERT(isEmpty());
    MOZ_ASSERT(!runtime()->gc.isVerifyPreBarriersEnabled());
    if (isEnabled())
        return;
    numActiveChunks_ = 1;
    setCurrentChunk(0);
    currentStart_ = position();
#ifdef JS_GC_ZEAL
    if (runtime()->gcZeal() == ZealGenerationalGCValue)
        enterZealMode();
#endif
}

void
js::Nursery::disable()
{
    MOZ_ASSERT(isEmpty());
    if (!isEnabled())
        return;
    numActiveChunks_ = 0;
    currentEnd_ = 0;
    updateDecommittedRegion();
}

bool
js::Nursery::isEmpty() const
{
    MOZ_ASSERT(runtime_);
    if (!isEnabled())
        return true;
    MOZ_ASSERT_IF(runtime_->gcZeal() != ZealGenerationalGCValue, currentStart_ == start());
    return position() == currentStart_;
}

#ifdef JS_GC_ZEAL
void
js::Nursery::enterZealMode() {
    if (isEnabled())
        numActiveChunks_ = numNurseryChunks_;
}

void
js::Nursery::leaveZealMode() {
    if (isEnabled()) {
        MOZ_ASSERT(isEmpty());
        setCurrentChunk(0);
        currentStart_ = start();
    }
}
#endif // JS_GC_ZEAL

JSObject*
js::Nursery::allocateObject(JSContext* cx, size_t size, size_t numDynamic, const js::Class* clasp)
{
    /* Ensure there's enough space to replace the contents with a RelocationOverlay. */
    MOZ_ASSERT(size >= sizeof(RelocationOverlay));

    /*
     * Classes with JSCLASS_SKIP_NURSERY_FINALIZE will not have their finalizer
     * called if they are nursery allocated and not promoted to the tenured
     * heap. The finalizers for these classes must do nothing except free data
     * which was allocated via Nursery::allocateBuffer.
     */
    MOZ_ASSERT_IF(clasp->finalize, clasp->flags & JSCLASS_SKIP_NURSERY_FINALIZE);

    /* Make the object allocation. */
    JSObject* obj = static_cast<JSObject*>(allocate(size));
    if (!obj)
        return nullptr;

    /* If we want external slots, add them. */
    HeapSlot* slots = nullptr;
    if (numDynamic) {
        MOZ_ASSERT(clasp->isNative());
        slots = static_cast<HeapSlot*>(allocateBuffer(cx->zone(), numDynamic * sizeof(HeapSlot)));
        if (!slots) {
            /*
             * It is safe to leave the allocated object uninitialized, since we
             * do not visit unallocated things in the nursery.
             */
            return nullptr;
        }
    }

    /* Always initialize the slots field to match the JIT behavior. */
    obj->setInitialSlotsMaybeNonNative(slots);

    TraceNurseryAlloc(obj, size);
    return obj;
}

void*
js::Nursery::allocate(size_t size)
{
    MOZ_ASSERT(isEnabled());
    MOZ_ASSERT(!runtime()->isHeapBusy());
    MOZ_ASSERT(position() >= currentStart_);

    if (currentEnd() < position() + size) {
        if (currentChunk_ + 1 == numActiveChunks_)
            return nullptr;
        setCurrentChunk(currentChunk_ + 1);
    }

    void* thing = (void*)position();
    position_ = position() + size;

    JS_EXTRA_POISON(thing, JS_ALLOCATED_NURSERY_PATTERN, size);
    MemProfiler::SampleNursery(reinterpret_cast<void*>(thing), size);
    return thing;
}

void*
js::Nursery::allocateBuffer(Zone* zone, uint32_t nbytes)
{
    MOZ_ASSERT(nbytes > 0);

    if (nbytes <= MaxNurseryBufferSize) {
        void* buffer = allocate(nbytes);
        if (buffer)
            return buffer;
    }

    void* buffer = zone->pod_malloc<uint8_t>(nbytes);
    if (buffer && !mallocedBuffers.putNew(buffer)) {
        js_free(buffer);
        return nullptr;
    }
    return buffer;
}

void*
js::Nursery::allocateBuffer(JSObject* obj, uint32_t nbytes)
{
    MOZ_ASSERT(obj);
    MOZ_ASSERT(nbytes > 0);

    if (!IsInsideNursery(obj))
        return obj->zone()->pod_malloc<uint8_t>(nbytes);
    return allocateBuffer(obj->zone(), nbytes);
}

void*
js::Nursery::reallocateBuffer(JSObject* obj, void* oldBuffer,
                              uint32_t oldBytes, uint32_t newBytes)
{
    if (!IsInsideNursery(obj))
        return obj->zone()->pod_realloc<uint8_t>((uint8_t*)oldBuffer, oldBytes, newBytes);

    if (!isInside(oldBuffer)) {
        void* newBuffer = obj->zone()->pod_realloc<uint8_t>((uint8_t*)oldBuffer, oldBytes, newBytes);
        if (newBuffer && oldBuffer != newBuffer)
            MOZ_ALWAYS_TRUE(mallocedBuffers.rekeyAs(oldBuffer, newBuffer, newBuffer));
        return newBuffer;
    }

    /* The nursery cannot make use of the returned slots data. */
    if (newBytes < oldBytes)
        return oldBuffer;

    void* newBuffer = allocateBuffer(obj->zone(), newBytes);
    if (newBuffer)
        PodCopy((uint8_t*)newBuffer, (uint8_t*)oldBuffer, oldBytes);
    return newBuffer;
}

void
js::Nursery::freeBuffer(void* buffer)
{
    if (!isInside(buffer)) {
        removeMallocedBuffer(buffer);
        js_free(buffer);
    }
}

void
Nursery::setForwardingPointer(void* oldData, void* newData, bool direct)
{
    MOZ_ASSERT(isInside(oldData));

    // Bug 1196210: If a zero-capacity header lands in the last 2 words of the
    // jemalloc chunk abutting the start of the nursery, the (invalid) newData
    // pointer will appear to be "inside" the nursery.
    MOZ_ASSERT(!isInside(newData) || uintptr_t(newData) == heapStart_);

    if (direct) {
        *reinterpret_cast<void**>(oldData) = newData;
    } else {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!forwardedBuffers.initialized() && !forwardedBuffers.init())
            oomUnsafe.crash("Nursery::setForwardingPointer");
#ifdef DEBUG
        if (ForwardedBufferMap::Ptr p = forwardedBuffers.lookup(oldData))
            MOZ_ASSERT(p->value() == newData);
#endif
        if (!forwardedBuffers.put(oldData, newData))
            oomUnsafe.crash("Nursery::setForwardingPointer");
    }
}

void
Nursery::setSlotsForwardingPointer(HeapSlot* oldSlots, HeapSlot* newSlots, uint32_t nslots)
{
    // Slot arrays always have enough space for a forwarding pointer, since the
    // number of slots is never zero.
    MOZ_ASSERT(nslots > 0);
    setForwardingPointer(oldSlots, newSlots, /* direct = */ true);
}

void
Nursery::setElementsForwardingPointer(ObjectElements* oldHeader, ObjectElements* newHeader,
                                      uint32_t nelems)
{
    // Only use a direct forwarding pointer if there is enough space for one.
    setForwardingPointer(oldHeader->elements(), newHeader->elements(),
                         nelems > ObjectElements::VALUES_PER_HEADER);
}

#ifdef DEBUG
static bool IsWriteableAddress(void* ptr)
{
    volatile uint64_t* vPtr = reinterpret_cast<volatile uint64_t*>(ptr);
    *vPtr = *vPtr;
    return true;
}
#endif

void
js::Nursery::forwardBufferPointer(HeapSlot** pSlotsElems)
{
    HeapSlot* old = *pSlotsElems;

    if (!isInside(old))
        return;

    // The new location for this buffer is either stored inline with it or in
    // the forwardedBuffers table.
    do {
        if (forwardedBuffers.initialized()) {
            if (ForwardedBufferMap::Ptr p = forwardedBuffers.lookup(old)) {
                *pSlotsElems = reinterpret_cast<HeapSlot*>(p->value());
                break;
            }
        }

        *pSlotsElems = *reinterpret_cast<HeapSlot**>(old);
    } while (false);

    MOZ_ASSERT(!isInside(*pSlotsElems));
    MOZ_ASSERT(IsWriteableAddress(*pSlotsElems));
}

js::TenuringTracer::TenuringTracer(JSRuntime* rt, Nursery* nursery)
  : JSTracer(rt, JSTracer::TracerKindTag::Tenuring, TraceWeakMapKeysValues)
  , nursery_(*nursery)
  , tenuredSize(0)
  , head(nullptr)
  , tail(&head)
{
}

#define TIME_START(name) int64_t timestampStart_##name = enableProfiling_ ? PRMJ_Now() : 0
#define TIME_END(name) int64_t timestampEnd_##name = enableProfiling_ ? PRMJ_Now() : 0
#define TIME_TOTAL(name) (timestampEnd_##name - timestampStart_##name)

void
js::Nursery::collect(JSRuntime* rt, JS::gcreason::Reason reason, ObjectGroupList* pretenureGroups)
{
    if (rt->mainThread.suppressGC)
        return;

    JS_AbortIfWrongThread(rt);

    StoreBuffer& sb = rt->gc.storeBuffer;
    if (!isEnabled() || isEmpty()) {
        /*
         * Our barriers are not always exact, and there may be entries in the
         * storebuffer even when the nursery is disabled or empty. It's not
         * safe to keep these entries as they may refer to tenured cells which
         * may be freed after this point.
         */
        sb.clear();
        return;
    }

    rt->gc.incMinorGcNumber();

    rt->gc.stats.count(gcstats::STAT_MINOR_GC);

    TraceMinorGCStart();

    int64_t timestampStart_total = PRMJ_Now();

    AutoTraceSession session(rt, JS::HeapState::MinorCollecting);
    AutoStopVerifyingBarriers av(rt, false);
    AutoDisableProxyCheck disableStrictProxyChecking(rt);
    mozilla::DebugOnly<AutoEnterOOMUnsafeRegion> oomUnsafeRegion;

    // Move objects pointed to by roots from the nursery to the major heap.
    TenuringTracer mover(rt, this);

    // Mark the store buffer. This must happen first.

    TIME_START(cancelIonCompilations);
    if (sb.cancelIonCompilations()) {
        for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next())
            jit::StopAllOffThreadCompilations(c);
    }
    TIME_END(cancelIonCompilations);

    TIME_START(traceValues);
    sb.traceValues(mover);
    TIME_END(traceValues);

    TIME_START(traceCells);
    sb.traceCells(mover);
    TIME_END(traceCells);

    TIME_START(traceSlots);
    sb.traceSlots(mover);
    TIME_END(traceSlots);

    TIME_START(traceWholeCells);
    sb.traceWholeCells(mover);
    TIME_END(traceWholeCells);

    TIME_START(traceGenericEntries);
    sb.traceGenericEntries(&mover);
    TIME_END(traceGenericEntries);

    TIME_START(markRuntime);
    rt->gc.markRuntime(&mover);
    TIME_END(markRuntime);

    TIME_START(markDebugger);
    {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_MARK_ROOTS);
        Debugger::markAll(&mover);
    }
    TIME_END(markDebugger);

    TIME_START(clearNewObjectCache);
    rt->newObjectCache.clearNurseryObjects(rt);
    TIME_END(clearNewObjectCache);

    // Most of the work is done here. This loop iterates over objects that have
    // been moved to the major heap. If these objects have any outgoing pointers
    // to the nursery, then those nursery objects get moved as well, until no
    // objects are left to move. That is, we iterate to a fixed point.
    TIME_START(collectToFP);
    TenureCountCache tenureCounts;
    collectToFixedPoint(mover, tenureCounts);
    TIME_END(collectToFP);

    // Sweep compartments to update the array buffer object's view lists.
    TIME_START(sweepArrayBufferViewList);
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next())
        c->sweepAfterMinorGC();
    TIME_END(sweepArrayBufferViewList);

    // Update any slot or element pointers whose destination has been tenured.
    TIME_START(updateJitActivations);
    js::jit::UpdateJitActivationsForMinorGC(rt, &mover);
    forwardedBuffers.finish();
    TIME_END(updateJitActivations);

    // Sweep.
    TIME_START(freeMallocedBuffers);
    freeMallocedBuffers();
    TIME_END(freeMallocedBuffers);

    TIME_START(sweep);
    sweep();
    TIME_END(sweep);

    TIME_START(clearStoreBuffer);
    rt->gc.storeBuffer.clear();
    TIME_END(clearStoreBuffer);

    // Make sure hashtables have been updated after the collection.
    TIME_START(checkHashTables);
#ifdef JS_GC_ZEAL
    if (rt->gcZeal() == ZealCheckHashTablesOnMinorGC)
        CheckHashTablesAfterMovingGC(rt);
#endif
    TIME_END(checkHashTables);

    // Resize the nursery.
    TIME_START(resize);
    double promotionRate = mover.tenuredSize / double(allocationEnd() - start());
    if (promotionRate > 0.05)
        growAllocableSpace();
    else if (promotionRate < 0.01)
        shrinkAllocableSpace();
    TIME_END(resize);

    // If we are promoting the nursery, or exhausted the store buffer with
    // pointers to nursery things, which will force a collection well before
    // the nursery is full, look for object groups that are getting promoted
    // excessively and try to pretenure them.
    TIME_START(pretenure);
    if (pretenureGroups && (promotionRate > 0.8 || reason == JS::gcreason::FULL_STORE_BUFFER)) {
        for (size_t i = 0; i < ArrayLength(tenureCounts.entries); i++) {
            const TenureCount& entry = tenureCounts.entries[i];
            if (entry.count >= 3000)
                pretenureGroups->append(entry.group); // ignore alloc failure
        }
    }
    TIME_END(pretenure);

    TIME_START(logPromotionsToTenured);
    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        zone->logPromotionsToTenured();
    }
    TIME_END(logPromotionsToTenured);

    // We ignore gcMaxBytes when allocating for minor collection. However, if we
    // overflowed, we disable the nursery. The next time we allocate, we'll fail
    // because gcBytes >= gcMaxBytes.
    if (rt->gc.usage.gcBytes() >= rt->gc.tunables.gcMaxBytes())
        disable();

    int64_t totalTime = PRMJ_Now() - timestampStart_total;
    rt->addTelemetry(JS_TELEMETRY_GC_MINOR_US, totalTime);
    rt->addTelemetry(JS_TELEMETRY_GC_MINOR_REASON, reason);
    if (totalTime > 1000)
        rt->addTelemetry(JS_TELEMETRY_GC_MINOR_REASON_LONG, reason);

    TraceMinorGCEnd();

    if (enableProfiling_ && totalTime >= profileThreshold_) {
        struct {
            const char* name;
            int64_t time;
        } PrintList[] = {
            {"canIon", TIME_TOTAL(cancelIonCompilations)},
            {"mkVals", TIME_TOTAL(traceValues)},
            {"mkClls", TIME_TOTAL(traceCells)},
            {"mkSlts", TIME_TOTAL(traceSlots)},
            {"mcWCll", TIME_TOTAL(traceWholeCells)},
            {"mkGnrc", TIME_TOTAL(traceGenericEntries)},
            {"ckTbls", TIME_TOTAL(checkHashTables)},
            {"mkRntm", TIME_TOTAL(markRuntime)},
            {"mkDbgr", TIME_TOTAL(markDebugger)},
            {"clrNOC", TIME_TOTAL(clearNewObjectCache)},
            {"collct", TIME_TOTAL(collectToFP)},
            {"swpABO", TIME_TOTAL(sweepArrayBufferViewList)},
            {"updtIn", TIME_TOTAL(updateJitActivations)},
            {"frSlts", TIME_TOTAL(freeMallocedBuffers)},
            {" clrSB", TIME_TOTAL(clearStoreBuffer)},
            {" sweep", TIME_TOTAL(sweep)},
            {"resize", TIME_TOTAL(resize)},
            {"pretnr", TIME_TOTAL(pretenure)},
            {"logPtT", TIME_TOTAL(logPromotionsToTenured)}
        };
        static int printedHeader = 0;
        if ((printedHeader++ % 200) == 0) {
            fprintf(stderr, "MinorGC:               Reason  PRate Size    Time");
            for (auto &entry : PrintList)
                fprintf(stderr, " %s", entry.name);
            fprintf(stderr, "\n");
        }

#define FMT " %6" PRIu64
        fprintf(stderr, "MinorGC: %20s %5.1f%% %4d " FMT, js::gcstats::ExplainReason(reason),
                promotionRate * 100, numActiveChunks_, totalTime);
        for (auto &entry : PrintList) {
            fprintf(stderr, FMT, entry.time);
        }
        fprintf(stderr, "\n");
#undef FMT
    }
}

#undef TIME_START
#undef TIME_END
#undef TIME_TOTAL

void
js::Nursery::FreeMallocedBuffersTask::transferBuffersToFree(MallocedBuffersSet& buffersToFree)
{
    // Transfer the contents of the source set to the task's buffers_ member by
    // swapping the sets, which also clears the source.
    MOZ_ASSERT(!isRunning());
    MOZ_ASSERT(buffers_.empty());
    mozilla::Swap(buffers_, buffersToFree);
}

void
js::Nursery::FreeMallocedBuffersTask::run()
{
    for (MallocedBuffersSet::Range r = buffers_.all(); !r.empty(); r.popFront())
        fop_->free_(r.front());
    buffers_.clear();
}

void
js::Nursery::freeMallocedBuffers()
{
    if (mallocedBuffers.empty())
        return;

    bool started;
    {
        AutoLockHelperThreadState lock;
        freeMallocedBuffersTask->joinWithLockHeld();
        freeMallocedBuffersTask->transferBuffersToFree(mallocedBuffers);
        started = freeMallocedBuffersTask->startWithLockHeld();
    }

    if (!started)
        freeMallocedBuffersTask->runFromMainThread(runtime());

    MOZ_ASSERT(mallocedBuffers.empty());
}

void
js::Nursery::waitBackgroundFreeEnd()
{
    MOZ_ASSERT(freeMallocedBuffersTask);
    freeMallocedBuffersTask->join();
}

void
js::Nursery::sweep()
{
    /* Sweep unique id's in all in-use chunks. */
    for (CellsWithUniqueIdSet::Enum e(cellsWithUid_); !e.empty(); e.popFront()) {
        JSObject* obj = static_cast<JSObject*>(e.front());
        if (!IsForwarded(obj))
            obj->zone()->removeUniqueId(obj);
        else
            MOZ_ASSERT(Forwarded(obj)->zone()->hasUniqueId(Forwarded(obj)));
    }
    cellsWithUid_.clear();

#ifdef JS_GC_ZEAL
    /* Poison the nursery contents so touching a freed object will crash. */
    JS_POISON((void*)start(), JS_SWEPT_NURSERY_PATTERN, nurserySize());
    for (int i = 0; i < numNurseryChunks_; ++i)
        initChunk(i);

    if (runtime()->gcZeal() == ZealGenerationalGCValue) {
        MOZ_ASSERT(numActiveChunks_ == numNurseryChunks_);

        /* Only reset the alloc point when we are close to the end. */
        if (currentChunk_ + 1 == numNurseryChunks_)
            setCurrentChunk(0);
    } else
#endif
    {
#ifdef JS_CRASH_DIAGNOSTICS
        JS_POISON((void*)start(), JS_SWEPT_NURSERY_PATTERN, allocationEnd() - start());
        for (int i = 0; i < numActiveChunks_; ++i)
            initChunk(i);
#endif
        setCurrentChunk(0);
    }

    /* Set current start position for isEmpty checks. */
    currentStart_ = position();
    MemProfiler::SweepNursery(runtime());
}

void
js::Nursery::growAllocableSpace()
{
#ifdef JS_GC_ZEAL
    MOZ_ASSERT_IF(runtime()->gcZeal() == ZealGenerationalGCValue,
                  numActiveChunks_ == numNurseryChunks_);
#endif
    numActiveChunks_ = Min(numActiveChunks_ * 2, numNurseryChunks_);
}

void
js::Nursery::shrinkAllocableSpace()
{
#ifdef JS_GC_ZEAL
    if (runtime()->gcZeal() == ZealGenerationalGCValue)
        return;
#endif
    numActiveChunks_ = Max(numActiveChunks_ - 1, 1);
    updateDecommittedRegion();
}
