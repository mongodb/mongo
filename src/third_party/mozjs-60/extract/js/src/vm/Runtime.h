/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Runtime_h
#define vm_Runtime_h

#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/DoublyLinkedList.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"
#include "mozilla/MaybeOneOf.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Scoped.h"
#include "mozilla/ThreadLocal.h"
#include "mozilla/Vector.h"

#include <setjmp.h>

#include "builtin/AtomicsObject.h"
#include "builtin/intl/SharedIntlData.h"
#include "builtin/Promise.h"
#include "frontend/NameCollections.h"
#include "gc/GCRuntime.h"
#include "gc/Tracer.h"
#include "gc/ZoneGroup.h"
#include "irregexp/RegExpStack.h"
#include "js/Debug.h"
#include "js/GCVector.h"
#include "js/HashTable.h"
#ifdef DEBUG
# include "js/Proxy.h" // For AutoEnterPolicy
#endif
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "threading/Thread.h"
#include "vm/Caches.h"
#include "vm/CodeCoverage.h"
#include "vm/CommonPropertyNames.h"
#include "vm/DateTime.h"
#include "vm/GeckoProfiler.h"
#include "vm/JSAtom.h"
#include "vm/JSScript.h"
#include "vm/Scope.h"
#include "vm/SharedImmutableStringsCache.h"
#include "vm/Stack.h"
#include "vm/Stopwatch.h"
#include "vm/SymbolType.h"
#include "wasm/WasmSignalHandlers.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4100) /* Silence unreferenced formal parameter warnings */
#endif

namespace js {

class AutoAssertNoContentJS;
class AutoKeepAtoms;
class EnterDebuggeeNoExecute;
#ifdef JS_TRACE_LOGGING
class TraceLoggerThread;
#endif

} // namespace js

struct DtoaState;

#ifdef JS_SIMULATOR_ARM64
namespace vixl {
class Simulator;
}
#endif

namespace js {

extern MOZ_COLD void
ReportOutOfMemory(JSContext* cx);

/* Different signature because the return type has MOZ_MUST_USE_TYPE. */
extern MOZ_COLD mozilla::GenericErrorResult<OOM&>
ReportOutOfMemoryResult(JSContext* cx);

extern MOZ_COLD void
ReportAllocationOverflow(JSContext* maybecx);

extern MOZ_COLD void
ReportOverRecursed(JSContext* cx);

class Activation;
class ActivationIterator;

namespace jit {
class JitRuntime;
class JitActivation;
struct PcScriptCache;
struct AutoFlushICache;
class CompileRuntime;

#ifdef JS_SIMULATOR_ARM64
typedef vixl::Simulator Simulator;
#elif defined(JS_SIMULATOR)
class Simulator;
#endif
} // namespace jit

// JS Engine Threading
//
// Multiple threads may interact with a JS runtime. JS has run-to-completion
// semantics, which means that scripts cannot observe changes in behavior
// due to activities performed on other threads (there is an exception to this
// for shared array buffers and related APIs).
//
// The main way we ensure that run-to-completion semantics are preserved is
// by dividing content into zone groups. Pieces of web content will be in the
// the same zone group if they have the same tab/origin or can otherwise
// observe changes in each other via Window.opener and so forth. When a thread
// executes JS in a zone group, it acquires that group --- including exclusive
// access to most of the group's content --- and does not relinquish control of
// the zone group until the script finishes executing.
//
// Threads interacting with a runtime are divided into two categories:
//
// - Cooperating threads are capable of running JS. At most one cooperating
//   thread may be |active| at a time in a runtime, but they may yield control
//   to each other so that their execution is interleaved. As described above,
//   each thread owns the zone groups it is operating on so that this
//   interleaving does not cause observable changes in a script's behavior.
//
// - Helper threads do not run JS, and are controlled or triggered by activity
//   in the cooperating threads. Helper threads may have exclusive access to
//   zone groups created for them, for parsing and similar tasks, but their
//   activities do not cause observable changes in script behaviors. Activity
//   on helper threads may be referred to as happening 'off thread' or on a
//   background thread in some parts of the VM.

} /* namespace js */

namespace JS {
struct RuntimeSizes;
} // namespace JS

/* Various built-in or commonly-used names pinned on first context. */
struct JSAtomState
{
#define PROPERTYNAME_FIELD(idpart, id, text) js::ImmutablePropertyNamePtr id;
    FOR_EACH_COMMON_PROPERTYNAME(PROPERTYNAME_FIELD)
#undef PROPERTYNAME_FIELD
#define PROPERTYNAME_FIELD(name, init, clasp) js::ImmutablePropertyNamePtr name;
    JS_FOR_EACH_PROTOTYPE(PROPERTYNAME_FIELD)
#undef PROPERTYNAME_FIELD
#define PROPERTYNAME_FIELD(name) js::ImmutablePropertyNamePtr name;
    JS_FOR_EACH_WELL_KNOWN_SYMBOL(PROPERTYNAME_FIELD)
#undef PROPERTYNAME_FIELD
#define PROPERTYNAME_FIELD(name) js::ImmutablePropertyNamePtr Symbol_##name;
    JS_FOR_EACH_WELL_KNOWN_SYMBOL(PROPERTYNAME_FIELD)
#undef PROPERTYNAME_FIELD

    js::ImmutablePropertyNamePtr* wellKnownSymbolNames() {
#define FIRST_PROPERTYNAME_FIELD(name) return &name;
    JS_FOR_EACH_WELL_KNOWN_SYMBOL(FIRST_PROPERTYNAME_FIELD)
#undef FIRST_PROPERTYNAME_FIELD
    }

    js::ImmutablePropertyNamePtr* wellKnownSymbolDescriptions() {
#define FIRST_PROPERTYNAME_FIELD(name) return &Symbol_ ##name;
    JS_FOR_EACH_WELL_KNOWN_SYMBOL(FIRST_PROPERTYNAME_FIELD)
#undef FIRST_PROPERTYNAME_FIELD
    }
};

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
struct WellKnownSymbols
{
#define DECLARE_SYMBOL(name) js::ImmutableSymbolPtr name;
    JS_FOR_EACH_WELL_KNOWN_SYMBOL(DECLARE_SYMBOL)
#undef DECLARE_SYMBOL

    const ImmutableSymbolPtr& get(size_t u) const {
        MOZ_ASSERT(u < JS::WellKnownSymbolLimit);
        const ImmutableSymbolPtr* symbols = reinterpret_cast<const ImmutableSymbolPtr*>(this);
        return symbols[u];
    }

    const ImmutableSymbolPtr& get(JS::SymbolCode code) const {
        return get(size_t(code));
    }

    WellKnownSymbols() {}
    WellKnownSymbols(const WellKnownSymbols&) = delete;
    WellKnownSymbols& operator=(const WellKnownSymbols&) = delete;
};

#define NAME_OFFSET(name)       offsetof(JSAtomState, name)

inline HandlePropertyName
AtomStateOffsetToName(const JSAtomState& atomState, size_t offset)
{
    return *reinterpret_cast<js::ImmutablePropertyNamePtr*>((char*)&atomState + offset);
}

// There are several coarse locks in the enum below. These may be either
// per-runtime or per-process. When acquiring more than one of these locks,
// the acquisition must be done in the order below to avoid deadlocks.
enum RuntimeLock {
    ExclusiveAccessLock,
    HelperThreadStateLock,
    GCLock
};

inline bool
CanUseExtraThreads()
{
    extern bool gCanUseExtraThreads;
    return gCanUseExtraThreads;
}

void DisableExtraThreads();

using ScriptAndCountsVector = GCVector<ScriptAndCounts, 0, SystemAllocPolicy>;

class AutoLockForExclusiveAccess;
class AutoLockScriptData;

} // namespace js

struct JSRuntime : public js::MallocProvider<JSRuntime>
{
  private:
    friend class js::Activation;
    friend class js::ActivationIterator;
    friend class js::jit::JitActivation;
    friend class js::jit::CompileRuntime;

  public:
    /*
     * If non-null, another runtime guaranteed to outlive this one and whose
     * permanent data may be used by this one where possible.
     */
    JSRuntime* const parentRuntime;

#ifdef DEBUG
    /* The number of child runtimes that have this runtime as their parent. */
    mozilla::Atomic<size_t> childRuntimeCount;

    class AutoUpdateChildRuntimeCount
    {
        JSRuntime* parent_;

      public:
        explicit AutoUpdateChildRuntimeCount(JSRuntime* parent)
          : parent_(parent)
        {
            if (parent_)
                parent_->childRuntimeCount++;
        }

        ~AutoUpdateChildRuntimeCount() {
            if (parent_)
                parent_->childRuntimeCount--;
        }
    };

    AutoUpdateChildRuntimeCount updateChildRuntimeCount;
#endif

  private:
#ifdef DEBUG
    js::WriteOnceData<bool> initialized_;
#endif

    // The context for the thread which currently has exclusive access to most
    // contents of the runtime. When execution on the runtime is cooperatively
    // scheduled, this is the thread which is currently running.
    mozilla::Atomic<JSContext*, mozilla::ReleaseAcquire> activeContext_;

    // All contexts participating in cooperative scheduling. All threads other
    // than |activeContext_| are suspended.
    js::ActiveThreadData<js::Vector<js::CooperatingContext, 4, js::SystemAllocPolicy>> cooperatingContexts_;

    // Count of AutoProhibitActiveContextChange instances on the active context.
    js::ActiveThreadData<size_t> activeContextChangeProhibited_;

    // Count of beginSingleThreadedExecution() calls that have occurred with no
    // matching endSingleThreadedExecution().
    js::ActiveThreadData<size_t> singleThreadedExecutionRequired_;

    // Whether some thread has called beginSingleThreadedExecution() and we are
    // in the associated callback (which may execute JS on other threads).
    js::ActiveThreadData<bool> startingSingleThreadedExecution_;

  public:
    JSContext* activeContext() const { return activeContext_; }
    const void* addressOfActiveContext() { return &activeContext_; }

    void setActiveContext(JSContext* cx);
    void setNewbornActiveContext(JSContext* cx);
    void deleteActiveContext(JSContext* cx);

    inline JSContext* activeContextFromOwnThread();

    js::Vector<js::CooperatingContext, 4, js::SystemAllocPolicy>& cooperatingContexts() {
        return cooperatingContexts_.ref();
    }

    class MOZ_RAII AutoProhibitActiveContextChange
    {
        JSRuntime* rt;

      public:
        explicit AutoProhibitActiveContextChange(JSRuntime* rt)
          : rt(rt)
        {
            rt->activeContextChangeProhibited_++;
        }

        ~AutoProhibitActiveContextChange()
        {
            rt->activeContextChangeProhibited_--;
        }
    };

    bool activeContextChangeProhibited() { return activeContextChangeProhibited_; }
    bool singleThreadedExecutionRequired() { return singleThreadedExecutionRequired_; }

    js::ActiveThreadData<JS::BeginSingleThreadedExecutionCallback> beginSingleThreadedExecutionCallback;
    js::ActiveThreadData<JS::EndSingleThreadedExecutionCallback> endSingleThreadedExecutionCallback;

    // Ensure there is only a single thread interacting with this runtime.
    // beginSingleThreadedExecution() returns false if some context has already
    // started forcing this runtime to be single threaded. Calls to these
    // functions must be balanced.
    bool beginSingleThreadedExecution(JSContext* cx);
    void endSingleThreadedExecution(JSContext* cx);

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
    mozilla::Atomic<uint64_t, mozilla::ReleaseAcquire> profilerSampleBufferRangeStart_;

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

    /* Call this to accumulate telemetry data. */
    js::ActiveThreadData<JSAccumulateTelemetryDataCallback> telemetryCallback;

    /* Call this to accumulate use counter data. */
    js::ActiveThreadData<JSSetUseCounterCallback> useCounterCallback;

  public:
    // Accumulates data for Firefox telemetry. |id| is the ID of a JS_TELEMETRY_*
    // histogram. |key| provides an additional key to identify the histogram.
    // |sample| is the data to add to the histogram.
    void addTelemetry(int id, uint32_t sample, const char* key = nullptr);

    void setTelemetryCallback(JSRuntime* rt, JSAccumulateTelemetryDataCallback callback);

    // Sets the use counter for a specific feature, measuring the presence or
    // absence of usage of a feature on a specific web page and document which
    // the passed JSObject belongs to.
    void setUseCounter(JSObject* obj, JSUseCounter counter);

    void setUseCounterCallback(JSRuntime* rt, JSSetUseCounterCallback callback);

  public:
    js::UnprotectedData<js::OffThreadPromiseRuntimeState> offThreadPromiseState;
    js::UnprotectedData<JS::ConsumeStreamCallback> consumeStreamCallback;

    JSObject* getIncumbentGlobal(JSContext* cx);
    bool enqueuePromiseJob(JSContext* cx, js::HandleFunction job, js::HandleObject promise,
                           js::HandleObject incumbentGlobal);
    void addUnhandledRejectedPromise(JSContext* cx, js::HandleObject promise);
    void removeUnhandledRejectedPromise(JSContext* cx, js::HandleObject promise);

    js::UnprotectedData<JS::RequestReadableStreamDataCallback> readableStreamDataRequestCallback;
    js::UnprotectedData<JS::WriteIntoReadRequestBufferCallback> readableStreamWriteIntoReadRequestCallback;
    js::UnprotectedData<JS::CancelReadableStreamCallback> readableStreamCancelCallback;
    js::UnprotectedData<JS::ReadableStreamClosedCallback> readableStreamClosedCallback;
    js::UnprotectedData<JS::ReadableStreamErroredCallback> readableStreamErroredCallback;
    js::UnprotectedData<JS::ReadableStreamFinalizeCallback> readableStreamFinalizeCallback;

    /* Had an out-of-memory error which did not populate an exception. */
    mozilla::Atomic<bool> hadOutOfMemory;

    /*
     * Allow relazifying functions in compartments that are active. This is
     * only used by the relazifyFunctions() testing function.
     */
    js::ActiveThreadData<bool> allowRelazificationForTesting;

    /* Compartment destroy callback. */
    js::ActiveThreadData<JSDestroyCompartmentCallback> destroyCompartmentCallback;

    /* Compartment memory reporting callback. */
    js::ActiveThreadData<JSSizeOfIncludingThisCompartmentCallback> sizeOfIncludingThisCompartmentCallback;

    /* Call this to get the name of a compartment. */
    js::ActiveThreadData<JSCompartmentNameCallback> compartmentNameCallback;

    /* Realm destroy callback. */
    js::ActiveThreadData<JS::DestroyRealmCallback> destroyRealmCallback;

    /* Call this to get the name of a realm. */
    js::ActiveThreadData<JS::RealmNameCallback> realmNameCallback;

    /* Callback for doing memory reporting on external strings. */
    js::ActiveThreadData<JSExternalStringSizeofCallback> externalStringSizeofCallback;

    js::ActiveThreadData<mozilla::UniquePtr<js::SourceHook>> sourceHook;

    js::ActiveThreadData<const JSSecurityCallbacks*> securityCallbacks;
    js::ActiveThreadData<const js::DOMCallbacks*> DOMcallbacks;
    js::ActiveThreadData<JSDestroyPrincipalsOp> destroyPrincipals;
    js::ActiveThreadData<JSReadPrincipalsOp> readPrincipals;

    /* Optional warning reporter. */
    js::ActiveThreadData<JS::WarningReporter> warningReporter;

  private:
    /* Gecko profiling metadata */
    js::UnprotectedData<js::GeckoProfilerRuntime> geckoProfiler_;
  public:
    js::GeckoProfilerRuntime& geckoProfiler() { return geckoProfiler_.ref(); }

    // Heap GC roots for PersistentRooted pointers.
    js::ActiveThreadData<mozilla::EnumeratedArray<JS::RootKind, JS::RootKind::Limit,
                                                 mozilla::LinkedList<JS::PersistentRooted<void*>>>> heapRoots;

    void tracePersistentRoots(JSTracer* trc);
    void finishPersistentRoots();

    void finishRoots();

  public:
    js::UnprotectedData<JS::BuildIdOp> buildIdOp;

    /* AsmJSCache callbacks are runtime-wide. */
    js::UnprotectedData<JS::AsmJSCacheOps> asmJSCacheOps;

  private:
    js::UnprotectedData<const JSPrincipals*> trustedPrincipals_;
  public:
    void setTrustedPrincipals(const JSPrincipals* p) { trustedPrincipals_ = p; }
    const JSPrincipals* trustedPrincipals() const { return trustedPrincipals_; }

    js::ActiveThreadData<const JSWrapObjectCallbacks*> wrapObjectCallbacks;
    js::ActiveThreadData<js::PreserveWrapperCallback> preserveWrapperCallback;

    js::ActiveThreadData<js::ScriptEnvironmentPreparer*> scriptEnvironmentPreparer;

    js::ActiveThreadData<js::CTypesActivityCallback> ctypesActivityCallback;

  private:
    js::WriteOnceData<const js::Class*> windowProxyClass_;

  public:
    const js::Class* maybeWindowProxyClass() const {
        return windowProxyClass_;
    }
    void setWindowProxyClass(const js::Class* clasp) {
        windowProxyClass_ = clasp;
    }

  private:
    // List of non-ephemeron weak containers to sweep during beginSweepingSweepGroup.
    js::ActiveThreadData<mozilla::LinkedList<JS::detail::WeakCacheBase>> weakCaches_;
  public:
    mozilla::LinkedList<JS::detail::WeakCacheBase>& weakCaches() { return weakCaches_.ref(); }
    void registerWeakCache(JS::detail::WeakCacheBase* cachep) {
        weakCaches().insertBack(cachep);
    }

    template <typename T>
    struct GlobalObjectWatchersLinkAccess {
      static mozilla::DoublyLinkedListElement<T>& Get(T* aThis) {
        return aThis->onNewGlobalObjectWatchersLink;
      }
    };

    using WatchersList =
        mozilla::DoublyLinkedList<js::Debugger,
                                  GlobalObjectWatchersLinkAccess<js::Debugger>>;
  private:
    /*
     * List of all enabled Debuggers that have onNewGlobalObject handler
     * methods established.
     */
    js::ActiveThreadData<WatchersList> onNewGlobalObjectWatchers_;

  public:
    WatchersList& onNewGlobalObjectWatchers() { return onNewGlobalObjectWatchers_.ref(); }

  private:
    /*
     * Lock taken when using per-runtime or per-zone data that could otherwise
     * be accessed simultaneously by multiple threads.
     *
     * Locking this only occurs if there is actually a thread other than the
     * active thread which could access such data.
     */
    js::Mutex exclusiveAccessLock;
#ifdef DEBUG
    bool activeThreadHasExclusiveAccess;
#endif

    /*
     * Lock used to protect the script data table, which can be used by
     * off-thread parsing.
     *
     * Locking this only occurs if there is actually a thread other than the
     * active thread which could access this.
     */
    js::Mutex scriptDataLock;
#ifdef DEBUG
    bool activeThreadHasScriptDataAccess;
#endif

    /*
     * Number of zones which may be operated on by non-cooperating helper
     * threads.
     */
    js::UnprotectedData<size_t> numActiveHelperThreadZones;

    friend class js::AutoLockForExclusiveAccess;
    friend class js::AutoLockScriptData;

  public:
    void setUsedByHelperThread(JS::Zone* zone);
    void clearUsedByHelperThread(JS::Zone* zone);

    bool hasHelperThreadZones() const {
        return numActiveHelperThreadZones > 0;
    }

#ifdef DEBUG
    bool currentThreadHasExclusiveAccess() const {
        return (!hasHelperThreadZones() && activeThreadHasExclusiveAccess) ||
            exclusiveAccessLock.ownedByCurrentThread();
    }

    bool currentThreadHasScriptDataAccess() const {
        return (!hasHelperThreadZones() && activeThreadHasScriptDataAccess) ||
            scriptDataLock.ownedByCurrentThread();
    }
#endif

    // How many compartments there are across all zones. This number includes
    // off thread context compartments, so it isn't necessarily equal to the
    // number of compartments visited by CompartmentsIter.
    js::ActiveThreadData<size_t> numCompartments;

    /* Locale-specific callbacks for string conversion. */
    js::ActiveThreadData<const JSLocaleCallbacks*> localeCallbacks;

    /* Default locale for Internationalization API */
    js::ActiveThreadData<char*> defaultLocale;

    /* If true, new scripts must be created with PC counter information. */
    js::ActiveThreadOrIonCompileData<bool> profilingScripts;

    /* Strong references on scripts held for PCCount profiling API. */
    js::ActiveThreadData<JS::PersistentRooted<js::ScriptAndCountsVector>*> scriptAndCountsVector;

  private:
    /* Code coverage output. */
    js::UnprotectedData<js::coverage::LCovRuntime> lcovOutput_;
  public:
    js::coverage::LCovRuntime& lcovOutput() { return lcovOutput_.ref(); }

  private:
    js::UnprotectedData<js::jit::JitRuntime*> jitRuntime_;

    /*
     * Self-hosting state cloned on demand into other compartments. Shared with the parent
     * runtime if there is one.
     */
    js::WriteOnceData<js::NativeObject*> selfHostingGlobal_;

    static js::GlobalObject*
    createSelfHostingGlobal(JSContext* cx);

    bool getUnclonedSelfHostedValue(JSContext* cx, js::HandlePropertyName name,
                                    js::MutableHandleValue vp);
    JSFunction* getUnclonedSelfHostedFunction(JSContext* cx, js::HandlePropertyName name);

    js::jit::JitRuntime* createJitRuntime(JSContext* cx);

  public:
    js::jit::JitRuntime* getJitRuntime(JSContext* cx) {
        return jitRuntime_ ? jitRuntime_.ref() : createJitRuntime(cx);
    }
    js::jit::JitRuntime* jitRuntime() const {
        return jitRuntime_.ref();
    }
    bool hasJitRuntime() const {
        return !!jitRuntime_;
    }

  private:
    // Used to generate random keys for hash tables.
    mozilla::Maybe<mozilla::non_crypto::XorShift128PlusRNG> randomKeyGenerator_;
    mozilla::non_crypto::XorShift128PlusRNG& randomKeyGenerator();

  public:
    mozilla::HashCodeScrambler randomHashCodeScrambler();
    mozilla::non_crypto::XorShift128PlusRNG forkRandomKeyGenerator();

    //-------------------------------------------------------------------------
    // Self-hosting support
    //-------------------------------------------------------------------------

    bool hasInitializedSelfHosting() const {
        return selfHostingGlobal_;
    }

    bool initSelfHosting(JSContext* cx);
    void finishSelfHosting();
    void traceSelfHostingGlobal(JSTracer* trc);
    bool isSelfHostingGlobal(JSObject* global) {
        return global == selfHostingGlobal_;
    }
    bool isSelfHostingCompartment(JSCompartment* comp) const;
    bool isSelfHostingZone(const JS::Zone* zone) const;
    bool createLazySelfHostedFunctionClone(JSContext* cx, js::HandlePropertyName selfHostedName,
                                           js::HandleAtom name, unsigned nargs,
                                           js::HandleObject proto,
                                           js::NewObjectKind newKind,
                                           js::MutableHandleFunction fun);
    bool cloneSelfHostedFunctionScript(JSContext* cx, js::Handle<js::PropertyName*> name,
                                       js::Handle<JSFunction*> targetFun);
    bool cloneSelfHostedValue(JSContext* cx, js::Handle<js::PropertyName*> name,
                              js::MutableHandleValue vp);
    void assertSelfHostedFunctionHasCanonicalName(JSContext* cx, js::HandlePropertyName name);

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
    js::gc::GCRuntime   gc;

    /* Garbage collector state has been successfully initialized. */
    js::WriteOnceData<bool> gcInitialized;

    bool hasZealMode(js::gc::ZealMode mode) { return gc.hasZealMode(mode); }

    void lockGC() {
        gc.lockGC();
    }

    void unlockGC() {
        gc.unlockGC();
    }

    /* Well-known numbers. */
    const js::Value     NaNValue;
    const js::Value     negativeInfinityValue;
    const js::Value     positiveInfinityValue;

    js::WriteOnceData<js::PropertyName*> emptyString;

  private:
    js::WriteOnceData<js::FreeOp*> defaultFreeOp_;

  public:
    js::FreeOp* defaultFreeOp() {
        MOZ_ASSERT(defaultFreeOp_);
        return defaultFreeOp_;
    }

#if !EXPOSE_INTL_API
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
        return sharedImmutableStrings_.isSome() ? &*sharedImmutableStrings_ : nullptr;
    }

    // Get a reference to this JSRuntime's or its parent's
    // SharedImmutableStringsCache.
    js::SharedImmutableStringsCache& sharedImmutableStrings() {
        MOZ_ASSERT_IF(parentRuntime, !sharedImmutableStrings_);
        MOZ_ASSERT_IF(!parentRuntime, sharedImmutableStrings_);
        return parentRuntime ? parentRuntime->sharedImmutableStrings() : *sharedImmutableStrings_;
    }

  private:
    js::WriteOnceData<bool> beingDestroyed_;
  public:
    bool isBeingDestroyed() const {
        return beingDestroyed_;
    }

  private:
    bool allowContentJS_;
  public:
    bool allowContentJS() const {
        return allowContentJS_;
    }

    friend class js::AutoAssertNoContentJS;

  private:
    // Set of all atoms other than those in permanentAtoms and staticStrings.
    // Reading or writing this set requires the calling thread to use
    // AutoLockForExclusiveAccess.
    js::ExclusiveAccessLockOrGCTaskData<js::AtomSet*> atoms_;

    // Set of all atoms added while the main atoms table is being swept.
    js::ExclusiveAccessLockData<js::AtomSet*> atomsAddedWhileSweeping_;

    // Compartment and associated zone containing all atoms in the runtime, as
    // well as runtime wide IonCode stubs. Modifying the contents of this
    // compartment requires the calling thread to use AutoLockForExclusiveAccess.
    js::WriteOnceData<JSCompartment*> atomsCompartment_;

    // Set of all live symbols produced by Symbol.for(). All such symbols are
    // allocated in the atomsCompartment. Reading or writing the symbol
    // registry requires the calling thread to use AutoLockForExclusiveAccess.
    js::ExclusiveAccessLockOrGCTaskData<js::SymbolRegistry> symbolRegistry_;

  public:
    bool initializeAtoms(JSContext* cx);
    void finishAtoms();
    bool atomsAreFinished() const { return !atoms_; }

    js::AtomSet* atomsForSweeping() {
        MOZ_ASSERT(JS::CurrentThreadIsHeapCollecting());
        return atoms_;
    }

    js::AtomSet& atoms(js::AutoLockForExclusiveAccess& lock) {
        MOZ_ASSERT(atoms_);
        return *atoms_;
    }
    js::AtomSet& unsafeAtoms() {
        MOZ_ASSERT(atoms_);
        return *atoms_;
    }

    bool createAtomsAddedWhileSweepingTable();
    void destroyAtomsAddedWhileSweepingTable();
    js::AtomSet* atomsAddedWhileSweeping() {
        return atomsAddedWhileSweeping_;
    }

    JSCompartment* atomsCompartment(js::AutoLockForExclusiveAccess& lock) {
        return atomsCompartment_;
    }
    JSCompartment* unsafeAtomsCompartment() {
        return atomsCompartment_;
    }

    bool isAtomsCompartment(JSCompartment* comp) {
        return comp == atomsCompartment_;
    }

    const JS::Zone* atomsZone(js::AutoLockForExclusiveAccess& lock) const {
        return gc.atomsZone;
    }

    // The atoms compartment is the only one in its zone.
    bool isAtomsZone(const JS::Zone* zone) const {
        return zone == gc.atomsZone;
    }

    bool activeGCInAtomsZone();

    js::SymbolRegistry& symbolRegistry(js::AutoLockForExclusiveAccess& lock) {
        return symbolRegistry_.ref();
    }
    js::SymbolRegistry& unsafeSymbolRegistry() {
        return symbolRegistry_.ref();
    }

    // Permanent atoms are fixed during initialization of the runtime and are
    // not modified or collected until the runtime is destroyed. These may be
    // shared with another, longer living runtime through |parentRuntime| and
    // can be freely accessed with no locking necessary.

    // Permanent atoms pre-allocated for general use.
    js::WriteOnceData<js::StaticStrings*> staticStrings;

    // Cached pointers to various permanent property names.
    js::WriteOnceData<JSAtomState*> commonNames;

    // All permanent atoms in the runtime, other than those in staticStrings.
    // Unlike |atoms_|, access to this does not require
    // AutoLockForExclusiveAccess because it is frozen and thus read-only.
    js::WriteOnceData<js::FrozenAtomSet*> permanentAtoms;

    bool transformToPermanentAtoms(JSContext* cx);

    // Cached well-known symbols (ES6 rev 24 6.1.5.1). Like permanent atoms,
    // these are shared with the parentRuntime, if any.
    js::WriteOnceData<js::WellKnownSymbols*> wellKnownSymbols;

    /* Shared Intl data for this runtime. */
    js::ActiveThreadData<js::intl::SharedIntlData> sharedIntlData;

    void traceSharedIntlData(JSTracer* trc);

    // Table of bytecode and other data that may be shared across scripts
    // within the runtime. This may be modified by threads using
    // AutoLockForExclusiveAccess.
  private:
    js::ScriptDataLockData<js::ScriptDataTable> scriptDataTable_;
  public:
    js::ScriptDataTable& scriptDataTable(const js::AutoLockScriptData& lock) {
        return scriptDataTable_.ref();
    }

    js::WriteOnceData<bool> jitSupportsFloatingPoint;
    js::WriteOnceData<bool> jitSupportsUnalignedAccesses;
    js::WriteOnceData<bool> jitSupportsSimd;

  private:
    static mozilla::Atomic<size_t> liveRuntimesCount;

  public:
    static bool hasLiveRuntimes() {
        return liveRuntimesCount > 0;
    }

    explicit JSRuntime(JSRuntime* parentRuntime);
    ~JSRuntime();

    // destroyRuntime is used instead of a destructor, to ensure the downcast
    // to JSContext remains valid. The final GC triggered here depends on this.
    void destroyRuntime();

    bool init(JSContext* cx, uint32_t maxbytes, uint32_t maxNurseryBytes);

    JSRuntime* thisFromCtor() { return this; }

  public:
    /*
     * Call this after allocating memory held by GC things, to update memory
     * pressure counters or report the OOM error if necessary. If oomError and
     * cx is not null the function also reports OOM error.
     *
     * The function must be called outside the GC lock and in case of OOM error
     * the caller must ensure that no deadlock possible during OOM reporting.
     */
    void updateMallocCounter(size_t nbytes);

    void reportAllocationOverflow() { js::ReportAllocationOverflow(nullptr); }

    /*
     * This should be called after system malloc/calloc/realloc returns nullptr
     * to try to recove some memory or to report an error.  For realloc, the
     * original pointer must be passed as reallocPtr.
     *
     * The function must be called outside the GC lock.
     */
    JS_FRIEND_API(void*) onOutOfMemory(js::AllocFunction allocator, size_t nbytes,
                                       void* reallocPtr = nullptr, JSContext* maybecx = nullptr);

    /*  onOutOfMemory but can call OnLargeAllocationFailure. */
    JS_FRIEND_API(void*) onOutOfMemoryCanGC(js::AllocFunction allocator, size_t nbytes,
                                            void* reallocPtr = nullptr);

    static const unsigned LARGE_ALLOCATION = 25 * 1024 * 1024;

    void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::RuntimeSizes* runtime);

  private:
    // Settings for how helper threads can be used.
    mozilla::Atomic<bool> offthreadIonCompilationEnabled_;
    mozilla::Atomic<bool> parallelParsingEnabled_;

    js::ActiveThreadData<bool> autoWritableJitCodeActive_;

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
    bool canUseParallelParsing() const {
        return parallelParsingEnabled_;
    }

    void toggleAutoWritableJitCodeActive(bool b) {
        MOZ_ASSERT(autoWritableJitCodeActive_ != b, "AutoWritableJitCode should not be nested.");
        autoWritableJitCodeActive_ = b;
    }

    /* See comment for JS::SetOutOfMemoryCallback in jsapi.h. */
    js::ActiveThreadData<JS::OutOfMemoryCallback> oomCallback;
    js::ActiveThreadData<void*> oomCallbackData;

    /*
     * Debugger.Memory functions like takeCensus use this embedding-provided
     * function to assess the size of malloc'd blocks of memory.
     */
    js::ActiveThreadData<mozilla::MallocSizeOf> debuggerMallocSizeOf;

    /* Last time at which an animation was played for this runtime. */
    mozilla::Atomic<int64_t> lastAnimationTime;

  private:
    js::ActiveThreadData<js::PerformanceMonitoring> performanceMonitoring_;
  public:
    js::PerformanceMonitoring& performanceMonitoring() { return performanceMonitoring_.ref(); }

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

    // For inherited heap state accessors.
    friend class js::gc::AutoTraceSession;
    friend class JS::AutoEnterCycleCollection;

  private:
    js::ActiveThreadData<js::RuntimeCaches> caches_;
  public:
    js::RuntimeCaches& caches() { return caches_.ref(); }

    // When wasm traps or is interrupted, the signal handler records some data
    // for unwinding purposes. Wasm code can't interrupt or trap reentrantly.
    js::ActiveThreadData<
        mozilla::MaybeOneOf<js::wasm::TrapData, js::wasm::InterruptData>
    > wasmUnwindData;

    js::wasm::TrapData& wasmTrapData() {
        return wasmUnwindData.ref().ref<js::wasm::TrapData>();
    }
    js::wasm::InterruptData& wasmInterruptData() {
        return wasmUnwindData.ref().ref<js::wasm::InterruptData>();
    }

  public:
#if defined(NIGHTLY_BUILD)
    // Support for informing the embedding of any error thrown.
    // This mechanism is designed to let the embedding
    // log/report/fail in case certain errors are thrown
    // (e.g. SyntaxError, ReferenceError or TypeError
    // in critical code).
    struct ErrorInterceptionSupport {
        ErrorInterceptionSupport()
          : isExecuting(false)
          , interceptor(nullptr)
        { }

        // true if the error interceptor is currently executing,
        // false otherwise. Used to avoid infinite loops.
        bool isExecuting;

        // if non-null, any call to `setPendingException`
        // in this runtime will trigger the call to `interceptor`
        JSErrorInterceptor* interceptor;
    };
    ErrorInterceptionSupport errorInterception;
#endif // defined(NIGHTLY_BUILD)
};

namespace js {

/*
 * RAII class that takes the GC lock while it is live.
 *
 * Usually functions will pass const references of this class.  However
 * non-const references can be used to either temporarily release the lock by
 * use of AutoUnlockGC or to start background allocation when the lock is
 * released.
 */
class MOZ_RAII AutoLockGC
{
  public:
    explicit AutoLockGC(JSRuntime* rt
                        MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : runtime_(rt)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        lock();
    }

    ~AutoLockGC() {
        lockGuard_.reset();
    }

    void lock() {
        MOZ_ASSERT(lockGuard_.isNothing());
        lockGuard_.emplace(runtime_->gc.lock);
    }

    void unlock() {
        MOZ_ASSERT(lockGuard_.isSome());
        lockGuard_.reset();
    }

    js::LockGuard<js::Mutex>& guard() {
        return lockGuard_.ref();
    }

  protected:
    JSRuntime* runtime() const { return runtime_; }

  private:
    JSRuntime* runtime_;
    mozilla::Maybe<js::LockGuard<js::Mutex>> lockGuard_;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    AutoLockGC(const AutoLockGC&) = delete;
    AutoLockGC& operator=(const AutoLockGC&) = delete;
};

/*
 * Same as AutoLockGC except it can optionally start a background chunk
 * allocation task when the lock is released.
 */
class MOZ_RAII AutoLockGCBgAlloc : public AutoLockGC
{
  public:
    explicit AutoLockGCBgAlloc(JSRuntime* rt)
      : AutoLockGC(rt)
      , startBgAlloc(false)
    {}

    ~AutoLockGCBgAlloc() {
        unlock();

        /*
         * We have to do this after releasing the lock because it may acquire
         * the helper lock which could cause lock inversion if we still held
         * the GC lock.
         */
        if (startBgAlloc)
            runtime()->gc.startBackgroundAllocTaskIfIdle();
    }

    /*
     * This can be used to start a background allocation task (if one isn't
     * already running) that allocates chunks and makes them available in the
     * free chunks list.  This happens after the lock is released in order to
     * avoid lock inversion.
     */
    void tryToStartBackgroundAllocation() {
        startBgAlloc = true;
    }

  private:
    // true if we should start a background chunk allocation task after the
    // lock is released.
    bool startBgAlloc;
};

class MOZ_RAII AutoUnlockGC
{
  public:
    explicit AutoUnlockGC(AutoLockGC& lock
                          MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : lock(lock)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        lock.unlock();
    }

    ~AutoUnlockGC() {
        lock.lock();
    }

  private:
    AutoLockGC& lock;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    AutoUnlockGC(const AutoUnlockGC&) = delete;
    AutoUnlockGC& operator=(const AutoUnlockGC&) = delete;
};

/************************************************************************/

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(Value* vec, size_t len)
{
    mozilla::PodZero(vec, len);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(Value* beg, Value* end)
{
    mozilla::PodZero(beg, end - beg);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(jsid* beg, jsid* end)
{
    for (jsid* id = beg; id != end; ++id)
        *id = INT_TO_JSID(0);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(jsid* vec, size_t len)
{
    MakeRangeGCSafe(vec, vec + len);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(Shape** beg, Shape** end)
{
    mozilla::PodZero(beg, end - beg);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(Shape** vec, size_t len)
{
    mozilla::PodZero(vec, len);
}

static MOZ_ALWAYS_INLINE void
SetValueRangeToUndefined(Value* beg, Value* end)
{
    for (Value* v = beg; v != end; ++v)
        v->setUndefined();
}

static MOZ_ALWAYS_INLINE void
SetValueRangeToUndefined(Value* vec, size_t len)
{
    SetValueRangeToUndefined(vec, vec + len);
}

static MOZ_ALWAYS_INLINE void
SetValueRangeToNull(Value* beg, Value* end)
{
    for (Value* v = beg; v != end; ++v)
        v->setNull();
}

static MOZ_ALWAYS_INLINE void
SetValueRangeToNull(Value* vec, size_t len)
{
    SetValueRangeToNull(vec, vec + len);
}

extern const JSSecurityCallbacks NullSecurityCallbacks;

inline Nursery&
ZoneGroup::nursery()
{
    return runtime->gc.nursery();
}

inline gc::StoreBuffer&
ZoneGroup::storeBuffer()
{
    return runtime->gc.storeBuffer();
}

// This callback is set by JS::SetProcessLargeAllocationFailureCallback
// and may be null. See comment in jsapi.h.
extern mozilla::Atomic<JS::LargeAllocationFailureCallback> OnLargeAllocationFailure;

} /* namespace js */

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /* vm_Runtime_h */
