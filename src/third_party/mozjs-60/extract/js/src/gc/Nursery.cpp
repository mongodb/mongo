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
#include "mozilla/Unused.h"

#include "jsutil.h"

#include "gc/FreeOp.h"
#include "gc/GCInternals.h"
#include "gc/Memory.h"
#include "gc/PublicIterators.h"
#include "jit/JitFrames.h"
#include "vm/ArrayObject.h"
#include "vm/Debugger.h"
#if defined(DEBUG)
#include "vm/EnvironmentObject.h"
#endif
#include "vm/JSCompartment.h"
#include "vm/JSONPrinter.h"
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"
#include "vm/TypeInference.h"

#include "gc/Marking-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace gc;

using mozilla::DebugOnly;
using mozilla::PodCopy;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

constexpr uintptr_t CanaryMagicValue = 0xDEADB15D;

struct js::Nursery::FreeMallocedBuffersTask : public GCParallelTaskHelper<FreeMallocedBuffersTask>
{
    explicit FreeMallocedBuffersTask(FreeOp* fop)
      : GCParallelTaskHelper(fop->runtime()),
        fop_(fop) {}
    bool init() { return buffers_.init(); }
    void transferBuffersToFree(MallocedBuffersSet& buffersToFree,
                               const AutoLockHelperThreadState& lock);
    ~FreeMallocedBuffersTask() { join(); }

    void run();

  private:
    FreeOp* fop_;
    MallocedBuffersSet buffers_;
};

#ifdef JS_GC_ZEAL
struct js::Nursery::Canary
{
    uintptr_t magicValue;
    Canary* next;
};
#endif

namespace js {
struct NurseryChunk {
    char data[Nursery::NurseryChunkUsableSize];
    gc::ChunkTrailer trailer;
    static NurseryChunk* fromChunk(gc::Chunk* chunk);
    void init(JSRuntime* rt);
    void poisonAndInit(JSRuntime* rt, uint8_t poison);
    uintptr_t start() const { return uintptr_t(&data); }
    uintptr_t end() const { return uintptr_t(&trailer); }
    gc::Chunk* toChunk(JSRuntime* rt);
};
static_assert(sizeof(js::NurseryChunk) == gc::ChunkSize,
              "Nursery chunk size must match gc::Chunk size.");

} /* namespace js */

inline void
js::NurseryChunk::poisonAndInit(JSRuntime* rt, uint8_t poison)
{
    JS_POISON(this, poison, ChunkSize);
    init(rt);
}

inline void
js::NurseryChunk::init(JSRuntime* rt)
{
    new (&trailer) gc::ChunkTrailer(rt, &rt->gc.storeBuffer());
}

/* static */ inline js::NurseryChunk*
js::NurseryChunk::fromChunk(Chunk* chunk)
{
    return reinterpret_cast<NurseryChunk*>(chunk);
}

inline Chunk*
js::NurseryChunk::toChunk(JSRuntime* rt)
{
    auto chunk = reinterpret_cast<Chunk*>(this);
    chunk->init(rt);
    return chunk;
}

js::Nursery::Nursery(JSRuntime* rt)
  : runtime_(rt)
  , position_(0)
  , currentStartChunk_(0)
  , currentStartPosition_(0)
  , currentEnd_(0)
  , currentStringEnd_(0)
  , currentChunk_(0)
  , maxChunkCount_(0)
  , chunkCountLimit_(0)
  , timeInChunkAlloc_(0)
  , previousPromotionRate_(0)
  , profileThreshold_(0)
  , enableProfiling_(false)
  , canAllocateStrings_(false)
  , reportTenurings_(0)
  , minorGCTriggerReason_(JS::gcreason::NO_REASON)
  , minorGcCount_(0)
  , freeMallocedBuffersTask(nullptr)
#ifdef JS_GC_ZEAL
  , lastCanary_(nullptr)
#endif
{
    const char* env = getenv("MOZ_NURSERY_STRINGS");
    if (env && *env)
        canAllocateStrings_ = (*env == '1');
}

bool
js::Nursery::init(uint32_t maxNurseryBytes, AutoLockGCBgAlloc& lock)
{
    if (!mallocedBuffers.init())
        return false;

    freeMallocedBuffersTask = js_new<FreeMallocedBuffersTask>(runtime()->defaultFreeOp());
    if (!freeMallocedBuffersTask || !freeMallocedBuffersTask->init())
        return false;

    /* maxNurseryBytes parameter is rounded down to a multiple of chunk size. */
    chunkCountLimit_ = maxNurseryBytes >> ChunkShift;

    /* If no chunks are specified then the nursery is permanently disabled. */
    if (chunkCountLimit_ == 0)
        return true;

    maxChunkCount_ = 1;
    if (!allocateNextChunk(0, lock)) {
        maxChunkCount_ = 0;
        return false;
    }
    /* After this point the Nursery has been enabled */

    setCurrentChunk(0);
    setStartPosition();

    char* env = getenv("JS_GC_PROFILE_NURSERY");
    if (env) {
        if (0 == strcmp(env, "help")) {
            fprintf(stderr, "JS_GC_PROFILE_NURSERY=N\n"
                    "\tReport minor GC's taking at least N microseconds.\n");
            exit(0);
        }
        enableProfiling_ = true;
        profileThreshold_ = TimeDuration::FromMicroseconds(atoi(env));
    }

    env = getenv("JS_GC_REPORT_TENURING");
    if (env) {
        if (0 == strcmp(env, "help")) {
            fprintf(stderr, "JS_GC_REPORT_TENURING=N\n"
                    "\tAfter a minor GC, report any ObjectGroups with at least N instances tenured.\n");
            exit(0);
        }
        reportTenurings_ = atoi(env);
    }

    if (!runtime()->gc.storeBuffer().enable())
        return false;

    MOZ_ASSERT(isEnabled());
    return true;
}

js::Nursery::~Nursery()
{
    disable();
    js_delete(freeMallocedBuffersTask);
}

void
js::Nursery::enable()
{
    MOZ_ASSERT(isEmpty());
    MOZ_ASSERT(!runtime()->gc.isVerifyPreBarriersEnabled());
    if (isEnabled() || !chunkCountLimit())
        return;

    {
        AutoLockGCBgAlloc lock(runtime());
        maxChunkCount_ = 1;
        if (!allocateNextChunk(0, lock)) {
            maxChunkCount_ = 0;
            return;
        }
    }

    setCurrentChunk(0);
    setStartPosition();
#ifdef JS_GC_ZEAL
    if (runtime()->hasZealMode(ZealMode::GenerationalGC))
        enterZealMode();
#endif

    MOZ_ALWAYS_TRUE(runtime()->gc.storeBuffer().enable());
}

void
js::Nursery::disable()
{
    MOZ_ASSERT(isEmpty());
    if (!isEnabled())
        return;

    freeChunksFrom(0);
    maxChunkCount_ = 0;

    currentEnd_ = 0;
    currentStringEnd_ = 0;
    runtime()->gc.storeBuffer().disable();
}

void
js::Nursery::enableStrings()
{
    MOZ_ASSERT(isEmpty());
    canAllocateStrings_ = true;
    currentStringEnd_ = currentEnd_;
}

void
js::Nursery::disableStrings()
{
    MOZ_ASSERT(isEmpty());
    canAllocateStrings_ = false;
    currentStringEnd_ = 0;
}

bool
js::Nursery::isEmpty() const
{
    if (!isEnabled())
        return true;

    if (!runtime()->hasZealMode(ZealMode::GenerationalGC)) {
        MOZ_ASSERT(currentStartChunk_ == 0);
        MOZ_ASSERT(currentStartPosition_ == chunk(0).start());
    }
    return position() == currentStartPosition_;
}

#ifdef JS_GC_ZEAL
void
js::Nursery::enterZealMode() {
    if (isEnabled())
        maxChunkCount_ = chunkCountLimit();
}

void
js::Nursery::leaveZealMode() {
    if (isEnabled()) {
        MOZ_ASSERT(isEmpty());
        setCurrentChunk(0);
        setStartPosition();
    }
}
#endif // JS_GC_ZEAL

JSObject*
js::Nursery::allocateObject(JSContext* cx, size_t size, size_t nDynamicSlots, const js::Class* clasp)
{
    /* Ensure there's enough space to replace the contents with a RelocationOverlay. */
    MOZ_ASSERT(size >= sizeof(RelocationOverlay));

    /* Sanity check the finalizer. */
    MOZ_ASSERT_IF(clasp->hasFinalize(), CanNurseryAllocateFinalizedClass(clasp) ||
                                        clasp->isProxy());

    /* Make the object allocation. */
    JSObject* obj = static_cast<JSObject*>(allocate(size));
    if (!obj)
        return nullptr;

    /* If we want external slots, add them. */
    HeapSlot* slots = nullptr;
    if (nDynamicSlots) {
        MOZ_ASSERT(clasp->isNative());
        slots = static_cast<HeapSlot*>(allocateBuffer(cx->zone(), nDynamicSlots * sizeof(HeapSlot)));
        if (!slots) {
            /*
             * It is safe to leave the allocated object uninitialized, since we
             * do not visit unallocated things in the nursery.
             */
            return nullptr;
        }
    }

    /* Store slots pointer directly in new object. If no dynamic slots were
     * requested, caller must initialize slots_ field itself as needed. We
     * don't know if the caller was a native object or not. */
    if (nDynamicSlots)
        static_cast<NativeObject*>(obj)->initSlots(slots);

    TraceNurseryAlloc(obj, size);
    return obj;
}

Cell*
js::Nursery::allocateString(Zone* zone, size_t size, AllocKind kind)
{
    /* Ensure there's enough space to replace the contents with a RelocationOverlay. */
    MOZ_ASSERT(size >= sizeof(RelocationOverlay));

    size_t allocSize = JS_ROUNDUP(sizeof(StringLayout) - 1 + size, CellAlignBytes);
    auto header = static_cast<StringLayout*>(allocate(allocSize));
    if (!header)
        return nullptr;
    header->zone = zone;

    auto cell = reinterpret_cast<Cell*>(&header->cell);
    TraceNurseryAlloc(cell, kind);
    return cell;
}

void*
js::Nursery::allocate(size_t size)
{
    MOZ_ASSERT(isEnabled());
    MOZ_ASSERT(!JS::CurrentThreadIsHeapBusy());
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));
    MOZ_ASSERT_IF(currentChunk_ == currentStartChunk_, position() >= currentStartPosition_);
    MOZ_ASSERT(position() % CellAlignBytes == 0);
    MOZ_ASSERT(size % CellAlignBytes == 0);

#ifdef JS_GC_ZEAL
    static const size_t CanarySize = (sizeof(Nursery::Canary) + CellAlignBytes - 1) & ~CellAlignMask;
    if (runtime()->gc.hasZealMode(ZealMode::CheckNursery))
        size += CanarySize;
#endif

    if (currentEnd() < position() + size) {
        unsigned chunkno = currentChunk_ + 1;
        MOZ_ASSERT(chunkno <= chunkCountLimit());
        MOZ_ASSERT(chunkno <= maxChunkCount());
        MOZ_ASSERT(chunkno <= allocatedChunkCount());
        if (chunkno == maxChunkCount())
            return nullptr;
        if (MOZ_UNLIKELY(chunkno == allocatedChunkCount())) {
            mozilla::TimeStamp start = TimeStamp::Now();
            {
                AutoLockGCBgAlloc lock(runtime());
                if (!allocateNextChunk(chunkno, lock))
                    return nullptr;
            }
            timeInChunkAlloc_ += TimeStamp::Now() - start;
            MOZ_ASSERT(chunkno < allocatedChunkCount());
        }
        setCurrentChunk(chunkno);
    }

    void* thing = (void*)position();
    position_ = position() + size;

    JS_EXTRA_POISON(thing, JS_ALLOCATED_NURSERY_PATTERN, size);

#ifdef JS_GC_ZEAL
    if (runtime()->gc.hasZealMode(ZealMode::CheckNursery)) {
        auto canary = reinterpret_cast<Canary*>(position() - CanarySize);
        canary->magicValue = CanaryMagicValue;
        canary->next = nullptr;
        if (lastCanary_) {
            MOZ_ASSERT(!lastCanary_->next);
            lastCanary_->next = canary;
        }
        lastCanary_ = canary;
    }
#endif

    return thing;
}

void*
js::Nursery::allocateBuffer(Zone* zone, size_t nbytes)
{
    MOZ_ASSERT(nbytes > 0);

    if (nbytes <= MaxNurseryBufferSize) {
        void* buffer = allocate(nbytes);
        if (buffer)
            return buffer;
    }

    void* buffer = zone->pod_malloc<uint8_t>(nbytes);
    if (buffer && !registerMallocedBuffer(buffer)) {
        js_free(buffer);
        return nullptr;
    }
    return buffer;
}

void*
js::Nursery::allocateBuffer(JSObject* obj, size_t nbytes)
{
    MOZ_ASSERT(obj);
    MOZ_ASSERT(nbytes > 0);

    if (!IsInsideNursery(obj))
        return obj->zone()->pod_malloc<uint8_t>(nbytes);
    return allocateBuffer(obj->zone(), nbytes);
}

void*
js::Nursery::allocateBufferSameLocation(JSObject* obj, size_t nbytes)
{
    MOZ_ASSERT(obj);
    MOZ_ASSERT(nbytes > 0);
    MOZ_ASSERT(nbytes <= MaxNurseryBufferSize);

    if (!IsInsideNursery(obj))
        return obj->zone()->pod_malloc<uint8_t>(nbytes);

    return allocate(nbytes);
}

void*
js::Nursery::reallocateBuffer(JSObject* obj, void* oldBuffer,
                              size_t oldBytes, size_t newBytes)
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
Nursery::setIndirectForwardingPointer(void* oldData, void* newData)
{
    MOZ_ASSERT(isInside(oldData));

    // Bug 1196210: If a zero-capacity header lands in the last 2 words of a
    // jemalloc chunk abutting the start of a nursery chunk, the (invalid)
    // newData pointer will appear to be "inside" the nursery.
    MOZ_ASSERT(!isInside(newData) || (uintptr_t(newData) & ChunkMask) == 0);

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
  , objHead(nullptr)
  , objTail(&objHead)
  , stringHead(nullptr)
  , stringTail(&stringHead)
{
}

inline float
js::Nursery::calcPromotionRate(bool *validForTenuring) const {
    float used = float(previousGC.nurseryUsedBytes);
    float capacity = float(previousGC.nurseryCapacity);
    float tenured = float(previousGC.tenuredBytes);
    float rate;

    if (previousGC.nurseryUsedBytes > 0) {
        if (validForTenuring) {
            /*
             * We can only use promotion rates if they're likely to be valid,
             * they're only valid if the nursury was at least 90% full.
             */
            *validForTenuring = used > capacity * 0.9f;
        }
        rate = tenured / used;
    } else {
        if (validForTenuring)
            *validForTenuring = false;
        rate = 0.0f;
    }

    return rate;
}

void
js::Nursery::renderProfileJSON(JSONPrinter& json) const
{
    if (!isEnabled()) {
        json.beginObject();
        json.property("status", "nursery disabled");
        json.endObject();
        return;
    }

    if (previousGC.reason == JS::gcreason::NO_REASON) {
        // If the nursery was empty when the last minorGC was requested, then
        // no nursery collection will have been performed but JSON may still be
        // requested. (And as a public API, this function should not crash in
        // such a case.)
        json.beginObject();
        json.property("status", "nursery empty");
        json.endObject();
        return;
    }

    json.beginObject();

    json.property("status", "complete");

    json.property("reason", JS::gcreason::ExplainReason(previousGC.reason));
    json.property("bytes_tenured", previousGC.tenuredBytes);
    json.property("bytes_used", previousGC.nurseryUsedBytes);
    json.property("cur_capacity", previousGC.nurseryCapacity);
    const size_t newCapacity = spaceToEnd(maxChunkCount());
    if (newCapacity != previousGC.nurseryCapacity)
        json.property("new_capacity", newCapacity);
    if (previousGC.nurseryLazyCapacity != previousGC.nurseryCapacity)
        json.property("lazy_capacity", previousGC.nurseryLazyCapacity);
    if (!timeInChunkAlloc_.IsZero())
        json.property("chunk_alloc_us", timeInChunkAlloc_, json.MICROSECONDS);

    json.beginObjectProperty("phase_times");

#define EXTRACT_NAME(name, text) #name,
    static const char* names[] = {
FOR_EACH_NURSERY_PROFILE_TIME(EXTRACT_NAME)
#undef EXTRACT_NAME
    "" };

    size_t i = 0;
    for (auto time : profileDurations_)
        json.property(names[i++], time, json.MICROSECONDS);

    json.endObject(); // timings value

    json.endObject();
}

/* static */ void
js::Nursery::printProfileHeader()
{
    fprintf(stderr, "MinorGC:               Reason  PRate Size        ");
#define PRINT_HEADER(name, text)                                              \
    fprintf(stderr, " %6s", text);
FOR_EACH_NURSERY_PROFILE_TIME(PRINT_HEADER)
#undef PRINT_HEADER
    fprintf(stderr, "\n");
}

/* static */ void
js::Nursery::printProfileDurations(const ProfileDurations& times)
{
    for (auto time : times)
        fprintf(stderr, " %6" PRIi64, static_cast<int64_t>(time.ToMicroseconds()));
    fprintf(stderr, "\n");
}

void
js::Nursery::printTotalProfileTimes()
{
    if (enableProfiling_) {
        fprintf(stderr, "MinorGC TOTALS: %7" PRIu64 " collections:             ", minorGcCount_);
        printProfileDurations(totalDurations_);
    }
}

void
js::Nursery::maybeClearProfileDurations()
{
    for (auto& duration : profileDurations_)
        duration = mozilla::TimeDuration();
}

inline void
js::Nursery::startProfile(ProfileKey key)
{
    startTimes_[key] = TimeStamp::Now();
}

inline void
js::Nursery::endProfile(ProfileKey key)
{
    profileDurations_[key] = TimeStamp::Now() - startTimes_[key];
    totalDurations_[key] += profileDurations_[key];
}

static inline bool
IsFullStoreBufferReason(JS::gcreason::Reason reason)
{
    return reason == JS::gcreason::FULL_WHOLE_CELL_BUFFER ||
           reason == JS::gcreason::FULL_GENERIC_BUFFER ||
           reason == JS::gcreason::FULL_VALUE_BUFFER ||
           reason == JS::gcreason::FULL_CELL_PTR_BUFFER ||
           reason == JS::gcreason::FULL_SLOT_BUFFER ||
           reason == JS::gcreason::FULL_SHAPE_BUFFER;
}

void
js::Nursery::collect(JS::gcreason::Reason reason)
{
    MOZ_ASSERT(!TlsContext.get()->suppressGC);

    if (!isEnabled() || isEmpty()) {
        // Our barriers are not always exact, and there may be entries in the
        // storebuffer even when the nursery is disabled or empty. It's not safe
        // to keep these entries as they may refer to tenured cells which may be
        // freed after this point.
        runtime()->gc.storeBuffer().clear();
    }

    if (!isEnabled())
        return;

    JSRuntime* rt = runtime();
    rt->gc.incMinorGcNumber();

#ifdef JS_GC_ZEAL
    if (rt->gc.hasZealMode(ZealMode::CheckNursery)) {
        for (auto canary = lastCanary_; canary; canary = canary->next)
            MOZ_ASSERT(canary->magicValue == CanaryMagicValue);
    }
    lastCanary_ = nullptr;
#endif

    rt->gc.stats().beginNurseryCollection(reason);
    TraceMinorGCStart();

    maybeClearProfileDurations();
    startProfile(ProfileKey::Total);

    // The hazard analysis thinks doCollection can invalidate pointers in
    // tenureCounts below.
    JS::AutoSuppressGCAnalysis nogc;

    TenureCountCache tenureCounts;
    previousGC.reason = JS::gcreason::NO_REASON;
    if (!isEmpty()) {
        doCollection(reason, tenureCounts);
    } else {
        previousGC.nurseryUsedBytes = 0;
        previousGC.nurseryCapacity = spaceToEnd(maxChunkCount());
        previousGC.nurseryLazyCapacity = spaceToEnd(allocatedChunkCount());
        previousGC.tenuredBytes = 0;
    }

    // Resize the nursery.
    maybeResizeNursery(reason);

    // If we are promoting the nursery, or exhausted the store buffer with
    // pointers to nursery things, which will force a collection well before
    // the nursery is full, look for object groups that are getting promoted
    // excessively and try to pretenure them.
    startProfile(ProfileKey::Pretenure);
    bool validPromotionRate;
    const float promotionRate = calcPromotionRate(&validPromotionRate);
    uint32_t pretenureCount = 0;
    bool shouldPretenure = (validPromotionRate && promotionRate > 0.6) ||
        IsFullStoreBufferReason(reason);

    if (shouldPretenure) {
        JSContext* cx = TlsContext.get();
        for (auto& entry : tenureCounts.entries) {
            if (entry.count >= 3000) {
                ObjectGroup* group = entry.group;
                if (group->canPreTenure() && group->zone()->group()->canEnterWithoutYielding(cx)) {
                    AutoCompartment ac(cx, group);
                    group->setShouldPreTenure(cx);
                    pretenureCount++;
                }
            }
        }
    }
    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        if (shouldPretenure && zone->allocNurseryStrings && zone->tenuredStrings >= 30 * 1000) {
            JSRuntime::AutoProhibitActiveContextChange apacc(rt);
            CancelOffThreadIonCompile(zone);
            bool preserving = zone->isPreservingCode();
            zone->setPreservingCode(false);
            zone->discardJitCode(rt->defaultFreeOp());
            zone->setPreservingCode(preserving);
            for (CompartmentsInZoneIter c(zone); !c.done(); c.next()) {
                if (jit::JitCompartment* jitComp = c->jitCompartment()) {
                    jitComp->discardStubs();
                    jitComp->stringsCanBeInNursery = false;
                }
            }
            zone->allocNurseryStrings = false;
        }
        zone->tenuredStrings = 0;
    }
    endProfile(ProfileKey::Pretenure);

    // We ignore gcMaxBytes when allocating for minor collection. However, if we
    // overflowed, we disable the nursery. The next time we allocate, we'll fail
    // because gcBytes >= gcMaxBytes.
    if (rt->gc.usage.gcBytes() >= rt->gc.tunables.gcMaxBytes())
        disable();
    // Disable the nursery if the user changed the configuration setting.  The
    // nursery can only be re-enabled by resetting the configurationa and
    // restarting firefox.
    if (chunkCountLimit_ == 0)
        disable();

    endProfile(ProfileKey::Total);
    minorGcCount_++;

    TimeDuration totalTime = profileDurations_[ProfileKey::Total];
    rt->addTelemetry(JS_TELEMETRY_GC_MINOR_US, totalTime.ToMicroseconds());
    rt->addTelemetry(JS_TELEMETRY_GC_MINOR_REASON, reason);
    if (totalTime.ToMilliseconds() > 1.0)
        rt->addTelemetry(JS_TELEMETRY_GC_MINOR_REASON_LONG, reason);
    rt->addTelemetry(JS_TELEMETRY_GC_NURSERY_BYTES, sizeOfHeapCommitted());
    rt->addTelemetry(JS_TELEMETRY_GC_PRETENURE_COUNT, pretenureCount);

    rt->gc.stats().endNurseryCollection(reason);
    TraceMinorGCEnd();
    timeInChunkAlloc_ = mozilla::TimeDuration();

    if (enableProfiling_ && totalTime >= profileThreshold_) {
        rt->gc.stats().maybePrintProfileHeaders();

        fprintf(stderr, "MinorGC: %20s %5.1f%% %4u        ",
                JS::gcreason::ExplainReason(reason),
                promotionRate * 100,
                maxChunkCount());
        printProfileDurations(profileDurations_);

        if (reportTenurings_) {
            for (auto& entry : tenureCounts.entries) {
                if (entry.count >= reportTenurings_) {
                    fprintf(stderr, "  %d x ", entry.count);
                    entry.group->print();
                }
            }
        }
    }
}

void
js::Nursery::doCollection(JS::gcreason::Reason reason,
                          TenureCountCache& tenureCounts)
{
    JSRuntime* rt = runtime();
    AutoTraceSession session(rt, JS::HeapState::MinorCollecting);
    AutoSetThreadIsPerformingGC performingGC;
    AutoStopVerifyingBarriers av(rt, false);
    AutoDisableProxyCheck disableStrictProxyChecking;
    mozilla::DebugOnly<AutoEnterOOMUnsafeRegion> oomUnsafeRegion;

    const size_t initialNurseryCapacity = spaceToEnd(maxChunkCount());
    const size_t initialNurseryUsedBytes = initialNurseryCapacity - freeSpace();

    // Move objects pointed to by roots from the nursery to the major heap.
    TenuringTracer mover(rt, this);

    // Mark the store buffer. This must happen first.
    StoreBuffer& sb = runtime()->gc.storeBuffer();

    // The MIR graph only contains nursery pointers if cancelIonCompilations()
    // is set on the store buffer, in which case we cancel all compilations
    // of such graphs.
    startProfile(ProfileKey::CancelIonCompilations);
    if (sb.cancelIonCompilations())
        js::CancelOffThreadIonCompilesUsingNurseryPointers(rt);
    endProfile(ProfileKey::CancelIonCompilations);

    startProfile(ProfileKey::TraceValues);
    sb.traceValues(mover);
    endProfile(ProfileKey::TraceValues);

    startProfile(ProfileKey::TraceCells);
    sb.traceCells(mover);
    endProfile(ProfileKey::TraceCells);

    startProfile(ProfileKey::TraceSlots);
    sb.traceSlots(mover);
    endProfile(ProfileKey::TraceSlots);

    startProfile(ProfileKey::TraceWholeCells);
    sb.traceWholeCells(mover);
    endProfile(ProfileKey::TraceWholeCells);

    startProfile(ProfileKey::TraceGenericEntries);
    sb.traceGenericEntries(&mover);
    endProfile(ProfileKey::TraceGenericEntries);

    startProfile(ProfileKey::MarkRuntime);
    rt->gc.traceRuntimeForMinorGC(&mover, session);
    endProfile(ProfileKey::MarkRuntime);

    startProfile(ProfileKey::MarkDebugger);
    {
        gcstats::AutoPhase ap(rt->gc.stats(), gcstats::PhaseKind::MARK_ROOTS);
        Debugger::traceAllForMovingGC(&mover);
    }
    endProfile(ProfileKey::MarkDebugger);

    startProfile(ProfileKey::SweepCaches);
    rt->gc.purgeRuntimeForMinorGC();
    endProfile(ProfileKey::SweepCaches);

    // Most of the work is done here. This loop iterates over objects that have
    // been moved to the major heap. If these objects have any outgoing pointers
    // to the nursery, then those nursery objects get moved as well, until no
    // objects are left to move. That is, we iterate to a fixed point.
    startProfile(ProfileKey::CollectToFP);
    collectToFixedPoint(mover, tenureCounts);
    endProfile(ProfileKey::CollectToFP);

    // Sweep to update any pointers to nursery objects that have now been
    // tenured.
    startProfile(ProfileKey::Sweep);
    sweep(&mover);
    endProfile(ProfileKey::Sweep);

    // Update any slot or element pointers whose destination has been tenured.
    startProfile(ProfileKey::UpdateJitActivations);
    js::jit::UpdateJitActivationsForMinorGC(rt);
    forwardedBuffers.finish();
    endProfile(ProfileKey::UpdateJitActivations);

    startProfile(ProfileKey::ObjectsTenuredCallback);
    rt->gc.callObjectsTenuredCallback();
    endProfile(ProfileKey::ObjectsTenuredCallback);

    // Sweep.
    startProfile(ProfileKey::FreeMallocedBuffers);
    freeMallocedBuffers();
    endProfile(ProfileKey::FreeMallocedBuffers);

    startProfile(ProfileKey::ClearNursery);
    clear();
    endProfile(ProfileKey::ClearNursery);

    startProfile(ProfileKey::ClearStoreBuffer);
    runtime()->gc.storeBuffer().clear();
    endProfile(ProfileKey::ClearStoreBuffer);

    // Make sure hashtables have been updated after the collection.
    startProfile(ProfileKey::CheckHashTables);
#ifdef JS_GC_ZEAL
    if (rt->hasZealMode(ZealMode::CheckHashTablesOnMinorGC))
        CheckHashTablesAfterMovingGC(rt);
#endif
    endProfile(ProfileKey::CheckHashTables);

    previousGC.reason = reason;
    previousGC.nurseryCapacity = initialNurseryCapacity;
    previousGC.nurseryLazyCapacity = spaceToEnd(allocatedChunkCount());
    previousGC.nurseryUsedBytes = initialNurseryUsedBytes;
    previousGC.tenuredBytes = mover.tenuredSize;
}

void
js::Nursery::FreeMallocedBuffersTask::transferBuffersToFree(MallocedBuffersSet& buffersToFree,
                                                            const AutoLockHelperThreadState& lock)
{
    // Transfer the contents of the source set to the task's buffers_ member by
    // swapping the sets, which also clears the source.
    MOZ_ASSERT(!isRunningWithLockHeld(lock));
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

bool
js::Nursery::registerMallocedBuffer(void* buffer)
{
    MOZ_ASSERT(buffer);
    return mallocedBuffers.putNew(buffer);
}

void
js::Nursery::freeMallocedBuffers()
{
    if (mallocedBuffers.empty())
        return;

    bool started;
    {
        AutoLockHelperThreadState lock;
        freeMallocedBuffersTask->joinWithLockHeld(lock);
        freeMallocedBuffersTask->transferBuffersToFree(mallocedBuffers, lock);
        started = freeMallocedBuffersTask->startWithLockHeld(lock);
    }

    if (!started)
        freeMallocedBuffersTask->runFromActiveCooperatingThread(runtime());

    MOZ_ASSERT(mallocedBuffers.empty());
}

void
js::Nursery::waitBackgroundFreeEnd()
{
    // We may finishRoots before nursery init if runtime init fails.
    if (!isEnabled())
        return;

    MOZ_ASSERT(freeMallocedBuffersTask);
    freeMallocedBuffersTask->join();
}

void
js::Nursery::sweep(JSTracer* trc)
{
    // Sweep unique IDs first before we sweep any tables that may be keyed based
    // on them.
    for (Cell* cell : cellsWithUid_) {
        JSObject* obj = static_cast<JSObject*>(cell);
        if (!IsForwarded(obj)) {
            obj->zone()->removeUniqueId(obj);
        } else {
            JSObject* dst = Forwarded(obj);
            dst->zone()->transferUniqueId(dst, obj);
        }
    }
    cellsWithUid_.clear();

    for (CompartmentsIter c(runtime(), SkipAtoms); !c.done(); c.next())
        c->sweepAfterMinorGC(trc);

    sweepDictionaryModeObjects();
}

void
js::Nursery::clear()
{
#if defined(JS_GC_ZEAL) || defined(JS_CRASH_DIAGNOSTICS)
    /* Poison the nursery contents so touching a freed object will crash. */
    for (unsigned i = currentStartChunk_; i < allocatedChunkCount(); ++i)
        chunk(i).poisonAndInit(runtime(), JS_SWEPT_NURSERY_PATTERN);
#endif

    if (runtime()->hasZealMode(ZealMode::GenerationalGC)) {
        /* Only reset the alloc point when we are close to the end. */
        if (currentChunk_ + 1 == maxChunkCount())
            setCurrentChunk(0);
    } else {
        setCurrentChunk(0);
    }

    /* Set current start position for isEmpty checks. */
    setStartPosition();
}

size_t
js::Nursery::spaceToEnd(unsigned chunkCount) const
{
    unsigned lastChunk = chunkCount - 1;

    MOZ_ASSERT(lastChunk >= currentStartChunk_);
    MOZ_ASSERT(currentStartPosition_ - chunk(currentStartChunk_).start() <= NurseryChunkUsableSize);

    size_t bytes = (chunk(currentStartChunk_).end() - currentStartPosition_) +
                   ((lastChunk - currentStartChunk_) * NurseryChunkUsableSize);

    MOZ_ASSERT(bytes <= maxChunkCount() * NurseryChunkUsableSize);

    return bytes;
}

MOZ_ALWAYS_INLINE void
js::Nursery::setCurrentChunk(unsigned chunkno)
{
    MOZ_ASSERT(chunkno < chunkCountLimit());
    MOZ_ASSERT(chunkno < allocatedChunkCount());
    currentChunk_ = chunkno;
    position_ = chunk(chunkno).start();
    currentEnd_ = chunk(chunkno).end();
    if (canAllocateStrings_)
        currentStringEnd_ = currentEnd_;
    chunk(chunkno).poisonAndInit(runtime(), JS_FRESH_NURSERY_PATTERN);
}

bool
js::Nursery::allocateNextChunk(const unsigned chunkno,
    AutoLockGCBgAlloc& lock)
{
    const unsigned priorCount = allocatedChunkCount();
    const unsigned newCount = priorCount + 1;

    MOZ_ASSERT((chunkno == currentChunk_ + 1) || (chunkno == 0 && allocatedChunkCount() == 0));
    MOZ_ASSERT(chunkno == allocatedChunkCount());
    MOZ_ASSERT(chunkno < chunkCountLimit());
    MOZ_ASSERT(chunkno < maxChunkCount());

    if (!chunks_.resize(newCount))
        return false;

    Chunk* newChunk;
    newChunk = runtime()->gc.getOrAllocChunk(lock);
    if (!newChunk) {
        chunks_.shrinkTo(priorCount);
        return false;
    }

    chunks_[chunkno] = NurseryChunk::fromChunk(newChunk);
    return true;
}

MOZ_ALWAYS_INLINE void
js::Nursery::setStartPosition()
{
    currentStartChunk_ = currentChunk_;
    currentStartPosition_ = position();
}

void
js::Nursery::maybeResizeNursery(JS::gcreason::Reason reason)
{
    static const double GrowThreshold   = 0.03;
    static const double ShrinkThreshold = 0.01;
    unsigned newMaxNurseryChunks;

    // Shrink the nursery to its minimum size of we ran out of memory or
    // received a memory pressure event.
    if (gc::IsOOMReason(reason)) {
        minimizeAllocableSpace();
        return;
    }

#ifdef JS_GC_ZEAL
    // This zeal mode disabled nursery resizing.
    if (runtime()->hasZealMode(ZealMode::GenerationalGC))
        return;
#endif

    /*
     * This incorrect promotion rate results in better nursery sizing
     * decisions, however we should to better tuning based on the real
     * promotion rate in the future.
     */
    const float promotionRate =
        float(previousGC.tenuredBytes) / float(previousGC.nurseryCapacity);

    newMaxNurseryChunks = runtime()->gc.tunables.gcMaxNurseryBytes() >> ChunkShift;
    if (newMaxNurseryChunks != chunkCountLimit_) {
        chunkCountLimit_ = newMaxNurseryChunks;
        /* The configured maximum nursery size is changing */
        if (maxChunkCount() > newMaxNurseryChunks) {
            /* We need to shrink the nursery */
            shrinkAllocableSpace(newMaxNurseryChunks);

            previousPromotionRate_ = promotionRate;
            return;
        }
    }

    if (promotionRate > GrowThreshold) {
        // The GC nursery is an optimization and so if we fail to allocate
        // nursery chunks we do not report an error.
        growAllocableSpace();
    } else if (promotionRate < ShrinkThreshold && previousPromotionRate_ < ShrinkThreshold) {
        shrinkAllocableSpace(maxChunkCount() - 1);
    }

    previousPromotionRate_ = promotionRate;
}

void
js::Nursery::growAllocableSpace()
{
    maxChunkCount_ = Min(maxChunkCount() * 2, chunkCountLimit());
}

void
js::Nursery::freeChunksFrom(unsigned firstFreeChunk)
{
    MOZ_ASSERT(firstFreeChunk < chunks_.length());
    {
        AutoLockGC lock(runtime());
        for (unsigned i = firstFreeChunk; i < chunks_.length(); i++)
            runtime()->gc.recycleChunk(chunk(i).toChunk(runtime()), lock);
    }
    chunks_.shrinkTo(firstFreeChunk);
}

void
js::Nursery::shrinkAllocableSpace(unsigned newCount)
{
#ifdef JS_GC_ZEAL
    if (runtime()->hasZealMode(ZealMode::GenerationalGC))
        return;
#endif

    // Don't shrink the nursery to zero (use Nursery::disable() instead) and
    // don't attempt to shrink it to the same size.
    if ((newCount == 0) || (newCount == maxChunkCount()))
        return;

    MOZ_ASSERT(newCount < maxChunkCount());

    if (newCount < allocatedChunkCount())
        freeChunksFrom(newCount);

    maxChunkCount_ = newCount;
}

void
js::Nursery::minimizeAllocableSpace()
{
    shrinkAllocableSpace(1);
}

bool
js::Nursery::queueDictionaryModeObjectToSweep(NativeObject* obj)
{
    MOZ_ASSERT(IsInsideNursery(obj));
    return dictionaryModeObjects_.append(obj);
}

uintptr_t
js::Nursery::currentEnd() const
{
    MOZ_ASSERT(currentEnd_ == chunk(currentChunk_).end());
    return currentEnd_;
}

void
js::Nursery::sweepDictionaryModeObjects()
{
    for (auto obj : dictionaryModeObjects_) {
        if (!IsForwarded(obj))
            obj->sweepDictionaryListPointer();
        else
            Forwarded(obj)->updateDictionaryListPointerAfterMinorGC(obj);
    }
    dictionaryModeObjects_.clear();
}


JS_PUBLIC_API(void)
JS::EnableNurseryStrings(JSContext* cx)
{
    AutoEmptyNursery empty(cx);
    ReleaseAllJITCode(cx->runtime()->defaultFreeOp());
    cx->runtime()->gc.nursery().enableStrings();
}

JS_PUBLIC_API(void)
JS::DisableNurseryStrings(JSContext* cx)
{
    AutoEmptyNursery empty(cx);
    ReleaseAllJITCode(cx->runtime()->defaultFreeOp());
    cx->runtime()->gc.nursery().disableStrings();
}
