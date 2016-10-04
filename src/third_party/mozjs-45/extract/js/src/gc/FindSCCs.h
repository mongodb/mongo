/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_FindSCCs_h
#define gc_FindSCCs_h

#include "jsfriendapi.h"
#include "jsutil.h"

namespace js {
namespace gc {

template<class Node>
struct GraphNodeBase
{
    Node*          gcNextGraphNode;
    Node*          gcNextGraphComponent;
    unsigned       gcDiscoveryTime;
    unsigned       gcLowLink;

    GraphNodeBase()
      : gcNextGraphNode(nullptr),
        gcNextGraphComponent(nullptr),
        gcDiscoveryTime(0),
        gcLowLink(0) {}

    ~GraphNodeBase() {}

    Node* nextNodeInGroup() const {
        if (gcNextGraphNode && gcNextGraphNode->gcNextGraphComponent == gcNextGraphComponent)
            return gcNextGraphNode;
        return nullptr;
    }

    Node* nextGroup() const {
        return gcNextGraphComponent;
    }
};

/*
 * Find the strongly connected components of a graph using Tarjan's algorithm,
 * and return them in topological order.
 *
 * Nodes derive from GraphNodeBase and implement findGraphEdges, which calls
 * finder.addEdgeTo to describe the outgoing edges from that node:
 *
 * struct MyGraphNode : public GraphNodeBase
 * {
 *     void findOutgoingEdges(ComponentFinder<MyGraphNode>& finder)
 *     {
 *         for edge in my_outgoing_edges:
 *             if is_relevant(edge):
 *                 finder.addEdgeTo(edge.destination)
 *     }
 * }
 *
 * ComponentFinder<MyGraphNode> finder;
 * finder.addNode(v);
 */
template<class Node>
class ComponentFinder
{
  public:
    explicit ComponentFinder(uintptr_t sl)
      : clock(1),
        stack(nullptr),
        firstComponent(nullptr),
        cur(nullptr),
        stackLimit(sl),
        stackFull(false)
    {}

    ~ComponentFinder() {
        MOZ_ASSERT(!stack);
        MOZ_ASSERT(!firstComponent);
    }

    /* Forces all nodes to be added to a single component. */
    void useOneComponent() { stackFull = true; }

    void addNode(Node* v) {
        if (v->gcDiscoveryTime == Undefined) {
            MOZ_ASSERT(v->gcLowLink == Undefined);
            processNode(v);
        }
    }

    Node* getResultsList() {
        if (stackFull) {
            /*
             * All nodes after the stack overflow are in |stack|. Put them all in
             * one big component of their own.
             */
            Node* firstGoodComponent = firstComponent;
            for (Node* v = stack; v; v = stack) {
                stack = v->gcNextGraphNode;
                v->gcNextGraphComponent = firstGoodComponent;
                v->gcNextGraphNode = firstComponent;
                firstComponent = v;
            }
            stackFull = false;
        }

        MOZ_ASSERT(!stack);

        Node* result = firstComponent;
        firstComponent = nullptr;

        for (Node* v = result; v; v = v->gcNextGraphNode) {
            v->gcDiscoveryTime = Undefined;
            v->gcLowLink = Undefined;
        }

        return result;
    }

    static void mergeGroups(Node* first) {
        for (Node* v = first; v; v = v->gcNextGraphNode)
            v->gcNextGraphComponent = nullptr;
    }

  public:
    /* Call from implementation of GraphNodeBase::findOutgoingEdges(). */
    void addEdgeTo(Node* w) {
        if (w->gcDiscoveryTime == Undefined) {
            processNode(w);
            cur->gcLowLink = Min(cur->gcLowLink, w->gcLowLink);
        } else if (w->gcDiscoveryTime != Finished) {
            cur->gcLowLink = Min(cur->gcLowLink, w->gcDiscoveryTime);
        }
    }

  private:
    /* Constant used to indicate an unprocessed vertex. */
    static const unsigned Undefined = 0;

    /* Constant used to indicate an processed vertex that is no longer on the stack. */
    static const unsigned Finished = (unsigned)-1;

    void processNode(Node* v) {
        v->gcDiscoveryTime = clock;
        v->gcLowLink = clock;
        ++clock;

        v->gcNextGraphNode = stack;
        stack = v;

        int stackDummy;
        if (stackFull || !JS_CHECK_STACK_SIZE(stackLimit, &stackDummy)) {
            stackFull = true;
            return;
        }

        Node* old = cur;
        cur = v;
        cur->findOutgoingEdges(*this);
        cur = old;

        if (stackFull)
            return;

        if (v->gcLowLink == v->gcDiscoveryTime) {
            Node* nextComponent = firstComponent;
            Node* w;
            do {
                MOZ_ASSERT(stack);
                w = stack;
                stack = w->gcNextGraphNode;

                /*
                 * Record that the element is no longer on the stack by setting the
                 * discovery time to a special value that's not Undefined.
                 */
                w->gcDiscoveryTime = Finished;

                /* Figure out which group we're in. */
                w->gcNextGraphComponent = nextComponent;

                /*
                 * Prepend the component to the beginning of the output list to
                 * reverse the list and achieve the desired order.
                 */
                w->gcNextGraphNode = firstComponent;
                firstComponent = w;
            } while (w != v);
        }
    }

  private:
    unsigned       clock;
    Node*          stack;
    Node*          firstComponent;
    Node*          cur;
    uintptr_t      stackLimit;
    bool           stackFull;
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_FindSCCs_h */
