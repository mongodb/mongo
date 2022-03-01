/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Sprintf.h"

#include <algorithm>
#include <utility>

#ifdef MOZ_VALGRIND
#  include <valgrind/memcheck.h>
#endif

#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/PublicIterators.h"
#include "gc/WeakMap.h"
#include "gc/Zone.h"
#include "js/friend/DumpFunctions.h"  // js::DumpObject
#include "js/HashTable.h"
#include "vm/JSContext.h"

#include "gc/ArenaList-inl.h"
#include "gc/GC-inl.h"
#include "gc/Marking-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/JSContext-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::DebugOnly;

#ifdef DEBUG
bool js::RuntimeIsVerifyingPreBarriers(JSRuntime* runtime) {
#  ifdef JS_GC_ZEAL
  return runtime->gc.isVerifyPreBarriersEnabled();
#  else
  return false;
#  endif
}
#endif

#ifdef JS_GC_ZEAL

/*
 * Write barrier verification
 *
 * The next few functions are for write barrier verification.
 *
 * The VerifyBarriers function is a shorthand. It checks if a verification phase
 * is currently running. If not, it starts one. Otherwise, it ends the current
 * phase and starts a new one.
 *
 * The user can adjust the frequency of verifications, which causes
 * VerifyBarriers to be a no-op all but one out of N calls. However, if the
 * |always| parameter is true, it starts a new phase no matter what.
 *
 * Pre-Barrier Verifier:
 *   When StartVerifyBarriers is called, a snapshot is taken of all objects in
 *   the GC heap and saved in an explicit graph data structure. Later,
 *   EndVerifyBarriers traverses the heap again. Any pointer values that were in
 *   the snapshot and are no longer found must be marked; otherwise an assertion
 *   triggers. Note that we must not GC in between starting and finishing a
 *   verification phase.
 */

struct EdgeValue {
  JS::GCCellPtr thing;
  const char* label;
};

struct VerifyNode {
  JS::GCCellPtr thing;
  uint32_t count;
  EdgeValue edges[1];
};

typedef HashMap<Cell*, VerifyNode*, DefaultHasher<Cell*>, SystemAllocPolicy>
    NodeMap;

/*
 * The verifier data structures are simple. The entire graph is stored in a
 * single block of memory. At the beginning is a VerifyNode for the root
 * node. It is followed by a sequence of EdgeValues--the exact number is given
 * in the node. After the edges come more nodes and their edges.
 *
 * The edgeptr and term fields are used to allocate out of the block of memory
 * for the graph. If we run out of memory (i.e., if edgeptr goes beyond term),
 * we just abandon the verification.
 *
 * The nodemap field is a hashtable that maps from the address of the GC thing
 * to the VerifyNode that represents it.
 */
class js::VerifyPreTracer final : public JS::CallbackTracer {
  JS::AutoDisableGenerationalGC noggc;

  void onChild(const JS::GCCellPtr& thing) override;

 public:
  /* The gcNumber when the verification began. */
  uint64_t number;

  /* This counts up to gcZealFrequency to decide whether to verify. */
  int count;

  /* This graph represents the initial GC "snapshot". */
  VerifyNode* curnode;
  VerifyNode* root;
  char* edgeptr;
  char* term;
  NodeMap nodemap;

  explicit VerifyPreTracer(JSRuntime* rt)
      : JS::CallbackTracer(rt, JS::TracerKind::Callback,
                           JS::WeakEdgeTraceAction::Skip),
        noggc(rt->mainContextFromOwnThread()),
        number(rt->gc.gcNumber()),
        count(0),
        curnode(nullptr),
        root(nullptr),
        edgeptr(nullptr),
        term(nullptr) {
    // We don't care about weak edges here. Since they are not marked they
    // cannot cause the problem that the pre-write barrier protects against.
  }

  ~VerifyPreTracer() { js_free(root); }
};

/*
 * This function builds up the heap snapshot by adding edges to the current
 * node.
 */
void VerifyPreTracer::onChild(const JS::GCCellPtr& thing) {
  MOZ_ASSERT(!IsInsideNursery(thing.asCell()));

  // Skip things in other runtimes.
  if (thing.asCell()->asTenured().runtimeFromAnyThread() != runtime()) {
    return;
  }

  edgeptr += sizeof(EdgeValue);
  if (edgeptr >= term) {
    edgeptr = term;
    return;
  }

  VerifyNode* node = curnode;
  uint32_t i = node->count;

  node->edges[i].thing = thing;
  node->edges[i].label = context().name();
  node->count++;
}

static VerifyNode* MakeNode(VerifyPreTracer* trc, JS::GCCellPtr thing) {
  NodeMap::AddPtr p = trc->nodemap.lookupForAdd(thing.asCell());
  if (!p) {
    VerifyNode* node = (VerifyNode*)trc->edgeptr;
    trc->edgeptr += sizeof(VerifyNode) - sizeof(EdgeValue);
    if (trc->edgeptr >= trc->term) {
      trc->edgeptr = trc->term;
      return nullptr;
    }

    node->thing = thing;
    node->count = 0;
    if (!trc->nodemap.add(p, thing.asCell(), node)) {
      trc->edgeptr = trc->term;
      return nullptr;
    }

    return node;
  }
  return nullptr;
}

static VerifyNode* NextNode(VerifyNode* node) {
  if (node->count == 0) {
    return (VerifyNode*)((char*)node + sizeof(VerifyNode) - sizeof(EdgeValue));
  } else {
    return (VerifyNode*)((char*)node + sizeof(VerifyNode) +
                         sizeof(EdgeValue) * (node->count - 1));
  }
}

void gc::GCRuntime::startVerifyPreBarriers() {
  if (verifyPreData || isIncrementalGCInProgress()) {
    return;
  }

  JSContext* cx = rt->mainContextFromOwnThread();

  if (IsIncrementalGCUnsafe(rt) != GCAbortReason::None ||
      rt->hasHelperThreadZones()) {
    return;
  }

  number++;

  VerifyPreTracer* trc = js_new<VerifyPreTracer>(rt);
  if (!trc) {
    return;
  }

  AutoPrepareForTracing prep(cx);

  {
    AutoLockGC lock(this);
    for (auto chunk = allNonEmptyChunks(lock); !chunk.done(); chunk.next()) {
      chunk->markBits.clear();
    }
  }

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::TRACE_HEAP);

  const size_t size = 64 * 1024 * 1024;
  trc->root = (VerifyNode*)js_malloc(size);
  if (!trc->root) {
    goto oom;
  }
  trc->edgeptr = (char*)trc->root;
  trc->term = trc->edgeptr + size;

  /* Create the root node. */
  trc->curnode = MakeNode(trc, JS::GCCellPtr());

  MOZ_ASSERT(incrementalState == State::NotActive);
  incrementalState = State::MarkRoots;

  /* Make all the roots be edges emanating from the root node. */
  traceRuntime(trc, prep);

  VerifyNode* node;
  node = trc->curnode;
  if (trc->edgeptr == trc->term) {
    goto oom;
  }

  /* For each edge, make a node for it if one doesn't already exist. */
  while ((char*)node < trc->edgeptr) {
    for (uint32_t i = 0; i < node->count; i++) {
      EdgeValue& e = node->edges[i];
      VerifyNode* child = MakeNode(trc, e.thing);
      if (child) {
        trc->curnode = child;
        JS::TraceChildren(trc, e.thing);
      }
      if (trc->edgeptr == trc->term) {
        goto oom;
      }
    }

    node = NextNode(node);
  }

  verifyPreData = trc;
  incrementalState = State::Mark;
  marker.start();

  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    MOZ_ASSERT(!zone->usedByHelperThread());
    zone->setNeedsIncrementalBarrier(true);
    zone->arenas.clearFreeLists();
  }

  return;

oom:
  incrementalState = State::NotActive;
  js_delete(trc);
  verifyPreData = nullptr;
}

static bool IsMarkedOrAllocated(TenuredCell* cell) {
  return cell->isMarkedAny();
}

struct CheckEdgeTracer final : public JS::CallbackTracer {
  VerifyNode* node;
  explicit CheckEdgeTracer(JSRuntime* rt)
      : JS::CallbackTracer(rt), node(nullptr) {}
  void onChild(const JS::GCCellPtr& thing) override;
};

static const uint32_t MAX_VERIFIER_EDGES = 1000;

/*
 * This function is called by EndVerifyBarriers for every heap edge. If the edge
 * already existed in the original snapshot, we "cancel it out" by overwriting
 * it with nullptr. EndVerifyBarriers later asserts that the remaining
 * non-nullptr edges (i.e., the ones from the original snapshot that must have
 * been modified) must point to marked objects.
 */
void CheckEdgeTracer::onChild(const JS::GCCellPtr& thing) {
  // Skip things in other runtimes.
  if (thing.asCell()->asTenured().runtimeFromAnyThread() != runtime()) {
    return;
  }

  /* Avoid n^2 behavior. */
  if (node->count > MAX_VERIFIER_EDGES) {
    return;
  }

  for (uint32_t i = 0; i < node->count; i++) {
    if (node->edges[i].thing == thing) {
      node->edges[i].thing = JS::GCCellPtr();
      return;
    }
  }
}

static bool IsMarkedOrAllocated(const EdgeValue& edge) {
  if (!edge.thing || IsMarkedOrAllocated(&edge.thing.asCell()->asTenured())) {
    return true;
  }

  // Permanent atoms and well-known symbols aren't marked during graph
  // traversal.
  if (edge.thing.is<JSString>() &&
      edge.thing.as<JSString>().isPermanentAtom()) {
    return true;
  }
  if (edge.thing.is<JS::Symbol>() &&
      edge.thing.as<JS::Symbol>().isWellKnownSymbol()) {
    return true;
  }

  return false;
}

void gc::GCRuntime::endVerifyPreBarriers() {
  VerifyPreTracer* trc = verifyPreData;

  if (!trc) {
    return;
  }

  MOZ_ASSERT(!JS::IsGenerationalGCEnabled(rt));

  // Flush the barrier tracer's buffer to ensure the pre-barrier has marked
  // everything it's going to. This usually happens as part of GC.
  SliceBudget budget = SliceBudget::unlimited();
  marker.traceBarrieredCells(budget);

  // Now that barrier marking has finished, prepare the heap to allow this
  // method to trace cells and discover their outgoing edges.
  AutoPrepareForTracing prep(rt->mainContextFromOwnThread());

  bool compartmentCreated = false;

  /* We need to disable barriers before tracing, which may invoke barriers. */
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    if (!zone->needsIncrementalBarrier()) {
      compartmentCreated = true;
    }
    zone->setNeedsIncrementalBarrier(false);
  }

  verifyPreData = nullptr;
  MOZ_ASSERT(incrementalState == State::Mark);
  incrementalState = State::NotActive;

  if (!compartmentCreated && IsIncrementalGCUnsafe(rt) == GCAbortReason::None &&
      !rt->hasHelperThreadZones()) {
    CheckEdgeTracer cetrc(rt);

    /* Start after the roots. */
    VerifyNode* node = NextNode(trc->root);
    while ((char*)node < trc->edgeptr) {
      cetrc.node = node;
      JS::TraceChildren(&cetrc, node->thing);

      if (node->count <= MAX_VERIFIER_EDGES) {
        for (uint32_t i = 0; i < node->count; i++) {
          EdgeValue& edge = node->edges[i];
          if (!IsMarkedOrAllocated(edge)) {
            char msgbuf[1024];
            SprintfLiteral(
                msgbuf,
                "[barrier verifier] Unmarked edge: %s %p '%s' edge to %s %p",
                JS::GCTraceKindToAscii(node->thing.kind()),
                node->thing.asCell(), edge.label,
                JS::GCTraceKindToAscii(edge.thing.kind()), edge.thing.asCell());
            MOZ_ReportAssertionFailure(msgbuf, __FILE__, __LINE__);
            MOZ_CRASH();
          }
        }
      }

      node = NextNode(node);
    }
  }

  marker.reset();
  marker.stop();

  js_delete(trc);
}

/*** Barrier Verifier Scheduling ***/

void gc::GCRuntime::verifyPreBarriers() {
  if (verifyPreData) {
    endVerifyPreBarriers();
  } else {
    startVerifyPreBarriers();
  }
}

void gc::VerifyBarriers(JSRuntime* rt, VerifierType type) {
  if (type == PreBarrierVerifier) {
    rt->gc.verifyPreBarriers();
  }
}

void gc::GCRuntime::maybeVerifyPreBarriers(bool always) {
  if (!hasZealMode(ZealMode::VerifierPre)) {
    return;
  }

  if (rt->mainContextFromOwnThread()->suppressGC) {
    return;
  }

  if (verifyPreData) {
    if (++verifyPreData->count < zealFrequency && !always) {
      return;
    }

    endVerifyPreBarriers();
  }

  startVerifyPreBarriers();
}

void js::gc::MaybeVerifyBarriers(JSContext* cx, bool always) {
  GCRuntime* gc = &cx->runtime()->gc;
  gc->maybeVerifyPreBarriers(always);
}

void js::gc::GCRuntime::finishVerifier() {
  if (verifyPreData) {
    js_delete(verifyPreData.ref());
    verifyPreData = nullptr;
  }
}

struct GCChunkHasher {
  typedef gc::TenuredChunk* Lookup;

  /*
   * Strip zeros for better distribution after multiplying by the golden
   * ratio.
   */
  static HashNumber hash(gc::TenuredChunk* chunk) {
    MOZ_ASSERT(!(uintptr_t(chunk) & gc::ChunkMask));
    return HashNumber(uintptr_t(chunk) >> gc::ChunkShift);
  }

  static bool match(gc::TenuredChunk* k, gc::TenuredChunk* l) {
    MOZ_ASSERT(!(uintptr_t(k) & gc::ChunkMask));
    MOZ_ASSERT(!(uintptr_t(l) & gc::ChunkMask));
    return k == l;
  }
};

class js::gc::MarkingValidator {
 public:
  explicit MarkingValidator(GCRuntime* gc);
  void nonIncrementalMark(AutoGCSession& session);
  void validate();

 private:
  GCRuntime* gc;
  bool initialized;

  using BitmapMap = HashMap<TenuredChunk*, UniquePtr<MarkBitmap>, GCChunkHasher,
                            SystemAllocPolicy>;
  BitmapMap map;
};

js::gc::MarkingValidator::MarkingValidator(GCRuntime* gc)
    : gc(gc), initialized(false) {}

void js::gc::MarkingValidator::nonIncrementalMark(AutoGCSession& session) {
  /*
   * Perform a non-incremental mark for all collecting zones and record
   * the results for later comparison.
   *
   * Currently this does not validate gray marking.
   */

  JSRuntime* runtime = gc->rt;
  GCMarker* gcmarker = &gc->marker;

  MOZ_ASSERT(!gcmarker->isWeakMarking());

  /* Wait for off-thread parsing which can allocate. */
  WaitForAllHelperThreads();

  gc->waitBackgroundAllocEnd();
  gc->waitBackgroundSweepEnd();

  /* Save existing mark bits. */
  {
    AutoLockGC lock(gc);
    for (auto chunk = gc->allNonEmptyChunks(lock); !chunk.done();
         chunk.next()) {
      MarkBitmap* bitmap = &chunk->markBits;
      auto entry = MakeUnique<MarkBitmap>();
      if (!entry) {
        return;
      }

      memcpy((void*)entry->bitmap, (void*)bitmap->bitmap,
             sizeof(bitmap->bitmap));

      if (!map.putNew(chunk, std::move(entry))) {
        return;
      }
    }
  }

  /*
   * Temporarily clear the weakmaps' mark flags for the compartments we are
   * collecting.
   */

  WeakMapColors markedWeakMaps;

  /*
   * For saving, smush all of the keys into one big table and split them back
   * up into per-zone tables when restoring.
   */
  gc::EphemeronEdgeTable savedEphemeronEdges(
      SystemAllocPolicy(), runtime->randomHashCodeScrambler());
  if (!savedEphemeronEdges.init()) {
    return;
  }

  for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
    if (!WeakMapBase::saveZoneMarkedWeakMaps(zone, markedWeakMaps)) {
      return;
    }

    AutoEnterOOMUnsafeRegion oomUnsafe;
    for (gc::EphemeronEdgeTable::Range r = zone->gcEphemeronEdges().all();
         !r.empty(); r.popFront()) {
      MOZ_ASSERT(r.front().key->asTenured().zone() == zone);
      if (!savedEphemeronEdges.put(r.front().key, std::move(r.front().value))) {
        oomUnsafe.crash("saving weak keys table for validator");
      }
    }

    if (!zone->gcEphemeronEdges().clear()) {
      oomUnsafe.crash("clearing weak keys table for validator");
    }
  }

  /*
   * After this point, the function should run to completion, so we shouldn't
   * do anything fallible.
   */
  initialized = true;

  /* Re-do all the marking, but non-incrementally. */
  js::gc::State state = gc->incrementalState;
  gc->incrementalState = State::MarkRoots;

  {
    gcstats::AutoPhase ap(gc->stats(), gcstats::PhaseKind::PREPARE);

    {
      gcstats::AutoPhase ap(gc->stats(), gcstats::PhaseKind::UNMARK);

      for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
        WeakMapBase::unmarkZone(zone);
      }

      MOZ_ASSERT(gcmarker->isDrained());
      gcmarker->reset();

      AutoLockGC lock(gc);
      for (auto chunk = gc->allNonEmptyChunks(lock); !chunk.done();
           chunk.next()) {
        chunk->markBits.clear();
      }
    }
  }

  {
    gcstats::AutoPhase ap(gc->stats(), gcstats::PhaseKind::MARK);

    gc->traceRuntimeForMajorGC(gcmarker, session);

    gc->incrementalState = State::Mark;
    gc->drainMarkStack();
  }

  gc->incrementalState = State::Sweep;
  {
    gcstats::AutoPhase ap1(gc->stats(), gcstats::PhaseKind::SWEEP);
    gcstats::AutoPhase ap2(gc->stats(), gcstats::PhaseKind::SWEEP_MARK);

    gc->markAllWeakReferences();

    /* Update zone state for gray marking. */
    for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
      zone->changeGCState(Zone::MarkBlackOnly, Zone::MarkBlackAndGray);
    }

    AutoSetMarkColor setColorGray(*gcmarker, MarkColor::Gray);
    gcmarker->setMainStackColor(MarkColor::Gray);

    gc->markAllGrayReferences(gcstats::PhaseKind::SWEEP_MARK_GRAY);
    gc->markAllWeakReferences();
    gc->marker.setMainStackColor(MarkColor::Black);

    /* Restore zone state. */
    for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
      zone->changeGCState(Zone::MarkBlackAndGray, Zone::MarkBlackOnly);
    }
    MOZ_ASSERT(gc->marker.isDrained());
  }

  /* Take a copy of the non-incremental mark state and restore the original. */
  {
    AutoLockGC lock(gc);
    for (auto chunk = gc->allNonEmptyChunks(lock); !chunk.done();
         chunk.next()) {
      MarkBitmap* bitmap = &chunk->markBits;
      auto ptr = map.lookup(chunk);
      MOZ_RELEASE_ASSERT(ptr, "Chunk not found in map");
      MarkBitmap* entry = ptr->value().get();
      for (size_t i = 0; i < MarkBitmap::WordCount; i++) {
        uintptr_t v = entry->bitmap[i];
        entry->bitmap[i] = uintptr_t(bitmap->bitmap[i]);
        bitmap->bitmap[i] = v;
      }
    }
  }

  for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
    WeakMapBase::unmarkZone(zone);
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!zone->gcEphemeronEdges().clear()) {
      oomUnsafe.crash("clearing weak keys table for validator");
    }
  }

  WeakMapBase::restoreMarkedWeakMaps(markedWeakMaps);

  for (gc::EphemeronEdgeTable::Range r = savedEphemeronEdges.all(); !r.empty();
       r.popFront()) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    Zone* zone = r.front().key->asTenured().zone();
    if (!zone->gcEphemeronEdges().put(r.front().key,
                                      std::move(r.front().value))) {
      oomUnsafe.crash("restoring weak keys table for validator");
    }
  }

  gc->incrementalState = state;
}

void js::gc::MarkingValidator::validate() {
  /*
   * Validates the incremental marking for a single compartment by comparing
   * the mark bits to those previously recorded for a non-incremental mark.
   */

  if (!initialized) {
    return;
  }

  MOZ_ASSERT(!gc->marker.isWeakMarking());

  gc->waitBackgroundSweepEnd();

  AutoLockGC lock(gc->rt);
  for (auto chunk = gc->allNonEmptyChunks(lock); !chunk.done(); chunk.next()) {
    BitmapMap::Ptr ptr = map.lookup(chunk);
    if (!ptr) {
      continue; /* Allocated after we did the non-incremental mark. */
    }

    MarkBitmap* bitmap = ptr->value().get();
    MarkBitmap* incBitmap = &chunk->markBits;

    for (size_t i = 0; i < ArenasPerChunk; i++) {
      if (chunk->decommittedPages[chunk->pageIndex(i)]) {
        continue;
      }
      Arena* arena = &chunk->arenas[i];
      if (!arena->allocated()) {
        continue;
      }
      if (!arena->zone->isGCSweeping()) {
        continue;
      }

      AllocKind kind = arena->getAllocKind();
      uintptr_t thing = arena->thingsStart();
      uintptr_t end = arena->thingsEnd();
      while (thing < end) {
        auto* cell = reinterpret_cast<TenuredCell*>(thing);

        /*
         * If a non-incremental GC wouldn't have collected a cell, then
         * an incremental GC won't collect it.
         */
        if (bitmap->isMarkedAny(cell)) {
          MOZ_RELEASE_ASSERT(incBitmap->isMarkedAny(cell));
        }

        /*
         * If the cycle collector isn't allowed to collect an object
         * after a non-incremental GC has run, then it isn't allowed to
         * collected it after an incremental GC.
         */
        if (!bitmap->isMarkedGray(cell)) {
          MOZ_RELEASE_ASSERT(!incBitmap->isMarkedGray(cell));
        }

        thing += Arena::thingSize(kind);
      }
    }
  }
}

void GCRuntime::computeNonIncrementalMarkingForValidation(
    AutoGCSession& session) {
  MOZ_ASSERT(!markingValidator);
  if (isIncremental && hasZealMode(ZealMode::IncrementalMarkingValidator)) {
    markingValidator = js_new<MarkingValidator>(this);
  }
  if (markingValidator) {
    markingValidator->nonIncrementalMark(session);
  }
}

void GCRuntime::validateIncrementalMarking() {
  if (markingValidator) {
    markingValidator->validate();
  }
}

void GCRuntime::finishMarkingValidation() {
  js_delete(markingValidator.ref());
  markingValidator = nullptr;
}

#endif /* JS_GC_ZEAL */

#if defined(JS_GC_ZEAL) || defined(DEBUG)

class HeapCheckTracerBase : public JS::CallbackTracer {
 public:
  explicit HeapCheckTracerBase(JSRuntime* rt, JS::TraceOptions options);
  bool traceHeap(AutoTraceSession& session);
  virtual void checkCell(Cell* cell) = 0;

 protected:
  void dumpCellInfo(Cell* cell);
  void dumpCellPath();

  Cell* parentCell() {
    return parentIndex == -1 ? nullptr : stack[parentIndex].thing.asCell();
  }

  size_t failures;

 private:
  void onChild(const JS::GCCellPtr& thing) override;

  struct WorkItem {
    WorkItem(JS::GCCellPtr thing, const char* name, int parentIndex)
        : thing(thing),
          name(name),
          parentIndex(parentIndex),
          processed(false) {}

    JS::GCCellPtr thing;
    const char* name;
    int parentIndex;
    bool processed;
  };

  JSRuntime* rt;
  bool oom;
  HashSet<Cell*, DefaultHasher<Cell*>, SystemAllocPolicy> visited;
  Vector<WorkItem, 0, SystemAllocPolicy> stack;
  int parentIndex;
};

HeapCheckTracerBase::HeapCheckTracerBase(JSRuntime* rt,
                                         JS::TraceOptions options)
    : CallbackTracer(rt, JS::TracerKind::Callback, options),
      failures(0),
      rt(rt),
      oom(false),
      parentIndex(-1) {}

void HeapCheckTracerBase::onChild(const JS::GCCellPtr& thing) {
  Cell* cell = thing.asCell();
  checkCell(cell);

  if (visited.lookup(cell)) {
    return;
  }

  if (!visited.put(cell)) {
    oom = true;
    return;
  }

  // Don't trace into GC things owned by another runtime.
  if (cell->runtimeFromAnyThread() != rt) {
    return;
  }

  // Don't trace into GC in zones being used by helper threads.
  Zone* zone = thing.asCell()->zone();
  if (zone->usedByHelperThread()) {
    return;
  }

  WorkItem item(thing, context().name(), parentIndex);
  if (!stack.append(item)) {
    oom = true;
  }
}

bool HeapCheckTracerBase::traceHeap(AutoTraceSession& session) {
  // The analysis thinks that traceRuntime might GC by calling a GC callback.
  JS::AutoSuppressGCAnalysis nogc;
  if (!rt->isBeingDestroyed()) {
    rt->gc.traceRuntime(this, session);
  }

  while (!stack.empty() && !oom) {
    WorkItem item = stack.back();
    if (item.processed) {
      stack.popBack();
    } else {
      parentIndex = stack.length() - 1;
      stack.back().processed = true;
      TraceChildren(this, item.thing);
    }
  }

  return !oom;
}

void HeapCheckTracerBase::dumpCellInfo(Cell* cell) {
  auto kind = cell->getTraceKind();
  JSObject* obj =
      kind == JS::TraceKind::Object ? static_cast<JSObject*>(cell) : nullptr;

  fprintf(stderr, "%s %s", cell->color().name(), GCTraceKindToAscii(kind));
  if (obj) {
    fprintf(stderr, " %s", obj->getClass()->name);
  }
  fprintf(stderr, " %p", cell);
  if (obj) {
    fprintf(stderr, " (compartment %p)", obj->compartment());
  }
}

void HeapCheckTracerBase::dumpCellPath() {
  const char* name = context().name();
  for (int index = parentIndex; index != -1; index = stack[index].parentIndex) {
    const WorkItem& parent = stack[index];
    Cell* cell = parent.thing.asCell();
    fprintf(stderr, "  from ");
    dumpCellInfo(cell);
    fprintf(stderr, " %s edge\n", name);
    name = parent.name;
  }
  fprintf(stderr, "  from root %s\n", name);
}

class CheckHeapTracer final : public HeapCheckTracerBase {
 public:
  enum GCType { Moving, NonMoving };

  explicit CheckHeapTracer(JSRuntime* rt, GCType type);
  void check(AutoTraceSession& session);

 private:
  void checkCell(Cell* cell) override;
  GCType gcType;
};

CheckHeapTracer::CheckHeapTracer(JSRuntime* rt, GCType type)
    : HeapCheckTracerBase(rt, JS::WeakMapTraceAction::TraceKeysAndValues),
      gcType(type) {}

inline static bool IsValidGCThingPointer(Cell* cell) {
  return (uintptr_t(cell) & CellAlignMask) == 0;
}

void CheckHeapTracer::checkCell(Cell* cell) {
  // Moving
  if (!IsValidGCThingPointer(cell) ||
      ((gcType == GCType::Moving) && !IsGCThingValidAfterMovingGC(cell)) ||
      ((gcType == GCType::NonMoving) && cell->isForwarded())) {
    failures++;
    fprintf(stderr, "Bad pointer %p\n", cell);
    dumpCellPath();
  }
}

void CheckHeapTracer::check(AutoTraceSession& session) {
  if (!traceHeap(session)) {
    return;
  }

  if (failures) {
    fprintf(stderr, "Heap check: %zu failure(s)\n", failures);
  }
  MOZ_RELEASE_ASSERT(failures == 0);
}

void js::gc::CheckHeapAfterGC(JSRuntime* rt) {
  AutoTraceSession session(rt);
  CheckHeapTracer::GCType gcType;

  if (rt->gc.nursery().isEmpty()) {
    gcType = CheckHeapTracer::GCType::Moving;
  } else {
    gcType = CheckHeapTracer::GCType::NonMoving;
  }

  CheckHeapTracer tracer(rt, gcType);
  tracer.check(session);
}

class CheckGrayMarkingTracer final : public HeapCheckTracerBase {
 public:
  explicit CheckGrayMarkingTracer(JSRuntime* rt);
  bool check(AutoTraceSession& session);

 private:
  void checkCell(Cell* cell) override;
};

CheckGrayMarkingTracer::CheckGrayMarkingTracer(JSRuntime* rt)
    : HeapCheckTracerBase(rt, JS::TraceOptions(JS::WeakMapTraceAction::Skip,
                                               JS::WeakEdgeTraceAction::Skip)) {
  // Weak gray->black edges are allowed.
}

void CheckGrayMarkingTracer::checkCell(Cell* cell) {
  Cell* parent = parentCell();
  if (!parent) {
    return;
  }

  if (parent->isMarkedBlack() && cell->isMarkedGray()) {
    failures++;

    fprintf(stderr, "Found black to gray edge to ");
    dumpCellInfo(cell);
    fprintf(stderr, "\n");
    dumpCellPath();

#  ifdef DEBUG
    if (parent->is<JSObject>()) {
      fprintf(stderr, "\nSource: ");
      DumpObject(parent->as<JSObject>(), stderr);
    }
    if (cell->is<JSObject>()) {
      fprintf(stderr, "\nTarget: ");
      DumpObject(cell->as<JSObject>(), stderr);
    }
#  endif
  }
}

bool CheckGrayMarkingTracer::check(AutoTraceSession& session) {
  if (!traceHeap(session)) {
    return true;  // Ignore failure.
  }

  return failures == 0;
}

JS_PUBLIC_API bool js::CheckGrayMarkingState(JSRuntime* rt) {
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());
  MOZ_ASSERT(!rt->gc.isIncrementalGCInProgress());
  if (!rt->gc.areGrayBitsValid()) {
    return true;
  }

  gcstats::AutoPhase ap(rt->gc.stats(), gcstats::PhaseKind::TRACE_HEAP);
  AutoTraceSession session(rt);
  CheckGrayMarkingTracer tracer(rt);

  return tracer.check(session);
}

static JSObject* MaybeGetDelegate(Cell* cell) {
  if (!cell->is<JSObject>()) {
    return nullptr;
  }

  JSObject* object = cell->as<JSObject>();
  return js::UncheckedUnwrapWithoutExpose(object);
}

bool js::gc::CheckWeakMapEntryMarking(const WeakMapBase* map, Cell* key,
                                      Cell* value) {
  bool ok = true;

  Zone* zone = map->zone();
  MOZ_ASSERT(CurrentThreadCanAccessZone(zone));
  MOZ_ASSERT(zone->isGCMarking());

  JSObject* object = map->memberOf;
  MOZ_ASSERT_IF(object, object->zone() == zone);

  // Debugger weak maps can have keys in different zones.
  Zone* keyZone = key->zoneFromAnyThread();
  MOZ_ASSERT_IF(!map->allowKeysInOtherZones(),
                keyZone == zone || keyZone->isAtomsZone());

  Zone* valueZone = value->zoneFromAnyThread();
  MOZ_ASSERT(valueZone == zone || valueZone->isAtomsZone());

  if (object && object->color() != map->mapColor) {
    fprintf(stderr, "WeakMap object is marked differently to the map\n");
    fprintf(stderr, "(map %p is %s, object %p is %s)\n", map,
            map->mapColor.name(), object, object->color().name());
    ok = false;
  }

  // Values belonging to other runtimes or in uncollected zones are treated as
  // black.
  JSRuntime* mapRuntime = zone->runtimeFromAnyThread();
  auto effectiveColor = [=](Cell* cell, Zone* cellZone) -> CellColor {
    if (cell->runtimeFromAnyThread() != mapRuntime) {
      return CellColor::Black;
    }
    if (cellZone->isGCMarkingOrSweeping()) {
      return cell->color();
    }
    return CellColor::Black;
  };

  CellColor valueColor = effectiveColor(value, valueZone);
  CellColor keyColor = effectiveColor(key, keyZone);

  if (valueColor < std::min(map->mapColor, keyColor)) {
    fprintf(stderr, "WeakMap value is less marked than map and key\n");
    fprintf(stderr, "(map %p is %s, key %p is %s, value %p is %s)\n", map,
            map->mapColor.name(), key, keyColor.name(), value,
            valueColor.name());
#  ifdef DEBUG
    fprintf(stderr, "Key:\n");
    key->dump();
    if (auto delegate = MaybeGetDelegate(key); delegate) {
      fprintf(stderr, "Delegate:\n");
      delegate->dump();
    }
    fprintf(stderr, "Value:\n");
    value->dump();
#  endif

    ok = false;
  }

  JSObject* delegate = MaybeGetDelegate(key);
  if (!delegate) {
    return ok;
  }

  CellColor delegateColor = effectiveColor(delegate, delegate->zone());
  if (keyColor < std::min(map->mapColor, delegateColor)) {
    fprintf(stderr, "WeakMap key is less marked than map or delegate\n");
    fprintf(stderr, "(map %p is %s, delegate %p is %s, key %p is %s)\n", map,
            map->mapColor.name(), delegate, delegateColor.name(), key,
            keyColor.name());
    ok = false;
  }

  return ok;
}

#endif  // defined(JS_GC_ZEAL) || defined(DEBUG)

#ifdef DEBUG
// Return whether an arbitrary pointer is within a cell with the given
// traceKind. Only for assertions.
bool GCRuntime::isPointerWithinTenuredCell(void* ptr, JS::TraceKind traceKind) {
  AutoLockGC lock(this);
  for (auto chunk = allNonEmptyChunks(lock); !chunk.done(); chunk.next()) {
    MOZ_ASSERT(!chunk->isNurseryChunk());
    if (ptr >= &chunk->arenas[0] && ptr < &chunk->arenas[ArenasPerChunk]) {
      auto* arena = reinterpret_cast<Arena*>(uintptr_t(ptr) & ~ArenaMask);
      if (!arena->allocated()) {
        return false;
      }

      return MapAllocToTraceKind(arena->getAllocKind()) == traceKind;
    }
  }

  return false;
}
#endif  // DEBUG
