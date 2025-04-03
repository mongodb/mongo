/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_Debugger_h
#define debugger_Debugger_h

#include "mozilla/Assertions.h"        // for MOZ_ASSERT_HELPER1
#include "mozilla/Attributes.h"        // for MOZ_RAII
#include "mozilla/DoublyLinkedList.h"  // for DoublyLinkedListElement
#include "mozilla/HashTable.h"         // for HashSet, DefaultHasher (ptr only)
#include "mozilla/LinkedList.h"        // for LinkedList (ptr only)
#include "mozilla/Maybe.h"             // for Maybe, Nothing
#include "mozilla/Range.h"             // for Range
#include "mozilla/Result.h"            // for Result
#include "mozilla/TimeStamp.h"         // for TimeStamp
#include "mozilla/Variant.h"           // for Variant

#include <stddef.h>  // for size_t
#include <stdint.h>  // for uint32_t, uint64_t, uintptr_t
#include <utility>   // for std::move

#include "jstypes.h"           // for JS_GC_ZEAL
#include "NamespaceImports.h"  // for Value, HandleObject

#include "debugger/DebugAPI.h"      // for DebugAPI
#include "debugger/Object.h"        // for DebuggerObject
#include "ds/TraceableFifo.h"       // for TraceableFifo
#include "gc/Barrier.h"             //
#include "gc/Tracer.h"              // for TraceNullableEdge, TraceEdge
#include "gc/WeakMap.h"             // for WeakMap
#include "gc/ZoneAllocator.h"       // for ZoneAllocPolicy
#include "js/Debug.h"               // JS_DefineDebuggerObject
#include "js/GCAPI.h"               // for GarbageCollectionEvent
#include "js/GCVariant.h"           // for GCVariant
#include "js/Proxy.h"               // for PropertyDescriptor
#include "js/RootingAPI.h"          // for Handle
#include "js/TracingAPI.h"          // for TraceRoot
#include "js/Wrapper.h"             // for UncheckedUnwrap
#include "proxy/DeadObjectProxy.h"  // for IsDeadProxyObject
#include "vm/GeneratorObject.h"     // for AbstractGeneratorObject
#include "vm/GlobalObject.h"        // for GlobalObject
#include "vm/JSContext.h"           // for JSContext
#include "vm/JSObject.h"            // for JSObject
#include "vm/JSScript.h"            // for JSScript, ScriptSourceObject
#include "vm/NativeObject.h"        // for NativeObject
#include "vm/Runtime.h"             // for JSRuntime
#include "vm/SavedFrame.h"          // for SavedFrame
#include "vm/Stack.h"               // for AbstractFramePtr, FrameIter
#include "vm/StringType.h"          // for JSAtom
#include "wasm/WasmJS.h"            // for WasmInstanceObject

class JS_PUBLIC_API JSFunction;

namespace JS {
class JS_PUBLIC_API AutoStableStringChars;
class JS_PUBLIC_API Compartment;
class JS_PUBLIC_API Realm;
class JS_PUBLIC_API Zone;
} /* namespace JS */

namespace js {
class AutoRealm;
class CrossCompartmentKey;
class Debugger;
class DebuggerEnvironment;
class PromiseObject;
namespace gc {
struct Cell;
} /* namespace gc */
namespace wasm {
class Instance;
} /* namespace wasm */
} /* namespace js */

/*
 * Windows 3.x used a cooperative multitasking model, with a Yield macro that
 * let you relinquish control to other cooperative threads. Microsoft replaced
 * it with an empty macro long ago. We should be free to use it in our code.
 */
#undef Yield

namespace js {

class Breakpoint;
class DebuggerFrame;
class DebuggerScript;
class DebuggerSource;
class DebuggerMemory;
class ScriptedOnStepHandler;
class ScriptedOnPopHandler;
class DebuggerDebuggeeLink;

/**
 * Tells how the JS engine should resume debuggee execution after firing a
 * debugger hook.  Most debugger hooks get to choose how the debuggee proceeds;
 * see js/src/doc/Debugger/Conventions.md under "Resumption Values".
 *
 * Debugger::processHandlerResult() translates between JavaScript values and
 * this enum.
 */
enum class ResumeMode {
  /**
   * The debuggee should continue unchanged.
   *
   * This corresponds to a resumption value of `undefined`.
   */
  Continue,

  /**
   * Throw an exception in the debuggee.
   *
   * This corresponds to a resumption value of `{throw: <value>}`.
   */
  Throw,

  /**
   * Terminate the debuggee, as if it had been cancelled via the "slow
   * script" ribbon.
   *
   * This corresponds to a resumption value of `null`.
   */
  Terminate,

  /**
   * Force the debuggee to return from the current frame.
   *
   * This corresponds to a resumption value of `{return: <value>}`.
   */
  Return,
};

/**
 * A completion value, describing how some sort of JavaScript evaluation
 * completed. This is used to tell an onPop handler what's going on with the
 * frame, and to report the outcome of call, apply, setProperty, and getProperty
 * operations.
 *
 * Local variables of type Completion should be held in Rooted locations,
 * and passed using Handle and MutableHandle.
 */
class Completion {
 public:
  struct Return {
    explicit Return(const Value& value) : value(value) {}
    Value value;

    void trace(JSTracer* trc) {
      JS::TraceRoot(trc, &value, "js::Completion::Return::value");
    }
  };

  struct Throw {
    Throw(const Value& exception, SavedFrame* stack)
        : exception(exception), stack(stack) {}
    Value exception;
    SavedFrame* stack;

    void trace(JSTracer* trc) {
      JS::TraceRoot(trc, &exception, "js::Completion::Throw::exception");
      JS::TraceRoot(trc, &stack, "js::Completion::Throw::stack");
    }
  };

  struct Terminate {
    void trace(JSTracer* trc) {}
  };

  struct InitialYield {
    explicit InitialYield(AbstractGeneratorObject* generatorObject)
        : generatorObject(generatorObject) {}
    AbstractGeneratorObject* generatorObject;

    void trace(JSTracer* trc) {
      JS::TraceRoot(trc, &generatorObject,
                    "js::Completion::InitialYield::generatorObject");
    }
  };

  struct Yield {
    Yield(AbstractGeneratorObject* generatorObject, const Value& iteratorResult)
        : generatorObject(generatorObject), iteratorResult(iteratorResult) {}
    AbstractGeneratorObject* generatorObject;
    Value iteratorResult;

    void trace(JSTracer* trc) {
      JS::TraceRoot(trc, &generatorObject,
                    "js::Completion::Yield::generatorObject");
      JS::TraceRoot(trc, &iteratorResult,
                    "js::Completion::Yield::iteratorResult");
    }
  };

  struct Await {
    Await(AbstractGeneratorObject* generatorObject, const Value& awaitee)
        : generatorObject(generatorObject), awaitee(awaitee) {}
    AbstractGeneratorObject* generatorObject;
    Value awaitee;

    void trace(JSTracer* trc) {
      JS::TraceRoot(trc, &generatorObject,
                    "js::Completion::Await::generatorObject");
      JS::TraceRoot(trc, &awaitee, "js::Completion::Await::awaitee");
    }
  };

  // The JS::Result macros want to assign to an existing variable, so having a
  // default constructor is handy.
  Completion() : variant(Terminate()) {}

  // Construct a completion from a specific variant.
  //
  // Unfortunately, using a template here would prevent the implicit definitions
  // of the copy and move constructor and assignment operators, which is icky.
  explicit Completion(Return&& variant)
      : variant(std::forward<Return>(variant)) {}
  explicit Completion(Throw&& variant)
      : variant(std::forward<Throw>(variant)) {}
  explicit Completion(Terminate&& variant)
      : variant(std::forward<Terminate>(variant)) {}
  explicit Completion(InitialYield&& variant)
      : variant(std::forward<InitialYield>(variant)) {}
  explicit Completion(Yield&& variant)
      : variant(std::forward<Yield>(variant)) {}
  explicit Completion(Await&& variant)
      : variant(std::forward<Await>(variant)) {}

  // Capture a JavaScript operation result as a Completion value. This clears
  // any exception and stack from cx, taking ownership of them itself.
  static Completion fromJSResult(JSContext* cx, bool ok, const Value& rv);

  // Construct a completion given an AbstractFramePtr that is being popped. This
  // clears any exception and stack from cx, taking ownership of them itself.
  static Completion fromJSFramePop(JSContext* cx, AbstractFramePtr frame,
                                   const jsbytecode* pc, bool ok);

  template <typename V>
  bool is() const {
    return variant.template is<V>();
  }

  template <typename V>
  V& as() {
    return variant.template as<V>();
  }

  template <typename V>
  const V& as() const {
    return variant.template as<V>();
  }

  void trace(JSTracer* trc);

  /* True if this completion is a suspension of a generator or async call. */
  bool suspending() const {
    return variant.is<InitialYield>() || variant.is<Yield>() ||
           variant.is<Await>();
  }

  /* Set `result` to a Debugger API completion value describing this completion.
   */
  bool buildCompletionValue(JSContext* cx, Debugger* dbg,
                            MutableHandleValue result) const;

  /*
   * Set `resumeMode`, `value`, and `exnStack` to values describing this
   * completion.
   */
  void toResumeMode(ResumeMode& resumeMode, MutableHandleValue value,
                    MutableHandle<SavedFrame*> exnStack) const;
  /*
   * Given a `ResumeMode` and value (typically derived from a resumption value
   * returned by a Debugger hook), update this completion as requested.
   */
  void updateFromHookResult(ResumeMode resumeMode, HandleValue value);

 private:
  using Variant =
      mozilla::Variant<Return, Throw, Terminate, InitialYield, Yield, Await>;
  struct BuildValueMatcher;
  struct ToResumeModeMatcher;

  Variant variant;
};

typedef HashSet<WeakHeapPtr<GlobalObject*>,
                StableCellHasher<WeakHeapPtr<GlobalObject*>>, ZoneAllocPolicy>
    WeakGlobalObjectSet;

#ifdef DEBUG
extern void CheckDebuggeeThing(BaseScript* script, bool invisibleOk);

extern void CheckDebuggeeThing(JSObject* obj, bool invisibleOk);
#endif

/*
 * [SMDOC] Cross-compartment weakmap entries for Debugger API objects
 *
 * The Debugger API creates objects like Debugger.Object, Debugger.Script,
 * Debugger.Environment, etc. to refer to things in the debuggee. Each Debugger
 * gets at most one Debugger.Mumble for each referent: Debugger.Mumbles are
 * unique per referent per Debugger. This is accomplished by storing the
 * debugger objects in a DebuggerWeakMap, using the debuggee thing as the key.
 *
 * Since a Debugger and its debuggee must be in different compartments, a
 * Debugger.Mumble's pointer to its referent is a cross-compartment edge, from
 * the debugger's compartment into the debuggee compartment. Like any other sort
 * of cross-compartment edge, the GC needs to be able to find all of these edges
 * readily. The GC therefore consults the debugger's weakmap tables as
 * necessary.  This allows the garbage collector to easily find edges between
 * debuggee object compartments and debugger compartments when calculating the
 * zone sweep groups.
 *
 * The current implementation results in all debuggee object compartments being
 * swept in the same group as the debugger. This is a conservative approach, and
 * compartments may be unnecessarily grouped. However this results in a simpler
 * and faster implementation.
 */

/*
 * A weakmap from GC thing keys to JSObject values that supports the keys being
 * in different compartments to the values. All values must be in the same
 * compartment.
 *
 * If InvisibleKeysOk is true, then the map can have keys in invisible-to-
 * debugger compartments. If it is false, we assert that such entries are never
 * created.
 *
 * Note that keys in these weakmaps can be in any compartment, debuggee or not,
 * because they are not deleted when a compartment is no longer a debuggee: the
 * values need to maintain object identity across add/remove/add
 * transitions. (Frames are an exception to the rule. Existing Debugger.Frame
 * objects are killed if their realm is removed as a debugger; if the realm
 * beacomes a debuggee again later, new Frame objects are created.)
 */
template <class Referent, class Wrapper, bool InvisibleKeysOk = false>
class DebuggerWeakMap : private WeakMap<HeapPtr<Referent*>, HeapPtr<Wrapper*>> {
 private:
  using Key = HeapPtr<Referent*>;
  using Value = HeapPtr<Wrapper*>;

  JS::Compartment* compartment;

 public:
  typedef WeakMap<Key, Value> Base;
  using ReferentType = Referent;
  using WrapperType = Wrapper;

  explicit DebuggerWeakMap(JSContext* cx)
      : Base(cx), compartment(cx->compartment()) {}

 public:
  // Expose those parts of HashMap public interface that are used by Debugger
  // methods.

  using Entry = typename Base::Entry;
  using Ptr = typename Base::Ptr;
  using AddPtr = typename Base::AddPtr;
  using Range = typename Base::Range;
  using Lookup = typename Base::Lookup;

  // Expose WeakMap public interface.

  using Base::all;
  using Base::has;
  using Base::lookup;
  using Base::lookupForAdd;
  using Base::lookupUnbarriered;
  using Base::remove;
  using Base::trace;
  using Base::zone;
#ifdef DEBUG
  using Base::hasEntry;
#endif

  class Enum : public Base::Enum {
   public:
    explicit Enum(DebuggerWeakMap& map) : Base::Enum(map) {}
  };

  template <typename KeyInput, typename ValueInput>
  bool relookupOrAdd(AddPtr& p, const KeyInput& k, const ValueInput& v) {
    MOZ_ASSERT(v->compartment() == this->compartment);
#ifdef DEBUG
    CheckDebuggeeThing(k, InvisibleKeysOk);
#endif
    MOZ_ASSERT(!Base::has(k));
    bool ok = Base::relookupOrAdd(p, k, v);
    return ok;
  }

 public:
  void traceCrossCompartmentEdges(JSTracer* tracer) {
    for (Enum e(*this); !e.empty(); e.popFront()) {
      TraceEdge(tracer, &e.front().mutableKey(), "Debugger WeakMap key");
      e.front().value()->trace(tracer);
    }
  }

  bool findSweepGroupEdges() override;

 private:
#ifdef JS_GC_ZEAL
  // Let the weak map marking verifier know that this map can
  // contain keys in other zones.
  virtual bool allowKeysInOtherZones() const override { return true; }
#endif
};

class LeaveDebuggeeNoExecute;

class MOZ_RAII EvalOptions {
  JS::UniqueChars filename_;
  unsigned lineno_ = 1;
  bool hideFromDebugger_ = false;

 public:
  EvalOptions() = default;
  ~EvalOptions() = default;
  const char* filename() const { return filename_.get(); }
  unsigned lineno() const { return lineno_; }
  bool hideFromDebugger() const { return hideFromDebugger_; }
  [[nodiscard]] bool setFilename(JSContext* cx, const char* filename);
  void setLineno(unsigned lineno) { lineno_ = lineno; }
  void setHideFromDebugger(bool hide) { hideFromDebugger_ = hide; }
};

/*
 * Env is the type of what ECMA-262 calls "lexical environments" (the records
 * that represent scopes and bindings). See vm/EnvironmentObject.h.
 *
 * This is JSObject rather than js::EnvironmentObject because GlobalObject and
 * some proxies, despite not being in the EnvironmentObject class hierarchy,
 * can be in environment chains.
 */
using Env = JSObject;

// The referent of a Debugger.Script.
//
// - For most scripts, we point at their BaseScript.
//
// - For Web Assembly instances for which we are presenting a script-like
//   interface, we point at their WasmInstanceObject.
//
// The DebuggerScript object itself simply stores a Cell* in its private
// pointer, but when we're working with that pointer in C++ code, we'd rather
// not pass around a Cell* and be constantly asserting that, yes, this really
// does point to something okay. Instead, we immediately build an instance of
// this type from the Cell* and use that instead, so we can benefit from
// Variant's static checks.
typedef mozilla::Variant<BaseScript*, WasmInstanceObject*>
    DebuggerScriptReferent;

// The referent of a Debugger.Source.
//
// - For most sources, this is a ScriptSourceObject.
//
// - For Web Assembly instances for which we are presenting a source-like
//   interface, we point at their WasmInstanceObject.
//
// The DebuggerSource object actually simply stores a Cell* in its private
// pointer. See the comments for DebuggerScriptReferent for the rationale for
// this type.
typedef mozilla::Variant<ScriptSourceObject*, WasmInstanceObject*>
    DebuggerSourceReferent;

template <typename HookIsEnabledFun /* bool (Debugger*) */>
class MOZ_RAII DebuggerList {
 private:
  // Note: In the general case, 'debuggers' contains references to objects in
  // different compartments--every compartment *except* the debugger's.
  RootedValueVector debuggers;
  HookIsEnabledFun hookIsEnabled;

 public:
  /**
   * The hook function will be called during `init()` to build the list of
   * active debuggers, and again during dispatch to validate that the hook is
   * still active for the given debugger.
   */
  DebuggerList(JSContext* cx, HookIsEnabledFun hookIsEnabled)
      : debuggers(cx), hookIsEnabled(hookIsEnabled) {}

  [[nodiscard]] bool init(JSContext* cx);

  bool empty() { return debuggers.empty(); }

  template <typename FireHookFun /* ResumeMode (Debugger*) */>
  bool dispatchHook(JSContext* cx, FireHookFun fireHook);

  template <typename FireHookFun /* void (Debugger*) */>
  void dispatchQuietHook(JSContext* cx, FireHookFun fireHook);

  template <typename FireHookFun /* bool (Debugger*, ResumeMode&, MutableHandleValue) */>
  [[nodiscard]] bool dispatchResumptionHook(JSContext* cx,
                                           AbstractFramePtr frame,
                                           FireHookFun fireHook);
};

// The Debugger.prototype object.
class DebuggerPrototypeObject : public NativeObject {
 public:
  static const JSClass class_;
};

class DebuggerInstanceObject : public NativeObject {
 private:
  static const JSClassOps classOps_;

 public:
  static const JSClass class_;
};

class Debugger : private mozilla::LinkedListElement<Debugger> {
  friend class DebugAPI;
  friend class Breakpoint;
  friend class DebuggerFrame;
  friend class DebuggerMemory;
  friend class DebuggerInstanceObject;

  template <typename>
  friend class DebuggerList;
  friend struct JSRuntime::GlobalObjectWatchersLinkAccess<Debugger>;
  friend struct JSRuntime::GarbageCollectionWatchersLinkAccess<Debugger>;
  friend class SavedStacks;
  friend class ScriptedOnStepHandler;
  friend class ScriptedOnPopHandler;
  friend class mozilla::LinkedListElement<Debugger>;
  friend class mozilla::LinkedList<Debugger>;
  friend bool(::JS_DefineDebuggerObject)(JSContext* cx, JS::HandleObject obj);
  friend bool(::JS::dbg::IsDebugger)(JSObject&);
  friend bool(::JS::dbg::GetDebuggeeGlobals)(JSContext*, JSObject&,
                                             MutableHandleObjectVector);
  friend bool JS::dbg::FireOnGarbageCollectionHookRequired(JSContext* cx);
  friend bool JS::dbg::FireOnGarbageCollectionHook(
      JSContext* cx, JS::dbg::GarbageCollectionEvent::Ptr&& data);

 public:
  enum Hook {
    OnDebuggerStatement,
    OnExceptionUnwind,
    OnNewScript,
    OnEnterFrame,
    OnNativeCall,
    OnNewGlobalObject,
    OnNewPromise,
    OnPromiseSettled,
    OnGarbageCollection,
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
    JSSLOT_DEBUG_DEBUGGER = JSSLOT_DEBUG_PROTO_STOP,
    JSSLOT_DEBUG_HOOK_START,
    JSSLOT_DEBUG_HOOK_STOP = JSSLOT_DEBUG_HOOK_START + HookCount,
    JSSLOT_DEBUG_MEMORY_INSTANCE = JSSLOT_DEBUG_HOOK_STOP,
    JSSLOT_DEBUG_DEBUGGEE_LINK,
    JSSLOT_DEBUG_COUNT
  };

  // Bring DebugAPI::IsObserving into the Debugger namespace.
  using IsObserving = DebugAPI::IsObserving;
  static const IsObserving Observing = DebugAPI::Observing;
  static const IsObserving NotObserving = DebugAPI::NotObserving;

  // Return true if the given realm is a debuggee of this debugger,
  // false otherwise.
  bool isDebuggeeUnbarriered(const Realm* realm) const;

  // Return true if this Debugger observed a debuggee that participated in the
  // GC identified by the given GC number. Return false otherwise.
  // May return false negatives if we have hit OOM.
  bool observedGC(uint64_t majorGCNumber) const {
    return observedGCs.has(majorGCNumber);
  }

  // Notify this Debugger that one or more of its debuggees is participating
  // in the GC identified by the given GC number.
  bool debuggeeIsBeingCollected(uint64_t majorGCNumber) {
    return observedGCs.put(majorGCNumber);
  }

  static SavedFrame* getObjectAllocationSite(JSObject& obj);

  struct AllocationsLogEntry {
    AllocationsLogEntry(HandleObject frame, mozilla::TimeStamp when,
                        const char* className, size_t size, bool inNursery)
        : frame(frame),
          when(when),
          className(className),
          size(size),
          inNursery(inNursery) {
      MOZ_ASSERT_IF(frame, UncheckedUnwrap(frame)->is<SavedFrame>() ||
                               IsDeadProxyObject(frame));
    }

    HeapPtr<JSObject*> frame;
    mozilla::TimeStamp when;
    const char* className;
    size_t size;
    bool inNursery;

    void trace(JSTracer* trc) {
      TraceNullableEdge(trc, &frame, "Debugger::AllocationsLogEntry::frame");
    }
  };

 private:
  HeapPtr<NativeObject*> object; /* The Debugger object. Strong reference. */
  WeakGlobalObjectSet
      debuggees; /* Debuggee globals. Cross-compartment weak references. */
  JS::ZoneSet debuggeeZones; /* Set of zones that we have debuggees in. */
  HeapPtr<JSObject*> uncaughtExceptionHook; /* Strong reference. */
  bool allowUnobservedAsmJS;
  bool allowUnobservedWasm;

  // Whether to enable code coverage on the Debuggee.
  bool collectCoverageInfo;

  template <typename T>
  struct DebuggerLinkAccess {
    static mozilla::DoublyLinkedListElement<T>& Get(T* aThis) {
      return aThis->debuggerLink;
    }
  };

  // List of all js::Breakpoints in this debugger.
  using BreakpointList =
      mozilla::DoublyLinkedList<js::Breakpoint,
                                DebuggerLinkAccess<js::Breakpoint>>;
  BreakpointList breakpoints;

  // The set of GC numbers for which one or more of this Debugger's observed
  // debuggees participated in.
  using GCNumberSet =
      HashSet<uint64_t, DefaultHasher<uint64_t>, ZoneAllocPolicy>;
  GCNumberSet observedGCs;

  using AllocationsLog = js::TraceableFifo<AllocationsLogEntry>;

  AllocationsLog allocationsLog;
  bool trackingAllocationSites;
  double allocationSamplingProbability;
  size_t maxAllocationsLogLength;
  bool allocationsLogOverflowed;

  static const size_t DEFAULT_MAX_LOG_LENGTH = 5000;

  [[nodiscard]] bool appendAllocationSite(JSContext* cx, HandleObject obj,
                                          Handle<SavedFrame*> frame,
                                          mozilla::TimeStamp when);

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
   * Add allocations tracking for objects allocated within the given
   * debuggee's compartment. The given debuggee global must be observed by at
   * least one Debugger that is tracking allocations.
   */
  [[nodiscard]] static bool addAllocationsTracking(
      JSContext* cx, Handle<GlobalObject*> debuggee);

  /*
   * Remove allocations tracking for objects allocated within the given
   * global's compartment. This is a no-op if there are still Debuggers
   * observing this global and who are tracking allocations.
   */
  static void removeAllocationsTracking(GlobalObject& global);

  /*
   * Add or remove allocations tracking for all debuggees.
   */
  [[nodiscard]] bool addAllocationsTrackingForAllDebuggees(JSContext* cx);
  void removeAllocationsTrackingForAllDebuggees();

  /*
   * If this Debugger has a onNewGlobalObject handler, then
   * this link is inserted into the list headed by
   * JSRuntime::onNewGlobalObjectWatchers.
   */
  mozilla::DoublyLinkedListElement<Debugger> onNewGlobalObjectWatchersLink;

  /*
   * If this Debugger has a onGarbageCollection handler, then
   * this link is inserted into the list headed by
   * JSRuntime::onGarbageCollectionWatchers.
   */
  mozilla::DoublyLinkedListElement<Debugger> onGarbageCollectionWatchersLink;

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
  typedef HashMap<AbstractFramePtr, HeapPtr<DebuggerFrame*>,
                  DefaultHasher<AbstractFramePtr>, ZoneAllocPolicy>
      FrameMap;
  FrameMap frames;

  /*
   * Map from generator objects to their Debugger.Frame instances.
   *
   * When a Debugger.Frame is created for a generator frame, it is added to
   * this map and remains there for the lifetime of the generator, whether
   * that frame is on the stack at the moment or not.  This is in addition to
   * the entry in `frames` that exists as long as the generator frame is on
   * the stack.
   *
   * We need to keep the Debugger.Frame object alive to deliver it to the
   * onEnterFrame handler on resume, and to retain onStep and onPop hooks.
   *
   * An entry is present in this table when:
   *  - both the debuggee generator object and the Debugger.Frame object exists
   *  - the debuggee generator object belongs to a realm that is a debuggee of
   *    the Debugger.Frame's owner.
   *
   * regardless of whether the frame is currently suspended. (This list is
   * meant to explain why we update the table in the particular places where
   * we do so.)
   *
   * An entry in this table exists if and only if the Debugger.Frame's
   * GENERATOR_INFO_SLOT is set.
   */
  typedef DebuggerWeakMap<AbstractGeneratorObject, DebuggerFrame>
      GeneratorWeakMap;
  GeneratorWeakMap generatorFrames;

  // An ephemeral map from BaseScript* to Debugger.Script instances.
  using ScriptWeakMap = DebuggerWeakMap<BaseScript, DebuggerScript>;
  ScriptWeakMap scripts;

  using BaseScriptVector = JS::GCVector<BaseScript*>;

  // The map from debuggee source script objects to their Debugger.Source
  // instances.
  typedef DebuggerWeakMap<ScriptSourceObject, DebuggerSource, true>
      SourceWeakMap;
  SourceWeakMap sources;

  // The map from debuggee objects to their Debugger.Object instances.
  typedef DebuggerWeakMap<JSObject, DebuggerObject> ObjectWeakMap;
  ObjectWeakMap objects;

  // The map from debuggee Envs to Debugger.Environment instances.
  typedef DebuggerWeakMap<JSObject, DebuggerEnvironment> EnvironmentWeakMap;
  EnvironmentWeakMap environments;

  // The map from WasmInstanceObjects to synthesized Debugger.Script
  // instances.
  typedef DebuggerWeakMap<WasmInstanceObject, DebuggerScript>
      WasmInstanceScriptWeakMap;
  WasmInstanceScriptWeakMap wasmInstanceScripts;

  // The map from WasmInstanceObjects to synthesized Debugger.Source
  // instances.
  typedef DebuggerWeakMap<WasmInstanceObject, DebuggerSource>
      WasmInstanceSourceWeakMap;
  WasmInstanceSourceWeakMap wasmInstanceSources;

  class QueryBase;
  class ScriptQuery;
  class SourceQuery;
  class ObjectQuery;

  enum class FromSweep { No, Yes };

  [[nodiscard]] bool addDebuggeeGlobal(JSContext* cx,
                                       Handle<GlobalObject*> obj);
  void removeDebuggeeGlobal(JS::GCContext* gcx, GlobalObject* global,
                            WeakGlobalObjectSet::Enum* debugEnum,
                            FromSweep fromSweep);

  /*
   * Handle the result of a hook that is expected to return a resumption
   * value <https://wiki.mozilla.org/Debugger#Resumption_Values>. This is
   * called when we return from a debugging hook to debuggee code.
   *
   * If `success` is false, the hook failed. If an exception is pending in
   * ar.context(), attempt to handle it via the uncaught exception hook,
   * otherwise report it to the AutoRealm's global.
   *
   * If `success` is true, there must be no exception pending in ar.context().
   * `rv` may be:
   *
   *     undefined - Set `resultMode` to `ResumeMode::Continue` to continue
   *         execution normally.
   *
   *     {return: value} or {throw: value} - Call unwrapDebuggeeValue to
   *         unwrap `value`. Store the result in `vp` and set `resultMode` to
   *         `ResumeMode::Return` or `ResumeMode::Throw`. The interpreter
   *         will force the current frame to return or throw an exception.
   *
   *     null - Set `resultMode` to `ResumeMode::Terminate` to terminate the
   *         debuggee with an uncatchable error.
   *
   *     anything else - Make a new TypeError the pending exception and
   *         attempt to handle it with the uncaught exception handler.
   */
  [[nodiscard]] bool processHandlerResult(
      JSContext* cx, bool success, HandleValue rv, AbstractFramePtr frame,
      jsbytecode* pc, ResumeMode& resultMode, MutableHandleValue vp);

  [[nodiscard]] bool processParsedHandlerResult(
      JSContext* cx, AbstractFramePtr frame, const jsbytecode* pc, bool success,
      ResumeMode resumeMode, HandleValue value, ResumeMode& resultMode,
      MutableHandleValue vp);

  /**
   * Given a resumption return value from a hook, parse and validate it based
   * on the given frame, and split the result into a ResumeMode and Value.
   */
  [[nodiscard]] bool prepareResumption(JSContext* cx, AbstractFramePtr frame,
                                       const jsbytecode* pc,
                                       ResumeMode& resumeMode,
                                       MutableHandleValue vp);

  /**
   * If there is a pending exception and a handler, call the handler with the
   * exception so that it can attempt to resolve the error.
   */
  [[nodiscard]] bool callUncaughtExceptionHandler(JSContext* cx,
                                                  MutableHandleValue vp);

  /**
   * If the context has a pending exception, report it to the current global.
   */
  void reportUncaughtException(JSContext* cx);

  /*
   * Call the uncaught exception handler if there is one, returning true
   * if it handled the error, or false otherwise.
   */
  [[nodiscard]] bool handleUncaughtException(JSContext* cx);

  GlobalObject* unwrapDebuggeeArgument(JSContext* cx, const Value& v);

  static void traceObject(JSTracer* trc, JSObject* obj);

  void trace(JSTracer* trc);

  void traceForMovingGC(JSTracer* trc);
  void traceCrossCompartmentEdges(JSTracer* tracer);

 private:
  template <typename F>
  void forEachWeakMap(const F& f);

  [[nodiscard]] static bool getHookImpl(JSContext* cx, const CallArgs& args,
                                        Debugger& dbg, Hook which);
  [[nodiscard]] static bool setHookImpl(JSContext* cx, const CallArgs& args,
                                        Debugger& dbg, Hook which);

  [[nodiscard]] static bool getGarbageCollectionHook(JSContext* cx,
                                                     const CallArgs& args,
                                                     Debugger& dbg);
  [[nodiscard]] static bool setGarbageCollectionHook(JSContext* cx,
                                                     const CallArgs& args,
                                                     Debugger& dbg);

  static bool isCompilableUnit(JSContext* cx, unsigned argc, Value* vp);
  static bool recordReplayProcessKind(JSContext* cx, unsigned argc, Value* vp);
  static bool construct(JSContext* cx, unsigned argc, Value* vp);

  struct CallData;

  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSFunctionSpec static_methods[];

  /**
   * Suspend the DebuggerFrame, clearing on-stack data but leaving it linked
   * with the AbstractGeneratorObject so it can be re-used later.
   */
  static void suspendGeneratorDebuggerFrames(JSContext* cx,
                                             AbstractFramePtr frame);

  /**
   * Terminate the DebuggerFrame, clearing all data associated with the frame
   * so that it cannot be used to introspect stack frame data.
   */
  static void terminateDebuggerFrames(JSContext* cx, AbstractFramePtr frame);

  /**
   * Terminate a given DebuggerFrame, removing all internal state and all
   * references to the frame from the Debugger itself. If the frame is being
   * terminated while 'frames' or 'generatorFrames' are being iterated, pass a
   * pointer to the iteration Enum to remove the entry and ensure that iteration
   * behaves properly.
   *
   * The AbstractFramePtr may be omited in a call so long as it is either
   * called again later with the correct 'frame', or the frame itself has never
   * had on-stack data or a 'frames' entry and has never had an onStep handler.
   */
  static void terminateDebuggerFrame(
      JS::GCContext* gcx, Debugger* dbg, DebuggerFrame* dbgFrame,
      AbstractFramePtr frame, FrameMap::Enum* maybeFramesEnum = nullptr,
      GeneratorWeakMap::Enum* maybeGeneratorFramesEnum = nullptr);

  static bool updateExecutionObservabilityOfFrames(
      JSContext* cx, const DebugAPI::ExecutionObservableSet& obs,
      IsObserving observing);
  static bool updateExecutionObservabilityOfScripts(
      JSContext* cx, const DebugAPI::ExecutionObservableSet& obs,
      IsObserving observing);
  static bool updateExecutionObservability(
      JSContext* cx, DebugAPI::ExecutionObservableSet& obs,
      IsObserving observing);

  template <typename FrameFn /* void (Debugger*, DebuggerFrame*) */>
  static void forEachOnStackDebuggerFrame(AbstractFramePtr frame,
                                          const JS::AutoRequireNoGC& nogc,
                                          FrameFn fn);
  template <typename FrameFn /* void (Debugger*, DebuggerFrame*) */>
  static void forEachOnStackOrSuspendedDebuggerFrame(
      JSContext* cx, AbstractFramePtr frame, const JS::AutoRequireNoGC& nogc,
      FrameFn fn);

  /*
   * Return a vector containing all Debugger.Frame instances referring to
   * |frame|. |global| is |frame|'s global object; if nullptr or omitted, we
   * compute it ourselves from |frame|.
   */
  using DebuggerFrameVector = GCVector<DebuggerFrame*, 0, SystemAllocPolicy>;
  [[nodiscard]] static bool getDebuggerFrames(
      AbstractFramePtr frame, MutableHandle<DebuggerFrameVector> frames);

 public:
  // Public for DebuggerScript::setBreakpoint.
  [[nodiscard]] static bool ensureExecutionObservabilityOfScript(
      JSContext* cx, JSScript* script);

  // Whether the Debugger instance needs to observe all non-AOT JS
  // execution of its debugees.
  IsObserving observesAllExecution() const;

  // Whether the Debugger instance needs to observe AOT-compiled asm.js
  // execution of its debuggees.
  IsObserving observesAsmJS() const;

  // Whether the Debugger instance needs to observe compiled Wasm
  // execution of its debuggees.
  IsObserving observesWasm() const;

  // Whether the Debugger instance needs to observe coverage of any JavaScript
  // execution.
  IsObserving observesCoverage() const;

  // Whether the Debugger instance needs to observe native call invocations.
  IsObserving observesNativeCalls() const;

 private:
  [[nodiscard]] static bool ensureExecutionObservabilityOfFrame(
      JSContext* cx, AbstractFramePtr frame);
  [[nodiscard]] static bool ensureExecutionObservabilityOfRealm(
      JSContext* cx, JS::Realm* realm);

  static bool hookObservesAllExecution(Hook which);

  [[nodiscard]] bool updateObservesAllExecutionOnDebuggees(
      JSContext* cx, IsObserving observing);
  [[nodiscard]] bool updateObservesCoverageOnDebuggees(JSContext* cx,
                                                       IsObserving observing);
  void updateObservesAsmJSOnDebuggees(IsObserving observing);
  void updateObservesWasmOnDebuggees(IsObserving observing);

  JSObject* getHook(Hook hook) const;
  bool hasAnyLiveHooks() const;
  inline bool isHookCallAllowed(JSContext* cx) const;

  static void slowPathPromiseHook(JSContext* cx, Hook hook,
                                  Handle<PromiseObject*> promise);

  template <typename HookIsEnabledFun /* bool (Debugger*) */,
            typename FireHookFun /* void (Debugger*) */>
  static void dispatchQuietHook(JSContext* cx, HookIsEnabledFun hookIsEnabled,
                                FireHookFun fireHook);
  template <
      typename HookIsEnabledFun /* bool (Debugger*) */, typename FireHookFun /* bool (Debugger*, ResumeMode&, MutableHandleValue) */>
  [[nodiscard]] static bool dispatchResumptionHook(
      JSContext* cx, AbstractFramePtr frame, HookIsEnabledFun hookIsEnabled,
      FireHookFun fireHook);

  template <typename RunImpl /* bool () */>
  [[nodiscard]] bool enterDebuggerHook(JSContext* cx, RunImpl runImpl) {
    if (!isHookCallAllowed(cx)) {
      return true;
    }

    AutoRealm ar(cx, object);

    if (!runImpl()) {
      // We do not want errors within one hook to effect errors in other hooks,
      // so the only errors that we allow to propagate out of a debugger hook
      // are OOM errors and general terminations.
      if (!cx->isExceptionPending() || cx->isThrowingOutOfMemory()) {
        return false;
      }

      reportUncaughtException(cx);
    }
    MOZ_ASSERT(!cx->isExceptionPending());
    return true;
  }

  [[nodiscard]] bool fireDebuggerStatement(JSContext* cx,
                                           ResumeMode& resumeMode,
                                           MutableHandleValue vp);
  [[nodiscard]] bool fireExceptionUnwind(JSContext* cx, HandleValue exc,
                                         ResumeMode& resumeMode,
                                         MutableHandleValue vp);
  [[nodiscard]] bool fireEnterFrame(JSContext* cx, ResumeMode& resumeMode,
                                    MutableHandleValue vp);
  [[nodiscard]] bool fireNativeCall(JSContext* cx, const CallArgs& args,
                                    CallReason reason, ResumeMode& resumeMode,
                                    MutableHandleValue vp);
  [[nodiscard]] bool fireNewGlobalObject(JSContext* cx,
                                         Handle<GlobalObject*> global);
  [[nodiscard]] bool firePromiseHook(JSContext* cx, Hook hook,
                                     HandleObject promise);

  DebuggerScript* newVariantWrapper(JSContext* cx,
                                    Handle<DebuggerScriptReferent> referent) {
    return newDebuggerScript(cx, referent);
  }
  DebuggerSource* newVariantWrapper(JSContext* cx,
                                    Handle<DebuggerSourceReferent> referent) {
    return newDebuggerSource(cx, referent);
  }

  /*
   * Helper function to help wrap Debugger objects whose referents may be
   * variants. Currently Debugger.Script and Debugger.Source referents may
   * be variants.
   *
   * Prefer using wrapScript, wrapWasmScript, wrapSource, and wrapWasmSource
   * whenever possible.
   */
  template <typename ReferentType, typename Map>
  typename Map::WrapperType* wrapVariantReferent(
      JSContext* cx, Map& map,
      Handle<typename Map::WrapperType::ReferentVariant> referent);
  DebuggerScript* wrapVariantReferent(JSContext* cx,
                                      Handle<DebuggerScriptReferent> referent);
  DebuggerSource* wrapVariantReferent(JSContext* cx,
                                      Handle<DebuggerSourceReferent> referent);

  /*
   * Allocate and initialize a Debugger.Script instance whose referent is
   * |referent|.
   */
  DebuggerScript* newDebuggerScript(JSContext* cx,
                                    Handle<DebuggerScriptReferent> referent);

  /*
   * Allocate and initialize a Debugger.Source instance whose referent is
   * |referent|.
   */
  DebuggerSource* newDebuggerSource(JSContext* cx,
                                    Handle<DebuggerSourceReferent> referent);

  /*
   * Receive a "new script" event from the engine. A new script was compiled
   * or deserialized.
   */
  [[nodiscard]] bool fireNewScript(
      JSContext* cx, Handle<DebuggerScriptReferent> scriptReferent);

  /*
   * Receive a "garbage collection" event from the engine. A GC cycle with the
   * given data was recently completed.
   */
  [[nodiscard]] bool fireOnGarbageCollectionHook(
      JSContext* cx, const JS::dbg::GarbageCollectionEvent::Ptr& gcData);

  inline Breakpoint* firstBreakpoint() const;

  [[nodiscard]] static bool replaceFrameGuts(JSContext* cx,
                                             AbstractFramePtr from,
                                             AbstractFramePtr to,
                                             ScriptFrameIter& iter);

 public:
  Debugger(JSContext* cx, NativeObject* dbg);
  ~Debugger();

  inline const js::HeapPtr<NativeObject*>& toJSObject() const;
  inline js::HeapPtr<NativeObject*>& toJSObjectRef();
  static inline Debugger* fromJSObject(const JSObject* obj);

#ifdef DEBUG
  static bool isChildJSObject(JSObject* obj);
#endif

  Zone* zone() const { return toJSObject()->zone(); }

  bool hasMemory() const;
  DebuggerMemory& memory() const;

  WeakGlobalObjectSet::Range allDebuggees() const { return debuggees.all(); }

#ifdef DEBUG
  static bool isDebuggerCrossCompartmentEdge(JSObject* obj,
                                             const js::gc::Cell* cell);
#endif

  static bool hasLiveHook(GlobalObject* global, Hook which);

  /*** Functions for use by Debugger.cpp. *********************************/

  inline bool observesEnterFrame() const;
  inline bool observesNewScript() const;
  inline bool observesNewGlobalObject() const;
  inline bool observesGlobal(GlobalObject* global) const;
  bool observesFrame(AbstractFramePtr frame) const;
  bool observesFrame(const FrameIter& iter) const;
  bool observesScript(JSScript* script) const;
  bool observesWasm(wasm::Instance* instance) const;

  /*
   * If env is nullptr, call vp->setNull() and return true. Otherwise, find
   * or create a Debugger.Environment object for the given Env. On success,
   * store the Environment object in *vp and return true.
   */
  [[nodiscard]] bool wrapEnvironment(JSContext* cx, Handle<Env*> env,
                                     MutableHandleValue vp);
  [[nodiscard]] bool wrapEnvironment(
      JSContext* cx, Handle<Env*> env,
      MutableHandle<DebuggerEnvironment*> result);

  /*
   * Like cx->compartment()->wrap(cx, vp), but for the debugger realm.
   *
   * Preconditions: *vp is a value from a debuggee realm; cx is in the
   * debugger's compartment.
   *
   * If *vp is an object, this produces a (new or existing) Debugger.Object
   * wrapper for it. Otherwise this is the same as Compartment::wrap.
   *
   * If *vp is a magic JS_OPTIMIZED_OUT value, this produces a plain object
   * of the form { optimizedOut: true }.
   *
   * If *vp is a magic JS_MISSING_ARGUMENTS value signifying missing
   * arguments, this produces a plain object of the form { missingArguments:
   * true }.
   *
   * If *vp is a magic JS_UNINITIALIZED_LEXICAL value signifying an
   * unaccessible uninitialized binding, this produces a plain object of the
   * form { uninitialized: true }.
   */
  [[nodiscard]] bool wrapDebuggeeValue(JSContext* cx, MutableHandleValue vp);
  [[nodiscard]] bool wrapDebuggeeObject(JSContext* cx, HandleObject obj,
                                        MutableHandle<DebuggerObject*> result);
  [[nodiscard]] bool wrapNullableDebuggeeObject(
      JSContext* cx, HandleObject obj, MutableHandle<DebuggerObject*> result);

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
   *     enter debuggee realm;
   *     call cx->compartment()->wrap;  // compartment-rewrapping
   *
   * (Extreme nerd sidebar: Unwrapping happens in two steps because there are
   * two different kinds of symmetry at work: regardless of which direction
   * we're going, we want any exceptions to be created and thrown in the
   * debugger compartment--mirror symmetry. But compartment wrapping always
   * happens in the target compartment--rotational symmetry.)
   */
  [[nodiscard]] bool unwrapDebuggeeValue(JSContext* cx, MutableHandleValue vp);
  [[nodiscard]] bool unwrapDebuggeeObject(JSContext* cx,
                                          MutableHandleObject obj);
  [[nodiscard]] bool unwrapPropertyDescriptor(
      JSContext* cx, HandleObject obj, MutableHandle<PropertyDescriptor> desc);

  /*
   * Store the Debugger.Frame object for iter in *vp/result.
   *
   * If this Debugger does not already have a Frame object for the frame
   * `iter` points to, a new Frame object is created, and `iter`'s private
   * data is copied into it.
   */
  [[nodiscard]] bool getFrame(JSContext* cx, const FrameIter& iter,
                              MutableHandleValue vp);
  [[nodiscard]] bool getFrame(JSContext* cx,
                              MutableHandle<DebuggerFrame*> result);
  [[nodiscard]] bool getFrame(JSContext* cx, const FrameIter& iter,
                              MutableHandle<DebuggerFrame*> result);
  [[nodiscard]] bool getFrame(JSContext* cx,
                              Handle<AbstractGeneratorObject*> genObj,
                              MutableHandle<DebuggerFrame*> result);

  /*
   * Return the Debugger.Script object for |script|, or create a new one if
   * needed. The context |cx| must be in the debugger realm; |script| must be
   * a script in a debuggee realm.
   */
  DebuggerScript* wrapScript(JSContext* cx, Handle<BaseScript*> script);

  /*
   * Return the Debugger.Script object for |wasmInstance| (the toplevel
   * script), synthesizing a new one if needed. The context |cx| must be in
   * the debugger compartment; |wasmInstance| must be a WasmInstanceObject in
   * the debuggee realm.
   */
  DebuggerScript* wrapWasmScript(JSContext* cx,
                                 Handle<WasmInstanceObject*> wasmInstance);

  /*
   * Return the Debugger.Source object for |source|, or create a new one if
   * needed. The context |cx| must be in the debugger compartment; |source|
   * must be a script source object in a debuggee realm.
   */
  DebuggerSource* wrapSource(JSContext* cx,
                             js::Handle<ScriptSourceObject*> source);

  /*
   * Return the Debugger.Source object for |wasmInstance| (the entire module),
   * synthesizing a new one if needed. The context |cx| must be in the
   * debugger compartment; |wasmInstance| must be a WasmInstanceObject in the
   * debuggee realm.
   */
  DebuggerSource* wrapWasmSource(JSContext* cx,
                                 Handle<WasmInstanceObject*> wasmInstance);

  DebuggerDebuggeeLink* getDebuggeeLink();

 private:
  Debugger(const Debugger&) = delete;
  Debugger& operator=(const Debugger&) = delete;
};

// Specialize InternalBarrierMethods so we can have WeakHeapPtr<Debugger*>.
template <>
struct InternalBarrierMethods<Debugger*> {
  static bool isMarkable(Debugger* dbg) { return dbg->toJSObject(); }

  static void postBarrier(Debugger** vp, Debugger* prev, Debugger* next) {}

  static void readBarrier(Debugger* dbg) {
    InternalBarrierMethods<JSObject*>::readBarrier(dbg->toJSObject());
  }

#ifdef DEBUG
  static void assertThingIsNotGray(Debugger* dbg) {}
#endif
};

/**
 * This class exists for one specific reason. If a given Debugger object is in
 * a state where:
 *
 *   a) nothing in the system has a reference to the object
 *   b) the debugger is currently attached to a live debuggee
 *   c) the debugger has hooks like 'onEnterFrame'
 *
 * then we don't want the GC to delete the Debugger, because the system could
 * still call the hooks. This means we need to ensure that, whenever the global
 * gets marked, the Debugger will get marked as well. Critically, we _only_
 * want that to happen if the debugger has hooks. If it doesn't, then GCing
 * the debugger is the right think to do.
 *
 * Note that there are _other_ cases where the debugger may be held live, but
 * those are not addressed by this case.
 *
 * To accomplish this, we use a bit of roundabout link approach. Both the
 * Debugger and the debuggees can reach the link object:
 *
 *   Debugger  -> DebuggerDebuggeeLink  <- CCW <- Debuggee Global #1
 *      |                  |    ^   ^---<- CCW <- Debuggee Global #2
 *      \--<<-optional-<<--/     \------<- CCW <- Debuggee Global #3
 *
 * and critically, the Debugger is able to conditionally add or remove the link
 * going from the DebuggerDebuggeeLink _back_ to the Debugger. When this link
 * exists, the GC can trace all the way from the global to the Debugger,
 * meaning that any Debugger with this link will be kept alive as long as any
 * of its debuggees are alive.
 */
class DebuggerDebuggeeLink : public NativeObject {
 private:
  enum {
    DEBUGGER_LINK_SLOT,
    RESERVED_SLOTS,
  };

 public:
  static const JSClass class_;

  void setLinkSlot(Debugger& dbg);
  void clearLinkSlot();
};

/*
 * A Handler represents a Debugger API reflection object's handler function,
 * like a Debugger.Frame's onStep handler. These handler functions are called by
 * the Debugger API to notify the user of certain events. For each event type,
 * we define a separate subclass of Handler.
 *
 * When a reflection object accepts a Handler, it calls its 'hold' method; and
 * if the Handler is replaced by another, or the reflection object is finalized,
 * the reflection object calls the Handler's 'drop' method. The reflection
 * object does not otherwise manage the Handler's lifetime, say, by calling its
 * destructor or freeing its memory. A simple Handler implementation might have
 * an empty 'hold' method, and have its 'drop' method delete the Handler. A more
 * complex Handler might process many kinds of events, and thus inherit from
 * many Handler subclasses and be held by many reflection objects
 * simultaneously; a handler like this could use 'hold' and 'drop' to manage a
 * reference count.
 *
 * To support SpiderMonkey's memory use tracking, 'hold' and 'drop' also require
 * a pointer to the owning reflection object, so that the Holder implementation
 * can properly report changes in ownership to functions using the
 * js::gc::MemoryUse categories.
 */
struct Handler {
  virtual ~Handler() = default;

  /*
   * If this Handler is a reference to a callable JSObject, return that
   * JSObject. Otherwise, this method returns nullptr.
   *
   * The JavaScript getters for handler properties on reflection objects use
   * this method to obtain the callable the handler represents. When a Handler's
   * 'object' method returns nullptr, that handler is simply not visible to
   * JavaScript.
   */
  virtual JSObject* object() const = 0;

  /* Report that this Handler is now held by owner. See comment above. */
  virtual void hold(JSObject* owner) = 0;

  /* Report that this Handler is no longer held by owner. See comment above. */
  virtual void drop(JS::GCContext* gcx, JSObject* owner) = 0;

  /*
   * Trace the reference to the handler. This method will be called by the
   * reflection object holding this Handler whenever the former is traced.
   */
  virtual void trace(JSTracer* tracer) = 0;

  /* Allocation size in bytes for memory accounting purposes. */
  virtual size_t allocSize() const = 0;
};

class JSBreakpointSite;
class WasmBreakpointSite;

/**
 * Breakpoint GC rules:
 *
 * BreakpointSites and Breakpoints are owned by the code in which they are set.
 * Tracing a JSScript or WasmInstance traces all BreakpointSites set in it,
 * which traces all Breakpoints; and if the code is garbage collected, the
 * BreakpointSite and the Breakpoints set at it are freed as well. Doing so is
 * not observable to JS, since the handlers would never fire, and there is no
 * way to enumerate all breakpoints without specifying a specific script, in
 * which case it must not have been GC'd.
 *
 * Although BreakpointSites and Breakpoints are not GC things, they should be
 * treated as belonging to the code's compartment. This means that the
 * BreakpointSite concrete subclasses' pointers to the code are not
 * cross-compartment references, but a Breakpoint's pointers to its handler and
 * owning Debugger are cross-compartment references, and go through
 * cross-compartment wrappers.
 */

/**
 * A location in a JSScript or WasmInstance at which we have breakpoints. A
 * BreakpointSite owns a linked list of all the breakpoints set at its location.
 * In general, this list contains breakpoints set by multiple Debuggers in
 * various compartments.
 *
 * BreakpointSites are created only as needed, for locations at which
 * breakpoints are currently set. When the last breakpoint is removed from a
 * location, the BreakpointSite is removed as well.
 *
 * This is an abstract base class, with subclasses specialized for the different
 * sorts of code a breakpoint might be set in. JSBreakpointSite manages sites in
 * JSScripts, and WasmBreakpointSite manages sites in WasmInstances.
 */
class BreakpointSite {
  friend class DebugAPI;
  friend class Breakpoint;
  friend class Debugger;

 private:
  template <typename T>
  struct SiteLinkAccess {
    static mozilla::DoublyLinkedListElement<T>& Get(T* aThis) {
      return aThis->siteLink;
    }
  };

  // List of all js::Breakpoints at this instruction.
  using BreakpointList =
      mozilla::DoublyLinkedList<js::Breakpoint, SiteLinkAccess<js::Breakpoint>>;
  BreakpointList breakpoints;

 protected:
  BreakpointSite() = default;
  virtual ~BreakpointSite() = default;
  void finalize(JS::GCContext* gcx);
  virtual gc::Cell* owningCell() = 0;

 public:
  Breakpoint* firstBreakpoint() const;
  bool hasBreakpoint(Breakpoint* bp);

  bool isEmpty() const;
  virtual void trace(JSTracer* trc);
  virtual void remove(JS::GCContext* gcx) = 0;
  void destroyIfEmpty(JS::GCContext* gcx) {
    if (isEmpty()) {
      remove(gcx);
    }
  }
  virtual Realm* realm() const = 0;
};

/*
 * A breakpoint set at a given BreakpointSite, indicating the owning debugger
 * and the handler object. A Breakpoint is a member of two linked lists: its
 * owning debugger's list and its site's list.
 */
class Breakpoint {
  friend class DebugAPI;
  friend class Debugger;
  friend class BreakpointSite;

 public:
  /* Our owning debugger. */
  Debugger* const debugger;

  /**
   * A cross-compartment wrapper for our owning debugger's object, a CCW in the
   * code's compartment to the Debugger object in its own compartment. Holding
   * this lets the GC know about the effective cross-compartment reference from
   * the code to the debugger; see "Breakpoint GC Rules", above.
   *
   * This is almost redundant with the `debugger` field, except that we need
   * access to our owning `Debugger` regardless of the relative privilege levels
   * of debugger and debuggee, regardless of whether we're in the midst of a GC,
   * and so on - unwrapping is just too entangled.
   */
  const HeapPtr<JSObject*> wrappedDebugger;

  /* The site at which we're inserted. */
  BreakpointSite* const site;

 private:
  /**
   * The breakpoint handler object, via a cross-compartment wrapper in the
   * code's compartment.
   *
   * Although eventually we would like this to be a `js::Handler` instance, for
   * now it is just cross-compartment wrapper for the JS object supplied to
   * `setBreakpoint`, hopefully with a callable `hit` property.
   */
  const HeapPtr<JSObject*> handler;

  /**
   * Link elements for each list this breakpoint can be in.
   */
  mozilla::DoublyLinkedListElement<Breakpoint> debuggerLink;
  mozilla::DoublyLinkedListElement<Breakpoint> siteLink;

  void trace(JSTracer* trc);

 public:
  Breakpoint(Debugger* debugger, HandleObject wrappedDebugger,
             BreakpointSite* site, HandleObject handler);

  enum MayDestroySite { False, True };

  /**
   * Unlink this breakpoint from its Debugger's and and BreakpointSite's lists,
   * and free its memory.
   *
   * This is the low-level primitive shared by breakpoint removal and script
   * finalization code. It is only concerned with cleaning up this Breakpoint;
   * it does not check for now-empty BreakpointSites, unneeded DebugScripts, or
   * the like.
   */
  void delete_(JS::GCContext* gcx);

  /**
   * Remove this breakpoint. Unlink it from its Debugger's and BreakpointSite's
   * lists, and if the BreakpointSite is now empty, clean that up and update JIT
   * code as necessary.
   */
  void remove(JS::GCContext* gcx);

  Breakpoint* nextInDebugger();
  Breakpoint* nextInSite();
  JSObject* getHandler() const { return handler; }
};

class JSBreakpointSite : public BreakpointSite {
 public:
  const HeapPtr<JSScript*> script;
  jsbytecode* const pc;

 public:
  JSBreakpointSite(JSScript* script, jsbytecode* pc);

  void trace(JSTracer* trc) override;
  void delete_(JS::GCContext* gcx);
  void remove(JS::GCContext* gcx) override;
  Realm* realm() const override;

 private:
  gc::Cell* owningCell() override;
};

class WasmBreakpointSite : public BreakpointSite {
 public:
  const HeapPtr<WasmInstanceObject*> instanceObject;
  uint32_t offset;

 public:
  WasmBreakpointSite(WasmInstanceObject* instanceObject, uint32_t offset);

  void trace(JSTracer* trc) override;
  void delete_(JS::GCContext* gcx);
  void remove(JS::GCContext* gcx) override;
  Realm* realm() const override;

 private:
  gc::Cell* owningCell() override;
};

Breakpoint* Debugger::firstBreakpoint() const {
  if (breakpoints.isEmpty()) {
    return nullptr;
  }
  return &(*breakpoints.begin());
}

const js::HeapPtr<NativeObject*>& Debugger::toJSObject() const {
  MOZ_ASSERT(object);
  return object;
}

js::HeapPtr<NativeObject*>& Debugger::toJSObjectRef() {
  MOZ_ASSERT(object);
  return object;
}

bool Debugger::observesEnterFrame() const { return getHook(OnEnterFrame); }

bool Debugger::observesNewScript() const { return getHook(OnNewScript); }

bool Debugger::observesNewGlobalObject() const {
  return getHook(OnNewGlobalObject);
}

bool Debugger::observesGlobal(GlobalObject* global) const {
  WeakHeapPtr<GlobalObject*> debuggee(global);
  return debuggees.has(debuggee);
}

[[nodiscard]] bool ReportObjectRequired(JSContext* cx);

JSObject* IdVectorToArray(JSContext* cx, HandleIdVector ids);
bool IsInterpretedNonSelfHostedFunction(JSFunction* fun);
JSScript* GetOrCreateFunctionScript(JSContext* cx, HandleFunction fun);
ArrayObject* GetFunctionParameterNamesArray(JSContext* cx, HandleFunction fun);
bool ValueToIdentifier(JSContext* cx, HandleValue v, MutableHandleId id);
bool ValueToStableChars(JSContext* cx, const char* fnname, HandleValue value,
                        JS::AutoStableStringChars& stableChars);
bool ParseEvalOptions(JSContext* cx, HandleValue value, EvalOptions& options);

Result<Completion> DebuggerGenericEval(
    JSContext* cx, const mozilla::Range<const char16_t> chars,
    HandleObject bindings, const EvalOptions& options, Debugger* dbg,
    HandleObject envArg, FrameIter* iter);

bool ParseResumptionValue(JSContext* cx, HandleValue rval,
                          ResumeMode& resumeMode, MutableHandleValue vp);

#define JS_DEBUG_PSG(Name, Getter) \
  JS_PSG(Name, CallData::ToNative<&CallData::Getter>, 0)

#define JS_DEBUG_PSGS(Name, Getter, Setter)            \
  JS_PSGS(Name, CallData::ToNative<&CallData::Getter>, \
          CallData::ToNative<&CallData::Setter>, 0)

#define JS_DEBUG_FN(Name, Method, NumArgs) \
  JS_FN(Name, CallData::ToNative<&CallData::Method>, NumArgs, 0)

} /* namespace js */

#endif /* debugger_Debugger_h */
