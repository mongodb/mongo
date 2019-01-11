/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Sprintf.h"

#ifdef MOZ_VALGRIND
# include <valgrind/memcheck.h>
#endif

#include "gc/GCInternals.h"
#include "gc/PublicIterators.h"
#include "gc/Zone.h"
#include "js/HashTable.h"
#include "vm/JSContext.h"

#include "gc/ArenaList-inl.h"
#include "gc/GC-inl.h"
#include "gc/Marking-inl.h"
#include "vm/JSContext-inl.h"

using namespace js;
using namespace js::gc;

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

struct EdgeValue
{
    void* thing;
    JS::TraceKind kind;
    const char* label;
};

struct VerifyNode
{
    void* thing;
    JS::TraceKind kind;
    uint32_t count;
    EdgeValue edges[1];
};

typedef HashMap<void*, VerifyNode*, DefaultHasher<void*>, SystemAllocPolicy> NodeMap;

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
class js::VerifyPreTracer final : public JS::CallbackTracer
{
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
      : JS::CallbackTracer(rt), noggc(TlsContext.get()), number(rt->gc.gcNumber()),
        count(0), curnode(nullptr), root(nullptr), edgeptr(nullptr), term(nullptr)
    {}

    ~VerifyPreTracer() {
        js_free(root);
    }
};

/*
 * This function builds up the heap snapshot by adding edges to the current
 * node.
 */
void
VerifyPreTracer::onChild(const JS::GCCellPtr& thing)
{
    MOZ_ASSERT(!IsInsideNursery(thing.asCell()));

    // Skip things in other runtimes.
    if (thing.asCell()->asTenured().runtimeFromAnyThread() != runtime())
        return;

    edgeptr += sizeof(EdgeValue);
    if (edgeptr >= term) {
        edgeptr = term;
        return;
    }

    VerifyNode* node = curnode;
    uint32_t i = node->count;

    node->edges[i].thing = thing.asCell();
    node->edges[i].kind = thing.kind();
    node->edges[i].label = contextName();
    node->count++;
}

static VerifyNode*
MakeNode(VerifyPreTracer* trc, void* thing, JS::TraceKind kind)
{
    NodeMap::AddPtr p = trc->nodemap.lookupForAdd(thing);
    if (!p) {
        VerifyNode* node = (VerifyNode*)trc->edgeptr;
        trc->edgeptr += sizeof(VerifyNode) - sizeof(EdgeValue);
        if (trc->edgeptr >= trc->term) {
            trc->edgeptr = trc->term;
            return nullptr;
        }

        node->thing = thing;
        node->count = 0;
        node->kind = kind;
        if (!trc->nodemap.add(p, thing, node)) {
            trc->edgeptr = trc->term;
            return nullptr;
        }

        return node;
    }
    return nullptr;
}

static VerifyNode*
NextNode(VerifyNode* node)
{
    if (node->count == 0)
        return (VerifyNode*)((char*)node + sizeof(VerifyNode) - sizeof(EdgeValue));
    else
        return (VerifyNode*)((char*)node + sizeof(VerifyNode) +
                             sizeof(EdgeValue)*(node->count - 1));
}

void
gc::GCRuntime::startVerifyPreBarriers()
{
    if (verifyPreData || isIncrementalGCInProgress())
        return;

    if (IsIncrementalGCUnsafe(rt) != AbortReason::None ||
        TlsContext.get()->keepAtoms ||
        rt->hasHelperThreadZones() ||
        rt->cooperatingContexts().length() != 1)
    {
        return;
    }

    number++;

    VerifyPreTracer* trc = js_new<VerifyPreTracer>(rt);
    if (!trc)
        return;

    JSContext* cx = TlsContext.get();
    AutoPrepareForTracing prep(cx);

    {
        AutoLockGC lock(cx->runtime());
        for (auto chunk = allNonEmptyChunks(lock); !chunk.done(); chunk.next())
            chunk->bitmap.clear();
    }

    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::TRACE_HEAP);

    const size_t size = 64 * 1024 * 1024;
    trc->root = (VerifyNode*)js_malloc(size);
    if (!trc->root)
        goto oom;
    trc->edgeptr = (char*)trc->root;
    trc->term = trc->edgeptr + size;

    if (!trc->nodemap.init())
        goto oom;

    /* Create the root node. */
    trc->curnode = MakeNode(trc, nullptr, JS::TraceKind(0));

    incrementalState = State::MarkRoots;

    /* Make all the roots be edges emanating from the root node. */
    traceRuntime(trc, prep.session());

    VerifyNode* node;
    node = trc->curnode;
    if (trc->edgeptr == trc->term)
        goto oom;

    /* For each edge, make a node for it if one doesn't already exist. */
    while ((char*)node < trc->edgeptr) {
        for (uint32_t i = 0; i < node->count; i++) {
            EdgeValue& e = node->edges[i];
            VerifyNode* child = MakeNode(trc, e.thing, e.kind);
            if (child) {
                trc->curnode = child;
                js::TraceChildren(trc, e.thing, e.kind);
            }
            if (trc->edgeptr == trc->term)
                goto oom;
        }

        node = NextNode(node);
    }

    verifyPreData = trc;
    incrementalState = State::Mark;
    marker.start();

    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
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

static bool
IsMarkedOrAllocated(TenuredCell* cell)
{
    return cell->isMarkedAny();
}

struct CheckEdgeTracer : public JS::CallbackTracer {
    VerifyNode* node;
    explicit CheckEdgeTracer(JSRuntime* rt) : JS::CallbackTracer(rt), node(nullptr) {}
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
void
CheckEdgeTracer::onChild(const JS::GCCellPtr& thing)
{
    // Skip things in other runtimes.
    if (thing.asCell()->asTenured().runtimeFromAnyThread() != runtime())
        return;

    /* Avoid n^2 behavior. */
    if (node->count > MAX_VERIFIER_EDGES)
        return;

    for (uint32_t i = 0; i < node->count; i++) {
        if (node->edges[i].thing == thing.asCell()) {
            MOZ_ASSERT(node->edges[i].kind == thing.kind());
            node->edges[i].thing = nullptr;
            return;
        }
    }
}

void
js::gc::AssertSafeToSkipBarrier(TenuredCell* thing)
{
    mozilla::DebugOnly<Zone*> zone = thing->zoneFromAnyThread();
    MOZ_ASSERT(!zone->needsIncrementalBarrier() || zone->isAtomsZone());
}

static bool
IsMarkedOrAllocated(const EdgeValue& edge)
{
    if (!edge.thing || IsMarkedOrAllocated(TenuredCell::fromPointer(edge.thing)))
        return true;

    // Permanent atoms and well-known symbols aren't marked during graph traversal.
    if (edge.kind == JS::TraceKind::String && static_cast<JSString*>(edge.thing)->isPermanentAtom())
        return true;
    if (edge.kind == JS::TraceKind::Symbol && static_cast<JS::Symbol*>(edge.thing)->isWellKnownSymbol())
        return true;

    return false;
}

void
gc::GCRuntime::endVerifyPreBarriers()
{
    VerifyPreTracer* trc = verifyPreData;

    if (!trc)
        return;

    MOZ_ASSERT(!JS::IsGenerationalGCEnabled(rt));

    AutoPrepareForTracing prep(rt->activeContextFromOwnThread());

    bool compartmentCreated = false;

    /* We need to disable barriers before tracing, which may invoke barriers. */
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (!zone->needsIncrementalBarrier())
            compartmentCreated = true;

        zone->setNeedsIncrementalBarrier(false);
    }

    /*
     * We need to bump gcNumber so that the methodjit knows that jitcode has
     * been discarded.
     */
    MOZ_ASSERT(trc->number == number);
    number++;

    verifyPreData = nullptr;
    incrementalState = State::NotActive;

    if (!compartmentCreated &&
        IsIncrementalGCUnsafe(rt) == AbortReason::None &&
        !TlsContext.get()->keepAtoms &&
        !rt->hasHelperThreadZones())
    {
        CheckEdgeTracer cetrc(rt);

        /* Start after the roots. */
        VerifyNode* node = NextNode(trc->root);
        while ((char*)node < trc->edgeptr) {
            cetrc.node = node;
            js::TraceChildren(&cetrc, node->thing, node->kind);

            if (node->count <= MAX_VERIFIER_EDGES) {
                for (uint32_t i = 0; i < node->count; i++) {
                    EdgeValue& edge = node->edges[i];
                    if (!IsMarkedOrAllocated(edge)) {
                        char msgbuf[1024];
                        SprintfLiteral(msgbuf,
                                       "[barrier verifier] Unmarked edge: %s %p '%s' edge to %s %p",
                                       JS::GCTraceKindToAscii(node->kind), node->thing,
                                       edge.label,
                                       JS::GCTraceKindToAscii(edge.kind), edge.thing);
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

void
gc::GCRuntime::verifyPreBarriers()
{
    if (verifyPreData)
        endVerifyPreBarriers();
    else
        startVerifyPreBarriers();
}

void
gc::VerifyBarriers(JSRuntime* rt, VerifierType type)
{
    if (type == PreBarrierVerifier)
        rt->gc.verifyPreBarriers();
}

void
gc::GCRuntime::maybeVerifyPreBarriers(bool always)
{
    if (!hasZealMode(ZealMode::VerifierPre))
        return;

    if (TlsContext.get()->suppressGC)
        return;

    if (verifyPreData) {
        if (++verifyPreData->count < zealFrequency && !always)
            return;

        endVerifyPreBarriers();
    }

    startVerifyPreBarriers();
}

void
js::gc::MaybeVerifyBarriers(JSContext* cx, bool always)
{
    GCRuntime* gc = &cx->runtime()->gc;
    gc->maybeVerifyPreBarriers(always);
}

void
js::gc::GCRuntime::finishVerifier()
{
    if (verifyPreData) {
        js_delete(verifyPreData.ref());
        verifyPreData = nullptr;
    }
}

#endif /* JS_GC_ZEAL */

#if defined(JSGC_HASH_TABLE_CHECKS) || defined(DEBUG)

class HeapCheckTracerBase : public JS::CallbackTracer
{
  public:
    explicit HeapCheckTracerBase(JSRuntime* rt, WeakMapTraceKind weakTraceKind);
    bool init();
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
          : thing(thing), name(name), parentIndex(parentIndex), processed(false)
        {}

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

HeapCheckTracerBase::HeapCheckTracerBase(JSRuntime* rt, WeakMapTraceKind weakTraceKind)
  : CallbackTracer(rt, weakTraceKind),
    failures(0),
    rt(rt),
    oom(false),
    parentIndex(-1)
{
#ifdef DEBUG
    setCheckEdges(false);
#endif
}

bool
HeapCheckTracerBase::init()
{
    return visited.init();
}

void
HeapCheckTracerBase::onChild(const JS::GCCellPtr& thing)
{
    Cell* cell = thing.asCell();
    checkCell(cell);

    if (visited.lookup(cell))
        return;

    if (!visited.put(cell)) {
        oom = true;
        return;
    }

    // Don't trace into GC things owned by another runtime.
    if (cell->runtimeFromAnyThread() != rt)
        return;

    // Don't trace into GC in zones being used by helper threads.
    Zone* zone;
    if (thing.is<JSObject>())
        zone = thing.as<JSObject>().zone();
    else if (thing.is<JSString>())
        zone = thing.as<JSString>().zone();
    else
        zone = cell->asTenured().zone();

    if (zone->group() && zone->group()->usedByHelperThread())
        return;

    WorkItem item(thing, contextName(), parentIndex);
    if (!stack.append(item))
        oom = true;
}

bool
HeapCheckTracerBase::traceHeap(AutoTraceSession& session)
{
    // The analysis thinks that traceRuntime might GC by calling a GC callback.
    JS::AutoSuppressGCAnalysis nogc;
    if (!rt->isBeingDestroyed())
        rt->gc.traceRuntime(this, session);

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

static const char*
GetCellColorName(Cell* cell)
{
    if (cell->isMarkedBlack())
        return "black";
    if (cell->isMarkedGray())
        return "gray";
    return "white";
}

void
HeapCheckTracerBase::dumpCellInfo(Cell* cell)
{
    auto kind = cell->getTraceKind();
    JSObject* obj = kind == JS::TraceKind::Object ? static_cast<JSObject*>(cell) : nullptr;

    fprintf(stderr, "%s %s", GetCellColorName(cell), GCTraceKindToAscii(kind));
    if (obj)
        fprintf(stderr, " %s", obj->getClass()->name);
    fprintf(stderr, " %p", cell);
    if (obj)
        fprintf(stderr, " (compartment %p)", obj->compartment());
}

void
HeapCheckTracerBase::dumpCellPath()
{
    const char* name = contextName();
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

#endif // defined(JSGC_HASH_TABLE_CHECKS) || defined(DEBUG)

#ifdef JSGC_HASH_TABLE_CHECKS

class CheckHeapTracer final : public HeapCheckTracerBase
{
  public:
    explicit CheckHeapTracer(JSRuntime* rt);
    void check(AutoTraceSession& session);

  private:
    void checkCell(Cell* cell) override;
};

CheckHeapTracer::CheckHeapTracer(JSRuntime* rt)
  : HeapCheckTracerBase(rt, TraceWeakMapKeysValues)
{}

inline static bool
IsValidGCThingPointer(Cell* cell)
{
    return (uintptr_t(cell) & CellAlignMask) == 0;
}

void
CheckHeapTracer::checkCell(Cell* cell)
{
    if (!IsValidGCThingPointer(cell) || !IsGCThingValidAfterMovingGC(cell)) {
        failures++;
        fprintf(stderr, "Bad pointer %p\n", cell);
        dumpCellPath();
    }
}

void
CheckHeapTracer::check(AutoTraceSession& session)
{
    if (!traceHeap(session))
        return;

    if (failures)
        fprintf(stderr, "Heap check: %zu failure(s)\n", failures);
    MOZ_RELEASE_ASSERT(failures == 0);
}

void
js::gc::CheckHeapAfterGC(JSRuntime* rt)
{
    AutoTraceSession session(rt, JS::HeapState::Tracing);
    CheckHeapTracer tracer(rt);
    if (tracer.init())
        tracer.check(session);
}

#endif /* JSGC_HASH_TABLE_CHECKS */

#if defined(JS_GC_ZEAL) || defined(DEBUG)

class CheckGrayMarkingTracer final : public HeapCheckTracerBase
{
  public:
    explicit CheckGrayMarkingTracer(JSRuntime* rt);
    bool check(AutoTraceSession& session);

  private:
    void checkCell(Cell* cell) override;
};

CheckGrayMarkingTracer::CheckGrayMarkingTracer(JSRuntime* rt)
  : HeapCheckTracerBase(rt, DoNotTraceWeakMaps)
{
    // Weak gray->black edges are allowed.
    setTraceWeakEdges(false);
}

void
CheckGrayMarkingTracer::checkCell(Cell* cell)
{
    Cell* parent = parentCell();
    if (!parent)
        return;

    if (parent->isMarkedBlack() && cell->isMarkedGray()) {
        failures++;

        fprintf(stderr, "Found black to gray edge to ");
        dumpCellInfo(cell);
        fprintf(stderr, "\n");
        dumpCellPath();

#ifdef DEBUG
        if (cell->is<JSObject>()) {
            fprintf(stderr, "\n");
            DumpObject(cell->as<JSObject>(), stderr);
        }
#endif
    }
}

bool
CheckGrayMarkingTracer::check(AutoTraceSession& session)
{
    if (!traceHeap(session))
        return true; // Ignore failure.

    return failures == 0;
}

JS_FRIEND_API(bool)
js::CheckGrayMarkingState(JSRuntime* rt)
{
    MOZ_ASSERT(!JS::CurrentThreadIsHeapCollecting());
    MOZ_ASSERT(!rt->gc.isIncrementalGCInProgress());
    if (!rt->gc.areGrayBitsValid())
        return true;

    gcstats::AutoPhase ap(rt->gc.stats(), gcstats::PhaseKind::TRACE_HEAP);
    AutoTraceSession session(rt, JS::HeapState::Tracing);
    CheckGrayMarkingTracer tracer(rt);
    if (!tracer.init())
        return true; // Ignore failure

    return tracer.check(session);
}

#endif // defined(JS_GC_ZEAL) || defined(DEBUG)
