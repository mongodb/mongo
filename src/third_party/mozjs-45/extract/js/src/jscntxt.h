/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS execution context. */

#ifndef jscntxt_h
#define jscntxt_h

#include "mozilla/MemoryReporting.h"

#include "js/TraceableVector.h"
#include "js/Vector.h"
#include "vm/Runtime.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4100) /* Silence unreferenced formal parameter warnings */
#endif

struct DtoaState;

namespace js {

namespace jit {
class JitContext;
class DebugModeOSRVolatileJitFrameIterator;
} // namespace jit

typedef HashSet<Shape*> ShapeSet;

/* Detects cycles when traversing an object graph. */
class MOZ_RAII AutoCycleDetector
{
  public:
    using Set = HashSet<JSObject*, MovableCellHasher<JSObject*>>;

    AutoCycleDetector(JSContext* cx, HandleObject objArg
                      MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : cx(cx), obj(cx, objArg), cyclic(true)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    ~AutoCycleDetector();

    bool init();

    bool foundCycle() { return cyclic; }

  private:
    JSContext* cx;
    RootedObject obj;
    bool cyclic;
    uint32_t hashsetGenerationAtInit;
    Set::AddPtr hashsetAddPointer;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/* Updates references in the cycle detection set if the GC moves them. */
extern void
TraceCycleDetectionSet(JSTracer* trc, AutoCycleDetector::Set& set);

struct AutoResolving;

namespace frontend { struct CompileError; }

/*
 * Execution Context Overview:
 *
 * Several different structures may be used to provide a context for operations
 * on the VM. Each context is thread local, but varies in what data it can
 * access and what other threads may be running.
 *
 * - ExclusiveContext is used by threads operating in one compartment/zone,
 * where other threads may operate in other compartments, but *not* the same
 * compartment or zone which the ExclusiveContext is in. A thread with an
 * ExclusiveContext may enter the atoms compartment and atomize strings, in
 * which case a lock is used.
 *
 * - JSContext is used only by the runtime's main thread. The context may
 * operate in any compartment or zone which is not used by an ExclusiveContext,
 * and will only run in parallel with threads using such contexts.
 *
 * A JSContext coerces to an ExclusiveContext.
 */

struct HelperThread;

class ExclusiveContext : public ContextFriendFields,
                         public MallocProvider<ExclusiveContext>
{
    friend class gc::ArenaLists;
    friend class AutoCompartment;
    friend class AutoLockForExclusiveAccess;
    friend struct StackBaseShape;
    friend void JSScript::initCompartment(ExclusiveContext* cx);
    friend class jit::JitContext;
    friend class Activation;

    // The thread on which this context is running, if this is not a JSContext.
    HelperThread* helperThread_;

  public:
    enum ContextKind {
        Context_JS,
        Context_Exclusive
    };

  private:
    ContextKind contextKind_;

  public:
    PerThreadData* perThreadData;

    ExclusiveContext(JSRuntime* rt, PerThreadData* pt, ContextKind kind);

    bool isJSContext() const {
        return contextKind_ == Context_JS;
    }

    JSContext* maybeJSContext() const {
        if (isJSContext())
            return (JSContext*) this;
        return nullptr;
    }

    JSContext* asJSContext() const {
        // Note: there is no way to perform an unchecked coercion from a
        // ThreadSafeContext to a JSContext. This ensures that trying to use
        // the context as a JSContext off the main thread will nullptr crash
        // rather than race.
        MOZ_ASSERT(isJSContext());
        return maybeJSContext();
    }

    // In some cases we could potentially want to do operations that require a
    // JSContext while running off the main thread. While this should never
    // actually happen, the wide enough API for working off the main thread
    // makes such operations impossible to rule out. Rather than blindly using
    // asJSContext() and crashing afterwards, this method may be used to watch
    // for such cases and produce either a soft failure in release builds or
    // an assertion failure in debug builds.
    bool shouldBeJSContext() const {
        MOZ_ASSERT(isJSContext());
        return isJSContext();
    }

  protected:
    js::gc::ArenaLists* arenas_;

  public:
    inline js::gc::ArenaLists* arenas() const { return arenas_; }

    template <typename T>
    bool isInsideCurrentZone(T thing) const {
        return thing->zoneFromAnyThread() == zone_;
    }

    template <typename T>
    inline bool isInsideCurrentCompartment(T thing) const {
        return thing->compartment() == compartment_;
    }

    void* onOutOfMemory(js::AllocFunction allocFunc, size_t nbytes, void* reallocPtr = nullptr) {
        if (!isJSContext())
            return nullptr;
        return runtime_->onOutOfMemory(allocFunc, nbytes, reallocPtr, asJSContext());
    }

    /* Clear the pending exception (if any) due to OOM. */
    void recoverFromOutOfMemory();

    inline void updateMallocCounter(size_t nbytes) {
        // Note: this is racy.
        runtime_->updateMallocCounter(zone_, nbytes);
    }

    void reportAllocationOverflow() {
        js::ReportAllocationOverflow(this);
    }

    // Accessors for immutable runtime data.
    JSAtomState& names() { return *runtime_->commonNames; }
    StaticStrings& staticStrings() { return *runtime_->staticStrings; }
    bool isPermanentAtomsInitialized() { return !!runtime_->permanentAtoms; }
    FrozenAtomSet& permanentAtoms() { return *runtime_->permanentAtoms; }
    WellKnownSymbols& wellKnownSymbols() { return *runtime_->wellKnownSymbols; }
    const JS::AsmJSCacheOps& asmJSCacheOps() { return runtime_->asmJSCacheOps; }
    PropertyName* emptyString() { return runtime_->emptyString; }
    FreeOp* defaultFreeOp() { return runtime_->defaultFreeOp(); }
    void* runtimeAddressForJit() { return runtime_; }
    void* runtimeAddressOfInterruptUint32() { return runtime_->addressOfInterruptUint32(); }
    void* stackLimitAddress(StackKind kind) { return &runtime_->mainThread.nativeStackLimit[kind]; }
    void* stackLimitAddressForJitCode(StackKind kind);
    uintptr_t stackLimit(StackKind kind) { return runtime_->mainThread.nativeStackLimit[kind]; }
    size_t gcSystemPageSize() { return gc::SystemPageSize(); }
    bool canUseSignalHandlers() const { return runtime_->canUseSignalHandlers(); }
    bool jitSupportsFloatingPoint() const { return runtime_->jitSupportsFloatingPoint; }
    bool jitSupportsSimd() const { return runtime_->jitSupportsSimd; }
    bool lcovEnabled() const { return runtime_->lcovOutput.isEnabled(); }

    // Thread local data that may be accessed freely.
    DtoaState* dtoaState() {
        return perThreadData->dtoaState;
    }

    /*
     * "Entering" a compartment changes cx->compartment (which changes
     * cx->global). Note that this does not push any InterpreterFrame which means
     * that it is possible for cx->fp()->compartment() != cx->compartment.
     * This is not a problem since, in general, most places in the VM cannot
     * know that they were called from script (e.g., they may have been called
     * through the JSAPI via JS_CallFunction) and thus cannot expect fp.
     *
     * Compartments should be entered/left in a LIFO fasion. The depth of this
     * enter/leave stack is maintained by enterCompartmentDepth_ and queried by
     * hasEnteredCompartment.
     *
     * To enter a compartment, code should prefer using AutoCompartment over
     * manually calling cx->enterCompartment/leaveCompartment.
     */
  protected:
    unsigned            enterCompartmentDepth_;
    inline void setCompartment(JSCompartment* comp);
  public:
    bool hasEnteredCompartment() const {
        return enterCompartmentDepth_ > 0;
    }
#ifdef DEBUG
    unsigned getEnterCompartmentDepth() const {
        return enterCompartmentDepth_;
    }
#endif

    inline void enterCompartment(JSCompartment* c);
    inline void enterNullCompartment();
    inline void leaveCompartment(JSCompartment* oldCompartment);

    void setHelperThread(HelperThread* helperThread);
    HelperThread* helperThread() const { return helperThread_; }

    // Threads with an ExclusiveContext may freely access any data in their
    // compartment and zone.
    JSCompartment* compartment() const {
        MOZ_ASSERT_IF(runtime_->isAtomsCompartment(compartment_),
                      runtime_->currentThreadHasExclusiveAccess());
        return compartment_;
    }
    JS::Zone* zone() const {
        MOZ_ASSERT_IF(!compartment(), !zone_);
        MOZ_ASSERT_IF(compartment(), js::GetCompartmentZone(compartment()) == zone_);
        return zone_;
    }

    // Zone local methods that can be used freely from an ExclusiveContext.
    inline js::LifoAlloc& typeLifoAlloc();

    // Current global. This is only safe to use within the scope of the
    // AutoCompartment from which it's called.
    inline js::Handle<js::GlobalObject*> global() const;

    // Methods to access runtime data that must be protected by locks.
    frontend::ParseMapPool& parseMapPool() {
        return runtime_->parseMapPool();
    }
    AtomSet& atoms() {
        return runtime_->atoms();
    }
    JSCompartment* atomsCompartment() {
        return runtime_->atomsCompartment();
    }
    SymbolRegistry& symbolRegistry() {
        return runtime_->symbolRegistry();
    }
    ScriptDataTable& scriptDataTable() {
        return runtime_->scriptDataTable();
    }

    // Methods specific to any HelperThread for the context.
    frontend::CompileError& addPendingCompileError();
    void addPendingOverRecursed();
};

} /* namespace js */

struct JSContext : public js::ExclusiveContext,
                   public mozilla::LinkedListElement<JSContext>
{
    explicit JSContext(JSRuntime* rt);
    ~JSContext();

    JSRuntime* runtime() const { return runtime_; }
    js::PerThreadData& mainThread() const { return runtime()->mainThread; }

    static size_t offsetOfRuntime() {
        return offsetof(JSContext, runtime_);
    }
    static size_t offsetOfCompartment() {
        return offsetof(JSContext, compartment_);
    }

    friend class js::ExclusiveContext;
    friend class JS::AutoSaveExceptionState;
    friend class js::jit::DebugModeOSRVolatileJitFrameIterator;
    friend void js::ReportOverRecursed(JSContext*);

  private:
    /* Exception state -- the exception member is a GC root by definition. */
    bool                throwing;            /* is there a pending exception? */
    JS::PersistentRooted<JS::Value> unwrappedException_; /* most-recently-thrown exception */

    /* Per-context options. */
    JS::ContextOptions  options_;

    // True if the exception currently being thrown is by result of
    // ReportOverRecursed. See Debugger::slowPathOnExceptionUnwind.
    bool                overRecursed_;

    // True if propagating a forced return from an interrupt handler during
    // debug mode.
    bool                propagatingForcedReturn_;

    // A stack of live iterators that need to be updated in case of debug mode
    // OSR.
    js::jit::DebugModeOSRVolatileJitFrameIterator* liveVolatileJitFrameIterators_;

  public:
    int32_t             reportGranularity;  /* see vm/Probes.h */

    js::AutoResolving*  resolvingList;

    /* True if generating an error, to prevent runaway recursion. */
    bool                generatingError;

    /* See JS_SaveFrameChain/JS_RestoreFrameChain. */
  private:
    struct SavedFrameChain {
        SavedFrameChain(JSCompartment* comp, unsigned count)
          : compartment(comp), enterCompartmentCount(count) {}
        JSCompartment* compartment;
        unsigned enterCompartmentCount;
    };
    typedef js::Vector<SavedFrameChain, 1, js::SystemAllocPolicy> SaveStack;
    SaveStack           savedFrameChains_;
  public:
    bool saveFrameChain();
    void restoreFrameChain();

  public:
    /* State for object and array toSource conversion. */
    js::AutoCycleDetector::Set cycleDetectorSet;

    /* Client opaque pointers. */
    void*               data;
    void*               data2;

  public:

    /*
     * Return:
     * - The newest scripted frame's version, if there is such a frame.
     * - The version from the compartment.
     * - The default version.
     *
     * Note: if this ever shows up in a profile, just add caching!
     */
    JSVersion findVersion() const;

    const JS::ContextOptions& options() const {
        return options_;
    }

    JS::ContextOptions& options() {
        return options_;
    }

    js::LifoAlloc& tempLifoAlloc() { return runtime()->tempLifoAlloc; }

    unsigned            outstandingRequests;/* number of JS_BeginRequest calls
                                               without the corresponding
                                               JS_EndRequest. */

    bool jitIsBroken;

    void updateJITEnabled();

    /* Whether this context has JS frames on the stack. */
    bool currentlyRunning() const;

    bool currentlyRunningInInterpreter() const {
        return runtime_->activation()->isInterpreter();
    }
    bool currentlyRunningInJit() const {
        return runtime_->activation()->isJit();
    }
    js::InterpreterFrame* interpreterFrame() const {
        return runtime_->activation()->asInterpreter()->current();
    }
    js::InterpreterRegs& interpreterRegs() const {
        return runtime_->activation()->asInterpreter()->regs();
    }

    /*
     * Get the topmost script and optional pc on the stack. By default, this
     * function only returns a JSScript in the current compartment, returning
     * nullptr if the current script is in a different compartment. This
     * behavior can be overridden by passing ALLOW_CROSS_COMPARTMENT.
     */
    enum MaybeAllowCrossCompartment {
        DONT_ALLOW_CROSS_COMPARTMENT = false,
        ALLOW_CROSS_COMPARTMENT = true
    };
    inline JSScript* currentScript(jsbytecode** pc = nullptr,
                                   MaybeAllowCrossCompartment = DONT_ALLOW_CROSS_COMPARTMENT) const;

    // The generational GC nursery may only be used on the main thread.
    inline js::Nursery& nursery() {
        return runtime_->gc.nursery;
    }

    void minorGC(JS::gcreason::Reason reason) {
        runtime_->gc.minorGC(this, reason);
    }

  public:
    bool isExceptionPending() {
        return throwing;
    }

    MOZ_WARN_UNUSED_RESULT
    bool getPendingException(JS::MutableHandleValue rval);

    bool isThrowingOutOfMemory();
    bool isClosingGenerator();

    void setPendingException(js::Value v);

    void clearPendingException() {
        throwing = false;
        overRecursed_ = false;
        unwrappedException_.setUndefined();
    }

    bool isThrowingOverRecursed() const { return throwing && overRecursed_; }
    bool isPropagatingForcedReturn() const { return propagatingForcedReturn_; }
    void setPropagatingForcedReturn() { propagatingForcedReturn_ = true; }
    void clearPropagatingForcedReturn() { propagatingForcedReturn_ = false; }

    /*
     * See JS_SetTrustedPrincipals in jsapi.h.
     * Note: !cx->compartment is treated as trusted.
     */
    inline bool runningWithTrustedPrincipals() const;

    JS_FRIEND_API(size_t) sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

    void mark(JSTracer* trc);

  private:
    /*
     * The allocation code calls the function to indicate either OOM failure
     * when p is null or that a memory pressure counter has reached some
     * threshold when p is not null. The function takes the pointer and not
     * a boolean flag to minimize the amount of code in its inlined callers.
     */
    JS_FRIEND_API(void) checkMallocGCPressure(void* p);
}; /* struct JSContext */

namespace js {

struct MOZ_RAII AutoResolving {
  public:
    enum Kind {
        LOOKUP,
        WATCH
    };

    AutoResolving(JSContext* cx, HandleObject obj, HandleId id, Kind kind = LOOKUP
                  MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : context(cx), object(obj), id(id), kind(kind), link(cx->resolvingList)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        MOZ_ASSERT(obj);
        cx->resolvingList = this;
    }

    ~AutoResolving() {
        MOZ_ASSERT(context->resolvingList == this);
        context->resolvingList = link;
    }

    bool alreadyStarted() const {
        return link && alreadyStartedSlow();
    }

  private:
    bool alreadyStartedSlow() const;

    JSContext*          const context;
    HandleObject        object;
    HandleId            id;
    Kind                const kind;
    AutoResolving*      const link;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * Enumerate all contexts in a runtime.
 */
class ContextIter
{
    JSContext* iter;

  public:
    explicit ContextIter(JSRuntime* rt) {
        iter = rt->contextList.getFirst();
    }

    bool done() const {
        return !iter;
    }

    void next() {
        MOZ_ASSERT(!done());
        iter = iter->getNext();
    }

    JSContext* get() const {
        MOZ_ASSERT(!done());
        return iter;
    }

    operator JSContext*() const {
        return get();
    }

    JSContext* operator ->() const {
        return get();
    }
};

/*
 * Create and destroy functions for JSContext, which is manually allocated
 * and exclusively owned.
 */
extern JSContext*
NewContext(JSRuntime* rt, size_t stackChunkSize);

enum DestroyContextMode {
    DCM_NO_GC,
    DCM_FORCE_GC,
    DCM_NEW_FAILED
};

extern void
DestroyContext(JSContext* cx, DestroyContextMode mode);

enum ErrorArgumentsType {
    ArgumentsAreUnicode,
    ArgumentsAreASCII
};

/*
 * Loads and returns a self-hosted function by name. For performance, define
 * the property name in vm/CommonPropertyNames.h.
 *
 * Defined in SelfHosting.cpp.
 */
JSFunction*
SelfHostedFunction(JSContext* cx, HandlePropertyName propName);

#ifdef va_start
extern bool
ReportErrorVA(JSContext* cx, unsigned flags, const char* format, va_list ap);

extern bool
ReportErrorNumberVA(JSContext* cx, unsigned flags, JSErrorCallback callback,
                    void* userRef, const unsigned errorNumber,
                    ErrorArgumentsType argumentsType, va_list ap);

extern bool
ReportErrorNumberUCArray(JSContext* cx, unsigned flags, JSErrorCallback callback,
                         void* userRef, const unsigned errorNumber,
                         const char16_t** args);
#endif

extern bool
ExpandErrorArgumentsVA(ExclusiveContext* cx, JSErrorCallback callback,
                       void* userRef, const unsigned errorNumber,
                       char** message, JSErrorReport* reportp,
                       ErrorArgumentsType argumentsType, va_list ap);

/* |callee| requires a usage string provided by JS_DefineFunctionsWithHelp. */
extern void
ReportUsageError(JSContext* cx, HandleObject callee, const char* msg);

/*
 * Prints a full report and returns true if the given report is non-nullptr
 * and the report doesn't have the JSREPORT_WARNING flag set or reportWarnings
 * is true.
 * Returns false otherwise, printing just the message if the report is nullptr.
 */
extern bool
PrintError(JSContext* cx, FILE* file, const char* message, JSErrorReport* report,
           bool reportWarnings);

/*
 * Send a JSErrorReport to the errorReporter callback.
 */
void
CallErrorReporter(JSContext* cx, const char* message, JSErrorReport* report);

extern bool
ReportIsNotDefined(JSContext* cx, HandlePropertyName name);

extern bool
ReportIsNotDefined(JSContext* cx, HandleId id);

/*
 * Report an attempt to access the property of a null or undefined value (v).
 */
extern bool
ReportIsNullOrUndefined(JSContext* cx, int spindex, HandleValue v, HandleString fallback);

extern void
ReportMissingArg(JSContext* cx, js::HandleValue v, unsigned arg);

/*
 * Report error using js_DecompileValueGenerator(cx, spindex, v, fallback) as
 * the first argument for the error message. If the error message has less
 * then 3 arguments, use null for arg1 or arg2.
 */
extern bool
ReportValueErrorFlags(JSContext* cx, unsigned flags, const unsigned errorNumber,
                      int spindex, HandleValue v, HandleString fallback,
                      const char* arg1, const char* arg2);

#define ReportValueError(cx,errorNumber,spindex,v,fallback)                   \
    ((void)ReportValueErrorFlags(cx, JSREPORT_ERROR, errorNumber,             \
                                    spindex, v, fallback, nullptr, nullptr))

#define ReportValueError2(cx,errorNumber,spindex,v,fallback,arg1)             \
    ((void)ReportValueErrorFlags(cx, JSREPORT_ERROR, errorNumber,             \
                                    spindex, v, fallback, arg1, nullptr))

#define ReportValueError3(cx,errorNumber,spindex,v,fallback,arg1,arg2)        \
    ((void)ReportValueErrorFlags(cx, JSREPORT_ERROR, errorNumber,             \
                                    spindex, v, fallback, arg1, arg2))

} /* namespace js */

extern const JSErrorFormatString js_ErrorFormatString[JSErr_Limit];

namespace js {

MOZ_ALWAYS_INLINE bool
CheckForInterrupt(JSContext* cx)
{
    // Add an inline fast-path since we have to check for interrupts in some hot
    // C++ loops of library builtins.
    JSRuntime* rt = cx->runtime();
    if (MOZ_UNLIKELY(rt->hasPendingInterrupt()))
        return rt->handleInterrupt(cx);
    return true;
}

/************************************************************************/

typedef JS::AutoVectorRooter<PropertyName*> AutoPropertyNameVector;

using ShapeVector = js::TraceableVector<Shape*>;
using StringVector = js::TraceableVector<JSString*>;

/* AutoArrayRooter roots an external array of Values. */
class MOZ_RAII AutoArrayRooter : private JS::AutoGCRooter
{
  public:
    AutoArrayRooter(JSContext* cx, size_t len, Value* vec
                    MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : JS::AutoGCRooter(cx, len), array(vec)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        MOZ_ASSERT(tag_ >= 0);
    }

    void changeLength(size_t newLength) {
        tag_ = ptrdiff_t(newLength);
        MOZ_ASSERT(tag_ >= 0);
    }

    void changeArray(Value* newArray, size_t newLength) {
        changeLength(newLength);
        array = newArray;
    }

    Value* start() {
        return array;
    }

    size_t length() {
        MOZ_ASSERT(tag_ >= 0);
        return size_t(tag_);
    }

    MutableHandleValue handleAt(size_t i) {
        MOZ_ASSERT(i < size_t(tag_));
        return MutableHandleValue::fromMarkedLocation(&array[i]);
    }
    HandleValue handleAt(size_t i) const {
        MOZ_ASSERT(i < size_t(tag_));
        return HandleValue::fromMarkedLocation(&array[i]);
    }
    MutableHandleValue operator[](size_t i) {
        MOZ_ASSERT(i < size_t(tag_));
        return MutableHandleValue::fromMarkedLocation(&array[i]);
    }
    HandleValue operator[](size_t i) const {
        MOZ_ASSERT(i < size_t(tag_));
        return HandleValue::fromMarkedLocation(&array[i]);
    }

    friend void JS::AutoGCRooter::trace(JSTracer* trc);

  private:
    Value* array;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoAssertNoException
{
#ifdef DEBUG
    JSContext* cx;
    bool hadException;
#endif

  public:
    explicit AutoAssertNoException(JSContext* cx)
#ifdef DEBUG
      : cx(cx),
        hadException(cx->isExceptionPending())
#endif
    {
    }

    ~AutoAssertNoException()
    {
        MOZ_ASSERT_IF(!hadException, !cx->isExceptionPending());
    }
};

/* Exposed intrinsics for the JITs. */
bool intrinsic_IsSuspendedStarGenerator(JSContext* cx, unsigned argc, Value* vp);

class MOZ_RAII AutoLockForExclusiveAccess
{
    JSRuntime* runtime;

    void init(JSRuntime* rt) {
        runtime = rt;
        if (runtime->numExclusiveThreads) {
            runtime->assertCanLock(ExclusiveAccessLock);
            PR_Lock(runtime->exclusiveAccessLock);
#ifdef DEBUG
            runtime->exclusiveAccessOwner = PR_GetCurrentThread();
#endif
        } else {
            MOZ_ASSERT(!runtime->mainThreadHasExclusiveAccess);
            runtime->mainThreadHasExclusiveAccess = true;
        }
    }

  public:
    explicit AutoLockForExclusiveAccess(ExclusiveContext* cx MOZ_GUARD_OBJECT_NOTIFIER_PARAM) {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        init(cx->runtime_);
    }
    explicit AutoLockForExclusiveAccess(JSRuntime* rt MOZ_GUARD_OBJECT_NOTIFIER_PARAM) {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        init(rt);
    }
    ~AutoLockForExclusiveAccess() {
        if (runtime->numExclusiveThreads) {
            MOZ_ASSERT(runtime->exclusiveAccessOwner == PR_GetCurrentThread());
            runtime->exclusiveAccessOwner = nullptr;
            PR_Unlock(runtime->exclusiveAccessLock);
        } else {
            MOZ_ASSERT(runtime->mainThreadHasExclusiveAccess);
            runtime->mainThreadHasExclusiveAccess = false;
        }
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

} /* namespace js */

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /* jscntxt_h */
