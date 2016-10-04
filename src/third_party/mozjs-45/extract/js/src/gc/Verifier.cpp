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
class js::VerifyPreTracer : public JS::CallbackTracer
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
      : JS::CallbackTracer(rt), noggc(rt), number(rt->gc.gcNumber()), count(0), curnode(nullptr),
        root(nullptr), edgeptr(nullptr), term(nullptr)
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

    evictNursery();

    AutoPrepareForTracing prep(rt, WithAtoms);

    if (!IsIncrementalGCSafe(rt))
        return;

    for (auto chunk = allNonEmptyChunks(); !chunk.done(); chunk.next())
        chunk->bitmap.clear();

    number++;

    VerifyPreTracer* trc = js_new<VerifyPreTracer>(rt);
    if (!trc)
        return;

    gcstats::AutoPhase ap(stats, gcstats::PHASE_TRACE_HEAP);

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
                js::TraceChildren(trc, e.thing, e.kind);
            }
            if (trc->edgeptr == trc->term)
                goto oom;
        }

        node = NextNode(node);
    }

    verifyPreData = trc;
    incrementalState = MARK;
    marker.start();

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

static void
AssertMarkedOrAllocated(const EdgeValue& edge)
{
    if (!edge.thing || IsMarkedOrAllocated(TenuredCell::fromPointer(edge.thing)))
        return;

    // Permanent atoms and well-known symbols aren't marked during graph traversal.
    if (edge.kind == JS::TraceKind::String && static_cast<JSString*>(edge.thing)->isPermanentAtom())
        return;
    if (edge.kind == JS::TraceKind::Symbol && static_cast<JS::Symbol*>(edge.thing)->isWellKnownSymbol())
        return;

    char msgbuf[1024];
    JS_snprintf(msgbuf, sizeof(msgbuf), "[barrier verifier] Unmarked edge: %s", edge.label);
    MOZ_ReportAssertionFailure(msgbuf, __FILE__, __LINE__);
    MOZ_CRASH();
}

bool
gc::GCRuntime::endVerifyPreBarriers()
{
    VerifyPreTracer* trc = verifyPreData;

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

    /*
     * We need to bump gcNumber so that the methodjit knows that jitcode has
     * been discarded.
     */
    MOZ_ASSERT(trc->number == number);
    number++;

    verifyPreData = nullptr;
    incrementalState = NO_INCREMENTAL;

    if (!compartmentCreated && IsIncrementalGCSafe(rt)) {
        CheckEdgeTracer cetrc(rt);

        /* Start after the roots. */
        VerifyNode* node = NextNode(trc->root);
        while ((char*)node < trc->edgeptr) {
            cetrc.node = node;
            js::TraceChildren(&cetrc, node->thing, node->kind);

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
    if (zealMode != ZealVerifierPreValue)
        return;

    if (rt->mainThread.suppressGC)
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
        js_delete(verifyPreData);
        verifyPreData = nullptr;
    }
}

#endif /* JS_GC_ZEAL */
