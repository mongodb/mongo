/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS execution context. */

#ifndef vm_JSContext_h
#define vm_JSContext_h

#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"

#include "jstypes.h"  // JS_PUBLIC_API

#include "ds/TraceableFifo.h"
#include "gc/Memory.h"
#include "irregexp/RegExpTypes.h"
#include "js/CharacterEncoding.h"
#include "js/ContextOptions.h"  // JS::ContextOptions
#include "js/GCVector.h"
#include "js/Promise.h"
#include "js/Result.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "threading/ProtectedData.h"
#include "util/StructuredSpewer.h"
#include "vm/Activation.h"  // js::Activation
#include "vm/ErrorReporting.h"
#include "vm/MallocProvider.h"
#include "vm/Runtime.h"
#include "vm/SharedStencil.h"  // js::SharedImmutableScriptDataTable
#include "wasm/WasmContext.h"

struct JS_PUBLIC_API JSContext;

struct DtoaState;

namespace js {

class AutoAllocInAtomsZone;
class AutoMaybeLeaveAtomsZone;
class AutoRealm;

namespace frontend {
class WellKnownParserAtoms;
}  // namespace frontend

namespace jit {
class ICScript;
class JitActivation;
class JitContext;
class DebugModeOSRVolatileJitFrameIter;
}  // namespace jit

namespace gc {
class AutoCheckCanAccessAtomsDuringGC;
class AutoSuppressNurseryCellAlloc;
}  // namespace gc

/* Detects cycles when traversing an object graph. */
class MOZ_RAII AutoCycleDetector {
 public:
  using Vector = GCVector<JSObject*, 8>;

  AutoCycleDetector(JSContext* cx, HandleObject objArg)
      : cx(cx), obj(cx, objArg), cyclic(true) {}

  ~AutoCycleDetector();

  bool init();

  bool foundCycle() { return cyclic; }

 private:
  JSContext* cx;
  RootedObject obj;
  bool cyclic;
};

struct AutoResolving;

struct ParseTask;

class InternalJobQueue : public JS::JobQueue {
 public:
  explicit InternalJobQueue(JSContext* cx)
      : queue(cx, SystemAllocPolicy()), draining_(false), interrupted_(false) {}
  ~InternalJobQueue() = default;

  // JS::JobQueue methods.
  JSObject* getIncumbentGlobal(JSContext* cx) override;
  bool enqueuePromiseJob(JSContext* cx, JS::HandleObject promise,
                         JS::HandleObject job, JS::HandleObject allocationSite,
                         JS::HandleObject incumbentGlobal) override;
  void runJobs(JSContext* cx) override;
  bool empty() const override;

  // If we are currently in a call to runJobs(), make that call stop processing
  // jobs once the current one finishes, and return. If we are not currently in
  // a call to runJobs, make all future calls return immediately.
  void interrupt() { interrupted_ = true; }

  // Return the front element of the queue, or nullptr if the queue is empty.
  // This is only used by shell testing functions.
  JSObject* maybeFront() const;

 private:
  using Queue = js::TraceableFifo<JSObject*, 0, SystemAllocPolicy>;

  JS::PersistentRooted<Queue> queue;

  // True if we are in the midst of draining jobs from this queue. We use this
  // to avoid re-entry (nested calls simply return immediately).
  bool draining_;

  // True if we've been asked to interrupt draining jobs. Set by interrupt().
  bool interrupted_;

  class SavedQueue;
  js::UniquePtr<JobQueue::SavedJobQueue> saveJobQueue(JSContext*) override;
};

class AutoLockScriptData;

void ReportOverRecursed(JSContext* cx, unsigned errorNumber);

/* Thread Local Storage slot for storing the context for a thread. */
extern MOZ_THREAD_LOCAL(JSContext*) TlsContext;

enum class ContextKind {
  Uninitialized,

  // Context for the main thread of a JSRuntime.
  MainThread,

  // Context for a helper thread.
  HelperThread
};

#ifdef DEBUG
JSContext* MaybeGetJSContext();
bool CurrentThreadIsParseThread();
#endif

enum class InterruptReason : uint32_t {
  GC = 1 << 0,
  AttachIonCompilations = 1 << 1,
  CallbackUrgent = 1 << 2,
  CallbackCanWait = 1 << 3,
};

} /* namespace js */

/*
 * A JSContext encapsulates the thread local state used when using the JS
 * runtime.
 */
struct JS_PUBLIC_API JSContext : public JS::RootingContext,
                                 public js::MallocProvider<JSContext> {
  JSContext(JSRuntime* runtime, const JS::ContextOptions& options);
  ~JSContext();

  bool init(js::ContextKind kind);

 private:
  js::UnprotectedData<JSRuntime*> runtime_;
  js::WriteOnceData<js::ContextKind> kind_;

  friend class js::gc::AutoSuppressNurseryCellAlloc;
  js::ContextData<size_t> nurserySuppressions_;

  js::ContextData<JS::ContextOptions> options_;

  // Free lists for allocating in the current zone.
  js::ContextData<js::gc::FreeLists*> freeLists_;

  // This is reset each time we switch zone, then added to the variable in the
  // zone when we switch away from it.  This would be a js::ThreadData but we
  // need to take its address.
  uint32_t allocsThisZoneSinceMinorGC_;

  // Free lists for parallel allocation in the atoms zone on helper threads.
  js::ContextData<js::gc::FreeLists*> atomsZoneFreeLists_;

  js::ContextData<JSFreeOp> defaultFreeOp_;

  // Thread that the JSContext is currently running on, if in use.
  js::ThreadId currentThread_;

  js::ParseTask* parseTask_;

  // When a helper thread is using a context, it may need to periodically
  // free unused memory.
  mozilla::Atomic<bool, mozilla::ReleaseAcquire> freeUnusedMemory;

  // Are we currently timing execution? This flag ensures that we do not
  // double-count execution time in reentrant situations.
  js::ContextData<bool> measuringExecutionTime_;

 public:
  // This is used by helper threads to change the runtime their context is
  // currently operating on.
  void setRuntime(JSRuntime* rt);

  void setHelperThread(const js::AutoLockHelperThreadState& locked);
  void clearHelperThread(const js::AutoLockHelperThreadState& locked);

  bool contextAvailable(js::AutoLockHelperThreadState& locked) {
    MOZ_ASSERT(kind_ == js::ContextKind::HelperThread);
    return currentThread_ == js::ThreadId();
  }

  void setFreeUnusedMemory(bool shouldFree) { freeUnusedMemory = shouldFree; }

  bool shouldFreeUnusedMemory() const {
    return kind_ == js::ContextKind::HelperThread && freeUnusedMemory;
  }

  bool isMeasuringExecutionTime() const { return measuringExecutionTime_; }
  void setIsMeasuringExecutionTime(bool value) {
    measuringExecutionTime_ = value;
  }

#ifdef DEBUG
  bool isInitialized() const { return kind_ != js::ContextKind::Uninitialized; }
#endif

  bool isMainThreadContext() const {
    return kind_ == js::ContextKind::MainThread;
  }

  bool isHelperThreadContext() const {
    return kind_ == js::ContextKind::HelperThread;
  }

  js::gc::FreeLists& freeLists() {
    MOZ_ASSERT(freeLists_);
    return *freeLists_;
  }

  js::gc::FreeLists& atomsZoneFreeLists() {
    MOZ_ASSERT(atomsZoneFreeLists_);
    return *atomsZoneFreeLists_;
  }

  template <typename T>
  bool isInsideCurrentZone(T thing) const {
    return thing->zoneFromAnyThread() == zone_;
  }

  template <typename T>
  inline bool isInsideCurrentCompartment(T thing) const {
    return thing->compartment() == compartment();
  }

  void* onOutOfMemory(js::AllocFunction allocFunc, arena_id_t arena,
                      size_t nbytes, void* reallocPtr = nullptr) {
    if (isHelperThreadContext()) {
      addPendingOutOfMemory();
      return nullptr;
    }
    return runtime_->onOutOfMemory(allocFunc, arena, nbytes, reallocPtr, this);
  }

  /* Clear the pending exception (if any) due to OOM. */
  void recoverFromOutOfMemory();

  void reportAllocationOverflow() { js::ReportAllocationOverflow(this); }

  void noteTenuredAlloc() { allocsThisZoneSinceMinorGC_++; }

  uint32_t* addressOfTenuredAllocCount() {
    return &allocsThisZoneSinceMinorGC_;
  }

  uint32_t getAndResetAllocsThisZoneSinceMinorGC() {
    uint32_t allocs = allocsThisZoneSinceMinorGC_;
    allocsThisZoneSinceMinorGC_ = 0;
    return allocs;
  }

  // Accessors for immutable runtime data.
  JSAtomState& names() { return *runtime_->commonNames; }
  js::StaticStrings& staticStrings() { return *runtime_->staticStrings; }
  js::SharedImmutableStringsCache& sharedImmutableStrings() {
    return runtime_->sharedImmutableStrings();
  }
  bool permanentAtomsPopulated() { return runtime_->permanentAtomsPopulated(); }
  const js::FrozenAtomSet& permanentAtoms() {
    return *runtime_->permanentAtoms();
  }
  js::WellKnownSymbols& wellKnownSymbols() {
    return *runtime_->wellKnownSymbols;
  }
  js::PropertyName* emptyString() { return runtime_->emptyString; }
  JSFreeOp* defaultFreeOp() { return &defaultFreeOp_.ref(); }
  uintptr_t stackLimit(JS::StackKind kind) { return nativeStackLimit[kind]; }
  uintptr_t stackLimitForJitCode(JS::StackKind kind);
  size_t gcSystemPageSize() { return js::gc::SystemPageSize(); }

  /*
   * "Entering" a realm changes cx->realm (which changes cx->global). Note
   * that this does not push an Activation so it's possible for the caller's
   * realm to be != cx->realm(). This is not a problem since, in general, most
   * places in the VM cannot know that they were called from script (e.g.,
   * they may have been called through the JSAPI via JS_CallFunction) and thus
   * cannot expect there is a scripted caller.
   *
   * Realms should be entered/left in a LIFO fasion. To enter a realm, code
   * should prefer using AutoRealm over JS::EnterRealm/JS::LeaveRealm.
   *
   * Also note that the JIT can enter (same-compartment) realms without going
   * through these methods - it will update cx->realm_ directly.
   */
 private:
  inline void setRealm(JS::Realm* realm);
  inline void enterRealm(JS::Realm* realm);

  inline void enterAtomsZone();
  inline void leaveAtomsZone(JS::Realm* oldRealm);
  enum IsAtomsZone { AtomsZone, NotAtomsZone };
  inline void setZone(js::Zone* zone, IsAtomsZone isAtomsZone);

  friend class js::AutoAllocInAtomsZone;
  friend class js::AutoMaybeLeaveAtomsZone;
  friend class js::AutoRealm;

 public:
  inline void enterRealmOf(JSObject* target);
  inline void enterRealmOf(JSScript* target);
  inline void enterRealmOf(js::Shape* target);
  inline void enterNullRealm();

  inline void setRealmForJitExceptionHandler(JS::Realm* realm);

  inline void leaveRealm(JS::Realm* oldRealm);

  void setParseTask(js::ParseTask* parseTask) { parseTask_ = parseTask; }
  js::ParseTask* parseTask() const { return parseTask_; }

  bool isNurseryAllocSuppressed() const { return nurserySuppressions_; }

  // Threads may freely access any data in their realm, compartment and zone.
  JS::Compartment* compartment() const {
    return realm_ ? JS::GetCompartmentForRealm(realm_) : nullptr;
  }

  JS::Realm* realm() const { return realm_; }

#ifdef DEBUG
  bool inAtomsZone() const;
#endif

  JS::Zone* zone() const {
    MOZ_ASSERT_IF(!realm() && zone_, inAtomsZone());
    MOZ_ASSERT_IF(realm(), js::GetRealmZone(realm()) == zone_);
    return zoneRaw();
  }

  // For use when the context's zone is being read by another thread and the
  // compartment and zone pointers might not be in sync.
  JS::Zone* zoneRaw() const { return zone_; }

  // For JIT use.
  static size_t offsetOfZone() { return offsetof(JSContext, zone_); }

  // Current global. This is only safe to use within the scope of the
  // AutoRealm from which it's called.
  inline js::Handle<js::GlobalObject*> global() const;

  js::AtomsTable& atoms() { return runtime_->atoms(); }

  const JS::Zone* atomsZone(const js::AutoAccessAtomsZone& access) {
    return runtime_->atomsZone(access);
  }

  js::SymbolRegistry& symbolRegistry() { return runtime_->symbolRegistry(); }

  // Methods to access runtime data that must be protected by locks.
  js::SharedImmutableScriptDataTable& scriptDataTable(
      js::AutoLockScriptData& lock) {
    return runtime_->scriptDataTable(lock);
  }

  // Methods to access other runtime data that checks locking internally.
  js::gc::AtomMarkingRuntime& atomMarking() { return runtime_->gc.atomMarking; }
  void markAtom(JSAtom* atom) { atomMarking().markAtom(this, atom); }
  void markAtom(JS::Symbol* symbol) { atomMarking().markAtom(this, symbol); }
  void markId(jsid id) { atomMarking().markId(this, id); }
  void markAtomValue(const js::Value& value) {
    atomMarking().markAtomValue(this, value);
  }

  // Methods specific to any HelperThread for the context.
  bool addPendingCompileError(js::CompileError** err);
  void addPendingOverRecursed();
  void addPendingOutOfMemory();

  bool isCompileErrorPending() const;

  JSRuntime* runtime() { return runtime_; }
  const JSRuntime* runtime() const { return runtime_; }

  static size_t offsetOfRealm() { return offsetof(JSContext, realm_); }

  friend class JS::AutoSaveExceptionState;
  friend class js::jit::DebugModeOSRVolatileJitFrameIter;
  friend void js::ReportOverRecursed(JSContext*, unsigned errorNumber);

 public:
  inline JS::Result<> boolToResult(bool ok);

  /**
   * Intentionally awkward signpost method that is stationed on the
   * boundary between Result-using and non-Result-using code.
   */
  template <typename V, typename E>
  bool resultToBool(const JS::Result<V, E>& result) {
    return result.isOk();
  }

  template <typename V, typename E>
  V* resultToPtr(JS::Result<V*, E>& result) {
    return result.isOk() ? result.unwrap() : nullptr;
  }

  mozilla::GenericErrorResult<JS::OOM> alreadyReportedOOM();
  mozilla::GenericErrorResult<JS::Error> alreadyReportedError();

  /*
   * Points to the most recent JitActivation pushed on the thread.
   * See JitActivation constructor in vm/Stack.cpp
   */
  js::ContextData<js::jit::JitActivation*> jitActivation;

  // Shim for V8 interfaces used by irregexp code
  js::ContextData<js::irregexp::Isolate*> isolate;

  /*
   * Points to the most recent activation running on the thread.
   * See Activation comment in vm/Stack.h.
   */
  js::ContextData<js::Activation*> activation_;

  /*
   * Points to the most recent profiling activation running on the
   * thread.
   */
  js::Activation* volatile profilingActivation_;

 public:
  js::Activation* activation() const { return activation_; }
  static size_t offsetOfActivation() {
    return offsetof(JSContext, activation_);
  }

  js::Activation* profilingActivation() const { return profilingActivation_; }
  static size_t offsetOfProfilingActivation() {
    return offsetof(JSContext, profilingActivation_);
  }

  static size_t offsetOfJitActivation() {
    return offsetof(JSContext, jitActivation);
  }

#ifdef DEBUG
  static size_t offsetOfInUnsafeCallWithABI() {
    return offsetof(JSContext, inUnsafeCallWithABI);
  }
#endif

  static size_t offsetOfInlinedICScript() {
    return offsetof(JSContext, inlinedICScript_);
  }

 public:
  js::InterpreterStack& interpreterStack() {
    return runtime()->interpreterStack();
  }

 private:
  // Base address of the native stack for the current thread.
  mozilla::Maybe<uintptr_t> nativeStackBase_;

 public:
  uintptr_t nativeStackBase() const { return *nativeStackBase_; }

 public:
  /* If non-null, report JavaScript entry points to this monitor. */
  js::ContextData<JS::dbg::AutoEntryMonitor*> entryMonitor;

  /*
   * Stack of debuggers that currently disallow debuggee execution.
   *
   * When we check for NX we are inside the debuggee compartment, and thus a
   * stack of Debuggers that have prevented execution need to be tracked to
   * enter the correct Debugger compartment to report the error.
   */
  js::ContextData<js::EnterDebuggeeNoExecute*> noExecuteDebuggerTop;

#ifdef DEBUG
  js::ContextData<uint32_t> inUnsafeCallWithABI;
  js::ContextData<bool> hasAutoUnsafeCallWithABI;
#endif

#ifdef JS_SIMULATOR
 private:
  js::ContextData<js::jit::Simulator*> simulator_;

 public:
  js::jit::Simulator* simulator() const;
  uintptr_t* addressOfSimulatorStackLimit();
#endif

#ifdef JS_TRACE_LOGGING
  js::UnprotectedData<js::TraceLoggerThread*> traceLogger;
#endif

 public:
  // State used by util/DoubleToString.cpp.
  js::ContextData<DtoaState*> dtoaState;

  /*
   * When this flag is non-zero, any attempt to GC will be skipped. See the
   * AutoSuppressGC class for for details.
   */
  js::ContextData<int32_t> suppressGC;

  // clang-format off
  enum class GCUse {
    // This thread is not running in the garbage collector.
    None,

    // This thread is currently marking GC things. This thread could be the main
    // thread or a helper thread doing sweep-marking.
    Marking,

    // This thread is currently sweeping GC things. This thread could be the
    // main thread or a helper thread while the main thread is running the
    // mutator.
    Sweeping,

    // Whether this thread is currently finalizing GC things. This thread could
    // be the main thread or a helper thread doing finalization while the main
    // thread is running the mutator.
    Finalizing
  };
  // clang-format on

#ifdef DEBUG
  // Which part of the garbage collector this context is running at the moment.
  js::ContextData<GCUse> gcUse;

  // The specific zone currently being swept, if any.
  js::ContextData<JS::Zone*> gcSweepZone;

  // Whether this thread is currently manipulating possibly-gray GC things.
  js::ContextData<size_t> isTouchingGrayThings;

  js::ContextData<size_t> noNurseryAllocationCheck;

  /*
   * If this is 0, all cross-compartment proxies must be registered in the
   * wrapper map. This checking must be disabled temporarily while creating
   * new wrappers. When non-zero, this records the recursion depth of wrapper
   * creation.
   */
  js::ContextData<uintptr_t> disableStrictProxyCheckingCount;

  bool isNurseryAllocAllowed() { return noNurseryAllocationCheck == 0; }
  void disallowNurseryAlloc() { ++noNurseryAllocationCheck; }
  void allowNurseryAlloc() {
    MOZ_ASSERT(!isNurseryAllocAllowed());
    --noNurseryAllocationCheck;
  }

  bool isStrictProxyCheckingEnabled() {
    return disableStrictProxyCheckingCount == 0;
  }
  void disableStrictProxyChecking() { ++disableStrictProxyCheckingCount; }
  void enableStrictProxyChecking() {
    MOZ_ASSERT(disableStrictProxyCheckingCount > 0);
    --disableStrictProxyCheckingCount;
  }
#endif

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
  // We are currently running a simulated OOM test.
  js::ContextData<bool> runningOOMTest;
#endif

#ifdef DEBUG
  js::ContextData<bool> disableCompartmentCheckTracer;
#endif

  /*
   * Some regions of code are hard for the static rooting hazard analysis to
   * understand. In those cases, we trade the static analysis for a dynamic
   * analysis. When this is non-zero, we should assert if we trigger, or
   * might trigger, a GC.
   */
  js::ContextData<int> inUnsafeRegion;

  // Count of AutoDisableGenerationalGC instances on the thread's stack.
  js::ContextData<unsigned> generationalDisabled;

  // Some code cannot tolerate compacting GC so it can be disabled temporarily
  // with AutoDisableCompactingGC which uses this counter.
  js::ContextData<unsigned> compactingDisabledCount;

  bool canCollectAtoms() const {
    // TODO: We may be able to improve this by collecting if
    // !isOffThreadParseRunning() (bug 1468422).
    return !runtime()->hasHelperThreadZones();
  }

 private:
  // Pools used for recycling name maps and vectors when parsing and
  // emitting bytecode. Purged on GC when there are no active script
  // compilations.
  js::ContextData<js::frontend::NameCollectionPool> frontendCollectionPool_;

 public:
  js::frontend::NameCollectionPool& frontendCollectionPool() {
    return frontendCollectionPool_.ref();
  }

  void verifyIsSafeToGC() {
    MOZ_DIAGNOSTIC_ASSERT(!inUnsafeRegion,
                          "[AutoAssertNoGC] possible GC in GC-unsafe region");
  }

  /* Whether sampling should be enabled or not. */
 private:
  mozilla::Atomic<bool, mozilla::SequentiallyConsistent>
      suppressProfilerSampling;

 public:
  bool isProfilerSamplingEnabled() const { return !suppressProfilerSampling; }
  void disableProfilerSampling() { suppressProfilerSampling = true; }
  void enableProfilerSampling() { suppressProfilerSampling = false; }

 private:
  js::wasm::Context wasm_;

 public:
  js::wasm::Context& wasm() { return wasm_; }

  /* Temporary arena pool used while compiling and decompiling. */
  static const size_t TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE = 4 * 1024;

 private:
  js::ContextData<js::LifoAlloc> tempLifoAlloc_;

 public:
  js::LifoAlloc& tempLifoAlloc() { return tempLifoAlloc_.ref(); }
  const js::LifoAlloc& tempLifoAlloc() const { return tempLifoAlloc_.ref(); }
  js::LifoAlloc& tempLifoAllocNoCheck() { return tempLifoAlloc_.refNoCheck(); }

  js::ContextData<uint32_t> debuggerMutations;

  // Cache for jit::GetPcScript().
  js::ContextData<js::UniquePtr<js::jit::PcScriptCache>> ionPcScriptCache;

 private:
  /* Exception state -- the exception member is a GC root by definition. */
  js::ContextData<bool> throwing; /* is there a pending exception? */
  js::ContextData<JS::PersistentRooted<JS::Value>>
      unwrappedException_; /* most-recently-thrown exception */
  js::ContextData<JS::PersistentRooted<js::SavedFrame*>>
      unwrappedExceptionStack_; /* stack when the exception was thrown */

  JS::Value& unwrappedException() {
    if (!unwrappedException_.ref().initialized()) {
      unwrappedException_.ref().init(this);
    }
    return unwrappedException_.ref().get();
  }

  js::SavedFrame*& unwrappedExceptionStack() {
    if (!unwrappedExceptionStack_.ref().initialized()) {
      unwrappedExceptionStack_.ref().init(this);
    }
    return unwrappedExceptionStack_.ref().get();
  }

  // True if the exception currently being thrown is by result of
  // ReportOverRecursed. See Debugger::slowPathOnExceptionUnwind.
  js::ContextData<bool> overRecursed_;

#ifdef DEBUG
  // True if this context has ever called ReportOverRecursed.
  js::ContextData<bool> hadOverRecursed_;

 public:
  bool hadNondeterministicException() const {
    return hadOverRecursed_ || runtime()->hadOutOfMemory;
  }
#endif

 private:
  // True if propagating a forced return from an interrupt handler during
  // debug mode.
  js::ContextData<bool> propagatingForcedReturn_;

 public:
  js::ContextData<int32_t> reportGranularity; /* see vm/Probes.h */

  js::ContextData<js::AutoResolving*> resolvingList;

#ifdef DEBUG
  js::ContextData<js::AutoEnterPolicy*> enteredPolicy;
#endif

  /* True if generating an error, to prevent runaway recursion. */
  js::ContextData<bool> generatingError;

 private:
  /* State for object and array toSource conversion. */
  js::ContextData<js::AutoCycleDetector::Vector> cycleDetectorVector_;

 public:
  js::AutoCycleDetector::Vector& cycleDetectorVector() {
    return cycleDetectorVector_.ref();
  }
  const js::AutoCycleDetector::Vector& cycleDetectorVector() const {
    return cycleDetectorVector_.ref();
  }

  /* Client opaque pointer. */
  js::UnprotectedData<void*> data;

  void initJitStackLimit();
  void resetJitStackLimit();

 public:
  JS::ContextOptions& options() { return options_.ref(); }

  bool runtimeMatches(JSRuntime* rt) const { return runtime_ == rt; }

 private:
  /*
   * Youngest frame of a saved stack that will be picked up as an async stack
   * by any new Activation, and is nullptr when no async stack should be used.
   *
   * The JS::AutoSetAsyncStackForNewCalls class can be used to set this.
   *
   * New activations will reset this to nullptr on construction after getting
   * the current value, and will restore the previous value on destruction.
   */
  js::ContextData<JS::PersistentRooted<js::SavedFrame*>>
      asyncStackForNewActivations_;

 public:
  js::SavedFrame*& asyncStackForNewActivations() {
    if (!asyncStackForNewActivations_.ref().initialized()) {
      asyncStackForNewActivations_.ref().init(this);
    }
    return asyncStackForNewActivations_.ref().get();
  }

  /*
   * Value of asyncCause to be attached to asyncStackForNewActivations.
   */
  js::ContextData<const char*> asyncCauseForNewActivations;

  /*
   * True if the async call was explicitly requested, e.g. via
   * callFunctionWithAsyncStack.
   */
  js::ContextData<bool> asyncCallIsExplicit;

  bool currentlyRunningInInterpreter() const {
    return activation()->isInterpreter();
  }
  bool currentlyRunningInJit() const { return activation()->isJit(); }
  js::InterpreterFrame* interpreterFrame() const {
    return activation()->asInterpreter()->current();
  }
  js::InterpreterRegs& interpreterRegs() const {
    return activation()->asInterpreter()->regs();
  }

  /*
   * Get the topmost script and optional pc on the stack. By default, this
   * function only returns a JSScript in the current realm, returning nullptr
   * if the current script is in a different realm. This behavior can be
   * overridden by passing AllowCrossRealm::Allow.
   */
  enum class AllowCrossRealm { DontAllow = false, Allow = true };
  inline JSScript* currentScript(
      jsbytecode** pc = nullptr,
      AllowCrossRealm allowCrossRealm = AllowCrossRealm::DontAllow) const;

  inline js::Nursery& nursery();
  inline void minorGC(JS::GCReason reason);

 public:
  bool isExceptionPending() const { return throwing; }

  [[nodiscard]] bool getPendingException(JS::MutableHandleValue rval);

  js::SavedFrame* getPendingExceptionStack();

  bool isThrowingOutOfMemory();
  bool isThrowingDebuggeeWouldRun();
  bool isClosingGenerator();

  void setPendingException(JS::HandleValue v, js::HandleSavedFrame stack);
  void setPendingExceptionAndCaptureStack(JS::HandleValue v);

  void clearPendingException() {
    throwing = false;
    overRecursed_ = false;
    unwrappedException().setUndefined();
    unwrappedExceptionStack() = nullptr;
  }

  bool isThrowingOverRecursed() const { return throwing && overRecursed_; }
  bool isPropagatingForcedReturn() const { return propagatingForcedReturn_; }
  void setPropagatingForcedReturn() { propagatingForcedReturn_ = true; }
  void clearPropagatingForcedReturn() { propagatingForcedReturn_ = false; }

  /*
   * See JS_SetTrustedPrincipals in jsapi.h.
   * Note: !cx->realm() is treated as trusted.
   */
  inline bool runningWithTrustedPrincipals();

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  void trace(JSTracer* trc);

  inline js::RuntimeCaches& caches();

 public:
  using InterruptCallbackVector =
      js::Vector<JSInterruptCallback, 2, js::SystemAllocPolicy>;

 private:
  js::ContextData<InterruptCallbackVector> interruptCallbacks_;

 public:
  InterruptCallbackVector& interruptCallbacks() {
    return interruptCallbacks_.ref();
  }

  js::ContextData<bool> interruptCallbackDisabled;

  // Bitfield storing InterruptReason values.
  mozilla::Atomic<uint32_t, mozilla::Relaxed> interruptBits_;

  // Any thread can call requestInterrupt() to request that this thread
  // stop running. To stop this thread, requestInterrupt sets two fields:
  // interruptBits_ (a bitset of InterruptReasons) and jitStackLimit_ (set to
  // UINTPTR_MAX). The JS engine must continually poll one of these fields
  // and call handleInterrupt if either field has the interrupt value.
  //
  // The point of setting jitStackLimit_ to UINTPTR_MAX is that JIT code
  // already needs to guard on jitStackLimit_ in every function prologue to
  // avoid stack overflow, so we avoid a second branch on interruptBits_ by
  // setting jitStackLimit_ to a value that is guaranteed to fail the guard.)
  //
  // Note that the writes to interruptBits_ and jitStackLimit_ use a Relaxed
  // Atomic so, while the writes are guaranteed to eventually be visible to
  // this thread, it can happen in any order. handleInterrupt calls the
  // interrupt callback if either is set, so it really doesn't matter as long
  // as the JS engine is continually polling at least one field. In corner
  // cases, this relaxed ordering could lead to an interrupt handler being
  // called twice in succession after a single requestInterrupt call, but
  // that's fine.
  void requestInterrupt(js::InterruptReason reason);
  bool handleInterrupt();

  MOZ_ALWAYS_INLINE bool hasAnyPendingInterrupt() const {
    static_assert(sizeof(interruptBits_) == sizeof(uint32_t),
                  "Assumed by JIT callers");
    return interruptBits_ != 0;
  }
  bool hasPendingInterrupt(js::InterruptReason reason) const {
    return interruptBits_ & uint32_t(reason);
  }

  // For JIT use. Points to the inlined ICScript for a baseline script
  // being invoked as part of a trial inlining.  Contains nullptr at
  // all times except for the brief moment between being set in the
  // caller and read in the callee's prologue.
  js::ContextData<js::jit::ICScript*> inlinedICScript_;

 public:
  void* addressOfInterruptBits() { return &interruptBits_; }
  void* addressOfJitStackLimit() { return &jitStackLimit; }
  void* addressOfJitStackLimitNoInterrupt() {
    return &jitStackLimitNoInterrupt;
  }
  void* addressOfZone() { return &zone_; }

  const void* addressOfRealm() const { return &realm_; }

  void* addressOfInlinedICScript() { return &inlinedICScript_; }

  // Futex state, used by Atomics.wait() and Atomics.wake() on the Atomics
  // object.
  js::FutexThread fx;

  mozilla::Atomic<uintptr_t, mozilla::Relaxed> jitStackLimit;

  // Like jitStackLimit, but not reset to trigger interrupts.
  js::ContextData<uintptr_t> jitStackLimitNoInterrupt;

  // Queue of pending jobs as described in ES2016 section 8.4.
  //
  // This is a non-owning pointer to either:
  // - a JobQueue implementation the embedding provided by calling
  //   JS::SetJobQueue, owned by the embedding, or
  // - our internal JobQueue implementation, established by calling
  //   js::UseInternalJobQueues, owned by JSContext::internalJobQueue below.
  js::ContextData<JS::JobQueue*> jobQueue;

  // If the embedding has called js::UseInternalJobQueues, this is the owning
  // pointer to our internal JobQueue implementation, which JSContext::jobQueue
  // borrows.
  js::ContextData<js::UniquePtr<js::InternalJobQueue>> internalJobQueue;

  // True if jobQueue is empty, or we are running the last job in the queue.
  // Such conditions permit optimizations around `await` expressions.
  js::ContextData<bool> canSkipEnqueuingJobs;

  js::ContextData<JS::PromiseRejectionTrackerCallback>
      promiseRejectionTrackerCallback;
  js::ContextData<void*> promiseRejectionTrackerCallbackData;

  JSObject* getIncumbentGlobal(JSContext* cx);
  bool enqueuePromiseJob(JSContext* cx, js::HandleFunction job,
                         js::HandleObject promise,
                         js::HandleObject incumbentGlobal);
  void addUnhandledRejectedPromise(JSContext* cx, js::HandleObject promise);
  void removeUnhandledRejectedPromise(JSContext* cx, js::HandleObject promise);

 private:
  template <class... Args>
  inline void checkImpl(const Args&... args);

  bool contextChecksEnabled() const {
    // Don't perform these checks when called from a finalizer. The checking
    // depends on other objects not having been swept yet.
    return !RuntimeHeapIsCollecting(runtime()->heapState());
  }

 public:
  // Assert the arguments are in this context's realm (for scripts),
  // compartment (for objects) or zone (for strings, symbols).
  template <class... Args>
  inline void check(const Args&... args);
  template <class... Args>
  inline void releaseCheck(const Args&... args);
  template <class... Args>
  MOZ_ALWAYS_INLINE void debugOnlyCheck(const Args&... args);

#ifdef JS_STRUCTURED_SPEW
 private:
  // Spewer for this thread
  js::UnprotectedData<js::StructuredSpewer> structuredSpewer_;

 public:
  js::StructuredSpewer& spewer() { return structuredSpewer_.ref(); }
#endif

  // During debugger evaluations which need to observe native calls, JITs are
  // completely disabled. This flag indicates whether we are in this state, and
  // the debugger which initiated the evaluation. This debugger has other
  // references on the stack and does not need to be traced.
  js::ContextData<js::Debugger*> insideDebuggerEvaluationWithOnNativeCallHook;

}; /* struct JSContext */

inline JS::Result<> JSContext::boolToResult(bool ok) {
  if (MOZ_LIKELY(ok)) {
    MOZ_ASSERT(!isExceptionPending());
    MOZ_ASSERT(!isPropagatingForcedReturn());
    return JS::Ok();
  }
  return JS::Result<>(JS::Error());
}

inline JSContext* JSRuntime::mainContextFromOwnThread() {
  MOZ_ASSERT(mainContextFromAnyThread() == js::TlsContext.get());
  return mainContextFromAnyThread();
}

namespace js {

struct MOZ_RAII AutoResolving {
 public:
  enum Kind { LOOKUP, WATCH };

  AutoResolving(JSContext* cx, HandleObject obj, HandleId id,
                Kind kind = LOOKUP)
      : context(cx), object(obj), id(id), kind(kind), link(cx->resolvingList) {
    MOZ_ASSERT(obj);
    cx->resolvingList = this;
  }

  ~AutoResolving() {
    MOZ_ASSERT(context->resolvingList == this);
    context->resolvingList = link;
  }

  bool alreadyStarted() const { return link && alreadyStartedSlow(); }

 private:
  bool alreadyStartedSlow() const;

  JSContext* const context;
  HandleObject object;
  HandleId id;
  Kind const kind;
  AutoResolving* const link;
};

/*
 * Create and destroy functions for JSContext, which is manually allocated
 * and exclusively owned.
 */
extern JSContext* NewContext(uint32_t maxBytes, JSRuntime* parentRuntime);

extern void DestroyContext(JSContext* cx);

/* |callee| requires a usage string provided by JS_DefineFunctionsWithHelp. */
extern void ReportUsageErrorASCII(JSContext* cx, HandleObject callee,
                                  const char* msg);

extern void ReportIsNotDefined(JSContext* cx, HandlePropertyName name);

extern void ReportIsNotDefined(JSContext* cx, HandleId id);

/*
 * Report an attempt to access the property of a null or undefined value (v).
 */
extern void ReportIsNullOrUndefinedForPropertyAccess(JSContext* cx,
                                                     HandleValue v, int vIndex);
extern void ReportIsNullOrUndefinedForPropertyAccess(JSContext* cx,
                                                     HandleValue v, int vIndex,
                                                     HandleId key);

/*
 * Report error using js::DecompileValueGenerator(cx, spindex, v, fallback) as
 * the first argument for the error message.
 */
extern bool ReportValueError(JSContext* cx, const unsigned errorNumber,
                             int spindex, HandleValue v, HandleString fallback,
                             const char* arg1 = nullptr,
                             const char* arg2 = nullptr);

JSObject* CreateErrorNotesArray(JSContext* cx, JSErrorReport* report);

/************************************************************************/

/*
 * Encapsulates an external array of values and adds a trace method, for use in
 * Rooted.
 */
class MOZ_STACK_CLASS ExternalValueArray {
 public:
  ExternalValueArray(size_t len, Value* vec) : array_(vec), length_(len) {}

  Value* begin() { return array_; }
  size_t length() { return length_; }

  void trace(JSTracer* trc);

 private:
  Value* array_;
  size_t length_;
};

/* RootedExternalValueArray roots an external array of Values. */
class MOZ_RAII RootedExternalValueArray
    : public JS::Rooted<ExternalValueArray> {
 public:
  RootedExternalValueArray(JSContext* cx, size_t len, Value* vec)
      : JS::Rooted<ExternalValueArray>(cx, ExternalValueArray(len, vec)) {}

 private:
};

class AutoAssertNoPendingException {
#ifdef DEBUG
  JSContext* cx_;

 public:
  explicit AutoAssertNoPendingException(JSContext* cxArg) : cx_(cxArg) {
    MOZ_ASSERT(!JS_IsExceptionPending(cx_));
  }

  ~AutoAssertNoPendingException() { MOZ_ASSERT(!JS_IsExceptionPending(cx_)); }
#else
 public:
  explicit AutoAssertNoPendingException(JSContext* cxArg) {}
#endif
};

class MOZ_RAII AutoLockScriptData {
  JSRuntime* runtime;

 public:
  explicit AutoLockScriptData(JSRuntime* rt) {
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt) ||
               CurrentThreadIsParseThread());
    runtime = rt;
    if (runtime->hasParseTasks()) {
      runtime->scriptDataLock.lock();
    } else {
      MOZ_ASSERT(!runtime->activeThreadHasScriptDataAccess);
#ifdef DEBUG
      runtime->activeThreadHasScriptDataAccess = true;
#endif
    }
  }
  ~AutoLockScriptData() {
    if (runtime->hasParseTasks()) {
      runtime->scriptDataLock.unlock();
    } else {
      MOZ_ASSERT(runtime->activeThreadHasScriptDataAccess);
#ifdef DEBUG
      runtime->activeThreadHasScriptDataAccess = false;
#endif
    }
  }
};

// A token used to prove you can safely access the atoms zone. This zone is
// accessed by the main thread and by off-thread parsing. There are two
// situations in which it is safe:
//
//  - the current thread holds all atoms table locks (off-thread parsing may be
//    running and must also take one of these locks for access)
//
//  - the GC is running and is collecting the atoms zone (this cannot be started
//    while off-thread parsing is happening)
class MOZ_STACK_CLASS AutoAccessAtomsZone {
 public:
  MOZ_IMPLICIT AutoAccessAtomsZone(const AutoLockAllAtoms& lock) {}
  MOZ_IMPLICIT AutoAccessAtomsZone(
      const gc::AutoCheckCanAccessAtomsDuringGC& canAccess) {}
};

class MOZ_RAII AutoNoteDebuggerEvaluationWithOnNativeCallHook {
  JSContext* cx;
  Debugger* oldValue;

 public:
  AutoNoteDebuggerEvaluationWithOnNativeCallHook(JSContext* cx, Debugger* dbg)
      : cx(cx), oldValue(cx->insideDebuggerEvaluationWithOnNativeCallHook) {
    cx->insideDebuggerEvaluationWithOnNativeCallHook = dbg;
  }

  ~AutoNoteDebuggerEvaluationWithOnNativeCallHook() {
    cx->insideDebuggerEvaluationWithOnNativeCallHook = oldValue;
  }
};

enum UnsafeABIStrictness {
  NoExceptions,
  AllowPendingExceptions,
  AllowThrownExceptions
};

// Should be used in functions called directly from JIT code (with
// masm.callWithABI) to assert invariants in debug builds.
// In debug mode, masm.callWithABI inserts code to verify that the
// callee function uses AutoUnsafeCallWithABI.
// While this object is live:
// 1. cx->hasAutoUnsafeCallWithABI must be true.
// 2. We can't GC.
// 3. Exceptions should not be pending/thrown.
//
// Note that #3 is a precaution, not a requirement. By default, we
// assert that the function is not called with a pending exception,
// and that it does not throw an exception itself.
class MOZ_RAII AutoUnsafeCallWithABI {
#ifdef DEBUG
  JSContext* cx_;
  bool nested_;
  bool checkForPendingException_;
#endif
  JS::AutoCheckCannotGC nogc;

 public:
#ifdef DEBUG
  explicit AutoUnsafeCallWithABI(
      UnsafeABIStrictness strictness = UnsafeABIStrictness::NoExceptions);
  ~AutoUnsafeCallWithABI();
#else
  explicit AutoUnsafeCallWithABI(
      UnsafeABIStrictness unused_ = UnsafeABIStrictness::NoExceptions) {}
#endif
};

namespace gc {

// Set/restore the performing GC flag for the current thread.
class MOZ_RAII AutoSetThreadIsPerformingGC {
  JSContext* cx;
  bool prev;

 public:
  AutoSetThreadIsPerformingGC()
      : cx(TlsContext.get()), prev(cx->defaultFreeOp()->isCollecting_) {
    cx->defaultFreeOp()->isCollecting_ = true;
  }

  ~AutoSetThreadIsPerformingGC() { cx->defaultFreeOp()->isCollecting_ = prev; }
};

struct MOZ_RAII AutoSetThreadGCUse {
 protected:
#ifndef DEBUG
  explicit AutoSetThreadGCUse(JSContext::GCUse use, Zone* sweepZone = nullptr) {
  }
#else
  explicit AutoSetThreadGCUse(JSContext::GCUse use, Zone* sweepZone = nullptr)
      : cx(TlsContext.get()), prevUse(cx->gcUse), prevZone(cx->gcSweepZone) {
    MOZ_ASSERT_IF(sweepZone, use == JSContext::GCUse::Sweeping);
    cx->gcUse = use;
    cx->gcSweepZone = sweepZone;
  }

  ~AutoSetThreadGCUse() {
    cx->gcUse = prevUse;
    cx->gcSweepZone = prevZone;
    MOZ_ASSERT_IF(cx->gcUse == JSContext::GCUse::None, !cx->gcSweepZone);
  }

 private:
  JSContext* cx;
  JSContext::GCUse prevUse;
  JS::Zone* prevZone;
#endif
};

// In debug builds, update the context state to indicate that the current thread
// is being used for GC marking.
struct MOZ_RAII AutoSetThreadIsMarking : public AutoSetThreadGCUse {
  explicit AutoSetThreadIsMarking()
      : AutoSetThreadGCUse(JSContext::GCUse::Marking) {}
};

// In debug builds, update the context state to indicate that the current thread
// is being used for GC sweeping.
struct MOZ_RAII AutoSetThreadIsSweeping : public AutoSetThreadGCUse {
  explicit AutoSetThreadIsSweeping(Zone* zone = nullptr)
      : AutoSetThreadGCUse(JSContext::GCUse::Sweeping, zone) {}
};

// In debug builds, update the context state to indicate that the current thread
// is being used for GC finalization.
struct MOZ_RAII AutoSetThreadIsFinalizing : public AutoSetThreadGCUse {
  explicit AutoSetThreadIsFinalizing()
      : AutoSetThreadGCUse(JSContext::GCUse::Finalizing) {}
};

// Note that this class does not suppress buffer allocation/reallocation in the
// nursery, only Cells themselves.
class MOZ_RAII AutoSuppressNurseryCellAlloc {
  JSContext* cx_;

 public:
  explicit AutoSuppressNurseryCellAlloc(JSContext* cx) : cx_(cx) {
    cx_->nurserySuppressions_++;
  }
  ~AutoSuppressNurseryCellAlloc() { cx_->nurserySuppressions_--; }
};

}  // namespace gc

} /* namespace js */

#define CHECK_THREAD(cx)                            \
  MOZ_ASSERT_IF(cx, !cx->isHelperThreadContext() && \
                        js::CurrentThreadCanAccessRuntime(cx->runtime()))

#endif /* vm_JSContext_h */
