/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS execution context. */

#ifndef vm_JSContext_h
#define vm_JSContext_h

#include "mozilla/BaseProfilerUtils.h"  // BaseProfilerThreadId
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"

#include "jstypes.h"  // JS_PUBLIC_API

#include "builtin/AtomicsObject.h"
#include "ds/TraceableFifo.h"
#include "frontend/NameCollections.h"
#include "gc/GCEnum.h"
#include "gc/Memory.h"
#include "irregexp/RegExpTypes.h"
#include "js/ContextOptions.h"  // JS::ContextOptions
#include "js/Exception.h"
#include "js/GCVector.h"
#include "js/Interrupt.h"
#include "js/Promise.h"
#include "js/Result.h"
#include "js/Stack.h"  // JS::NativeStackBase, JS::NativeStackLimit
#include "js/Utility.h"
#include "js/Vector.h"
#include "threading/ProtectedData.h"
#include "util/StructuredSpewer.h"
#include "vm/Activation.h"  // js::Activation
#include "vm/MallocProvider.h"
#include "vm/Runtime.h"
#include "wasm/WasmContext.h"

struct JS_PUBLIC_API JSContext;

struct DtoaState;

namespace js {

class AutoAllocInAtomsZone;
class AutoMaybeLeaveAtomsZone;
class AutoRealm;
struct PortableBaselineStack;

#ifdef MOZ_EXECUTION_TRACING
class ExecutionTracer;
#endif

namespace jit {
class ICScript;
class JitActivation;
class JitContext;
class DebugModeOSRVolatileJitFrameIter;
}  // namespace jit

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

class InternalJobQueue : public JS::JobQueue {
 public:
  explicit InternalJobQueue(JSContext* cx)
      : queue(cx, SystemAllocPolicy()), draining_(false), interrupted_(false) {}
  ~InternalJobQueue() = default;

  // JS::JobQueue methods.
  bool getHostDefinedData(JSContext* cx,
                          JS::MutableHandle<JSObject*> data) const override;

  bool enqueuePromiseJob(JSContext* cx, JS::HandleObject promise,
                         JS::HandleObject job, JS::HandleObject allocationSite,
                         JS::HandleObject hostDefinedData) override;
  void runJobs(JSContext* cx) override;
  bool empty() const override;
  bool isDrainingStopped() const override { return interrupted_; }

  // If we are currently in a call to runJobs(), make that call stop processing
  // jobs once the current one finishes, and return. If we are not currently in
  // a call to runJobs, make all future calls return immediately.
  void interrupt() { interrupted_ = true; }

  void uninterrupt() { interrupted_ = false; }

  // Return the front element of the queue, or nullptr if the queue is empty.
  // This is only used by shell testing functions.
  JSObject* maybeFront() const;

#ifdef DEBUG
  JSObject* copyJobs(JSContext* cx);
#endif

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

/* Thread Local Storage slot for storing the context for a thread. */
extern MOZ_THREAD_LOCAL(JSContext*) TlsContext;

#ifdef DEBUG
JSContext* MaybeGetJSContext();
#endif

enum class InterruptReason : uint32_t {
  MinorGC = 1 << 0,
  MajorGC = 1 << 1,
  AttachOffThreadCompilations = 1 << 2,
  CallbackUrgent = 1 << 3,
  CallbackCanWait = 1 << 4,
};

enum class ShouldCaptureStack { Maybe, Always };

} /* namespace js */

/*
 * A JSContext encapsulates the thread local state used when using the JS
 * runtime.
 */
struct JS_PUBLIC_API JSContext : public JS::RootingContext,
                                 public js::MallocProvider<JSContext> {
  JSContext(JSRuntime* runtime, const JS::ContextOptions& options);
  ~JSContext();

  bool init();

  static JSContext* from(JS::RootingContext* rcx) {
    return static_cast<JSContext*>(rcx);
  }

 private:
  js::UnprotectedData<JSRuntime*> runtime_;
#ifdef DEBUG
  js::WriteOnceData<bool> initialized_;
#endif

  js::ContextData<JS::ContextOptions> options_;

  // Are we currently timing execution? This flag ensures that we do not
  // double-count execution time in reentrant situations.
  js::ContextData<bool> measuringExecutionTime_;

  // This variable is used by the HelperThread scheduling to update the priority
  // of task based on whether JavaScript is being executed on the main thread.
  mozilla::Atomic<bool, mozilla::ReleaseAcquire> isExecuting_;

 public:
  // This is used by helper threads to change the runtime their context is
  // currently operating on.
  void setRuntime(JSRuntime* rt);

  bool isMeasuringExecutionTime() const { return measuringExecutionTime_; }
  void setIsMeasuringExecutionTime(bool value) {
    measuringExecutionTime_ = value;
  }

  // While JSContexts are meant to be used on a single thread, this reference is
  // meant to be shared to helper thread tasks. This is used by helper threads
  // to change the priority of tasks based on whether JavaScript is executed on
  // the main thread.
  const mozilla::Atomic<bool, mozilla::ReleaseAcquire>& isExecutingRef() const {
    return isExecuting_;
  }
  void setIsExecuting(bool value) { isExecuting_ = value; }

#ifdef DEBUG
  bool isInitialized() const { return initialized_; }
#endif

  template <typename T>
  bool isInsideCurrentZone(T thing) const {
    return thing->zoneFromAnyThread() == zone_;
  }

  template <typename T>
  inline bool isInsideCurrentCompartment(T thing) const {
    return thing->compartment() == compartment();
  }

  void onOutOfMemory();
  void* onOutOfMemory(js::AllocFunction allocFunc, arena_id_t arena,
                      size_t nbytes, void* reallocPtr = nullptr) {
    return runtime_->onOutOfMemory(allocFunc, arena, nbytes, reallocPtr, this);
  }

  void onOverRecursed();

  // Allocate a GC thing.
  template <typename T, js::AllowGC allowGC = js::CanGC, typename... Args>
  T* newCell(Args&&... args);

  /* Clear the pending exception (if any) due to OOM. */
  void recoverFromOutOfMemory();

  void reportAllocationOverflow();

  // Accessors for immutable runtime data.
  JSAtomState& names() { return *runtime_->commonNames; }
  js::StaticStrings& staticStrings() { return *runtime_->staticStrings; }
  bool permanentAtomsPopulated() { return runtime_->permanentAtomsPopulated(); }
  const js::FrozenAtomSet& permanentAtoms() {
    return *runtime_->permanentAtoms();
  }
  js::WellKnownSymbols& wellKnownSymbols() {
    return *runtime_->wellKnownSymbols;
  }
  js::PropertyName* emptyString() { return runtime_->emptyString; }
  JS::GCContext* gcContext() { return runtime_->gcContext(); }
  JS::StackKind stackKindForCurrentPrincipal();
  JS::NativeStackLimit stackLimitForCurrentPrincipal();
  JS::NativeStackLimit stackLimit(JS::StackKind kind) {
    return nativeStackLimit[kind];
  }
  JS::NativeStackLimit stackLimitForJitCode(JS::StackKind kind);
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
  inline void setZone(js::Zone* zone);

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
    return zone_;
  }

  // For JIT use.
  static size_t offsetOfZone() { return offsetof(JSContext, zone_); }

  // Current global. This is only safe to use within the scope of the
  // AutoRealm from which it's called.
  inline js::Handle<js::GlobalObject*> global() const;

  js::AtomsTable& atoms() { return runtime_->atoms(); }

  js::SymbolRegistry& symbolRegistry() { return runtime_->symbolRegistry(); }

  // Methods to access other runtime data that checks locking internally.
  js::gc::AtomMarkingRuntime& atomMarking() { return runtime_->gc.atomMarking; }
  void markAtom(JSAtom* atom) { atomMarking().markAtom(this, atom); }
  void markAtom(JS::Symbol* symbol) { atomMarking().markAtom(this, symbol); }
  void markId(jsid id) { atomMarking().markId(this, id); }
  void markAtomValue(const js::Value& value) {
    atomMarking().markAtomValue(this, value);
  }

  // Interface for recording telemetry metrics.
  js::Metrics metrics() { return js::Metrics(runtime_); }

  JSRuntime* runtime() { return runtime_; }
  const JSRuntime* runtime() const { return runtime_; }

  static size_t offsetOfRealm() { return offsetof(JSContext, realm_); }

  friend class JS::AutoSaveExceptionState;
  friend class js::jit::DebugModeOSRVolatileJitFrameIter;
  friend void js::ReportOutOfMemory(JSContext*);
  friend void js::ReportOverRecursed(JSContext*);
  friend void js::ReportOversizedAllocation(JSContext*, const unsigned);

 public:
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

#ifdef JS_CHECK_UNSAFE_CALL_WITH_ABI
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
#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  js::PortableBaselineStack& portableBaselineStack() {
    return runtime()->portableBaselineStack();
  }
#endif

 private:
  // Base address of the native stack for the current thread.
  mozilla::Maybe<JS::NativeStackBase> nativeStackBase_;

 public:
  JS::NativeStackBase nativeStackBase() const { return *nativeStackBase_; }

 public:
  // In brittle mode, any failure will produce a diagnostic assertion rather
  // than propagating an error or throwing an exception. This is used for
  // intermittent crash diagnostics: if an operation is failing for unknown
  // reasons, turn on brittle mode and annotate the operations within
  // SpiderMonkey that the failing operation uses with:
  //
  //   MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "specific failure");
  //
  bool brittleMode = false;

  /*
   * Stack of debuggers that currently disallow debuggee execution.
   *
   * When we check for NX we are inside the debuggee compartment, and thus a
   * stack of Debuggers that have prevented execution need to be tracked to
   * enter the correct Debugger compartment to report the error.
   */
  js::ContextData<js::EnterDebuggeeNoExecute*> noExecuteDebuggerTop;

#ifdef JS_CHECK_UNSAFE_CALL_WITH_ABI
  js::ContextData<uint32_t> inUnsafeCallWithABI;
  js::ContextData<bool> hasAutoUnsafeCallWithABI;
#endif

#ifdef DEBUG
  js::ContextData<uint32_t> liveArraySortDataInstances;
#endif

#ifdef JS_SIMULATOR
 private:
  js::ContextData<js::jit::Simulator*> simulator_;

 public:
  js::jit::Simulator* simulator() const;
  JS::NativeStackLimit* addressOfSimulatorStackLimit();
#endif

 public:
  // State used by util/DoubleToString.cpp.
  js::ContextData<DtoaState*> dtoaState;

  /*
   * When this flag is non-zero, any attempt to GC will be skipped. See the
   * AutoSuppressGC class for for details.
   */
  js::ContextData<int32_t> suppressGC;

#ifdef FUZZING_JS_FUZZILLI
  uint32_t executionHash;
  uint32_t executionHashInputs;
#endif

#ifdef DEBUG
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

  // Match limit result for the most recent call to RegExpSearcher.
  js::ContextData<uint32_t> regExpSearcherLastLimit;

  static constexpr size_t offsetOfRegExpSearcherLastLimit() {
    return offsetof(JSContext, regExpSearcherLastLimit);
  }

  // Whether we are currently executing the top level of a module.
  js::ContextData<uint32_t> isEvaluatingModule;

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

  bool isInUnsafeRegion() const { return bool(inUnsafeRegion); }

  // For JIT use.
  MOZ_NEVER_INLINE void resetInUnsafeRegion() {
    MOZ_ASSERT(inUnsafeRegion >= 0);
    inUnsafeRegion = 0;
  }

  static constexpr size_t offsetOfInUnsafeRegion() {
    return offsetof(JSContext, inUnsafeRegion);
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

  js::ContextData<uint32_t> debuggerMutations;

 private:
  // Indicates if an exception is pending and the reason for it.
  js::ContextData<JS::ExceptionStatus> status;
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

#ifdef DEBUG
  // True if this context has ever thrown an exception because of an exceeded
  // limit: stack space (ReportOverRecursed), memory (ReportOutOfMemory), or
  // some other self-imposed limit (eg ReportOversizedAllocation). Used when
  // detecting bailout loops in WarpOracle: bailout loops involving resource
  // exhaustion are generally not interesting.
  js::ContextData<bool> hadResourceExhaustion_;

  // True if this context has ever thrown an uncatchable exception to terminate
  // execution from the interrupt callback.
  js::ContextData<bool> hadUncatchableException_;

 public:
  bool hadResourceExhaustion() const {
    return hadResourceExhaustion_ || js::oom::simulator.isThreadSimulatingAny();
  }
  bool hadUncatchableException() const { return hadUncatchableException_; }
#endif

 public:
  void reportResourceExhaustion() {
#ifdef DEBUG
    hadResourceExhaustion_ = true;
#endif
  }
  void reportUncatchableException() {
    // Make sure the context has no pending exception. See also the comment for
    // JS::ReportUncatchableException.
    clearPendingException();
#ifdef DEBUG
    hadUncatchableException_ = true;
#endif
  }

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
  JSScript* currentScript(
      jsbytecode** ppc = nullptr,
      AllowCrossRealm allowCrossRealm = AllowCrossRealm::DontAllow);

  inline void minorGC(JS::GCReason reason);

 public:
  bool isExceptionPending() const {
    return JS::IsCatchableExceptionStatus(status);
  }

  /**
   * Return the pending exception and wrap it into the current compartment.
   */
  [[nodiscard]] bool getPendingException(JS::MutableHandleValue rval);

  /**
   * Return the pending exception stack and wrap it into the current
   * compartment. Return |JS::NullValue| when the pending exception has no stack
   * attached.
   */
  [[nodiscard]] bool getPendingExceptionStack(JS::MutableHandleValue rval);

  /**
   * Return the pending exception stack, but does not wrap it into the current
   * compartment. Return |nullptr| when the pending exception has no stack
   * attached.
   */
  js::SavedFrame* getPendingExceptionStack();

#ifdef DEBUG
  /**
   * Return the pending exception (without wrapping).
   */
  const JS::Value& getPendingExceptionUnwrapped();
#endif

  bool isThrowingDebuggeeWouldRun();
  bool isClosingGenerator();

  void setPendingException(JS::HandleValue v,
                           JS::Handle<js::SavedFrame*> stack);
  void setPendingException(JS::HandleValue v,
                           js::ShouldCaptureStack captureStack);

  void clearPendingException() {
    status = JS::ExceptionStatus::None;
    unwrappedException().setUndefined();
    unwrappedExceptionStack() = nullptr;
  }

  bool isThrowingOutOfMemory() const {
    return status == JS::ExceptionStatus::OutOfMemory;
  }
  bool isThrowingOverRecursed() const {
    return status == JS::ExceptionStatus::OverRecursed;
  }
  bool isPropagatingForcedReturn() const {
    return status == JS::ExceptionStatus::ForcedReturn;
  }
  void setPropagatingForcedReturn() {
    MOZ_ASSERT(status == JS::ExceptionStatus::None);
    status = JS::ExceptionStatus::ForcedReturn;
  }
  void clearPropagatingForcedReturn() {
    MOZ_ASSERT(status == JS::ExceptionStatus::ForcedReturn);
    status = JS::ExceptionStatus::None;
  }

  /*
   * See JS_SetTrustedPrincipals in jsapi.h.
   * Note: !cx->realm() is treated as trusted.
   */
  inline bool runningWithTrustedPrincipals();

  // Checks if the page's Content-Security-Policy (CSP) allows
  // runtime code generation "unsafe-eval", or "wasm-unsafe-eval" for Wasm.
  bool isRuntimeCodeGenEnabled(
      JS::RuntimeCode kind, JS::Handle<JSString*> codeString,
      JS::CompilationType compilationType,
      JS::Handle<JS::StackGCVector<JSString*>> parameterStrings,
      JS::Handle<JSString*> bodyString,
      JS::Handle<JS::StackGCVector<JS::Value>> parameterArgs,
      JS::Handle<JS::Value> bodyArg, bool* outCanCompileStrings);

  // Get code to be used by eval for Object argument.
  bool getCodeForEval(JS::HandleObject code,
                      JS::MutableHandle<JSString*> outCode);

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
  // interruptBits_ (a bitset of InterruptReasons) and jitStackLimit (set to
  // JS::NativeStackLimitMin). The JS engine must continually poll one of these
  // fields and call handleInterrupt if either field has the interrupt value.
  //
  // The point of setting jitStackLimit to JS::NativeStackLimitMin is that JIT
  // code already needs to guard on jitStackLimit in every function prologue to
  // avoid stack overflow, so we avoid a second branch on interruptBits_ by
  // setting jitStackLimit to a value that is guaranteed to fail the guard.)
  //
  // Note that the writes to interruptBits_ and jitStackLimit use a Relaxed
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
  void clearPendingInterrupt(js::InterruptReason reason);

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

  const void* addressOfJitActivation() const { return &jitActivation; }

  // Futex state, used by Atomics.wait() and Atomics.wake() on the Atomics
  // object.
  js::FutexThread fx;

  mozilla::Atomic<JS::NativeStackLimit, mozilla::Relaxed> jitStackLimit;

  // Like jitStackLimit, but not reset to trigger interrupts.
  js::ContextData<JS::NativeStackLimit> jitStackLimitNoInterrupt;

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

  // Debugger having set `exclusiveDebuggerOnEval` property to true
  // want their evaluations and calls to be ignore by all other Debuggers
  // except themself. This flag indicates whether we are in such debugger
  // evaluation, and which debugger initiated the evaluation. This debugger
  // has other references on the stack and does not need to be traced.
  js::ContextData<js::Debugger*> insideExclusiveDebuggerOnEval;

#ifdef MOZ_EXECUTION_TRACING
 private:
  // This holds onto the JS execution tracer, a system which when turned on
  // records function calls and other information about the JS which has been
  // run under this context.
  js::UniquePtr<js::ExecutionTracer> executionTracer_;

  // See suspendExecutionTracing
  bool executionTracerSuspended_ = false;

  // Cleans up caches and realm flags associated with execution tracing, while
  // leaving the underlying tracing buffers intact to be read from later.
  void cleanUpExecutionTracingState();

 public:
  js::ExecutionTracer& getExecutionTracer() {
    MOZ_ASSERT(hasExecutionTracer());
    return *executionTracer_;
  }

  // See the latter clause of the comment over executionTracer_
  [[nodiscard]] bool enableExecutionTracing();
  void disableExecutionTracing();

  // suspendExecutionTracing will turn off tracing, and clean up the relevant
  // flags on this context's realms, but still leave the trace around to be
  // collected. This currently is only called when an error occurs during
  // tracing.
  void suspendExecutionTracing();

  // Returns true if there is currently an ExecutionTracer tracing this
  // context's execution.
  bool hasExecutionTracer() {
    return !!executionTracer_ && !executionTracerSuspended_;
  }
#else
  bool hasExecutionTracer() { return false; }
#endif

}; /* struct JSContext */

inline JSContext* JSRuntime::mainContextFromOwnThread() {
  MOZ_ASSERT(mainContextFromAnyThread() == js::TlsContext.get());
  return mainContextFromAnyThread();
}

namespace js {

struct MOZ_RAII AutoResolving {
 public:
  AutoResolving(JSContext* cx, HandleObject obj, HandleId id)
      : context(cx), object(obj), id(id), link(cx->resolvingList) {
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

extern void ReportIsNotDefined(JSContext* cx, Handle<PropertyName*> name);

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

class MOZ_RAII AutoNoteExclusiveDebuggerOnEval {
  JSContext* cx;
  Debugger* oldValue;

 public:
  AutoNoteExclusiveDebuggerOnEval(JSContext* cx, Debugger* dbg)
      : cx(cx), oldValue(cx->insideExclusiveDebuggerOnEval) {
    cx->insideExclusiveDebuggerOnEval = dbg;
  }

  ~AutoNoteExclusiveDebuggerOnEval() {
    cx->insideExclusiveDebuggerOnEval = oldValue;
  }
};

enum UnsafeABIStrictness {
  NoExceptions,
  AllowPendingExceptions,
  AllowThrownExceptions
};

// Should be used in functions called directly from JIT code (with
// masm.callWithABI). This assert invariants in debug builds. Resets
// JSContext::inUnsafeCallWithABI on destruction.
//
// In debug mode, masm.callWithABI inserts code to verify that the callee
// function uses AutoUnsafeCallWithABI.
//
// While this object is live:
//   1. cx->hasAutoUnsafeCallWithABI must be true.
//   2. We can't GC.
//   3. Exceptions should not be pending/thrown.
//
// Note that #3 is a precaution, not a requirement. By default, we assert that
// the function is not called with a pending exception, and that it does not
// throw an exception itself.
class MOZ_RAII AutoUnsafeCallWithABI {
#ifdef JS_CHECK_UNSAFE_CALL_WITH_ABI
  JSContext* cx_;
  bool nested_;
  bool checkForPendingException_;
#endif
  JS::AutoCheckCannotGC nogc;

 public:
#ifdef JS_CHECK_UNSAFE_CALL_WITH_ABI
  explicit AutoUnsafeCallWithABI(
      UnsafeABIStrictness strictness = UnsafeABIStrictness::NoExceptions);
  ~AutoUnsafeCallWithABI();
#else
  explicit AutoUnsafeCallWithABI(
      UnsafeABIStrictness unused_ = UnsafeABIStrictness::NoExceptions) {}
#endif
};

} /* namespace js */

#define CHECK_THREAD(cx) \
  MOZ_ASSERT_IF(cx, js::CurrentThreadCanAccessRuntime(cx->runtime()))

/**
 * [SMDOC] JS::Result transitional macros
 *
 * ## Checking Results when your return type is not Result
 *
 * This header defines alternatives to MOZ_TRY and MOZ_TRY_VAR for when you
 * need to call a `Result` function from a function that uses false or nullptr
 * to indicate errors:
 *
 *     JS_TRY_OR_RETURN_FALSE(cx, DefenestrateObject(cx, obj));
 *     JS_TRY_VAR_OR_RETURN_FALSE(cx, v, GetObjectThrug(cx, obj));
 *
 *     JS_TRY_VAR_OR_RETURN_NULL(cx, v, GetObjectThrug(cx, obj));
 *
 * When TRY is not what you want, because you need to do some cleanup or
 * recovery on error, use this idiom:
 *
 *     if (!cx->resultToBool(expr_that_is_a_Result)) {
 *         ... your recovery code here ...
 *     }
 *
 * In place of a tail call, you can use one of these methods:
 *
 *     return cx->resultToBool(expr);  // false on error
 *     return cx->resultToPtr(expr);  // null on error
 *
 * Once we are using `Result` everywhere, including in public APIs, all of
 * these will go away.
 */

/**
 * JS_TRY_OR_RETURN_FALSE(cx, expr) runs expr to compute a Result value.
 * On success, nothing happens; on error, it returns false immediately.
 *
 * Implementation note: this involves cx because this may eventually
 * do the work of setting a pending exception or reporting OOM.
 */
#define JS_TRY_OR_RETURN_FALSE(cx, expr)                           \
  do {                                                             \
    auto tmpResult_ = (expr);                                      \
    if (tmpResult_.isErr()) return (cx)->resultToBool(tmpResult_); \
  } while (0)

#define JS_TRY_VAR_OR_RETURN_FALSE(cx, target, expr)               \
  do {                                                             \
    auto tmpResult_ = (expr);                                      \
    if (tmpResult_.isErr()) return (cx)->resultToBool(tmpResult_); \
    (target) = tmpResult_.unwrap();                                \
  } while (0)

#define JS_TRY_VAR_OR_RETURN_NULL(cx, target, expr)     \
  do {                                                  \
    auto tmpResult_ = (expr);                           \
    if (tmpResult_.isErr()) {                           \
      MOZ_ALWAYS_FALSE((cx)->resultToBool(tmpResult_)); \
      return nullptr;                                   \
    }                                                   \
    (target) = tmpResult_.unwrap();                     \
  } while (0)

#endif /* vm_JSContext_h */
