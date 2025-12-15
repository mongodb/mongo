/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_DebugAPI_h
#define debugger_DebugAPI_h

#include "js/Debug.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"

namespace js {

// This file contains the API which SpiderMonkey should use to interact with any
// active Debuggers.

class AbstractGeneratorObject;
class DebugScriptMap;
class PromiseObject;

namespace gc {
class AutoSuppressGC;
}  // namespace gc

/**
 * DebugAPI::onNativeCall allows the debugger to call callbacks just before
 * some native functions are to be executed. It also allows the hooks
 * themselves to affect the result of the call. This enum represents the
 * various affects that DebugAPI::onNativeCall may perform.
 */
enum class NativeResumeMode {
  /**
   * If the debugger hook did not return a value to manipulate the result of
   * the native call, execution can continue unchanged.
   *
   * Continue indicates that the native function should execute normally.
   */
  Continue,

  /**
   * If the debugger hook returned an explicit return value that is meant to
   * take the place of the native call's result, execution of the native
   * function needs to be skipped in favor of the explicit result.
   *
   * Override indicates that the native function should be skipped and that
   * the debugger has already stored the return value into the CallArgs.
   */
  Override,

  /**
   * If the debugger hook returns an explicit termination or an explicit
   * thrown exception, execution of the native function needs to be skipped
   * in favor of handling the error condition.
   *
   * Abort indicates that the native function should be skipped and that
   * execution should be terminated. The debugger may or may not have set a
   * pending exception.
   */
  Abort,
};

class DebugScript;
class DebuggerVector;

class DebugAPI {
 public:
  friend class Debugger;

  /*** Methods for interaction with the GC. ***********************************/

  /*
   * Trace (inferred) owning edges from stack frames to Debugger.Frames, as part
   * of root marking.
   *
   * Even if a Debugger.Frame for a live stack frame is entirely unreachable
   * from JS, if it has onStep or onPop hooks set, then collecting it would have
   * observable side effects - namely, the hooks would fail to run. The effect
   * is the same as if the stack frame held an owning edge to its
   * Debugger.Frame.
   *
   * Debugger.Frames must also be retained if the Debugger to which they belong
   * is reachable, even if they have no hooks set, but we handle that elsewhere;
   * this function is only concerned with the inferred roots from stack frames
   * to Debugger.Frames that have hooks set.
   */
  static void traceFramesWithLiveHooks(JSTracer* tracer);

  /*
   * Trace (inferred) owning edges from generator objects to Debugger.Frames.
   *
   * Even if a Debugger.Frame for a live suspended generator object is entirely
   * unreachable from JS, if it has onStep or onPop hooks set, then collecting
   * it would have observable side effects - namely, the hooks would fail to run
   * if the generator is resumed. The effect is the same as if the generator
   * object held an owning edge to its Debugger.Frame.
   */
  static inline void traceGeneratorFrame(JSTracer* tracer,
                                         AbstractGeneratorObject* generator);

  // Trace cross compartment edges in all debuggers relevant to the current GC.
  static void traceCrossCompartmentEdges(JSTracer* tracer);

  // Trace all debugger-owned GC things unconditionally, during a moving GC.
  static void traceAllForMovingGC(JSTracer* trc);

  // Trace the debug script map.  Called as part of tracing a zone's roots.
  static void traceDebugScriptMap(JSTracer* trc, DebugScriptMap* map);

  static void traceFromRealm(JSTracer* trc, Realm* realm);

  // The garbage collector calls this after everything has been marked, but
  // before anything has been finalized. We use this to clear Debugger /
  // debuggee edges at a point where the parties concerned are all still
  // initialized. This does not update edges to moved GC things which is handled
  // via the other trace methods.
  static void sweepAll(JS::GCContext* gcx);

  // Add sweep group edges due to the presence of any debuggers.
  [[nodiscard]] static bool findSweepGroupEdges(JSRuntime* rt);

  // Remove the debugging information associated with a script.
  static void removeDebugScript(JS::GCContext* gcx, JSScript* script);

  // Delete a Zone's debug script map. Called when a zone is destroyed.
  static void deleteDebugScriptMap(DebugScriptMap* map);

  // Validate the debugging information in a script after a moving GC>
#ifdef JSGC_HASH_TABLE_CHECKS
  static void checkDebugScriptAfterMovingGC(DebugScript* ds);
#endif

#ifdef DEBUG
  static bool edgeIsInDebuggerWeakmap(JSRuntime* rt, JSObject* src,
                                      JS::GCCellPtr dst);
#endif

  /*** Methods for querying script breakpoint state. **************************/

  // Query information about whether any debuggers are observing a script.
  static inline bool stepModeEnabled(JSScript* script);
  static inline bool hasBreakpointsAt(JSScript* script, jsbytecode* pc);
  static inline bool hasAnyBreakpointsOrStepMode(JSScript* script);

  /*** Methods for interacting with the JITs. *********************************/

  // Update Debugger frames when an interpreter frame is replaced with a
  // baseline frame.
  [[nodiscard]] static bool handleBaselineOsr(JSContext* cx,
                                              InterpreterFrame* from,
                                              jit::BaselineFrame* to);

  // Update Debugger frames when an Ion frame bails out and is replaced with a
  // baseline frame.
  [[nodiscard]] static bool handleIonBailout(JSContext* cx,
                                             jit::RematerializedFrame* from,
                                             jit::BaselineFrame* to);

  // Detach any Debugger frames from an Ion frame after an error occurred while
  // it bailed out.
  static void handleUnrecoverableIonBailoutError(
      JSContext* cx, jit::RematerializedFrame* frame);

  // When doing on-stack-replacement of a debuggee interpreter frame with a
  // baseline frame, ensure that the resulting frame can be observed by the
  // debugger.
  [[nodiscard]] static bool ensureExecutionObservabilityOfOsrFrame(
      JSContext* cx, AbstractFramePtr osrSourceFrame);

  // Describes a set of scripts or frames whose execution observability can
  // change due to debugger activity.
  class ExecutionObservableSet {
   public:
    using ZoneRange = HashSet<Zone*>::Range;

    virtual Zone* singleZone() const { return nullptr; }
    virtual JSScript* singleScriptForZoneInvalidation() const {
      return nullptr;
    }
    virtual const HashSet<Zone*>* zones() const { return nullptr; }

    virtual bool shouldRecompileOrInvalidate(JSScript* script) const = 0;
    virtual bool shouldMarkAsDebuggee(FrameIter& iter) const = 0;
  };

  // This enum is converted to and compare with bool values; NotObserving
  // must be 0 and Observing must be 1.
  enum IsObserving { NotObserving = 0, Observing = 1 };

  /*** Methods for calling installed debugger handlers. ***********************/

  // Called when a new script becomes accessible to debuggers.
  static void onNewScript(JSContext* cx, HandleScript script);

  // Called when a new wasm instance becomes accessible to debuggers.
  static inline void onNewWasmInstance(
      JSContext* cx, Handle<WasmInstanceObject*> wasmInstance);

  /*
   * Announce to the debugger that the context has entered a new JavaScript
   * frame, |frame|. Call whatever hooks have been registered to observe new
   * frames.
   */
  [[nodiscard]] static inline bool onEnterFrame(JSContext* cx,
                                                AbstractFramePtr frame);

  /*
   * Like onEnterFrame, but for resuming execution of a generator or async
   * function. `frame` is a new baseline or interpreter frame, but abstractly
   * it can be identified with a particular generator frame that was
   * suspended earlier.
   *
   * There is no separate user-visible Debugger.onResumeFrame hook; this
   * fires .onEnterFrame (again, since we're re-entering the frame).
   *
   * Unfortunately, the interpreter and the baseline JIT arrange for this to
   * be called in different ways. The interpreter calls it from JSOp::Resume,
   * immediately after pushing the resumed frame; the JIT calls it from
   * JSOp::AfterYield, just after the generator resumes. The difference
   * should not be user-visible.
   */
  [[nodiscard]] static inline bool onResumeFrame(JSContext* cx,
                                                 AbstractFramePtr frame);

  // Called when Wasm frame is suspended by JS PI.
  static void onSuspendWasmFrame(JSContext* cx, wasm::DebugFrame* debugFrame);

  // Called when Wasm frame is resumed by JS PI.
  static void onResumeWasmFrame(JSContext* cx, const FrameIter& iter);

  static inline NativeResumeMode onNativeCall(JSContext* cx,
                                              const CallArgs& args,
                                              CallReason reason);

  static inline bool shouldAvoidSideEffects(JSContext* cx);

  /*
   * Announce to the debugger a |debugger;| statement on has been
   * encountered on the youngest JS frame on |cx|. Call whatever hooks have
   * been registered to observe this.
   *
   * Note that this method is called for all |debugger;| statements,
   * regardless of the frame's debuggee-ness.
   */
  [[nodiscard]] static inline bool onDebuggerStatement(JSContext* cx,
                                                       AbstractFramePtr frame);

  /*
   * Announce to the debugger that an exception has been thrown and propagated
   * to |frame|. Call whatever hooks have been registered to observe this.
   */
  [[nodiscard]] static inline bool onExceptionUnwind(JSContext* cx,
                                                     AbstractFramePtr frame);

  /*
   * Announce to the debugger that the thread has exited a JavaScript frame,
   * |frame|. If |ok| is true, the frame is returning normally; if |ok| is
   * false, the frame is throwing an exception or terminating.
   *
   * Change cx's current exception and |frame|'s return value to reflect the
   * changes in behavior the hooks request, if any. Return the new error/success
   * value.
   *
   * This function may be called twice for the same outgoing frame; only the
   * first call has any effect. (Permitting double calls simplifies some
   * cases where an onPop handler's resumption value changes a return to a
   * throw, or vice versa: we can redirect to a complete copy of the
   * alternative path, containing its own call to onLeaveFrame.)
   */
  [[nodiscard]] static inline bool onLeaveFrame(JSContext* cx,
                                                AbstractFramePtr frame,
                                                const jsbytecode* pc, bool ok);

  // Call any breakpoint handlers for the current scripted location.
  [[nodiscard]] static bool onTrap(JSContext* cx);

  // Call any stepping handlers for the current scripted location.
  [[nodiscard]] static bool onSingleStep(JSContext* cx);

  // Notify any Debugger instances observing this promise's global that a new
  // promise was allocated.
  static inline void onNewPromise(JSContext* cx,
                                  Handle<PromiseObject*> promise);

  // Notify any Debugger instances observing this promise's global that the
  // promise has settled (ie, it has either been fulfilled or rejected). Note
  // that this is *not* equivalent to the promise resolution (ie, the promise's
  // fate getting locked in) because you can resolve a promise with another
  // pending promise, in which case neither promise has settled yet.
  //
  // This should never be called on the same promise more than once, because a
  // promise can only make the transition from unsettled to settled once.
  static inline void onPromiseSettled(JSContext* cx,
                                      Handle<PromiseObject*> promise);

  // Notify any Debugger instances that a new global object has been created.
  static inline void onNewGlobalObject(JSContext* cx,
                                       Handle<GlobalObject*> global);

  /*** Methods for querying installed debugger handlers. **********************/

  // Whether any debugger is observing execution in a global.
  static bool debuggerObservesAllExecution(GlobalObject* global);

  // Whether any debugger is observing JS execution coverage in a global.
  static bool debuggerObservesCoverage(GlobalObject* global);

  // Whether any Debugger is observing asm.js execution in a global.
  static bool debuggerObservesAsmJS(GlobalObject* global);

  // Whether any Debugger is observing WebAssembly execution in a global.
  static bool debuggerObservesWasm(GlobalObject* global);

  // Whether any Debugger is observing native function call.
  static bool debuggerObservesNativeCall(GlobalObject* global);

  /*
   * Return true if the given global is being observed by at least one
   * Debugger that is tracking allocations.
   */
  static bool isObservedByDebuggerTrackingAllocations(
      const GlobalObject& debuggee);

  // If any debuggers are tracking allocations for a global, return the
  // probability that a given allocation should be tracked. Nothing otherwise.
  static mozilla::Maybe<double> allocationSamplingProbability(
      GlobalObject* global);

  // Whether any debugger is observing exception unwinds in a realm.
  static bool hasExceptionUnwindHook(GlobalObject* global);

  // Whether any debugger is observing debugger statements in a realm.
  static bool hasDebuggerStatementHook(GlobalObject* global);

  /*** Assorted methods for interacting with the runtime. *********************/

  // Checks if the current compartment is allowed to execute code.
  [[nodiscard]] static inline bool checkNoExecute(JSContext* cx,
                                                  HandleScript script);

  /*
   * Announce to the debugger that a generator object has been created,
   * via JSOp::Generator.
   *
   * This does not fire user hooks, but it's needed for debugger bookkeeping.
   */
  [[nodiscard]] static inline bool onNewGenerator(
      JSContext* cx, AbstractFramePtr frame,
      Handle<AbstractGeneratorObject*> genObj);

  static inline void onGeneratorClosed(JSContext* cx,
                                       AbstractGeneratorObject* genObj);

  // If necessary, record an object that was just allocated for any observing
  // debuggers.
  [[nodiscard]] static inline bool onLogAllocationSite(
      JSContext* cx, JSObject* obj, Handle<SavedFrame*> frame,
      mozilla::TimeStamp when);

  // Announce to the debugger that a global object is being collected by the
  // specified major GC.
  static inline void notifyParticipatesInGC(GlobalObject* global,
                                            uint64_t majorGCNumber);

 private:
  static bool stepModeEnabledSlow(JSScript* script);
  static bool hasBreakpointsAtSlow(JSScript* script, jsbytecode* pc);
  static void slowPathOnNewGlobalObject(JSContext* cx,
                                        Handle<GlobalObject*> global);
  static void slowPathNotifyParticipatesInGC(uint64_t majorGCNumber,
                                             JS::Realm::DebuggerVector& dbgs,
                                             const JS::AutoRequireNoGC& nogc);
  [[nodiscard]] static bool slowPathOnLogAllocationSite(
      JSContext* cx, HandleObject obj, Handle<SavedFrame*> frame,
      mozilla::TimeStamp when, JS::Realm::DebuggerVector& dbgs,
      const gc::AutoSuppressGC& nogc);
  [[nodiscard]] static bool slowPathOnLeaveFrame(JSContext* cx,
                                                 AbstractFramePtr frame,
                                                 const jsbytecode* pc, bool ok);
  [[nodiscard]] static bool slowPathOnNewGenerator(
      JSContext* cx, AbstractFramePtr frame,
      Handle<AbstractGeneratorObject*> genObj);
  static void slowPathOnGeneratorClosed(JSContext* cx,
                                        AbstractGeneratorObject* genObj);
  [[nodiscard]] static bool slowPathCheckNoExecute(JSContext* cx,
                                                   HandleScript script);
  [[nodiscard]] static bool slowPathOnEnterFrame(JSContext* cx,
                                                 AbstractFramePtr frame);
  [[nodiscard]] static bool slowPathOnResumeFrame(JSContext* cx,
                                                  AbstractFramePtr frame);
  static NativeResumeMode slowPathOnNativeCall(JSContext* cx,
                                               const CallArgs& args,
                                               CallReason reason);
  static bool slowPathShouldAvoidSideEffects(JSContext* cx);
  [[nodiscard]] static bool slowPathOnDebuggerStatement(JSContext* cx,
                                                        AbstractFramePtr frame);
  [[nodiscard]] static bool slowPathOnExceptionUnwind(JSContext* cx,
                                                      AbstractFramePtr frame);
  static void slowPathOnNewWasmInstance(
      JSContext* cx, Handle<WasmInstanceObject*> wasmInstance);
  static void slowPathOnNewPromise(JSContext* cx,
                                   Handle<PromiseObject*> promise);
  static void slowPathOnPromiseSettled(JSContext* cx,
                                       Handle<PromiseObject*> promise);
  static bool inFrameMaps(AbstractFramePtr frame);
  static void slowPathTraceGeneratorFrame(JSTracer* tracer,
                                          AbstractGeneratorObject* generator);
};

// Suppresses all debuggee NX checks, i.e., allow all execution. Used to allow
// certain whitelisted operations to execute code.
//
// WARNING
// WARNING Do not use this unless you know what you are doing!
// WARNING
class AutoSuppressDebuggeeNoExecuteChecks {
  EnterDebuggeeNoExecute** stack_;
  EnterDebuggeeNoExecute* prev_;

 public:
  explicit AutoSuppressDebuggeeNoExecuteChecks(JSContext* cx) {
    stack_ = &cx->noExecuteDebuggerTop.ref();
    prev_ = *stack_;
    *stack_ = nullptr;
  }

  ~AutoSuppressDebuggeeNoExecuteChecks() {
    MOZ_ASSERT(!*stack_);
    *stack_ = prev_;
  }
};

} /* namespace js */

#endif /* debugger_DebugAPI_h */
