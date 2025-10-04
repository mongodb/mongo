/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef MOZ_VALGRIND
#  include <valgrind/memcheck.h>
#endif

#include "jstypes.h"

#include "debugger/DebugAPI.h"
#include "gc/ClearEdgesTracer.h"
#include "gc/GCInternals.h"
#include "gc/PublicIterators.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "js/ValueArray.h"
#include "vm/BigIntType.h"
#include "vm/Compartment.h"
#include "vm/HelperThreadState.h"
#include "vm/JSContext.h"

using namespace js;
using namespace js::gc;

using mozilla::LinkedList;

using JS::AutoGCRooter;

using RootRange = RootedValueMap::Range;
using RootEntry = RootedValueMap::Entry;
using RootEnum = RootedValueMap::Enum;

template <typename Base, typename T>
inline void TypedRootedGCThingBase<Base, T>::trace(JSTracer* trc,
                                                   const char* name) {
  auto* self = this->template derived<T>();
  TraceNullableRoot(trc, self->address(), name);
}

template <typename T>
static inline void TraceExactStackRootList(JSTracer* trc,
                                           StackRootedBase* listHead,
                                           const char* name) {
  // Check size of Rooted<T> does not increase.
  static_assert(sizeof(Rooted<T>) == sizeof(T) + 2 * sizeof(uintptr_t));

  for (StackRootedBase* root = listHead; root; root = root->previous()) {
    static_cast<Rooted<T>*>(root)->trace(trc, name);
  }
}

static inline void TraceExactStackRootTraceableList(JSTracer* trc,
                                                    StackRootedBase* listHead,
                                                    const char* name) {
  for (StackRootedBase* root = listHead; root; root = root->previous()) {
    static_cast<StackRootedTraceableBase*>(root)->trace(trc, name);
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

  TraceExactStackRootTraceableList(trc, stackRoots[JS::RootKind::Traceable],
                                   "Traceable");
}

void JS::RootingContext::traceStackRoots(JSTracer* trc) {
  TraceStackRoots(trc, stackRoots_);
}

static void TraceExactStackRoots(JSContext* cx, JSTracer* trc) {
  cx->traceStackRoots(trc);
}

template <typename T>
static inline void TracePersistentRootedList(
    JSTracer* trc, LinkedList<PersistentRootedBase>& list, const char* name) {
  for (PersistentRootedBase* root : list) {
    static_cast<PersistentRooted<T>*>(root)->trace(trc, name);
  }
}

static inline void TracePersistentRootedTraceableList(
    JSTracer* trc, LinkedList<PersistentRootedBase>& list, const char* name) {
  for (PersistentRootedBase* root : list) {
    static_cast<PersistentRootedTraceableBase*>(root)->trace(trc, name);
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

  TracePersistentRootedTraceableList(
      trc, heapRoots.ref()[JS::RootKind::Traceable], "persistent-traceable");
}

static void TracePersistentRooted(JSRuntime* rt, JSTracer* trc) {
  rt->tracePersistentRoots(trc);
}

template <typename T>
static void FinishPersistentRootedChain(
    LinkedList<PersistentRootedBase>& list) {
  while (!list.isEmpty()) {
    static_cast<PersistentRooted<T>*>(list.getFirst())->reset();
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
  if (atomsZone()->isGCMarking()) {
    traceRuntimeAtoms(trc);
  }

  {
    // Trace incoming cross compartment edges from uncollected compartments,
    // skipping gray edges which are traced later.
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_CCWS);
    Compartment::traceIncomingCrossCompartmentEdgesForZoneGC(
        trc, Compartment::NonGrayEdges);
  }

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

  traceRuntimeAtoms(trc);
  traceRuntimeCommon(trc, TraceRuntime);
}

void js::gc::GCRuntime::traceRuntimeAtoms(JSTracer* trc) {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_RUNTIME_DATA);
  TraceAtoms(trc);
  jit::JitRuntime::TraceAtomZoneRoots(trc);
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

  if (!JS::RuntimeHeapIsMinorCollecting()) {
    // Trace the self-hosting stencil. The contents of this are always tenured.
    rt->traceSelfHostingStencil(trc);

    for (ZonesIter zone(this, ZoneSelector::SkipAtoms); !zone.done();
         zone.next()) {
      zone->traceRootsInMajorGC(trc);
    }

    // Trace interpreter entry code generated with --emit-interpreter-entry
    if (rt->hasJitRuntime() && rt->jitRuntime()->hasInterpreterEntryMap()) {
      rt->jitRuntime()->getInterpreterEntryMap()->traceTrampolineCode(trc);
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

  for (const auto& callback : blackRootTracers.ref()) {
    (*callback.op)(trc, callback.data);
  }
}

void GCRuntime::traceEmbeddingGrayRoots(JSTracer* trc) {
  SliceBudget budget = SliceBudget::unlimited();
  MOZ_ALWAYS_TRUE(traceEmbeddingGrayRoots(trc, budget) == Finished);
}

IncrementalProgress GCRuntime::traceEmbeddingGrayRoots(JSTracer* trc,
                                                       SliceBudget& budget) {
  // The analysis doesn't like the function pointer below.
  JS::AutoSuppressGCAnalysis nogc;

  const auto& callback = grayRootTracer.ref();
  if (!callback.op) {
    return Finished;
  }

  return callback.op(trc, budget, callback.data) ? Finished : NotFinished;
}

#ifdef DEBUG
class AssertNoRootsTracer final : public JS::CallbackTracer {
  void onChild(JS::GCCellPtr thing, const char* name) override {
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

  rt->finishAtoms();
  restoreSharedAtomsZone();

  rootsHash.ref().clear();

  rt->finishPersistentRoots();

  rt->finishSelfHosting();

  for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
    zone->finishRoots();
  }

#ifdef JS_GC_ZEAL
  clearSelectedForMarking();
#endif

  // Clear out the interpreter entry map before the final gc.
  ClearInterpreterEntryMap(rt);

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

JS_PUBLIC_API void JS::AddPersistentRoot(JS::RootingContext* cx, RootKind kind,
                                         PersistentRootedBase* root) {
  JSRuntime* rt = static_cast<JSContext*>(cx)->runtime();
  rt->heapRoots.ref()[kind].insertBack(root);
}

JS_PUBLIC_API void JS::AddPersistentRoot(JSRuntime* rt, RootKind kind,
                                         PersistentRootedBase* root) {
  rt->heapRoots.ref()[kind].insertBack(root);
}
