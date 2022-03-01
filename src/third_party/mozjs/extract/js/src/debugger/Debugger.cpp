/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "debugger/Debugger-inl.h"

#include "mozilla/Attributes.h"        // for MOZ_STACK_CLASS, MOZ_RAII
#include "mozilla/DebugOnly.h"         // for DebugOnly
#include "mozilla/DoublyLinkedList.h"  // for DoublyLinkedList<>::Iterator
#include "mozilla/HashTable.h"         // for HashSet<>::Range, HashMapEntry
#include "mozilla/Maybe.h"             // for Maybe, Nothing, Some
#include "mozilla/ScopeExit.h"         // for MakeScopeExit, ScopeExit
#include "mozilla/ThreadLocal.h"       // for ThreadLocal
#include "mozilla/TimeStamp.h"         // for TimeStamp, TimeDuration
#include "mozilla/UniquePtr.h"         // for UniquePtr
#include "mozilla/Variant.h"           // for AsVariant, AsVariantTemporary
#include "mozilla/Vector.h"            // for Vector, Vector<>::ConstRange

#include <algorithm>   // for std::find, std::max
#include <functional>  // for function
#include <stddef.h>    // for size_t
#include <stdint.h>    // for uint32_t, uint64_t, int32_t
#include <string.h>    // for strlen, strcmp
#include <utility>     // for std::move

#include "jsapi.h"    // for CallArgs, CallArgsFromVp
#include "jstypes.h"  // for JS_PUBLIC_API

#include "builtin/Array.h"                // for NewDenseFullyAllocatedArray
#include "debugger/DebugAPI.h"            // for ResumeMode, DebugAPI
#include "debugger/DebuggerMemory.h"      // for DebuggerMemory
#include "debugger/DebugScript.h"         // for DebugScript
#include "debugger/Environment.h"         // for DebuggerEnvironment
#include "debugger/Frame.h"               // for DebuggerFrame
#include "debugger/NoExecute.h"           // for EnterDebuggeeNoExecute
#include "debugger/Object.h"              // for DebuggerObject
#include "debugger/Script.h"              // for DebuggerScript
#include "debugger/Source.h"              // for DebuggerSource
#include "frontend/CompilationStencil.h"  // for CompilationStencil
#include "frontend/NameAnalysisTypes.h"   // for ParseGoal, ParseGoal::Script
#include "frontend/ParseContext.h"        // for UsedNameTracker
#include "frontend/Parser.h"              // for Parser
#include "gc/FreeOp.h"                    // for JSFreeOp
#include "gc/GC.h"                        // for IterateScripts
#include "gc/GCMarker.h"                  // for GCMarker
#include "gc/GCRuntime.h"                 // for GCRuntime, AutoEnterIteration
#include "gc/HashUtil.h"                  // for DependentAddPtr
#include "gc/Marking.h"                   // for IsMarkedUnbarriered, IsMarked
#include "gc/PublicIterators.h"           // for RealmsIter, CompartmentsIter
#include "gc/Rooting.h"                   // for RootedNativeObject
#include "gc/Statistics.h"                // for Statistics::SliceData
#include "gc/Tracer.h"                    // for TraceEdge
#include "gc/Zone.h"                      // for Zone
#include "gc/ZoneAllocator.h"             // for ZoneAllocPolicy
#include "jit/BaselineDebugModeOSR.h"  // for RecompileOnStackBaselineScriptsForDebugMode
#include "jit/BaselineJIT.h"           // for FinishDiscardBaselineScript
#include "jit/Invalidation.h"         // for RecompileInfoVector
#include "jit/Ion.h"                  // for JitContext
#include "jit/JitScript.h"            // for JitScript
#include "jit/JSJitFrameIter.h"       // for InlineFrameIterator
#include "jit/RematerializedFrame.h"  // for RematerializedFrame
#include "js/Conversions.h"           // for ToBoolean, ToUint32
#include "js/Debug.h"                 // for Builder::Object, Builder
#include "js/friend/ErrorMessages.h"  // for GetErrorMessage, JSMSG_*
#include "js/GCAPI.h"                 // for GarbageCollectionEvent
#include "js/HeapAPI.h"               // for ExposeObjectToActiveJS
#include "js/Promise.h"               // for AutoDebuggerJobQueueInterruption
#include "js/Proxy.h"                 // for PropertyDescriptor
#include "js/SourceText.h"            // for SourceOwnership, SourceText
#include "js/StableStringChars.h"     // for AutoStableStringChars
#include "js/UbiNode.h"               // for Node, RootList, Edge
#include "js/UbiNodeBreadthFirst.h"   // for BreadthFirst
#include "js/Warnings.h"              // for AutoSuppressWarningReporter
#include "js/Wrapper.h"               // for CheckedUnwrapStatic
#include "util/Text.h"                // for DuplicateString, js_strlen
#include "vm/ArrayObject.h"           // for ArrayObject
#include "vm/AsyncFunction.h"         // for AsyncFunctionGeneratorObject
#include "vm/AsyncIteration.h"        // for AsyncGeneratorObject
#include "vm/BytecodeUtil.h"          // for JSDVG_IGNORE_STACK
#include "vm/Compartment.h"           // for CrossCompartmentKey
#include "vm/EnvironmentObject.h"     // for IsSyntacticEnvironment
#include "vm/ErrorReporting.h"        // for ReportErrorToGlobal
#include "vm/GeneratorObject.h"       // for AbstractGeneratorObject
#include "vm/GlobalObject.h"          // for GlobalObject
#include "vm/Interpreter.h"           // for Call, ReportIsNotFunction
#include "vm/Iteration.h"             // for CreateIterResultObject
#include "vm/JSAtom.h"                // for Atomize, ClassName
#include "vm/JSContext.h"             // for JSContext
#include "vm/JSFunction.h"            // for JSFunction
#include "vm/JSObject.h"              // for JSObject, RequireObject,
#include "vm/ObjectOperations.h"      // for DefineDataProperty
#include "vm/PlainObject.h"           // for js::PlainObject
#include "vm/PromiseObject.h"         // for js::PromiseObject
#include "vm/ProxyObject.h"           // for ProxyObject, JSObject::is
#include "vm/Realm.h"                 // for AutoRealm, Realm
#include "vm/Runtime.h"               // for ReportOutOfMemory, JSRuntime
#include "vm/SavedFrame.h"            // for SavedFrame
#include "vm/SavedStacks.h"           // for SavedStacks
#include "vm/Scope.h"                 // for Scope
#include "vm/StringType.h"            // for JSString, PropertyName
#include "vm/TraceLogging.h"          // for TraceLoggerForCurrentThread
#include "vm/WrapperObject.h"         // for CrossCompartmentWrapperObject
#include "wasm/WasmDebug.h"           // for DebugState
#include "wasm/WasmInstance.h"        // for Instance
#include "wasm/WasmJS.h"              // for WasmInstanceObject
#include "wasm/WasmRealm.h"           // for Realm
#include "wasm/WasmTypes.h"           // for WasmInstanceObjectVector

#include "debugger/DebugAPI-inl.h"
#include "debugger/Environment-inl.h"  // for DebuggerEnvironment::owner
#include "debugger/Frame-inl.h"        // for DebuggerFrame::hasGeneratorInfo
#include "debugger/Object-inl.h"   // for DebuggerObject::owner and isInstance.
#include "debugger/Script-inl.h"   // for DebuggerScript::getReferent
#include "gc/GC-inl.h"             // for ZoneCellIter
#include "gc/Marking-inl.h"        // for MaybeForwarded
#include "gc/WeakMap-inl.h"        // for DebuggerWeakMap::trace
#include "vm/Compartment-inl.h"    // for Compartment::wrap
#include "vm/GeckoProfiler-inl.h"  // for AutoSuppressProfilerSampling
#include "vm/JSAtom-inl.h"         // for AtomToId, ValueToId
#include "vm/JSContext-inl.h"      // for JSContext::check
#include "vm/JSObject-inl.h"  // for JSObject::isCallable, NewTenuredObjectWithGivenProto
#include "vm/JSScript-inl.h"      // for JSScript::isDebuggee, JSScript
#include "vm/NativeObject-inl.h"  // for NativeObject::ensureDenseInitializedLength
#include "vm/ObjectOperations-inl.h"  // for GetProperty, HasProperty
#include "vm/Realm-inl.h"             // for AutoRealm::AutoRealm
#include "vm/Stack-inl.h"             // for AbstractFramePtr::script

namespace js {

namespace frontend {
class FullParseHandler;
}

namespace gc {
struct Cell;
}

namespace jit {
class BaselineFrame;
}

} /* namespace js */

using namespace js;

using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::SourceOwnership;
using JS::SourceText;
using JS::dbg::AutoEntryMonitor;
using JS::dbg::Builder;
using js::frontend::IsIdentifier;
using mozilla::AsVariant;
using mozilla::DebugOnly;
using mozilla::MakeScopeExit;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

/*** Utils ******************************************************************/

bool js::IsInterpretedNonSelfHostedFunction(JSFunction* fun) {
  return fun->isInterpreted() && !fun->isSelfHostedBuiltin();
}

JSScript* js::GetOrCreateFunctionScript(JSContext* cx, HandleFunction fun) {
  MOZ_ASSERT(IsInterpretedNonSelfHostedFunction(fun));
  AutoRealm ar(cx, fun);
  return JSFunction::getOrCreateScript(cx, fun);
}

ArrayObject* js::GetFunctionParameterNamesArray(JSContext* cx,
                                                HandleFunction fun) {
  RootedValueVector names(cx);

  // The default value for each argument is |undefined|.
  if (!names.growBy(fun->nargs())) {
    return nullptr;
  }

  if (IsInterpretedNonSelfHostedFunction(fun) && fun->nargs() > 0) {
    RootedScript script(cx, GetOrCreateFunctionScript(cx, fun));
    if (!script) {
      return nullptr;
    }

    MOZ_ASSERT(fun->nargs() == script->numArgs());

    PositionalFormalParameterIter fi(script);
    for (size_t i = 0; i < fun->nargs(); i++, fi++) {
      MOZ_ASSERT(fi.argumentSlot() == i);
      if (JSAtom* atom = fi.name()) {
        // Skip any internal, non-identifier names, like for example ".args".
        if (IsIdentifier(atom)) {
          cx->markAtom(atom);
          names[i].setString(atom);
        }
      }
    }
  }

  return NewDenseCopiedArray(cx, names.length(), names.begin());
}

bool js::ValueToIdentifier(JSContext* cx, HandleValue v, MutableHandleId id) {
  if (!ToPropertyKey(cx, v, id)) {
    return false;
  }
  if (!id.isAtom() || !IsIdentifier(id.toAtom())) {
    RootedValue val(cx, v);
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, val,
                     nullptr, "not an identifier");
    return false;
  }
  return true;
}

class js::AutoRestoreRealmDebugMode {
  Realm* realm_;
  unsigned bits_;

 public:
  explicit AutoRestoreRealmDebugMode(Realm* realm)
      : realm_(realm), bits_(realm->debugModeBits_) {
    MOZ_ASSERT(realm_);
  }

  ~AutoRestoreRealmDebugMode() {
    if (realm_) {
      realm_->debugModeBits_ = bits_;
    }
  }

  void release() { realm_ = nullptr; }
};

/* static */
bool DebugAPI::slowPathCheckNoExecute(JSContext* cx, HandleScript script) {
  MOZ_ASSERT(cx->realm()->isDebuggee());
  MOZ_ASSERT(cx->noExecuteDebuggerTop);
  return EnterDebuggeeNoExecute::reportIfFoundInStack(cx, script);
}

static inline void NukeDebuggerWrapper(NativeObject* wrapper) {
  // In some OOM failure cases, we need to destroy the edge to the referent,
  // to avoid trying to trace it during untimely collections.
  wrapper->setPrivate(nullptr);
}

static void PropagateForcedReturn(JSContext* cx, AbstractFramePtr frame,
                                  HandleValue rval) {
  // The Debugger's hooks may return a value that affects the completion
  // value of the given frame. For example, a hook may return `{ return: 42 }`
  // to terminate the frame and return `42` as the final frame result.
  // To accomplish this, the debugger treats these return values as if
  // execution of the JS function has been terminated without a pending
  // exception, but with a special flag. When the error is handled by the
  // interpreter or JIT, the special flag and the error state will be cleared
  // and execution will continue from the end of the frame.
  MOZ_ASSERT(!cx->isExceptionPending());
  cx->setPropagatingForcedReturn();
  frame.setReturnValue(rval);
}

[[nodiscard]] static bool AdjustGeneratorResumptionValue(JSContext* cx,
                                                         AbstractFramePtr frame,
                                                         ResumeMode& resumeMode,
                                                         MutableHandleValue vp);

[[nodiscard]] static bool ApplyFrameResumeMode(JSContext* cx,
                                               AbstractFramePtr frame,
                                               ResumeMode resumeMode,
                                               HandleValue rv,
                                               HandleSavedFrame exnStack) {
  RootedValue rval(cx, rv);

  // The value passed in here is unwrapped and has no guarantees about what
  // compartment it may be associated with, so we explicitly wrap it into the
  // debuggee compartment.
  if (!cx->compartment()->wrap(cx, &rval)) {
    return false;
  }

  if (!AdjustGeneratorResumptionValue(cx, frame, resumeMode, &rval)) {
    return false;
  }

  switch (resumeMode) {
    case ResumeMode::Continue:
      break;

    case ResumeMode::Throw:
      // If we have a stack from the original throw, use it instead of
      // associating the throw with the current execution point.
      if (exnStack) {
        cx->setPendingException(rval, exnStack);
      } else {
        cx->setPendingExceptionAndCaptureStack(rval);
      }
      return false;

    case ResumeMode::Terminate:
      cx->clearPendingException();
      return false;

    case ResumeMode::Return:
      PropagateForcedReturn(cx, frame, rval);
      return false;

    default:
      MOZ_CRASH("bad Debugger::onEnterFrame resume mode");
  }

  return true;
}
static bool ApplyFrameResumeMode(JSContext* cx, AbstractFramePtr frame,
                                 ResumeMode resumeMode, HandleValue rval) {
  RootedSavedFrame nullStack(cx);
  return ApplyFrameResumeMode(cx, frame, resumeMode, rval, nullStack);
}

bool js::ValueToStableChars(JSContext* cx, const char* fnname,
                            HandleValue value,
                            AutoStableStringChars& stableChars) {
  if (!value.isString()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, fnname, "string",
                              InformalValueTypeName(value));
    return false;
  }
  RootedLinearString linear(cx, value.toString()->ensureLinear(cx));
  if (!linear) {
    return false;
  }
  if (!stableChars.initTwoByte(cx, linear)) {
    return false;
  }
  return true;
}

bool EvalOptions::setFilename(JSContext* cx, const char* filename) {
  JS::UniqueChars copy;
  if (filename) {
    copy = DuplicateString(cx, filename);
    if (!copy) {
      return false;
    }
  }

  filename_ = std::move(copy);
  return true;
}

bool js::ParseEvalOptions(JSContext* cx, HandleValue value,
                          EvalOptions& options) {
  if (!value.isObject()) {
    return true;
  }

  RootedObject opts(cx, &value.toObject());

  RootedValue v(cx);
  if (!JS_GetProperty(cx, opts, "url", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    RootedString url_str(cx, ToString<CanGC>(cx, v));
    if (!url_str) {
      return false;
    }
    UniqueChars url_bytes = JS_EncodeStringToLatin1(cx, url_str);
    if (!url_bytes) {
      return false;
    }
    if (!options.setFilename(cx, url_bytes.get())) {
      return false;
    }
  }

  if (!JS_GetProperty(cx, opts, "lineNumber", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    uint32_t lineno;
    if (!ToUint32(cx, v, &lineno)) {
      return false;
    }
    options.setLineno(lineno);
  }

  return true;
}

/*** Breakpoints ************************************************************/

bool BreakpointSite::isEmpty() const { return breakpoints.isEmpty(); }

void BreakpointSite::trace(JSTracer* trc) {
  for (auto p = breakpoints.begin(); p; p++) {
    p->trace(trc);
  }
}

void BreakpointSite::finalize(JSFreeOp* fop) {
  while (!breakpoints.isEmpty()) {
    breakpoints.begin()->delete_(fop);
  }
}

Breakpoint* BreakpointSite::firstBreakpoint() const {
  if (isEmpty()) {
    return nullptr;
  }
  return &(*breakpoints.begin());
}

bool BreakpointSite::hasBreakpoint(Breakpoint* toFind) {
  const BreakpointList::Iterator bp(toFind);
  for (auto p = breakpoints.begin(); p; p++) {
    if (p == bp) {
      return true;
    }
  }
  return false;
}

Breakpoint::Breakpoint(Debugger* debugger, HandleObject wrappedDebugger,
                       BreakpointSite* site, HandleObject handler)
    : debugger(debugger),
      wrappedDebugger(wrappedDebugger),
      site(site),
      handler(handler) {
  MOZ_ASSERT(UncheckedUnwrap(wrappedDebugger) == debugger->object);
  MOZ_ASSERT(handler->compartment() == wrappedDebugger->compartment());

  debugger->breakpoints.pushBack(this);
  site->breakpoints.pushBack(this);
}

void Breakpoint::trace(JSTracer* trc) {
  TraceEdge(trc, &wrappedDebugger, "breakpoint owner");
  TraceEdge(trc, &handler, "breakpoint handler");
}

void Breakpoint::delete_(JSFreeOp* fop) {
  debugger->breakpoints.remove(this);
  site->breakpoints.remove(this);
  gc::Cell* cell = site->owningCell();
  fop->delete_(cell, this, MemoryUse::Breakpoint);
}

void Breakpoint::remove(JSFreeOp* fop) {
  BreakpointSite* savedSite = site;
  delete_(fop);

  savedSite->destroyIfEmpty(fop);
}

Breakpoint* Breakpoint::nextInDebugger() { return debuggerLink.mNext; }

Breakpoint* Breakpoint::nextInSite() { return siteLink.mNext; }

JSBreakpointSite::JSBreakpointSite(JSScript* script, jsbytecode* pc)
    : script(script), pc(pc) {
  MOZ_ASSERT(!DebugAPI::hasBreakpointsAt(script, pc));
}

void JSBreakpointSite::remove(JSFreeOp* fop) {
  DebugScript::destroyBreakpointSite(fop, script, pc);
}

void JSBreakpointSite::trace(JSTracer* trc) {
  BreakpointSite::trace(trc);
  TraceEdge(trc, &script, "breakpoint script");
}

void JSBreakpointSite::delete_(JSFreeOp* fop) {
  BreakpointSite::finalize(fop);

  fop->delete_(script, this, MemoryUse::BreakpointSite);
}

gc::Cell* JSBreakpointSite::owningCell() { return script; }

Realm* JSBreakpointSite::realm() const { return script->realm(); }

WasmBreakpointSite::WasmBreakpointSite(WasmInstanceObject* instanceObject_,
                                       uint32_t offset_)
    : instanceObject(instanceObject_), offset(offset_) {
  MOZ_ASSERT(instanceObject_);
  MOZ_ASSERT(instanceObject_->instance().debugEnabled());
}

void WasmBreakpointSite::trace(JSTracer* trc) {
  BreakpointSite::trace(trc);
  TraceEdge(trc, &instanceObject, "breakpoint Wasm instance");
}

void WasmBreakpointSite::remove(JSFreeOp* fop) {
  instanceObject->instance().destroyBreakpointSite(fop, offset);
}

void WasmBreakpointSite::delete_(JSFreeOp* fop) {
  BreakpointSite::finalize(fop);

  fop->delete_(instanceObject, this, MemoryUse::BreakpointSite);
}

gc::Cell* WasmBreakpointSite::owningCell() { return instanceObject; }

Realm* WasmBreakpointSite::realm() const { return instanceObject->realm(); }

/*** Debugger hook dispatch *************************************************/

Debugger::Debugger(JSContext* cx, NativeObject* dbg)
    : object(dbg),
      debuggees(cx->zone()),
      uncaughtExceptionHook(nullptr),
      allowUnobservedAsmJS(false),
      collectCoverageInfo(false),
      observedGCs(cx->zone()),
      allocationsLog(cx),
      trackingAllocationSites(false),
      allocationSamplingProbability(1.0),
      maxAllocationsLogLength(DEFAULT_MAX_LOG_LENGTH),
      allocationsLogOverflowed(false),
      frames(cx->zone()),
      generatorFrames(cx),
      scripts(cx),
      sources(cx),
      objects(cx),
      environments(cx),
      wasmInstanceScripts(cx),
      wasmInstanceSources(cx),
#ifdef NIGHTLY_BUILD
      traceLoggerLastDrainedSize(0),
      traceLoggerLastDrainedIteration(0),
#endif
      traceLoggerScriptedCallsLastDrainedSize(0),
      traceLoggerScriptedCallsLastDrainedIteration(0) {
  cx->check(dbg);

#ifdef JS_TRACE_LOGGING
  TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
  if (logger) {
#  ifdef NIGHTLY_BUILD
    logger->getIterationAndSize(&traceLoggerLastDrainedIteration,
                                &traceLoggerLastDrainedSize);
#  endif
    logger->getIterationAndSize(&traceLoggerScriptedCallsLastDrainedIteration,
                                &traceLoggerScriptedCallsLastDrainedSize);
  }
#endif

  cx->runtime()->debuggerList().insertBack(this);
}

template <typename ElementAccess>
static void RemoveDebuggerEntry(
    mozilla::DoublyLinkedList<Debugger, ElementAccess>& list, Debugger* dbg) {
  // The "probably" here is because there could technically be multiple lists
  // with this type signature and theoretically the debugger could be an entry
  // in a different one. That is not actually possible however because there
  // is only one list the debugger could be in.
  if (list.ElementProbablyInList(dbg)) {
    list.remove(dbg);
  }
}

Debugger::~Debugger() {
  MOZ_ASSERT(debuggees.empty());
  allocationsLog.clear();

  // Breakpoints should hold us alive, so any breakpoints remaining must be set
  // in dying JSScripts. We should clean them up, but this never asserts. I'm
  // not sure why.
  MOZ_ASSERT(breakpoints.isEmpty());

  // We don't have to worry about locking here since Debugger is not
  // background finalized.
  JSContext* cx = TlsContext.get();
  RemoveDebuggerEntry(cx->runtime()->onNewGlobalObjectWatchers(), this);
  RemoveDebuggerEntry(cx->runtime()->onGarbageCollectionWatchers(), this);
}

#ifdef DEBUG
/* static */
bool Debugger::isChildJSObject(JSObject* obj) {
  return obj->getClass() == &DebuggerFrame::class_ ||
         obj->getClass() == &DebuggerScript::class_ ||
         obj->getClass() == &DebuggerSource::class_ ||
         obj->getClass() == &DebuggerObject::class_ ||
         obj->getClass() == &DebuggerEnvironment::class_;
}
#endif

bool Debugger::hasMemory() const {
  return object->getReservedSlot(JSSLOT_DEBUG_MEMORY_INSTANCE).isObject();
}

DebuggerMemory& Debugger::memory() const {
  MOZ_ASSERT(hasMemory());
  return object->getReservedSlot(JSSLOT_DEBUG_MEMORY_INSTANCE)
      .toObject()
      .as<DebuggerMemory>();
}

/*** Debugger accessors *******************************************************/

bool Debugger::getFrame(JSContext* cx, const FrameIter& iter,
                        MutableHandleValue vp) {
  RootedDebuggerFrame result(cx);
  if (!Debugger::getFrame(cx, iter, &result)) {
    return false;
  }
  vp.setObject(*result);
  return true;
}

bool Debugger::getFrame(JSContext* cx, MutableHandleDebuggerFrame result) {
  RootedObject proto(
      cx, &object->getReservedSlot(JSSLOT_DEBUG_FRAME_PROTO).toObject());
  RootedNativeObject debugger(cx, object);

  // Since there is no frame/generator data to associate with this frame, this
  // will create a new, "terminated" Debugger.Frame object.
  RootedDebuggerFrame frame(
      cx, DebuggerFrame::create(cx, proto, debugger, nullptr, nullptr));
  if (!frame) {
    return false;
  }

  result.set(frame);
  return true;
}

bool Debugger::getFrame(JSContext* cx, const FrameIter& iter,
                        MutableHandleDebuggerFrame result) {
  AbstractFramePtr referent = iter.abstractFramePtr();
  MOZ_ASSERT_IF(referent.hasScript(), !referent.script()->selfHosted());

  FrameMap::AddPtr p = frames.lookupForAdd(referent);
  if (!p) {
    Rooted<AbstractGeneratorObject*> genObj(cx);
    if (referent.isGeneratorFrame()) {
      if (referent.isFunctionFrame()) {
        AutoRealm ar(cx, referent.callee());
        genObj = GetGeneratorObjectForFrame(cx, referent);
      } else {
        MOZ_ASSERT(referent.isModuleFrame());
        AutoRealm ar(cx, referent.script()->module());
        genObj = GetGeneratorObjectForFrame(cx, referent);
      }

      // If this frame has a generator associated with it, but no on-stack
      // Debugger.Frame object was found, there should not be a suspended
      // Debugger.Frame either because otherwise slowPathOnResumeFrame would
      // have already populated the "frames" map with a Debugger.Frame.
      MOZ_ASSERT_IF(genObj, !generatorFrames.has(genObj));

      // If the frame's generator is closed, there is no way to associate the
      // generator with the frame successfully because there is no way to
      // get the generator's callee script, and even if we could, having it
      // there would in no way affect the behavior of the frame.
      if (genObj && genObj->isClosed()) {
        genObj = nullptr;
      }

      // If no AbstractGeneratorObject exists yet, we create a Debugger.Frame
      // below anyway, and Debugger::onNewGenerator() will associate it
      // with the AbstractGeneratorObject later when we hit JSOp::Generator.
    }

    // Create and populate the Debugger.Frame object.
    RootedObject proto(
        cx, &object->getReservedSlot(JSSLOT_DEBUG_FRAME_PROTO).toObject());
    RootedNativeObject debugger(cx, object);

    RootedDebuggerFrame frame(
        cx, DebuggerFrame::create(cx, proto, debugger, &iter, genObj));
    if (!frame) {
      return false;
    }

    auto terminateDebuggerFrameGuard = MakeScopeExit([&] {
      terminateDebuggerFrame(cx->defaultFreeOp(), this, frame, referent);
    });

    if (genObj) {
      DependentAddPtr<GeneratorWeakMap> genPtr(cx, generatorFrames, genObj);
      if (!genPtr.add(cx, generatorFrames, genObj, frame)) {
        return false;
      }
    }

    if (!ensureExecutionObservabilityOfFrame(cx, referent)) {
      return false;
    }

    if (!frames.add(p, referent, frame)) {
      ReportOutOfMemory(cx);
      return false;
    }

    terminateDebuggerFrameGuard.release();
  }

  result.set(p->value());
  return true;
}

bool Debugger::getFrame(JSContext* cx, Handle<AbstractGeneratorObject*> genObj,
                        MutableHandleDebuggerFrame result) {
  // To create a Debugger.Frame for a running generator, we'd also need a
  // FrameIter for its stack frame. We could make this work by searching the
  // stack for the generator's frame, but for the moment, we only need this
  // function to handle generators we've found on promises' reaction records,
  // which should always be suspended.
  MOZ_ASSERT(genObj->isSuspended());

  // Do we have an existing Debugger.Frame for this generator?
  DependentAddPtr<GeneratorWeakMap> p(cx, generatorFrames, genObj);
  if (p) {
    MOZ_ASSERT(&p->value()->unwrappedGenerator() == genObj);
    result.set(p->value());
    return true;
  }

  // Create a new Debugger.Frame.
  RootedObject proto(
      cx, &object->getReservedSlot(JSSLOT_DEBUG_FRAME_PROTO).toObject());
  RootedNativeObject debugger(cx, object);

  result.set(DebuggerFrame::create(cx, proto, debugger, nullptr, genObj));
  if (!result) {
    return false;
  }

  if (!p.add(cx, generatorFrames, genObj, result)) {
    terminateDebuggerFrame(cx->runtime()->defaultFreeOp(), this, result,
                           NullFramePtr());
    return false;
  }

  return true;
}

static bool DebuggerExists(
    GlobalObject* global, const std::function<bool(Debugger* dbg)>& predicate) {
  // The GC analysis can't determine that the predicate can't GC, so let it know
  // explicitly.
  JS::AutoSuppressGCAnalysis nogc;

  for (Realm::DebuggerVectorEntry& entry : global->getDebuggers()) {
    // Callbacks should not create new references to the debugger, so don't
    // use a barrier. This allows this method to be called during GC.
    if (predicate(entry.dbg.unbarrieredGet())) {
      return true;
    }
  }
  return false;
}

/* static */
bool Debugger::hasLiveHook(GlobalObject* global, Hook which) {
  return DebuggerExists(global,
                        [=](Debugger* dbg) { return dbg->getHook(which); });
}

/* static */
bool DebugAPI::debuggerObservesAllExecution(GlobalObject* global) {
  return DebuggerExists(
      global, [=](Debugger* dbg) { return dbg->observesAllExecution(); });
}

/* static */
bool DebugAPI::debuggerObservesCoverage(GlobalObject* global) {
  return DebuggerExists(global,
                        [=](Debugger* dbg) { return dbg->observesCoverage(); });
}

/* static */
bool DebugAPI::debuggerObservesAsmJS(GlobalObject* global) {
  return DebuggerExists(global,
                        [=](Debugger* dbg) { return dbg->observesAsmJS(); });
}

/* static */
bool DebugAPI::hasExceptionUnwindHook(GlobalObject* global) {
  return Debugger::hasLiveHook(global, Debugger::OnExceptionUnwind);
}

/* static */
bool DebugAPI::hasDebuggerStatementHook(GlobalObject* global) {
  return Debugger::hasLiveHook(global, Debugger::OnDebuggerStatement);
}

template <typename HookIsEnabledFun /* bool (Debugger*) */>
bool DebuggerList<HookIsEnabledFun>::init(JSContext* cx) {
  // Determine which debuggers will receive this event, and in what order.
  // Make a copy of the list, since the original is mutable and we will be
  // calling into arbitrary JS.
  Handle<GlobalObject*> global = cx->global();
  for (Realm::DebuggerVectorEntry& entry : global->getDebuggers()) {
    Debugger* dbg = entry.dbg;
    if (dbg->isHookCallAllowed(cx) && hookIsEnabled(dbg)) {
      if (!debuggers.append(ObjectValue(*dbg->toJSObject()))) {
        return false;
      }
    }
  }
  return true;
}

template <typename HookIsEnabledFun /* bool (Debugger*) */>
template <typename FireHookFun /* bool (Debugger*) */>
bool DebuggerList<HookIsEnabledFun>::dispatchHook(JSContext* cx,
                                                  FireHookFun fireHook) {
  // Preserve the debuggee's microtask event queue while we run the hooks, so
  // the debugger's microtask checkpoints don't run from the debuggee's
  // microtasks, and vice versa.
  JS::AutoDebuggerJobQueueInterruption adjqi;
  if (!adjqi.init(cx)) {
    return false;
  }

  // Deliver the event to each debugger, checking again to make sure it
  // should still be delivered.
  Handle<GlobalObject*> global = cx->global();
  for (Value* p = debuggers.begin(); p != debuggers.end(); p++) {
    Debugger* dbg = Debugger::fromJSObject(&p->toObject());
    EnterDebuggeeNoExecute nx(cx, *dbg, adjqi);
    if (dbg->debuggees.has(global) && hookIsEnabled(dbg)) {
      bool result =
          dbg->enterDebuggerHook(cx, [&]() -> bool { return fireHook(dbg); });
      adjqi.runJobs();
      if (!result) {
        return false;
      }
    }
  }
  return true;
}

template <typename HookIsEnabledFun /* bool (Debugger*) */>
template <typename FireHookFun /* bool (Debugger*) */>
void DebuggerList<HookIsEnabledFun>::dispatchQuietHook(JSContext* cx,
                                                       FireHookFun fireHook) {
  bool result =
      dispatchHook(cx, [&](Debugger* dbg) -> bool { return fireHook(dbg); });

  // dispatchHook may fail due to OOM. This OOM is not handlable at the
  // callsites of dispatchQuietHook in the engine.
  if (!result) {
    cx->clearPendingException();
  }
}

template <typename HookIsEnabledFun /* bool (Debugger*) */>
template <typename FireHookFun /* bool (Debugger*, ResumeMode&, MutableHandleValue vp) */>
bool DebuggerList<HookIsEnabledFun>::dispatchResumptionHook(
    JSContext* cx, AbstractFramePtr frame, FireHookFun fireHook) {
  ResumeMode resumeMode = ResumeMode::Continue;
  RootedValue rval(cx);
  return dispatchHook(cx,
                      [&](Debugger* dbg) -> bool {
                        return fireHook(dbg, resumeMode, &rval);
                      }) &&
         ApplyFrameResumeMode(cx, frame, resumeMode, rval);
}

JSObject* Debugger::getHook(Hook hook) const {
  MOZ_ASSERT(hook >= 0 && hook < HookCount);
  const Value& v = object->getReservedSlot(JSSLOT_DEBUG_HOOK_START + hook);
  return v.isUndefined() ? nullptr : &v.toObject();
}

bool Debugger::hasAnyLiveHooks() const {
  // A onNewGlobalObject hook does not hold its Debugger live, so its behavior
  // is nondeterministic. This behavior is not satisfying, but it is at least
  // documented.
  if (getHook(OnDebuggerStatement) || getHook(OnExceptionUnwind) ||
      getHook(OnNewScript) || getHook(OnEnterFrame)) {
    return true;
  }

  return false;
}

/* static */
bool DebugAPI::slowPathOnEnterFrame(JSContext* cx, AbstractFramePtr frame) {
  return Debugger::dispatchResumptionHook(
      cx, frame,
      [frame](Debugger* dbg) -> bool {
        return dbg->observesFrame(frame) && dbg->observesEnterFrame();
      },
      [&](Debugger* dbg, ResumeMode& resumeMode, MutableHandleValue vp)
          -> bool { return dbg->fireEnterFrame(cx, resumeMode, vp); });
}

/* static */
bool DebugAPI::slowPathOnResumeFrame(JSContext* cx, AbstractFramePtr frame) {
  // Don't count on this method to be called every time a generator is
  // resumed! This is called only if the frame's debuggee bit is set,
  // i.e. the script has breakpoints or the frame is stepping.
  MOZ_ASSERT(frame.isGeneratorFrame());
  MOZ_ASSERT(frame.isDebuggee());

  Rooted<AbstractGeneratorObject*> genObj(
      cx, GetGeneratorObjectForFrame(cx, frame));
  MOZ_ASSERT(genObj);

  // If there is an OOM, we mark all of the Debugger.Frame objects terminated
  // because we want to ensure that none of the frames are in a partially
  // initialized state where they are in "generatorFrames" but not "frames".
  auto terminateDebuggerFramesGuard = MakeScopeExit([&] {
    Debugger::terminateDebuggerFrames(cx, frame);

    MOZ_ASSERT(!DebugAPI::inFrameMaps(frame));
  });

  // For each debugger, if there is an existing Debugger.Frame object for the
  // resumed `frame`, update it with the new frame pointer and make sure the
  // frame is observable.
  FrameIter iter(cx);
  MOZ_ASSERT(iter.abstractFramePtr() == frame);
  for (Realm::DebuggerVectorEntry& entry : frame.global()->getDebuggers()) {
    Debugger* dbg = entry.dbg;
    if (Debugger::GeneratorWeakMap::Ptr generatorEntry =
            dbg->generatorFrames.lookup(genObj)) {
      DebuggerFrame* frameObj = generatorEntry->value();
      MOZ_ASSERT(&frameObj->unwrappedGenerator() == genObj);
      if (!dbg->frames.putNew(frame, frameObj)) {
        ReportOutOfMemory(cx);
        return false;
      }
      if (!frameObj->resume(iter)) {
        return false;
      }
    }
  }

  terminateDebuggerFramesGuard.release();

  return slowPathOnEnterFrame(cx, frame);
}

/* static */
NativeResumeMode DebugAPI::slowPathOnNativeCall(JSContext* cx,
                                                const CallArgs& args,
                                                CallReason reason) {
  // "onNativeCall" only works consistently in the context of an explicit eval
  // that has set the "insideDebuggerEvaluationWithOnNativeCallHook" state
  // on the JSContext, so we fast-path this hook to bail right away if that is
  // not currently set. If this flag is set to a _different_ debugger, the
  // standard "isHookCallAllowed" debugger logic will apply and only hooks on
  // that debugger will be callable.
  if (!cx->insideDebuggerEvaluationWithOnNativeCallHook) {
    return NativeResumeMode::Continue;
  }

  DebuggerList debuggerList(cx, [](Debugger* dbg) -> bool {
    return dbg->getHook(Debugger::OnNativeCall);
  });

  if (!debuggerList.init(cx)) {
    return NativeResumeMode::Abort;
  }

  if (debuggerList.empty()) {
    return NativeResumeMode::Continue;
  }

  // The onNativeCall hook is fired when self hosted functions are called,
  // and any other self hosted function or C++ native that is directly called
  // by the self hosted function is considered to be part of the same
  // native call.
  //
  // We check this only after checking that debuggerList has items in order
  // to avoid unnecessary calls to cx->currentScript(), which can be expensive
  // when the top frame is in jitcode.
  JSScript* script = cx->currentScript();
  if (script && script->selfHosted()) {
    return NativeResumeMode::Continue;
  }

  RootedValue rval(cx);
  ResumeMode resumeMode = ResumeMode::Continue;
  bool result = debuggerList.dispatchHook(cx, [&](Debugger* dbg) -> bool {
    return dbg->fireNativeCall(cx, args, reason, resumeMode, &rval);
  });
  if (!result) {
    return NativeResumeMode::Abort;
  }

  // The value is not in any particular compartment, so it needs to be
  // explicitly wrapped into the debuggee compartment.
  if (!cx->compartment()->wrap(cx, &rval)) {
    return NativeResumeMode::Abort;
  }

  switch (resumeMode) {
    case ResumeMode::Continue:
      break;

    case ResumeMode::Throw:
      cx->setPendingExceptionAndCaptureStack(rval);
      return NativeResumeMode::Abort;

    case ResumeMode::Terminate:
      cx->clearPendingException();
      return NativeResumeMode::Abort;

    case ResumeMode::Return:
      args.rval().set(rval);
      return NativeResumeMode::Override;
  }

  return NativeResumeMode::Continue;
}

/*
 * RAII class to mark a generator as "running" temporarily while running
 * debugger code.
 *
 * When Debugger::slowPathOnLeaveFrame is called for a frame that is yielding
 * or awaiting, its generator is in the "suspended" state. Letting script
 * observe this state, with the generator on stack yet also reenterable, would
 * be bad, so we mark it running while we fire events.
 */
class MOZ_RAII AutoSetGeneratorRunning {
  int32_t resumeIndex_;
  AsyncGeneratorObject::State asyncGenState_;
  Rooted<AbstractGeneratorObject*> genObj_;

 public:
  AutoSetGeneratorRunning(JSContext* cx,
                          Handle<AbstractGeneratorObject*> genObj)
      : resumeIndex_(0),
        asyncGenState_(static_cast<AsyncGeneratorObject::State>(0)),
        genObj_(cx, genObj) {
    if (genObj) {
      if (!genObj->isClosed() && !genObj->isBeforeInitialYield() &&
          genObj->isSuspended()) {
        // Yielding or awaiting.
        resumeIndex_ = genObj->resumeIndex();
        genObj->setRunning();

        // Async generators have additionally bookkeeping which must be
        // adjusted when switching over to the running state.
        if (genObj->is<AsyncGeneratorObject>()) {
          auto* asyncGenObj = &genObj->as<AsyncGeneratorObject>();
          asyncGenState_ = asyncGenObj->state();
          asyncGenObj->setExecuting();
        }
      } else {
        // Returning or throwing. The generator is already closed, if
        // it was ever exposed at all.
        genObj_ = nullptr;
      }
    }
  }

  ~AutoSetGeneratorRunning() {
    if (genObj_) {
      MOZ_ASSERT(genObj_->isRunning());
      genObj_->setResumeIndex(resumeIndex_);
      if (genObj_->is<AsyncGeneratorObject>()) {
        genObj_->as<AsyncGeneratorObject>().setState(asyncGenState_);
      }
    }
  }
};

/*
 * Handle leaving a frame with debuggers watching. |frameOk| indicates whether
 * the frame is exiting normally or abruptly. Set |cx|'s exception and/or
 * |cx->fp()|'s return value, and return a new success value.
 */
/* static */
bool DebugAPI::slowPathOnLeaveFrame(JSContext* cx, AbstractFramePtr frame,
                                    jsbytecode* pc, bool frameOk) {
  MOZ_ASSERT_IF(!frame.isWasmDebugFrame(), pc);

  mozilla::DebugOnly<Handle<GlobalObject*>> debuggeeGlobal = cx->global();

  // These are updated below, but consulted by the cleanup code we register now,
  // so declare them here, initialized to quiescent values.
  Rooted<Completion> completion(cx);
  bool success = false;

  auto frameMapsGuard = MakeScopeExit([&] {
    // Clean up all Debugger.Frame instances on exit. On suspending, pass the
    // flag that says to leave those frames `.live`. Note that if the completion
    // is a suspension but success is false, the generator gets closed, not
    // suspended.
    if (success && completion.get().suspending()) {
      Debugger::suspendGeneratorDebuggerFrames(cx, frame);
    } else {
      Debugger::terminateDebuggerFrames(cx, frame);
    }
  });

  // The onPop handler and associated clean up logic should not run multiple
  // times on the same frame. If slowPathOnLeaveFrame has already been
  // called, the frame will not be present in the Debugger frame maps.
  Rooted<Debugger::DebuggerFrameVector> frames(
      cx, Debugger::DebuggerFrameVector(cx));
  if (!Debugger::getDebuggerFrames(frame, &frames)) {
    return false;
  }
  if (frames.empty()) {
    return frameOk;
  }

  completion = Completion::fromJSFramePop(cx, frame, pc, frameOk);

  ResumeMode resumeMode = ResumeMode::Continue;
  RootedValue rval(cx);

  {
    // Preserve the debuggee's microtask event queue while we run the hooks, so
    // the debugger's microtask checkpoints don't run from the debuggee's
    // microtasks, and vice versa.
    JS::AutoDebuggerJobQueueInterruption adjqi;
    if (!adjqi.init(cx)) {
      return false;
    }

    // This path can be hit via unwinding the stack due to over-recursion or
    // OOM. In those cases, don't fire the frames' onPop handlers, because
    // invoking JS will only trigger the same condition. See
    // slowPathOnExceptionUnwind.
    if (!cx->isThrowingOverRecursed() && !cx->isThrowingOutOfMemory()) {
      Rooted<AbstractGeneratorObject*> genObj(
          cx, frame.isGeneratorFrame() ? GetGeneratorObjectForFrame(cx, frame)
                                       : nullptr);

      // For each Debugger.Frame, fire its onPop handler, if any.
      for (size_t i = 0; i < frames.length(); i++) {
        HandleDebuggerFrame frameobj = frames[i];
        Debugger* dbg = frameobj->owner();
        EnterDebuggeeNoExecute nx(cx, *dbg, adjqi);

        // Removing a global from a Debugger's debuggee set kills all of that
        // Debugger's D.Fs in that global. This means that one D.F's onPop can
        // kill the next D.F. So we have to check whether frameobj is still "on
        // the stack".
        if (frameobj->isOnStack() && frameobj->onPopHandler()) {
          OnPopHandler* handler = frameobj->onPopHandler();

          bool result = dbg->enterDebuggerHook(cx, [&]() -> bool {
            ResumeMode nextResumeMode = ResumeMode::Continue;
            RootedValue nextValue(cx);

            // Call the onPop handler.
            bool success;
            {
              // Mark the generator as running, to prevent reentrance.
              //
              // At certain points in a generator's lifetime,
              // GetGeneratorObjectForFrame can return null even when the
              // generator exists, but at those points the generator has not yet
              // been exposed to JavaScript, so reentrance isn't possible
              // anyway. So there's no harm done if this has no effect in that
              // case.
              AutoSetGeneratorRunning asgr(cx, genObj);
              success = handler->onPop(cx, frameobj, completion, nextResumeMode,
                                       &nextValue);
            }

            return dbg->processParsedHandlerResult(cx, frame, pc, success,
                                                   nextResumeMode, nextValue,
                                                   resumeMode, &rval);
          });
          adjqi.runJobs();

          if (!result) {
            return false;
          }

          // At this point, we are back in the debuggee compartment, and
          // any error has been wrapped up as a completion value.
          MOZ_ASSERT(!cx->isExceptionPending());
        }
      }
    }
  }

  completion.get().updateFromHookResult(resumeMode, rval);

  // Now that we've run all the handlers, extract the final resumption mode. */
  ResumeMode completionResumeMode;
  RootedValue completionValue(cx);
  RootedSavedFrame completionStack(cx);
  completion.get().toResumeMode(completionResumeMode, &completionValue,
                                &completionStack);

  // If we are returning the original value used to create the completion, then
  // we don't want to treat the resumption value as a Return completion, because
  // that would cause us to apply AdjustGeneratorResumptionValue to the
  // already-adjusted value that the generator actually returned.
  if (resumeMode == ResumeMode::Continue &&
      completionResumeMode == ResumeMode::Return) {
    completionResumeMode = ResumeMode::Continue;
  }

  if (!ApplyFrameResumeMode(cx, frame, completionResumeMode, completionValue,
                            completionStack)) {
    if (!cx->isPropagatingForcedReturn()) {
      // If this is an exception or termination, we just propagate that along.
      return false;
    }

    // Since we are leaving the frame here, we can convert a forced return
    // into a normal return right away.
    cx->clearPropagatingForcedReturn();
  }
  success = true;
  return true;
}

/* static */
bool DebugAPI::slowPathOnNewGenerator(JSContext* cx, AbstractFramePtr frame,
                                      Handle<AbstractGeneratorObject*> genObj) {
  // This is called from JSOp::Generator, after default parameter expressions
  // are evaluated and well after onEnterFrame, so Debugger.Frame objects for
  // `frame` may already have been exposed to debugger code. The
  // AbstractGeneratorObject for this generator call, though, has just been
  // created. It must be associated with any existing Debugger.Frames.

  // Initializing frames with their associated generator is critical to the
  // functionality of the debugger, so if there is an OOM, we want to
  // cleanly terminate all of the frames.
  auto terminateDebuggerFramesGuard =
      MakeScopeExit([&] { Debugger::terminateDebuggerFrames(cx, frame); });

  bool ok = true;
  Debugger::forEachOnStackDebuggerFrame(
      frame, [&](Debugger* dbg, DebuggerFrame* frameObjPtr) {
        if (!ok) {
          return;
        }

        RootedDebuggerFrame frameObj(cx, frameObjPtr);

        AutoRealm ar(cx, frameObj);

        if (!DebuggerFrame::setGeneratorInfo(cx, frameObj, genObj)) {
          // This leaves `genObj` and `frameObj` unassociated. It's OK
          // because we won't pause again with this generator on the stack:
          // the caller will immediately discard `genObj` and unwind `frame`.
          ok = false;
          return;
        }

        DependentAddPtr<Debugger::GeneratorWeakMap> genPtr(
            cx, dbg->generatorFrames, genObj);
        if (!genPtr.add(cx, dbg->generatorFrames, genObj, frameObj)) {
          ok = false;
        }
      });

  if (!ok) {
    return false;
  }

  terminateDebuggerFramesGuard.release();
  return true;
}

/* static */
bool DebugAPI::slowPathOnDebuggerStatement(JSContext* cx,
                                           AbstractFramePtr frame) {
  return Debugger::dispatchResumptionHook(
      cx, frame,
      [](Debugger* dbg) -> bool {
        return dbg->getHook(Debugger::OnDebuggerStatement);
      },
      [&](Debugger* dbg, ResumeMode& resumeMode, MutableHandleValue vp)
          -> bool { return dbg->fireDebuggerStatement(cx, resumeMode, vp); });
}

/* static */
bool DebugAPI::slowPathOnExceptionUnwind(JSContext* cx,
                                         AbstractFramePtr frame) {
  // Invoking more JS on an over-recursed stack or after OOM is only going
  // to result in more of the same error.
  if (cx->isThrowingOverRecursed() || cx->isThrowingOutOfMemory()) {
    return true;
  }

  // The Debugger API mustn't muck with frames from self-hosted scripts.
  if (frame.hasScript() && frame.script()->selfHosted()) {
    return true;
  }

  DebuggerList debuggerList(cx, [](Debugger* dbg) -> bool {
    return dbg->getHook(Debugger::OnExceptionUnwind);
  });

  if (!debuggerList.init(cx)) {
    return false;
  }

  if (debuggerList.empty()) {
    return true;
  }

  // We save and restore the exception once up front to avoid having to do it
  // for each 'onExceptionUnwind' hook that has been registered, and we also
  // only do it if the debuggerList contains items in order to avoid extra work.
  RootedValue exc(cx);
  RootedSavedFrame stack(cx, cx->getPendingExceptionStack());
  if (!cx->getPendingException(&exc)) {
    return false;
  }
  cx->clearPendingException();

  bool result = debuggerList.dispatchResumptionHook(
      cx, frame,
      [&](Debugger* dbg, ResumeMode& resumeMode,
          MutableHandleValue vp) -> bool {
        return dbg->fireExceptionUnwind(cx, exc, resumeMode, vp);
      });
  if (!result) {
    return false;
  }

  cx->setPendingException(exc, stack);
  return true;
}

// TODO: Remove Remove this function when all properties/methods returning a
///      DebuggerEnvironment have been given a C++ interface (bug 1271649).
bool Debugger::wrapEnvironment(JSContext* cx, Handle<Env*> env,
                               MutableHandleValue rval) {
  if (!env) {
    rval.setNull();
    return true;
  }

  RootedDebuggerEnvironment envobj(cx);

  if (!wrapEnvironment(cx, env, &envobj)) {
    return false;
  }

  rval.setObject(*envobj);
  return true;
}

bool Debugger::wrapEnvironment(JSContext* cx, Handle<Env*> env,
                               MutableHandleDebuggerEnvironment result) {
  MOZ_ASSERT(env);

  // DebuggerEnv should only wrap a debug scope chain obtained (transitively)
  // from GetDebugEnvironmentFor(Frame|Function).
  MOZ_ASSERT(!IsSyntacticEnvironment(env));

  DependentAddPtr<EnvironmentWeakMap> p(cx, environments, env);
  if (p) {
    result.set(&p->value()->as<DebuggerEnvironment>());
  } else {
    // Create a new Debugger.Environment for env.
    RootedObject proto(
        cx, &object->getReservedSlot(JSSLOT_DEBUG_ENV_PROTO).toObject());
    RootedNativeObject debugger(cx, object);

    RootedDebuggerEnvironment envobj(
        cx, DebuggerEnvironment::create(cx, proto, env, debugger));
    if (!envobj) {
      return false;
    }

    if (!p.add(cx, environments, env, envobj)) {
      NukeDebuggerWrapper(envobj);
      return false;
    }

    result.set(envobj);
  }

  return true;
}

bool Debugger::wrapDebuggeeValue(JSContext* cx, MutableHandleValue vp) {
  cx->check(object.get());

  if (vp.isObject()) {
    RootedObject obj(cx, &vp.toObject());
    RootedDebuggerObject dobj(cx);

    if (!wrapDebuggeeObject(cx, obj, &dobj)) {
      return false;
    }

    vp.setObject(*dobj);
  } else if (vp.isMagic()) {
    RootedPlainObject optObj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!optObj) {
      return false;
    }

    // We handle three sentinel values: missing arguments
    // (JS_MISSING_ARGUMENTS), optimized out slots (JS_OPTIMIZED_OUT),
    // and uninitialized bindings (JS_UNINITIALIZED_LEXICAL).
    //
    // Other magic values should not have escaped.
    PropertyName* name;
    switch (vp.whyMagic()) {
      case JS_MISSING_ARGUMENTS:
        name = cx->names().missingArguments;
        break;
      case JS_OPTIMIZED_OUT:
        name = cx->names().optimizedOut;
        break;
      case JS_UNINITIALIZED_LEXICAL:
        name = cx->names().uninitialized;
        break;
      default:
        MOZ_CRASH("Unsupported magic value escaped to Debugger");
    }

    RootedValue trueVal(cx, BooleanValue(true));
    if (!DefineDataProperty(cx, optObj, name, trueVal)) {
      return false;
    }

    vp.setObject(*optObj);
  } else if (!cx->compartment()->wrap(cx, vp)) {
    vp.setUndefined();
    return false;
  }

  return true;
}

bool Debugger::wrapNullableDebuggeeObject(JSContext* cx, HandleObject obj,
                                          MutableHandleDebuggerObject result) {
  if (!obj) {
    result.set(nullptr);
    return true;
  }

  return wrapDebuggeeObject(cx, obj, result);
}

bool Debugger::wrapDebuggeeObject(JSContext* cx, HandleObject obj,
                                  MutableHandleDebuggerObject result) {
  MOZ_ASSERT(obj);

  DependentAddPtr<ObjectWeakMap> p(cx, objects, obj);
  if (p) {
    result.set(&p->value()->as<DebuggerObject>());
  } else {
    // Create a new Debugger.Object for obj.
    RootedNativeObject debugger(cx, object);
    RootedObject proto(
        cx, &object->getReservedSlot(JSSLOT_DEBUG_OBJECT_PROTO).toObject());
    RootedDebuggerObject dobj(cx,
                              DebuggerObject::create(cx, proto, obj, debugger));
    if (!dobj) {
      return false;
    }

    if (!p.add(cx, objects, obj, dobj)) {
      NukeDebuggerWrapper(dobj);
      return false;
    }

    result.set(dobj);
  }

  return true;
}

static DebuggerObject* ToNativeDebuggerObject(JSContext* cx,
                                              MutableHandleObject obj) {
  if (!obj->is<DebuggerObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, "Debugger",
                              "Debugger.Object", obj->getClass()->name);
    return nullptr;
  }

  DebuggerObject* ndobj = &obj->as<DebuggerObject>();

  if (!ndobj->isInstance()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_PROTO,
                              "Debugger.Object", "Debugger.Object");
    return nullptr;
  }

  return ndobj;
}

bool Debugger::unwrapDebuggeeObject(JSContext* cx, MutableHandleObject obj) {
  DebuggerObject* ndobj = ToNativeDebuggerObject(cx, obj);
  if (!ndobj) {
    return false;
  }

  if (ndobj->owner() != Debugger::fromJSObject(object)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_WRONG_OWNER, "Debugger.Object");
    return false;
  }

  obj.set(ndobj->referent());
  return true;
}

bool Debugger::unwrapDebuggeeValue(JSContext* cx, MutableHandleValue vp) {
  cx->check(object.get(), vp);
  if (vp.isObject()) {
    RootedObject dobj(cx, &vp.toObject());
    if (!unwrapDebuggeeObject(cx, &dobj)) {
      return false;
    }
    vp.setObject(*dobj);
  }
  return true;
}

static bool CheckArgCompartment(JSContext* cx, JSObject* obj, JSObject* arg,
                                const char* methodname, const char* propname) {
  if (arg->compartment() != obj->compartment()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_COMPARTMENT_MISMATCH, methodname,
                              propname);
    return false;
  }
  return true;
}

static bool CheckArgCompartment(JSContext* cx, JSObject* obj, HandleValue v,
                                const char* methodname, const char* propname) {
  if (v.isObject()) {
    return CheckArgCompartment(cx, obj, &v.toObject(), methodname, propname);
  }
  return true;
}

bool Debugger::unwrapPropertyDescriptor(
    JSContext* cx, HandleObject obj, MutableHandle<PropertyDescriptor> desc) {
  if (desc.hasValue()) {
    RootedValue value(cx, desc.value());
    if (!unwrapDebuggeeValue(cx, &value) ||
        !CheckArgCompartment(cx, obj, value, "defineProperty", "value")) {
      return false;
    }
    desc.setValue(value);
  }

  if (desc.hasGetter()) {
    RootedObject get(cx, desc.getter());
    if (get) {
      if (!unwrapDebuggeeObject(cx, &get)) {
        return false;
      }
      if (!CheckArgCompartment(cx, obj, get, "defineProperty", "get")) {
        return false;
      }
    }
    desc.setGetter(get);
  }

  if (desc.hasSetter()) {
    RootedObject set(cx, desc.setter());
    if (set) {
      if (!unwrapDebuggeeObject(cx, &set)) {
        return false;
      }
      if (!CheckArgCompartment(cx, obj, set, "defineProperty", "set")) {
        return false;
      }
    }
    desc.setSetter(set);
  }

  return true;
}

/*** Debuggee resumption values and debugger error handling *****************/

static bool GetResumptionProperty(JSContext* cx, HandleObject obj,
                                  HandlePropertyName name, ResumeMode namedMode,
                                  ResumeMode& resumeMode, MutableHandleValue vp,
                                  int* hits) {
  bool found;
  if (!HasProperty(cx, obj, name, &found)) {
    return false;
  }
  if (found) {
    ++*hits;
    resumeMode = namedMode;
    if (!GetProperty(cx, obj, obj, name, vp)) {
      return false;
    }
  }
  return true;
}

bool js::ParseResumptionValue(JSContext* cx, HandleValue rval,
                              ResumeMode& resumeMode, MutableHandleValue vp) {
  if (rval.isUndefined()) {
    resumeMode = ResumeMode::Continue;
    vp.setUndefined();
    return true;
  }
  if (rval.isNull()) {
    resumeMode = ResumeMode::Terminate;
    vp.setUndefined();
    return true;
  }

  int hits = 0;
  if (rval.isObject()) {
    RootedObject obj(cx, &rval.toObject());
    if (!GetResumptionProperty(cx, obj, cx->names().return_, ResumeMode::Return,
                               resumeMode, vp, &hits)) {
      return false;
    }
    if (!GetResumptionProperty(cx, obj, cx->names().throw_, ResumeMode::Throw,
                               resumeMode, vp, &hits)) {
      return false;
    }
  }

  if (hits != 1) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_BAD_RESUMPTION);
    return false;
  }
  return true;
}

static bool CheckResumptionValue(JSContext* cx, AbstractFramePtr frame,
                                 jsbytecode* pc, ResumeMode resumeMode,
                                 MutableHandleValue vp) {
  // Only forced returns from a frame need to be validated because forced
  // throw values behave just like debuggee `throw` statements. Since
  // forced-return is all custom logic within SpiderMonkey itself, we need
  // our own custom validation for it to conform with what is expected.
  if (resumeMode != ResumeMode::Return || !frame) {
    return true;
  }

  // This replicates the ECMA spec's behavior for [[Construct]] in derived
  // class constructors (section 9.2.2 of ECMA262-2020), where returning a
  // non-undefined primitive causes an exception tobe thrown.
  if (frame.debuggerNeedsCheckPrimitiveReturn() && vp.isPrimitive()) {
    if (!vp.isUndefined()) {
      ReportValueError(cx, JSMSG_BAD_DERIVED_RETURN, JSDVG_IGNORE_STACK, vp,
                       nullptr);
      return false;
    }

    RootedValue thisv(cx);
    {
      AutoRealm ar(cx, frame.environmentChain());
      if (!GetThisValueForDebuggerFrameMaybeOptimizedOut(cx, frame, pc,
                                                         &thisv)) {
        return false;
      }
    }

    if (thisv.isMagic(JS_UNINITIALIZED_LEXICAL)) {
      return ThrowUninitializedThis(cx);
    }
    MOZ_ASSERT(!thisv.isMagic());

    if (!cx->compartment()->wrap(cx, &thisv)) {
      return false;
    }
    vp.set(thisv);
  }

  // Check for forcing return from a generator before the initial yield. This
  // is not supported because some engine-internal code assumes a call to a
  // generator will return a GeneratorObject; see bug 1477084.
  if (frame.isFunctionFrame() && frame.callee()->isGenerator()) {
    Rooted<AbstractGeneratorObject*> genObj(cx);
    {
      AutoRealm ar(cx, frame.callee());
      genObj = GetGeneratorObjectForFrame(cx, frame);
    }

    if (!genObj || genObj->isBeforeInitialYield()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_FORCED_RETURN_DISALLOWED);
      return false;
    }
  }

  return true;
}

// Last-minute sanity adjustments to resumption.
//
// This is called last, as we leave the debugger. It must happen outside the
// control of the uncaughtExceptionHook, because this code assumes we won't
// change our minds and continue execution--we must not close the generator
// object unless we're really going to force-return.
[[nodiscard]] static bool AdjustGeneratorResumptionValue(
    JSContext* cx, AbstractFramePtr frame, ResumeMode& resumeMode,
    MutableHandleValue vp) {
  if (resumeMode != ResumeMode::Return && resumeMode != ResumeMode::Throw) {
    return true;
  }

  if (!frame) {
    return true;
  }
  // Async modules need to be handled separately, as they do not have a callee.
  // frame.callee will throw if it is called on a moduleFrame.
  bool isAsyncModule = frame.isModuleFrame() && frame.script()->isAsync();
  if (!frame.isFunctionFrame() && !isAsyncModule) {
    return true;
  }

  // Treat `{return: <value>}` like a `return` statement. Simulate what the
  // debuggee would do for an ordinary `return` statement, using a few bytecode
  // instructions. It's simpler to do the work manually than to count on that
  // bytecode sequence existing in the debuggee, somehow jump to it, and then
  // avoid re-entering the debugger from it.
  //
  // Similarly treat `{throw: <value>}` like a `throw` statement.
  //
  // Note: Async modules use the same handling as async functions.
  if (frame.isFunctionFrame() && frame.callee()->isGenerator()) {
    // Throw doesn't require any special processing for (async) generators.
    if (resumeMode == ResumeMode::Throw) {
      return true;
    }

    // Forcing return from a (possibly async) generator.
    Rooted<AbstractGeneratorObject*> genObj(
        cx, GetGeneratorObjectForFrame(cx, frame));

    // We already went through CheckResumptionValue, which would have replaced
    // this invalid resumption value with an error if we were trying to force
    // return before the initial yield.
    MOZ_RELEASE_ASSERT(genObj && !genObj->isBeforeInitialYield());

    // 1.  `return <value>` creates and returns a new object,
    //     `{value: <value>, done: true}`.
    //
    // For non-async generators, the iterator result object is created in
    // bytecode, so we have to simulate that here. For async generators, our
    // C++ implementation of AsyncGeneratorResolve will do this. So don't do it
    // twice:
    if (!genObj->is<AsyncGeneratorObject>()) {
      PlainObject* pair = CreateIterResultObject(cx, vp, true);
      if (!pair) {
        return false;
      }
      vp.setObject(*pair);
    }

    // 2.  The generator must be closed.
    genObj->setClosed();

    // Async generators have additionally bookkeeping which must be adjusted
    // when switching over to the closed state.
    if (genObj->is<AsyncGeneratorObject>()) {
      genObj->as<AsyncGeneratorObject>().setCompleted();
    }
  } else if (isAsyncModule || frame.callee()->isAsync()) {
    if (AbstractGeneratorObject* genObj =
            GetGeneratorObjectForFrame(cx, frame)) {
      // Throw doesn't require any special processing for async functions when
      // the internal generator object is already present.
      if (resumeMode == ResumeMode::Throw) {
        return true;
      }

      Rooted<AsyncFunctionGeneratorObject*> asyncGenObj(
          cx, &genObj->as<AsyncFunctionGeneratorObject>());

      // 1.  `return <value>` fulfills and returns the async function's promise.
      Rooted<PromiseObject*> promise(cx, asyncGenObj->promise());
      if (promise->state() == JS::PromiseState::Pending) {
        if (!AsyncFunctionResolve(cx, asyncGenObj, vp,
                                  AsyncFunctionResolveKind::Fulfill)) {
          return false;
        }
      }
      vp.setObject(*promise);

      // 2.  The generator must be closed.
      asyncGenObj->setClosed();
    } else {
      // We're before entering the actual function code.

      // 1.  `throw <value>` creates a promise rejected with the value *vp.
      // 1.  `return <value>` creates a promise resolved with the value *vp.
      JSObject* promise = resumeMode == ResumeMode::Throw
                              ? PromiseObject::unforgeableReject(cx, vp)
                              : PromiseObject::unforgeableResolve(cx, vp);
      if (!promise) {
        return false;
      }
      vp.setObject(*promise);

      // 2.  Return normally in both cases.
      resumeMode = ResumeMode::Return;
    }
  }

  return true;
}

bool Debugger::processParsedHandlerResult(JSContext* cx, AbstractFramePtr frame,
                                          jsbytecode* pc, bool success,
                                          ResumeMode resumeMode,
                                          HandleValue value,
                                          ResumeMode& resultMode,
                                          MutableHandleValue vp) {
  RootedValue rootValue(cx, value);
  if (!success || !prepareResumption(cx, frame, pc, resumeMode, &rootValue)) {
    RootedValue exceptionRv(cx);
    if (!callUncaughtExceptionHandler(cx, &exceptionRv) ||
        !ParseResumptionValue(cx, exceptionRv, resumeMode, &rootValue) ||
        !prepareResumption(cx, frame, pc, resumeMode, &rootValue)) {
      return false;
    }
  }

  // Since debugger hooks accumulate into the same final value handle, we
  // use that to throw if multiple hooks try to set a resumption value.
  if (resumeMode != ResumeMode::Continue) {
    if (resultMode != ResumeMode::Continue) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_RESUMPTION_CONFLICT);
      return false;
    }

    vp.set(rootValue);
    resultMode = resumeMode;
  }

  return true;
}

bool Debugger::processHandlerResult(JSContext* cx, bool success, HandleValue rv,
                                    AbstractFramePtr frame, jsbytecode* pc,
                                    ResumeMode& resultMode,
                                    MutableHandleValue vp) {
  ResumeMode resumeMode = ResumeMode::Continue;
  RootedValue value(cx);
  if (success) {
    success = ParseResumptionValue(cx, rv, resumeMode, &value);
  }
  return processParsedHandlerResult(cx, frame, pc, success, resumeMode, value,
                                    resultMode, vp);
}

bool Debugger::prepareResumption(JSContext* cx, AbstractFramePtr frame,
                                 jsbytecode* pc, ResumeMode& resumeMode,
                                 MutableHandleValue vp) {
  return unwrapDebuggeeValue(cx, vp) &&
         CheckResumptionValue(cx, frame, pc, resumeMode, vp);
}

bool Debugger::callUncaughtExceptionHandler(JSContext* cx,
                                            MutableHandleValue vp) {
  // Uncaught exceptions arise from Debugger code, and so we must already be in
  // an NX section. This also establishes that we are already within the scope
  // of an AutoDebuggerJobQueueInterruption object.
  MOZ_ASSERT(EnterDebuggeeNoExecute::isLockedInStack(cx, *this));

  if (cx->isExceptionPending() && uncaughtExceptionHook) {
    RootedValue exc(cx);
    if (!cx->getPendingException(&exc)) {
      return false;
    }
    cx->clearPendingException();

    RootedValue fval(cx, ObjectValue(*uncaughtExceptionHook));
    if (js::Call(cx, fval, object, exc, vp)) {
      return true;
    }
  }
  return false;
}

bool Debugger::handleUncaughtException(JSContext* cx) {
  RootedValue rv(cx);

  return callUncaughtExceptionHandler(cx, &rv);
}

void Debugger::reportUncaughtException(JSContext* cx) {
  // Uncaught exceptions arise from Debugger code, and so we must already be
  // in an NX section.
  MOZ_ASSERT(EnterDebuggeeNoExecute::isLockedInStack(cx, *this));

  if (cx->isExceptionPending()) {
    // We want to report the pending exception, but we want to let the
    // embedding handle it however it wants to.  So pretend like we're
    // starting a new script execution on our current compartment (which
    // is the debugger compartment, so reported errors won't get
    // reported to various onerror handlers in debuggees) and as part of
    // that "execution" simply throw our exception so the embedding can
    // deal.
    RootedValue exn(cx);
    if (cx->getPendingException(&exn)) {
      // Clear the exception, because ReportErrorToGlobal will assert that
      // we don't have one.
      cx->clearPendingException();
      ReportErrorToGlobal(cx, cx->global(), exn);
    }

    // And if not, or if PrepareScriptEnvironmentAndInvoke somehow left an
    // exception on cx (which it totally shouldn't do), just give up.
    cx->clearPendingException();
  }
}

/*** Debuggee completion values *********************************************/

/* static */
Completion Completion::fromJSResult(JSContext* cx, bool ok, const Value& rv) {
  MOZ_ASSERT_IF(ok, !cx->isExceptionPending());

  if (ok) {
    return Completion(Return(rv));
  }

  if (!cx->isExceptionPending()) {
    return Completion(Terminate());
  }

  RootedValue exception(cx);
  RootedSavedFrame stack(cx, cx->getPendingExceptionStack());
  bool getSucceeded = cx->getPendingException(&exception);
  cx->clearPendingException();
  if (!getSucceeded) {
    return Completion(Terminate());
  }

  return Completion(Throw(exception, stack));
}

/* static */
Completion Completion::fromJSFramePop(JSContext* cx, AbstractFramePtr frame,
                                      const jsbytecode* pc, bool ok) {
  // Only Wasm frames get a null pc.
  MOZ_ASSERT_IF(!frame.isWasmDebugFrame(), pc);

  // If this isn't a generator suspension, then that's already handled above.
  if (!ok || !frame.isGeneratorFrame()) {
    return fromJSResult(cx, ok, frame.returnValue());
  }

  // A generator is being suspended or returning.

  // Since generators are never wasm, we can assume pc is not nullptr, and
  // that analyzing bytecode is meaningful.
  MOZ_ASSERT(!frame.isWasmDebugFrame());

  // If we're leaving successfully at a yield opcode, we're probably
  // suspending; the `isClosed()` check detects a debugger forced return from
  // an `onStep` handler, which looks almost the same.
  //
  // GetGeneratorObjectForFrame can return nullptr even when a generator
  // object does exist, if the frame is paused between the Generator and
  // SetAliasedVar opcodes. But by checking the opcode first we eliminate that
  // possibility, so it's fine to call genObj->isClosed().
  Rooted<AbstractGeneratorObject*> generatorObj(
      cx, GetGeneratorObjectForFrame(cx, frame));
  switch (JSOp(*pc)) {
    case JSOp::InitialYield:
      MOZ_ASSERT(!generatorObj->isClosed());
      return Completion(InitialYield(generatorObj));

    case JSOp::Yield:
      MOZ_ASSERT(!generatorObj->isClosed());
      return Completion(Yield(generatorObj, frame.returnValue()));

    case JSOp::Await:
      MOZ_ASSERT(!generatorObj->isClosed());
      return Completion(Await(generatorObj, frame.returnValue()));

    default:
      return Completion(Return(frame.returnValue()));
  }
}

void Completion::trace(JSTracer* trc) {
  variant.match([=](auto& var) { var.trace(trc); });
}

struct MOZ_STACK_CLASS Completion::BuildValueMatcher {
  JSContext* cx;
  Debugger* dbg;
  MutableHandleValue result;

  BuildValueMatcher(JSContext* cx, Debugger* dbg, MutableHandleValue result)
      : cx(cx), dbg(dbg), result(result) {
    cx->check(dbg->toJSObject());
  }

  bool operator()(const Completion::Return& ret) {
    RootedNativeObject obj(cx, newObject());
    RootedValue retval(cx, ret.value);
    if (!obj || !wrap(&retval) || !add(obj, cx->names().return_, retval)) {
      return false;
    }
    result.setObject(*obj);
    return true;
  }

  bool operator()(const Completion::Throw& thr) {
    RootedNativeObject obj(cx, newObject());
    RootedValue exc(cx, thr.exception);
    if (!obj || !wrap(&exc) || !add(obj, cx->names().throw_, exc)) {
      return false;
    }
    if (thr.stack) {
      RootedValue stack(cx, ObjectValue(*thr.stack));
      if (!wrapStack(&stack) || !add(obj, cx->names().stack, stack)) {
        return false;
      }
    }
    result.setObject(*obj);
    return true;
  }

  bool operator()(const Completion::Terminate& term) {
    result.setNull();
    return true;
  }

  bool operator()(const Completion::InitialYield& initialYield) {
    RootedNativeObject obj(cx, newObject());
    RootedValue gen(cx, ObjectValue(*initialYield.generatorObject));
    if (!obj || !wrap(&gen) || !add(obj, cx->names().return_, gen) ||
        !add(obj, cx->names().yield, TrueHandleValue) ||
        !add(obj, cx->names().initial, TrueHandleValue)) {
      return false;
    }
    result.setObject(*obj);
    return true;
  }

  bool operator()(const Completion::Yield& yield) {
    RootedNativeObject obj(cx, newObject());
    RootedValue iteratorResult(cx, yield.iteratorResult);
    if (!obj || !wrap(&iteratorResult) ||
        !add(obj, cx->names().return_, iteratorResult) ||
        !add(obj, cx->names().yield, TrueHandleValue)) {
      return false;
    }
    result.setObject(*obj);
    return true;
  }

  bool operator()(const Completion::Await& await) {
    RootedNativeObject obj(cx, newObject());
    RootedValue awaitee(cx, await.awaitee);
    if (!obj || !wrap(&awaitee) || !add(obj, cx->names().return_, awaitee) ||
        !add(obj, cx->names().await, TrueHandleValue)) {
      return false;
    }
    result.setObject(*obj);
    return true;
  }

 private:
  NativeObject* newObject() const {
    return NewBuiltinClassInstance<PlainObject>(cx);
  }

  bool add(HandleNativeObject obj, PropertyName* name,
           HandleValue value) const {
    return NativeDefineDataProperty(cx, obj, name, value, JSPROP_ENUMERATE);
  }

  bool wrap(MutableHandleValue v) const {
    return dbg->wrapDebuggeeValue(cx, v);
  }

  // Saved stacks are wrapped for direct consumption by debugger code.
  bool wrapStack(MutableHandleValue stack) const {
    return cx->compartment()->wrap(cx, stack);
  }
};

bool Completion::buildCompletionValue(JSContext* cx, Debugger* dbg,
                                      MutableHandleValue result) const {
  return variant.match(BuildValueMatcher(cx, dbg, result));
}

void Completion::updateFromHookResult(ResumeMode resumeMode,
                                      HandleValue value) {
  switch (resumeMode) {
    case ResumeMode::Continue:
      // No change to how we'll resume.
      break;

    case ResumeMode::Throw:
      // Since this is a new exception, the stack for the old one may not apply.
      // If we extend resumption values to specify stacks, we could revisit
      // this.
      variant = Variant(Throw(value, nullptr));
      break;

    case ResumeMode::Terminate:
      variant = Variant(Terminate());
      break;

    case ResumeMode::Return:
      variant = Variant(Return(value));
      break;

    default:
      MOZ_CRASH("invalid resumeMode value");
  }
}

struct MOZ_STACK_CLASS Completion::ToResumeModeMatcher {
  MutableHandleValue value;
  MutableHandleSavedFrame exnStack;
  ToResumeModeMatcher(MutableHandleValue value,
                      MutableHandleSavedFrame exnStack)
      : value(value), exnStack(exnStack) {}

  ResumeMode operator()(const Return& ret) {
    value.set(ret.value);
    return ResumeMode::Return;
  }

  ResumeMode operator()(const Throw& thr) {
    value.set(thr.exception);
    exnStack.set(thr.stack);
    return ResumeMode::Throw;
  }

  ResumeMode operator()(const Terminate& term) {
    value.setUndefined();
    return ResumeMode::Terminate;
  }

  ResumeMode operator()(const InitialYield& initialYield) {
    value.setObject(*initialYield.generatorObject);
    return ResumeMode::Return;
  }

  ResumeMode operator()(const Yield& yield) {
    value.set(yield.iteratorResult);
    return ResumeMode::Return;
  }

  ResumeMode operator()(const Await& await) {
    value.set(await.awaitee);
    return ResumeMode::Return;
  }
};

void Completion::toResumeMode(ResumeMode& resumeMode, MutableHandleValue value,
                              MutableHandleSavedFrame exnStack) const {
  resumeMode = variant.match(ToResumeModeMatcher(value, exnStack));
}

/*** Firing debugger hooks **************************************************/

static bool CallMethodIfPresent(JSContext* cx, HandleObject obj,
                                const char* name, size_t argc, Value* argv,
                                MutableHandleValue rval) {
  rval.setUndefined();
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }

  RootedId id(cx, AtomToId(atom));
  RootedValue fval(cx);
  if (!GetProperty(cx, obj, obj, id, &fval)) {
    return false;
  }

  if (!IsCallable(fval)) {
    return true;
  }

  InvokeArgs args(cx);
  if (!args.init(cx, argc)) {
    return false;
  }

  for (size_t i = 0; i < argc; i++) {
    args[i].set(argv[i]);
  }

  rval.setObject(*obj);  // overwritten by successful Call
  return js::Call(cx, fval, rval, args, rval);
}

bool Debugger::fireDebuggerStatement(JSContext* cx, ResumeMode& resumeMode,
                                     MutableHandleValue vp) {
  RootedObject hook(cx, getHook(OnDebuggerStatement));
  MOZ_ASSERT(hook);
  MOZ_ASSERT(hook->isCallable());

  ScriptFrameIter iter(cx);
  RootedValue scriptFrame(cx);
  if (!getFrame(cx, iter, &scriptFrame)) {
    return false;
  }

  RootedValue fval(cx, ObjectValue(*hook));
  RootedValue rv(cx);
  bool ok = js::Call(cx, fval, object, scriptFrame, &rv);
  return processHandlerResult(cx, ok, rv, iter.abstractFramePtr(), iter.pc(),
                              resumeMode, vp);
}

bool Debugger::fireExceptionUnwind(JSContext* cx, HandleValue exc,
                                   ResumeMode& resumeMode,
                                   MutableHandleValue vp) {
  RootedObject hook(cx, getHook(OnExceptionUnwind));
  MOZ_ASSERT(hook);
  MOZ_ASSERT(hook->isCallable());

  RootedValue scriptFrame(cx);
  RootedValue wrappedExc(cx, exc);

  FrameIter iter(cx);
  if (!getFrame(cx, iter, &scriptFrame) ||
      !wrapDebuggeeValue(cx, &wrappedExc)) {
    return false;
  }

  RootedValue fval(cx, ObjectValue(*hook));
  RootedValue rv(cx);
  bool ok = js::Call(cx, fval, object, scriptFrame, wrappedExc, &rv);
  return processHandlerResult(cx, ok, rv, iter.abstractFramePtr(), iter.pc(),
                              resumeMode, vp);
}

bool Debugger::fireEnterFrame(JSContext* cx, ResumeMode& resumeMode,
                              MutableHandleValue vp) {
  RootedObject hook(cx, getHook(OnEnterFrame));
  MOZ_ASSERT(hook);
  MOZ_ASSERT(hook->isCallable());

  RootedValue scriptFrame(cx);

  FrameIter iter(cx);

#if DEBUG
  // Assert that the hook won't be able to re-enter the generator.
  if (iter.hasScript() && JSOp(*iter.pc()) == JSOp::AfterYield) {
    AutoRealm ar(cx, iter.script());
    auto* genObj = GetGeneratorObjectForFrame(cx, iter.abstractFramePtr());
    MOZ_ASSERT(genObj->isRunning());
  }
#endif

  if (!getFrame(cx, iter, &scriptFrame)) {
    return false;
  }

  RootedValue fval(cx, ObjectValue(*hook));
  RootedValue rv(cx);
  bool ok = js::Call(cx, fval, object, scriptFrame, &rv);

  return processHandlerResult(cx, ok, rv, iter.abstractFramePtr(), iter.pc(),
                              resumeMode, vp);
}

bool Debugger::fireNativeCall(JSContext* cx, const CallArgs& args,
                              CallReason reason, ResumeMode& resumeMode,
                              MutableHandleValue vp) {
  RootedObject hook(cx, getHook(OnNativeCall));
  MOZ_ASSERT(hook);
  MOZ_ASSERT(hook->isCallable());

  RootedValue fval(cx, ObjectValue(*hook));
  RootedValue calleeval(cx, args.calleev());
  if (!wrapDebuggeeValue(cx, &calleeval)) {
    return false;
  }

  JSAtom* reasonAtom = nullptr;
  switch (reason) {
    case CallReason::Call:
      reasonAtom = cx->names().call;
      break;
    case CallReason::Getter:
      reasonAtom = cx->names().get;
      break;
    case CallReason::Setter:
      reasonAtom = cx->names().set;
      break;
  }
  cx->markAtom(reasonAtom);

  RootedValue reasonval(cx, StringValue(reasonAtom));

  RootedValue rv(cx);
  bool ok = js::Call(cx, fval, object, calleeval, reasonval, &rv);

  return processHandlerResult(cx, ok, rv, NullFramePtr(), nullptr, resumeMode,
                              vp);
}

bool Debugger::fireNewScript(JSContext* cx,
                             Handle<DebuggerScriptReferent> scriptReferent) {
  RootedObject hook(cx, getHook(OnNewScript));
  MOZ_ASSERT(hook);
  MOZ_ASSERT(hook->isCallable());

  JSObject* dsobj = wrapVariantReferent(cx, scriptReferent);
  if (!dsobj) {
    return false;
  }

  RootedValue fval(cx, ObjectValue(*hook));
  RootedValue dsval(cx, ObjectValue(*dsobj));
  RootedValue rv(cx);
  return js::Call(cx, fval, object, dsval, &rv) || handleUncaughtException(cx);
}

bool Debugger::fireOnGarbageCollectionHook(
    JSContext* cx, const JS::dbg::GarbageCollectionEvent::Ptr& gcData) {
  MOZ_ASSERT(observedGC(gcData->majorGCNumber()));
  observedGCs.remove(gcData->majorGCNumber());

  RootedObject hook(cx, getHook(OnGarbageCollection));
  MOZ_ASSERT(hook);
  MOZ_ASSERT(hook->isCallable());

  JSObject* dataObj = gcData->toJSObject(cx);
  if (!dataObj) {
    return false;
  }

  RootedValue fval(cx, ObjectValue(*hook));
  RootedValue dataVal(cx, ObjectValue(*dataObj));
  RootedValue rv(cx);
  return js::Call(cx, fval, object, dataVal, &rv) ||
         handleUncaughtException(cx);
}

template <typename HookIsEnabledFun /* bool (Debugger*) */,
          typename FireHookFun /* bool (Debugger*) */>
/* static */
void Debugger::dispatchQuietHook(JSContext* cx, HookIsEnabledFun hookIsEnabled,
                                 FireHookFun fireHook) {
  DebuggerList<HookIsEnabledFun> debuggerList(cx, hookIsEnabled);

  if (!debuggerList.init(cx)) {
    // init may fail due to OOM. This OOM is not handlable at the
    // callsites of dispatchQuietHook in the engine.
    cx->clearPendingException();
    return;
  }

  debuggerList.dispatchQuietHook(cx, fireHook);
}

template <typename HookIsEnabledFun /* bool (Debugger*) */, typename FireHookFun /* bool (Debugger*, ResumeMode&, MutableHandleValue vp) */>
/* static */
bool Debugger::dispatchResumptionHook(JSContext* cx, AbstractFramePtr frame,
                                      HookIsEnabledFun hookIsEnabled,
                                      FireHookFun fireHook) {
  DebuggerList<HookIsEnabledFun> debuggerList(cx, hookIsEnabled);

  if (!debuggerList.init(cx)) {
    return false;
  }

  return debuggerList.dispatchResumptionHook(cx, frame, fireHook);
}

// Maximum length for source URLs that can be remembered.
static const size_t SourceURLMaxLength = 1024;

// Maximum number of source URLs that can be remembered in a realm.
static const size_t SourceURLRealmLimit = 100;

static bool RememberSourceURL(JSContext* cx, HandleScript script) {
  cx->check(script);

  // Sources introduced dynamically are not remembered.
  if (script->sourceObject()->unwrappedIntroductionScript()) {
    return true;
  }

  const char* filename = script->filename();
  if (!filename ||
      strnlen(filename, SourceURLMaxLength + 1) > SourceURLMaxLength) {
    return true;
  }

  RootedObject holder(cx, script->global().getSourceURLsHolder());
  if (!holder) {
    holder = NewDenseEmptyArray(cx);
    if (!holder) {
      return false;
    }
    script->global().setSourceURLsHolder(holder);
  }

  if (holder->as<ArrayObject>().length() >= SourceURLRealmLimit) {
    return true;
  }

  RootedString filenameString(cx, JS_AtomizeString(cx, filename));
  if (!filenameString) {
    return false;
  }

  // The source URLs holder never escapes to script, so we can treat it as a
  // newborn array for the purpose of adding elements.
  return NewbornArrayPush(cx, holder, StringValue(filenameString));
}

void DebugAPI::onNewScript(JSContext* cx, HandleScript script) {
  if (!script->realm()->isDebuggee()) {
    // Remember the URLs associated with scripts in non-system realms,
    // in case the debugger is attached later.
    if (!script->realm()->isSystem()) {
      if (!RememberSourceURL(cx, script)) {
        cx->clearPendingException();
      }
    }
    return;
  }

  Debugger::dispatchQuietHook(
      cx,
      [script](Debugger* dbg) -> bool {
        return dbg->observesNewScript() && dbg->observesScript(script);
      },
      [&](Debugger* dbg) -> bool {
        BaseScript* base = script.get();
        Rooted<DebuggerScriptReferent> scriptReferent(cx, base);
        return dbg->fireNewScript(cx, scriptReferent);
      });
}

void DebugAPI::slowPathOnNewWasmInstance(
    JSContext* cx, Handle<WasmInstanceObject*> wasmInstance) {
  Debugger::dispatchQuietHook(
      cx,
      [wasmInstance](Debugger* dbg) -> bool {
        return dbg->observesNewScript() &&
               dbg->observesGlobal(&wasmInstance->global());
      },
      [&](Debugger* dbg) -> bool {
        Rooted<DebuggerScriptReferent> scriptReferent(cx, wasmInstance.get());
        return dbg->fireNewScript(cx, scriptReferent);
      });
}

/* static */
bool DebugAPI::onTrap(JSContext* cx) {
  FrameIter iter(cx);
  JS::AutoSaveExceptionState savedExc(cx);
  Rooted<GlobalObject*> global(cx);
  BreakpointSite* site;
  bool isJS;       // true when iter.hasScript(), false when iter.isWasm()
  jsbytecode* pc;  // valid when isJS == true
  uint32_t bytecodeOffset;  // valid when isJS == false
  if (iter.hasScript()) {
    RootedScript script(cx, iter.script());
    MOZ_ASSERT(script->isDebuggee());
    global.set(&script->global());
    isJS = true;
    pc = iter.pc();
    bytecodeOffset = 0;
    site = DebugScript::getBreakpointSite(script, pc);
  } else {
    MOZ_ASSERT(iter.isWasm());
    global.set(&iter.wasmInstance()->object()->global());
    isJS = false;
    pc = nullptr;
    bytecodeOffset = iter.wasmBytecodeOffset();
    site = iter.wasmInstance()->debug().getBreakpointSite(bytecodeOffset);
  }

  // Build list of breakpoint handlers.
  //
  // This does not need to be rooted: since the JSScript/WasmInstance is on the
  // stack, the Breakpoints will not be GC'd. However, they may be deleted, and
  // we check for that case below.
  Vector<Breakpoint*> triggered(cx);
  for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = bp->nextInSite()) {
    if (!triggered.append(bp)) {
      return false;
    }
  }

  ResumeMode resumeMode = ResumeMode::Continue;
  RootedValue rval(cx);

  if (triggered.length() > 0) {
    // Preserve the debuggee's microtask event queue while we run the hooks, so
    // the debugger's microtask checkpoints don't run from the debuggee's
    // microtasks, and vice versa.
    JS::AutoDebuggerJobQueueInterruption adjqi;
    if (!adjqi.init(cx)) {
      return false;
    }

    for (Breakpoint* bp : triggered) {
      // Handlers can clear breakpoints. Check that bp still exists.
      if (!site || !site->hasBreakpoint(bp)) {
        continue;
      }

      // There are two reasons we have to check whether dbg is debugging
      // global.
      //
      // One is just that one breakpoint handler can disable other Debuggers
      // or remove debuggees.
      //
      // The other has to do with non-compile-and-go scripts, which have no
      // specific global--until they are executed. Only now do we know which
      // global the script is running against.
      Debugger* dbg = bp->debugger;
      if (dbg->debuggees.has(global)) {
        EnterDebuggeeNoExecute nx(cx, *dbg, adjqi);

        bool result = dbg->enterDebuggerHook(cx, [&]() -> bool {
          RootedValue scriptFrame(cx);
          if (!dbg->getFrame(cx, iter, &scriptFrame)) {
            return false;
          }

          // Re-wrap the breakpoint's handler for the Debugger's compartment.
          // When the handler and the Debugger are in the same compartment (the
          // usual case), this actually unwraps it, but there's no requirement
          // that they be in the same compartment, so we can't be sure.
          Rooted<JSObject*> handler(cx, bp->handler);
          if (!cx->compartment()->wrap(cx, &handler)) {
            return false;
          }

          RootedValue rv(cx);
          bool ok = CallMethodIfPresent(cx, handler, "hit", 1,
                                        scriptFrame.address(), &rv);

          return dbg->processHandlerResult(cx, ok, rv, iter.abstractFramePtr(),
                                           iter.pc(), resumeMode, &rval);
        });
        adjqi.runJobs();

        if (!result) {
          return false;
        }

        // Calling JS code invalidates site. Reload it.
        if (isJS) {
          site = DebugScript::getBreakpointSite(iter.script(), pc);
        } else {
          site = iter.wasmInstance()->debug().getBreakpointSite(bytecodeOffset);
        }
      }
    }
  }

  if (!ApplyFrameResumeMode(cx, iter.abstractFramePtr(), resumeMode, rval)) {
    savedExc.drop();
    return false;
  }
  return true;
}

/* static */
bool DebugAPI::onSingleStep(JSContext* cx) {
  FrameIter iter(cx);

  // We may be stepping over a JSOp::Exception, that pushes the context's
  // pending exception for a 'catch' clause to handle. Don't let the onStep
  // handlers mess with that (other than by returning a resumption value).
  JS::AutoSaveExceptionState savedExc(cx);

  // Build list of Debugger.Frame instances referring to this frame with
  // onStep handlers.
  Rooted<Debugger::DebuggerFrameVector> frames(
      cx, Debugger::DebuggerFrameVector(cx));
  if (!Debugger::getDebuggerFrames(iter.abstractFramePtr(), &frames)) {
    return false;
  }

#ifdef DEBUG
  // Validate the single-step count on this frame's script, to ensure that
  // we're not receiving traps we didn't ask for. Even when frames is
  // non-empty (and thus we know this trap was requested), do the check
  // anyway, to make sure the count has the correct non-zero value.
  //
  // The converse --- ensuring that we do receive traps when we should --- can
  // be done with unit tests.
  if (iter.hasScript()) {
    uint32_t liveStepperCount = 0;
    uint32_t suspendedStepperCount = 0;
    JSScript* trappingScript = iter.script();
    for (Realm::DebuggerVectorEntry& entry : cx->global()->getDebuggers()) {
      Debugger* dbg = entry.dbg;
      for (Debugger::FrameMap::Range r = dbg->frames.all(); !r.empty();
           r.popFront()) {
        AbstractFramePtr frame = r.front().key();
        NativeObject* frameobj = r.front().value();
        if (frame.isWasmDebugFrame()) {
          continue;
        }
        if (frame.script() == trappingScript &&
            !frameobj->getReservedSlot(DebuggerFrame::ONSTEP_HANDLER_SLOT)
                 .isUndefined()) {
          liveStepperCount++;
        }
      }

      // Also count hooks set on suspended generator frames.
      for (Debugger::GeneratorWeakMap::Range r = dbg->generatorFrames.all();
           !r.empty(); r.popFront()) {
        AbstractGeneratorObject& genObj = *r.front().key();
        DebuggerFrame& frameObj = *r.front().value();
        MOZ_ASSERT(&frameObj.unwrappedGenerator() == &genObj);

        // Live Debugger.Frames were already counted in dbg->frames loop.
        if (frameObj.isOnStack()) {
          continue;
        }

        // A closed generator no longer has a callee so it will not be able to
        // compare with the trappingScript.
        if (genObj.isClosed()) {
          continue;
        }

        // If a frame isn't live, but it has an entry in generatorFrames,
        // it had better be suspended.
        MOZ_ASSERT(genObj.isSuspended());

        if (genObj.callee().hasBaseScript() &&
            genObj.callee().baseScript() == trappingScript &&
            !frameObj.getReservedSlot(DebuggerFrame::ONSTEP_HANDLER_SLOT)
                 .isUndefined()) {
          suspendedStepperCount++;
        }
      }
    }

    MOZ_ASSERT(liveStepperCount + suspendedStepperCount ==
               DebugScript::getStepperCount(trappingScript));
  }
#endif

  RootedValue rval(cx);
  ResumeMode resumeMode = ResumeMode::Continue;

  if (frames.length() > 0) {
    // Preserve the debuggee's microtask event queue while we run the hooks, so
    // the debugger's microtask checkpoints don't run from the debuggee's
    // microtasks, and vice versa.
    JS::AutoDebuggerJobQueueInterruption adjqi;
    if (!adjqi.init(cx)) {
      return false;
    }

    // Call onStep for frames that have the handler set.
    for (size_t i = 0; i < frames.length(); i++) {
      HandleDebuggerFrame frame = frames[i];
      OnStepHandler* handler = frame->onStepHandler();
      if (!handler) {
        continue;
      }

      Debugger* dbg = frame->owner();
      EnterDebuggeeNoExecute nx(cx, *dbg, adjqi);

      bool result = dbg->enterDebuggerHook(cx, [&]() -> bool {
        ResumeMode nextResumeMode = ResumeMode::Continue;
        RootedValue nextValue(cx);

        bool success = handler->onStep(cx, frame, nextResumeMode, &nextValue);
        return dbg->processParsedHandlerResult(
            cx, iter.abstractFramePtr(), iter.pc(), success, nextResumeMode,
            nextValue, resumeMode, &rval);
      });
      adjqi.runJobs();

      if (!result) {
        return false;
      }
    }
  }

  if (!ApplyFrameResumeMode(cx, iter.abstractFramePtr(), resumeMode, rval)) {
    savedExc.drop();
    return false;
  }
  return true;
}

bool Debugger::fireNewGlobalObject(JSContext* cx,
                                   Handle<GlobalObject*> global) {
  RootedObject hook(cx, getHook(OnNewGlobalObject));
  MOZ_ASSERT(hook);
  MOZ_ASSERT(hook->isCallable());

  RootedValue wrappedGlobal(cx, ObjectValue(*global));
  if (!wrapDebuggeeValue(cx, &wrappedGlobal)) {
    return false;
  }

  // onNewGlobalObject is infallible, and thus is only allowed to return
  // undefined as a resumption value. If it returns anything else, we throw.
  // And if that happens, or if the hook itself throws, we invoke the
  // uncaughtExceptionHook so that we never leave an exception pending on the
  // cx. This allows JS_NewGlobalObject to avoid handling failures from
  // debugger hooks.
  RootedValue rv(cx);
  RootedValue fval(cx, ObjectValue(*hook));
  bool ok = js::Call(cx, fval, object, wrappedGlobal, &rv);
  if (ok && !rv.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_RESUMPTION_VALUE_DISALLOWED);
    ok = false;
  }

  return ok || handleUncaughtException(cx);
}

void DebugAPI::slowPathOnNewGlobalObject(JSContext* cx,
                                         Handle<GlobalObject*> global) {
  MOZ_ASSERT(!cx->runtime()->onNewGlobalObjectWatchers().isEmpty());
  if (global->realm()->creationOptions().invisibleToDebugger()) {
    return;
  }

  // Make a copy of the runtime's onNewGlobalObjectWatchers before running the
  // handlers. Since one Debugger's handler can disable another's, the list
  // can be mutated while we're walking it.
  RootedObjectVector watchers(cx);
  for (auto& dbg : cx->runtime()->onNewGlobalObjectWatchers()) {
    MOZ_ASSERT(dbg.observesNewGlobalObject());
    JSObject* obj = dbg.object;
    JS::ExposeObjectToActiveJS(obj);
    if (!watchers.append(obj)) {
      if (cx->isExceptionPending()) {
        cx->clearPendingException();
      }
      return;
    }
  }

  // Preserve the debuggee's microtask event queue while we run the hooks, so
  // the debugger's microtask checkpoints don't run from the debuggee's
  // microtasks, and vice versa.
  JS::AutoDebuggerJobQueueInterruption adjqi;
  if (!adjqi.init(cx)) {
    cx->clearPendingException();
    return;
  }

  for (size_t i = 0; i < watchers.length(); i++) {
    Debugger* dbg = Debugger::fromJSObject(watchers[i]);
    EnterDebuggeeNoExecute nx(cx, *dbg, adjqi);

    if (dbg->observesNewGlobalObject()) {
      bool result = dbg->enterDebuggerHook(
          cx, [&]() -> bool { return dbg->fireNewGlobalObject(cx, global); });
      adjqi.runJobs();

      if (!result) {
        // Like other quiet hooks using dispatchQuietHook, this hook
        // silently ignores all errors that propagate out of it and aren't
        // already handled by the hook error reporting.
        cx->clearPendingException();
        break;
      }
    }
  }
  MOZ_ASSERT(!cx->isExceptionPending());
}

/* static */
void DebugAPI::slowPathNotifyParticipatesInGC(uint64_t majorGCNumber,
                                              Realm::DebuggerVector& dbgs) {
  for (Realm::DebuggerVector::Range r = dbgs.all(); !r.empty(); r.popFront()) {
    if (!r.front().dbg.unbarrieredGet()->debuggeeIsBeingCollected(
            majorGCNumber)) {
#ifdef DEBUG
      fprintf(stderr,
              "OOM while notifying observing Debuggers of a GC: The "
              "onGarbageCollection\n"
              "hook will not be fired for this GC for some Debuggers!\n");
#endif
      return;
    }
  }
}

/* static */
Maybe<double> DebugAPI::allocationSamplingProbability(GlobalObject* global) {
  Realm::DebuggerVector& dbgs = global->getDebuggers();
  if (dbgs.empty()) {
    return Nothing();
  }

  DebugOnly<Realm::DebuggerVectorEntry*> begin = dbgs.begin();

  double probability = 0;
  bool foundAnyDebuggers = false;
  for (auto p = dbgs.begin(); p < dbgs.end(); p++) {
    // The set of debuggers had better not change while we're iterating,
    // such that the vector gets reallocated.
    MOZ_ASSERT(dbgs.begin() == begin);
    // Use unbarrieredGet() to prevent triggering read barrier while collecting,
    // this is safe as long as dbgp does not escape.
    Debugger* dbgp = p->dbg.unbarrieredGet();

    if (dbgp->trackingAllocationSites) {
      foundAnyDebuggers = true;
      probability = std::max(dbgp->allocationSamplingProbability, probability);
    }
  }

  return foundAnyDebuggers ? Some(probability) : Nothing();
}

/* static */
bool DebugAPI::slowPathOnLogAllocationSite(JSContext* cx, HandleObject obj,
                                           HandleSavedFrame frame,
                                           mozilla::TimeStamp when,
                                           Realm::DebuggerVector& dbgs) {
  MOZ_ASSERT(!dbgs.empty());
  mozilla::DebugOnly<Realm::DebuggerVectorEntry*> begin = dbgs.begin();

  // Root all the Debuggers while we're iterating over them;
  // appendAllocationSite calls Compartment::wrap, and thus can GC.
  //
  // SpiderMonkey protocol is generally for the caller to prove that it has
  // rooted the stuff it's asking you to operate on (i.e. by passing a
  // Handle), but in this case, we're iterating over a global's list of
  // Debuggers, and globals only hold their Debuggers weakly.
  Rooted<GCVector<JSObject*>> activeDebuggers(cx, GCVector<JSObject*>(cx));
  for (auto p = dbgs.begin(); p < dbgs.end(); p++) {
    if (!activeDebuggers.append(p->dbg->object)) {
      return false;
    }
  }

  for (auto p = dbgs.begin(); p < dbgs.end(); p++) {
    // The set of debuggers had better not change while we're iterating,
    // such that the vector gets reallocated.
    MOZ_ASSERT(dbgs.begin() == begin);

    if (p->dbg->trackingAllocationSites &&
        !p->dbg->appendAllocationSite(cx, obj, frame, when)) {
      return false;
    }
  }

  return true;
}

bool Debugger::isDebuggeeUnbarriered(const Realm* realm) const {
  MOZ_ASSERT(realm);
  return realm->isDebuggee() &&
         debuggees.has(realm->unsafeUnbarrieredMaybeGlobal());
}

bool Debugger::appendAllocationSite(JSContext* cx, HandleObject obj,
                                    HandleSavedFrame frame,
                                    mozilla::TimeStamp when) {
  MOZ_ASSERT(trackingAllocationSites);

  AutoRealm ar(cx, object);
  RootedObject wrappedFrame(cx, frame);
  if (!cx->compartment()->wrap(cx, &wrappedFrame)) {
    return false;
  }

  auto className = obj->getClass()->name;
  auto size =
      JS::ubi::Node(obj.get()).size(cx->runtime()->debuggerMallocSizeOf);
  auto inNursery = gc::IsInsideNursery(obj);

  if (!allocationsLog.emplaceBack(wrappedFrame, when, className, size,
                                  inNursery)) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (allocationsLog.length() > maxAllocationsLogLength) {
    allocationsLog.popFront();
    MOZ_ASSERT(allocationsLog.length() == maxAllocationsLogLength);
    allocationsLogOverflowed = true;
  }

  return true;
}

bool Debugger::firePromiseHook(JSContext* cx, Hook hook, HandleObject promise) {
  MOZ_ASSERT(hook == OnNewPromise || hook == OnPromiseSettled);

  RootedObject hookObj(cx, getHook(hook));
  MOZ_ASSERT(hookObj);
  MOZ_ASSERT(hookObj->isCallable());

  RootedValue dbgObj(cx, ObjectValue(*promise));
  if (!wrapDebuggeeValue(cx, &dbgObj)) {
    return false;
  }

  // Like onNewGlobalObject, the Promise hooks are infallible and the comments
  // in |Debugger::fireNewGlobalObject| apply here as well.
  RootedValue fval(cx, ObjectValue(*hookObj));
  RootedValue rv(cx);
  bool ok = js::Call(cx, fval, object, dbgObj, &rv);
  if (ok && !rv.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_RESUMPTION_VALUE_DISALLOWED);
    ok = false;
  }

  return ok || handleUncaughtException(cx);
}

/* static */
void Debugger::slowPathPromiseHook(JSContext* cx, Hook hook,
                                   Handle<PromiseObject*> promise) {
  MOZ_ASSERT(hook == OnNewPromise || hook == OnPromiseSettled);

  if (hook == OnPromiseSettled) {
    // We should be in the right compartment, but for simplicity always enter
    // the promise's realm below.
    cx->check(promise);
  }

  AutoRealm ar(cx, promise);

  Debugger::dispatchQuietHook(
      cx, [hook](Debugger* dbg) -> bool { return dbg->getHook(hook); },
      [&](Debugger* dbg) -> bool {
        return dbg->firePromiseHook(cx, hook, promise);
      });
}

/* static */
void DebugAPI::slowPathOnNewPromise(JSContext* cx,
                                    Handle<PromiseObject*> promise) {
  Debugger::slowPathPromiseHook(cx, Debugger::OnNewPromise, promise);
}

/* static */
void DebugAPI::slowPathOnPromiseSettled(JSContext* cx,
                                        Handle<PromiseObject*> promise) {
  Debugger::slowPathPromiseHook(cx, Debugger::OnPromiseSettled, promise);
}

/*** Debugger code invalidation for observing execution *********************/

class MOZ_RAII ExecutionObservableRealms
    : public DebugAPI::ExecutionObservableSet {
  HashSet<Realm*> realms_;
  HashSet<Zone*> zones_;

 public:
  explicit ExecutionObservableRealms(JSContext* cx) : realms_(cx), zones_(cx) {}

  bool add(Realm* realm) {
    return realms_.put(realm) && zones_.put(realm->zone());
  }

  using RealmRange = HashSet<Realm*>::Range;
  const HashSet<Realm*>* realms() const { return &realms_; }

  const HashSet<Zone*>* zones() const override { return &zones_; }
  bool shouldRecompileOrInvalidate(JSScript* script) const override {
    return script->hasBaselineScript() && realms_.has(script->realm());
  }
  bool shouldMarkAsDebuggee(FrameIter& iter) const override {
    // AbstractFramePtr can't refer to non-remateralized Ion frames or
    // non-debuggee wasm frames, so if iter refers to one such, we know we
    // don't match.
    return iter.hasUsableAbstractFramePtr() && realms_.has(iter.realm());
  }
};

// Given a particular AbstractFramePtr F that has become observable, this
// represents the stack frames that need to be bailed out or marked as
// debuggees, and the scripts that need to be recompiled, taking inlining into
// account.
class MOZ_RAII ExecutionObservableFrame
    : public DebugAPI::ExecutionObservableSet {
  AbstractFramePtr frame_;

 public:
  explicit ExecutionObservableFrame(AbstractFramePtr frame) : frame_(frame) {}

  Zone* singleZone() const override {
    // We never inline across realms, let alone across zones, so
    // frames_'s script's zone is the only one of interest.
    return frame_.script()->zone();
  }

  JSScript* singleScriptForZoneInvalidation() const override {
    MOZ_CRASH(
        "ExecutionObservableFrame shouldn't need zone-wide invalidation.");
    return nullptr;
  }

  bool shouldRecompileOrInvalidate(JSScript* script) const override {
    // Normally, *this represents exactly one script: the one frame_ is
    // running.
    //
    // However, debug-mode OSR uses *this for both invalidating Ion frames,
    // and recompiling the Baseline scripts that those Ion frames will bail
    // out into. Suppose frame_ is an inline frame, executing a copy of its
    // JSScript, S_inner, that has been inlined into the IonScript of some
    // other JSScript, S_outer. We must match S_outer, to decide which Ion
    // frame to invalidate; and we must match S_inner, to decide which
    // Baseline script to recompile.
    //
    // Note that this does not, by design, invalidate *all* inliners of
    // frame_.script(), as only frame_ is made observable, not
    // frame_.script().
    if (!script->hasBaselineScript()) {
      return false;
    }

    if (frame_.hasScript() && script == frame_.script()) {
      return true;
    }

    return frame_.isRematerializedFrame() &&
           script == frame_.asRematerializedFrame()->outerScript();
  }

  bool shouldMarkAsDebuggee(FrameIter& iter) const override {
    // AbstractFramePtr can't refer to non-remateralized Ion frames or
    // non-debuggee wasm frames, so if iter refers to one such, we know we
    // don't match.
    //
    // We never use this 'has' overload for frame invalidation, only for
    // frame debuggee marking; so this overload doesn't need a parallel to
    // the just-so inlining logic above.
    return iter.hasUsableAbstractFramePtr() &&
           iter.abstractFramePtr() == frame_;
  }
};

class MOZ_RAII ExecutionObservableScript
    : public DebugAPI::ExecutionObservableSet {
  RootedScript script_;

 public:
  ExecutionObservableScript(JSContext* cx, JSScript* script)
      : script_(cx, script) {}

  Zone* singleZone() const override { return script_->zone(); }
  JSScript* singleScriptForZoneInvalidation() const override { return script_; }
  bool shouldRecompileOrInvalidate(JSScript* script) const override {
    return script->hasBaselineScript() && script == script_;
  }
  bool shouldMarkAsDebuggee(FrameIter& iter) const override {
    // AbstractFramePtr can't refer to non-remateralized Ion frames, and
    // while a non-rematerialized Ion frame may indeed be running script_,
    // we cannot mark them as debuggees until they bail out.
    //
    // Upon bailing out, any newly constructed Baseline frames that came
    // from Ion frames with scripts that are isDebuggee() is marked as
    // debuggee. This is correct in that the only other way a frame may be
    // marked as debuggee is via Debugger.Frame reflection, which would
    // have rematerialized any Ion frames.
    //
    // Also AbstractFramePtr can't refer to non-debuggee wasm frames, so if
    // iter refers to one such, we know we don't match.
    return iter.hasUsableAbstractFramePtr() && !iter.isWasm() &&
           iter.abstractFramePtr().script() == script_;
  }
};

/* static */
bool Debugger::updateExecutionObservabilityOfFrames(
    JSContext* cx, const DebugAPI::ExecutionObservableSet& obs,
    IsObserving observing) {
  AutoSuppressProfilerSampling suppressProfilerSampling(cx);

  {
    jit::JitContext jctx(cx, nullptr);
    if (!jit::RecompileOnStackBaselineScriptsForDebugMode(cx, obs, observing)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  AbstractFramePtr oldestEnabledFrame;
  for (AllFramesIter iter(cx); !iter.done(); ++iter) {
    if (obs.shouldMarkAsDebuggee(iter)) {
      if (observing) {
        if (!iter.abstractFramePtr().isDebuggee()) {
          oldestEnabledFrame = iter.abstractFramePtr();
          oldestEnabledFrame.setIsDebuggee();
        }
        if (iter.abstractFramePtr().isWasmDebugFrame()) {
          iter.abstractFramePtr().asWasmDebugFrame()->observe(cx);
        }
      } else {
#ifdef DEBUG
        // Debugger.Frame lifetimes are managed by the debug epilogue,
        // so in general it's unsafe to unmark a frame if it has a
        // Debugger.Frame associated with it.
        MOZ_ASSERT(!DebugAPI::inFrameMaps(iter.abstractFramePtr()));
#endif
        iter.abstractFramePtr().unsetIsDebuggee();
      }
    }
  }

  // See comment in unsetPrevUpToDateUntil.
  if (oldestEnabledFrame) {
    AutoRealm ar(cx, oldestEnabledFrame.environmentChain());
    DebugEnvironments::unsetPrevUpToDateUntil(cx, oldestEnabledFrame);
  }

  return true;
}

static inline void MarkJitScriptActiveIfObservable(
    JSScript* script, const DebugAPI::ExecutionObservableSet& obs) {
  if (obs.shouldRecompileOrInvalidate(script)) {
    script->jitScript()->setActive();
  }
}

static bool AppendAndInvalidateScript(JSContext* cx, Zone* zone,
                                      JSScript* script,
                                      jit::RecompileInfoVector& invalid,
                                      Vector<JSScript*>& scripts) {
  // Enter the script's realm as AddPendingInvalidation attempts to
  // cancel off-thread compilations, whose books are kept on the
  // script's realm.
  MOZ_ASSERT(script->zone() == zone);
  AutoRealm ar(cx, script);
  AddPendingInvalidation(invalid, script);
  return scripts.append(script);
}

static bool UpdateExecutionObservabilityOfScriptsInZone(
    JSContext* cx, Zone* zone, const DebugAPI::ExecutionObservableSet& obs,
    Debugger::IsObserving observing) {
  using namespace js::jit;

  AutoSuppressProfilerSampling suppressProfilerSampling(cx);

  JSFreeOp* fop = cx->runtime()->defaultFreeOp();

  Vector<JSScript*> scripts(cx);

  // Iterate through observable scripts, invalidating their Ion scripts and
  // appending them to a vector for discarding their baseline scripts later.
  {
    RecompileInfoVector invalid;
    if (JSScript* script = obs.singleScriptForZoneInvalidation()) {
      if (obs.shouldRecompileOrInvalidate(script)) {
        if (!AppendAndInvalidateScript(cx, zone, script, invalid, scripts)) {
          return false;
        }
      }
    } else {
      for (auto base = zone->cellIter<BaseScript>(); !base.done();
           base.next()) {
        if (!base->hasJitScript()) {
          continue;
        }
        JSScript* script = base->asJSScript();
        if (obs.shouldRecompileOrInvalidate(script)) {
          if (!AppendAndInvalidateScript(cx, zone, script, invalid, scripts)) {
            return false;
          }
        }
      }
    }
    Invalidate(cx, invalid);
  }

  // Code below this point must be infallible to ensure the active bit of
  // BaselineScripts is in a consistent state.
  //
  // Mark active baseline scripts in the observable set so that they don't
  // get discarded. They will be recompiled.
  for (JitActivationIterator actIter(cx); !actIter.done(); ++actIter) {
    if (actIter->compartment()->zone() != zone) {
      continue;
    }

    for (OnlyJSJitFrameIter iter(actIter); !iter.done(); ++iter) {
      const JSJitFrameIter& frame = iter.frame();
      switch (frame.type()) {
        case FrameType::BaselineJS:
          MarkJitScriptActiveIfObservable(frame.script(), obs);
          break;
        case FrameType::IonJS:
          MarkJitScriptActiveIfObservable(frame.script(), obs);
          for (InlineFrameIterator inlineIter(cx, &frame); inlineIter.more();
               ++inlineIter) {
            MarkJitScriptActiveIfObservable(inlineIter.script(), obs);
          }
          break;
        default:;
      }
    }
  }

  // Iterate through the scripts again and finish discarding
  // BaselineScripts. This must be done as a separate phase as we can only
  // discard the BaselineScript on scripts that have no IonScript.
  for (size_t i = 0; i < scripts.length(); i++) {
    MOZ_ASSERT_IF(scripts[i]->isDebuggee(), observing);
    if (!scripts[i]->jitScript()->active()) {
      FinishDiscardBaselineScript(fop, scripts[i]);
    }
    scripts[i]->jitScript()->resetActive();
  }

  // Iterate through all wasm instances to find ones that need to be updated.
  for (RealmsInZoneIter r(zone); !r.done(); r.next()) {
    for (wasm::Instance* instance : r->wasm.instances()) {
      if (!instance->debugEnabled()) {
        continue;
      }

      bool enableTrap = observing == Debugger::Observing;
      instance->debug().ensureEnterFrameTrapsState(cx, enableTrap);
    }
  }

  return true;
}

/* static */
bool Debugger::updateExecutionObservabilityOfScripts(
    JSContext* cx, const DebugAPI::ExecutionObservableSet& obs,
    IsObserving observing) {
  if (Zone* zone = obs.singleZone()) {
    return UpdateExecutionObservabilityOfScriptsInZone(cx, zone, obs,
                                                       observing);
  }

  using ZoneRange = DebugAPI::ExecutionObservableSet::ZoneRange;
  for (ZoneRange r = obs.zones()->all(); !r.empty(); r.popFront()) {
    if (!UpdateExecutionObservabilityOfScriptsInZone(cx, r.front(), obs,
                                                     observing)) {
      return false;
    }
  }

  return true;
}

template <typename FrameFn>
/* static */
void Debugger::forEachOnStackDebuggerFrame(AbstractFramePtr frame, FrameFn fn) {
  for (Realm::DebuggerVectorEntry& entry : frame.global()->getDebuggers()) {
    Debugger* dbg = entry.dbg;
    if (FrameMap::Ptr frameEntry = dbg->frames.lookup(frame)) {
      fn(dbg, frameEntry->value());
    }
  }
}

template <typename FrameFn>
/* static */
void Debugger::forEachOnStackOrSuspendedDebuggerFrame(JSContext* cx,
                                                      AbstractFramePtr frame,
                                                      FrameFn fn) {
  Rooted<AbstractGeneratorObject*> genObj(
      cx, frame.isGeneratorFrame() ? GetGeneratorObjectForFrame(cx, frame)
                                   : nullptr);

  for (Realm::DebuggerVectorEntry& entry : frame.global()->getDebuggers()) {
    Debugger* dbg = entry.dbg;

    DebuggerFrame* frameObj = nullptr;
    if (FrameMap::Ptr frameEntry = dbg->frames.lookup(frame)) {
      frameObj = frameEntry->value();
    } else if (GeneratorWeakMap::Ptr frameEntry =
                   dbg->generatorFrames.lookup(genObj)) {
      frameObj = frameEntry->value();
    }

    if (frameObj) {
      fn(dbg, frameObj);
    }
  }
}

/* static */
bool Debugger::getDebuggerFrames(AbstractFramePtr frame,
                                 MutableHandle<DebuggerFrameVector> frames) {
  bool hadOOM = false;
  forEachOnStackDebuggerFrame(frame, [&](Debugger*, DebuggerFrame* frameobj) {
    if (!hadOOM && !frames.append(frameobj)) {
      hadOOM = true;
    }
  });
  return !hadOOM;
}

/* static */
bool Debugger::updateExecutionObservability(
    JSContext* cx, DebugAPI::ExecutionObservableSet& obs,
    IsObserving observing) {
  if (!obs.singleZone() && obs.zones()->empty()) {
    return true;
  }

  // Invalidate scripts first so we can set the needsArgsObj flag on scripts
  // before patching frames.
  return updateExecutionObservabilityOfScripts(cx, obs, observing) &&
         updateExecutionObservabilityOfFrames(cx, obs, observing);
}

/* static */
bool Debugger::ensureExecutionObservabilityOfScript(JSContext* cx,
                                                    JSScript* script) {
  if (script->isDebuggee()) {
    return true;
  }
  ExecutionObservableScript obs(cx, script);
  return updateExecutionObservability(cx, obs, Observing);
}

/* static */
bool DebugAPI::ensureExecutionObservabilityOfOsrFrame(
    JSContext* cx, AbstractFramePtr osrSourceFrame) {
  MOZ_ASSERT(osrSourceFrame.isDebuggee());
  if (osrSourceFrame.script()->hasBaselineScript() &&
      osrSourceFrame.script()->baselineScript()->hasDebugInstrumentation()) {
    return true;
  }
  ExecutionObservableFrame obs(osrSourceFrame);
  return Debugger::updateExecutionObservabilityOfFrames(cx, obs, Observing);
}

/* static */
bool Debugger::ensureExecutionObservabilityOfFrame(JSContext* cx,
                                                   AbstractFramePtr frame) {
  MOZ_ASSERT_IF(frame.hasScript() && frame.script()->isDebuggee(),
                frame.isDebuggee());
  MOZ_ASSERT_IF(frame.isWasmDebugFrame(), frame.wasmInstance()->debugEnabled());
  if (frame.isDebuggee()) {
    return true;
  }
  ExecutionObservableFrame obs(frame);
  return updateExecutionObservabilityOfFrames(cx, obs, Observing);
}

/* static */
bool Debugger::ensureExecutionObservabilityOfRealm(JSContext* cx,
                                                   Realm* realm) {
  if (realm->debuggerObservesAllExecution()) {
    return true;
  }
  ExecutionObservableRealms obs(cx);
  if (!obs.add(realm)) {
    return false;
  }
  realm->updateDebuggerObservesAllExecution();
  return updateExecutionObservability(cx, obs, Observing);
}

/* static */
bool Debugger::hookObservesAllExecution(Hook which) {
  return which == OnEnterFrame;
}

Debugger::IsObserving Debugger::observesAllExecution() const {
  if (!!getHook(OnEnterFrame)) {
    return Observing;
  }
  return NotObserving;
}

Debugger::IsObserving Debugger::observesAsmJS() const {
  if (!allowUnobservedAsmJS) {
    return Observing;
  }
  return NotObserving;
}

Debugger::IsObserving Debugger::observesCoverage() const {
  if (collectCoverageInfo) {
    return Observing;
  }
  return NotObserving;
}

Debugger::IsObserving Debugger::observesNativeCalls() const {
  if (getHook(Debugger::OnNativeCall)) {
    return Observing;
  }
  return NotObserving;
}

// Toggle whether this Debugger's debuggees observe all execution. This is
// called when a hook that observes all execution is set or unset. See
// hookObservesAllExecution.
bool Debugger::updateObservesAllExecutionOnDebuggees(JSContext* cx,
                                                     IsObserving observing) {
  ExecutionObservableRealms obs(cx);

  for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty();
       r.popFront()) {
    GlobalObject* global = r.front();
    JS::Realm* realm = global->realm();

    if (realm->debuggerObservesAllExecution() == observing) {
      continue;
    }

    // It's expensive to eagerly invalidate and recompile a realm,
    // so add the realm to the set only if we are observing.
    if (observing && !obs.add(realm)) {
      return false;
    }
  }

  if (!updateExecutionObservability(cx, obs, observing)) {
    return false;
  }

  using RealmRange = ExecutionObservableRealms::RealmRange;
  for (RealmRange r = obs.realms()->all(); !r.empty(); r.popFront()) {
    r.front()->updateDebuggerObservesAllExecution();
  }

  return true;
}

bool Debugger::updateObservesCoverageOnDebuggees(JSContext* cx,
                                                 IsObserving observing) {
  ExecutionObservableRealms obs(cx);

  for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty();
       r.popFront()) {
    GlobalObject* global = r.front();
    Realm* realm = global->realm();

    if (realm->debuggerObservesCoverage() == observing) {
      continue;
    }

    // Invalidate and recompile a realm to add or remove PCCounts
    // increments. We have to eagerly invalidate, as otherwise we might have
    // dangling pointers to freed PCCounts.
    if (!obs.add(realm)) {
      return false;
    }
  }

  // If any frame on the stack belongs to the debuggee, then we cannot update
  // the ScriptCounts, because this would imply to invalidate a Debugger.Frame
  // to recompile it with/without ScriptCount support.
  for (FrameIter iter(cx); !iter.done(); ++iter) {
    if (obs.shouldMarkAsDebuggee(iter)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_NOT_IDLE);
      return false;
    }
  }

  if (!updateExecutionObservability(cx, obs, observing)) {
    return false;
  }

  // All realms can safely be toggled, and all scripts will be recompiled.
  // Thus we can update each realm accordingly.
  using RealmRange = ExecutionObservableRealms::RealmRange;
  for (RealmRange r = obs.realms()->all(); !r.empty(); r.popFront()) {
    r.front()->updateDebuggerObservesCoverage();
  }

  return true;
}

void Debugger::updateObservesAsmJSOnDebuggees(IsObserving observing) {
  for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty();
       r.popFront()) {
    GlobalObject* global = r.front();
    Realm* realm = global->realm();

    if (realm->debuggerObservesAsmJS() == observing) {
      continue;
    }

    realm->updateDebuggerObservesAsmJS();
  }
}

/*** Allocations Tracking ***************************************************/

/* static */
bool Debugger::cannotTrackAllocations(const GlobalObject& global) {
  auto existingCallback = global.realm()->getAllocationMetadataBuilder();
  return existingCallback && existingCallback != &SavedStacks::metadataBuilder;
}

/* static */
bool DebugAPI::isObservedByDebuggerTrackingAllocations(
    const GlobalObject& debuggee) {
  for (Realm::DebuggerVectorEntry& entry : debuggee.getDebuggers()) {
    // Use unbarrieredGet() to prevent triggering read barrier while
    // collecting, this is safe as long as dbg does not escape.
    Debugger* dbg = entry.dbg.unbarrieredGet();
    if (dbg->trackingAllocationSites) {
      return true;
    }
  }

  return false;
}

/* static */
bool Debugger::addAllocationsTracking(JSContext* cx,
                                      Handle<GlobalObject*> debuggee) {
  // Precondition: the given global object is being observed by at least one
  // Debugger that is tracking allocations.
  MOZ_ASSERT(DebugAPI::isObservedByDebuggerTrackingAllocations(*debuggee));

  if (Debugger::cannotTrackAllocations(*debuggee)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_METADATA_CALLBACK_ALREADY_SET);
    return false;
  }

  debuggee->realm()->setAllocationMetadataBuilder(
      &SavedStacks::metadataBuilder);
  debuggee->realm()->chooseAllocationSamplingProbability();
  return true;
}

/* static */
void Debugger::removeAllocationsTracking(GlobalObject& global) {
  // If there are still Debuggers that are observing allocations, we cannot
  // remove the metadata callback yet. Recompute the sampling probability
  // based on the remaining debuggers' needs.
  if (DebugAPI::isObservedByDebuggerTrackingAllocations(global)) {
    global.realm()->chooseAllocationSamplingProbability();
    return;
  }

  if (!global.realm()->runtimeFromMainThread()->recordAllocationCallback) {
    // Something like the Gecko Profiler could request from the the JS runtime
    // to record allocations. If it is recording allocations, then do not
    // destroy the allocation metadata builder at this time.
    global.realm()->forgetAllocationMetadataBuilder();
  }
}

bool Debugger::addAllocationsTrackingForAllDebuggees(JSContext* cx) {
  MOZ_ASSERT(trackingAllocationSites);

  // We don't want to end up in a state where we added allocations
  // tracking to some of our debuggees, but failed to do so for
  // others. Before attempting to start tracking allocations in *any* of
  // our debuggees, ensure that we will be able to track allocations for
  // *all* of our debuggees.
  for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty();
       r.popFront()) {
    if (Debugger::cannotTrackAllocations(*r.front().get())) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_OBJECT_METADATA_CALLBACK_ALREADY_SET);
      return false;
    }
  }

  Rooted<GlobalObject*> g(cx);
  for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty();
       r.popFront()) {
    // This should always succeed, since we already checked for the
    // error case above.
    g = r.front().get();
    MOZ_ALWAYS_TRUE(Debugger::addAllocationsTracking(cx, g));
  }

  return true;
}

void Debugger::removeAllocationsTrackingForAllDebuggees() {
  for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty();
       r.popFront()) {
    Debugger::removeAllocationsTracking(*r.front().get());
  }

  allocationsLog.clear();
}

/*** Debugger JSObjects *****************************************************/

template <typename F>
inline void Debugger::forEachWeakMap(const F& f) {
  f(generatorFrames);
  f(objects);
  f(environments);
  f(scripts);
  f(sources);
  f(wasmInstanceScripts);
  f(wasmInstanceSources);
}

void Debugger::traceCrossCompartmentEdges(JSTracer* trc) {
  forEachWeakMap(
      [trc](auto& weakMap) { weakMap.traceCrossCompartmentEdges(trc); });
}

/*
 * Ordinarily, WeakMap keys and values are marked because at some point it was
 * discovered that the WeakMap was live; that is, some object containing the
 * WeakMap was marked during mark phase.
 *
 * However, during zone GC, we have to do something about cross-compartment
 * edges in non-GC'd compartments. Since the source may be live, we
 * conservatively assume it is and mark the edge.
 *
 * Each Debugger object keeps five cross-compartment WeakMaps: objects, scripts,
 * lazy scripts, script source objects, and environments. They have the property
 * that all their values are in the same compartment as the Debugger object,
 * but we have to mark the keys and the private pointer in the wrapper object.
 *
 * We must scan all Debugger objects regardless of whether they *currently* have
 * any debuggees in a compartment being GC'd, because the WeakMap entries
 * persist even when debuggees are removed.
 *
 * This happens during the initial mark phase, not iterative marking, because
 * all the edges being reported here are strong references.
 *
 * This method is also used during compacting GC to update cross compartment
 * pointers into zones that are being compacted.
 */
/* static */
void DebugAPI::traceCrossCompartmentEdges(JSTracer* trc) {
  MOZ_ASSERT(JS::RuntimeHeapIsMajorCollecting());

  JSRuntime* rt = trc->runtime();
  gc::State state = rt->gc.state();

  for (Debugger* dbg : rt->debuggerList()) {
    Zone* zone = MaybeForwarded(dbg->object.get())->zone();
    if (!zone->isCollecting() || state == gc::State::Compact) {
      dbg->traceCrossCompartmentEdges(trc);
    }
  }
}

#ifdef DEBUG

static bool RuntimeHasDebugger(JSRuntime* rt, Debugger* dbg) {
  for (Debugger* d : rt->debuggerList()) {
    if (d == dbg) {
      return true;
    }
  }
  return false;
}

/* static */
bool DebugAPI::edgeIsInDebuggerWeakmap(JSRuntime* rt, JSObject* src,
                                       JS::GCCellPtr dst) {
  if (!Debugger::isChildJSObject(src)) {
    return false;
  }

  if (src->is<DebuggerFrame>()) {
    DebuggerFrame* frame = &src->as<DebuggerFrame>();
    Debugger* dbg = frame->owner();
    MOZ_ASSERT(RuntimeHasDebugger(rt, dbg));

    if (dst.is<BaseScript>()) {
      // The generatorFrames map is not keyed on the associated JSScript. Get
      // the key from the source object and check everything matches.
      AbstractGeneratorObject* genObj = &frame->unwrappedGenerator();
      return frame->generatorScript() == &dst.as<BaseScript>() &&
             dbg->generatorFrames.hasEntry(genObj, src);
    }
    return dst.is<JSObject>() &&
           dst.as<JSObject>().is<AbstractGeneratorObject>() &&
           dbg->generatorFrames.hasEntry(
               &dst.as<JSObject>().as<AbstractGeneratorObject>(), src);
  }
  if (src->is<DebuggerObject>()) {
    Debugger* dbg = src->as<DebuggerObject>().owner();
    MOZ_ASSERT(RuntimeHasDebugger(rt, dbg));
    return dst.is<JSObject>() &&
           dbg->objects.hasEntry(&dst.as<JSObject>(), src);
  }
  if (src->is<DebuggerEnvironment>()) {
    Debugger* dbg = src->as<DebuggerEnvironment>().owner();
    MOZ_ASSERT(RuntimeHasDebugger(rt, dbg));
    return dst.is<JSObject>() &&
           dbg->environments.hasEntry(&dst.as<JSObject>(), src);
  }
  if (src->is<DebuggerScript>()) {
    Debugger* dbg = src->as<DebuggerScript>().owner();
    MOZ_ASSERT(RuntimeHasDebugger(rt, dbg));

    return src->as<DebuggerScript>().getReferent().match(
        [=](BaseScript* script) {
          return dst.is<BaseScript>() && script == &dst.as<BaseScript>() &&
                 dbg->scripts.hasEntry(script, src);
        },
        [=](WasmInstanceObject* instance) {
          return dst.is<JSObject>() && instance == &dst.as<JSObject>() &&
                 dbg->wasmInstanceScripts.hasEntry(instance, src);
        });
  }
  if (src->is<DebuggerSource>()) {
    Debugger* dbg = src->as<DebuggerSource>().owner();
    MOZ_ASSERT(RuntimeHasDebugger(rt, dbg));

    return src->as<DebuggerSource>().getReferent().match(
        [=](ScriptSourceObject* sso) {
          return dst.is<JSObject>() && sso == &dst.as<JSObject>() &&
                 dbg->sources.hasEntry(sso, src);
        },
        [=](WasmInstanceObject* instance) {
          return dst.is<JSObject>() && instance == &dst.as<JSObject>() &&
                 dbg->wasmInstanceSources.hasEntry(instance, src);
        });
  }
  MOZ_ASSERT_UNREACHABLE("Unhandled cross-compartment edge");
}

#endif

/* See comments in DebugAPI.h. */
void DebugAPI::traceFramesWithLiveHooks(JSTracer* tracer) {
  JSRuntime* rt = tracer->runtime();

  // Note that we must loop over all Debuggers here, not just those known to be
  // reachable from JavaScript. The existence of hooks set on a Debugger.Frame
  // for a live stack frame makes the Debuger.Frame (and hence its Debugger)
  // reachable.
  for (Debugger* dbg : rt->debuggerList()) {
    // Callback tracers set their own traversal boundaries, but otherwise we're
    // only interested in Debugger.Frames participating in the collection.
    if (!dbg->zone()->isGCMarking() && !tracer->isCallbackTracer()) {
      continue;
    }

    for (Debugger::FrameMap::Range r = dbg->frames.all(); !r.empty();
         r.popFront()) {
      HeapPtr<DebuggerFrame*>& frameobj = r.front().value();
      MOZ_ASSERT(frameobj->isOnStackMaybeForwarded());
      if (frameobj->hasAnyHooks()) {
        TraceEdge(tracer, &frameobj, "Debugger.Frame with live hooks");
      }
    }
  }
}

void DebugAPI::slowPathTraceGeneratorFrame(JSTracer* tracer,
                                           AbstractGeneratorObject* generator) {
  MOZ_ASSERT(generator->realm()->isDebuggee());

  // Ignore generic tracers.
  //
  // There are two kinds of generic tracers we need to bar: MovingTracers used
  // by compacting GC; and CompartmentCheckTracers.
  //
  // MovingTracers are used by the compacting GC to update pointers to objects
  // that have been moved: the MovingTracer checks each outgoing pointer to see
  // if it refers to a forwarding pointer, and if so, updates the pointer stored
  // in the object.
  //
  // Generator objects are background finalized, so the compacting GC assumes it
  // can update their pointers in the background as well. Since we treat
  // generator objects as having an owning edge to their Debugger.Frame objects,
  // a helper thread trying to update a generator object will end up calling
  // this function. However, it is verboten to do weak map lookups (e.g., in
  // Debugger::generatorFrames) off the main thread, since MovableCellHasher
  // must consult the Zone to find the key's unique id.
  //
  // Fortunately, it's not necessary for compacting GC to worry about that edge
  // in the first place: the edge isn't a literal pointer stored on the
  // generator object, it's only inferred from the realm's debuggee status and
  // its Debuggers' generatorFrames weak maps. Those get relocated when the
  // Debugger itself is visited, so compacting GC can just ignore this edge.
  //
  // CompartmentCheckTracers walk the graph and verify that all
  // cross-compartment edges are recorded in the cross-compartment wrapper
  // tables. But edges between Debugger.Foo objects and their referents are not
  // in the CCW tables, so a CrossCompartmentCheckTracers also calls
  // DebugAPI::edgeIsInDebuggerWeakmap to see if a given cross-compartment edge
  // is accounted for there. However, edgeIsInDebuggerWeakmap only handles
  // debugger -> debuggee edges, so it won't recognize the edge we're
  // potentially traversing here, from a generator object to its Debugger.Frame.
  //
  // But since the purpose of this function is to retrieve such edges, if they
  // exist, from the very tables that edgeIsInDebuggerWeakmap would consult,
  // we're at no risk of reporting edges that they do not cover. So we can
  // safely hide the edges from CompartmentCheckTracers.
  //
  // We can't quite recognize MovingTracers and CompartmentCheckTracers
  // precisely, but they're both generic tracers, so we just show them all the
  // door. This means the generator -> Debugger.Frame edge is going to be
  // invisible to some traversals. We'll cope with that when it's a problem.
  if (tracer->isGenericTracer()) {
    return;
  }

  for (Realm::DebuggerVectorEntry& entry : generator->realm()->getDebuggers()) {
    Debugger* dbg = entry.dbg.unbarrieredGet();

    if (Debugger::GeneratorWeakMap::Ptr entry =
            dbg->generatorFrames.lookupUnbarriered(generator)) {
      HeapPtr<DebuggerFrame*>& frameObj = entry->value();
      if (frameObj->hasAnyHooks()) {
        // See comment above.
        TraceCrossCompartmentEdge(tracer, generator, &frameObj,
                                  "Debugger.Frame with hooks for generator");
      }
    }
  }
}

/* static */
void DebugAPI::traceAllForMovingGC(JSTracer* trc) {
  JSRuntime* rt = trc->runtime();
  for (Debugger* dbg : rt->debuggerList()) {
    dbg->traceForMovingGC(trc);
  }
}

/*
 * Trace all debugger-owned GC things unconditionally. This is used during
 * compacting GC and in minor GC: the minor GC cannot apply the weak constraints
 * of the full GC because it visits only part of the heap.
 */
void Debugger::traceForMovingGC(JSTracer* trc) {
  trace(trc);

  for (WeakGlobalObjectSet::Enum e(debuggees); !e.empty(); e.popFront()) {
    TraceEdge(trc, &e.mutableFront(), "Global Object");
  }
}

/* static */
void Debugger::traceObject(JSTracer* trc, JSObject* obj) {
  if (Debugger* dbg = Debugger::fromJSObject(obj)) {
    dbg->trace(trc);
  }
}

void Debugger::trace(JSTracer* trc) {
  TraceEdge(trc, &object, "Debugger Object");

  TraceNullableEdge(trc, &uncaughtExceptionHook, "hooks");

  // Mark Debugger.Frame objects. Since the Debugger is reachable, JS could call
  // getNewestFrame and then walk the stack, so these are all reachable from JS.
  //
  // Note that if a Debugger.Frame has hooks set, it must be retained even if
  // its Debugger is unreachable, since JS could observe that its hooks did not
  // fire. That case is handled by DebugAPI::traceFrames.
  //
  // (We have weakly-referenced Debugger.Frame objects as well, for suspended
  // generator frames; these are traced via generatorFrames just below.)
  for (FrameMap::Range r = frames.all(); !r.empty(); r.popFront()) {
    HeapPtr<DebuggerFrame*>& frameobj = r.front().value();
    TraceEdge(trc, &frameobj, "live Debugger.Frame");
    MOZ_ASSERT(frameobj->isOnStackMaybeForwarded());
  }

  allocationsLog.trace(trc);

  forEachWeakMap([trc](auto& weakMap) { weakMap.trace(trc); });
}

/* static */
void DebugAPI::traceFromRealm(JSTracer* trc, Realm* realm) {
  for (Realm::DebuggerVectorEntry& entry : realm->getDebuggers()) {
    TraceEdge(trc, &entry.debuggerLink, "realm debugger");
  }
}

/* static */
void DebugAPI::sweepAll(JSFreeOp* fop) {
  JSRuntime* rt = fop->runtime();

  Debugger* next;
  for (Debugger* dbg = rt->debuggerList().getFirst(); dbg; dbg = next) {
    next = dbg->getNext();

    // Debugger.Frames for generator calls bump the JSScript's
    // generatorObserverCount, so the JIT will instrument the code to notify
    // Debugger when the generator is resumed. When a Debugger.Frame gets GC'd,
    // generatorObserverCount needs to be decremented. It's much easier to do
    // this when we know that all parties involved - the Debugger.Frame, the
    // generator object, and the JSScript - have not yet been finalized.
    //
    // Since DebugAPI::sweepAll is called after everything is marked, but before
    // anything has been finalized, this is the perfect place to drop the count.
    if (dbg->zone()->isGCSweeping()) {
      for (Debugger::GeneratorWeakMap::Enum e(dbg->generatorFrames); !e.empty();
           e.popFront()) {
        DebuggerFrame* frameObj = e.front().value();
        if (IsAboutToBeFinalizedUnbarriered(&frameObj)) {
          // If the DebuggerFrame is being finalized, that means either:
          //  1) It is not present in "frames".
          //  2) The Debugger itself is also being finalized.
          //
          // In the first case, passing the frame is not necessary because there
          // isn't a frame entry to clear, and in the second case,
          // removeDebuggeeGlobal below will iterate and remove the entries
          // anyway, so things will be cleaned up properly.
          Debugger::terminateDebuggerFrame(fop, dbg, frameObj, NullFramePtr(),
                                           nullptr, &e);
        }
      }
    }

    // Detach dying debuggers and debuggees from each other. Since this
    // requires access to both objects it must be done before either
    // object is finalized.
    bool debuggerDying = IsAboutToBeFinalized(&dbg->object);
    for (WeakGlobalObjectSet::Enum e(dbg->debuggees); !e.empty();
         e.popFront()) {
      GlobalObject* global = e.front().unbarrieredGet();
      if (debuggerDying || IsAboutToBeFinalizedUnbarriered(&global)) {
        dbg->removeDebuggeeGlobal(fop, e.front().unbarrieredGet(), &e,
                                  Debugger::FromSweep::Yes);
      }
    }

    if (debuggerDying) {
      fop->delete_(dbg->object, dbg, MemoryUse::Debugger);
    }

    dbg = next;
  }
}

static inline bool SweepZonesInSameGroup(Zone* a, Zone* b) {
  // Ensure two zones are swept in the same sweep group by adding an edge
  // between them in each direction.
  return a->addSweepGroupEdgeTo(b) && b->addSweepGroupEdgeTo(a);
}

/* static */
bool DebugAPI::findSweepGroupEdges(JSRuntime* rt) {
  // Ensure that debuggers and their debuggees are finalized in the same group
  // by adding edges in both directions for debuggee zones. These are weak
  // references that are not in the cross compartment wrapper map.

  for (Debugger* dbg : rt->debuggerList()) {
    Zone* debuggerZone = dbg->object->zone();
    if (!debuggerZone->isGCMarking()) {
      continue;
    }

    for (auto e = dbg->debuggeeZones.all(); !e.empty(); e.popFront()) {
      Zone* debuggeeZone = e.front();
      if (!debuggeeZone->isGCMarking()) {
        continue;
      }

      if (!SweepZonesInSameGroup(debuggerZone, debuggeeZone)) {
        return false;
      }
    }
  }

  return true;
}

template <class UnbarrieredKey, class Wrapper, bool InvisibleKeysOk>
bool DebuggerWeakMap<UnbarrieredKey, Wrapper,
                     InvisibleKeysOk>::findSweepGroupEdges() {
  Zone* debuggerZone = zone();
  MOZ_ASSERT(debuggerZone->isGCMarking());
  for (Enum e(*this); !e.empty(); e.popFront()) {
    MOZ_ASSERT(e.front().value()->zone() == debuggerZone);

    Zone* keyZone = e.front().key()->zone();
    if (keyZone->isGCMarking() &&
        !SweepZonesInSameGroup(debuggerZone, keyZone)) {
      return false;
    }
  }

  // Add in edges for delegates, if relevant for the key type.
  return Base::findSweepGroupEdges();
}

const JSClassOps DebuggerInstanceObject::classOps_ = {
    nullptr,                // addProperty
    nullptr,                // delProperty
    nullptr,                // enumerate
    nullptr,                // newEnumerate
    nullptr,                // resolve
    nullptr,                // mayResolve
    nullptr,                // finalize
    nullptr,                // call
    nullptr,                // hasInstance
    nullptr,                // construct
    Debugger::traceObject,  // trace
};

const JSClass DebuggerInstanceObject::class_ = {
    "Debugger",
    JSCLASS_HAS_PRIVATE |
        JSCLASS_HAS_RESERVED_SLOTS(Debugger::JSSLOT_DEBUG_COUNT),
    &classOps_};

static Debugger* Debugger_fromThisValue(JSContext* cx, const CallArgs& args,
                                        const char* fnname) {
  JSObject* thisobj = RequireObject(cx, args.thisv());
  if (!thisobj) {
    return nullptr;
  }
  if (!thisobj->is<DebuggerInstanceObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "Debugger", fnname,
                              thisobj->getClass()->name);
    return nullptr;
  }

  // Forbid Debugger.prototype, which is of the Debugger JSClass but isn't
  // really a Debugger object. The prototype object is distinguished by
  // having a nullptr private value.
  Debugger* dbg = Debugger::fromJSObject(thisobj);
  if (!dbg) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "Debugger", fnname,
                              "prototype object");
  }
  return dbg;
}

struct MOZ_STACK_CLASS Debugger::CallData {
  JSContext* cx;
  const CallArgs& args;

  Debugger* dbg;

  CallData(JSContext* cx, const CallArgs& args, Debugger* dbg)
      : cx(cx), args(args), dbg(dbg) {}

  bool getOnDebuggerStatement();
  bool setOnDebuggerStatement();
  bool getOnExceptionUnwind();
  bool setOnExceptionUnwind();
  bool getOnNewScript();
  bool setOnNewScript();
  bool getOnEnterFrame();
  bool setOnEnterFrame();
  bool getOnNativeCall();
  bool setOnNativeCall();
  bool getOnNewGlobalObject();
  bool setOnNewGlobalObject();
  bool getOnNewPromise();
  bool setOnNewPromise();
  bool getOnPromiseSettled();
  bool setOnPromiseSettled();
  bool getUncaughtExceptionHook();
  bool setUncaughtExceptionHook();
  bool getAllowUnobservedAsmJS();
  bool setAllowUnobservedAsmJS();
  bool getCollectCoverageInfo();
  bool setCollectCoverageInfo();
  bool getMemory();
  bool addDebuggee();
  bool addAllGlobalsAsDebuggees();
  bool removeDebuggee();
  bool removeAllDebuggees();
  bool hasDebuggee();
  bool getDebuggees();
  bool getNewestFrame();
  bool clearAllBreakpoints();
  bool findScripts();
  bool findSources();
  bool findObjects();
  bool findAllGlobals();
  bool findSourceURLs();
  bool makeGlobalObjectReference();
  bool adoptDebuggeeValue();
  bool adoptFrame();
  bool adoptSource();

  using Method = bool (CallData::*)();

  template <Method MyMethod>
  static bool ToNative(JSContext* cx, unsigned argc, Value* vp);
};

template <Debugger::CallData::Method MyMethod>
/* static */
bool Debugger::CallData::ToNative(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Debugger* dbg = Debugger_fromThisValue(cx, args, "method");
  if (!dbg) {
    return false;
  }

  CallData data(cx, args, dbg);
  return (data.*MyMethod)();
}

/* static */
bool Debugger::getHookImpl(JSContext* cx, const CallArgs& args, Debugger& dbg,
                           Hook which) {
  MOZ_ASSERT(which >= 0 && which < HookCount);
  args.rval().set(dbg.object->getReservedSlot(JSSLOT_DEBUG_HOOK_START + which));
  return true;
}

/* static */
bool Debugger::setHookImpl(JSContext* cx, const CallArgs& args, Debugger& dbg,
                           Hook which) {
  MOZ_ASSERT(which >= 0 && which < HookCount);
  if (!args.requireAtLeast(cx, "Debugger.setHook", 1)) {
    return false;
  }
  if (args[0].isObject()) {
    if (!args[0].toObject().isCallable()) {
      return ReportIsNotFunction(cx, args[0], args.length() - 1);
    }
  } else if (!args[0].isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_CALLABLE_OR_UNDEFINED);
    return false;
  }
  uint32_t slot = JSSLOT_DEBUG_HOOK_START + which;
  RootedValue oldHook(cx, dbg.object->getReservedSlot(slot));
  dbg.object->setReservedSlot(slot, args[0]);
  if (hookObservesAllExecution(which)) {
    if (!dbg.updateObservesAllExecutionOnDebuggees(
            cx, dbg.observesAllExecution())) {
      dbg.object->setReservedSlot(slot, oldHook);
      return false;
    }
  }

  Rooted<DebuggerDebuggeeLink*> debuggeeLink(cx, dbg.getDebuggeeLink());
  if (dbg.hasAnyLiveHooks()) {
    debuggeeLink->setLinkSlot(dbg);
  } else {
    debuggeeLink->clearLinkSlot();
  }

  args.rval().setUndefined();
  return true;
}

/* static */
bool Debugger::getGarbageCollectionHook(JSContext* cx, const CallArgs& args,
                                        Debugger& dbg) {
  return getHookImpl(cx, args, dbg, OnGarbageCollection);
}

/* static */
bool Debugger::setGarbageCollectionHook(JSContext* cx, const CallArgs& args,
                                        Debugger& dbg) {
  Rooted<JSObject*> oldHook(cx, dbg.getHook(OnGarbageCollection));

  if (!setHookImpl(cx, args, dbg, OnGarbageCollection)) {
    // We want to maintain the invariant that the hook is always set when the
    // Debugger is in the runtime's list, and vice-versa, so if we return early
    // and don't adjust the watcher list below, we need to be sure that the
    // hook didn't change.
    MOZ_ASSERT(dbg.getHook(OnGarbageCollection) == oldHook);
    return false;
  }

  // Add or remove ourselves from the runtime's list of Debuggers that care
  // about garbage collection.
  JSObject* newHook = dbg.getHook(OnGarbageCollection);
  if (!oldHook && newHook) {
    cx->runtime()->onGarbageCollectionWatchers().pushBack(&dbg);
  } else if (oldHook && !newHook) {
    cx->runtime()->onGarbageCollectionWatchers().remove(&dbg);
  }

  return true;
}

bool Debugger::CallData::getOnDebuggerStatement() {
  return getHookImpl(cx, args, *dbg, OnDebuggerStatement);
}

bool Debugger::CallData::setOnDebuggerStatement() {
  return setHookImpl(cx, args, *dbg, OnDebuggerStatement);
}

bool Debugger::CallData::getOnExceptionUnwind() {
  return getHookImpl(cx, args, *dbg, OnExceptionUnwind);
}

bool Debugger::CallData::setOnExceptionUnwind() {
  return setHookImpl(cx, args, *dbg, OnExceptionUnwind);
}

bool Debugger::CallData::getOnNewScript() {
  return getHookImpl(cx, args, *dbg, OnNewScript);
}

bool Debugger::CallData::setOnNewScript() {
  return setHookImpl(cx, args, *dbg, OnNewScript);
}

bool Debugger::CallData::getOnNewPromise() {
  return getHookImpl(cx, args, *dbg, OnNewPromise);
}

bool Debugger::CallData::setOnNewPromise() {
  return setHookImpl(cx, args, *dbg, OnNewPromise);
}

bool Debugger::CallData::getOnPromiseSettled() {
  return getHookImpl(cx, args, *dbg, OnPromiseSettled);
}

bool Debugger::CallData::setOnPromiseSettled() {
  return setHookImpl(cx, args, *dbg, OnPromiseSettled);
}

bool Debugger::CallData::getOnEnterFrame() {
  return getHookImpl(cx, args, *dbg, OnEnterFrame);
}

bool Debugger::CallData::setOnEnterFrame() {
  return setHookImpl(cx, args, *dbg, OnEnterFrame);
}

bool Debugger::CallData::getOnNativeCall() {
  return getHookImpl(cx, args, *dbg, OnNativeCall);
}

bool Debugger::CallData::setOnNativeCall() {
  return setHookImpl(cx, args, *dbg, OnNativeCall);
}

bool Debugger::CallData::getOnNewGlobalObject() {
  return getHookImpl(cx, args, *dbg, OnNewGlobalObject);
}

bool Debugger::CallData::setOnNewGlobalObject() {
  RootedObject oldHook(cx, dbg->getHook(OnNewGlobalObject));

  if (!setHookImpl(cx, args, *dbg, OnNewGlobalObject)) {
    return false;
  }

  // Add or remove ourselves from the runtime's list of Debuggers that care
  // about new globals.
  JSObject* newHook = dbg->getHook(OnNewGlobalObject);
  if (!oldHook && newHook) {
    cx->runtime()->onNewGlobalObjectWatchers().pushBack(dbg);
  } else if (oldHook && !newHook) {
    cx->runtime()->onNewGlobalObjectWatchers().remove(dbg);
  }

  return true;
}

bool Debugger::CallData::getUncaughtExceptionHook() {
  args.rval().setObjectOrNull(dbg->uncaughtExceptionHook);
  return true;
}

bool Debugger::CallData::setUncaughtExceptionHook() {
  if (!args.requireAtLeast(cx, "Debugger.set uncaughtExceptionHook", 1)) {
    return false;
  }
  if (!args[0].isNull() &&
      (!args[0].isObject() || !args[0].toObject().isCallable())) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ASSIGN_FUNCTION_OR_NULL,
                              "uncaughtExceptionHook");
    return false;
  }
  dbg->uncaughtExceptionHook = args[0].toObjectOrNull();
  args.rval().setUndefined();
  return true;
}

bool Debugger::CallData::getAllowUnobservedAsmJS() {
  args.rval().setBoolean(dbg->allowUnobservedAsmJS);
  return true;
}

bool Debugger::CallData::setAllowUnobservedAsmJS() {
  if (!args.requireAtLeast(cx, "Debugger.set allowUnobservedAsmJS", 1)) {
    return false;
  }
  dbg->allowUnobservedAsmJS = ToBoolean(args[0]);

  for (WeakGlobalObjectSet::Range r = dbg->debuggees.all(); !r.empty();
       r.popFront()) {
    GlobalObject* global = r.front();
    Realm* realm = global->realm();
    realm->updateDebuggerObservesAsmJS();
  }

  args.rval().setUndefined();
  return true;
}

bool Debugger::CallData::getCollectCoverageInfo() {
  args.rval().setBoolean(dbg->collectCoverageInfo);
  return true;
}

bool Debugger::CallData::setCollectCoverageInfo() {
  if (!args.requireAtLeast(cx, "Debugger.set collectCoverageInfo", 1)) {
    return false;
  }
  dbg->collectCoverageInfo = ToBoolean(args[0]);

  IsObserving observing = dbg->collectCoverageInfo ? Observing : NotObserving;
  if (!dbg->updateObservesCoverageOnDebuggees(cx, observing)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool Debugger::CallData::getMemory() {
  Value memoryValue =
      dbg->object->getReservedSlot(JSSLOT_DEBUG_MEMORY_INSTANCE);

  if (!memoryValue.isObject()) {
    RootedObject memory(cx, DebuggerMemory::create(cx, dbg));
    if (!memory) {
      return false;
    }
    memoryValue = ObjectValue(*memory);
  }

  args.rval().set(memoryValue);
  return true;
}

/*
 * Given a value used to designate a global (there's quite a variety; see the
 * docs), return the actual designee.
 *
 * Note that this does not check whether the designee is marked "invisible to
 * Debugger" or not; different callers need to handle invisible-to-Debugger
 * globals in different ways.
 */
GlobalObject* Debugger::unwrapDebuggeeArgument(JSContext* cx, const Value& v) {
  if (!v.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_UNEXPECTED_TYPE, "argument",
                              "not a global object");
    return nullptr;
  }

  RootedObject obj(cx, &v.toObject());

  // If it's a Debugger.Object belonging to this debugger, dereference that.
  if (obj->getClass() == &DebuggerObject::class_) {
    RootedValue rv(cx, v);
    if (!unwrapDebuggeeValue(cx, &rv)) {
      return nullptr;
    }
    obj = &rv.toObject();
  }

  // If we have a cross-compartment wrapper, dereference as far as is secure.
  //
  // Since we're dealing with globals, we may have a WindowProxy here.  So we
  // have to make sure to do a dynamic unwrap, and we want to unwrap the
  // WindowProxy too, if we have one.
  obj = CheckedUnwrapDynamic(obj, cx, /* stopAtWindowProxy = */ false);
  if (!obj) {
    ReportAccessDenied(cx);
    return nullptr;
  }

  // If that didn't produce a global object, it's an error.
  if (!obj->is<GlobalObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_UNEXPECTED_TYPE, "argument",
                              "not a global object");
    return nullptr;
  }

  return &obj->as<GlobalObject>();
}

bool Debugger::CallData::addDebuggee() {
  if (!args.requireAtLeast(cx, "Debugger.addDebuggee", 1)) {
    return false;
  }
  Rooted<GlobalObject*> global(cx, dbg->unwrapDebuggeeArgument(cx, args[0]));
  if (!global) {
    return false;
  }

  if (!dbg->addDebuggeeGlobal(cx, global)) {
    return false;
  }

  RootedValue v(cx, ObjectValue(*global));
  if (!dbg->wrapDebuggeeValue(cx, &v)) {
    return false;
  }
  args.rval().set(v);
  return true;
}

bool Debugger::CallData::addAllGlobalsAsDebuggees() {
  for (CompartmentsIter comp(cx->runtime()); !comp.done(); comp.next()) {
    if (comp == dbg->object->compartment()) {
      continue;
    }
    for (RealmsInCompartmentIter r(comp); !r.done(); r.next()) {
      if (r->creationOptions().invisibleToDebugger()) {
        continue;
      }
      r->compartment()->gcState.scheduledForDestruction = false;
      GlobalObject* global = r->maybeGlobal();
      if (global) {
        Rooted<GlobalObject*> rg(cx, global);
        if (!dbg->addDebuggeeGlobal(cx, rg)) {
          return false;
        }
      }
    }
  }

  args.rval().setUndefined();
  return true;
}

bool Debugger::CallData::removeDebuggee() {
  if (!args.requireAtLeast(cx, "Debugger.removeDebuggee", 1)) {
    return false;
  }
  Rooted<GlobalObject*> global(cx, dbg->unwrapDebuggeeArgument(cx, args[0]));
  if (!global) {
    return false;
  }

  ExecutionObservableRealms obs(cx);

  if (dbg->debuggees.has(global)) {
    dbg->removeDebuggeeGlobal(cx->runtime()->defaultFreeOp(), global, nullptr,
                              FromSweep::No);

    // Only update the realm if there are no Debuggers left, as it's
    // expensive to check if no other Debugger has a live script or frame
    // hook on any of the current on-stack debuggee frames.
    if (global->getDebuggers().empty() && !obs.add(global->realm())) {
      return false;
    }
    if (!updateExecutionObservability(cx, obs, NotObserving)) {
      return false;
    }
  }

  args.rval().setUndefined();
  return true;
}

bool Debugger::CallData::removeAllDebuggees() {
  ExecutionObservableRealms obs(cx);

  for (WeakGlobalObjectSet::Enum e(dbg->debuggees); !e.empty(); e.popFront()) {
    Rooted<GlobalObject*> global(cx, e.front());
    dbg->removeDebuggeeGlobal(cx->runtime()->defaultFreeOp(), global, &e,
                              FromSweep::No);

    // See note about adding to the observable set in removeDebuggee.
    if (global->getDebuggers().empty() && !obs.add(global->realm())) {
      return false;
    }
  }

  if (!updateExecutionObservability(cx, obs, NotObserving)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool Debugger::CallData::hasDebuggee() {
  if (!args.requireAtLeast(cx, "Debugger.hasDebuggee", 1)) {
    return false;
  }
  GlobalObject* global = dbg->unwrapDebuggeeArgument(cx, args[0]);
  if (!global) {
    return false;
  }
  args.rval().setBoolean(!!dbg->debuggees.lookup(global));
  return true;
}

bool Debugger::CallData::getDebuggees() {
  // Obtain the list of debuggees before wrapping each debuggee, as a GC could
  // update the debuggees set while we are iterating it.
  unsigned count = dbg->debuggees.count();
  RootedValueVector debuggees(cx);
  if (!debuggees.resize(count)) {
    return false;
  }
  unsigned i = 0;
  {
    JS::AutoCheckCannotGC nogc;
    for (WeakGlobalObjectSet::Enum e(dbg->debuggees); !e.empty();
         e.popFront()) {
      debuggees[i++].setObject(*e.front().get());
    }
  }

  RootedArrayObject arrobj(cx, NewDenseFullyAllocatedArray(cx, count));
  if (!arrobj) {
    return false;
  }
  arrobj->ensureDenseInitializedLength(0, count);
  for (i = 0; i < count; i++) {
    RootedValue v(cx, debuggees[i]);
    if (!dbg->wrapDebuggeeValue(cx, &v)) {
      return false;
    }
    arrobj->setDenseElement(i, v);
  }

  args.rval().setObject(*arrobj);
  return true;
}

bool Debugger::CallData::getNewestFrame() {
  // Since there may be multiple contexts, use AllFramesIter.
  for (AllFramesIter i(cx); !i.done(); ++i) {
    if (dbg->observesFrame(i)) {
      // Ensure that Ion frames are rematerialized. Only rematerialized
      // Ion frames may be used as AbstractFramePtrs.
      if (i.isIon() && !i.ensureHasRematerializedFrame(cx)) {
        return false;
      }
      AbstractFramePtr frame = i.abstractFramePtr();
      FrameIter iter(i.activation()->cx());
      while (!iter.hasUsableAbstractFramePtr() ||
             iter.abstractFramePtr() != frame) {
        ++iter;
      }
      return dbg->getFrame(cx, iter, args.rval());
    }
  }
  args.rval().setNull();
  return true;
}

bool Debugger::CallData::clearAllBreakpoints() {
  JSFreeOp* fop = cx->defaultFreeOp();
  Breakpoint* nextbp;
  for (Breakpoint* bp = dbg->firstBreakpoint(); bp; bp = nextbp) {
    nextbp = bp->nextInDebugger();

    bp->remove(fop);
  }
  MOZ_ASSERT(!dbg->firstBreakpoint());

  return true;
}

/* static */
bool Debugger::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Check that the arguments, if any, are cross-compartment wrappers.
  for (unsigned i = 0; i < args.length(); i++) {
    JSObject* argobj = RequireObject(cx, args[i]);
    if (!argobj) {
      return false;
    }
    if (!argobj->is<CrossCompartmentWrapperObject>()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_CCW_REQUIRED, "Debugger");
      return false;
    }
  }

  // Get Debugger.prototype.
  RootedValue v(cx);
  RootedObject callee(cx, &args.callee());
  if (!GetProperty(cx, callee, callee, cx->names().prototype, &v)) {
    return false;
  }
  RootedNativeObject proto(cx, &v.toObject().as<NativeObject>());
  MOZ_ASSERT(proto->is<DebuggerInstanceObject>());

  // Make the new Debugger object. Each one has a reference to
  // Debugger.{Frame,Object,Script,Memory}.prototype in reserved slots. The
  // rest of the reserved slots are for hooks; they default to undefined.
  Rooted<DebuggerInstanceObject*> obj(
      cx, NewTenuredObjectWithGivenProto<DebuggerInstanceObject>(cx, proto));
  if (!obj) {
    return false;
  }
  for (unsigned slot = JSSLOT_DEBUG_PROTO_START; slot < JSSLOT_DEBUG_PROTO_STOP;
       slot++) {
    obj->setReservedSlot(slot, proto->getReservedSlot(slot));
  }
  obj->setReservedSlot(JSSLOT_DEBUG_MEMORY_INSTANCE, NullValue());

  RootedNativeObject livenessLink(
      cx, NewObjectWithGivenProto<DebuggerDebuggeeLink>(cx, nullptr));
  if (!livenessLink) {
    return false;
  }
  obj->setReservedSlot(JSSLOT_DEBUG_DEBUGGEE_LINK, ObjectValue(*livenessLink));

  Debugger* debugger;
  {
    // Construct the underlying C++ object.
    auto dbg = cx->make_unique<Debugger>(cx, obj.get());
    if (!dbg) {
      return false;
    }

    // The object owns the released pointer.
    debugger = dbg.release();
    InitObjectPrivate(obj, debugger, MemoryUse::Debugger);
  }

  // Add the initial debuggees, if any.
  for (unsigned i = 0; i < args.length(); i++) {
    JSObject& wrappedObj =
        args[i].toObject().as<ProxyObject>().private_().toObject();
    Rooted<GlobalObject*> debuggee(cx, &wrappedObj.nonCCWGlobal());
    if (!debugger->addDebuggeeGlobal(cx, debuggee)) {
      return false;
    }
  }

  args.rval().setObject(*obj);
  return true;
}

bool Debugger::addDebuggeeGlobal(JSContext* cx, Handle<GlobalObject*> global) {
  if (debuggees.has(global)) {
    return true;
  }

  // Callers should generally be unable to get a reference to a debugger-
  // invisible global in order to pass it to addDebuggee. But this is possible
  // with certain testing aides we expose in the shell, so just make addDebuggee
  // throw in that case.
  Realm* debuggeeRealm = global->realm();
  if (debuggeeRealm->creationOptions().invisibleToDebugger()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_CANT_DEBUG_GLOBAL);
    return false;
  }

  // Debugger and debuggee must be in different compartments.
  if (debuggeeRealm->compartment() == object->compartment()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_SAME_COMPARTMENT);
    return false;
  }

  // Check for cycles. If global's realm is reachable from this Debugger
  // object's realm by following debuggee-to-debugger links, then adding
  // global would create a cycle. (Typically nobody is debugging the
  // debugger, in which case we zip through this code without looping.)
  Vector<Realm*> visited(cx);
  if (!visited.append(object->realm())) {
    return false;
  }
  for (size_t i = 0; i < visited.length(); i++) {
    Realm* realm = visited[i];
    if (realm == debuggeeRealm) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_LOOP);
      return false;
    }

    // Find all realms containing debuggers debugging realm's global object.
    // Add those realms to visited.
    if (realm->isDebuggee()) {
      for (Realm::DebuggerVectorEntry& entry : realm->getDebuggers()) {
        Realm* next = entry.dbg->object->realm();
        if (std::find(visited.begin(), visited.end(), next) == visited.end()) {
          if (!visited.append(next)) {
            return false;
          }
        }
      }
    }
  }

  // For global to become this js::Debugger's debuggee:
  //
  // 1. this js::Debugger must be in global->getDebuggers(),
  // 2. global must be in this->debuggees,
  // 3. the debuggee's zone must be in this->debuggeeZones,
  // 4. if we are tracking allocations, the SavedStacksMetadataBuilder must be
  //    installed for this realm, and
  // 5. Realm::isDebuggee()'s bit must be set.
  //
  // All five indications must be kept consistent.

  AutoRealm ar(cx, global);
  Zone* zone = global->zone();

  RootedObject debuggeeLink(cx, getDebuggeeLink());
  if (!cx->compartment()->wrap(cx, &debuggeeLink)) {
    return false;
  }

  // (1)
  auto& globalDebuggers = global->getDebuggers();
  if (!globalDebuggers.append(Realm::DebuggerVectorEntry(this, debuggeeLink))) {
    ReportOutOfMemory(cx);
    return false;
  }
  auto globalDebuggersGuard = MakeScopeExit([&] { globalDebuggers.popBack(); });

  // (2)
  if (!debuggees.put(global)) {
    ReportOutOfMemory(cx);
    return false;
  }
  auto debuggeesGuard = MakeScopeExit([&] { debuggees.remove(global); });

  bool addingZoneRelation = !debuggeeZones.has(zone);

  // (3)
  if (addingZoneRelation && !debuggeeZones.put(zone)) {
    ReportOutOfMemory(cx);
    return false;
  }
  auto debuggeeZonesGuard = MakeScopeExit([&] {
    if (addingZoneRelation) {
      debuggeeZones.remove(zone);
    }
  });

  // (4)
  if (trackingAllocationSites &&
      !Debugger::addAllocationsTracking(cx, global)) {
    return false;
  }

  auto allocationsTrackingGuard = MakeScopeExit([&] {
    if (trackingAllocationSites) {
      Debugger::removeAllocationsTracking(*global);
    }
  });

  // (5)
  AutoRestoreRealmDebugMode debugModeGuard(debuggeeRealm);
  debuggeeRealm->setIsDebuggee();
  debuggeeRealm->updateDebuggerObservesAsmJS();
  debuggeeRealm->updateDebuggerObservesCoverage();
  if (observesAllExecution() &&
      !ensureExecutionObservabilityOfRealm(cx, debuggeeRealm)) {
    return false;
  }

  globalDebuggersGuard.release();
  debuggeesGuard.release();
  debuggeeZonesGuard.release();
  allocationsTrackingGuard.release();
  debugModeGuard.release();
  return true;
}

void Debugger::recomputeDebuggeeZoneSet() {
  AutoEnterOOMUnsafeRegion oomUnsafe;
  debuggeeZones.clear();
  for (auto range = debuggees.all(); !range.empty(); range.popFront()) {
    if (!debuggeeZones.put(range.front().unbarrieredGet()->zone())) {
      oomUnsafe.crash("Debugger::removeDebuggeeGlobal");
    }
  }
}

template <typename T, typename AP>
static T* findDebuggerInVector(Debugger* dbg, Vector<T, 0, AP>* vec) {
  T* p;
  for (p = vec->begin(); p != vec->end(); p++) {
    if (p->dbg == dbg) {
      break;
    }
  }
  MOZ_ASSERT(p != vec->end());
  return p;
}

void Debugger::removeDebuggeeGlobal(JSFreeOp* fop, GlobalObject* global,
                                    WeakGlobalObjectSet::Enum* debugEnum,
                                    FromSweep fromSweep) {
  // The caller might have found global by enumerating this->debuggees; if
  // so, use HashSet::Enum::removeFront rather than HashSet::remove below,
  // to avoid invalidating the live enumerator.
  MOZ_ASSERT(debuggees.has(global));
  MOZ_ASSERT(debuggeeZones.has(global->zone()));
  MOZ_ASSERT_IF(debugEnum, debugEnum->front().unbarrieredGet() == global);

  // Clear this global's generators from generatorFrames as well.
  //
  // This method can be called either from script (dbg.removeDebuggee) or during
  // GC sweeping, because the Debugger, debuggee global, or both are being GC'd.
  //
  // When called from script, it's okay to iterate over generatorFrames and
  // touch its keys and values (even when an incremental GC is in progress).
  // When called from GC, it's not okay; the keys and values may be dying. But
  // in that case, we can actually just skip the loop entirely! If the Debugger
  // is going away, it doesn't care about the state of its generatorFrames
  // table, and the Debugger.Frame finalizer will fix up the generator observer
  // counts.
  if (fromSweep == FromSweep::No) {
    for (GeneratorWeakMap::Enum e(generatorFrames); !e.empty(); e.popFront()) {
      AbstractGeneratorObject& genObj = *e.front().key();
      if (&genObj.global() == global) {
        terminateDebuggerFrame(fop, this, e.front().value(), NullFramePtr(),
                               nullptr, &e);
      }
    }
  }

  for (FrameMap::Enum e(frames); !e.empty(); e.popFront()) {
    AbstractFramePtr frame = e.front().key();
    if (frame.hasGlobal(global)) {
      terminateDebuggerFrame(fop, this, e.front().value(), frame, &e);
    }
  }

  auto& globalDebuggersVector = global->getDebuggers();

  // The relation must be removed from up to three places:
  // globalDebuggersVector and debuggees for sure, and possibly the
  // compartment's debuggee set.
  //
  // The debuggee zone set is recomputed on demand. This avoids refcounting
  // and in practice we have relatively few debuggees that tend to all be in
  // the same zone. If after recomputing the debuggee zone set, this global's
  // zone is not in the set, then we must remove ourselves from the zone's
  // vector of observing debuggers.
  globalDebuggersVector.erase(
      findDebuggerInVector(this, &globalDebuggersVector));

  if (debugEnum) {
    debugEnum->removeFront();
  } else {
    debuggees.remove(global);
  }

  recomputeDebuggeeZoneSet();

  // Remove all breakpoints for the debuggee.
  Breakpoint* nextbp;
  for (Breakpoint* bp = firstBreakpoint(); bp; bp = nextbp) {
    nextbp = bp->nextInDebugger();

    if (bp->site->realm() == global->realm()) {
      bp->remove(fop);
    }
  }
  MOZ_ASSERT_IF(debuggees.empty(), !firstBreakpoint());

  // If we are tracking allocation sites, we need to remove the object
  // metadata callback from this global's realm.
  if (trackingAllocationSites) {
    Debugger::removeAllocationsTracking(*global);
  }

  if (global->realm()->getDebuggers().empty()) {
    global->realm()->unsetIsDebuggee();
  } else {
    global->realm()->updateDebuggerObservesAllExecution();
    global->realm()->updateDebuggerObservesAsmJS();
    global->realm()->updateDebuggerObservesCoverage();
  }
}

class MOZ_STACK_CLASS Debugger::QueryBase {
 protected:
  QueryBase(JSContext* cx, Debugger* dbg)
      : cx(cx),
        debugger(dbg),
        iterMarker(&cx->runtime()->gc),
        realms(cx->zone()) {}

  // The context in which we should do our work.
  JSContext* cx;

  // The debugger for which we conduct queries.
  Debugger* debugger;

  // Require the set of realms to stay fixed while the query is alive.
  gc::AutoEnterIteration iterMarker;

  using RealmSet = HashSet<Realm*, DefaultHasher<Realm*>, ZoneAllocPolicy>;

  // A script must be in one of these realms to match the query.
  RealmSet realms;

  // Indicates whether OOM has occurred while matching.
  bool oom = false;

  bool addRealm(Realm* realm) { return realms.put(realm); }

  // Arrange for this query to match only scripts that run in |global|.
  bool matchSingleGlobal(GlobalObject* global) {
    MOZ_ASSERT(realms.count() == 0);
    if (!addRealm(global->realm())) {
      ReportOutOfMemory(cx);
      return false;
    }
    return true;
  }

  // Arrange for this ScriptQuery to match all scripts running in debuggee
  // globals.
  bool matchAllDebuggeeGlobals() {
    MOZ_ASSERT(realms.count() == 0);
    // Build our realm set from the debugger's set of debuggee globals.
    for (WeakGlobalObjectSet::Range r = debugger->debuggees.all(); !r.empty();
         r.popFront()) {
      if (!addRealm(r.front()->realm())) {
        ReportOutOfMemory(cx);
        return false;
      }
    }
    return true;
  }
};

/*
 * A class for parsing 'findScripts' query arguments and searching for
 * scripts that match the criteria they represent.
 */
class MOZ_STACK_CLASS Debugger::ScriptQuery : public Debugger::QueryBase {
 public:
  /* Construct a ScriptQuery to use matching scripts for |dbg|. */
  ScriptQuery(JSContext* cx, Debugger* dbg)
      : QueryBase(cx, dbg),
        url(cx),
        displayURLString(cx),
        source(cx, AsVariant(static_cast<ScriptSourceObject*>(nullptr))),
        scriptVector(cx, BaseScriptVector(cx)),
        partialMatchVector(cx, BaseScriptVector(cx)),
        wasmInstanceVector(cx, WasmInstanceObjectVector(cx)) {}

  /*
   * Parse the query object |query|, and prepare to match only the scripts
   * it specifies.
   */
  bool parseQuery(HandleObject query) {
    // Check for a 'global' property, which limits the results to those
    // scripts scoped to a particular global object.
    RootedValue global(cx);
    if (!GetProperty(cx, query, query, cx->names().global, &global)) {
      return false;
    }
    if (global.isUndefined()) {
      if (!matchAllDebuggeeGlobals()) {
        return false;
      }
    } else {
      GlobalObject* globalObject = debugger->unwrapDebuggeeArgument(cx, global);
      if (!globalObject) {
        return false;
      }

      // If the given global isn't a debuggee, just leave the set of
      // acceptable globals empty; we'll return no scripts.
      if (debugger->debuggees.has(globalObject)) {
        if (!matchSingleGlobal(globalObject)) {
          return false;
        }
      }
    }

    // Check for a 'url' property.
    if (!GetProperty(cx, query, query, cx->names().url, &url)) {
      return false;
    }
    if (!url.isUndefined() && !url.isString()) {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
          "query object's 'url' property", "neither undefined nor a string");
      return false;
    }

    // Check for a 'source' property
    RootedValue debuggerSource(cx);
    if (!GetProperty(cx, query, query, cx->names().source, &debuggerSource)) {
      return false;
    }
    if (!debuggerSource.isUndefined()) {
      if (!debuggerSource.isObject() ||
          !debuggerSource.toObject().is<DebuggerSource>()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_UNEXPECTED_TYPE,
                                  "query object's 'source' property",
                                  "not undefined nor a Debugger.Source object");
        return false;
      }

      DebuggerSource& debuggerSourceObj =
          debuggerSource.toObject().as<DebuggerSource>();

      // The given source must have an owner. Otherwise, it's a
      // Debugger.Source.prototype, which would match no scripts, and is
      // probably a mistake.
      if (!debuggerSourceObj.isInstance()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DEBUG_PROTO, "Debugger.Source",
                                  "Debugger.Source");
        return false;
      }

      // If it does have an owner, it should match the Debugger we're
      // calling findScripts on. It would work fine even if it didn't,
      // but mixing Debugger.Sources is probably a sign of confusion.
      if (debuggerSourceObj.owner() != debugger) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DEBUG_WRONG_OWNER, "Debugger.Source");
        return false;
      }

      hasSource = true;
      source = debuggerSourceObj.getReferent();
    }

    // Check for a 'displayURL' property.
    RootedValue displayURL(cx);
    if (!GetProperty(cx, query, query, cx->names().displayURL, &displayURL)) {
      return false;
    }
    if (!displayURL.isUndefined() && !displayURL.isString()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_UNEXPECTED_TYPE,
                                "query object's 'displayURL' property",
                                "neither undefined nor a string");
      return false;
    }

    if (displayURL.isString()) {
      displayURLString = displayURL.toString()->ensureLinear(cx);
      if (!displayURLString) {
        return false;
      }
    }

    // Check for a 'line' property.
    RootedValue lineProperty(cx);
    if (!GetProperty(cx, query, query, cx->names().line, &lineProperty)) {
      return false;
    }
    if (lineProperty.isUndefined()) {
      hasLine = false;
    } else if (lineProperty.isNumber()) {
      if (displayURL.isUndefined() && url.isUndefined() && !hasSource) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_QUERY_LINE_WITHOUT_URL);
        return false;
      }
      double doubleLine = lineProperty.toNumber();
      uint32_t uintLine = (uint32_t)doubleLine;
      if (doubleLine <= 0 || uintLine != doubleLine) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DEBUG_BAD_LINE);
        return false;
      }
      hasLine = true;
      line = uintLine;
    } else {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
          "query object's 'line' property", "neither undefined nor an integer");
      return false;
    }

    // Check for an 'innermost' property.
    PropertyName* innermostName = cx->names().innermost;
    RootedValue innermostProperty(cx);
    if (!GetProperty(cx, query, query, innermostName, &innermostProperty)) {
      return false;
    }
    innermost = ToBoolean(innermostProperty);
    if (innermost) {
      // Technically, we need only check hasLine, but this is clearer.
      if ((displayURL.isUndefined() && url.isUndefined() && !hasSource) ||
          !hasLine) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_QUERY_INNERMOST_WITHOUT_LINE_URL);
        return false;
      }
    }

    return true;
  }

  /* Set up this ScriptQuery appropriately for a missing query argument. */
  bool omittedQuery() {
    url.setUndefined();
    hasLine = false;
    innermost = false;
    displayURLString = nullptr;
    return matchAllDebuggeeGlobals();
  }

  /*
   * Search all relevant realms and the stack for scripts matching
   * this query, and append the matching scripts to |scriptVector|.
   */
  bool findScripts() {
    if (!prepareQuery()) {
      return false;
    }

    Realm* singletonRealm = nullptr;
    if (realms.count() == 1) {
      singletonRealm = realms.all().front();
    }

    // Search each realm for debuggee scripts.
    MOZ_ASSERT(scriptVector.empty());
    MOZ_ASSERT(partialMatchVector.empty());
    oom = false;
    IterateScripts(cx, singletonRealm, this, considerScript);
    if (oom) {
      ReportOutOfMemory(cx);
      return false;
    }

    // If we are filtering by line number, the lazy BaseScripts were not checked
    // yet since they do not implement `GetScriptLineExtent`. Instead we revisit
    // each result script and delazify its children and add any matching ones to
    // the results list.
    MOZ_ASSERT(hasLine || partialMatchVector.empty());
    Rooted<BaseScript*> script(cx);
    RootedFunction fun(cx);
    while (!partialMatchVector.empty()) {
      script = partialMatchVector.popCopy();

      // As a performance optimization, we can skip scripts that are definitely
      // out-of-bounds for the target line. This was checked before adding to
      // the partialMatchVector, but the bound may have improved since then.
      if (script->extent().sourceEnd <= sourceOffsetLowerBound) {
        continue;
      }

      MOZ_ASSERT(script->isFunction());
      MOZ_ASSERT(script->isReadyForDelazification());

      fun = script->function();

      // Ignore any delazification placeholder functions. These should not be
      // exposed to debugger in any way.
      if (fun->isGhost()) {
        continue;
      }

      // Delazify script.
      JSScript* compiledScript = GetOrCreateFunctionScript(cx, fun);
      if (!compiledScript) {
        return false;
      }

      // If target line isn't in script, we are done with it.
      if (!scriptIsLineMatch(compiledScript)) {
        continue;
      }

      // Add script to results now that we've completed checks.
      if (!scriptVector.append(compiledScript)) {
        return false;
      }

      // If script was a leaf we are done with it. This is an optional
      // optimization to avoid inspecting the `gcthings` list below.
      if (!script->hasInnerFunctions()) {
        continue;
      }

      // Now add inner scripts to `partialMatchVector` work list to determine if
      // they are matches. Note that out IterateScripts callback ignored them
      // already since they did not have a compiled parent at the time.
      for (const JS::GCCellPtr& thing : script->gcthings()) {
        if (!thing.is<JSObject>() || !thing.as<JSObject>().is<JSFunction>()) {
          continue;
        }
        JSFunction* fun = &thing.as<JSObject>().as<JSFunction>();
        if (!fun->hasBaseScript()) {
          continue;
        }
        BaseScript* inner = fun->baseScript();
        MOZ_ASSERT(inner);
        if (!inner) {
          // If the function doesn't have script, ignore it.
          continue;
        }

        if (!scriptIsPartialLineMatch(inner)) {
          continue;
        }

        // Add the matching inner script to the back of the results queue
        // where it will be processed recursively.
        if (!partialMatchVector.append(inner)) {
          return false;
        }
      }
    }

    // If this is an 'innermost' query, we want to filter the results again to
    // only return the innermost script for each realm. To do this we build a
    // hashmap to track innermost and then recreate the `scriptVector` with the
    // results that remain in the hashmap.
    if (innermost) {
      using RealmToScriptMap =
          GCHashMap<Realm*, BaseScript*, DefaultHasher<Realm*>>;

      Rooted<RealmToScriptMap> innermostForRealm(cx);

      // Visit each candidate script and find innermost in each realm.
      for (BaseScript* script : scriptVector) {
        Realm* realm = script->realm();
        RealmToScriptMap::AddPtr p = innermostForRealm.lookupForAdd(realm);
        if (p) {
          // Is our newly found script deeper than the last one we found?
          BaseScript* incumbent = p->value();
          if (script->asJSScript()->innermostScope()->chainLength() >
              incumbent->asJSScript()->innermostScope()->chainLength()) {
            p->value() = script;
          }
        } else {
          // This is the first matching script we've encountered for this
          // realm, so it is thus the innermost such script.
          if (!innermostForRealm.add(p, realm, script)) {
            return false;
          }
        }
      }

      // Reset the results vector.
      scriptVector.clear();

      // Re-add only the innermost scripts to the results.
      for (RealmToScriptMap::Range r = innermostForRealm.all(); !r.empty();
           r.popFront()) {
        if (!scriptVector.append(r.front().value())) {
          return false;
        }
      }
    }

    // TODO: Until such time that wasm modules are real ES6 modules,
    // unconditionally consider all wasm toplevel instance scripts.
    for (WeakGlobalObjectSet::Range r = debugger->allDebuggees(); !r.empty();
         r.popFront()) {
      for (wasm::Instance* instance : r.front()->realm()->wasm.instances()) {
        consider(instance->object());
        if (oom) {
          ReportOutOfMemory(cx);
          return false;
        }
      }
    }

    return true;
  }

  Handle<BaseScriptVector> foundScripts() const { return scriptVector; }

  Handle<WasmInstanceObjectVector> foundWasmInstances() const {
    return wasmInstanceVector;
  }

 private:
  /* If this is a string, matching scripts have urls equal to it. */
  RootedValue url;

  /* url as a C string. */
  UniqueChars urlCString;

  /* If this is a string, matching scripts' sources have displayURLs equal to
   * it. */
  RootedLinearString displayURLString;

  /*
   * If this is a source referent, matching scripts will have sources equal
   * to this instance. Ideally we'd use a Maybe here, but Maybe interacts
   * very badly with Rooted's LIFO invariant.
   */
  bool hasSource = false;
  Rooted<DebuggerSourceReferent> source;

  /* True if the query contained a 'line' property. */
  bool hasLine = false;

  /* The line matching scripts must cover. */
  uint32_t line = 0;

  // As a performance optimization (and to avoid delazifying as many scripts),
  // we would like to know the source offset of the target line.
  //
  // Since we do not have a simple way to compute this precisely, we instead
  // track a lower-bound of the offset value. As we collect SourceExtent
  // examples with (line,column) <-> sourceStart mappings, we can improve the
  // bound. The target line is within the range [sourceOffsetLowerBound, Inf).
  //
  // NOTE: Using a SourceExtent for updating the bound happens independently of
  //       if the script matches the target line or not in the in the end.
  mutable uint32_t sourceOffsetLowerBound = 0;

  /* True if the query has an 'innermost' property whose value is true. */
  bool innermost = false;

  /*
   * Accumulate the scripts in an Rooted<BaseScriptVector> instead of creating
   * the JS array as we go, because we mustn't allocate JS objects or GC while
   * we use the CellIter.
   */
  Rooted<BaseScriptVector> scriptVector;

  /*
   * While in the CellIter we may find BaseScripts that need to be compiled
   * before the query can be fully checked. Since we cannot compile while under
   * CellIter we accumulate them here instead.
   *
   * This occurs when matching line numbers since `GetScriptLineExtent` cannot
   * be computed without bytecode existing.
   */
  Rooted<BaseScriptVector> partialMatchVector;

  /*
   * Like above, but for wasm modules.
   */
  Rooted<WasmInstanceObjectVector> wasmInstanceVector;

  /*
   * Given that parseQuery or omittedQuery has been called, prepare to match
   * scripts. Set urlCString and displayURLChars as appropriate.
   */
  bool prepareQuery() {
    // Compute urlCString and displayURLChars, if a url or displayURL was
    // given respectively.
    if (url.isString()) {
      urlCString = JS_EncodeStringToLatin1(cx, url.toString());
      if (!urlCString) {
        return false;
      }
    }

    return true;
  }

  void updateSourceOffsetLowerBound(const SourceExtent& extent) {
    // We trying to find the offset of (target-line, 0) so just ignore any
    // extents on target line to keep things simple.
    MOZ_ASSERT(extent.lineno <= line);
    if (extent.lineno == line) {
      return;
    }

    // The extent.sourceStart position is now definitely *before* the target
    // line, so update sourceOffsetLowerBound if extent.sourceStart is a tighter
    // bound.
    if (extent.sourceStart > sourceOffsetLowerBound) {
      sourceOffsetLowerBound = extent.sourceStart;
    }
  }

  // A partial match is a script that starts before the target line, but may or
  // may not end before it. If we can prove the script definitely ends before
  // the target line, we may return false here.
  bool scriptIsPartialLineMatch(BaseScript* script) {
    const SourceExtent& extent = script->extent();

    // Check that start of script is before or on target line.
    if (extent.lineno > line) {
      return false;
    }

    // Use the implicit (line, column) <-> sourceStart mapping from the
    // SourceExtent to update our bounds on possible matches. We call this
    // without knowing if the script is a match or not.
    updateSourceOffsetLowerBound(script->extent());

    // As an optional performance optimization, we rule out any script that ends
    // before the lower-bound on where target line exists.
    return extent.sourceEnd > sourceOffsetLowerBound;
  }

  // True if any part of script source is on the target line.
  bool scriptIsLineMatch(JSScript* script) {
    MOZ_ASSERT(scriptIsPartialLineMatch(script));

    uint32_t lineCount = GetScriptLineExtent(script);
    return (script->lineno() + lineCount > line);
  }

  static void considerScript(JSRuntime* rt, void* data, BaseScript* script,
                             const JS::AutoRequireNoGC& nogc) {
    ScriptQuery* self = static_cast<ScriptQuery*>(data);
    self->consider(script, nogc);
  }

  template <typename T>
  [[nodiscard]] bool commonFilter(T script, const JS::AutoRequireNoGC& nogc) {
    if (urlCString) {
      bool gotFilename = false;
      if (script->filename() &&
          strcmp(script->filename(), urlCString.get()) == 0) {
        gotFilename = true;
      }

      bool gotSourceURL = false;
      if (!gotFilename && script->scriptSource()->introducerFilename() &&
          strcmp(script->scriptSource()->introducerFilename(),
                 urlCString.get()) == 0) {
        gotSourceURL = true;
      }
      if (!gotFilename && !gotSourceURL) {
        return false;
      }
    }
    if (displayURLString) {
      if (!script->scriptSource() || !script->scriptSource()->hasDisplayURL()) {
        return false;
      }

      const char16_t* s = script->scriptSource()->displayURL();
      if (CompareChars(s, js_strlen(s), displayURLString) != 0) {
        return false;
      }
    }
    if (hasSource && !(source.is<ScriptSourceObject*>() &&
                       source.as<ScriptSourceObject*>()->source() ==
                           script->scriptSource())) {
      return false;
    }
    return true;
  }

  /*
   * If |script| matches this query, append it to |scriptVector|. Set |oom| if
   * an out of memory condition occurred.
   */
  void consider(BaseScript* script, const JS::AutoRequireNoGC& nogc) {
    if (oom || script->selfHosted()) {
      return;
    }

    Realm* realm = script->realm();
    if (!realms.has(realm)) {
      return;
    }

    if (!commonFilter(script, nogc)) {
      return;
    }

    bool partial = false;

    if (hasLine) {
      if (!scriptIsPartialLineMatch(script)) {
        return;
      }

      if (script->hasBytecode()) {
        // Check if line is within script (or any of its inner scripts).
        if (!scriptIsLineMatch(script->asJSScript())) {
          return;
        }
      } else {
        // GetScriptLineExtent is not available on lazy scripts so instead to
        // the partial match list for be compiled and reprocessed later. We only
        // add scripts that are ready for delazification and they may in turn
        // process their inner functions.
        if (!script->isReadyForDelazification()) {
          return;
        }
        partial = true;
      }
    }

    // If innermost filter is required, we collect everything that matches the
    // line number and filter at the end of `findScripts`.
    MOZ_ASSERT_IF(innermost, hasLine);

    Rooted<BaseScriptVector>& vec = partial ? partialMatchVector : scriptVector;
    if (!vec.append(script)) {
      oom = true;
    }
  }

  /*
   * If |instanceObject| matches this query, append it to |wasmInstanceVector|.
   * Set |oom| if an out of memory condition occurred.
   */
  void consider(WasmInstanceObject* instanceObject) {
    if (oom) {
      return;
    }

    if (hasSource && source != AsVariant(instanceObject)) {
      return;
    }

    if (!wasmInstanceVector.append(instanceObject)) {
      oom = true;
    }
  }
};

bool Debugger::CallData::findScripts() {
  ScriptQuery query(cx, dbg);

  if (args.length() >= 1) {
    RootedObject queryObject(cx, RequireObject(cx, args[0]));
    if (!queryObject || !query.parseQuery(queryObject)) {
      return false;
    }
  } else {
    if (!query.omittedQuery()) {
      return false;
    }
  }

  if (!query.findScripts()) {
    return false;
  }

  Handle<BaseScriptVector> scripts(query.foundScripts());
  Handle<WasmInstanceObjectVector> wasmInstances(query.foundWasmInstances());

  size_t resultLength = scripts.length() + wasmInstances.length();
  RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, resultLength));
  if (!result) {
    return false;
  }

  result->ensureDenseInitializedLength(0, resultLength);

  for (size_t i = 0; i < scripts.length(); i++) {
    JSObject* scriptObject = dbg->wrapScript(cx, scripts[i]);
    if (!scriptObject) {
      return false;
    }
    result->setDenseElement(i, ObjectValue(*scriptObject));
  }

  size_t wasmStart = scripts.length();
  for (size_t i = 0; i < wasmInstances.length(); i++) {
    JSObject* scriptObject = dbg->wrapWasmScript(cx, wasmInstances[i]);
    if (!scriptObject) {
      return false;
    }
    result->setDenseElement(wasmStart + i, ObjectValue(*scriptObject));
  }

  args.rval().setObject(*result);
  return true;
}

/*
 * A class for searching sources for 'findSources'.
 */
class MOZ_STACK_CLASS Debugger::SourceQuery : public Debugger::QueryBase {
 public:
  using SourceSet = JS::GCHashSet<JSObject*, js::MovableCellHasher<JSObject*>,
                                  ZoneAllocPolicy>;

  SourceQuery(JSContext* cx, Debugger* dbg)
      : QueryBase(cx, dbg), sources(cx, SourceSet(cx->zone())) {}

  bool findSources() {
    if (!matchAllDebuggeeGlobals()) {
      return false;
    }

    Realm* singletonRealm = nullptr;
    if (realms.count() == 1) {
      singletonRealm = realms.all().front();
    }

    // Search each realm for debuggee scripts.
    MOZ_ASSERT(sources.empty());
    oom = false;
    IterateScripts(cx, singletonRealm, this, considerScript);
    if (oom) {
      ReportOutOfMemory(cx);
      return false;
    }

    // TODO: Until such time that wasm modules are real ES6 modules,
    // unconditionally consider all wasm toplevel instance scripts.
    for (WeakGlobalObjectSet::Range r = debugger->allDebuggees(); !r.empty();
         r.popFront()) {
      for (wasm::Instance* instance : r.front()->realm()->wasm.instances()) {
        consider(instance->object());
        if (oom) {
          ReportOutOfMemory(cx);
          return false;
        }
      }
    }

    return true;
  }

  Handle<SourceSet> foundSources() const { return sources; }

 private:
  Rooted<SourceSet> sources;

  static void considerScript(JSRuntime* rt, void* data, BaseScript* script,
                             const JS::AutoRequireNoGC& nogc) {
    SourceQuery* self = static_cast<SourceQuery*>(data);
    self->consider(script, nogc);
  }

  void consider(BaseScript* script, const JS::AutoRequireNoGC& nogc) {
    if (oom || script->selfHosted()) {
      return;
    }

    Realm* realm = script->realm();
    if (!realms.has(realm)) {
      return;
    }

    ScriptSourceObject* source = script->sourceObject();
    if (!sources.put(source)) {
      oom = true;
    }
  }

  void consider(WasmInstanceObject* instanceObject) {
    if (oom) {
      return;
    }

    if (!sources.put(instanceObject)) {
      oom = true;
    }
  }
};

static inline DebuggerSourceReferent AsSourceReferent(JSObject* obj) {
  if (obj->is<ScriptSourceObject>()) {
    return AsVariant(&obj->as<ScriptSourceObject>());
  }
  return AsVariant(&obj->as<WasmInstanceObject>());
}

bool Debugger::CallData::findSources() {
  SourceQuery query(cx, dbg);
  if (!query.findSources()) {
    return false;
  }

  Handle<SourceQuery::SourceSet> sources(query.foundSources());

  size_t resultLength = sources.count();
  RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, resultLength));
  if (!result) {
    return false;
  }

  result->ensureDenseInitializedLength(0, resultLength);

  size_t i = 0;
  for (auto iter = sources.get().iter(); !iter.done(); iter.next()) {
    Rooted<DebuggerSourceReferent> sourceReferent(cx,
                                                  AsSourceReferent(iter.get()));
    RootedObject sourceObject(cx, dbg->wrapVariantReferent(cx, sourceReferent));
    if (!sourceObject) {
      return false;
    }
    result->setDenseElement(i, ObjectValue(*sourceObject));
    i++;
  }

  args.rval().setObject(*result);
  return true;
}

/*
 * A class for parsing 'findObjects' query arguments and searching for objects
 * that match the criteria they represent.
 */
class MOZ_STACK_CLASS Debugger::ObjectQuery {
 public:
  /* Construct an ObjectQuery to use matching scripts for |dbg|. */
  ObjectQuery(JSContext* cx, Debugger* dbg)
      : objects(cx), cx(cx), dbg(dbg), className(cx) {}

  /* The vector that we are accumulating results in. */
  RootedObjectVector objects;

  /* The set of debuggee compartments. */
  JS::CompartmentSet debuggeeCompartments;

  /*
   * Parse the query object |query|, and prepare to match only the objects it
   * specifies.
   */
  bool parseQuery(HandleObject query) {
    // Check for the 'class' property
    RootedValue cls(cx);
    if (!GetProperty(cx, query, query, cx->names().class_, &cls)) {
      return false;
    }
    if (!cls.isUndefined()) {
      if (!cls.isString()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_UNEXPECTED_TYPE,
                                  "query object's 'class' property",
                                  "neither undefined nor a string");
        return false;
      }
      JSLinearString* str = cls.toString()->ensureLinear(cx);
      if (!str) {
        return false;
      }
      if (!StringIsAscii(str)) {
        JS_ReportErrorNumberASCII(
            cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
            "query object's 'class' property",
            "not a string containing only ASCII characters");
        return false;
      }
      className = cls;
    }
    return true;
  }

  /* Set up this ObjectQuery appropriately for a missing query argument. */
  void omittedQuery() { className.setUndefined(); }

  /*
   * Traverse the heap to find all relevant objects and add them to the
   * provided vector.
   */
  bool findObjects() {
    if (!prepareQuery()) {
      return false;
    }

    for (WeakGlobalObjectSet::Range r = dbg->allDebuggees(); !r.empty();
         r.popFront()) {
      if (!debuggeeCompartments.put(r.front()->compartment())) {
        ReportOutOfMemory(cx);
        return false;
      }
    }

    {
      // We can't tolerate the GC moving things around while we're
      // searching the heap. Check that nothing we do causes a GC.
      Maybe<JS::AutoCheckCannotGC> maybeNoGC;
      RootedObject dbgObj(cx, dbg->object);
      JS::ubi::RootList rootList(cx, maybeNoGC);
      if (!rootList.init(dbgObj)) {
        ReportOutOfMemory(cx);
        return false;
      }

      Traversal traversal(cx, *this, maybeNoGC.ref());
      traversal.wantNames = false;

      return traversal.addStart(JS::ubi::Node(&rootList)) &&
             traversal.traverse();
    }
  }

  /*
   * |ubi::Node::BreadthFirst| interface.
   */
  class NodeData {};
  using Traversal = JS::ubi::BreadthFirst<ObjectQuery>;
  bool operator()(Traversal& traversal, JS::ubi::Node origin,
                  const JS::ubi::Edge& edge, NodeData*, bool first) {
    if (!first) {
      return true;
    }

    JS::ubi::Node referent = edge.referent;

    // Only follow edges within our set of debuggee compartments; we don't
    // care about the heap's subgraphs outside of our debuggee compartments,
    // so we abandon the referent. Either (1) there is not a path from this
    // non-debuggee node back to a node in our debuggee compartments, and we
    // don't need to follow edges to or from this node, or (2) there does
    // exist some path from this non-debuggee node back to a node in our
    // debuggee compartments. However, if that were true, then the incoming
    // cross compartment edge back into a debuggee compartment is already
    // listed as an edge in the RootList we started traversal with, and
    // therefore we don't need to follow edges to or from this non-debuggee
    // node.
    JS::Compartment* comp = referent.compartment();
    if (comp && !debuggeeCompartments.has(comp)) {
      traversal.abandonReferent();
      return true;
    }

    // If the referent has an associated realm and it's not a debuggee
    // realm, skip it. Don't abandonReferent() here like above: realms
    // within a compartment can reference each other without going through
    // cross-compartment wrappers.
    Realm* realm = referent.realm();
    if (realm && !dbg->isDebuggeeUnbarriered(realm)) {
      return true;
    }

    // If the referent is an object and matches our query's restrictions,
    // add it to the vector accumulating results. Skip objects that should
    // never be exposed to JS, like EnvironmentObjects and internal
    // functions.

    if (!referent.is<JSObject>() || referent.exposeToJS().isUndefined()) {
      return true;
    }

    JSObject* obj = referent.as<JSObject>();

    if (!className.isUndefined()) {
      const char* objClassName = obj->getClass()->name;
      if (strcmp(objClassName, classNameCString.get()) != 0) {
        return true;
      }
    }

    return objects.append(obj);
  }

 private:
  /* The context in which we should do our work. */
  JSContext* cx;

  /* The debugger for which we conduct queries. */
  Debugger* dbg;

  /*
   * If this is non-null, matching objects will have a class whose name is
   * this property.
   */
  RootedValue className;

  /* The className member, as a C string. */
  UniqueChars classNameCString;

  /*
   * Given that either omittedQuery or parseQuery has been called, prepare the
   * query for matching objects.
   */
  bool prepareQuery() {
    if (className.isString()) {
      classNameCString = JS_EncodeStringToASCII(cx, className.toString());
      if (!classNameCString) {
        return false;
      }
    }

    return true;
  }
};

bool Debugger::CallData::findObjects() {
  ObjectQuery query(cx, dbg);

  if (args.length() >= 1) {
    RootedObject queryObject(cx, RequireObject(cx, args[0]));
    if (!queryObject || !query.parseQuery(queryObject)) {
      return false;
    }
  } else {
    query.omittedQuery();
  }

  if (!query.findObjects()) {
    return false;
  }

  size_t length = query.objects.length();
  RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, length));
  if (!result) {
    return false;
  }

  result->ensureDenseInitializedLength(0, length);

  for (size_t i = 0; i < length; i++) {
    RootedValue debuggeeVal(cx, ObjectValue(*query.objects[i]));
    if (!dbg->wrapDebuggeeValue(cx, &debuggeeVal)) {
      return false;
    }
    result->setDenseElement(i, debuggeeVal);
  }

  args.rval().setObject(*result);
  return true;
}

bool Debugger::CallData::findAllGlobals() {
  RootedObjectVector globals(cx);

  {
    // Accumulate the list of globals before wrapping them, because
    // wrapping can GC and collect realms from under us, while iterating.
    JS::AutoCheckCannotGC nogc;

    for (RealmsIter r(cx->runtime()); !r.done(); r.next()) {
      if (r->creationOptions().invisibleToDebugger()) {
        continue;
      }

      if (!r->hasLiveGlobal()) {
        continue;
      }

      if (JS::RealmBehaviorsRef(r).isNonLive()) {
        continue;
      }

      r->compartment()->gcState.scheduledForDestruction = false;

      GlobalObject* global = r->maybeGlobal();

      if (cx->runtime()->isSelfHostingGlobal(global)) {
        continue;
      }

      // We pulled |global| out of nowhere, so it's possible that it was
      // marked gray by XPConnect. Since we're now exposing it to JS code,
      // we need to mark it black.
      JS::ExposeObjectToActiveJS(global);
      if (!globals.append(global)) {
        return false;
      }
    }
  }

  RootedObject result(cx, NewDenseEmptyArray(cx));
  if (!result) {
    return false;
  }

  for (size_t i = 0; i < globals.length(); i++) {
    RootedValue globalValue(cx, ObjectValue(*globals[i]));
    if (!dbg->wrapDebuggeeValue(cx, &globalValue)) {
      return false;
    }
    if (!NewbornArrayPush(cx, result, globalValue)) {
      return false;
    }
  }

  args.rval().setObject(*result);
  return true;
}

bool Debugger::CallData::findSourceURLs() {
  RootedObject result(cx, NewDenseEmptyArray(cx));
  if (!result) {
    return false;
  }

  for (WeakGlobalObjectSet::Range r = dbg->allDebuggees(); !r.empty();
       r.popFront()) {
    RootedObject holder(cx, r.front()->getSourceURLsHolder());
    if (holder) {
      for (size_t i = 0; i < holder->as<ArrayObject>().length(); i++) {
        Value v = holder->as<ArrayObject>().getDenseElement(i);

        // The value is an atom and doesn't need wrapping, but the holder may be
        // in another zone and the atom must be marked when we create a
        // reference in this zone.
        MOZ_ASSERT(v.isString() && v.toString()->isAtom());
        cx->markAtomValue(v);

        if (!NewbornArrayPush(cx, result, v)) {
          return false;
        }
      }
    }
  }

  args.rval().setObject(*result);
  return true;
}

bool Debugger::CallData::makeGlobalObjectReference() {
  if (!args.requireAtLeast(cx, "Debugger.makeGlobalObjectReference", 1)) {
    return false;
  }

  Rooted<GlobalObject*> global(cx, dbg->unwrapDebuggeeArgument(cx, args[0]));
  if (!global) {
    return false;
  }

  // If we create a D.O referring to a global in an invisible realm,
  // then from it we can reach function objects, scripts, environments, etc.,
  // none of which we're ever supposed to see.
  if (global->realm()->creationOptions().invisibleToDebugger()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_INVISIBLE_COMPARTMENT);
    return false;
  }

  args.rval().setObject(*global);
  return dbg->wrapDebuggeeValue(cx, args.rval());
}

bool Debugger::isCompilableUnit(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "Debugger.isCompilableUnit", 1)) {
    return false;
  }

  if (!args[0].isString()) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
        "Debugger.isCompilableUnit", "string", InformalValueTypeName(args[0]));
    return false;
  }

  JSString* str = args[0].toString();
  size_t length = str->length();

  AutoStableStringChars chars(cx);
  if (!chars.initTwoByte(cx, str)) {
    return false;
  }

  bool result = true;

  CompileOptions options(cx);
  Rooted<frontend::CompilationInput> input(cx,
                                           frontend::CompilationInput(options));
  if (!input.get().initForGlobal(cx)) {
    return false;
  }

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  frontend::CompilationState compilationState(cx, allocScope, input.get());
  if (!compilationState.init(cx)) {
    return false;
  }

  JS::AutoSuppressWarningReporter suppressWarnings(cx);
  frontend::Parser<frontend::FullParseHandler, char16_t> parser(
      cx, options, chars.twoByteChars(), length,
      /* foldConstants = */ true, compilationState,
      /* syntaxParser = */ nullptr);
  if (!parser.checkOptions() || !parser.parse()) {
    // We ran into an error. If it was because we ran out of memory we report
    // it in the usual way.
    if (cx->isThrowingOutOfMemory()) {
      return false;
    }

    // If it was because we ran out of source, we return false so our caller
    // knows to try to collect more [source].
    if (parser.isUnexpectedEOF()) {
      result = false;
    }

    cx->clearPendingException();
  }

  args.rval().setBoolean(result);
  return true;
}

bool Debugger::CallData::adoptDebuggeeValue() {
  if (!args.requireAtLeast(cx, "Debugger.adoptDebuggeeValue", 1)) {
    return false;
  }

  RootedValue v(cx, args[0]);
  if (v.isObject()) {
    RootedObject obj(cx, &v.toObject());
    DebuggerObject* ndobj = ToNativeDebuggerObject(cx, &obj);
    if (!ndobj) {
      return false;
    }

    obj.set(ndobj->referent());
    v = ObjectValue(*obj);

    if (!dbg->wrapDebuggeeValue(cx, &v)) {
      return false;
    }
  }

  args.rval().set(v);
  return true;
}

class DebuggerAdoptSourceMatcher {
  JSContext* cx_;
  Debugger* dbg_;

 public:
  explicit DebuggerAdoptSourceMatcher(JSContext* cx, Debugger* dbg)
      : cx_(cx), dbg_(dbg) {}

  using ReturnType = DebuggerSource*;

  ReturnType match(HandleScriptSourceObject source) {
    if (source->compartment() == cx_->compartment()) {
      JS_ReportErrorASCII(cx_,
                          "Source is in the same compartment as this debugger");
      return nullptr;
    }
    return dbg_->wrapSource(cx_, source);
  }
  ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
    if (wasmInstance->compartment() == cx_->compartment()) {
      JS_ReportErrorASCII(
          cx_, "WasmInstance is in the same compartment as this debugger");
      return nullptr;
    }
    return dbg_->wrapWasmSource(cx_, wasmInstance);
  }
};

bool Debugger::CallData::adoptFrame() {
  if (!args.requireAtLeast(cx, "Debugger.adoptFrame", 1)) {
    return false;
  }

  RootedObject obj(cx, RequireObject(cx, args[0]));
  if (!obj) {
    return false;
  }

  obj = UncheckedUnwrap(obj);
  if (!obj->is<DebuggerFrame>()) {
    JS_ReportErrorASCII(cx, "Argument is not a Debugger.Frame");
    return false;
  }

  RootedValue objVal(cx, ObjectValue(*obj));
  RootedDebuggerFrame frameObj(cx, DebuggerFrame::check(cx, objVal));
  if (!frameObj) {
    return false;
  }

  RootedDebuggerFrame adoptedFrame(cx);
  if (frameObj->isOnStack()) {
    FrameIter iter = frameObj->getFrameIter(cx);
    if (!dbg->observesFrame(iter)) {
      JS_ReportErrorASCII(cx, "Debugger.Frame's global is not a debuggee");
      return false;
    }
    if (!dbg->getFrame(cx, iter, &adoptedFrame)) {
      return false;
    }
  } else if (frameObj->isSuspended()) {
    Rooted<AbstractGeneratorObject*> gen(cx, &frameObj->unwrappedGenerator());
    if (!dbg->observesGlobal(&gen->global())) {
      JS_ReportErrorASCII(cx, "Debugger.Frame's global is not a debuggee");
      return false;
    }

    if (!dbg->getFrame(cx, gen, &adoptedFrame)) {
      return false;
    }
  } else {
    if (!dbg->getFrame(cx, &adoptedFrame)) {
      return false;
    }
  }

  args.rval().setObject(*adoptedFrame);
  return true;
}

bool Debugger::CallData::adoptSource() {
  if (!args.requireAtLeast(cx, "Debugger.adoptSource", 1)) {
    return false;
  }

  RootedObject obj(cx, RequireObject(cx, args[0]));
  if (!obj) {
    return false;
  }

  obj = UncheckedUnwrap(obj);
  if (!obj->is<DebuggerSource>()) {
    JS_ReportErrorASCII(cx, "Argument is not a Debugger.Source");
    return false;
  }

  RootedDebuggerSource sourceObj(cx, &obj->as<DebuggerSource>());
  if (!sourceObj->getReferentRawObject()) {
    JS_ReportErrorASCII(cx, "Argument is Debugger.Source.prototype");
    return false;
  }

  Rooted<DebuggerSourceReferent> referent(cx, sourceObj->getReferent());

  DebuggerAdoptSourceMatcher matcher(cx, dbg);
  DebuggerSource* res = referent.match(matcher);
  if (!res) {
    return false;
  }

  args.rval().setObject(*res);
  return true;
}

const JSPropertySpec Debugger::properties[] = {
    JS_DEBUG_PSGS("onDebuggerStatement", getOnDebuggerStatement,
                  setOnDebuggerStatement),
    JS_DEBUG_PSGS("onExceptionUnwind", getOnExceptionUnwind,
                  setOnExceptionUnwind),
    JS_DEBUG_PSGS("onNewScript", getOnNewScript, setOnNewScript),
    JS_DEBUG_PSGS("onNewPromise", getOnNewPromise, setOnNewPromise),
    JS_DEBUG_PSGS("onPromiseSettled", getOnPromiseSettled, setOnPromiseSettled),
    JS_DEBUG_PSGS("onEnterFrame", getOnEnterFrame, setOnEnterFrame),
    JS_DEBUG_PSGS("onNativeCall", getOnNativeCall, setOnNativeCall),
    JS_DEBUG_PSGS("onNewGlobalObject", getOnNewGlobalObject,
                  setOnNewGlobalObject),
    JS_DEBUG_PSGS("uncaughtExceptionHook", getUncaughtExceptionHook,
                  setUncaughtExceptionHook),
    JS_DEBUG_PSGS("allowUnobservedAsmJS", getAllowUnobservedAsmJS,
                  setAllowUnobservedAsmJS),
    JS_DEBUG_PSGS("collectCoverageInfo", getCollectCoverageInfo,
                  setCollectCoverageInfo),
    JS_DEBUG_PSG("memory", getMemory),
    JS_STRING_SYM_PS(toStringTag, "Debugger", JSPROP_READONLY),
    JS_PS_END};

const JSFunctionSpec Debugger::methods[] = {
    JS_DEBUG_FN("addDebuggee", addDebuggee, 1),
    JS_DEBUG_FN("addAllGlobalsAsDebuggees", addAllGlobalsAsDebuggees, 0),
    JS_DEBUG_FN("removeDebuggee", removeDebuggee, 1),
    JS_DEBUG_FN("removeAllDebuggees", removeAllDebuggees, 0),
    JS_DEBUG_FN("hasDebuggee", hasDebuggee, 1),
    JS_DEBUG_FN("getDebuggees", getDebuggees, 0),
    JS_DEBUG_FN("getNewestFrame", getNewestFrame, 0),
    JS_DEBUG_FN("clearAllBreakpoints", clearAllBreakpoints, 0),
    JS_DEBUG_FN("findScripts", findScripts, 1),
    JS_DEBUG_FN("findSources", findSources, 1),
    JS_DEBUG_FN("findObjects", findObjects, 1),
    JS_DEBUG_FN("findAllGlobals", findAllGlobals, 0),
    JS_DEBUG_FN("findSourceURLs", findSourceURLs, 0),
    JS_DEBUG_FN("makeGlobalObjectReference", makeGlobalObjectReference, 1),
    JS_DEBUG_FN("adoptDebuggeeValue", adoptDebuggeeValue, 1),
    JS_DEBUG_FN("adoptFrame", adoptFrame, 1),
    JS_DEBUG_FN("adoptSource", adoptSource, 1),
    JS_FS_END};

const JSFunctionSpec Debugger::static_methods[]{
    JS_FN("isCompilableUnit", Debugger::isCompilableUnit, 1, 0), JS_FS_END};

DebuggerScript* Debugger::newDebuggerScript(
    JSContext* cx, Handle<DebuggerScriptReferent> referent) {
  cx->check(object.get());

  RootedObject proto(
      cx, &object->getReservedSlot(JSSLOT_DEBUG_SCRIPT_PROTO).toObject());
  MOZ_ASSERT(proto);
  RootedNativeObject debugger(cx, object);

  return DebuggerScript::create(cx, proto, referent, debugger);
}

template <typename ReferentType, typename Map>
typename Map::WrapperType* Debugger::wrapVariantReferent(
    JSContext* cx, Map& map,
    Handle<typename Map::WrapperType::ReferentVariant> referent) {
  cx->check(object);

  Handle<ReferentType*> untaggedReferent =
      referent.template as<ReferentType*>();
  MOZ_ASSERT(cx->compartment() != untaggedReferent->compartment());

  DependentAddPtr<Map> p(cx, map, untaggedReferent);
  if (!p) {
    typename Map::WrapperType* wrapper = newVariantWrapper(cx, referent);
    if (!wrapper) {
      return nullptr;
    }

    if (!p.add(cx, map, untaggedReferent, wrapper)) {
      NukeDebuggerWrapper(wrapper);
      return nullptr;
    }
  }

  return &p->value()->template as<typename Map::WrapperType>();
}

DebuggerScript* Debugger::wrapVariantReferent(
    JSContext* cx, Handle<DebuggerScriptReferent> referent) {
  if (referent.is<BaseScript*>()) {
    return wrapVariantReferent<BaseScript>(cx, scripts, referent);
  }

  return wrapVariantReferent<WasmInstanceObject>(cx, wasmInstanceScripts,
                                                 referent);
}

DebuggerScript* Debugger::wrapScript(JSContext* cx,
                                     Handle<BaseScript*> script) {
  Rooted<DebuggerScriptReferent> referent(cx,
                                          DebuggerScriptReferent(script.get()));
  return wrapVariantReferent(cx, referent);
}

DebuggerScript* Debugger::wrapWasmScript(
    JSContext* cx, Handle<WasmInstanceObject*> wasmInstance) {
  Rooted<DebuggerScriptReferent> referent(cx, wasmInstance.get());
  return wrapVariantReferent(cx, referent);
}

DebuggerSource* Debugger::newDebuggerSource(
    JSContext* cx, Handle<DebuggerSourceReferent> referent) {
  cx->check(object.get());

  RootedObject proto(
      cx, &object->getReservedSlot(JSSLOT_DEBUG_SOURCE_PROTO).toObject());
  MOZ_ASSERT(proto);
  RootedNativeObject debugger(cx, object);
  return DebuggerSource::create(cx, proto, referent, debugger);
}

DebuggerSource* Debugger::wrapVariantReferent(
    JSContext* cx, Handle<DebuggerSourceReferent> referent) {
  DebuggerSource* obj;
  if (referent.is<ScriptSourceObject*>()) {
    obj = wrapVariantReferent<ScriptSourceObject>(cx, sources, referent);
  } else {
    obj = wrapVariantReferent<WasmInstanceObject>(cx, wasmInstanceSources,
                                                  referent);
  }
  MOZ_ASSERT_IF(obj, obj->getReferent() == referent);
  return obj;
}

DebuggerSource* Debugger::wrapSource(JSContext* cx,
                                     HandleScriptSourceObject source) {
  Rooted<DebuggerSourceReferent> referent(cx, source.get());
  return wrapVariantReferent(cx, referent);
}

DebuggerSource* Debugger::wrapWasmSource(
    JSContext* cx, Handle<WasmInstanceObject*> wasmInstance) {
  Rooted<DebuggerSourceReferent> referent(cx, wasmInstance.get());
  return wrapVariantReferent(cx, referent);
}

bool Debugger::observesFrame(AbstractFramePtr frame) const {
  if (frame.isWasmDebugFrame()) {
    return observesWasm(frame.wasmInstance());
  }

  return observesScript(frame.script());
}

bool Debugger::observesFrame(const FrameIter& iter) const {
  // Skip frames not yet fully initialized during their prologue.
  if (iter.isInterp() && iter.isFunctionFrame()) {
    const Value& thisVal = iter.interpFrame()->thisArgument();
    if (thisVal.isMagic() && thisVal.whyMagic() == JS_IS_CONSTRUCTING) {
      return false;
    }
  }
  if (iter.isWasm()) {
    // Skip frame of wasm instances we cannot observe.
    if (!iter.wasmDebugEnabled()) {
      return false;
    }
    return observesWasm(iter.wasmInstance());
  }
  return observesScript(iter.script());
}

bool Debugger::observesScript(JSScript* script) const {
  // Don't ever observe self-hosted scripts: the Debugger API can break
  // self-hosted invariants.
  return observesGlobal(&script->global()) && !script->selfHosted();
}

bool Debugger::observesWasm(wasm::Instance* instance) const {
  if (!instance->debugEnabled()) {
    return false;
  }
  return observesGlobal(&instance->object()->global());
}

/* static */
bool Debugger::replaceFrameGuts(JSContext* cx, AbstractFramePtr from,
                                AbstractFramePtr to, ScriptFrameIter& iter) {
  MOZ_ASSERT(from != to);

  // Rekey missingScopes to maintain Debugger.Environment identity and
  // forward liveScopes to point to the new frame.
  DebugEnvironments::forwardLiveFrame(cx, from, to);

  // If we hit an OOM anywhere in here, we need to make sure there aren't any
  // Debugger.Frame objects left partially-initialized.
  auto terminateDebuggerFramesOnExit = MakeScopeExit([&] {
    terminateDebuggerFrames(cx, from);
    terminateDebuggerFrames(cx, to);

    MOZ_ASSERT(!DebugAPI::inFrameMaps(from));
    MOZ_ASSERT(!DebugAPI::inFrameMaps(to));
  });

  // Forward live Debugger.Frame objects.
  Rooted<DebuggerFrameVector> frames(cx, DebuggerFrameVector(cx));
  if (!getDebuggerFrames(from, &frames)) {
    // An OOM here means that all Debuggers' frame maps still contain
    // entries for 'from' and no entries for 'to'. Since the 'from' frame
    // will be gone, they are removed by terminateDebuggerFramesOnExit
    // above.
    return false;
  }

  for (size_t i = 0; i < frames.length(); i++) {
    HandleDebuggerFrame frameobj = frames[i];
    Debugger* dbg = frameobj->owner();

    // Update frame object's ScriptFrameIter::data pointer.
    if (!frameobj->replaceFrameIterData(cx, iter)) {
      return false;
    }

    // Add the frame object with |to| as key.
    if (!dbg->frames.putNew(to, frameobj)) {
      ReportOutOfMemory(cx);
      return false;
    }

    // Remove the old frame entry after all fallible operations are completed
    // so that an OOM will be able to clean up properly.
    dbg->frames.remove(from);
  }

  // All frames successfuly replaced, cancel the rollback.
  terminateDebuggerFramesOnExit.release();

  MOZ_ASSERT(!DebugAPI::inFrameMaps(from));
  MOZ_ASSERT_IF(!frames.empty(), DebugAPI::inFrameMaps(to));
  return true;
}

/* static */
bool DebugAPI::inFrameMaps(AbstractFramePtr frame) {
  bool foundAny = false;
  Debugger::forEachOnStackDebuggerFrame(
      frame, [&](Debugger*, DebuggerFrame* frameobj) { foundAny = true; });
  return foundAny;
}

/* static */
void Debugger::suspendGeneratorDebuggerFrames(JSContext* cx,
                                              AbstractFramePtr frame) {
  JSFreeOp* fop = cx->runtime()->defaultFreeOp();
  forEachOnStackDebuggerFrame(
      frame, [&](Debugger* dbg, DebuggerFrame* dbgFrame) {
        dbg->frames.remove(frame);

#if DEBUG
        MOZ_ASSERT(dbgFrame->hasGeneratorInfo());
        AbstractGeneratorObject& genObj = dbgFrame->unwrappedGenerator();
        GeneratorWeakMap::Ptr p = dbg->generatorFrames.lookup(&genObj);
        MOZ_ASSERT(p);
        MOZ_ASSERT(p->value() == dbgFrame);
#endif

        dbgFrame->suspend(fop);
      });
}

/* static */
void Debugger::terminateDebuggerFrames(JSContext* cx, AbstractFramePtr frame) {
  JSFreeOp* fop = cx->runtime()->defaultFreeOp();

  forEachOnStackOrSuspendedDebuggerFrame(
      cx, frame, [&](Debugger* dbg, DebuggerFrame* dbgFrame) {
        Debugger::terminateDebuggerFrame(fop, dbg, dbgFrame, frame);
      });

  // If this is an eval frame, then from the debugger's perspective the
  // script is about to be destroyed. Remove any breakpoints in it.
  if (frame.isEvalFrame()) {
    RootedScript script(cx, frame.script());
    DebugScript::clearBreakpointsIn(cx->runtime()->defaultFreeOp(), script,
                                    nullptr, nullptr);
  }
}

/* static */
void Debugger::terminateDebuggerFrame(
    JSFreeOp* fop, Debugger* dbg, DebuggerFrame* dbgFrame,
    AbstractFramePtr frame, FrameMap::Enum* maybeFramesEnum,
    GeneratorWeakMap::Enum* maybeGeneratorFramesEnum) {
  // If we were not passed the frame, either we are destroying a frame early
  // on before it was inserted into the "frames" list, or else we are
  // terminating a frame from "generatorFrames" and the "frames" entries will
  // be cleaned up later on with a second call to this function.
  MOZ_ASSERT_IF(!frame, !maybeFramesEnum);
  MOZ_ASSERT_IF(!frame, dbgFrame->hasGeneratorInfo());
  MOZ_ASSERT_IF(!dbgFrame->hasGeneratorInfo(), !maybeGeneratorFramesEnum);

  if (frame) {
    if (maybeFramesEnum) {
      maybeFramesEnum->removeFront();
    } else {
      dbg->frames.remove(frame);
    }
  }

  if (dbgFrame->hasGeneratorInfo()) {
    if (maybeGeneratorFramesEnum) {
      maybeGeneratorFramesEnum->removeFront();
    } else {
      dbg->generatorFrames.remove(&dbgFrame->unwrappedGenerator());
    }
  }

  dbgFrame->terminate(fop, frame);
}

DebuggerDebuggeeLink* Debugger::getDebuggeeLink() {
  return &object->getReservedSlot(JSSLOT_DEBUG_DEBUGGEE_LINK)
              .toObject()
              .as<DebuggerDebuggeeLink>();
}

void DebuggerDebuggeeLink::setLinkSlot(Debugger& dbg) {
  setReservedSlot(DEBUGGER_LINK_SLOT, ObjectValue(*dbg.toJSObject()));
}

void DebuggerDebuggeeLink::clearLinkSlot() {
  setReservedSlot(DEBUGGER_LINK_SLOT, UndefinedValue());
}

const JSClass DebuggerDebuggeeLink::class_ = {
    "DebuggerDebuggeeLink", JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS)};

/* static */
bool DebugAPI::handleBaselineOsr(JSContext* cx, InterpreterFrame* from,
                                 jit::BaselineFrame* to) {
  ScriptFrameIter iter(cx);
  MOZ_ASSERT(iter.abstractFramePtr() == to);
  return Debugger::replaceFrameGuts(cx, from, to, iter);
}

/* static */
bool DebugAPI::handleIonBailout(JSContext* cx, jit::RematerializedFrame* from,
                                jit::BaselineFrame* to) {
  // When we return to a bailed-out Ion real frame, we must update all
  // Debugger.Frames that refer to its inline frames. However, since we
  // can't pop individual inline frames off the stack (we can only pop the
  // real frame that contains them all, as a unit), we cannot assume that
  // the frame we're dealing with is the top frame. Advance the iterator
  // across any inlined frames younger than |to|, the baseline frame
  // reconstructed during bailout from the Ion frame corresponding to
  // |from|.
  ScriptFrameIter iter(cx);
  while (iter.abstractFramePtr() != to) {
    ++iter;
  }
  return Debugger::replaceFrameGuts(cx, from, to, iter);
}

/* static */
void DebugAPI::handleUnrecoverableIonBailoutError(
    JSContext* cx, jit::RematerializedFrame* frame) {
  // Ion bailout can fail due to overrecursion. In such cases we cannot
  // honor any further Debugger hooks on the frame, and need to ensure that
  // its Debugger.Frame entry is cleaned up.
  Debugger::terminateDebuggerFrames(cx, frame);
}

/*** JS::dbg::Builder *******************************************************/

Builder::Builder(JSContext* cx, js::Debugger* debugger)
    : debuggerObject(cx, debugger->toJSObject().get()), debugger(debugger) {}

#if DEBUG
void Builder::assertBuilt(JSObject* obj) {
  // We can't use assertSameCompartment here, because that is always keyed to
  // some JSContext's current compartment, whereas BuiltThings can be
  // constructed and assigned to without respect to any particular context;
  // the only constraint is that they should be in their debugger's compartment.
  MOZ_ASSERT_IF(obj, debuggerObject->compartment() == obj->compartment());
}
#endif

bool Builder::Object::definePropertyToTrusted(JSContext* cx, const char* name,
                                              JS::MutableHandleValue trusted) {
  // We should have checked for false Objects before calling this.
  MOZ_ASSERT(value);

  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));

  return DefineDataProperty(cx, value, id, trusted);
}

bool Builder::Object::defineProperty(JSContext* cx, const char* name,
                                     JS::HandleValue propval_) {
  AutoRealm ar(cx, debuggerObject());

  RootedValue propval(cx, propval_);
  if (!debugger()->wrapDebuggeeValue(cx, &propval)) {
    return false;
  }

  return definePropertyToTrusted(cx, name, &propval);
}

bool Builder::Object::defineProperty(JSContext* cx, const char* name,
                                     JS::HandleObject propval_) {
  RootedValue propval(cx, ObjectOrNullValue(propval_));
  return defineProperty(cx, name, propval);
}

bool Builder::Object::defineProperty(JSContext* cx, const char* name,
                                     Builder::Object& propval_) {
  AutoRealm ar(cx, debuggerObject());

  RootedValue propval(cx, ObjectOrNullValue(propval_.value));
  return definePropertyToTrusted(cx, name, &propval);
}

Builder::Object Builder::newObject(JSContext* cx) {
  AutoRealm ar(cx, debuggerObject);

  RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));

  // If the allocation failed, this will return a false Object, as the spec
  // promises.
  return Object(cx, *this, obj);
}

/*** JS::dbg::AutoEntryMonitor **********************************************/

AutoEntryMonitor::AutoEntryMonitor(JSContext* cx)
    : cx_(cx), savedMonitor_(cx->entryMonitor) {
  cx->entryMonitor = this;
}

AutoEntryMonitor::~AutoEntryMonitor() { cx_->entryMonitor = savedMonitor_; }

/*** Glue *******************************************************************/

extern JS_PUBLIC_API bool JS_DefineDebuggerObject(JSContext* cx,
                                                  HandleObject obj) {
  RootedNativeObject debugCtor(cx), debugProto(cx), frameProto(cx),
      scriptProto(cx), sourceProto(cx), objectProto(cx), envProto(cx),
      memoryProto(cx);
  RootedObject debuggeeWouldRunProto(cx);
  RootedValue debuggeeWouldRunCtor(cx);
  Handle<GlobalObject*> global = obj.as<GlobalObject>();

  debugProto =
      InitClass(cx, global, nullptr, &DebuggerInstanceObject::class_,
                Debugger::construct, 1, Debugger::properties, Debugger::methods,
                nullptr, Debugger::static_methods, debugCtor.address());
  if (!debugProto) {
    return false;
  }

  frameProto = DebuggerFrame::initClass(cx, global, debugCtor);
  if (!frameProto) {
    return false;
  }

  scriptProto = DebuggerScript::initClass(cx, global, debugCtor);
  if (!scriptProto) {
    return false;
  }

  sourceProto = DebuggerSource::initClass(cx, global, debugCtor);
  if (!sourceProto) {
    return false;
  }

  objectProto = DebuggerObject::initClass(cx, global, debugCtor);
  if (!objectProto) {
    return false;
  }

  envProto = DebuggerEnvironment::initClass(cx, global, debugCtor);
  if (!envProto) {
    return false;
  }

  memoryProto =
      InitClass(cx, debugCtor, nullptr, &DebuggerMemory::class_,
                DebuggerMemory::construct, 0, DebuggerMemory::properties,
                DebuggerMemory::methods, nullptr, nullptr);
  if (!memoryProto) {
    return false;
  }

  debuggeeWouldRunProto = GlobalObject::getOrCreateCustomErrorPrototype(
      cx, global, JSEXN_DEBUGGEEWOULDRUN);
  if (!debuggeeWouldRunProto) {
    return false;
  }
  debuggeeWouldRunCtor = global->getConstructor(JSProto_DebuggeeWouldRun);
  RootedId debuggeeWouldRunId(
      cx, NameToId(ClassName(JSProto_DebuggeeWouldRun, cx)));
  if (!DefineDataProperty(cx, debugCtor, debuggeeWouldRunId,
                          debuggeeWouldRunCtor, 0)) {
    return false;
  }

  debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_FRAME_PROTO,
                              ObjectValue(*frameProto));
  debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_OBJECT_PROTO,
                              ObjectValue(*objectProto));
  debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_SCRIPT_PROTO,
                              ObjectValue(*scriptProto));
  debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_SOURCE_PROTO,
                              ObjectValue(*sourceProto));
  debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_ENV_PROTO,
                              ObjectValue(*envProto));
  debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_MEMORY_PROTO,
                              ObjectValue(*memoryProto));
  return true;
}

JS_PUBLIC_API bool JS::dbg::IsDebugger(JSObject& obj) {
  /* We only care about debugger objects, so CheckedUnwrapStatic is OK. */
  JSObject* unwrapped = CheckedUnwrapStatic(&obj);
  return unwrapped && unwrapped->is<DebuggerInstanceObject>() &&
         js::Debugger::fromJSObject(unwrapped) != nullptr;
}

JS_PUBLIC_API bool JS::dbg::GetDebuggeeGlobals(
    JSContext* cx, JSObject& dbgObj, MutableHandleObjectVector vector) {
  MOZ_ASSERT(IsDebugger(dbgObj));
  /* Since we know we have a debugger object, CheckedUnwrapStatic is fine. */
  js::Debugger* dbg = js::Debugger::fromJSObject(CheckedUnwrapStatic(&dbgObj));

  if (!vector.reserve(vector.length() + dbg->debuggees.count())) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  for (WeakGlobalObjectSet::Range r = dbg->allDebuggees(); !r.empty();
       r.popFront()) {
    vector.infallibleAppend(static_cast<JSObject*>(r.front()));
  }

  return true;
}

#ifdef DEBUG
/* static */
bool Debugger::isDebuggerCrossCompartmentEdge(JSObject* obj,
                                              const gc::Cell* target) {
  MOZ_ASSERT(target);

  const gc::Cell* referent = nullptr;
  if (obj->is<DebuggerScript>()) {
    referent = obj->as<DebuggerScript>().getReferentCell();
  } else if (obj->is<DebuggerSource>()) {
    referent = obj->as<DebuggerSource>().getReferentRawObject();
  } else if (obj->is<DebuggerObject>()) {
    referent = obj->as<DebuggerObject>().referent();
  } else if (obj->is<DebuggerEnvironment>()) {
    referent = obj->as<DebuggerEnvironment>().referent();
  }

  return referent == target;
}

static void CheckDebuggeeThingRealm(Realm* realm, bool invisibleOk) {
  MOZ_ASSERT(!realm->creationOptions().mergeable());
  MOZ_ASSERT_IF(!invisibleOk, !realm->creationOptions().invisibleToDebugger());
}

void js::CheckDebuggeeThing(BaseScript* script, bool invisibleOk) {
  CheckDebuggeeThingRealm(script->realm(), invisibleOk);
}

void js::CheckDebuggeeThing(JSObject* obj, bool invisibleOk) {
  if (Realm* realm = JS::GetObjectRealmOrNull(obj)) {
    CheckDebuggeeThingRealm(realm, invisibleOk);
  }
}
#endif  // DEBUG

/*** JS::dbg::GarbageCollectionEvent ****************************************/

namespace JS {
namespace dbg {

/* static */ GarbageCollectionEvent::Ptr GarbageCollectionEvent::Create(
    JSRuntime* rt, ::js::gcstats::Statistics& stats, uint64_t gcNumber) {
  auto data = MakeUnique<GarbageCollectionEvent>(gcNumber);
  if (!data) {
    return nullptr;
  }

  data->nonincrementalReason = stats.nonincrementalReason();

  for (auto& slice : stats.slices()) {
    if (!data->reason) {
      // There is only one GC reason for the whole cycle, but for legacy
      // reasons this data is stored and replicated on each slice. Each
      // slice used to have its own GCReason, but now they are all the
      // same.
      data->reason = ExplainGCReason(slice.reason);
      MOZ_ASSERT(data->reason);
    }

    if (!data->collections.growBy(1)) {
      return nullptr;
    }

    data->collections.back().startTimestamp = slice.start;
    data->collections.back().endTimestamp = slice.end;
  }

  return data;
}

static bool DefineStringProperty(JSContext* cx, HandleObject obj,
                                 PropertyName* propName, const char* strVal) {
  RootedValue val(cx, UndefinedValue());
  if (strVal) {
    JSAtom* atomized = Atomize(cx, strVal, strlen(strVal));
    if (!atomized) {
      return false;
    }
    val = StringValue(atomized);
  }
  return DefineDataProperty(cx, obj, propName, val);
}

JSObject* GarbageCollectionEvent::toJSObject(JSContext* cx) const {
  RootedObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
  RootedValue gcCycleNumberVal(cx, NumberValue(majorGCNumber_));
  if (!obj ||
      !DefineStringProperty(cx, obj, cx->names().nonincrementalReason,
                            nonincrementalReason) ||
      !DefineStringProperty(cx, obj, cx->names().reason, reason) ||
      !DefineDataProperty(cx, obj, cx->names().gcCycleNumber,
                          gcCycleNumberVal)) {
    return nullptr;
  }

  RootedArrayObject slicesArray(cx, NewDenseEmptyArray(cx));
  if (!slicesArray) {
    return nullptr;
  }

  TimeStamp originTime = TimeStamp::ProcessCreation();

  size_t idx = 0;
  for (auto range = collections.all(); !range.empty(); range.popFront()) {
    RootedPlainObject collectionObj(cx,
                                    NewBuiltinClassInstance<PlainObject>(cx));
    if (!collectionObj) {
      return nullptr;
    }

    RootedValue start(cx), end(cx);
    start = NumberValue(
        (range.front().startTimestamp - originTime).ToMilliseconds());
    end =
        NumberValue((range.front().endTimestamp - originTime).ToMilliseconds());
    if (!DefineDataProperty(cx, collectionObj, cx->names().startTimestamp,
                            start) ||
        !DefineDataProperty(cx, collectionObj, cx->names().endTimestamp, end)) {
      return nullptr;
    }

    RootedValue collectionVal(cx, ObjectValue(*collectionObj));
    if (!DefineDataElement(cx, slicesArray, idx++, collectionVal)) {
      return nullptr;
    }
  }

  RootedValue slicesValue(cx, ObjectValue(*slicesArray));
  if (!DefineDataProperty(cx, obj, cx->names().collections, slicesValue)) {
    return nullptr;
  }

  return obj;
}

JS_PUBLIC_API bool FireOnGarbageCollectionHookRequired(JSContext* cx) {
  AutoCheckCannotGC noGC;

  for (auto& dbg : cx->runtime()->onGarbageCollectionWatchers()) {
    MOZ_ASSERT(dbg.getHook(Debugger::OnGarbageCollection));
    if (dbg.observedGC(cx->runtime()->gc.majorGCCount())) {
      return true;
    }
  }

  return false;
}

JS_PUBLIC_API bool FireOnGarbageCollectionHook(
    JSContext* cx, JS::dbg::GarbageCollectionEvent::Ptr&& data) {
  RootedObjectVector triggered(cx);

  {
    // We had better not GC (and potentially get a dangling Debugger
    // pointer) while finding all Debuggers observing a debuggee that
    // participated in this GC.
    AutoCheckCannotGC noGC;

    for (auto& dbg : cx->runtime()->onGarbageCollectionWatchers()) {
      MOZ_ASSERT(dbg.getHook(Debugger::OnGarbageCollection));
      if (dbg.observedGC(data->majorGCNumber())) {
        if (!triggered.append(dbg.object)) {
          JS_ReportOutOfMemory(cx);
          return false;
        }
      }
    }
  }

  for (; !triggered.empty(); triggered.popBack()) {
    Debugger* dbg = Debugger::fromJSObject(triggered.back());

    if (dbg->getHook(Debugger::OnGarbageCollection)) {
      (void)dbg->enterDebuggerHook(cx, [&]() -> bool {
        return dbg->fireOnGarbageCollectionHook(cx, data);
      });
      MOZ_ASSERT(!cx->isExceptionPending());
    }
  }

  return true;
}

}  // namespace dbg
}  // namespace JS
