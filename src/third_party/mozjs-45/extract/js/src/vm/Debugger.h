/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Debugger_h
#define vm_Debugger_h

#include "mozilla/GuardObjects.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Range.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"

#include "jsclist.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jsweakmap.h"
#include "jswrapper.h"

#include "ds/TraceableFifo.h"
#include "gc/Barrier.h"
#include "js/Debug.h"
#include "js/HashTable.h"
#include "vm/GlobalObject.h"
#include "vm/SavedStacks.h"

enum JSTrapStatus {
    JSTRAP_ERROR,
    JSTRAP_CONTINUE,
    JSTRAP_RETURN,
    JSTRAP_THROW,
    JSTRAP_LIMIT
};

namespace js {

class LSprinter;

class Breakpoint;
class DebuggerMemory;

typedef HashSet<ReadBarrieredGlobalObject,
                MovableCellHasher<ReadBarrieredGlobalObject>,
                SystemAllocPolicy> WeakGlobalObjectSet;

/*
 * A weakmap from GC thing keys to JSObject values that supports the keys being
 * in different compartments to the values. All values must be in the same
 * compartment.
 *
 * The purpose of this is to allow the garbage collector to easily find edges
 * from debuggee object compartments to debugger compartments when calculating
 * the compartment groups.  Note that these edges are the inverse of the edges
 * stored in the cross compartment map.
 *
 * The current implementation results in all debuggee object compartments being
 * swept in the same group as the debugger.  This is a conservative approach,
 * and compartments may be unnecessarily grouped, however it results in a
 * simpler and faster implementation.
 *
 * If InvisibleKeysOk is true, then the map can have keys in invisible-to-
 * debugger compartments. If it is false, we assert that such entries are never
 * created.
 *
 * Also note that keys in these weakmaps can be in any compartment, debuggee or
 * not, because they cannot be deleted when a compartment is no longer a
 * debuggee: the values need to maintain object identity across add/remove/add
 * transitions.
 */
template <class UnbarrieredKey, bool InvisibleKeysOk=false>
class DebuggerWeakMap : private WeakMap<RelocatablePtr<UnbarrieredKey>, RelocatablePtrObject,
                                        MovableCellHasher<RelocatablePtr<UnbarrieredKey>>>
{
  private:
    typedef RelocatablePtr<UnbarrieredKey> Key;
    typedef RelocatablePtrObject Value;

    typedef HashMap<JS::Zone*,
                    uintptr_t,
                    DefaultHasher<JS::Zone*>,
                    RuntimeAllocPolicy> CountMap;

    CountMap zoneCounts;
    JSCompartment* compartment;

  public:
    typedef WeakMap<Key, Value, MovableCellHasher<Key>> Base;

    explicit DebuggerWeakMap(JSContext* cx)
        : Base(cx),
          zoneCounts(cx->runtime()),
          compartment(cx->compartment())
    { }

  public:
    /* Expose those parts of HashMap public interface that are used by Debugger methods. */

    typedef typename Base::Entry Entry;
    typedef typename Base::Ptr Ptr;
    typedef typename Base::AddPtr AddPtr;
    typedef typename Base::Range Range;
    typedef typename Base::Enum Enum;
    typedef typename Base::Lookup Lookup;

    /* Expose WeakMap public interface */

    using Base::lookupForAdd;
    using Base::all;
    using Base::trace;

    bool init(uint32_t len = 16) {
        return Base::init(len) && zoneCounts.init();
    }

    template<typename KeyInput, typename ValueInput>
    bool relookupOrAdd(AddPtr& p, const KeyInput& k, const ValueInput& v) {
        MOZ_ASSERT(v->compartment() == this->compartment);
        MOZ_ASSERT(!k->compartment()->options_.mergeable());
        MOZ_ASSERT_IF(!InvisibleKeysOk, !k->compartment()->options_.invisibleToDebugger());
        MOZ_ASSERT(!Base::has(k));
        if (!incZoneCount(k->zone()))
            return false;
        bool ok = Base::relookupOrAdd(p, k, v);
        if (!ok)
            decZoneCount(k->zone());
        return ok;
    }

    void remove(const Lookup& l) {
        MOZ_ASSERT(Base::has(l));
        Base::remove(l);
        decZoneCount(l->zone());
    }

  public:
    template <void (traceValueEdges)(JSTracer*, JSObject*)>
    void markCrossCompartmentEdges(JSTracer* tracer) {
        for (Enum e(*static_cast<Base*>(this)); !e.empty(); e.popFront()) {
            traceValueEdges(tracer, e.front().value());
            Key key = e.front().key();
            TraceEdge(tracer, &key, "Debugger WeakMap key");
            if (key != e.front().key())
                e.rekeyFront(key);
            key.unsafeSet(nullptr);
        }
    }

    bool hasKeyInZone(JS::Zone* zone) {
        CountMap::Ptr p = zoneCounts.lookup(zone);
        MOZ_ASSERT_IF(p.found(), p->value() > 0);
        return p.found();
    }

  private:
    /* Override sweep method to also update our edge cache. */
    void sweep() {
        for (Enum e(*static_cast<Base*>(this)); !e.empty(); e.popFront()) {
            if (gc::IsAboutToBeFinalized(&e.front().mutableKey())) {
                decZoneCount(e.front().key()->zone());
                e.removeFront();
            }
        }
        Base::assertEntriesNotAboutToBeFinalized();
    }

    bool incZoneCount(JS::Zone* zone) {
        CountMap::Ptr p = zoneCounts.lookupWithDefault(zone, 0);
        if (!p)
            return false;
        ++p->value();
        return true;
    }

    void decZoneCount(JS::Zone* zone) {
        CountMap::Ptr p = zoneCounts.lookup(zone);
        MOZ_ASSERT(p);
        MOZ_ASSERT(p->value() > 0);
        --p->value();
        if (p->value() == 0)
            zoneCounts.remove(zone);
    }
};

/*
 * Env is the type of what ES5 calls "lexical environments" (runtime
 * activations of lexical scopes). This is currently just JSObject, and is
 * implemented by Call, Block, With, and DeclEnv objects, among others--but
 * environments and objects are really two different concepts.
 */
typedef JSObject Env;

class Debugger : private mozilla::LinkedListElement<Debugger>
{
    friend class Breakpoint;
    friend class DebuggerMemory;
    friend class SavedStacks;
    friend class mozilla::LinkedListElement<Debugger>;
    friend class mozilla::LinkedList<Debugger>;
    friend bool (::JS_DefineDebuggerObject)(JSContext* cx, JS::HandleObject obj);
    friend bool (::JS::dbg::IsDebugger)(JSObject&);
    friend bool (::JS::dbg::GetDebuggeeGlobals)(JSContext*, JSObject&, AutoObjectVector&);
    friend void JS::dbg::onNewPromise(JSContext* cx, HandleObject promise);
    friend void JS::dbg::onPromiseSettled(JSContext* cx, HandleObject promise);
    friend bool JS::dbg::FireOnGarbageCollectionHook(JSContext* cx,
                                                     JS::dbg::GarbageCollectionEvent::Ptr&& data);

  public:
    enum Hook {
        OnDebuggerStatement,
        OnExceptionUnwind,
        OnNewScript,
        OnEnterFrame,
        OnNewGlobalObject,
        OnNewPromise,
        OnPromiseSettled,
        OnGarbageCollection,
        OnIonCompilation,
        HookCount
    };
    enum {
        JSSLOT_DEBUG_PROTO_START,
        JSSLOT_DEBUG_FRAME_PROTO = JSSLOT_DEBUG_PROTO_START,
        JSSLOT_DEBUG_ENV_PROTO,
        JSSLOT_DEBUG_OBJECT_PROTO,
        JSSLOT_DEBUG_SCRIPT_PROTO,
        JSSLOT_DEBUG_SOURCE_PROTO,
        JSSLOT_DEBUG_MEMORY_PROTO,
        JSSLOT_DEBUG_PROTO_STOP,
        JSSLOT_DEBUG_HOOK_START = JSSLOT_DEBUG_PROTO_STOP,
        JSSLOT_DEBUG_HOOK_STOP = JSSLOT_DEBUG_HOOK_START + HookCount,
        JSSLOT_DEBUG_MEMORY_INSTANCE = JSSLOT_DEBUG_HOOK_STOP,
        JSSLOT_DEBUG_COUNT
    };

    class ExecutionObservableSet
    {
      public:
        typedef HashSet<Zone*>::Range ZoneRange;

        virtual Zone* singleZone() const { return nullptr; }
        virtual JSScript* singleScriptForZoneInvalidation() const { return nullptr; }
        virtual const HashSet<Zone*>* zones() const { return nullptr; }

        virtual bool shouldRecompileOrInvalidate(JSScript* script) const = 0;
        virtual bool shouldMarkAsDebuggee(ScriptFrameIter& iter) const = 0;
    };

    // This enum is converted to and compare with bool values; NotObserving
    // must be 0 and Observing must be 1.
    enum IsObserving {
        NotObserving = 0,
        Observing = 1
    };

    // Return true if the given compartment is a debuggee of this debugger,
    // false otherwise.
    bool isDebuggeeUnbarriered(const JSCompartment* compartment) const;

    // Return true if this Debugger observed a debuggee that participated in the
    // GC identified by the given GC number. Return false otherwise.
    bool observedGC(uint64_t majorGCNumber) const {
        return observedGCs.has(majorGCNumber);
    }

    // Notify this Debugger that one or more of its debuggees is participating
    // in the GC identified by the given GC number.
    bool debuggeeIsBeingCollected(uint64_t majorGCNumber) {
        return observedGCs.put(majorGCNumber);
    }

    bool isTrackingTenurePromotions() const {
        return trackingTenurePromotions;
    }

    bool isEnabled() const {
        return enabled;
    }

    void logTenurePromotion(JSRuntime* rt, JSObject& obj, double when);
    static SavedFrame* getObjectAllocationSite(JSObject& obj);

    struct TenurePromotionsLogEntry : public JS::Traceable
    {
        TenurePromotionsLogEntry(JSRuntime* rt, JSObject& obj, double when);

        const char* className;
        double when;
        RelocatablePtrObject frame;
        size_t size;

        static void trace(TenurePromotionsLogEntry* e, JSTracer* trc) {
            if (e->frame)
                TraceEdge(trc, &e->frame, "Debugger::TenurePromotionsLogEntry::frame");
        }
    };

    struct AllocationsLogEntry : public JS::Traceable
    {
        AllocationsLogEntry(HandleObject frame, double when, const char* className,
                            HandleAtom ctorName, size_t size, bool inNursery)
            : frame(frame),
              when(when),
              className(className),
              ctorName(ctorName),
              size(size),
              inNursery(inNursery)
        {
            MOZ_ASSERT_IF(frame, UncheckedUnwrap(frame)->is<SavedFrame>());
        };

        RelocatablePtrObject frame;
        double when;
        const char* className;
        RelocatablePtrAtom ctorName;
        size_t size;
        bool inNursery;

        static void trace(AllocationsLogEntry* e, JSTracer* trc) {
            if (e->frame)
                TraceEdge(trc, &e->frame, "Debugger::AllocationsLogEntry::frame");
            if (e->ctorName)
                TraceEdge(trc, &e->ctorName, "Debugger::AllocationsLogEntry::ctorName");
        }
    };

  private:
    HeapPtrNativeObject object;         /* The Debugger object. Strong reference. */
    WeakGlobalObjectSet debuggees;      /* Debuggee globals. Cross-compartment weak references. */
    JS::ZoneSet debuggeeZones; /* Set of zones that we have debuggees in. */
    js::HeapPtrObject uncaughtExceptionHook; /* Strong reference. */
    bool enabled;
    bool allowUnobservedAsmJS;

    // Wether to enable code coverage on the Debuggee.
    bool collectCoverageInfo;

    JSCList breakpoints;                /* Circular list of all js::Breakpoints in this debugger */

    // The set of GC numbers for which one or more of this Debugger's observed
    // debuggees participated in.
    js::HashSet<uint64_t> observedGCs;

    using TenurePromotionsLog = js::TraceableFifo<TenurePromotionsLogEntry>;
    TenurePromotionsLog tenurePromotionsLog;
    bool trackingTenurePromotions;
    size_t maxTenurePromotionsLogLength;
    bool tenurePromotionsLogOverflowed;

    using AllocationsLog = js::TraceableFifo<AllocationsLogEntry>;

    AllocationsLog allocationsLog;
    bool trackingAllocationSites;
    double allocationSamplingProbability;
    size_t maxAllocationsLogLength;
    bool allocationsLogOverflowed;

    static const size_t DEFAULT_MAX_LOG_LENGTH = 5000;

    bool appendAllocationSite(JSContext* cx, HandleObject obj, HandleSavedFrame frame,
                              double when);

    /*
     * Recompute the set of debuggee zones based on the set of debuggee globals.
     */
    void recomputeDebuggeeZoneSet();

    /*
     * Return true if there is an existing object metadata callback for the
     * given global's compartment that will prevent our instrumentation of
     * allocations.
     */
    static bool cannotTrackAllocations(const GlobalObject& global);

    /*
     * Return true if the given global is being observed by at least one
     * Debugger that is tracking allocations.
     */
    static bool isObservedByDebuggerTrackingAllocations(const GlobalObject& global);

    /*
     * Add allocations tracking for objects allocated within the given
     * debuggee's compartment. The given debuggee global must be observed by at
     * least one Debugger that is enabled and tracking allocations.
     */
    static bool addAllocationsTracking(JSContext* cx, Handle<GlobalObject*> debuggee);

    /*
     * Remove allocations tracking for objects allocated within the given
     * global's compartment. This is a no-op if there are still Debuggers
     * observing this global and who are tracking allocations.
     */
    static void removeAllocationsTracking(GlobalObject& global);

    /*
     * Add or remove allocations tracking for all debuggees.
     */
    bool addAllocationsTrackingForAllDebuggees(JSContext* cx);
    void removeAllocationsTrackingForAllDebuggees();

    /*
     * If this Debugger is enabled, and has a onNewGlobalObject handler, then
     * this link is inserted into the circular list headed by
     * JSRuntime::onNewGlobalObjectWatchers. Otherwise, this is set to a
     * singleton cycle.
     */
    JSCList onNewGlobalObjectWatchersLink;

    /*
     * Map from stack frames that are currently on the stack to Debugger.Frame
     * instances.
     *
     * The keys are always live stack frames. We drop them from this map as
     * soon as they leave the stack (see slowPathOnLeaveFrame) and in
     * removeDebuggee.
     *
     * We don't trace the keys of this map (the frames are on the stack and
     * thus necessarily live), but we do trace the values. It's like a WeakMap
     * that way, but since stack frames are not gc-things, the implementation
     * has to be different.
     */
    typedef HashMap<AbstractFramePtr,
                    RelocatablePtrNativeObject,
                    DefaultHasher<AbstractFramePtr>,
                    RuntimeAllocPolicy> FrameMap;
    FrameMap frames;

    /* An ephemeral map from JSScript* to Debugger.Script instances. */
    typedef DebuggerWeakMap<JSScript*> ScriptWeakMap;
    ScriptWeakMap scripts;

    /* The map from debuggee source script objects to their Debugger.Source instances. */
    typedef DebuggerWeakMap<JSObject*, true> SourceWeakMap;
    SourceWeakMap sources;

    /* The map from debuggee objects to their Debugger.Object instances. */
    typedef DebuggerWeakMap<JSObject*> ObjectWeakMap;
    ObjectWeakMap objects;

    /* The map from debuggee Envs to Debugger.Environment instances. */
    ObjectWeakMap environments;

    /*
     * Keep track of tracelogger last drained identifiers to know if there are
     * lost events.
     */
#ifdef NIGHTLY_BUILD
    uint32_t traceLoggerLastDrainedSize;
    uint32_t traceLoggerLastDrainedIteration;
#endif
    uint32_t traceLoggerScriptedCallsLastDrainedSize;
    uint32_t traceLoggerScriptedCallsLastDrainedIteration;

    class FrameRange;
    class ScriptQuery;
    class ObjectQuery;

    bool addDebuggeeGlobal(JSContext* cx, Handle<GlobalObject*> obj);
    void removeDebuggeeGlobal(FreeOp* fop, GlobalObject* global,
                              WeakGlobalObjectSet::Enum* debugEnum);

    /*
     * Cope with an error or exception in a debugger hook.
     *
     * If callHook is true, then call the uncaughtExceptionHook, if any. If, in
     * addition, vp is given, then parse the value returned by
     * uncaughtExceptionHook as a resumption value.
     *
     * If there is no uncaughtExceptionHook, or if it fails, report and clear
     * the pending exception on ac.context and return JSTRAP_ERROR.
     *
     * This always calls ac.leave(); ac is a parameter because this method must
     * do some things in the debugger compartment and some things in the
     * debuggee compartment.
     */
    JSTrapStatus handleUncaughtException(mozilla::Maybe<AutoCompartment>& ac, bool callHook);
    JSTrapStatus handleUncaughtException(mozilla::Maybe<AutoCompartment>& ac, MutableHandleValue vp, bool callHook);

    JSTrapStatus handleUncaughtExceptionHelper(mozilla::Maybe<AutoCompartment>& ac,
                                               MutableHandleValue* vp, bool callHook);

    /*
     * Handle the result of a hook that is expected to return a resumption
     * value <https://wiki.mozilla.org/Debugger#Resumption_Values>. This is called
     * when we return from a debugging hook to debuggee code. The interpreter wants
     * a (JSTrapStatus, Value) pair telling it how to proceed.
     *
     * Precondition: ac is entered. We are in the debugger compartment.
     *
     * Postcondition: This called ac.leave(). See handleUncaughtException.
     *
     * If ok is false, the hook failed. If an exception is pending in
     * ac.context(), return handleUncaughtException(ac, vp, callhook).
     * Otherwise just return JSTRAP_ERROR.
     *
     * If ok is true, there must be no exception pending in ac.context(). rv may be:
     *     undefined - Return JSTRAP_CONTINUE to continue execution normally.
     *     {return: value} or {throw: value} - Call unwrapDebuggeeValue to
     *         unwrap value. Store the result in *vp and return JSTRAP_RETURN
     *         or JSTRAP_THROW. The interpreter will force the current frame to
     *         return or throw an exception.
     *     null - Return JSTRAP_ERROR to terminate the debuggee with an
     *         uncatchable error.
     *     anything else - Make a new TypeError the pending exception and
     *         return handleUncaughtException(ac, vp, callHook).
     */
    JSTrapStatus parseResumptionValue(mozilla::Maybe<AutoCompartment>& ac, bool ok, const Value& rv,
                                      MutableHandleValue vp, bool callHook = true);

    GlobalObject* unwrapDebuggeeArgument(JSContext* cx, const Value& v);

    static void traceObject(JSTracer* trc, JSObject* obj);
    void trace(JSTracer* trc);
    static void finalize(FreeOp* fop, JSObject* obj);
    void markCrossCompartmentEdges(JSTracer* tracer);

    static const Class jsclass;

    static bool getHookImpl(JSContext* cx, CallArgs& args, Debugger& dbg, Hook which);
    static bool setHookImpl(JSContext* cx, CallArgs& args, Debugger& dbg, Hook which);

    static Debugger* fromThisValue(JSContext* cx, const CallArgs& ca, const char* fnname);
    static bool getEnabled(JSContext* cx, unsigned argc, Value* vp);
    static bool setEnabled(JSContext* cx, unsigned argc, Value* vp);
    static bool getOnDebuggerStatement(JSContext* cx, unsigned argc, Value* vp);
    static bool setOnDebuggerStatement(JSContext* cx, unsigned argc, Value* vp);
    static bool getOnExceptionUnwind(JSContext* cx, unsigned argc, Value* vp);
    static bool setOnExceptionUnwind(JSContext* cx, unsigned argc, Value* vp);
    static bool getOnNewScript(JSContext* cx, unsigned argc, Value* vp);
    static bool setOnNewScript(JSContext* cx, unsigned argc, Value* vp);
    static bool getOnEnterFrame(JSContext* cx, unsigned argc, Value* vp);
    static bool setOnEnterFrame(JSContext* cx, unsigned argc, Value* vp);
    static bool getOnNewGlobalObject(JSContext* cx, unsigned argc, Value* vp);
    static bool setOnNewGlobalObject(JSContext* cx, unsigned argc, Value* vp);
    static bool getOnNewPromise(JSContext* cx, unsigned argc, Value* vp);
    static bool setOnNewPromise(JSContext* cx, unsigned argc, Value* vp);
    static bool getOnPromiseSettled(JSContext* cx, unsigned argc, Value* vp);
    static bool setOnPromiseSettled(JSContext* cx, unsigned argc, Value* vp);
    static bool getUncaughtExceptionHook(JSContext* cx, unsigned argc, Value* vp);
    static bool setUncaughtExceptionHook(JSContext* cx, unsigned argc, Value* vp);
    static bool getAllowUnobservedAsmJS(JSContext* cx, unsigned argc, Value* vp);
    static bool setAllowUnobservedAsmJS(JSContext* cx, unsigned argc, Value* vp);
    static bool getCollectCoverageInfo(JSContext* cx, unsigned argc, Value* vp);
    static bool setCollectCoverageInfo(JSContext* cx, unsigned argc, Value* vp);
    static bool getMemory(JSContext* cx, unsigned argc, Value* vp);
    static bool getOnIonCompilation(JSContext* cx, unsigned argc, Value* vp);
    static bool setOnIonCompilation(JSContext* cx, unsigned argc, Value* vp);
    static bool addDebuggee(JSContext* cx, unsigned argc, Value* vp);
    static bool addAllGlobalsAsDebuggees(JSContext* cx, unsigned argc, Value* vp);
    static bool removeDebuggee(JSContext* cx, unsigned argc, Value* vp);
    static bool removeAllDebuggees(JSContext* cx, unsigned argc, Value* vp);
    static bool hasDebuggee(JSContext* cx, unsigned argc, Value* vp);
    static bool getDebuggees(JSContext* cx, unsigned argc, Value* vp);
    static bool getNewestFrame(JSContext* cx, unsigned argc, Value* vp);
    static bool clearAllBreakpoints(JSContext* cx, unsigned argc, Value* vp);
    static bool findScripts(JSContext* cx, unsigned argc, Value* vp);
    static bool findObjects(JSContext* cx, unsigned argc, Value* vp);
    static bool findAllGlobals(JSContext* cx, unsigned argc, Value* vp);
    static bool makeGlobalObjectReference(JSContext* cx, unsigned argc, Value* vp);
    static bool setupTraceLoggerScriptCalls(JSContext* cx, unsigned argc, Value* vp);
    static bool drainTraceLoggerScriptCalls(JSContext* cx, unsigned argc, Value* vp);
    static bool startTraceLogger(JSContext* cx, unsigned argc, Value* vp);
    static bool endTraceLogger(JSContext* cx, unsigned argc, Value* vp);
#ifdef NIGHTLY_BUILD
    static bool setupTraceLogger(JSContext* cx, unsigned argc, Value* vp);
    static bool drainTraceLogger(JSContext* cx, unsigned argc, Value* vp);
#endif
    static bool construct(JSContext* cx, unsigned argc, Value* vp);
    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];

    static void removeFromFrameMapsAndClearBreakpointsIn(JSContext* cx, AbstractFramePtr frame);
    static bool updateExecutionObservabilityOfFrames(JSContext* cx, const ExecutionObservableSet& obs,
                                                     IsObserving observing);
    static bool updateExecutionObservabilityOfScripts(JSContext* cx, const ExecutionObservableSet& obs,
                                                      IsObserving observing);
    static bool updateExecutionObservability(JSContext* cx, ExecutionObservableSet& obs,
                                             IsObserving observing);

  public:
    static bool ensureExecutionObservabilityOfOsrFrame(JSContext* cx, InterpreterFrame* frame);

    // Public for DebuggerScript_setBreakpoint.
    static bool ensureExecutionObservabilityOfScript(JSContext* cx, JSScript* script);

    // Whether the Debugger instance needs to observe all non-AOT JS
    // execution of its debugees.
    IsObserving observesAllExecution() const;

    // Whether the Debugger instance needs to observe AOT-compiled asm.js
    // execution of its debuggees.
    IsObserving observesAsmJS() const;

    // Whether the Debugger instance needs to observe coverage of any JavaScript
    // execution.
    IsObserving observesCoverage() const;

  private:
    static bool ensureExecutionObservabilityOfFrame(JSContext* cx, AbstractFramePtr frame);
    static bool ensureExecutionObservabilityOfCompartment(JSContext* cx, JSCompartment* comp);

    static bool hookObservesAllExecution(Hook which);

    bool updateObservesAllExecutionOnDebuggees(JSContext* cx, IsObserving observing);
    bool updateObservesCoverageOnDebuggees(JSContext* cx, IsObserving observing);
    void updateObservesAsmJSOnDebuggees(IsObserving observing);

    JSObject* getHook(Hook hook) const;
    bool hasAnyLiveHooks() const;

    static JSTrapStatus slowPathOnEnterFrame(JSContext* cx, AbstractFramePtr frame);
    static bool slowPathOnLeaveFrame(JSContext* cx, AbstractFramePtr frame, bool ok);
    static JSTrapStatus slowPathOnDebuggerStatement(JSContext* cx, AbstractFramePtr frame);
    static JSTrapStatus slowPathOnExceptionUnwind(JSContext* cx, AbstractFramePtr frame);
    static void slowPathOnNewScript(JSContext* cx, HandleScript script);
    static void slowPathOnNewGlobalObject(JSContext* cx, Handle<GlobalObject*> global);
    static bool slowPathOnLogAllocationSite(JSContext* cx, HandleObject obj, HandleSavedFrame frame,
                                            double when, GlobalObject::DebuggerVector& dbgs);
    static void slowPathPromiseHook(JSContext* cx, Hook hook, HandleObject promise);
    static void slowPathOnIonCompilation(JSContext* cx, Handle<ScriptVector> scripts,
                                         LSprinter& graph);

    template <typename HookIsEnabledFun /* bool (Debugger*) */,
              typename FireHookFun /* JSTrapStatus (Debugger*) */>
    static JSTrapStatus dispatchHook(JSContext* cx, HookIsEnabledFun hookIsEnabled,
                                     FireHookFun fireHook);

    JSTrapStatus fireDebuggerStatement(JSContext* cx, MutableHandleValue vp);
    JSTrapStatus fireExceptionUnwind(JSContext* cx, MutableHandleValue vp);
    JSTrapStatus fireEnterFrame(JSContext* cx, AbstractFramePtr frame, MutableHandleValue vp);
    JSTrapStatus fireNewGlobalObject(JSContext* cx, Handle<GlobalObject*> global, MutableHandleValue vp);
    JSTrapStatus firePromiseHook(JSContext* cx, Hook hook, HandleObject promise, MutableHandleValue vp);

    /*
     * Allocate and initialize a Debugger.Script instance whose referent is
     * |script|.
     */
    JSObject* newDebuggerScript(JSContext* cx, HandleScript script);

    /*
     * Allocate and initialize a Debugger.Source instance whose referent is
     * |source|.
     */
    JSObject* newDebuggerSource(JSContext* cx, js::HandleScriptSource source);

    /*
     * Receive a "new script" event from the engine. A new script was compiled
     * or deserialized.
     */
    void fireNewScript(JSContext* cx, HandleScript script);

    /*
     * Receive a "garbage collection" event from the engine. A GC cycle with the
     * given data was recently completed.
     */
    void fireOnGarbageCollectionHook(JSContext* cx,
                                     const JS::dbg::GarbageCollectionEvent::Ptr& gcData);

    /*
     * Receive a "Ion compilation" event from the engine. An Ion compilation with
     * the given summary just got linked.
     */
    JSTrapStatus fireOnIonCompilationHook(JSContext* cx, Handle<ScriptVector> scripts,
                                          LSprinter& graph);

    /*
     * Gets a Debugger.Frame object. If maybeIter is non-null, we eagerly copy
     * its data if we need to make a new Debugger.Frame.
     */
    bool getScriptFrameWithIter(JSContext* cx, AbstractFramePtr frame,
                                const ScriptFrameIter* maybeIter, MutableHandleValue vp);

    inline Breakpoint* firstBreakpoint() const;

    static inline Debugger* fromOnNewGlobalObjectWatchersLink(JSCList* link);

    static bool replaceFrameGuts(JSContext* cx, AbstractFramePtr from, AbstractFramePtr to,
                                 ScriptFrameIter& iter);

  public:
    Debugger(JSContext* cx, NativeObject* dbg);
    ~Debugger();

    bool init(JSContext* cx);
    inline const js::HeapPtrNativeObject& toJSObject() const;
    inline js::HeapPtrNativeObject& toJSObjectRef();
    static inline Debugger* fromJSObject(const JSObject* obj);
    static Debugger* fromChildJSObject(JSObject* obj);

    bool hasMemory() const;
    DebuggerMemory& memory() const;

    WeakGlobalObjectSet::Range allDebuggees() const { return debuggees.all(); }

    /*********************************** Methods for interaction with the GC. */

    /*
     * A Debugger object is live if:
     *   * the Debugger JSObject is live (Debugger::trace handles this case); OR
     *   * it is in the middle of dispatching an event (the event dispatching
     *     code roots it in this case); OR
     *   * it is enabled, and it is debugging at least one live compartment,
     *     and at least one of the following is true:
     *       - it has a debugger hook installed
     *       - it has a breakpoint set on a live script
     *       - it has a watchpoint set on a live object.
     *
     * Debugger::markAllIteratively handles the last case. If it finds any
     * Debugger objects that are definitely live but not yet marked, it marks
     * them and returns true. If not, it returns false.
     */
    static void markIncomingCrossCompartmentEdges(JSTracer* tracer);
    static bool markAllIteratively(GCMarker* trc);
    static void markAll(JSTracer* trc);
    static void sweepAll(FreeOp* fop);
    static void detachAllDebuggersFromGlobal(FreeOp* fop, GlobalObject* global);
    static void findZoneEdges(JS::Zone* v, gc::ComponentFinder<JS::Zone>& finder);

    /*
     * JSTrapStatus Overview
     * ---------------------
     *
     * The |onEnterFrame|, |onDebuggerStatement|, and |onExceptionUnwind|
     * methods below return a JSTrapStatus code that indicates how execution
     * should proceed:
     *
     * - JSTRAP_CONTINUE: Continue execution normally.
     *
     * - JSTRAP_THROW: Throw an exception. The method has set |cx|'s
     *   pending exception to the value to be thrown.
     *
     * - JSTRAP_ERROR: Terminate execution (as is done when a script is terminated
     *   for running too long). The method has cleared |cx|'s pending
     *   exception.
     *
     * - JSTRAP_RETURN: Return from the new frame immediately. The method has
     *   set the youngest JS frame's return value appropriately.
     */

    /*
     * Announce to the debugger that the context has entered a new JavaScript
     * frame, |frame|. Call whatever hooks have been registered to observe new
     * frames.
     */
    static inline JSTrapStatus onEnterFrame(JSContext* cx, AbstractFramePtr frame);

    /*
     * Announce to the debugger a |debugger;| statement on has been
     * encountered on the youngest JS frame on |cx|. Call whatever hooks have
     * been registered to observe this.
     *
     * Note that this method is called for all |debugger;| statements,
     * regardless of the frame's debuggee-ness.
     */
    static inline JSTrapStatus onDebuggerStatement(JSContext* cx, AbstractFramePtr frame);

    /*
     * Announce to the debugger that an exception has been thrown and propagated
     * to |frame|. Call whatever hooks have been registered to observe this.
     */
    static inline JSTrapStatus onExceptionUnwind(JSContext* cx, AbstractFramePtr frame);

    /*
     * Announce to the debugger that the thread has exited a JavaScript frame, |frame|.
     * If |ok| is true, the frame is returning normally; if |ok| is false, the frame
     * is throwing an exception or terminating.
     *
     * Change cx's current exception and |frame|'s return value to reflect the changes
     * in behavior the hooks request, if any. Return the new error/success value.
     *
     * This function may be called twice for the same outgoing frame; only the
     * first call has any effect. (Permitting double calls simplifies some
     * cases where an onPop handler's resumption value changes a return to a
     * throw, or vice versa: we can redirect to a complete copy of the
     * alternative path, containing its own call to onLeaveFrame.)
     */
    static inline bool onLeaveFrame(JSContext* cx, AbstractFramePtr frame, bool ok);

    static inline void onNewScript(JSContext* cx, HandleScript script);
    static inline void onNewGlobalObject(JSContext* cx, Handle<GlobalObject*> global);
    static inline bool onLogAllocationSite(JSContext* cx, JSObject* obj, HandleSavedFrame frame,
                                           double when);
    static inline bool observesIonCompilation(JSContext* cx);
    static inline void onIonCompilation(JSContext* cx, Handle<ScriptVector> scripts,
                                        LSprinter& graph);
    static JSTrapStatus onTrap(JSContext* cx, MutableHandleValue vp);
    static JSTrapStatus onSingleStep(JSContext* cx, MutableHandleValue vp);
    static bool handleBaselineOsr(JSContext* cx, InterpreterFrame* from, jit::BaselineFrame* to);
    static bool handleIonBailout(JSContext* cx, jit::RematerializedFrame* from, jit::BaselineFrame* to);
    static void handleUnrecoverableIonBailoutError(JSContext* cx, jit::RematerializedFrame* frame);
    static void propagateForcedReturn(JSContext* cx, AbstractFramePtr frame, HandleValue rval);
    static bool hasLiveHook(GlobalObject* global, Hook which);
    static bool inFrameMaps(AbstractFramePtr frame);

    /************************************* Functions for use by Debugger.cpp. */

    inline bool observesEnterFrame() const;
    inline bool observesNewScript() const;
    inline bool observesNewGlobalObject() const;
    inline bool observesGlobal(GlobalObject* global) const;
    bool observesFrame(AbstractFramePtr frame) const;
    bool observesFrame(const ScriptFrameIter& iter) const;
    bool observesScript(JSScript* script) const;

    /*
     * If env is nullptr, call vp->setNull() and return true. Otherwise, find
     * or create a Debugger.Environment object for the given Env. On success,
     * store the Environment object in *vp and return true.
     */
    bool wrapEnvironment(JSContext* cx, Handle<Env*> env, MutableHandleValue vp);

    /*
     * Like cx->compartment()->wrap(cx, vp), but for the debugger compartment.
     *
     * Preconditions: *vp is a value from a debuggee compartment; cx is in the
     * debugger's compartment.
     *
     * If *vp is an object, this produces a (new or existing) Debugger.Object
     * wrapper for it. Otherwise this is the same as JSCompartment::wrap.
     *
     * If *vp is a magic JS_OPTIMIZED_OUT value, this produces a plain object
     * of the form { optimizedOut: true }.
     *
     * If *vp is a magic JS_OPTIMIZED_ARGUMENTS value signifying missing
     * arguments, this produces a plain object of the form { missingArguments:
     * true }.
     *
     * If *vp is a magic JS_UNINITIALIZED_LEXICAL value signifying an
     * unaccessible uninitialized binding, this produces a plain object of the
     * form { uninitialized: true }.
     */
    bool wrapDebuggeeValue(JSContext* cx, MutableHandleValue vp);

    /*
     * Unwrap a Debug.Object, without rewrapping it for any particular debuggee
     * compartment.
     *
     * Preconditions: cx is in the debugger compartment. *vp is a value in that
     * compartment. (*vp should be a "debuggee value", meaning it is the
     * debugger's reflection of a value in the debuggee.)
     *
     * If *vp is a Debugger.Object, store the referent in *vp. Otherwise, if *vp
     * is an object, throw a TypeError, because it is not a debuggee
     * value. Otherwise *vp is a primitive, so leave it alone.
     *
     * When passing values from the debuggee to the debugger:
     *     enter debugger compartment;
     *     call wrapDebuggeeValue;  // compartment- and debugger-wrapping
     *
     * When passing values from the debugger to the debuggee:
     *     call unwrapDebuggeeValue;  // debugger-unwrapping
     *     enter debuggee compartment;
     *     call cx->compartment()->wrap;  // compartment-rewrapping
     *
     * (Extreme nerd sidebar: Unwrapping happens in two steps because there are
     * two different kinds of symmetry at work: regardless of which direction
     * we're going, we want any exceptions to be created and thrown in the
     * debugger compartment--mirror symmetry. But compartment wrapping always
     * happens in the target compartment--rotational symmetry.)
     */
    bool unwrapDebuggeeValue(JSContext* cx, MutableHandleValue vp);
    bool unwrapDebuggeeObject(JSContext* cx, MutableHandleObject obj);
    bool unwrapPropertyDescriptor(JSContext* cx, HandleObject obj,
                                  MutableHandle<PropertyDescriptor> desc);

    /*
     * Store the Debugger.Frame object for frame in *vp.
     *
     * Use this if you have already access to a frame pointer without having
     * to incur the cost of walking the stack.
     */
    bool getScriptFrame(JSContext* cx, AbstractFramePtr frame, MutableHandleValue vp) {
        return getScriptFrameWithIter(cx, frame, nullptr, vp);
    }

    /*
     * Store the Debugger.Frame object for iter in *vp. Eagerly copies a
     * ScriptFrameIter::Data.
     *
     * Use this if you had to make a ScriptFrameIter to get the required
     * frame, in which case the cost of walking the stack has already been
     * paid.
     */
    bool getScriptFrame(JSContext* cx, const ScriptFrameIter& iter, MutableHandleValue vp) {
        return getScriptFrameWithIter(cx, iter.abstractFramePtr(), &iter, vp);
    }

    /*
     * Set |*status| and |*value| to a (JSTrapStatus, Value) pair reflecting a
     * standard SpiderMonkey call state: a boolean success value |ok|, a return
     * value |rv|, and a context |cx| that may or may not have an exception set.
     * If an exception was pending on |cx|, it is cleared (and |ok| is asserted
     * to be false).
     */
    static void resultToCompletion(JSContext* cx, bool ok, const Value& rv,
                                   JSTrapStatus* status, MutableHandleValue value);

    /*
     * Set |*result| to a JavaScript completion value corresponding to |status|
     * and |value|. |value| should be the return value or exception value, not
     * wrapped as a debuggee value. |cx| must be in the debugger compartment.
     */
    bool newCompletionValue(JSContext* cx, JSTrapStatus status, Value value,
                            MutableHandleValue result);

    /*
     * Precondition: we are in the debuggee compartment (ac is entered) and ok
     * is true if the operation in the debuggee compartment succeeded, false on
     * error or exception.
     *
     * Postcondition: we are in the debugger compartment, having called
     * ac.leave() even if an error occurred.
     *
     * On success, a completion value is in vp and ac.context does not have a
     * pending exception. (This ordinarily returns true even if the ok argument
     * is false.)
     */
    bool receiveCompletionValue(mozilla::Maybe<AutoCompartment>& ac, bool ok,
                                HandleValue val,
                                MutableHandleValue vp);

    /*
     * Return the Debugger.Script object for |script|, or create a new one if
     * needed. The context |cx| must be in the debugger compartment; |script|
     * must be a script in a debuggee compartment.
     */
    JSObject* wrapScript(JSContext* cx, HandleScript script);

    /*
     * Return the Debugger.Source object for |source|, or create a new one if
     * needed. The context |cx| must be in the debugger compartment; |source|
     * must be a script source object in a debuggee compartment.
     */
    JSObject* wrapSource(JSContext* cx, js::HandleScriptSource source);

  private:
    Debugger(const Debugger&) = delete;
    Debugger & operator=(const Debugger&) = delete;
};

template<>
struct DefaultGCPolicy<Debugger::TenurePromotionsLogEntry> {
    static void trace(JSTracer* trc, Debugger::TenurePromotionsLogEntry* e, const char*) {
        Debugger::TenurePromotionsLogEntry::trace(e, trc);
    }
};

template<>
struct DefaultGCPolicy<Debugger::AllocationsLogEntry> {
    static void trace(JSTracer* trc, Debugger::AllocationsLogEntry* e, const char*) {
        Debugger::AllocationsLogEntry::trace(e, trc);
    }
};

class BreakpointSite {
    friend class Breakpoint;
    friend struct ::JSCompartment;
    friend class ::JSScript;
    friend class Debugger;

  public:
    JSScript* script;
    jsbytecode * const pc;

  private:
    JSCList breakpoints;  /* cyclic list of all js::Breakpoints at this instruction */
    size_t enabledCount;  /* number of breakpoints in the list that are enabled */

    void recompile(FreeOp* fop);

  public:
    BreakpointSite(JSScript* script, jsbytecode* pc);
    Breakpoint* firstBreakpoint() const;
    bool hasBreakpoint(Breakpoint* bp);

    void inc(FreeOp* fop);
    void dec(FreeOp* fop);
    void destroyIfEmpty(FreeOp* fop);
};

/*
 * Each Breakpoint is a member of two linked lists: its debugger's list and its
 * site's list.
 *
 * GC rules:
 *   - script is live, breakpoint exists, and debugger is enabled
 *      ==> debugger is live
 *   - script is live, breakpoint exists, and debugger is live
 *      ==> retain the breakpoint and the handler object is live
 *
 * Debugger::markAllIteratively implements these two rules. It uses
 * Debugger::hasAnyLiveHooks to check for rule 1.
 *
 * Nothing else causes a breakpoint to be retained, so if its script or
 * debugger is collected, the breakpoint is destroyed during GC sweep phase,
 * even if the debugger compartment isn't being GC'd. This is implemented in
 * Zone::sweepBreakpoints.
 */
class Breakpoint {
    friend struct ::JSCompartment;
    friend class Debugger;

  public:
    Debugger * const debugger;
    BreakpointSite * const site;
  private:
    /* |handler| is marked unconditionally during minor GC. */
    js::PreBarrieredObject handler;
    JSCList debuggerLinks;
    JSCList siteLinks;

  public:
    static Breakpoint* fromDebuggerLinks(JSCList* links);
    static Breakpoint* fromSiteLinks(JSCList* links);
    Breakpoint(Debugger* debugger, BreakpointSite* site, JSObject* handler);
    void destroy(FreeOp* fop);
    Breakpoint* nextInDebugger();
    Breakpoint* nextInSite();
    const PreBarrieredObject& getHandler() const { return handler; }
    PreBarrieredObject& getHandlerRef() { return handler; }
};

Breakpoint*
Debugger::firstBreakpoint() const
{
    if (JS_CLIST_IS_EMPTY(&breakpoints))
        return nullptr;
    return Breakpoint::fromDebuggerLinks(JS_NEXT_LINK(&breakpoints));
}

/* static */ Debugger*
Debugger::fromOnNewGlobalObjectWatchersLink(JSCList* link) {
    char* p = reinterpret_cast<char*>(link);
    return reinterpret_cast<Debugger*>(p - offsetof(Debugger, onNewGlobalObjectWatchersLink));
}

const js::HeapPtrNativeObject&
Debugger::toJSObject() const
{
    MOZ_ASSERT(object);
    return object;
}

js::HeapPtrNativeObject&
Debugger::toJSObjectRef()
{
    MOZ_ASSERT(object);
    return object;
}

bool
Debugger::observesEnterFrame() const
{
    return enabled && getHook(OnEnterFrame);
}

bool
Debugger::observesNewScript() const
{
    return enabled && getHook(OnNewScript);
}

bool
Debugger::observesNewGlobalObject() const
{
    return enabled && getHook(OnNewGlobalObject);
}

bool
Debugger::observesGlobal(GlobalObject* global) const
{
    ReadBarriered<GlobalObject*> debuggee(global);
    return debuggees.has(debuggee);
}

/* static */ void
Debugger::onNewScript(JSContext* cx, HandleScript script)
{
    // We early return in slowPathOnNewScript for self-hosted scripts, so we can
    // ignore those in our assertion here.
    MOZ_ASSERT_IF(!script->compartment()->options().invisibleToDebugger() &&
                  !script->selfHosted(),
                  script->compartment()->firedOnNewGlobalObject);
    if (script->compartment()->isDebuggee())
        slowPathOnNewScript(cx, script);
}

/* static */ void
Debugger::onNewGlobalObject(JSContext* cx, Handle<GlobalObject*> global)
{
    MOZ_ASSERT(!global->compartment()->firedOnNewGlobalObject);
#ifdef DEBUG
    global->compartment()->firedOnNewGlobalObject = true;
#endif
    if (!JS_CLIST_IS_EMPTY(&cx->runtime()->onNewGlobalObjectWatchers))
        Debugger::slowPathOnNewGlobalObject(cx, global);
}

/* static */ bool
Debugger::onLogAllocationSite(JSContext* cx, JSObject* obj, HandleSavedFrame frame, double when)
{
    GlobalObject::DebuggerVector* dbgs = cx->global()->getDebuggers();
    if (!dbgs || dbgs->empty())
        return true;
    RootedObject hobj(cx, obj);
    return Debugger::slowPathOnLogAllocationSite(cx, hobj, frame, when, *dbgs);
}

bool ReportObjectRequired(JSContext* cx);

} /* namespace js */


#endif /* vm_Debugger_h */
