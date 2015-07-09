/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef MOZ_VALGRIND
# include <valgrind/memcheck.h>
#endif

#include "jscntxt.h"
#include "jsgc.h"
#include "jsprf.h"

#include "gc/GCInternals.h"
#include "gc/Zone.h"
#include "js/GCAPI.h"
#include "js/HashTable.h"

#include "jscntxtinlines.h"
#include "jsgcinlines.h"

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
 *
 * Post-Barrier Verifier:
 *   When StartVerifyBarriers is called, we create a virtual "Nursery Set" which
 *   future allocations are recorded in and turn on the StoreBuffer. Later,
 *   EndVerifyBarriers traverses the heap and ensures that the set of cross-
 *   generational pointers we find is a subset of the pointers recorded in our
 *   StoreBuffer.
 */

struct EdgeValue
{
    void* thing;
    JSGCTraceKind kind;
    const char* label;
};

struct VerifyNode
{
    void* thing;
    JSGCTraceKind kind;
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
struct VerifyPreTracer : JSTracer
{
    JS::AutoDisableGenerationalGC noggc;

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

    VerifyPreTracer(JSRuntime* rt, JSTraceCallback callback)
      : JSTracer(rt, callback), noggc(rt), number(rt->gc.gcNumber()), count(0), root(nullptr)
    {}

    ~VerifyPreTracer() {
        js_free(root);
    }
};

/*
 * This function builds up the heap snapshot by adding edges to the current
 * node.
 */
static void
AccumulateEdge(JSTracer* jstrc, void** thingp, JSGCTraceKind kind)
{
    VerifyPreTracer* trc = (VerifyPreTracer*)jstrc;

    MOZ_ASSERT(!IsInsideNursery(*reinterpret_cast<Cell**>(thingp)));

    trc->edgeptr += sizeof(EdgeValue);
    if (trc->edgeptr >= trc->term) {
        trc->edgeptr = trc->term;
        return;
    }

    VerifyNode* node = trc->curnode;
    uint32_t i = node->count;

    node->edges[i].thing = *thingp;
    node->edges[i].kind = kind;
    node->edges[i].label = trc->tracingName("<unknown>");
    node->count++;
}

static VerifyNode*
MakeNode(VerifyPreTracer* trc, void* thing, JSGCTraceKind kind)
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
        trc->nodemap.add(p, thing, node);
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

    /*
     * The post barrier verifier requires the storebuffer to be enabled, but the
     * pre barrier verifier disables it as part of disabling GGC.  Don't allow
     * starting the pre barrier verifier if the post barrier verifier is already
     * running.
     */
    if (verifyPostData)
        return;

    evictNursery();

    AutoPrepareForTracing prep(rt, WithAtoms);

    if (!IsIncrementalGCSafe(rt))
        return;

    for (auto chunk = allNonEmptyChunks(); !chunk.done(); chunk.next())
        chunk->bitmap.clear();

    number++;

    VerifyPreTracer* trc = js_new<VerifyPreTracer>(rt, JSTraceCallback(nullptr));
    if (!trc)
        return;

    gcstats::AutoPhase ap(stats, gcstats::PHASE_TRACE_HEAP);

    /*
     * Passing a function pointer directly to js_new trips a compiler bug in
     * MSVC. Work around by filling the pointer after allocating with nullptr.
     */
    trc->setTraceCallback(AccumulateEdge);

    const size_t size = 64 * 1024 * 1024;
    trc->root = (VerifyNode*)js_malloc(size);
    if (!trc->root)
        goto oom;
    trc->edgeptr = (char*)trc->root;
    trc->term = trc->edgeptr + size;

    if (!trc->nodemap.init())
        goto oom;

    /* Create the root node. */
    trc->curnode = MakeNode(trc, nullptr, JSGCTraceKind(0));

    incrementalState = MARK_ROOTS;

    /* Make all the roots be edges emanating from the root node. */
    markRuntime(trc);

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
                JS_TraceChildren(trc, e.thing, e.kind);
            }
            if (trc->edgeptr == trc->term)
                goto oom;
        }

        node = NextNode(node);
    }

    verifyPreData = trc;
    incrementalState = MARK;
    marker.start();

    rt->setNeedsIncrementalBarrier(true);
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        PurgeJITCaches(zone);
        zone->setNeedsIncrementalBarrier(true, Zone::UpdateJit);
        zone->arenas.purge();
    }

    return;

oom:
    incrementalState = NO_INCREMENTAL;
    js_delete(trc);
    verifyPreData = nullptr;
}

static bool
IsMarkedOrAllocated(TenuredCell* cell)
{
    return cell->isMarked() || cell->arenaHeader()->allocatedDuringIncremental;
}

static const uint32_t MAX_VERIFIER_EDGES = 1000;

/*
 * This function is called by EndVerifyBarriers for every heap edge. If the edge
 * already existed in the original snapshot, we "cancel it out" by overwriting
 * it with nullptr. EndVerifyBarriers later asserts that the remaining
 * non-nullptr edges (i.e., the ones from the original snapshot that must have
 * been modified) must point to marked objects.
 */
static void
CheckEdge(JSTracer* jstrc, void** thingp, JSGCTraceKind kind)
{
    VerifyPreTracer* trc = (VerifyPreTracer*)jstrc;
    VerifyNode* node = trc->curnode;

    /* Avoid n^2 behavior. */
    if (node->count > MAX_VERIFIER_EDGES)
        return;

    for (uint32_t i = 0; i < node->count; i++) {
        if (node->edges[i].thing == *thingp) {
            MOZ_ASSERT(node->edges[i].kind == kind);
            node->edges[i].thing = nullptr;
            return;
        }
    }
}

static void
AssertMarkedOrAllocated(const EdgeValue& edge)
{
    if (!edge.thing || IsMarkedOrAllocated(TenuredCell::fromPointer(edge.thing)))
        return;

    // Permanent atoms and well-known symbols aren't marked during graph traversal.
    if (edge.kind == JSTRACE_STRING && static_cast<JSString*>(edge.thing)->isPermanentAtom())
        return;
    if (edge.kind == JSTRACE_SYMBOL && static_cast<JS::Symbol*>(edge.thing)->isWellKnownSymbol())
        return;

    char msgbuf[1024];
    const char* label = edge.label;

    JS_snprintf(msgbuf, sizeof(msgbuf), "[barrier verifier] Unmarked edge: %s", label);
    MOZ_ReportAssertionFailure(msgbuf, __FILE__, __LINE__);
    MOZ_CRASH();
}

bool
gc::GCRuntime::endVerifyPreBarriers()
{
    VerifyPreTracer* trc = (VerifyPreTracer*)verifyPreData;

    if (!trc)
        return false;

    MOZ_ASSERT(!JS::IsGenerationalGCEnabled(rt));

    AutoPrepareForTracing prep(rt, SkipAtoms);

    bool compartmentCreated = false;

    /* We need to disable barriers before tracing, which may invoke barriers. */
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (!zone->needsIncrementalBarrier())
            compartmentCreated = true;

        zone->setNeedsIncrementalBarrier(false, Zone::UpdateJit);
        PurgeJITCaches(zone);
    }
    rt->setNeedsIncrementalBarrier(false);

    /*
     * We need to bump gcNumber so that the methodjit knows that jitcode has
     * been discarded.
     */
    MOZ_ASSERT(trc->number == number);
    number++;

    verifyPreData = nullptr;
    incrementalState = NO_INCREMENTAL;

    if (!compartmentCreated && IsIncrementalGCSafe(rt)) {
        trc->setTraceCallback(CheckEdge);

        /* Start after the roots. */
        VerifyNode* node = NextNode(trc->root);
        while ((char*)node < trc->edgeptr) {
            trc->curnode = node;
            JS_TraceChildren(trc, node->thing, node->kind);

            if (node->count <= MAX_VERIFIER_EDGES) {
                for (uint32_t i = 0; i < node->count; i++)
                    AssertMarkedOrAllocated(node->edges[i]);
            }

            node = NextNode(node);
        }
    }

    marker.reset();
    marker.stop();

    js_delete(trc);
    return true;
}

/*** Post-Barrier Verifyier ***/

struct VerifyPostTracer : JSTracer
{
    /* The gcNumber when the verification began. */
    uint64_t number;

    /* This counts up to gcZealFrequency to decide whether to verify. */
    int count;

    /* The set of edges in the StoreBuffer at the end of verification. */
    typedef HashSet<void**, PointerHasher<void**, 3>, SystemAllocPolicy> EdgeSet;
    EdgeSet* edges;

    VerifyPostTracer(JSRuntime* rt, JSTraceCallback callback)
      : JSTracer(rt, callback), number(rt->gc.gcNumber()), count(0)
    {}
};

/*
 * The post-barrier verifier runs the full store buffer and a fake nursery when
 * running and when it stops, walks the full heap to ensure that all the
 * important edges were inserted into the storebuffer.
 */
void
gc::GCRuntime::startVerifyPostBarriers()
{
    if (!JS::IsGenerationalGCEnabled(rt) || verifyPostData || isIncrementalGCInProgress())
        return;

    evictNursery();

    number++;

    VerifyPostTracer* trc = js_new<VerifyPostTracer>(rt, JSTraceCallback(nullptr));
    if (!trc)
        return;

    verifyPostData = trc;
}

void
PostVerifierCollectStoreBufferEdges(JSTracer* jstrc, void** thingp, JSGCTraceKind kind)
{
    VerifyPostTracer* trc = (VerifyPostTracer*)jstrc;

    /* The nursery only stores objects. */
    if (kind != JSTRACE_OBJECT)
        return;

    /* The store buffer may store extra, non-cross-generational edges. */
    JSObject* dst = *reinterpret_cast<JSObject**>(thingp);
    if (trc->runtime()->gc.nursery.isInside(thingp) || !IsInsideNursery(dst))
        return;

    /*
     * Values will be unpacked to the stack before getting here. However, the
     * only things that enter this callback are marked by the store buffer. The
     * store buffer ensures that the real tracing location is set correctly.
     */
    void** loc = trc->tracingLocation(thingp);

    trc->edges->put(loc);
}

static void
AssertStoreBufferContainsEdge(VerifyPostTracer::EdgeSet* edges, void** loc, JSObject* dst)
{
    if (edges->has(loc))
        return;

    char msgbuf[1024];
    JS_snprintf(msgbuf, sizeof(msgbuf), "[post-barrier verifier] Missing edge @ %p to %p",
                (void*)loc, (void*)dst);
    MOZ_ReportAssertionFailure(msgbuf, __FILE__, __LINE__);
    MOZ_CRASH();
}

void
PostVerifierVisitEdge(JSTracer* jstrc, void** thingp, JSGCTraceKind kind)
{
    VerifyPostTracer* trc = (VerifyPostTracer*)jstrc;

    /* The nursery only stores objects. */
    if (kind != JSTRACE_OBJECT)
        return;

    /* Filter out non cross-generational edges. */
    MOZ_ASSERT(!trc->runtime()->gc.nursery.isInside(thingp));
    JSObject* dst = *reinterpret_cast<JSObject**>(thingp);
    if (!IsInsideNursery(dst))
        return;

    /*
     * Values will be unpacked to the stack before getting here. However, the
     * only things that enter this callback are marked by the JS_TraceChildren
     * below. Since JSObject::markChildren handles this, the real trace
     * location will be set correctly in these cases.
     */
    void** loc = trc->tracingLocation(thingp);

    AssertStoreBufferContainsEdge(trc->edges, loc, dst);
}

bool
js::gc::GCRuntime::endVerifyPostBarriers()
{
    VerifyPostTracer* trc = (VerifyPostTracer*)verifyPostData;
    if (!trc)
        return false;

    VerifyPostTracer::EdgeSet edges;
    AutoPrepareForTracing prep(rt, SkipAtoms);

    /* Visit every entry in the store buffer and put the edges in a hash set. */
    trc->setTraceCallback(PostVerifierCollectStoreBufferEdges);
    if (!edges.init())
        goto oom;
    trc->edges = &edges;
    storeBuffer.markAll(trc);

    /* Walk the heap to find any edges not the the |edges| set. */
    trc->setTraceCallback(PostVerifierVisitEdge);
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        for (size_t kind = 0; kind < FINALIZE_LIMIT; ++kind) {
            for (ZoneCellIterUnderGC cells(zone, AllocKind(kind)); !cells.done(); cells.next()) {
                Cell* src = cells.getCell();
                JS_TraceChildren(trc, src, MapAllocToTraceKind(AllocKind(kind)));
            }
        }
    }

oom:
    js_delete(trc);
    verifyPostData = nullptr;
    return true;
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
gc::GCRuntime::verifyPostBarriers()
{
    if (verifyPostData)
        endVerifyPostBarriers();
    else
        startVerifyPostBarriers();
}

void
gc::VerifyBarriers(JSRuntime* rt, VerifierType type)
{
    if (type == PreBarrierVerifier)
        rt->gc.verifyPreBarriers();
    else
        rt->gc.verifyPostBarriers();
}

void
gc::GCRuntime::maybeVerifyPreBarriers(bool always)
{
    if (zealMode != ZealVerifierPreValue)
        return;

    if (rt->mainThread.suppressGC)
        return;

    if (VerifyPreTracer* trc = (VerifyPreTracer*)verifyPreData) {
        if (++trc->count < zealFrequency && !always)
            return;

        endVerifyPreBarriers();
    }

    startVerifyPreBarriers();
}

void
gc::GCRuntime::maybeVerifyPostBarriers(bool always)
{
    if (zealMode != ZealVerifierPostValue)
        return;

    if (rt->mainThread.suppressGC || !storeBuffer.isEnabled())
        return;

    if (VerifyPostTracer* trc = (VerifyPostTracer*)verifyPostData) {
        if (++trc->count < zealFrequency && !always)
            return;

        endVerifyPostBarriers();
    }
    startVerifyPostBarriers();
}

void
js::gc::MaybeVerifyBarriers(JSContext* cx, bool always)
{
    GCRuntime* gc = &cx->runtime()->gc;
    gc->maybeVerifyPreBarriers(always);
    gc->maybeVerifyPostBarriers(always);
}

void
js::gc::GCRuntime::finishVerifier()
{
    if (VerifyPreTracer* trc = (VerifyPreTracer*)verifyPreData) {
        js_delete(trc);
        verifyPreData = nullptr;
    }
    if (VerifyPostTracer* trc = (VerifyPostTracer*)verifyPostData) {
        js_delete(trc);
        verifyPostData = nullptr;
    }
}

#endif /* JS_GC_ZEAL */
