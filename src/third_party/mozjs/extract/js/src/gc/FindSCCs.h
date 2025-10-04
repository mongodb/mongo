/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_FindSCCs_h
#define gc_FindSCCs_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <algorithm>  // std::min

#include "js/AllocPolicy.h"         // js::SystemAllocPolicy
#include "js/friend/StackLimits.h"  // js::AutoCheckRecursionLimit
#include "js/HashTable.h"           // js::HashSet, js::DefaultHasher

namespace js {
namespace gc {

template <typename Node>
struct GraphNodeBase {
  using NodeSet =
      js::HashSet<Node*, js::DefaultHasher<Node*>, js::SystemAllocPolicy>;

  NodeSet gcGraphEdges;
  Node* gcNextGraphNode = nullptr;
  Node* gcNextGraphComponent = nullptr;
  unsigned gcDiscoveryTime = 0;
  unsigned gcLowLink = 0;

  Node* nextNodeInGroup() const {
    if (gcNextGraphNode &&
        gcNextGraphNode->gcNextGraphComponent == gcNextGraphComponent) {
      return gcNextGraphNode;
    }
    return nullptr;
  }

  Node* nextGroup() const { return gcNextGraphComponent; }
};

/*
 * Find the strongly connected components of a graph using Tarjan's algorithm,
 * and return them in topological order.
 *
 * Nodes derive from GraphNodeBase and add target edge pointers to
 * sourceNode.gcGraphEdges to describe the graph:
 *
 * struct MyGraphNode : public GraphNodeBase<MyGraphNode>
 * {
 *   ...
 * }
 *
 * MyGraphNode node1, node2, node3;
 * node1.gcGraphEdges.put(node2); // Error checking elided.
 * node2.gcGraphEdges.put(node3);
 * node3.gcGraphEdges.put(node2);
 *
 * ComponentFinder<MyGraphNode> finder;
 * finder.addNode(node1);
 * finder.addNode(node2);
 * finder.addNode(node3);
 * MyGraphNode* result = finder.getResultsList();
 */

template <typename Node>
class ComponentFinder {
 public:
  explicit ComponentFinder(JSContext* cx) : cx(cx) {}

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
    for (Node* v = first; v; v = v->gcNextGraphNode) {
      v->gcNextGraphComponent = nullptr;
    }
  }

 private:
  // Constant used to indicate an unprocessed vertex.
  static const unsigned Undefined = 0;

  // Constant used to indicate a processed vertex that is no longer on the
  // stack.
  static const unsigned Finished = (unsigned)-1;

  void addEdgeTo(Node* w) {
    if (w->gcDiscoveryTime == Undefined) {
      processNode(w);
      cur->gcLowLink = std::min(cur->gcLowLink, w->gcLowLink);
    } else if (w->gcDiscoveryTime != Finished) {
      cur->gcLowLink = std::min(cur->gcLowLink, w->gcDiscoveryTime);
    }
  }

  void processNode(Node* v) {
    v->gcDiscoveryTime = clock;
    v->gcLowLink = clock;
    ++clock;

    v->gcNextGraphNode = stack;
    stack = v;

    if (stackFull) {
      return;
    }

    AutoCheckRecursionLimit recursion(cx);
    if (!recursion.checkSystemDontReport(cx)) {
      stackFull = true;
      return;
    }

    Node* old = cur;
    cur = v;
    for (auto r = cur->gcGraphEdges.all(); !r.empty(); r.popFront()) {
      addEdgeTo(r.front());
    }
    cur = old;

    if (stackFull) {
      return;
    }

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
  unsigned clock = 1;
  Node* stack = nullptr;
  Node* firstComponent = nullptr;
  Node* cur = nullptr;
  JSContext* cx;
  bool stackFull = false;
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_FindSCCs_h */
