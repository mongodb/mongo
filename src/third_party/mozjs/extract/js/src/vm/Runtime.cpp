/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/ThreadLocal.h"

#if defined(XP_DARWIN)
#  include <mach/mach.h>
#elif defined(XP_UNIX)
#  include <sys/resource.h>
#endif  // defined(XP_DARWIN) || defined(XP_UNIX) || defined(XP_WIN)
#include <locale.h>
#include <string.h>
#ifdef JS_CAN_CHECK_THREADSAFE_ACCESSES
#  include <sys/mman.h>
#endif

#include "jsfriendapi.h"
#include "jsmath.h"

#include "gc/FreeOp.h"
#include "gc/PublicIterators.h"
#include "jit/IonCompileTask.h"
#include "jit/JitRuntime.h"
#include "jit/Simulator.h"
#include "js/AllocationLogging.h"  // JS_COUNT_CTOR, JS_COUNT_DTOR
#include "js/Date.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/MemoryMetrics.h"
#include "js/SliceBudget.h"
#include "js/Wrapper.h"
#if JS_HAS_INTL_API
#  include "unicode/uloc.h"
#endif
#include "util/Windows.h"
#include "vm/DateTime.h"
#include "vm/JSAtom.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/PromiseObject.h"  // js::PromiseObject
#include "vm/TraceLogging.h"
#include "vm/TraceLoggingGraph.h"
#include "vm/Warnings.h"  // js::WarnNumberUC
#include "wasm/WasmSignalHandlers.h"

#include "debugger/DebugAPI-inl.h"
#include "gc/GC-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/Realm-inl.h"

using namespace js;

using mozilla::Atomic;
using mozilla::DebugOnly;
using mozilla::NegativeInfinity;
using mozilla::PositiveInfinity;

/* static */ MOZ_THREAD_LOCAL(JSContext*) js::TlsContext;
/* static */
Atomic<size_t> JSRuntime::liveRuntimesCount;
Atomic<JS::LargeAllocationFailureCallback> js::OnLargeAllocationFailure;

JS::FilenameValidationCallback js::gFilenameValidationCallback = nullptr;

namespace js {

#ifndef __wasi__
bool gCanUseExtraThreads = true;
#else
bool gCanUseExtraThreads = false;
#endif
}  // namespace js

void js::DisableExtraThreads() { gCanUseExtraThreads = false; }

const JSSecurityCallbacks js::NullSecurityCallbacks = {};

static const JSWrapObjectCallbacks DefaultWrapObjectCallbacks = {
    TransparentObjectWrapper, nullptr};

static size_t ReturnZeroSize(const void* p) { return 0; }

JSRuntime::JSRuntime(JSRuntime* parentRuntime)
    : parentRuntime(parentRuntime),
#ifdef DEBUG
      updateChildRuntimeCount(parentRuntime),
      initialized_(false),
#endif
      mainContext_(nullptr),
      profilerSampleBufferRangeStart_(0),
      telemetryCallback(nullptr),
      consumeStreamCallback(nullptr),
      reportStreamErrorCallback(nullptr),
      hadOutOfMemory(false),
      allowRelazificationForTesting(false),
      destroyCompartmentCallback(nullptr),
      sizeOfIncludingThisCompartmentCallback(nullptr),
      destroyRealmCallback(nullptr),
      realmNameCallback(nullptr),
      securityCallbacks(&NullSecurityCallbacks),
      DOMcallbacks(nullptr),
      destroyPrincipals(nullptr),
      readPrincipals(nullptr),
      warningReporter(nullptr),
      selfHostedLazyScript(),
      geckoProfiler_(thisFromCtor()),
      trustedPrincipals_(nullptr),
      wrapObjectCallbacks(&DefaultWrapObjectCallbacks),
      preserveWrapperCallback(nullptr),
      scriptEnvironmentPreparer(nullptr),
      ctypesActivityCallback(nullptr),
      windowProxyClass_(nullptr),
      scriptDataLock(mutexid::SharedImmutableScriptData),
#ifdef DEBUG
      activeThreadHasScriptDataAccess(false),
#endif
      numParseTasks(0),
      numActiveHelperThreadZones(0),
      numRealms(0),
      numDebuggeeRealms_(0),
      numDebuggeeRealmsObservingCoverage_(0),
      localeCallbacks(nullptr),
      defaultLocale(nullptr),
      profilingScripts(false),
      scriptAndCountsVector(nullptr),
      lcovOutput_(),
      jitRuntime_(nullptr),
      selfHostingGlobal_(nullptr),
      gc(thisFromCtor()),
      gcInitialized(false),
      emptyString(nullptr),
      defaultFreeOp_(nullptr),
#if !JS_HAS_INTL_API
      thousandsSeparator(nullptr),
      decimalSeparator(nullptr),
      numGrouping(nullptr),
#endif
      beingDestroyed_(false),
      allowContentJS_(true),
      atoms_(nullptr),
      permanentAtomsDuringInit_(nullptr),
      permanentAtoms_(nullptr),
      staticStrings(nullptr),
      commonNames(nullptr),
      wellKnownSymbols(nullptr),
      liveSABs(0),
      beforeWaitCallback(nullptr),
      afterWaitCallback(nullptr),
      offthreadIonCompilationEnabled_(true),
      parallelParsingEnabled_(true),
#ifdef DEBUG
      offThreadParsesRunning_(0),
      offThreadParsingBlocked_(false),
#endif
      autoWritableJitCodeActive_(false),
      oomCallback(nullptr),
      debuggerMallocSizeOf(ReturnZeroSize),
      stackFormat_(parentRuntime ? js::StackFormat::Default
                                 : js::StackFormat::SpiderMonkey),
      wasmInstances(mutexid::WasmRuntimeInstances),
      moduleResolveHook(),
      moduleMetadataHook(),
      moduleDynamicImportHook(),
      scriptPrivateAddRefHook(),
      scriptPrivateReleaseHook() {
  JS_COUNT_CTOR(JSRuntime);
  liveRuntimesCount++;

#ifndef __wasi__
  // See function comment for why we call this now, not in JS_Init().
  wasm::EnsureEagerProcessSignalHandlers();
#endif  // __wasi__
}

JSRuntime::~JSRuntime() {
  JS_COUNT_DTOR(JSRuntime);
  MOZ_ASSERT(!initialized_);

  DebugOnly<size_t> oldCount = liveRuntimesCount--;
  MOZ_ASSERT(oldCount > 0);

  MOZ_ASSERT(wasmInstances.lock()->empty());

  MOZ_ASSERT(offThreadParsesRunning_ == 0);
  MOZ_ASSERT(!offThreadParsingBlocked_);

  MOZ_ASSERT(numRealms == 0);
  MOZ_ASSERT(numDebuggeeRealms_ == 0);
  MOZ_ASSERT(numDebuggeeRealmsObservingCoverage_ == 0);
}

bool JSRuntime::init(JSContext* cx, uint32_t maxbytes) {
#ifdef DEBUG
  MOZ_ASSERT(!initialized_);
  initialized_ = true;
#endif

  if (CanUseExtraThreads() && !EnsureHelperThreadsInitialized()) {
    return false;
  }

  mainContext_ = cx;

  defaultFreeOp_ = cx->defaultFreeOp();

  if (!gc.init(maxbytes)) {
    return false;
  }

  UniquePtr<Zone> atomsZone = MakeUnique<Zone>(this, Zone::AtomsZone);
  if (!atomsZone || !atomsZone->init()) {
    return false;
  }

  MOZ_ASSERT(atomsZone->isAtomsZone());
  gc.atomsZone = atomsZone.release();

  // The garbage collector depends on everything before this point being
  // initialized.
  gcInitialized = true;

  if (!InitRuntimeNumberState(this)) {
    return false;
  }

  // As a hack, we clear our timezone cache every time we create a new runtime.
  // Also see the comment in JS::Realm::init().
  js::ResetTimeZoneInternal(ResetTimeZoneMode::DontResetIfOffsetUnchanged);

  if (!parentRuntime) {
    sharedImmutableStrings_ = js::SharedImmutableStringsCache::Create();
    if (!sharedImmutableStrings_) {
      return false;
    }
  }

  return true;
}

void JSRuntime::destroyRuntime() {
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy());
  MOZ_ASSERT(childRuntimeCount == 0);
  MOZ_ASSERT(initialized_);

#ifdef JS_HAS_INTL_API
  sharedIntlData.ref().destroyInstance();
#endif

  if (gcInitialized) {
    /*
     * Finish any in-progress GCs first. This ensures the parseWaitingOnGC
     * list is empty in CancelOffThreadParses.
     */
    JSContext* cx = mainContextFromOwnThread();
    if (JS::IsIncrementalGCInProgress(cx)) {
      gc::FinishGC(cx);
    }

    /* Free source hook early, as its destructor may want to delete roots. */
    sourceHook = nullptr;

    /*
     * Cancel any pending, in progress or completed Ion compilations and
     * parse tasks. Waiting for wasm and compression tasks is done
     * synchronously (on the main thread or during parse tasks), so no
     * explicit canceling is needed for these.
     */
    CancelOffThreadIonCompile(this);
    CancelOffThreadParses(this);
    CancelOffThreadCompressions(this);

    /*
     * Flag us as being destroyed. This allows the GC to free things like
     * interned atoms and Ion trampolines.
     */
    beingDestroyed_ = true;

    /* Remove persistent GC roots. */
    gc.finishRoots();

    /* Allow the GC to release scripts that were being profiled. */
    profilingScripts = false;

    JS::PrepareForFullGC(cx);
    gc.gc(JS::GCOptions::Normal, JS::GCReason::DESTROY_RUNTIME);
  }

  AutoNoteSingleThreadedRegion anstr;

  MOZ_ASSERT(!hasHelperThreadZones());

#ifdef DEBUG
  {
    AutoLockScriptData lock(this);
    MOZ_ASSERT(scriptDataTable(lock).empty());
  }
#endif

#if !JS_HAS_INTL_API
  FinishRuntimeNumberState(this);
#endif

  gc.finish();

  defaultLocale = nullptr;
  js_delete(jitRuntime_.ref());

#ifdef DEBUG
  initialized_ = false;
#endif
}

void JSRuntime::addTelemetry(int id, uint32_t sample, const char* key) {
  if (telemetryCallback) {
    (*telemetryCallback)(id, sample, key);
  }
}

JSTelemetrySender JSRuntime::getTelemetrySender() const {
  return JSTelemetrySender(telemetryCallback);
}

void JSRuntime::setTelemetryCallback(
    JSRuntime* rt, JSAccumulateTelemetryDataCallback callback) {
  rt->telemetryCallback = callback;
}

void JSRuntime::setSourceElementCallback(JSRuntime* rt,
                                         JSSourceElementCallback callback) {
  rt->sourceElementCallback = callback;
}

void JSRuntime::setUseCounter(JSObject* obj, JSUseCounter counter) {
  if (useCounterCallback) {
    (*useCounterCallback)(obj, counter);
  }
}

void JSRuntime::setUseCounterCallback(JSRuntime* rt,
                                      JSSetUseCounterCallback callback) {
  rt->useCounterCallback = callback;
}

void JSRuntime::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                       JS::RuntimeSizes* rtSizes) {
  rtSizes->object += mallocSizeOf(this);

  rtSizes->atomsTable += atoms().sizeOfIncludingThis(mallocSizeOf);
  rtSizes->gc.marker += gc.marker.sizeOfExcludingThis(mallocSizeOf);

  if (!parentRuntime) {
    rtSizes->atomsTable += mallocSizeOf(staticStrings);
    rtSizes->atomsTable += mallocSizeOf(commonNames);
    rtSizes->atomsTable += permanentAtoms()->sizeOfIncludingThis(mallocSizeOf);
    rtSizes->atomsTable +=
        commonParserNames.ref()->sizeOfIncludingThis(mallocSizeOf);
  }

  JSContext* cx = mainContextFromAnyThread();
  rtSizes->contexts += cx->sizeOfIncludingThis(mallocSizeOf);
  rtSizes->temporary += cx->tempLifoAlloc().sizeOfExcludingThis(mallocSizeOf);
  rtSizes->interpreterStack +=
      cx->interpreterStack().sizeOfExcludingThis(mallocSizeOf);
#ifdef JS_TRACE_LOGGING
  if (cx->traceLogger) {
    rtSizes->tracelogger += cx->traceLogger->sizeOfIncludingThis(mallocSizeOf);
  }
#endif

  rtSizes->uncompressedSourceCache +=
      caches().uncompressedSourceCache.sizeOfExcludingThis(mallocSizeOf);

  rtSizes->gc.nurseryCommitted += gc.nursery().committed();
  rtSizes->gc.nurseryMallocedBuffers +=
      gc.nursery().sizeOfMallocedBuffers(mallocSizeOf);
  gc.storeBuffer().addSizeOfExcludingThis(mallocSizeOf, &rtSizes->gc);

  if (sharedImmutableStrings_) {
    rtSizes->sharedImmutableStringsCache +=
        sharedImmutableStrings_->sizeOfExcludingThis(mallocSizeOf);
  }

#ifdef JS_HAS_INTL_API
  rtSizes->sharedIntlData +=
      sharedIntlData.ref().sizeOfExcludingThis(mallocSizeOf);
#endif

  {
    AutoLockScriptData lock(this);
    rtSizes->scriptData +=
        scriptDataTable(lock).shallowSizeOfExcludingThis(mallocSizeOf);
    for (SharedImmutableScriptDataTable::Range r = scriptDataTable(lock).all();
         !r.empty(); r.popFront()) {
      rtSizes->scriptData += r.front()->sizeOfIncludingThis(mallocSizeOf);
    }
  }

  if (jitRuntime_) {
    // Sizes of the IonCompileTasks we are holding for lazy linking
    for (auto* task : jitRuntime_->ionLazyLinkList(this)) {
      rtSizes->jitLazyLink += task->sizeOfExcludingThis(mallocSizeOf);
    }
  }

  rtSizes->wasmRuntime +=
      wasmInstances.lock()->sizeOfExcludingThis(mallocSizeOf);
}

static bool HandleInterrupt(JSContext* cx, bool invokeCallback) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  cx->runtime()->gc.gcIfRequested();

  // A worker thread may have requested an interrupt after finishing an Ion
  // compilation.
  jit::AttachFinishedCompilations(cx);

  // Don't call the interrupt callback if we only interrupted for GC or Ion.
  if (!invokeCallback) {
    return true;
  }

  // Important: Additional callbacks can occur inside the callback handler
  // if it re-enters the JS engine. The embedding must ensure that the
  // callback is disconnected before attempting such re-entry.
  if (cx->interruptCallbackDisabled) {
    return true;
  }

  bool stop = false;
  for (JSInterruptCallback cb : cx->interruptCallbacks()) {
    if (!cb(cx)) {
      stop = true;
    }
  }

  if (!stop) {
    // Debugger treats invoking the interrupt callback as a "step", so
    // invoke the onStep handler.
    if (cx->realm()->isDebuggee()) {
      ScriptFrameIter iter(cx);
      if (!iter.done() && cx->compartment() == iter.compartment() &&
          DebugAPI::stepModeEnabled(iter.script())) {
        if (!DebugAPI::onSingleStep(cx)) {
          return false;
        }
      }
    }

    return true;
  }

  // No need to set aside any pending exception here: ComputeStackString
  // already does that.
  JSString* stack = ComputeStackString(cx);

  UniqueTwoByteChars stringChars;
  if (stack) {
    stringChars = JS_CopyStringCharsZ(cx, stack);
    if (!stringChars) {
      cx->recoverFromOutOfMemory();
    }
  }

  const char16_t* chars;
  if (stringChars) {
    chars = stringChars.get();
  } else {
    chars = u"(stack not available)";
  }
  WarnNumberUC(cx, JSMSG_TERMINATED, chars);
  return false;
}

void JSContext::requestInterrupt(InterruptReason reason) {
  interruptBits_ |= uint32_t(reason);
  jitStackLimit = UINTPTR_MAX;

  if (reason == InterruptReason::CallbackUrgent) {
    // If this interrupt is urgent (slow script dialog for instance), take
    // additional steps to interrupt corner cases where the above fields are
    // not regularly polled.
    FutexThread::lock();
    if (fx.isWaiting()) {
      fx.notify(FutexThread::NotifyForJSInterrupt);
    }
    fx.unlock();
    wasm::InterruptRunningCode(this);
  }
}

bool JSContext::handleInterrupt() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));
  if (hasAnyPendingInterrupt() || jitStackLimit == UINTPTR_MAX) {
    bool invokeCallback =
        hasPendingInterrupt(InterruptReason::CallbackUrgent) ||
        hasPendingInterrupt(InterruptReason::CallbackCanWait);
    interruptBits_ = 0;
    resetJitStackLimit();
    return HandleInterrupt(this, invokeCallback);
  }
  return true;
}

bool JSRuntime::setDefaultLocale(const char* locale) {
  if (!locale) {
    return false;
  }

  UniqueChars newLocale = DuplicateString(mainContextFromOwnThread(), locale);
  if (!newLocale) {
    return false;
  }

  defaultLocale.ref() = std::move(newLocale);
  return true;
}

void JSRuntime::resetDefaultLocale() { defaultLocale = nullptr; }

const char* JSRuntime::getDefaultLocale() {
  if (defaultLocale.ref()) {
    return defaultLocale.ref().get();
  }

  // Use ICU if available to retrieve the default locale, this ensures ICU's
  // default locale matches our default locale.
#if JS_HAS_INTL_API
  const char* locale = uloc_getDefault();
#else
  const char* locale = setlocale(LC_ALL, nullptr);
#endif

  // convert to a well-formed BCP 47 language tag
  if (!locale || !strcmp(locale, "C")) {
    locale = "und";
  }

  UniqueChars lang = DuplicateString(mainContextFromOwnThread(), locale);
  if (!lang) {
    return nullptr;
  }

  char* p;
  if ((p = strchr(lang.get(), '.'))) {
    *p = '\0';
  }
  while ((p = strchr(lang.get(), '_'))) {
    *p = '-';
  }

  defaultLocale.ref() = std::move(lang);
  return defaultLocale.ref().get();
}

#ifdef JS_HAS_INTL_API
void JSRuntime::traceSharedIntlData(JSTracer* trc) {
  sharedIntlData.ref().trace(trc);
}
#endif

JSFreeOp::JSFreeOp(JSRuntime* maybeRuntime, bool isDefault)
    : runtime_(maybeRuntime), isDefault(isDefault), isCollecting_(!isDefault) {
  MOZ_ASSERT_IF(maybeRuntime, CurrentThreadCanAccessRuntime(maybeRuntime));
}

JSFreeOp::~JSFreeOp() {
  if (!jitPoisonRanges.empty()) {
    jit::ExecutableAllocator::poisonCode(runtime(), jitPoisonRanges);
  }
}

GlobalObject* JSRuntime::getIncumbentGlobal(JSContext* cx) {
  MOZ_ASSERT(cx->jobQueue);

  JSObject* obj = cx->jobQueue->getIncumbentGlobal(cx);
  if (!obj) {
    return nullptr;
  }

  MOZ_ASSERT(obj->is<GlobalObject>(),
             "getIncumbentGlobalCallback must return a global!");
  return &obj->as<GlobalObject>();
}

bool JSRuntime::enqueuePromiseJob(JSContext* cx, HandleFunction job,
                                  HandleObject promise,
                                  Handle<GlobalObject*> incumbentGlobal) {
  MOZ_ASSERT(cx->jobQueue,
             "Must select a JobQueue implementation using JS::JobQueue "
             "or js::UseInternalJobQueues before using Promises");

  RootedObject allocationSite(cx);
  if (promise) {
#ifdef DEBUG
    AssertSameCompartment(job, promise);
#endif

    RootedObject unwrappedPromise(cx, promise);
    // While the job object is guaranteed to be unwrapped, the promise
    // might be wrapped. See the comments in EnqueuePromiseReactionJob in
    // builtin/Promise.cpp for details.
    if (IsWrapper(promise)) {
      unwrappedPromise = UncheckedUnwrap(promise);
    }
    if (unwrappedPromise->is<PromiseObject>()) {
      allocationSite = JS::GetPromiseAllocationSite(unwrappedPromise);
    }
  }
  return cx->jobQueue->enqueuePromiseJob(cx, promise, job, allocationSite,
                                         incumbentGlobal);
}

void JSRuntime::addUnhandledRejectedPromise(JSContext* cx,
                                            js::HandleObject promise) {
  MOZ_ASSERT(promise->is<PromiseObject>());
  if (!cx->promiseRejectionTrackerCallback) {
    return;
  }

  bool mutedErrors = false;
  if (JSScript* script = cx->currentScript()) {
    mutedErrors = script->mutedErrors();
  }

  void* data = cx->promiseRejectionTrackerCallbackData;
  cx->promiseRejectionTrackerCallback(
      cx, mutedErrors, promise, JS::PromiseRejectionHandlingState::Unhandled,
      data);
}

void JSRuntime::removeUnhandledRejectedPromise(JSContext* cx,
                                               js::HandleObject promise) {
  MOZ_ASSERT(promise->is<PromiseObject>());
  if (!cx->promiseRejectionTrackerCallback) {
    return;
  }

  bool mutedErrors = false;
  if (JSScript* script = cx->currentScript()) {
    mutedErrors = script->mutedErrors();
  }

  void* data = cx->promiseRejectionTrackerCallbackData;
  cx->promiseRejectionTrackerCallback(
      cx, mutedErrors, promise, JS::PromiseRejectionHandlingState::Handled,
      data);
}

mozilla::non_crypto::XorShift128PlusRNG& JSRuntime::randomKeyGenerator() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(this));
  if (randomKeyGenerator_.isNothing()) {
    mozilla::Array<uint64_t, 2> seed;
    GenerateXorShift128PlusSeed(seed);
    randomKeyGenerator_.emplace(seed[0], seed[1]);
  }
  return randomKeyGenerator_.ref();
}

mozilla::HashCodeScrambler JSRuntime::randomHashCodeScrambler() {
  auto& rng = randomKeyGenerator();
  return mozilla::HashCodeScrambler(rng.next(), rng.next());
}

mozilla::non_crypto::XorShift128PlusRNG JSRuntime::forkRandomKeyGenerator() {
  auto& rng = randomKeyGenerator();
  return mozilla::non_crypto::XorShift128PlusRNG(rng.next(), rng.next());
}

js::HashNumber JSRuntime::randomHashCode() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(this));

  if (randomHashCodeGenerator_.isNothing()) {
    mozilla::Array<uint64_t, 2> seed;
    GenerateXorShift128PlusSeed(seed);
    randomHashCodeGenerator_.emplace(seed[0], seed[1]);
  }

  return HashNumber(randomHashCodeGenerator_->next());
}

JS_PUBLIC_API void* JSRuntime::onOutOfMemory(AllocFunction allocFunc,
                                             arena_id_t arena, size_t nbytes,
                                             void* reallocPtr,
                                             JSContext* maybecx) {
  MOZ_ASSERT_IF(allocFunc != AllocFunction::Realloc, !reallocPtr);

  if (JS::RuntimeHeapIsBusy()) {
    return nullptr;
  }

  if (!oom::IsSimulatedOOMAllocation()) {
    /*
     * Retry when we are done with the background sweeping and have stopped
     * all the allocations and released the empty GC chunks.
     */
    gc.onOutOfMallocMemory();
    void* p;
    switch (allocFunc) {
      case AllocFunction::Malloc:
        p = js_arena_malloc(arena, nbytes);
        break;
      case AllocFunction::Calloc:
        p = js_arena_calloc(arena, nbytes, 1);
        break;
      case AllocFunction::Realloc:
        p = js_arena_realloc(arena, reallocPtr, nbytes);
        break;
      default:
        MOZ_CRASH();
    }
    if (p) {
      return p;
    }
  }

  if (maybecx) {
    ReportOutOfMemory(maybecx);
  }
  return nullptr;
}

void* JSRuntime::onOutOfMemoryCanGC(AllocFunction allocFunc, arena_id_t arena,
                                    size_t bytes, void* reallocPtr) {
  if (OnLargeAllocationFailure && bytes >= LARGE_ALLOCATION) {
    OnLargeAllocationFailure();
  }
  return onOutOfMemory(allocFunc, arena, bytes, reallocPtr);
}

bool JSRuntime::activeGCInAtomsZone() {
  Zone* zone = unsafeAtomsZone();
  return (zone->needsIncrementalBarrier() &&
          !gc.isVerifyPreBarriersEnabled()) ||
         zone->wasGCStarted();
}

void JSRuntime::setUsedByHelperThread(Zone* zone) {
  MOZ_ASSERT(!zone->usedByHelperThread());
  MOZ_ASSERT(!zone->wasGCStarted());
  MOZ_ASSERT(!isOffThreadParsingBlocked());

  zone->setUsedByHelperThread();
  if (numActiveHelperThreadZones++ == 0) {
    gc.setParallelAtomsAllocEnabled(true);
  }
}

void JSRuntime::clearUsedByHelperThread(Zone* zone) {
  MOZ_ASSERT(zone->usedByHelperThread());

  zone->clearUsedByHelperThread();
  if (--numActiveHelperThreadZones == 0) {
    gc.setParallelAtomsAllocEnabled(false);
  }

  JSContext* cx = mainContextFromOwnThread();
  if (gc.fullGCForAtomsRequested() && cx->canCollectAtoms()) {
    gc.triggerFullGCForAtoms(cx);
  }
}

void JSRuntime::incrementNumDebuggeeRealms() {
  if (numDebuggeeRealms_ == 0) {
    jitRuntime()->baselineInterpreter().toggleDebuggerInstrumentation(true);
  }

  numDebuggeeRealms_++;
  MOZ_ASSERT(numDebuggeeRealms_ <= numRealms);
}

void JSRuntime::decrementNumDebuggeeRealms() {
  MOZ_ASSERT(numDebuggeeRealms_ > 0);
  numDebuggeeRealms_--;

  // Note: if we had shutdown leaks we can end up here while destroying the
  // runtime. It's not safe to access JitRuntime trampolines because they're no
  // longer traced.
  if (numDebuggeeRealms_ == 0 && !isBeingDestroyed()) {
    jitRuntime()->baselineInterpreter().toggleDebuggerInstrumentation(false);
  }
}

void JSRuntime::incrementNumDebuggeeRealmsObservingCoverage() {
  if (numDebuggeeRealmsObservingCoverage_ == 0) {
    jit::BaselineInterpreter& interp = jitRuntime()->baselineInterpreter();
    interp.toggleCodeCoverageInstrumentation(true);
  }

  numDebuggeeRealmsObservingCoverage_++;
  MOZ_ASSERT(numDebuggeeRealmsObservingCoverage_ <= numRealms);
}

void JSRuntime::decrementNumDebuggeeRealmsObservingCoverage() {
  MOZ_ASSERT(numDebuggeeRealmsObservingCoverage_ > 0);
  numDebuggeeRealmsObservingCoverage_--;

  // Note: if we had shutdown leaks we can end up here while destroying the
  // runtime. It's not safe to access JitRuntime trampolines because they're no
  // longer traced.
  if (numDebuggeeRealmsObservingCoverage_ == 0 && !isBeingDestroyed()) {
    jit::BaselineInterpreter& interp = jitRuntime()->baselineInterpreter();
    interp.toggleCodeCoverageInstrumentation(false);
  }
}

bool js::CurrentThreadCanAccessRuntime(const JSRuntime* rt) {
  return rt->mainContextFromAnyThread() == TlsContext.get();
}

bool js::CurrentThreadCanAccessZone(Zone* zone) {
  // Helper thread zones can only be used by their owning thread.
  if (zone->usedByHelperThread()) {
    return zone->ownedByCurrentHelperThread();
  }

  // Other zones can only be accessed by the runtime's active context.
  return CurrentThreadCanAccessRuntime(zone->runtime_);
}

#ifdef DEBUG
bool js::CurrentThreadIsPerformingGC() {
  JSContext* cx = TlsContext.get();
  return cx->defaultFreeOp()->isCollecting();
}
#endif

JS_PUBLIC_API void JS::SetJSContextProfilerSampleBufferRangeStart(
    JSContext* cx, uint64_t rangeStart) {
  cx->runtime()->setProfilerSampleBufferRangeStart(rangeStart);
}

JS_PUBLIC_API bool JS::IsProfilingEnabledForContext(JSContext* cx) {
  MOZ_ASSERT(cx);
  return cx->runtime()->geckoProfiler().enabled();
}

JS_PUBLIC_API void JS::EnableRecordingAllocations(
    JSContext* cx, JS::RecordAllocationsCallback callback, double probability) {
  MOZ_ASSERT(cx);
  MOZ_ASSERT(cx->isMainThreadContext());
  cx->runtime()->startRecordingAllocations(probability, callback);
}

JS_PUBLIC_API void JS::DisableRecordingAllocations(JSContext* cx) {
  MOZ_ASSERT(cx);
  MOZ_ASSERT(cx->isMainThreadContext());
  cx->runtime()->stopRecordingAllocations();
}

JS_PUBLIC_API void JS::shadow::RegisterWeakCache(
    JSRuntime* rt, detail::WeakCacheBase* cachep) {
  rt->registerWeakCache(cachep);
}

void JSRuntime::startRecordingAllocations(
    double probability, JS::RecordAllocationsCallback callback) {
  allocationSamplingProbability = probability;
  recordAllocationCallback = callback;

  // Go through all of the existing realms, and turn on allocation tracking.
  for (RealmsIter realm(this); !realm.done(); realm.next()) {
    realm->setAllocationMetadataBuilder(&SavedStacks::metadataBuilder);
    realm->chooseAllocationSamplingProbability();
  }
}

void JSRuntime::stopRecordingAllocations() {
  recordAllocationCallback = nullptr;
  // Go through all of the existing realms, and turn on allocation tracking.
  for (RealmsIter realm(this); !realm.done(); realm.next()) {
    js::GlobalObject* global = realm->maybeGlobal();
    if (!realm->isDebuggee() || !global ||
        !DebugAPI::isObservedByDebuggerTrackingAllocations(*global)) {
      // Only remove the allocation metadata builder if no Debuggers are
      // tracking allocations.
      realm->forgetAllocationMetadataBuilder();
    }
  }
}

// This function can run to ensure that when new realms are created
// they have allocation logging turned on.
void JSRuntime::ensureRealmIsRecordingAllocations(
    Handle<GlobalObject*> global) {
  if (recordAllocationCallback) {
    if (!global->realm()->isRecordingAllocations()) {
      // This is a new realm, turn on allocations for it.
      global->realm()->setAllocationMetadataBuilder(
          &SavedStacks::metadataBuilder);
    }
    // Ensure the probability is up to date with the current combination of
    // debuggers and runtime profiling.
    global->realm()->chooseAllocationSamplingProbability();
  }
}
