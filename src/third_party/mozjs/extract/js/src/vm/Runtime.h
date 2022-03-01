/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Runtime_h
#define vm_Runtime_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/DoublyLinkedList.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"
#include "mozilla/MaybeOneOf.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/ThreadLocal.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Vector.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <algorithm>

#include "jsapi.h"

#include "builtin/AtomicsObject.h"
#ifdef JS_HAS_INTL_API
#  include "builtin/intl/SharedIntlData.h"
#endif
#include "frontend/NameCollections.h"
#include "gc/GCRuntime.h"
#include "gc/Tracer.h"
#include "js/AllocationRecording.h"
#include "js/BuildId.h"  // JS::BuildIdOp
#include "js/Debug.h"
#include "js/experimental/CTypes.h"      // JS::CTypesActivityCallback
#include "js/experimental/SourceHook.h"  // js::SourceHook
#include "js/friend/StackLimits.h"       // js::ReportOverRecursed
#include "js/friend/UsageStatistics.h"   // JSAccumulateTelemetryDataCallback
#include "js/GCVector.h"
#include "js/HashTable.h"
#include "js/Initialization.h"
#include "js/Modules.h"  // JS::Module{DynamicImport,Metadata,Resolve}Hook
#ifdef DEBUG
#  include "js/Proxy.h"  // For AutoEnterPolicy
#endif
#include "js/Stream.h"  // JS::AbortSignalIsAborted
#include "js/Symbol.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "js/Warnings.h"  // JS::WarningReporter
#include "threading/Thread.h"
#include "vm/Caches.h"
#include "vm/CodeCoverage.h"
#include "vm/CommonPropertyNames.h"
#include "vm/GeckoProfiler.h"
#include "vm/JSAtom.h"
#include "vm/JSAtomState.h"
#include "vm/JSScript.h"
#include "vm/OffThreadPromiseRuntimeState.h"  // js::OffThreadPromiseRuntimeState
#include "vm/Scope.h"
#include "vm/SharedImmutableStringsCache.h"
#include "vm/SharedStencil.h"  // js::SharedImmutableScriptDataTable
#include "vm/Stack.h"
#include "vm/SymbolType.h"
#include "wasm/WasmTypes.h"

struct JSClass;

namespace js {

class AutoAssertNoContentJS;
class EnterDebuggeeNoExecute;
#ifdef JS_TRACE_LOGGING
class TraceLoggerThread;
#endif

}  // namespace js

struct DtoaState;
struct JSLocaleCallbacks;

#ifdef JS_SIMULATOR_ARM64
namespace vixl {
class Simulator;
}
#endif

namespace js {

extern MOZ_COLD void ReportOutOfMemory(JSContext* cx);

/* Different signature because the return type has [[nodiscard]]_TYPE. */
extern MOZ_COLD mozilla::GenericErrorResult<OOM> ReportOutOfMemoryResult(
    JSContext* cx);

extern MOZ_COLD void ReportAllocationOverflow(JSContext* maybecx);

class Activation;
class ActivationIterator;

namespace jit {
class JitRuntime;
class JitActivation;
struct PcScriptCache;
class CompileRuntime;

#ifdef JS_SIMULATOR_ARM64
typedef vixl::Simulator Simulator;
#elif defined(JS_SIMULATOR)
class Simulator;
#endif
}  // namespace jit

namespace frontend {
class WellKnownParserAtoms;
}  // namespace frontend

// [SMDOC] JS Engine Threading
//
// Threads interacting with a runtime are divided into two categories:
//
// - The main thread is capable of running JS. There's at most one main thread
//   per runtime.
//
// - Helper threads do not run JS, and are controlled or triggered by activity
//   on the main thread (or main threads, since all runtimes in a process share
//   helper threads). Helper threads may have exclusive access to zones created
//   for them, for parsing and similar tasks, but their activities do not cause
//   observable changes in script behaviors. Activity on helper threads may be
//   referred to as happening 'off thread' or on a background thread in some
//   parts of the VM.

} /* namespace js */

namespace JS {
struct RuntimeSizes;
}  // namespace JS

namespace js {

/*
 * Storage for well-known symbols. It's a separate struct from the Runtime so
 * that it can be shared across multiple runtimes. As in JSAtomState, each
 * field is a smart pointer that's immutable once initialized.
 * `rt->wellKnownSymbols->iterator` is convertible to Handle<Symbol*>.
 *
 * Well-known symbols are never GC'd. The description() of each well-known
 * symbol is a permanent atom.
 */
struct WellKnownSymbols {
#define DECLARE_SYMBOL(name) js::ImmutableSymbolPtr name;
  JS_FOR_EACH_WELL_KNOWN_SYMBOL(DECLARE_SYMBOL)
#undef DECLARE_SYMBOL

  const ImmutableSymbolPtr& get(size_t u) const {
    MOZ_ASSERT(u < JS::WellKnownSymbolLimit);
    const ImmutableSymbolPtr* symbols =
        reinterpret_cast<const ImmutableSymbolPtr*>(this);
    return symbols[u];
  }

  const ImmutableSymbolPtr& get(JS::SymbolCode code) const {
    return get(size_t(code));
  }

  WellKnownSymbols() = default;
  WellKnownSymbols(const WellKnownSymbols&) = delete;
  WellKnownSymbols& operator=(const WellKnownSymbols&) = delete;
};

// There are several coarse locks in the enum below. These may be either
// per-runtime or per-process. When acquiring more than one of these locks,
// the acquisition must be done in the order below to avoid deadlocks.
enum RuntimeLock { HelperThreadStateLock, GCLock };

inline bool CanUseExtraThreads() {
  extern bool gCanUseExtraThreads;
  return gCanUseExtraThreads;
}

void DisableExtraThreads();

using ScriptAndCountsVector = GCVector<ScriptAndCounts, 0, SystemAllocPolicy>;

class AutoLockScriptData;

// Self-hosted lazy functions do not maintain a BaseScript as we can clone from
// the copy in the self-hosting zone. To allow these functions to be called by
// the JITs, we need a minimal script object. There is one instance per runtime.
struct SelfHostedLazyScript {
  SelfHostedLazyScript() = default;

  // Pointer to interpreter trampoline. This field is stored at same location as
  // in BaseScript::jitCodeRaw_.
  uint8_t* jitCodeRaw_ = nullptr;

  // Warm-up count of zero. This field is stored at the same offset as
  // BaseScript::warmUpData_.
  ScriptWarmUpData warmUpData_ = {};

  static constexpr size_t offsetOfJitCodeRaw() {
    return offsetof(SelfHostedLazyScript, jitCodeRaw_);
  }
  static constexpr size_t offsetOfWarmUpData() {
    return offsetof(SelfHostedLazyScript, warmUpData_);
  }
};

}  // namespace js

struct JSTelemetrySender;

struct JSRuntime {
 private:
  friend class js::Activation;
  friend class js::ActivationIterator;
  friend class js::jit::JitActivation;
  friend class js::jit::CompileRuntime;

  /* Space for interpreter frames. */
  js::MainThreadData<js::InterpreterStack> interpreterStack_;

 public:
  js::InterpreterStack& interpreterStack() { return interpreterStack_.ref(); }

  /*
   * If non-null, another runtime guaranteed to outlive this one and whose
   * permanent data may be used by this one where possible.
   */
  JSRuntime* const parentRuntime;

  bool isMainRuntime() const { return !parentRuntime; }

#ifdef DEBUG
  /* The number of child runtimes that have this runtime as their parent. */
  mozilla::Atomic<size_t> childRuntimeCount;

  class AutoUpdateChildRuntimeCount {
    JSRuntime* parent_;

   public:
    explicit AutoUpdateChildRuntimeCount(JSRuntime* parent) : parent_(parent) {
      if (parent_) {
        parent_->childRuntimeCount++;
      }
    }

    ~AutoUpdateChildRuntimeCount() {
      if (parent_) {
        parent_->childRuntimeCount--;
      }
    }
  };

  AutoUpdateChildRuntimeCount updateChildRuntimeCount;
#endif

 private:
#ifdef DEBUG
  js::WriteOnceData<bool> initialized_;
#endif

  // The JSContext* for the runtime's main thread. Immutable after this is set
  // in JSRuntime::init.
  JSContext* mainContext_;

 public:
  JSContext* mainContextFromAnyThread() const { return mainContext_; }
  const void* addressOfMainContext() { return &mainContext_; }
  js::Fprinter parserWatcherFile;

  inline JSContext* mainContextFromOwnThread();

  /*
   * The start of the range stored in the profiler sample buffer, as measured
   * after the most recent sample.
   * All JitcodeGlobalTable entries referenced from a given sample are
   * assigned the buffer position of the START of the sample. The buffer
   * entries that reference the JitcodeGlobalTable entries will only ever be
   * read from the buffer while the entire sample is still inside the buffer;
   * if some buffer entries at the start of the sample have left the buffer,
   * the entire sample will be considered inaccessible.
   * This means that, once profilerSampleBufferRangeStart_ advances beyond
   * the sample position that's stored on a JitcodeGlobalTable entry, the
   * buffer entries that reference this JitcodeGlobalTable entry will be
   * considered inaccessible, and those JitcodeGlobalTable entry can be
   * disposed of.
   */
  mozilla::Atomic<uint64_t, mozilla::ReleaseAcquire>
      profilerSampleBufferRangeStart_;

  mozilla::Maybe<uint64_t> profilerSampleBufferRangeStart() {
    if (beingDestroyed_ || !geckoProfiler().enabled()) {
      return mozilla::Nothing();
    }
    uint64_t rangeStart = profilerSampleBufferRangeStart_;
    return mozilla::Some(rangeStart);
  }
  void setProfilerSampleBufferRangeStart(uint64_t rangeStart) {
    profilerSampleBufferRangeStart_ = rangeStart;
  }

  /* Call this to accumulate telemetry data. May be called from any thread; the
   * embedder is responsible for locking. */
  JSAccumulateTelemetryDataCallback telemetryCallback;

  /* Call this to accumulate use counter data. */
  js::MainThreadData<JSSetUseCounterCallback> useCounterCallback;

  js::MainThreadData<JSSourceElementCallback> sourceElementCallback;

 public:
  // Accumulates data for Firefox telemetry. |id| is the ID of a JS_TELEMETRY_*
  // histogram. |key| provides an additional key to identify the histogram.
  // |sample| is the data to add to the histogram.
  void addTelemetry(int id, uint32_t sample, const char* key = nullptr);

  JSTelemetrySender getTelemetrySender() const;

  void setTelemetryCallback(JSRuntime* rt,
                            JSAccumulateTelemetryDataCallback callback);

  void setSourceElementCallback(JSRuntime* rt,
                                JSSourceElementCallback callback);

  // Sets the use counter for a specific feature, measuring the presence or
  // absence of usage of a feature on a specific web page and document which
  // the passed JSObject belongs to.
  void setUseCounter(JSObject* obj, JSUseCounter counter);

  void setUseCounterCallback(JSRuntime* rt, JSSetUseCounterCallback callback);

 public:
  js::UnprotectedData<js::OffThreadPromiseRuntimeState> offThreadPromiseState;
  js::UnprotectedData<JS::ConsumeStreamCallback> consumeStreamCallback;
  js::UnprotectedData<JS::ReportStreamErrorCallback> reportStreamErrorCallback;

  js::GlobalObject* getIncumbentGlobal(JSContext* cx);
  bool enqueuePromiseJob(JSContext* cx, js::HandleFunction job,
                         js::HandleObject promise,
                         js::Handle<js::GlobalObject*> incumbentGlobal);
  void addUnhandledRejectedPromise(JSContext* cx, js::HandleObject promise);
  void removeUnhandledRejectedPromise(JSContext* cx, js::HandleObject promise);

  /* Had an out-of-memory error which did not populate an exception. */
  mozilla::Atomic<bool, mozilla::SequentiallyConsistent> hadOutOfMemory;

  /*
   * Allow relazifying functions in compartments that are active. This is
   * only used by the relazifyFunctions() testing function.
   */
  js::MainThreadData<bool> allowRelazificationForTesting;

  /* Zone destroy callback. */
  js::MainThreadData<JSDestroyZoneCallback> destroyZoneCallback;

  /* Compartment destroy callback. */
  js::MainThreadData<JSDestroyCompartmentCallback> destroyCompartmentCallback;

  /* Compartment memory reporting callback. */
  js::MainThreadData<JSSizeOfIncludingThisCompartmentCallback>
      sizeOfIncludingThisCompartmentCallback;

  /* Callback for creating ubi::Nodes representing DOM node objects. Set by
   * JS::ubi::SetConstructUbiNodeForDOMObjectCallback. Refer to
   * js/public/UbiNode.h.
   */
  void (*constructUbiNodeForDOMObjectCallback)(void*, JSObject*) = nullptr;

  /* Realm destroy callback. */
  js::MainThreadData<JS::DestroyRealmCallback> destroyRealmCallback;

  /* Call this to get the name of a realm. */
  js::MainThreadData<JS::RealmNameCallback> realmNameCallback;

  js::MainThreadData<mozilla::UniquePtr<js::SourceHook>> sourceHook;

  js::MainThreadData<const JSSecurityCallbacks*> securityCallbacks;
  js::MainThreadData<const js::DOMCallbacks*> DOMcallbacks;
  js::MainThreadData<JSDestroyPrincipalsOp> destroyPrincipals;
  js::MainThreadData<JSReadPrincipalsOp> readPrincipals;

  /* Optional warning reporter. */
  js::MainThreadData<JS::WarningReporter> warningReporter;

  // Lazy self-hosted functions use a shared SelfHostedLazyScript instance
  // instead instead of a BaseScript. This contains the minimal pointers to
  // trampolines for the scripts to support direct jitCodeRaw calls.
  js::UnprotectedData<js::SelfHostedLazyScript> selfHostedLazyScript;

 private:
  /* Gecko profiling metadata */
  js::UnprotectedData<js::GeckoProfilerRuntime> geckoProfiler_;

 public:
  js::GeckoProfilerRuntime& geckoProfiler() { return geckoProfiler_.ref(); }

  // Heap GC roots for PersistentRooted pointers.
  js::MainThreadData<mozilla::EnumeratedArray<
      JS::RootKind, JS::RootKind::Limit,
      mozilla::LinkedList<JS::PersistentRooted<JS::detail::RootListEntry*>>>>
      heapRoots;

  void tracePersistentRoots(JSTracer* trc);
  void finishPersistentRoots();

  void finishRoots();

 private:
  js::UnprotectedData<const JSPrincipals*> trustedPrincipals_;

 public:
  void setTrustedPrincipals(const JSPrincipals* p) { trustedPrincipals_ = p; }
  const JSPrincipals* trustedPrincipals() const { return trustedPrincipals_; }

  js::MainThreadData<const JSWrapObjectCallbacks*> wrapObjectCallbacks;
  js::MainThreadData<js::PreserveWrapperCallback> preserveWrapperCallback;
  js::MainThreadData<js::HasReleasedWrapperCallback> hasReleasedWrapperCallback;

  js::MainThreadData<js::ScriptEnvironmentPreparer*> scriptEnvironmentPreparer;

  js::MainThreadData<JS::CTypesActivityCallback> ctypesActivityCallback;

 private:
  js::WriteOnceData<const JSClass*> windowProxyClass_;

 public:
  const JSClass* maybeWindowProxyClass() const { return windowProxyClass_; }
  void setWindowProxyClass(const JSClass* clasp) { windowProxyClass_ = clasp; }

 private:
  js::WriteOnceData<const JSClass*> abortSignalClass_;
  js::WriteOnceData<JS::AbortSignalIsAborted> abortSignalIsAborted_;

 public:
  void initPipeToHandling(const JSClass* abortSignalClass,
                          JS::AbortSignalIsAborted isAborted) {
    MOZ_ASSERT(abortSignalClass != nullptr,
               "doesn't make sense for an embedder to provide a null class "
               "when specifying pipeTo handling");
    MOZ_ASSERT(isAborted != nullptr, "must pass a valid function pointer");

    abortSignalClass_ = abortSignalClass;
    abortSignalIsAborted_ = isAborted;
  }

  const JSClass* maybeAbortSignalClass() const { return abortSignalClass_; }

  bool abortSignalIsAborted(JSObject* obj) {
    MOZ_ASSERT(abortSignalIsAborted_ != nullptr,
               "must call initPipeToHandling first");
    return abortSignalIsAborted_(obj);
  }

 private:
  // List of non-ephemeron weak containers to sweep during
  // beginSweepingSweepGroup.
  js::MainThreadData<mozilla::LinkedList<JS::detail::WeakCacheBase>>
      weakCaches_;

 public:
  mozilla::LinkedList<JS::detail::WeakCacheBase>& weakCaches() {
    return weakCaches_.ref();
  }
  void registerWeakCache(JS::detail::WeakCacheBase* cachep) {
    weakCaches().insertBack(cachep);
  }

  template <typename T>
  struct GlobalObjectWatchersLinkAccess {
    static mozilla::DoublyLinkedListElement<T>& Get(T* aThis) {
      return aThis->onNewGlobalObjectWatchersLink;
    }
  };

  template <typename T>
  struct GarbageCollectionWatchersLinkAccess {
    static mozilla::DoublyLinkedListElement<T>& Get(T* aThis) {
      return aThis->onGarbageCollectionWatchersLink;
    }
  };

  using OnNewGlobalWatchersList =
      mozilla::DoublyLinkedList<js::Debugger,
                                GlobalObjectWatchersLinkAccess<js::Debugger>>;
  using OnGarbageCollectionWatchersList = mozilla::DoublyLinkedList<
      js::Debugger, GarbageCollectionWatchersLinkAccess<js::Debugger>>;

 private:
  /*
   * List of all enabled Debuggers that have onNewGlobalObject handler
   * methods established.
   */
  js::MainThreadData<OnNewGlobalWatchersList> onNewGlobalObjectWatchers_;

  /*
   * List of all enabled Debuggers that have onGarbageCollection handler
   * methods established.
   */
  js::MainThreadData<OnGarbageCollectionWatchersList>
      onGarbageCollectionWatchers_;

 public:
  OnNewGlobalWatchersList& onNewGlobalObjectWatchers() {
    return onNewGlobalObjectWatchers_.ref();
  }

  OnGarbageCollectionWatchersList& onGarbageCollectionWatchers() {
    return onGarbageCollectionWatchers_.ref();
  }

 private:
  /* Linked list of all Debugger objects in the runtime. */
  js::MainThreadData<mozilla::LinkedList<js::Debugger>> debuggerList_;

 public:
  mozilla::LinkedList<js::Debugger>& debuggerList() {
    return debuggerList_.ref();
  }

 private:
  /*
   * Lock used to protect the script data table, which can be used by
   * off-thread parsing.
   *
   * Locking this only occurs if there is actually a thread other than the
   * main thread which could access this.
   */
  js::Mutex scriptDataLock;
#ifdef DEBUG
  bool activeThreadHasScriptDataAccess;
#endif

  // Number of off-thread ParseTasks that are using this runtime. This is only
  // updated on main-thread. If this is non-zero we must use `scriptDataLock` to
  // protect access to the bytecode table;
  mozilla::Atomic<size_t, mozilla::SequentiallyConsistent> numParseTasks;

  // Number of zones which may be operated on by helper threads.
  mozilla::Atomic<size_t, mozilla::SequentiallyConsistent>
      numActiveHelperThreadZones;

  friend class js::AutoLockScriptData;

 public:
  void setUsedByHelperThread(JS::Zone* zone);
  void clearUsedByHelperThread(JS::Zone* zone);

  bool hasParseTasks() const { return numParseTasks > 0; }
  bool hasHelperThreadZones() const { return numActiveHelperThreadZones > 0; }

  void addParseTaskRef() { numParseTasks++; }
  void decParseTaskRef() { numParseTasks--; }

#ifdef DEBUG
  void assertCurrentThreadHasScriptDataAccess() const {
    if (!hasParseTasks()) {
      MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(this) &&
                 activeThreadHasScriptDataAccess);
      return;
    }

    scriptDataLock.assertOwnedByCurrentThread();
  }

  bool currentThreadHasAtomsTableAccess() const {
    return js::CurrentThreadCanAccessRuntime(this) &&
           atoms_->mainThreadHasAllLocks();
  }
#endif

  JS::HeapState heapState() const { return gc.heapState(); }

  // How many realms there are across all zones. This number includes
  // off-thread context realms, so it isn't necessarily equal to the
  // number of realms visited by RealmsIter.
  js::MainThreadData<size_t> numRealms;

  // The Gecko Profiler may want to sample the allocations happening across the
  // browser. This callback can be registered to record the allocation.
  js::MainThreadData<JS::RecordAllocationsCallback> recordAllocationCallback;
  js::MainThreadData<double> allocationSamplingProbability;

 private:
  // Number of debuggee realms in the runtime.
  js::MainThreadData<size_t> numDebuggeeRealms_;

  // Number of debuggee realms in the runtime observing code coverage.
  js::MainThreadData<size_t> numDebuggeeRealmsObservingCoverage_;

 public:
  void incrementNumDebuggeeRealms();
  void decrementNumDebuggeeRealms();

  size_t numDebuggeeRealms() const { return numDebuggeeRealms_; }

  void incrementNumDebuggeeRealmsObservingCoverage();
  void decrementNumDebuggeeRealmsObservingCoverage();

  void startRecordingAllocations(double probability,
                                 JS::RecordAllocationsCallback callback);
  void stopRecordingAllocations();
  void ensureRealmIsRecordingAllocations(JS::Handle<js::GlobalObject*> global);

  /* Locale-specific callbacks for string conversion. */
  js::MainThreadData<const JSLocaleCallbacks*> localeCallbacks;

  /* Default locale for Internationalization API */
  js::MainThreadData<js::UniqueChars> defaultLocale;

  /* If true, new scripts must be created with PC counter information. */
  js::MainThreadOrIonCompileData<bool> profilingScripts;

  /* Strong references on scripts held for PCCount profiling API. */
  js::MainThreadData<JS::PersistentRooted<js::ScriptAndCountsVector>*>
      scriptAndCountsVector;

 private:
  /* Code coverage output. */
  js::UnprotectedData<js::coverage::LCovRuntime> lcovOutput_;

 public:
  js::coverage::LCovRuntime& lcovOutput() { return lcovOutput_.ref(); }

 private:
  js::UnprotectedData<js::jit::JitRuntime*> jitRuntime_;

  /*
   * Self-hosting state cloned on demand into other compartments. Shared with
   * the parent runtime if there is one.
   */
  js::WriteOnceData<js::NativeObject*> selfHostingGlobal_;

  static js::GlobalObject* createSelfHostingGlobal(JSContext* cx);

 public:
  void getUnclonedSelfHostedValue(js::PropertyName* name, JS::Value* vp);
  JSFunction* getUnclonedSelfHostedFunction(js::PropertyName* name);

  [[nodiscard]] bool createJitRuntime(JSContext* cx);
  js::jit::JitRuntime* jitRuntime() const { return jitRuntime_.ref(); }
  bool hasJitRuntime() const { return !!jitRuntime_; }

 private:
  // Used to generate random keys for hash tables.
  mozilla::Maybe<mozilla::non_crypto::XorShift128PlusRNG> randomKeyGenerator_;
  mozilla::non_crypto::XorShift128PlusRNG& randomKeyGenerator();

  // Used to generate random hash codes for symbols.
  mozilla::Maybe<mozilla::non_crypto::XorShift128PlusRNG>
      randomHashCodeGenerator_;

 public:
  mozilla::HashCodeScrambler randomHashCodeScrambler();
  mozilla::non_crypto::XorShift128PlusRNG forkRandomKeyGenerator();

  js::HashNumber randomHashCode();

  //-------------------------------------------------------------------------
  // Self-hosting support
  //-------------------------------------------------------------------------

  bool hasInitializedSelfHosting() const { return selfHostingGlobal_; }

  bool initSelfHosting(JSContext* cx, JS::SelfHostedCache xdrCache = nullptr,
                       JS::SelfHostedWriter xdrWriter = nullptr);
  void finishSelfHosting();
  void traceSelfHostingGlobal(JSTracer* trc);
  bool isSelfHostingGlobal(JSObject* global) {
    return global == selfHostingGlobal_;
  }
  js::GeneratorKind getSelfHostedFunctionGeneratorKind(JSAtom* name);
  bool createLazySelfHostedFunctionClone(JSContext* cx,
                                         js::HandlePropertyName selfHostedName,
                                         js::HandleAtom name, unsigned nargs,
                                         js::NewObjectKind newKind,
                                         js::MutableHandleFunction fun);
  bool cloneSelfHostedFunctionScript(JSContext* cx,
                                     js::Handle<js::PropertyName*> name,
                                     js::Handle<JSFunction*> targetFun);
  bool cloneSelfHostedValue(JSContext* cx, js::Handle<js::PropertyName*> name,
                            js::MutableHandleValue vp);
  void assertSelfHostedFunctionHasCanonicalName(JSContext* cx,
                                                js::HandlePropertyName name);
#if DEBUG
  bool isSelfHostingZone(const JS::Zone* zone) const {
    return selfHostingGlobal_ && selfHostingGlobal_->zone() == zone;
  }
#endif

  //-------------------------------------------------------------------------
  // Locale information
  //-------------------------------------------------------------------------

  /*
   * Set the default locale for the ECMAScript Internationalization API
   * (Intl.Collator, Intl.NumberFormat, Intl.DateTimeFormat).
   * Note that the Internationalization API encourages clients to
   * specify their own locales.
   * The locale string remains owned by the caller.
   */
  bool setDefaultLocale(const char* locale);

  /* Reset the default locale to OS defaults. */
  void resetDefaultLocale();

  /* Gets current default locale. String remains owned by context. */
  const char* getDefaultLocale();

  /* Garbage collector state. */
  js::gc::GCRuntime gc;

  /* Garbage collector state has been successfully initialized. */
  js::WriteOnceData<bool> gcInitialized;

  bool hasZealMode(js::gc::ZealMode mode) { return gc.hasZealMode(mode); }

  void lockGC() { gc.lockGC(); }

  void unlockGC() { gc.unlockGC(); }

  js::WriteOnceData<js::PropertyName*> emptyString;

 private:
  js::MainThreadOrGCTaskData<JSFreeOp*> defaultFreeOp_;

 public:
  JSFreeOp* defaultFreeOp() {
    MOZ_ASSERT(defaultFreeOp_);
    return defaultFreeOp_;
  }

#if !JS_HAS_INTL_API
  /* Number localization, used by jsnum.cpp. */
  js::WriteOnceData<const char*> thousandsSeparator;
  js::WriteOnceData<const char*> decimalSeparator;
  js::WriteOnceData<const char*> numGrouping;
#endif

 private:
  mozilla::Maybe<js::SharedImmutableStringsCache> sharedImmutableStrings_;

 public:
  // If this particular JSRuntime has a SharedImmutableStringsCache, return a
  // pointer to it, otherwise return nullptr.
  js::SharedImmutableStringsCache* maybeThisRuntimeSharedImmutableStrings() {
    return sharedImmutableStrings_.isSome() ? &*sharedImmutableStrings_
                                            : nullptr;
  }

  // Get a reference to this JSRuntime's or its parent's
  // SharedImmutableStringsCache.
  js::SharedImmutableStringsCache& sharedImmutableStrings() {
    MOZ_ASSERT_IF(parentRuntime, !sharedImmutableStrings_);
    MOZ_ASSERT_IF(!parentRuntime, sharedImmutableStrings_);
    return parentRuntime ? parentRuntime->sharedImmutableStrings()
                         : *sharedImmutableStrings_;
  }

 private:
  js::WriteOnceData<bool> beingDestroyed_;

 public:
  bool isBeingDestroyed() const { return beingDestroyed_; }

 private:
  bool allowContentJS_;

 public:
  bool allowContentJS() const { return allowContentJS_; }

  friend class js::AutoAssertNoContentJS;

 private:
  // Table of all atoms other than those in permanentAtoms and staticStrings.
  js::WriteOnceData<js::AtomsTable*> atoms_;

  // Set of all live symbols produced by Symbol.for(). All such symbols are
  // allocated in the atoms zone. Reading or writing the symbol registry
  // can only be done from the main thread.
  js::MainThreadOrGCTaskData<js::SymbolRegistry> symbolRegistry_;

  js::WriteOnceData<js::AtomSet*> permanentAtomsDuringInit_;
  js::WriteOnceData<js::FrozenAtomSet*> permanentAtoms_;

 public:
  bool initializeAtoms(JSContext* cx);
  bool initializeParserAtoms(JSContext* cx);
  void finishAtoms();
  void finishParserAtoms();
  bool atomsAreFinished() const {
    return !atoms_ && !permanentAtomsDuringInit_;
  }

  js::AtomsTable* atomsForSweeping() {
    MOZ_ASSERT(JS::RuntimeHeapIsCollecting());
    return atoms_;
  }

  js::AtomsTable& atoms() {
    MOZ_ASSERT(atoms_);
    return *atoms_;
  }

  const JS::Zone* atomsZone(const js::AutoAccessAtomsZone& access) const {
    return gc.atomsZone;
  }
  JS::Zone* atomsZone(const js::AutoAccessAtomsZone& access) {
    return gc.atomsZone;
  }
  JS::Zone* unsafeAtomsZone() { return gc.atomsZone; }

#ifdef DEBUG
  bool isAtomsZone(const JS::Zone* zone) const { return zone == gc.atomsZone; }
#endif

  bool activeGCInAtomsZone();

  js::SymbolRegistry& symbolRegistry() { return symbolRegistry_.ref(); }

  // Permanent atoms are fixed during initialization of the runtime and are
  // not modified or collected until the runtime is destroyed. These may be
  // shared with another, longer living runtime through |parentRuntime| and
  // can be freely accessed with no locking necessary.

  // Permanent atoms pre-allocated for general use.
  js::WriteOnceData<js::StaticStrings*> staticStrings;

  // Cached pointers to various permanent property names.
  js::WriteOnceData<JSAtomState*> commonNames;
  js::WriteOnceData<js::frontend::WellKnownParserAtoms*> commonParserNames;

  // All permanent atoms in the runtime, other than those in staticStrings.
  // Access to this does not require a lock because it is frozen and thus
  // read-only.
  const js::FrozenAtomSet* permanentAtoms() const {
    MOZ_ASSERT(permanentAtomsPopulated());
    return permanentAtoms_.ref();
  }

  // The permanent atoms table is populated during initialization.
  bool permanentAtomsPopulated() const { return permanentAtoms_; }

  // For internal use, return the permanent atoms table while it is being
  // populated.
  js::AtomSet* permanentAtomsDuringInit() const {
    MOZ_ASSERT(!permanentAtoms_);
    return permanentAtomsDuringInit_.ref();
  }

  bool initMainAtomsTables(JSContext* cx);
  void tracePermanentAtoms(JSTracer* trc);

  // Cached well-known symbols (ES6 rev 24 6.1.5.1). Like permanent atoms,
  // these are shared with the parentRuntime, if any.
  js::WriteOnceData<js::WellKnownSymbols*> wellKnownSymbols;

#ifdef JS_HAS_INTL_API
  /* Shared Intl data for this runtime. */
  js::MainThreadData<js::intl::SharedIntlData> sharedIntlData;

  void traceSharedIntlData(JSTracer* trc);
#endif

  // Table of bytecode and other data that may be shared across scripts
  // within the runtime. This may be modified by threads using
  // AutoLockScriptData.
 private:
  js::ScriptDataLockData<js::SharedImmutableScriptDataTable> scriptDataTable_;

 public:
  js::SharedImmutableScriptDataTable& scriptDataTable(
      const js::AutoLockScriptData& lock) {
    return scriptDataTable_.ref();
  }

 private:
  static mozilla::Atomic<size_t> liveRuntimesCount;

 public:
  static bool hasLiveRuntimes() { return liveRuntimesCount > 0; }

  explicit JSRuntime(JSRuntime* parentRuntime);
  ~JSRuntime();

  // destroyRuntime is used instead of a destructor, to ensure the downcast
  // to JSContext remains valid. The final GC triggered here depends on this.
  void destroyRuntime();

  bool init(JSContext* cx, uint32_t maxbytes);

  JSRuntime* thisFromCtor() { return this; }

 private:
  // Number of live SharedArrayBuffer objects, including those in Wasm shared
  // memories.  uint64_t to avoid any risk of overflow.
  js::MainThreadData<uint64_t> liveSABs;

 public:
  void incSABCount() {
    MOZ_RELEASE_ASSERT(liveSABs != UINT64_MAX);
    liveSABs++;
  }

  void decSABCount() {
    MOZ_RELEASE_ASSERT(liveSABs > 0);
    liveSABs--;
  }

  bool hasLiveSABs() const { return liveSABs > 0; }

 public:
  js::MainThreadData<JS::BeforeWaitCallback> beforeWaitCallback;
  js::MainThreadData<JS::AfterWaitCallback> afterWaitCallback;

 public:
  void reportAllocationOverflow() { js::ReportAllocationOverflow(nullptr); }

  /*
   * This should be called after system malloc/calloc/realloc returns nullptr
   * to try to recove some memory or to report an error.  For realloc, the
   * original pointer must be passed as reallocPtr.
   *
   * The function must be called outside the GC lock.
   */
  JS_PUBLIC_API void* onOutOfMemory(js::AllocFunction allocator,
                                    arena_id_t arena, size_t nbytes,
                                    void* reallocPtr = nullptr,
                                    JSContext* maybecx = nullptr);

  /*  onOutOfMemory but can call OnLargeAllocationFailure. */
  JS_PUBLIC_API void* onOutOfMemoryCanGC(js::AllocFunction allocator,
                                         arena_id_t arena, size_t nbytes,
                                         void* reallocPtr = nullptr);

  static const unsigned LARGE_ALLOCATION = 25 * 1024 * 1024;

  void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              JS::RuntimeSizes* rtSizes);

 private:
  // Settings for how helper threads can be used.
  mozilla::Atomic<bool, mozilla::SequentiallyConsistent>
      offthreadIonCompilationEnabled_;
  mozilla::Atomic<bool, mozilla::SequentiallyConsistent>
      parallelParsingEnabled_;

#ifdef DEBUG
  mozilla::Atomic<uint32_t> offThreadParsesRunning_;
  mozilla::Atomic<bool> offThreadParsingBlocked_;
#endif

  js::MainThreadData<bool> autoWritableJitCodeActive_;

 public:
  // Note: these values may be toggled dynamically (in response to about:config
  // prefs changing).
  void setOffthreadIonCompilationEnabled(bool value) {
    offthreadIonCompilationEnabled_ = value;
  }
  bool canUseOffthreadIonCompilation() const {
    return offthreadIonCompilationEnabled_;
  }
  void setParallelParsingEnabled(bool value) {
    parallelParsingEnabled_ = value;
  }
  bool canUseParallelParsing() const { return parallelParsingEnabled_; }

#ifdef DEBUG

  void incOffThreadParsesRunning() {
    MOZ_ASSERT(!isOffThreadParsingBlocked());
    offThreadParsesRunning_++;
  }

  void decOffThreadParsesRunning() {
    MOZ_ASSERT(isOffThreadParseRunning());
    offThreadParsesRunning_--;
  }

  bool isOffThreadParseRunning() const { return offThreadParsesRunning_; }

  bool isOffThreadParsingBlocked() const { return offThreadParsingBlocked_; }
  void setOffThreadParsingBlocked(bool blocked) {
    MOZ_ASSERT(offThreadParsingBlocked_ != blocked);
    MOZ_ASSERT(!isOffThreadParseRunning());
    offThreadParsingBlocked_ = blocked;
  }

#endif

  void toggleAutoWritableJitCodeActive(bool b) {
    MOZ_ASSERT(autoWritableJitCodeActive_ != b,
               "AutoWritableJitCode should not be nested.");
    autoWritableJitCodeActive_ = b;
  }

  /* See comment for JS::SetOutOfMemoryCallback in jsapi.h. */
  js::MainThreadData<JS::OutOfMemoryCallback> oomCallback;
  js::MainThreadData<void*> oomCallbackData;

  /*
   * Debugger.Memory functions like takeCensus use this embedding-provided
   * function to assess the size of malloc'd blocks of memory.
   */
  js::MainThreadData<mozilla::MallocSizeOf> debuggerMallocSizeOf;

  /* Last time at which an animation was played for this runtime. */
  js::MainThreadData<mozilla::TimeStamp> lastAnimationTime;

 private:
  /* The stack format for the current runtime.  Only valid on non-child
   * runtimes. */
  mozilla::Atomic<js::StackFormat, mozilla::ReleaseAcquire> stackFormat_;

 public:
  js::StackFormat stackFormat() const {
    const JSRuntime* rt = this;
    while (rt->parentRuntime) {
      MOZ_ASSERT(rt->stackFormat_ == js::StackFormat::Default);
      rt = rt->parentRuntime;
    }
    MOZ_ASSERT(rt->stackFormat_ != js::StackFormat::Default);
    return rt->stackFormat_;
  }
  void setStackFormat(js::StackFormat format) {
    MOZ_ASSERT(!parentRuntime);
    MOZ_ASSERT(format != js::StackFormat::Default);
    stackFormat_ = format;
  }

 private:
  js::MainThreadData<js::RuntimeCaches> caches_;

 public:
  js::RuntimeCaches& caches() { return caches_.ref(); }

  // List of all the live wasm::Instances in the runtime. Equal to the union
  // of all instances registered in all JS::Realms. Accessed from watchdog
  // threads for purposes of wasm::InterruptRunningCode().
  js::ExclusiveData<js::wasm::InstanceVector> wasmInstances;

  // The implementation-defined abstract operation HostResolveImportedModule.
  js::MainThreadData<JS::ModuleResolveHook> moduleResolveHook;

  // A hook that implements the abstract operations
  // HostGetImportMetaProperties and HostFinalizeImportMeta.
  js::MainThreadData<JS::ModuleMetadataHook> moduleMetadataHook;

  // A hook that implements the abstract operation
  // HostImportModuleDynamically. This is also used to enable/disable dynamic
  // module import and can accessed by off-thread parsing.
  mozilla::Atomic<JS::ModuleDynamicImportHook> moduleDynamicImportHook;

  // Hooks called when script private references are created and destroyed.
  js::MainThreadData<JS::ScriptPrivateReferenceHook> scriptPrivateAddRefHook;
  js::MainThreadData<JS::ScriptPrivateReferenceHook> scriptPrivateReleaseHook;

  void addRefScriptPrivate(const JS::Value& value) {
    if (!value.isUndefined() && scriptPrivateAddRefHook) {
      scriptPrivateAddRefHook(value);
    }
  }

  void releaseScriptPrivate(const JS::Value& value) {
    if (!value.isUndefined() && scriptPrivateReleaseHook) {
      scriptPrivateReleaseHook(value);
    }
  }

 public:
#if defined(NIGHTLY_BUILD)
  // Support for informing the embedding of any error thrown.
  // This mechanism is designed to let the embedding
  // log/report/fail in case certain errors are thrown
  // (e.g. SyntaxError, ReferenceError or TypeError
  // in critical code).
  struct ErrorInterceptionSupport {
    ErrorInterceptionSupport() : isExecuting(false), interceptor(nullptr) {}

    // true if the error interceptor is currently executing,
    // false otherwise. Used to avoid infinite loops.
    bool isExecuting;

    // if non-null, any call to `setPendingException`
    // in this runtime will trigger the call to `interceptor`
    JSErrorInterceptor* interceptor;
  };
  ErrorInterceptionSupport errorInterception;
#endif  // defined(NIGHTLY_BUILD)
};

// Context for sending telemetry to the embedder from any thread, main or
// helper.  Obtain a |JSTelemetrySender| by calling |getTelemetrySender()| on
// the |JSRuntime|.
struct JSTelemetrySender {
 private:
  friend struct JSRuntime;

  JSAccumulateTelemetryDataCallback callback_;

  explicit JSTelemetrySender(JSAccumulateTelemetryDataCallback callback)
      : callback_(callback) {}

 public:
  JSTelemetrySender() : callback_(nullptr) {}
  JSTelemetrySender(const JSTelemetrySender& other) = default;
  explicit JSTelemetrySender(JSRuntime* runtime)
      : JSTelemetrySender(runtime->getTelemetrySender()) {}

  // Accumulates data for Firefox telemetry. |id| is the ID of a JS_TELEMETRY_*
  // histogram. |key| provides an additional key to identify the histogram.
  // |sample| is the data to add to the histogram.
  void addTelemetry(int id, uint32_t sample, const char* key = nullptr) {
    if (callback_) {
      callback_(id, sample, key);
    }
  }
};

namespace js {

static MOZ_ALWAYS_INLINE void MakeRangeGCSafe(Value* vec, size_t len) {
  // Don't PodZero here because JS::Value is non-trivial.
  for (size_t i = 0; i < len; i++) {
    vec[i].setDouble(+0.0);
  }
}

static MOZ_ALWAYS_INLINE void MakeRangeGCSafe(Value* beg, Value* end) {
  MakeRangeGCSafe(beg, end - beg);
}

static MOZ_ALWAYS_INLINE void MakeRangeGCSafe(jsid* beg, jsid* end) {
  std::fill(beg, end, INT_TO_JSID(0));
}

static MOZ_ALWAYS_INLINE void MakeRangeGCSafe(jsid* vec, size_t len) {
  MakeRangeGCSafe(vec, vec + len);
}

static MOZ_ALWAYS_INLINE void MakeRangeGCSafe(Shape** beg, Shape** end) {
  std::fill(beg, end, nullptr);
}

static MOZ_ALWAYS_INLINE void MakeRangeGCSafe(Shape** vec, size_t len) {
  MakeRangeGCSafe(vec, vec + len);
}

static MOZ_ALWAYS_INLINE void SetValueRangeToUndefined(Value* beg, Value* end) {
  for (Value* v = beg; v != end; ++v) {
    v->setUndefined();
  }
}

static MOZ_ALWAYS_INLINE void SetValueRangeToUndefined(Value* vec, size_t len) {
  SetValueRangeToUndefined(vec, vec + len);
}

static MOZ_ALWAYS_INLINE void SetValueRangeToNull(Value* beg, Value* end) {
  for (Value* v = beg; v != end; ++v) {
    v->setNull();
  }
}

static MOZ_ALWAYS_INLINE void SetValueRangeToNull(Value* vec, size_t len) {
  SetValueRangeToNull(vec, vec + len);
}

extern const JSSecurityCallbacks NullSecurityCallbacks;

// This callback is set by JS::SetProcessLargeAllocationFailureCallback
// and may be null. See comment in jsapi.h.
extern mozilla::Atomic<JS::LargeAllocationFailureCallback>
    OnLargeAllocationFailure;

// This callback is set by JS::SetBuildIdOp and may be null. See comment in
// jsapi.h.
extern mozilla::Atomic<JS::BuildIdOp> GetBuildId;

extern JS::FilenameValidationCallback gFilenameValidationCallback;

} /* namespace js */

#endif /* vm_Runtime_h */
