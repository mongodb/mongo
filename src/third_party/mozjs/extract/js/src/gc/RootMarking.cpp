/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef MOZ_VALGRIND
#  include <valgrind/memcheck.h>
#endif

#include "jstypes.h"

#include "builtin/MapObject.h"
#include "debugger/DebugAPI.h"
#include "frontend/BytecodeCompiler.h"
#include "frontend/Parser.h"
#include "gc/ClearEdgesTracer.h"
#include "gc/GCInternals.h"
#include "gc/Marking.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "js/HashTable.h"
#include "js/ValueArray.h"
#include "vm/HelperThreadState.h"
#include "vm/JSContext.h"
#include "vm/JSONParser.h"

#include "gc/Nursery-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::LinkedList;

using JS::AutoGCRooter;

using RootRange = RootedValueMap::Range;
using RootEntry = RootedValueMap::Entry;
using RootEnum = RootedValueMap::Enum;

// For more detail see JS::Rooted::root and js::RootedTraceable.
//
// The JS::RootKind::Traceable list contains a bunch of totally disparate types,
// but to refer to this list we need /something/ in the type field. We use the
// following type as a compatible stand-in. No actual methods from
// ConcreteTraceable type are actually used at runtime.
struct ConcreteTraceable {
  ConcreteTraceable() = delete;
  void trace(JSTracer*) { MOZ_CRASH("This path is unreachable."); }
};

template <typename T>
inline void RootedGCThingTraits<T>::trace(JSTracer* trc, T* thingp,
                                          const char* name) {
  TraceNullableRoot(trc, thingp, name);
}

template <typename T>
inline void RootedTraceableTraits<T>::trace(JSTracer* trc,
                                            VirtualTraceable* thingp,
                                            const char* name) {
  thingp->trace(trc, name);
}

template <typename T>
inline void JS::Rooted<T>::trace(JSTracer* trc, const char* name) {
  PtrTraits::trace(trc, &ptr, name);
}

template <typename T>
inline void JS::PersistentRooted<T>::trace(JSTracer* trc, const char* name) {
  PtrTraits::trace(trc, &ptr, name);
}

template <typename T>
static inline void TraceExactStackRootList(
    JSTracer* trc, JS::Rooted<JS::detail::RootListEntry*>* listHead,
    const char* name) {
  auto* typedList = reinterpret_cast<JS::Rooted<T>*>(listHead);
  for (JS::Rooted<T>* root = typedList; root; root = root->previous()) {
    root->trace(trc, name);
  }
}

static inline void TraceStackRoots(JSTracer* trc,
                                   JS::RootedListHeads& stackRoots) {
#define TRACE_ROOTS(name, type, _, _1)                                \
  TraceExactStackRootList<type*>(trc, stackRoots[JS::RootKind::name], \
                                 "exact-" #name);
  JS_FOR_EACH_TRACEKIND(TRACE_ROOTS)
#undef TRACE_ROOTS
  TraceExactStackRootList<jsid>(trc, stackRoots[JS::RootKind::Id], "exact-id");
  TraceExactStackRootList<Value>(trc, stackRoots[JS::RootKind::Value],
                                 "exact-value");

  // RootedTraceable uses virtual dispatch.
  JS::AutoSuppressGCAnalysis nogc;

  TraceExactStackRootList<ConcreteTraceable>(
      trc, stackRoots[JS::RootKind::Traceable], "Traceable");
}

void JS::RootingContext::traceStackRoots(JSTracer* trc) {
  TraceStackRoots(trc, stackRoots_);
}

static void TraceExactStackRoots(JSContext* cx, JSTracer* trc) {
  cx->traceStackRoots(trc);
}

template <typename T>
static inline void TracePersistentRootedList(
    JSTracer* trc,
    LinkedList<PersistentRooted<JS::detail::RootListEntry*>>& list,
    const char* name) {
  auto& typedList = reinterpret_cast<LinkedList<PersistentRooted<T>>&>(list);
  for (PersistentRooted<T>* root : typedList) {
    root->trace(trc, name);
  }
}

void JSRuntime::tracePersistentRoots(JSTracer* trc) {
#define TRACE_ROOTS(name, type, _, _1)                                       \
  TracePersistentRootedList<type*>(trc, heapRoots.ref()[JS::RootKind::name], \
                                   "persistent-" #name);
  JS_FOR_EACH_TRACEKIND(TRACE_ROOTS)
#undef TRACE_ROOTS
  TracePersistentRootedList<jsid>(trc, heapRoots.ref()[JS::RootKind::Id],
                                  "persistent-id");
  TracePersistentRootedList<Value>(trc, heapRoots.ref()[JS::RootKind::Value],
                                   "persistent-value");

  // RootedTraceable uses virtual dispatch.
  JS::AutoSuppressGCAnalysis nogc;

  TracePersistentRootedList<ConcreteTraceable>(
      trc, heapRoots.ref()[JS::RootKind::Traceable], "persistent-traceable");
}

static void TracePersistentRooted(JSRuntime* rt, JSTracer* trc) {
  rt->tracePersistentRoots(trc);
}

template <typename T>
static void FinishPersistentRootedChain(
    LinkedList<PersistentRooted<JS::detail::RootListEntry*>>& listArg) {
  auto& list = reinterpret_cast<LinkedList<PersistentRooted<T>>&>(listArg);
  while (!list.isEmpty()) {
    list.getFirst()->reset();
  }
}

void JSRuntime::finishPersistentRoots() {
#define FINISH_ROOT_LIST(name, type, _, _1) \
  FinishPersistentRootedChain<type*>(heapRoots.ref()[JS::RootKind::name]);
  JS_FOR_EACH_TRACEKIND(FINISH_ROOT_LIST)
#undef FINISH_ROOT_LIST
  FinishPersistentRootedChain<jsid>(heapRoots.ref()[JS::RootKind::Id]);
  FinishPersistentRootedChain<Value>(heapRoots.ref()[JS::RootKind::Value]);

  // Note that we do not finalize the Traceable list as we do not know how to
  // safely clear members. We instead assert that none escape the RootLists.
  // See the comment on RootLists::~RootLists for details.
}

JS_PUBLIC_API void js::TraceValueArray(JSTracer* trc, size_t length,
                                       Value* elements) {
  TraceRootRange(trc, length, elements, "JS::RootedValueArray");
}

void AutoGCRooter::trace(JSTracer* trc) {
  switch (kind_) {
    case Kind::Wrapper:
      static_cast<AutoWrapperRooter*>(this)->trace(trc);
      break;

    case Kind::WrapperVector:
      static_cast<AutoWrapperVector*>(this)->trace(trc);
      break;

    case Kind::Custom:
      static_cast<JS::CustomAutoRooter*>(this)->trace(trc);
      break;

    default:
      MOZ_CRASH("Bad AutoGCRooter::Kind");
      break;
  }
}

void AutoWrapperRooter::trace(JSTracer* trc) {
  /*
   * We need to use TraceManuallyBarrieredEdge here because we trace wrapper
   * roots in every slice. This is because of some rule-breaking in
   * RemapAllWrappersForObject; see comment there.
   */
  TraceManuallyBarrieredEdge(trc, &value.get(), "js::AutoWrapperRooter.value");
}

void AutoWrapperVector::trace(JSTracer* trc) {
  /*
   * We need to use TraceManuallyBarrieredEdge here because we trace wrapper
   * roots in every slice. This is because of some rule-breaking in
   * RemapAllWrappersForObject; see comment there.
   */
  for (WrapperValue& value : *this) {
    TraceManuallyBarrieredEdge(trc, &value.get(),
                               "js::AutoWrapperVector.vector");
  }
}

void JS::RootingContext::traceAllGCRooters(JSTracer* trc) {
  for (AutoGCRooter* list : autoGCRooters_) {
    traceGCRooterList(trc, list);
  }
}

void JS::RootingContext::traceWrapperGCRooters(JSTracer* trc) {
  traceGCRooterList(trc, autoGCRooters_[AutoGCRooter::Kind::Wrapper]);
  traceGCRooterList(trc, autoGCRooters_[AutoGCRooter::Kind::WrapperVector]);
}

/* static */
inline void JS::RootingContext::traceGCRooterList(JSTracer* trc,
                                                  AutoGCRooter* head) {
  for (AutoGCRooter* rooter = head; rooter; rooter = rooter->down) {
    rooter->trace(trc);
  }
}

void PropertyDescriptor::trace(JSTracer* trc) {
  TraceRoot(trc, &value_, "Descriptor::value");
  if (getter_) {
    TraceRoot(trc, &getter_, "Descriptor::getter");
  }
  if (setter_) {
    TraceRoot(trc, &setter_, "Descriptor::setter");
  }
}

void js::gc::GCRuntime::traceRuntimeForMajorGC(JSTracer* trc,
                                               AutoGCSession& session) {
  MOZ_ASSERT(!TlsContext.get()->suppressGC);

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_ROOTS);

  // We only need to trace atoms when we're marking; atoms are never moved by
  // compacting GC.
  if (atomsZone->isGCMarking()) {
    traceRuntimeAtoms(trc, session.checkAtomsAccess());
  }

  {
    // Trace incoming cross compartment edges from uncollected compartments,
    // skipping gray edges which are traced later.
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_CCWS);
    Compartment::traceIncomingCrossCompartmentEdgesForZoneGC(
        trc, Compartment::NonGrayEdges);
  }

  markFinalizationRegistryRoots(trc);

  traceRuntimeCommon(trc, MarkRuntime);
}

void js::gc::GCRuntime::traceRuntimeForMinorGC(JSTracer* trc,
                                               AutoGCSession& session) {
  MOZ_ASSERT(!TlsContext.get()->suppressGC);

  // Note that we *must* trace the runtime during the SHUTDOWN_GC's minor GC
  // despite having called FinishRoots already. This is because FinishRoots
  // does not clear the crossCompartmentWrapper map. It cannot do this
  // because Proxy's trace for CrossCompartmentWrappers asserts presence in
  // the map. And we can reach its trace function despite having finished the
  // roots via the edges stored by the pre-barrier verifier when we finish
  // the verifier for the last time.
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_ROOTS);

  traceRuntimeCommon(trc, TraceRuntime);
}

void js::TraceRuntime(JSTracer* trc) {
  MOZ_ASSERT(!trc->isMarkingTracer());

  JSRuntime* rt = trc->runtime();
  AutoEmptyNurseryAndPrepareForTracing prep(rt->mainContextFromOwnThread());
  gcstats::AutoPhase ap(rt->gc.stats(), gcstats::PhaseKind::TRACE_HEAP);
  rt->gc.traceRuntime(trc, prep);
}

void js::TraceRuntimeWithoutEviction(JSTracer* trc) {
  MOZ_ASSERT(!trc->isMarkingTracer());

  JSRuntime* rt = trc->runtime();
  AutoTraceSession session(rt);
  gcstats::AutoPhase ap(rt->gc.stats(), gcstats::PhaseKind::TRACE_HEAP);
  rt->gc.traceRuntime(trc, session);
}

void js::gc::GCRuntime::traceRuntime(JSTracer* trc, AutoTraceSession& session) {
  MOZ_ASSERT(!rt->isBeingDestroyed());

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_ROOTS);

  traceRuntimeAtoms(trc, session);
  traceRuntimeCommon(trc, TraceRuntime);
}

void js::gc::GCRuntime::traceRuntimeAtoms(JSTracer* trc,
                                          const AutoAccessAtomsZone& access) {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_RUNTIME_DATA);
  rt->tracePermanentAtoms(trc);
  TraceAtoms(trc, access);
  TraceWellKnownSymbols(trc);
  jit::JitRuntime::TraceAtomZoneRoots(trc, access);
}

void js::gc::GCRuntime::traceRuntimeCommon(JSTracer* trc,
                                           TraceOrMarkRuntime traceOrMark) {
  {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_STACK);

    JSContext* cx = rt->mainContextFromOwnThread();

    // Trace active interpreter and JIT stack roots.
    TraceInterpreterActivations(cx, trc);
    jit::TraceJitActivations(cx, trc);

    // Trace legacy C stack roots.
    cx->traceAllGCRooters(trc);

    // Trace C stack roots.
    TraceExactStackRoots(cx, trc);

    for (RootRange r = rootsHash.ref().all(); !r.empty(); r.popFront()) {
      const RootEntry& entry = r.front();
      TraceRoot(trc, entry.key(), entry.value());
    }
  }

  // Trace runtime global roots.
  TracePersistentRooted(rt, trc);

  // Trace the self-hosting global compartment.
  rt->traceSelfHostingGlobal(trc);

#ifdef JS_HAS_INTL_API
  // Trace the shared Intl data.
  rt->traceSharedIntlData(trc);
#endif

  // Trace the JSContext.
  rt->mainContextFromOwnThread()->trace(trc);

  // Trace all realm roots, but not the realm itself; it is traced via the
  // parent pointer if traceRoots actually traces anything.
  for (RealmsIter r(rt); !r.done(); r.next()) {
    r->traceRoots(trc, traceOrMark);
  }

  // Trace zone script-table roots. See comment in
  // Zone::traceScriptTableRoots() for justification re: calling this only
  // during major (non-nursery) collections.
  if (!JS::RuntimeHeapIsMinorCollecting()) {
    for (ZonesIter zone(this, ZoneSelector::SkipAtoms); !zone.done();
         zone.next()) {
      zone->traceScriptTableRoots(trc);
    }
  }

  // Trace helper thread roots.
  HelperThreadState().trace(trc);

  // Trace Debugger.Frames that have live hooks, since dropping them would be
  // observable. In effect, they are rooted by the stack frames.
  DebugAPI::traceFramesWithLiveHooks(trc);

  // Trace the embedding's black and gray roots.
  if (!JS::RuntimeHeapIsMinorCollecting()) {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_EMBEDDING);

    /*
     * The embedding can register additional roots here.
     *
     * We don't need to trace these in a minor GC because all pointers into
     * the nursery should be in the store buffer, and we want to avoid the
     * time taken to trace all these roots.
     */
    traceEmbeddingBlackRoots(trc);

    /* During GC, we don't trace gray roots at this stage. */
    if (traceOrMark == TraceRuntime) {
      traceEmbeddingGrayRoots(trc);
    }
  }

  traceKeptObjects(trc);
}

void GCRuntime::traceEmbeddingBlackRoots(JSTracer* trc) {
  // The analysis doesn't like the function pointer below.
  JS::AutoSuppressGCAnalysis nogc;

  for (size_t i = 0; i < blackRootTracers.ref().length(); i++) {
    const Callback<JSTraceDataOp>& e = blackRootTracers.ref()[i];
    (*e.op)(trc, e.data);
  }
}

void GCRuntime::traceEmbeddingGrayRoots(JSTracer* trc) {
  // The analysis doesn't like the function pointer below.
  JS::AutoSuppressGCAnalysis nogc;

  const auto& callback = grayRootTracer.ref();
  if (JSTraceDataOp op = callback.op) {
    (*op)(trc, callback.data);
  }
}

#ifdef DEBUG
class AssertNoRootsTracer final : public JS::CallbackTracer {
  void onChild(const JS::GCCellPtr& thing) override {
    MOZ_CRASH("There should not be any roots during runtime shutdown");
  }

 public:
  // This skips tracking WeakMap entries because they are not roots.
  explicit AssertNoRootsTracer(JSRuntime* rt)
      : JS::CallbackTracer(rt, JS::TracerKind::Callback,
                           JS::WeakMapTraceAction::Skip) {}
};
#endif  // DEBUG

void js::gc::GCRuntime::finishRoots() {
  AutoNoteSingleThreadedRegion anstr;

  rt->finishParserAtoms();
  rt->finishAtoms();

  rootsHash.ref().clear();

  rt->finishPersistentRoots();

  rt->finishSelfHosting();
  selfHostingZoneFrozen = false;

  for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
    zone->finishRoots();
  }

#ifdef JS_GC_ZEAL
  clearSelectedForMarking();
#endif

  // Clear any remaining roots from the embedding (as otherwise they will be
  // left dangling after we shut down) and remove the callbacks.
  ClearEdgesTracer trc(rt);
  traceEmbeddingBlackRoots(&trc);
  traceEmbeddingGrayRoots(&trc);
  clearBlackAndGrayRootTracers();
}

void js::gc::GCRuntime::checkNoRuntimeRoots(AutoGCSession& session) {
#ifdef DEBUG
  AssertNoRootsTracer trc(rt);
  traceRuntimeForMajorGC(&trc, session);
#endif  // DEBUG
}

// Append traced things to a buffer on the zone for use later in the GC.
// See the comment in GCRuntime.h above grayBufferState for details.
class BufferGrayRootsTracer final : public GenericTracer {
  // Set to false if we OOM while buffering gray roots.
  bool bufferingGrayRootsFailed;

  JSObject* onObjectEdge(JSObject* obj) override { return bufferRoot(obj); }
  JSString* onStringEdge(JSString* string) override {
    return bufferRoot(string);
  }
  js::BaseScript* onScriptEdge(js::BaseScript* script) override {
    return bufferRoot(script);
  }
  JS::Symbol* onSymbolEdge(JS::Symbol* symbol) override {
    return bufferRoot(symbol);
  }
  JS::BigInt* onBigIntEdge(JS::BigInt* bi) override { return bufferRoot(bi); }

  js::Shape* onShapeEdge(js::Shape* shape) override {
    unsupportedEdge();
    return nullptr;
  }
  js::BaseShape* onBaseShapeEdge(js::BaseShape* base) override {
    unsupportedEdge();
    return nullptr;
  }
  js::GetterSetter* onGetterSetterEdge(js::GetterSetter* gs) override {
    unsupportedEdge();
    return nullptr;
  }
  js::PropMap* onPropMapEdge(js::PropMap* map) override {
    unsupportedEdge();
    return nullptr;
  }
  js::jit::JitCode* onJitCodeEdge(js::jit::JitCode* code) override {
    unsupportedEdge();
    return nullptr;
  }
  js::Scope* onScopeEdge(js::Scope* scope) override {
    unsupportedEdge();
    return nullptr;
  }
  js::RegExpShared* onRegExpSharedEdge(js::RegExpShared* shared) override {
    unsupportedEdge();
    return nullptr;
  }

  void unsupportedEdge() { MOZ_CRASH("Unsupported gray root edge kind"); }

  template <typename T>
  inline T* bufferRoot(T* thing);

 public:
  explicit BufferGrayRootsTracer(JSRuntime* rt)
      : GenericTracer(rt, JS::TracerKind::GrayBuffering),
        bufferingGrayRootsFailed(false) {}

  bool failed() const { return bufferingGrayRootsFailed; }
  void setFailed() { bufferingGrayRootsFailed = true; }
};

void js::gc::GCRuntime::bufferGrayRoots() {
  // Precondition: the state has been reset to "unused" after the last GC
  //               and the zone's buffers have been cleared.
  MOZ_ASSERT(grayBufferState == GrayBufferState::Unused);
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    MOZ_ASSERT(zone->gcGrayRoots().IsEmpty());
  }

  BufferGrayRootsTracer grayBufferer(rt);
  traceEmbeddingGrayRoots(&grayBufferer);
  Compartment::traceIncomingCrossCompartmentEdgesForZoneGC(
      &grayBufferer, Compartment::GrayEdges);

  // Propagate the failure flag from the marker to the runtime.
  if (grayBufferer.failed()) {
    grayBufferState = GrayBufferState::Failed;
    resetBufferedGrayRoots();
  } else {
    grayBufferState = GrayBufferState::Okay;
  }
}

template <typename T>
inline T* BufferGrayRootsTracer::bufferRoot(T* thing) {
  MOZ_ASSERT(JS::RuntimeHeapIsBusy());
  MOZ_ASSERT(thing);
  // Check if |thing| is corrupt by calling a method that touches the heap.
  MOZ_ASSERT(thing->getTraceKind() != JS::TraceKind(0xff));

  TenuredCell* tenured = &thing->asTenured();

  // This is run from a helper thread while the mutator is paused so we have
  // to use *FromAnyThread methods here.
  Zone* zone = tenured->zoneFromAnyThread();
  if (zone->isCollectingFromAnyThread()) {
    // See the comment on SetMaybeAliveFlag to see why we only do this for
    // objects and scripts. We rely on gray root buffering for this to work,
    // but we only need to worry about uncollected dead compartments during
    // incremental GCs (when we do gray root buffering).
    SetMaybeAliveFlag(thing);

    if (!zone->gcGrayRoots().Append(tenured)) {
      bufferingGrayRootsFailed = true;
    }
  }

  return thing;
}

void GCRuntime::markBufferedGrayRoots(JS::Zone* zone) {
  MOZ_ASSERT(grayBufferState == GrayBufferState::Okay);
  MOZ_ASSERT(zone->isGCMarkingBlackAndGray() || zone->isGCCompacting());

  auto& roots = zone->gcGrayRoots();
  if (roots.IsEmpty()) {
    return;
  }

  for (auto iter = roots.Iter(); !iter.Done(); iter.Next()) {
    Cell* cell = iter.Get();

    // Bug 1203273: Check for bad pointers on OSX and output diagnostics.
#if defined(XP_DARWIN) && defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED)
    auto addr = uintptr_t(cell);
    if (addr < ChunkSize || addr % CellAlignBytes != 0) {
      MOZ_CRASH_UNSAFE_PRINTF(
          "Bad GC thing pointer in gray root buffer: %p at address %p", cell,
          &iter.Get());
    }
#else
    MOZ_ASSERT(IsCellPointerValid(cell));
#endif

    TraceManuallyBarrieredGenericPointerEdge(&marker, &cell,
                                             "buffered gray root");
  }
}

void GCRuntime::resetBufferedGrayRoots() {
  MOZ_ASSERT(
      grayBufferState != GrayBufferState::Okay,
      "Do not clear the gray buffers unless we are Failed or becoming Unused");
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    zone->gcGrayRoots().Clear();
  }
}

JS_PUBLIC_API void JS::AddPersistentRoot(
    JS::RootingContext* cx, RootKind kind,
    PersistentRooted<JS::detail::RootListEntry*>* root) {
  static_cast<JSContext*>(cx)->runtime()->heapRoots.ref()[kind].insertBack(
      root);
}

JS_PUBLIC_API void JS::AddPersistentRoot(
    JSRuntime* rt, RootKind kind,
    PersistentRooted<JS::detail::RootListEntry*>* root) {
  rt->heapRoots.ref()[kind].insertBack(root);
}
