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
#include "mozilla/MemoryReporting.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <algorithm>
#include <utility>

#ifdef JS_HAS_INTL_API
#  include "builtin/intl/SharedIntlData.h"
#endif
#include "frontend/ScriptIndex.h"
#include "gc/GCRuntime.h"
#include "js/AllocationRecording.h"
#include "js/BuildId.h"  // JS::BuildIdOp
#include "js/Context.h"
#include "js/experimental/CTypes.h"     // JS::CTypesActivityCallback
#include "js/friend/StackLimits.h"      // js::ReportOverRecursed
#include "js/friend/UsageStatistics.h"  // JSAccumulateTelemetryDataCallback
#include "js/GCVector.h"
#include "js/HashTable.h"
#include "js/Initialization.h"
#include "js/MemoryCallbacks.h"
#include "js/Modules.h"  // JS::Module{DynamicImport,Metadata,Resolve}Hook
#include "js/ScriptPrivate.h"
#include "js/shadow/Zone.h"
#include "js/ShadowRealmCallbacks.h"
#include "js/Stack.h"
#include "js/StreamConsumer.h"
#include "js/Symbol.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/WaitCallbacks.h"
#include "js/Warnings.h"  // JS::WarningReporter
#include "js/Zone.h"
#include "vm/Caches.h"  // js::RuntimeCaches
#include "vm/CodeCoverage.h"
#include "vm/GeckoProfiler.h"
#include "vm/InvalidatingFuse.h"
#include "vm/JSScript.h"
#include "vm/OffThreadPromiseRuntimeState.h"  // js::OffThreadPromiseRuntimeState
#include "vm/SharedScriptDataTableHolder.h"   // js::SharedScriptDataTableHolder
#include "vm/Stack.h"
#include "wasm/WasmTypeDecls.h"

struct JSAtomState;
struct JSClass;
struct JSErrorInterceptor;
struct JSWrapObjectCallbacks;

namespace js {

class AutoAssertNoContentJS;
class Debugger;
class EnterDebuggeeNoExecute;
class FrontendContext;
class PlainObject;
class StaticStrings;

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
extern MOZ_COLD void ReportAllocationOverflow(JSContext* maybecx);
extern MOZ_COLD void ReportAllocationOverflow(FrontendContext* fc);
extern MOZ_COLD void ReportOversizedAllocation(JSContext* cx,
                                               const unsigned errorNumber);

class Activation;
class ActivationIterator;
class Shape;
class SourceHook;

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
struct CompilationInput;
struct CompilationStencil;
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
#define DECLARE_SYMBOL(name) ImmutableTenuredPtr<JS::Symbol*> name;
  JS_FOR_EACH_WELL_KNOWN_SYMBOL(DECLARE_SYMBOL)
#undef DECLARE_SYMBOL

  const ImmutableTenuredPtr<JS::Symbol*>& get(size_t u) const {
    MOZ_ASSERT(u < JS::WellKnownSymbolLimit);
    const ImmutableTenuredPtr<JS::Symbol*>* symbols =
        reinterpret_cast<const ImmutableTenuredPtr<JS::Symbol*>*>(this);
    return symbols[u];
  }

  const ImmutableTenuredPtr<JS::Symbol*>& get(JS::SymbolCode code) const {
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

// An interface for reporting telemetry from within SpiderMonkey. Reporting data
// to this interface will forward it to the embedding if a telemetry callback
// was registered. It is the embedding's responsibility to store and/or combine
// repeated samples for each metric.
class Metrics {
 private:
  JSRuntime* rt_;

 public:
  explicit Metrics(JSRuntime* rt) : rt_(rt) {}

  // Records a TimeDuration metric. These are converted to integers when being
  // recorded so choose an appropriate scale. In the future these will be Glean
  // Timing Distribution metrics.
  struct TimeDuration_S {
    using SourceType = mozilla::TimeDuration;
    static uint32_t convert(SourceType td) { return uint32_t(td.ToSeconds()); }
  };
  struct TimeDuration_MS {
    using SourceType = mozilla::TimeDuration;
    static uint32_t convert(SourceType td) {
      return uint32_t(td.ToMilliseconds());
    }
  };
  struct TimeDuration_US {
    using SourceType = mozilla::TimeDuration;
    static uint32_t convert(SourceType td) {
      return uint32_t(td.ToMicroseconds());
    }
  };

  // Record a metric in bytes. In the future these will be Glean Memory
  // Distribution metrics.
  struct MemoryDistribution {
    using SourceType = size_t;
    static uint32_t convert(SourceType sz) {
      return static_cast<uint32_t>(std::min(sz, size_t(UINT32_MAX)));
    }
  };

  // Record a metric for a quanity of items. This doesn't currently have a Glean
  // analogue and we avoid using MemoryDistribution directly to avoid confusion
  // about units.
  using QuantityDistribution = MemoryDistribution;

  // Record the distribution of boolean values. In the future this will be a
  // Glean Rate metric.
  struct Boolean {
    using SourceType = bool;
    static uint32_t convert(SourceType sample) {
      return static_cast<uint32_t>(sample);
    }
  };

  // Record the distribution of an enumeration value. This records integer
  // values so take care not to redefine the value of enum values. In the
  // future, these should become Glean Labeled Counter metrics.
  struct Enumeration {
    using SourceType = unsigned int;
    static uint32_t convert(SourceType sample) {
      MOZ_ASSERT(sample <= 100);
      return static_cast<uint32_t>(sample);
    }
  };

  // Record a percentage distribution in the range 0 to 100. This takes a double
  // and converts it to an integer. In the future, this will be a Glean Custom
  // Distribution unless they add a better match.
  struct Percentage {
    using SourceType = double;
    static uint32_t convert(SourceType sample) {
      MOZ_ASSERT(sample >= 0.0 && sample <= 100.0);
      return static_cast<uint32_t>(sample);
    }
  };

  // Record an unsigned integer.
  struct Integer {
    using SourceType = uint32_t;
    static uint32_t convert(SourceType sample) { return sample; }
  };

  inline void addTelemetry(JSMetric id, uint32_t sample);

#define DECLARE_METRIC_HELPER(NAME, TY)                \
  void NAME(TY::SourceType sample) {                   \
    addTelemetry(JSMetric::NAME, TY::convert(sample)); \
  }
  FOR_EACH_JS_METRIC(DECLARE_METRIC_HELPER)
#undef DECLARE_METRIC_HELPER
};

class HasSeenObjectEmulateUndefinedFuse : public js::InvalidatingRuntimeFuse {
  virtual const char* name() override {
    return "HasSeenObjectEmulateUndefinedFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override {
    // Without traversing the GC heap I don't think it's possible to assert
    // this invariant directly.
    return true;
  }
};

}  // namespace js

struct JSRuntime {
 private:
  friend class js::Activation;
  friend class js::ActivationIterator;
  friend class js::jit::JitActivation;
  friend class js::jit::CompileRuntime;

  /* Space for interpreter frames. */
  js::MainThreadData<js::InterpreterStack> interpreterStack_;

#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  /* Space for portable baseline interpreter frames. */
  js::MainThreadData<js::PortableBaselineStack> portableBaselineStack_;
#endif

 public:
  js::InterpreterStack& interpreterStack() { return interpreterStack_.ref(); }
#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  js::PortableBaselineStack& portableBaselineStack() {
    return portableBaselineStack_.ref();
  }
#endif

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

  js::Metrics metrics() { return js::Metrics(this); }

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

 public:
  // Accumulates data for Firefox telemetry.
  void addTelemetry(JSMetric id, uint32_t sample);

  void setTelemetryCallback(JSRuntime* rt,
                            JSAccumulateTelemetryDataCallback callback);

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

  js::MainThreadData<JS::EnsureCanAddPrivateElementOp> canAddPrivateElement;

  /* Optional warning reporter. */
  js::MainThreadData<JS::WarningReporter> warningReporter;

  // Lazy self-hosted functions use a shared SelfHostedLazyScript instance
  // instead instead of a BaseScript. This contains the minimal pointers to
  // trampolines for the scripts to support direct jitCodeRaw calls.
  js::UnprotectedData<js::SelfHostedLazyScript> selfHostedLazyScript;

 private:
  // The self-hosted JS code is compiled as a Stencil which is then attached to
  // the Runtime. This is used to instantiate functions into realms on demand.
  js::WriteOnceData<js::frontend::CompilationInput*> selfHostStencilInput_;
  js::WriteOnceData<js::frontend::CompilationStencil*> selfHostStencil_;

 public:
  // The self-hosted stencil is immutable once attached to the runtime, so
  // worker runtimes directly use the stencil on the parent runtime.
  js::frontend::CompilationInput& selfHostStencilInput() {
    MOZ_ASSERT(hasSelfHostStencil());
    return *selfHostStencilInput_.ref();
  }
  js::frontend::CompilationStencil& selfHostStencil() {
    MOZ_ASSERT(hasSelfHostStencil());
    return *selfHostStencil_.ref();
  }
  bool hasSelfHostStencil() const { return bool(selfHostStencil_.ref()); }

  // A mapping from the name of self-hosted function to a ScriptIndex range of
  // the function and inner-functions within the self-hosted stencil.
  js::MainThreadData<
      JS::GCHashMap<js::PreBarriered<JSAtom*>, js::frontend::ScriptIndexRange,
                    js::DefaultHasher<JSAtom*>, js::SystemAllocPolicy>>
      selfHostScriptMap;

 private:
  /* Gecko profiling metadata */
  js::UnprotectedData<js::GeckoProfilerRuntime> geckoProfiler_;

 public:
  js::GeckoProfilerRuntime& geckoProfiler() { return geckoProfiler_.ref(); }

  // Heap GC roots for PersistentRooted pointers.
  js::MainThreadData<mozilla::EnumeratedArray<
      JS::RootKind, mozilla::LinkedList<js::PersistentRootedBase>,
      size_t(JS::RootKind::Limit)>>
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

 public:
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

  using RootedPlainObjVec = JS::PersistentRooted<
      JS::GCVector<js::PlainObject*, 0, js::SystemAllocPolicy>>;
  js::MainThreadData<js::UniquePtr<RootedPlainObjVec>> watchtowerTestingLog;

 private:
  /* Code coverage output. */
  js::UnprotectedData<js::coverage::LCovRuntime> lcovOutput_;

  /* Functions to call, together with data, when the runtime is being torn down.
   */
  js::MainThreadData<mozilla::Vector<std::pair<void (*)(void*), void*>, 4>>
      cleanupClosures;

 public:
  js::coverage::LCovRuntime& lcovOutput() { return lcovOutput_.ref(); }

  /* Register a cleanup function to be called during runtime shutdown. Do not
   * depend on the ordering of cleanup calls. */
  bool atExit(void (*function)(void*), void* data) {
    return cleanupClosures.ref().append(std::pair(function, data));
  }

 private:
  js::UnprotectedData<js::jit::JitRuntime*> jitRuntime_;

 public:
  mozilla::Maybe<js::frontend::ScriptIndexRange> getSelfHostedScriptIndexRange(
      js::PropertyName* name);

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

  bool hasInitializedSelfHosting() const { return hasSelfHostStencil(); }

  bool initSelfHostingStencil(JSContext* cx, JS::SelfHostedCache xdrCache,
                              JS::SelfHostedWriter xdrWriter);
  bool initSelfHostingFromStencil(JSContext* cx);
  void finishSelfHosting();
  void traceSelfHostingStencil(JSTracer* trc);
  js::GeneratorKind getSelfHostedFunctionGeneratorKind(js::PropertyName* name);
  bool delazifySelfHostedFunction(JSContext* cx,
                                  js::Handle<js::PropertyName*> name,
                                  js::Handle<JSFunction*> targetFun);
  bool getSelfHostedValue(JSContext* cx, js::Handle<js::PropertyName*> name,
                          js::MutableHandleValue vp);
  void assertSelfHostedFunctionHasCanonicalName(
      JS::Handle<js::PropertyName*> name);

 private:
  void setSelfHostingStencil(
      JS::MutableHandle<js::UniquePtr<js::frontend::CompilationInput>> input,
      RefPtr<js::frontend::CompilationStencil>&& stencil);

  //-------------------------------------------------------------------------
  // Locale information
  //-------------------------------------------------------------------------

 public:
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

  bool hasZealMode(js::gc::ZealMode mode) { return gc.hasZealMode(mode); }

  void lockGC() { gc.lockGC(); }

  void unlockGC() { gc.unlockGC(); }

  js::WriteOnceData<js::PropertyName*> emptyString;

 public:
  JS::GCContext* gcContext() { return &gc.mainThreadContext.ref(); }

#if !JS_HAS_INTL_API
  /* Number localization, used by jsnum.cpp. */
  js::WriteOnceData<const char*> thousandsSeparator;
  js::WriteOnceData<const char*> decimalSeparator;
  js::WriteOnceData<const char*> numGrouping;
#endif

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

  js::WriteOnceData<js::FrozenAtomSet*> permanentAtoms_;

 public:
  bool initializeAtoms(JSContext* cx);
  void finishAtoms();
  bool atomsAreFinished() const { return !atoms_; }

  js::AtomsTable* atomsForSweeping() {
    MOZ_ASSERT(JS::RuntimeHeapIsCollecting());
    return atoms_;
  }

  js::AtomsTable& atoms() {
    MOZ_ASSERT(atoms_);
    return *atoms_;
  }

  JS::Zone* atomsZone() {
    MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(this));
    return unsafeAtomsZone();
  }
  JS::Zone* unsafeAtomsZone() { return gc.atomsZone(); }

#ifdef DEBUG
  bool isAtomsZone(const JS::Zone* zone) const {
    return JS::shadow::Zone::from(zone)->isAtomsZone();
  }
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

  // All permanent atoms in the runtime, other than those in staticStrings.
  // Access to this does not require a lock because it is frozen and thus
  // read-only.
  const js::FrozenAtomSet* permanentAtoms() const {
    MOZ_ASSERT(permanentAtomsPopulated());
    return permanentAtoms_.ref();
  }

  // The permanent atoms table is populated during initialization.
  bool permanentAtomsPopulated() const { return permanentAtoms_; }

  // Cached well-known symbols (ES6 rev 24 6.1.5.1). Like permanent atoms,
  // these are shared with the parentRuntime, if any.
  js::WriteOnceData<js::WellKnownSymbols*> wellKnownSymbols;

#ifdef JS_HAS_INTL_API
  /* Shared Intl data for this runtime. */
  js::MainThreadData<js::intl::SharedIntlData> sharedIntlData;

  void traceSharedIntlData(JSTracer* trc);
#endif

 private:
  js::SharedScriptDataTableHolder scriptDataTableHolder_;

 public:
  // Returns the runtime's local script data table holder.
  js::SharedScriptDataTableHolder& scriptDataTableHolder();

 private:
  static mozilla::Atomic<size_t> liveRuntimesCount;

 public:
  static bool hasLiveRuntimes() { return liveRuntimesCount > 0; }
  static bool hasSingleLiveRuntime() { return liveRuntimesCount == 1; }

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
  void reportAllocationOverflow() {
    js::ReportAllocationOverflow(static_cast<JSContext*>(nullptr));
  }

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

  void toggleAutoWritableJitCodeActive(bool b) {
    MOZ_ASSERT(autoWritableJitCodeActive_ != b,
               "AutoWritableJitCode should not be nested.");
    autoWritableJitCodeActive_ = b;
  }

  /* See comment for JS::SetOutOfMemoryCallback in js/MemoryCallbacks.h. */
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
  // Warning: no data should be accessed in these caches from another thread,
  // but Ion needs to be able to access addresses inside here, which should be
  // safe, as the actual cache lookups will be performed on the main thread
  // through jitted code.
  js::MainThreadOrIonCompileData<js::RuntimeCaches> caches_;

 public:
  js::RuntimeCaches& caches() { return caches_.ref(); }

  // List of all the live wasm::Instances in the runtime. Equal to the union
  // of all instances registered in all JS::Realms. Accessed from watchdog
  // threads for purposes of wasm::InterruptRunningCode().
  js::ExclusiveData<js::wasm::InstanceVector> wasmInstances;

  // A counter used when recording the order in which modules had their
  // AsyncEvaluation field set to true. This is used to order queued
  // evaluations. This is reset when the last module that was async evaluating
  // is finished.
  //
  // See https://tc39.es/ecma262/#sec-async-module-execution-fulfilled step 10
  // for use.
  js::MainThreadData<uint32_t> moduleAsyncEvaluatingPostOrder;

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

 public:
  JS::GlobalInitializeCallback getShadowRealmInitializeGlobalCallback() {
    return shadowRealmInitializeGlobalCallback;
  }

  JS::GlobalCreationCallback getShadowRealmGlobalCreationCallback() {
    return shadowRealmGlobalCreationCallback;
  }

  js::MainThreadData<JS::GlobalInitializeCallback>
      shadowRealmInitializeGlobalCallback;

  js::MainThreadData<JS::GlobalCreationCallback>
      shadowRealmGlobalCreationCallback;

  js::MainThreadData<js::HasSeenObjectEmulateUndefinedFuse>
      hasSeenObjectEmulateUndefinedFuse;
};

namespace js {

void Metrics::addTelemetry(JSMetric id, uint32_t sample) {
  rt_->addTelemetry(id, sample);
}

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
  std::fill(beg, end, PropertyKey::Int(0));
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
