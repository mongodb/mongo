/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Runtime-inl.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/ThreadLocal.h"

#if defined(XP_DARWIN)
#include <mach/mach.h>
#elif defined(XP_UNIX)
#include <sys/resource.h>
#elif defined(XP_WIN)
#include <processthreadsapi.h>
#include <windows.h>
#endif // defined(XP_DARWIN) || defined(XP_UNIX) || defined(XP_WIN)

#include <locale.h>
#include <string.h>

#ifdef JS_CAN_CHECK_THREADSAFE_ACCESSES
# include <sys/mman.h>
#endif

#include "jsatom.h"
#include "jsdtoa.h"
#include "jsgc.h"
#include "jsmath.h"
#include "jsnativestack.h"
#include "jsobj.h"
#include "jsscript.h"
#include "jswatchpoint.h"
#include "jswin.h"
#include "jswrapper.h"

#include "asmjs/AsmJSSignalHandlers.h"
#include "jit/arm/Simulator-arm.h"
#include "jit/arm64/vixl/Simulator-vixl.h"
#include "jit/JitCompartment.h"
#include "jit/mips32/Simulator-mips32.h"
#include "jit/mips64/Simulator-mips64.h"
#include "jit/PcScriptCache.h"
#include "js/Date.h"
#include "js/MemoryMetrics.h"
#include "js/SliceBudget.h"
#include "vm/Debugger.h"

#include "jscntxtinlines.h"
#include "jsgcinlines.h"

using namespace js;
using namespace js::gc;

using mozilla::Atomic;
using mozilla::DebugOnly;
using mozilla::NegativeInfinity;
using mozilla::PodZero;
using mozilla::PodArrayZero;
using mozilla::PositiveInfinity;
using mozilla::ThreadLocal;
using JS::GenericNaN;
using JS::DoubleNaNValue;

/* static */ ThreadLocal<PerThreadData*> js::TlsPerThreadData;
/* static */ Atomic<size_t> JSRuntime::liveRuntimesCount;

namespace js {
    bool gCanUseExtraThreads = true;
} // namespace js

void
js::DisableExtraThreads()
{
    gCanUseExtraThreads = false;
}

const JSSecurityCallbacks js::NullSecurityCallbacks = { };

PerThreadData::PerThreadData(JSRuntime* runtime)
  : PerThreadDataFriendFields(),
    runtime_(runtime),
#ifdef JS_TRACE_LOGGING
    traceLogger(nullptr),
#endif
    autoFlushICache_(nullptr),
    dtoaState(nullptr),
    suppressGC(0),
#ifdef DEBUG
    ionCompiling(false),
    ionCompilingSafeForMinorGC(false),
    gcSweeping(false),
#endif
    activeCompilations(0)
{}

PerThreadData::~PerThreadData()
{
    if (dtoaState)
        DestroyDtoaState(dtoaState);
}

bool
PerThreadData::init()
{
    dtoaState = NewDtoaState();
    if (!dtoaState)
        return false;

    return true;
}

static const JSWrapObjectCallbacks DefaultWrapObjectCallbacks = {
    TransparentObjectWrapper,
    nullptr
};

static size_t
ReturnZeroSize(const void* p)
{
    return 0;
}

JSRuntime::JSRuntime(JSRuntime* parentRuntime)
  : mainThread(this),
    jitTop(nullptr),
    jitJSContext(nullptr),
    jitActivation(nullptr),
    jitStackLimit_(0xbad),
    jitStackLimitNoInterrupt_(0xbad),
    activation_(nullptr),
    profilingActivation_(nullptr),
    profilerSampleBufferGen_(0),
    profilerSampleBufferLapCount_(1),
    asmJSActivationStack_(nullptr),
    asyncStackForNewActivations(this),
    asyncCauseForNewActivations(this),
    asyncCallIsExplicit(false),
    entryMonitor(nullptr),
    parentRuntime(parentRuntime),
    interrupt_(false),
    telemetryCallback(nullptr),
    handlingSignal(false),
    interruptCallback(nullptr),
    exclusiveAccessLock(nullptr),
    exclusiveAccessOwner(nullptr),
    mainThreadHasExclusiveAccess(false),
    numExclusiveThreads(0),
    numCompartments(0),
    localeCallbacks(nullptr),
    defaultLocale(nullptr),
    defaultVersion_(JSVERSION_DEFAULT),
    ownerThread_(nullptr),
    ownerThreadNative_(0),
    tempLifoAlloc(TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
    jitRuntime_(nullptr),
    selfHostingGlobal_(nullptr),
    nativeStackBase(GetNativeStackBase()),
    cxCallback(nullptr),
    destroyCompartmentCallback(nullptr),
    destroyZoneCallback(nullptr),
    sweepZoneCallback(nullptr),
    compartmentNameCallback(nullptr),
    activityCallback(nullptr),
    activityCallbackArg(nullptr),
    requestDepth(0),
#ifdef DEBUG
    checkRequestDepth(0),
    activeContext(nullptr),
#endif
    gc(thisFromCtor()),
    gcInitialized(false),
#ifdef JS_SIMULATOR
    simulator_(nullptr),
#endif
    scriptAndCountsVector(nullptr),
    lcovOutput(),
    NaNValue(DoubleNaNValue()),
    negativeInfinityValue(DoubleValue(NegativeInfinity<double>())),
    positiveInfinityValue(DoubleValue(PositiveInfinity<double>())),
    emptyString(nullptr),
    spsProfiler(thisFromCtor()),
    profilingScripts(false),
    suppressProfilerSampling(false),
    hadOutOfMemory(false),
    handlingInitFailure(false),
    haveCreatedContext(false),
    allowRelazificationForTesting(false),
    data(nullptr),
    signalHandlersInstalled_(false),
    canUseSignalHandlers_(false),
    defaultFreeOp_(thisFromCtor()),
    debuggerMutations(0),
    securityCallbacks(const_cast<JSSecurityCallbacks*>(&NullSecurityCallbacks)),
    DOMcallbacks(nullptr),
    destroyPrincipals(nullptr),
    readPrincipals(nullptr),
    errorReporter(nullptr),
    linkedAsmJSModules(nullptr),
    propertyRemovals(0),
#if !EXPOSE_INTL_API
    thousandsSeparator(0),
    decimalSeparator(0),
    numGrouping(0),
#endif
    mathCache_(nullptr),
    activeCompilations_(0),
    keepAtoms_(0),
    trustedPrincipals_(nullptr),
    beingDestroyed_(false),
    atoms_(nullptr),
    atomsCompartment_(nullptr),
    staticStrings(nullptr),
    commonNames(nullptr),
    permanentAtoms(nullptr),
    wellKnownSymbols(nullptr),
    wrapObjectCallbacks(&DefaultWrapObjectCallbacks),
    preserveWrapperCallback(nullptr),
    jitSupportsFloatingPoint(false),
    jitSupportsSimd(false),
    ionPcScriptCache(nullptr),
    scriptEnvironmentPreparer(nullptr),
    ctypesActivityCallback(nullptr),
    windowProxyClass_(nullptr),
    offthreadIonCompilationEnabled_(true),
    parallelParsingEnabled_(true),
    autoWritableJitCodeActive_(false),
#ifdef DEBUG
    enteredPolicy(nullptr),
#endif
    largeAllocationFailureCallback(nullptr),
    oomCallback(nullptr),
    debuggerMallocSizeOf(ReturnZeroSize),
    lastAnimationTime(0),
    performanceMonitoring(thisFromCtor())
{
    setGCStoreBufferPtr(&gc.storeBuffer);

    liveRuntimesCount++;

    /* Initialize infallibly first, so we can goto bad and JS_DestroyRuntime. */
    JS_INIT_CLIST(&onNewGlobalObjectWatchers);

    PodArrayZero(nativeStackQuota);
    PodZero(&asmJSCacheOps);
    lcovOutput.init();
}

static bool
SignalBasedTriggersDisabled()
{
  // Don't bother trying to cache the getenv lookup; this should be called
  // infrequently.
  return !!getenv("JS_DISABLE_SLOW_SCRIPT_SIGNALS") || !!getenv("JS_NO_SIGNALS");
}

bool
JSRuntime::init(uint32_t maxbytes, uint32_t maxNurseryBytes)
{
    ownerThread_ = PR_GetCurrentThread();

    // Get a platform-native handle for the owner thread, used by
    // js::InterruptRunningJitCode to halt the runtime's main thread.
#ifdef XP_WIN
    size_t openFlags = THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME |
                       THREAD_QUERY_INFORMATION;
    HANDLE self = OpenThread(openFlags, false, GetCurrentThreadId());
    if (!self)
        return false;
    static_assert(sizeof(HANDLE) <= sizeof(ownerThreadNative_), "need bigger field");
    ownerThreadNative_ = (size_t)self;
#else
    static_assert(sizeof(pthread_t) <= sizeof(ownerThreadNative_), "need bigger field");
    ownerThreadNative_ = (size_t)pthread_self();
#endif

    exclusiveAccessLock = PR_NewLock();
    if (!exclusiveAccessLock)
        return false;

    if (!mainThread.init())
        return false;

    if (!regexpStack.init())
        return false;

    if (CanUseExtraThreads() && !EnsureHelperThreadsInitialized())
        return false;

    js::TlsPerThreadData.set(&mainThread);

    if (!gc.init(maxbytes, maxNurseryBytes))
        return false;

    ScopedJSDeletePtr<Zone> atomsZone(new_<Zone>(this));
    if (!atomsZone || !atomsZone->init(true))
        return false;

    JS::CompartmentOptions options;
    ScopedJSDeletePtr<JSCompartment> atomsCompartment(new_<JSCompartment>(atomsZone.get(), options));
    if (!atomsCompartment || !atomsCompartment->init(nullptr))
        return false;

    if (!gc.zones.append(atomsZone.get()))
        return false;
    if (!atomsZone->compartments.append(atomsCompartment.get()))
        return false;

    atomsCompartment->setIsSystem(true);

    atomsZone.forget();
    this->atomsCompartment_ = atomsCompartment.forget();

    if (!symbolRegistry_.init())
        return false;

    if (!scriptDataTable_.init())
        return false;

    if (!evalCache.init())
        return false;

    if (!compressedSourceSet.init())
        return false;

    /* The garbage collector depends on everything before this point being initialized. */
    gcInitialized = true;

    if (!InitRuntimeNumberState(this))
        return false;

    JS::ResetTimeZone();

#ifdef JS_SIMULATOR
    simulator_ = js::jit::Simulator::Create();
    if (!simulator_)
        return false;
#endif

    jitSupportsFloatingPoint = js::jit::JitSupportsFloatingPoint();
    jitSupportsSimd = js::jit::JitSupportsSimd();

    signalHandlersInstalled_ = EnsureSignalHandlersInstalled(this);
    canUseSignalHandlers_ = signalHandlersInstalled_ && !SignalBasedTriggersDisabled();

    if (!spsProfiler.init())
        return false;

    if (!fx.initInstance())
        return false;

    return true;
}

JSRuntime::~JSRuntime()
{
    MOZ_ASSERT(!isHeapBusy());

    fx.destroyInstance();

    if (gcInitialized) {
        /* Free source hook early, as its destructor may want to delete roots. */
        sourceHook = nullptr;

        /*
         * Cancel any pending, in progress or completed Ion compilations and
         * parse tasks. Waiting for AsmJS and compression tasks is done
         * synchronously (on the main thread or during parse tasks), so no
         * explicit canceling is needed for these.
         */
        for (CompartmentsIter comp(this, SkipAtoms); !comp.done(); comp.next())
            CancelOffThreadIonCompile(comp, nullptr);
        CancelOffThreadParses(this);

        /* Clear debugging state to remove GC roots. */
        for (CompartmentsIter comp(this, SkipAtoms); !comp.done(); comp.next()) {
            if (WatchpointMap* wpmap = comp->watchpointMap)
                wpmap->clear();
        }

        /*
         * Clear script counts map, to remove the strong reference on the
         * JSScript key.
         */
        for (CompartmentsIter comp(this, SkipAtoms); !comp.done(); comp.next())
            comp->clearScriptCounts();

        /* Clear atoms to remove GC roots and heap allocations. */
        finishAtoms();

        /* Remove persistent GC roots. */
        gc.finishRoots();

        /*
         * Flag us as being destroyed. This allows the GC to free things like
         * interned atoms and Ion trampolines.
         */
        beingDestroyed_ = true;

        /* Allow the GC to release scripts that were being profiled. */
        profilingScripts = false;

        /* Set the profiler sampler buffer generation to invalid. */
        profilerSampleBufferGen_ = UINT32_MAX;

        JS::PrepareForFullGC(this);
        gc.gc(GC_NORMAL, JS::gcreason::DESTROY_RUNTIME);
    }

    /*
     * Clear the self-hosted global and delete self-hosted classes *after*
     * GC, as finalizers for objects check for clasp->finalize during GC.
     */
    finishSelfHosting();

    MOZ_ASSERT(!exclusiveAccessOwner);
    if (exclusiveAccessLock)
        PR_DestroyLock(exclusiveAccessLock);

    // Avoid bogus asserts during teardown.
    MOZ_ASSERT(!numExclusiveThreads);
    mainThreadHasExclusiveAccess = true;

    /*
     * Even though all objects in the compartment are dead, we may have keep
     * some filenames around because of gcKeepAtoms.
     */
    FreeScriptData(this);

#ifdef DEBUG
    /* Don't hurt everyone in leaky ol' Mozilla with a fatal MOZ_ASSERT! */
    if (hasContexts()) {
        unsigned cxcount = 0;
        for (ContextIter acx(this); !acx.done(); acx.next()) {
            fprintf(stderr,
"JS API usage error: found live context at %p\n",
                    (void*) acx.get());
            cxcount++;
        }
        fprintf(stderr,
"JS API usage error: %u context%s left in runtime upon JS_DestroyRuntime.\n",
                cxcount, (cxcount == 1) ? "" : "s");
    }
#endif

#if !EXPOSE_INTL_API
    FinishRuntimeNumberState(this);
#endif

    gc.finish();
    atomsCompartment_ = nullptr;

    js_free(defaultLocale);
    js_delete(mathCache_);
    js_delete(jitRuntime_);

    js_delete(ionPcScriptCache);

    gc.storeBuffer.disable();
    gc.nursery.disable();

#ifdef JS_SIMULATOR
    js::jit::Simulator::Destroy(simulator_);
#endif

    DebugOnly<size_t> oldCount = liveRuntimesCount--;
    MOZ_ASSERT(oldCount > 0);

    js::TlsPerThreadData.set(nullptr);

#ifdef XP_WIN
    if (ownerThreadNative_)
        CloseHandle((HANDLE)ownerThreadNative_);
#endif
}

void
JSRuntime::addTelemetry(int id, uint32_t sample, const char* key)
{
    if (telemetryCallback)
        (*telemetryCallback)(id, sample, key);
}

void
JSRuntime::setTelemetryCallback(JSRuntime* rt, JSAccumulateTelemetryDataCallback callback)
{
    rt->telemetryCallback = callback;
}

void
NewObjectCache::clearNurseryObjects(JSRuntime* rt)
{
    for (unsigned i = 0; i < mozilla::ArrayLength(entries); ++i) {
        Entry& e = entries[i];
        NativeObject* obj = reinterpret_cast<NativeObject*>(&e.templateObject);
        if (IsInsideNursery(e.key) ||
            rt->gc.nursery.isInside(obj->slots_) ||
            rt->gc.nursery.isInside(obj->elements_))
        {
            PodZero(&e);
        }
    }
}

void
JSRuntime::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::RuntimeSizes* rtSizes)
{
    // Several tables in the runtime enumerated below can be used off thread.
    AutoLockForExclusiveAccess lock(this);

    rtSizes->object += mallocSizeOf(this);

    rtSizes->atomsTable += atoms().sizeOfIncludingThis(mallocSizeOf);

    if (!parentRuntime) {
        rtSizes->atomsTable += mallocSizeOf(staticStrings);
        rtSizes->atomsTable += mallocSizeOf(commonNames);
        rtSizes->atomsTable += permanentAtoms->sizeOfIncludingThis(mallocSizeOf);
    }

    for (ContextIter acx(this); !acx.done(); acx.next())
        rtSizes->contexts += acx->sizeOfIncludingThis(mallocSizeOf);

    rtSizes->dtoa += mallocSizeOf(mainThread.dtoaState);

    rtSizes->temporary += tempLifoAlloc.sizeOfExcludingThis(mallocSizeOf);

    rtSizes->interpreterStack += interpreterStack_.sizeOfExcludingThis(mallocSizeOf);

    rtSizes->mathCache += mathCache_ ? mathCache_->sizeOfIncludingThis(mallocSizeOf) : 0;

    rtSizes->uncompressedSourceCache += uncompressedSourceCache.sizeOfExcludingThis(mallocSizeOf);

    rtSizes->compressedSourceSet += compressedSourceSet.sizeOfExcludingThis(mallocSizeOf);

    rtSizes->scriptData += scriptDataTable().sizeOfExcludingThis(mallocSizeOf);
    for (ScriptDataTable::Range r = scriptDataTable().all(); !r.empty(); r.popFront())
        rtSizes->scriptData += mallocSizeOf(r.front());

    if (jitRuntime_)
        jitRuntime_->execAlloc().addSizeOfCode(&rtSizes->code);

    rtSizes->gc.marker += gc.marker.sizeOfExcludingThis(mallocSizeOf);
    rtSizes->gc.nurseryCommitted += gc.nursery.sizeOfHeapCommitted();
    rtSizes->gc.nurseryDecommitted += gc.nursery.sizeOfHeapDecommitted();
    rtSizes->gc.nurseryMallocedBuffers += gc.nursery.sizeOfMallocedBuffers(mallocSizeOf);
    gc.storeBuffer.addSizeOfExcludingThis(mallocSizeOf, &rtSizes->gc);
}

static bool
InvokeInterruptCallback(JSContext* cx)
{
    MOZ_ASSERT(cx->runtime()->requestDepth >= 1);

    cx->runtime()->gc.gcIfRequested();

    // A worker thread may have requested an interrupt after finishing an Ion
    // compilation.
    jit::AttachFinishedCompilations(cx);

    // Important: Additional callbacks can occur inside the callback handler
    // if it re-enters the JS engine. The embedding must ensure that the
    // callback is disconnected before attempting such re-entry.
    JSInterruptCallback cb = cx->runtime()->interruptCallback;
    if (!cb)
        return true;

    if (cb(cx)) {
        // Debugger treats invoking the interrupt callback as a "step", so
        // invoke the onStep handler.
        if (cx->compartment()->isDebuggee()) {
            ScriptFrameIter iter(cx);
            if (iter.script()->stepModeEnabled()) {
                RootedValue rval(cx);
                switch (Debugger::onSingleStep(cx, &rval)) {
                  case JSTRAP_ERROR:
                    return false;
                  case JSTRAP_CONTINUE:
                    return true;
                  case JSTRAP_RETURN:
                    // See note in Debugger::propagateForcedReturn.
                    Debugger::propagateForcedReturn(cx, iter.abstractFramePtr(), rval);
                    return false;
                  case JSTRAP_THROW:
                    cx->setPendingException(rval);
                    return false;
                  default:;
                }
            }
        }

        return true;
    }

    // No need to set aside any pending exception here: ComputeStackString
    // already does that.
    JSString* stack = ComputeStackString(cx);
    JSFlatString* flat = stack ? stack->ensureFlat(cx) : nullptr;

    const char16_t* chars;
    AutoStableStringChars stableChars(cx);
    if (flat && stableChars.initTwoByte(cx, flat))
        chars = stableChars.twoByteRange().start().get();
    else
        chars = MOZ_UTF16("(stack not available)");
    JS_ReportErrorFlagsAndNumberUC(cx, JSREPORT_WARNING, GetErrorMessage, nullptr,
                                   JSMSG_TERMINATED, chars);

    return false;
}

void
JSRuntime::resetJitStackLimit()
{
    // Note that, for now, we use the untrusted limit for ion. This is fine,
    // because it's the most conservative limit, and if we hit it, we'll bail
    // out of ion into the interpreter, which will do a proper recursion check.
#ifdef JS_SIMULATOR
    jitStackLimit_ = jit::Simulator::StackLimit();
#else
    jitStackLimit_ = mainThread.nativeStackLimit[StackForUntrustedScript];
#endif
    jitStackLimitNoInterrupt_ = jitStackLimit_;
}

void
JSRuntime::initJitStackLimit()
{
    resetJitStackLimit();
}

void
JSRuntime::requestInterrupt(InterruptMode mode)
{
    interrupt_ = true;
    jitStackLimit_ = UINTPTR_MAX;

    if (mode == JSRuntime::RequestInterruptUrgent) {
        // If this interrupt is urgent (slow script dialog and garbage
        // collection among others), take additional steps to
        // interrupt corner cases where the above fields are not
        // regularly polled.  Wake both ilooping JIT code and
        // futexWait.
        fx.lock();
        if (fx.isWaiting())
            fx.wake(FutexRuntime::WakeForJSInterrupt);
        fx.unlock();
        InterruptRunningJitCode(this);
    }
}

bool
JSRuntime::handleInterrupt(JSContext* cx)
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
    if (interrupt_ || jitStackLimit_ == UINTPTR_MAX) {
        interrupt_ = false;
        resetJitStackLimit();
        return InvokeInterruptCallback(cx);
    }
    return true;
}

MathCache*
JSRuntime::createMathCache(JSContext* cx)
{
    MOZ_ASSERT(!mathCache_);
    MOZ_ASSERT(cx->runtime() == this);

    MathCache* newMathCache = js_new<MathCache>();
    if (!newMathCache) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    mathCache_ = newMathCache;
    return mathCache_;
}

bool
JSRuntime::setDefaultLocale(const char* locale)
{
    if (!locale)
        return false;
    resetDefaultLocale();
    defaultLocale = JS_strdup(this, locale);
    return defaultLocale != nullptr;
}

void
JSRuntime::resetDefaultLocale()
{
    js_free(defaultLocale);
    defaultLocale = nullptr;
}

const char*
JSRuntime::getDefaultLocale()
{
    if (defaultLocale)
        return defaultLocale;

    char* locale;
    char* lang;
    char* p;
#ifdef HAVE_SETLOCALE
    locale = setlocale(LC_ALL, nullptr);
#else
    locale = getenv("LANG");
#endif
    // convert to a well-formed BCP 47 language tag
    if (!locale || !strcmp(locale, "C"))
        locale = const_cast<char*>("und");
    lang = JS_strdup(this, locale);
    if (!lang)
        return nullptr;
    if ((p = strchr(lang, '.')))
        *p = '\0';
    while ((p = strchr(lang, '_')))
        *p = '-';

    defaultLocale = lang;
    return defaultLocale;
}

void
JSRuntime::triggerActivityCallback(bool active)
{
    if (!activityCallback)
        return;

    /*
     * The activity callback must not trigger a GC: it would create a cirular
     * dependency between entering a request and Rooted's requirement of being
     * in a request. In practice this callback already cannot trigger GC. The
     * suppression serves to inform the exact rooting hazard analysis of this
     * property and ensures that it remains true in the future.
     */
    AutoSuppressGC suppress(this);

    activityCallback(activityCallbackArg, active);
}

void
JSRuntime::updateMallocCounter(size_t nbytes)
{
    updateMallocCounter(nullptr, nbytes);
}

void
JSRuntime::updateMallocCounter(JS::Zone* zone, size_t nbytes)
{
    gc.updateMallocCounter(zone, nbytes);
}

JS_FRIEND_API(void*)
JSRuntime::onOutOfMemory(AllocFunction allocFunc, size_t nbytes, void* reallocPtr,
                         JSContext* maybecx)
{
    MOZ_ASSERT_IF(allocFunc != AllocFunction::Realloc, !reallocPtr);
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(this));

    if (isHeapBusy())
        return nullptr;

    if (!oom::IsSimulatedOOMAllocation()) {
        /*
         * Retry when we are done with the background sweeping and have stopped
         * all the allocations and released the empty GC chunks.
         */
        gc.onOutOfMallocMemory();
        void* p;
        switch (allocFunc) {
          case AllocFunction::Malloc:
            p = js_malloc(nbytes);
            break;
          case AllocFunction::Calloc:
            p = js_calloc(nbytes);
            break;
          case AllocFunction::Realloc:
            p = js_realloc(reallocPtr, nbytes);
            break;
          default:
            MOZ_CRASH();
        }
        if (p)
            return p;
    }

    if (maybecx)
        ReportOutOfMemory(maybecx);
    return nullptr;
}

void*
JSRuntime::onOutOfMemoryCanGC(AllocFunction allocFunc, size_t bytes, void* reallocPtr)
{
    if (largeAllocationFailureCallback && bytes >= LARGE_ALLOCATION)
        largeAllocationFailureCallback(largeAllocationFailureCallbackData);
    return onOutOfMemory(allocFunc, bytes, reallocPtr);
}

bool
JSRuntime::activeGCInAtomsZone()
{
    Zone* zone = atomsCompartment_->zone();
    return zone->needsIncrementalBarrier() || zone->isGCScheduled() || zone->wasGCStarted();
}

void
JSRuntime::setUsedByExclusiveThread(Zone* zone)
{
    MOZ_ASSERT(!zone->usedByExclusiveThread);
    zone->usedByExclusiveThread = true;
    numExclusiveThreads++;
}

void
JSRuntime::clearUsedByExclusiveThread(Zone* zone)
{
    MOZ_ASSERT(zone->usedByExclusiveThread);
    zone->usedByExclusiveThread = false;
    numExclusiveThreads--;
    if (gc.fullGCForAtomsRequested() && !keepAtoms())
        gc.triggerFullGCForAtoms();
}

bool
js::CurrentThreadCanAccessRuntime(JSRuntime* rt)
{
    return rt->ownerThread_ == PR_GetCurrentThread();
}

bool
js::CurrentThreadCanAccessZone(Zone* zone)
{
    if (CurrentThreadCanAccessRuntime(zone->runtime_))
        return true;

    // Only zones in use by an exclusive thread can be used off the main thread.
    // We don't keep track of which thread owns such zones though, so this check
    // is imperfect.
    return zone->usedByExclusiveThread;
}

#ifdef DEBUG

void
JSRuntime::assertCanLock(RuntimeLock which)
{
    // In the switch below, each case falls through to the one below it. None
    // of the runtime locks are reentrant, and when multiple locks are acquired
    // it must be done in the order below.
    switch (which) {
      case ExclusiveAccessLock:
        MOZ_ASSERT(exclusiveAccessOwner != PR_GetCurrentThread());
      case HelperThreadStateLock:
        MOZ_ASSERT(!HelperThreadState().isLocked());
      case GCLock:
        gc.assertCanLock();
        break;
      default:
        MOZ_CRASH();
    }
}

void
js::AssertCurrentThreadCanLock(RuntimeLock which)
{
    PerThreadData* pt = TlsPerThreadData.get();
    if (pt && pt->runtime_)
        pt->runtime_->assertCanLock(which);
}

#endif // DEBUG

JS_FRIEND_API(void)
JS::UpdateJSRuntimeProfilerSampleBufferGen(JSRuntime* runtime, uint32_t generation,
                                           uint32_t lapCount)
{
    runtime->setProfilerSampleBufferGen(generation);
    runtime->updateProfilerSampleBufferLapCount(lapCount);
}

JS_FRIEND_API(bool)
JS::IsProfilingEnabledForRuntime(JSRuntime* runtime)
{
    MOZ_ASSERT(runtime);
    return runtime->spsProfiler.enabled();
}
