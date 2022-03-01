/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_UbiNodeBreadthFirst_h
#define js_UbiNodeBreadthFirst_h

#include "js/HashTable.h"
#include "js/UbiNode.h"
#include "js/Vector.h"

namespace JS {
namespace ubi {

// A breadth-first traversal template for graphs of ubi::Nodes.
//
// No GC may occur while an instance of this template is live.
//
// The provided Handler type should have two members:
//
//   typename NodeData;
//
//      The value type of |BreadthFirst<Handler>::visited|, the HashMap of
//      ubi::Nodes that have been visited so far. Since the algorithm needs a
//      hash table like this for its own use anyway, it is simple to let
//      Handler store its own metadata about each node in the same table.
//
//      For example, if you want to find a shortest path to each node from any
//      traversal starting point, your |NodeData| type could record the first
//      edge to reach each node, and the node from which it originates. Then,
//      when the traversal is complete, you can walk backwards from any node
//      to some starting point, and the path recorded will be a shortest path.
//
//      This type must have a default constructor. If this type owns any other
//      resources, move constructors and assignment operators are probably a
//      good idea, too.
//
//   bool operator() (BreadthFirst& traversal,
//                    Node origin, const Edge& edge,
//                    Handler::NodeData* referentData, bool first);
//
//      The visitor function, called to report that we have traversed
//      |edge| from |origin|. This is called once for each edge we traverse.
//      As this is a breadth-first search, any prior calls to the visitor
//      function were for origin nodes not further from the start nodes than
//      |origin|.
//
//      |traversal| is this traversal object, passed along for convenience.
//
//      |referentData| is a pointer to the value of the entry in
//      |traversal.visited| for |edge.referent|; the visitor function can
//      store whatever metadata it likes about |edge.referent| there.
//
//      |first| is true if this is the first time we have visited an edge
//      leading to |edge.referent|. This could be stored in NodeData, but
//      the algorithm knows whether it has just created the entry in
//      |traversal.visited|, so it passes it along for convenience.
//
//      The visitor function may call |traversal.abandonReferent()| if it
//      doesn't want to traverse the outgoing edges of |edge.referent|. You can
//      use this to limit the traversal to a given portion of the graph: it will
//      never visit nodes reachable only through nodes that you have abandoned.
//      Note that |abandonReferent| must be called the first time the given node
//      is reached; that is, |first| must be true.
//
//      The visitor function may call |doNotMarkReferentAsVisited()| if it
//      does not want a node to be considered 'visited' (and added to the
//      'visited' set). This is useful when the visitor has custom logic to
//      determine whether an edge is 'interesting'.
//
//      The visitor function may call |traversal.stop()| if it doesn't want
//      to visit any more nodes at all.
//
//      The visitor function may consult |traversal.visited| for information
//      about other nodes, but it should not add or remove entries.
//
//      The visitor function should return true on success, or false if an
//      error occurs. A false return value terminates the traversal
//      immediately, and causes BreadthFirst<Handler>::traverse to return
//      false.
template <typename Handler>
struct BreadthFirst {
  // Construct a breadth-first traversal object that reports the nodes it
  // reaches to |handler|. The traversal asserts that no GC happens in its
  // runtime during its lifetime.
  //
  // We do nothing with noGC, other than require it to exist, with a lifetime
  // that encloses our own.
  BreadthFirst(JSContext* cx, Handler& handler, const JS::AutoRequireNoGC& noGC)
      : wantNames(true),
        cx(cx),
        visited(),
        handler(handler),
        pending(),
        traversalBegun(false),
        stopRequested(false),
        abandonRequested(false),
        markReferentAsVisited(false) {}

  // Add |node| as a starting point for the traversal. You may add
  // as many starting points as you like. Return false on OOM.
  bool addStart(Node node) { return pending.append(node); }

  // Add |node| as a starting point for the traversal (see addStart) and also
  // add it to the |visited| set. Return false on OOM.
  bool addStartVisited(Node node) {
    typename NodeMap::AddPtr ptr = visited.lookupForAdd(node);
    if (!ptr && !visited.add(ptr, node, typename Handler::NodeData())) {
      return false;
    }
    return addStart(node);
  }

  // True if the handler wants us to compute edge names; doing so can be
  // expensive in time and memory. True by default.
  bool wantNames;

  // Traverse the graph in breadth-first order, starting at the given
  // start nodes, applying |handler::operator()| for each edge traversed
  // as described above.
  //
  // This should be called only once per instance of this class.
  //
  // Return false on OOM or error return from |handler::operator()|.
  bool traverse() {
    MOZ_ASSERT(!traversalBegun);
    traversalBegun = true;

    // While there are pending nodes, visit them.
    while (!pending.empty()) {
      Node origin = pending.front();
      pending.popFront();

      // Get a range containing all origin's outgoing edges.
      auto range = origin.edges(cx, wantNames);
      if (!range) {
        return false;
      }

      // Traverse each edge.
      for (; !range->empty(); range->popFront()) {
        MOZ_ASSERT(!stopRequested);

        Edge& edge = range->front();
        typename NodeMap::AddPtr a = visited.lookupForAdd(edge.referent);
        bool first = !a;

        // Pass a pointer to a stack-allocated NodeData if the referent is not
        // in |visited|.
        typename Handler::NodeData nodeData;
        typename Handler::NodeData* nodeDataPtr =
            first ? &nodeData : &a->value();

        // Report this edge to the visitor function.
        markReferentAsVisited = true;
        if (!handler(*this, origin, edge, nodeDataPtr, first)) {
          return false;
        }

        if (first && markReferentAsVisited) {
          // This is the first time we've reached |edge.referent| and the
          // handler wants it marked as visited.
          if (!visited.add(a, edge.referent, std::move(nodeData))) {
            return false;
          }
        }

        if (stopRequested) {
          return true;
        }

        // Arrange to traverse this edge's referent's outgoing edges
        // later --- unless |handler| asked us not to.
        if (abandonRequested) {
          // Skip the enqueue; reset flag for future iterations.
          abandonRequested = false;
        } else if (first) {
          if (!pending.append(edge.referent)) {
            return false;
          }
        }
      }
    }

    return true;
  }

  // Stop traversal, and return true from |traverse| without visiting any
  // more nodes. Only |handler::operator()| should call this function; it
  // may do so to stop the traversal early, without returning false and
  // then making |traverse|'s caller disambiguate that result from a real
  // error.
  void stop() { stopRequested = true; }

  // Request that the current edge's referent's outgoing edges not be
  // traversed. This must be called the first time that referent is reached.
  // Other edges *to* that referent will still be traversed.
  void abandonReferent() { abandonRequested = true; }

  // Request the the current edge's referent not be added to the |visited| set
  // if this is the first time we're visiting it.
  void doNotMarkReferentAsVisited() { markReferentAsVisited = false; }

  // The context with which we were constructed.
  JSContext* cx;

  // A map associating each node N that we have reached with a
  // Handler::NodeData, for |handler|'s use. This is public, so that
  // |handler| can access it to see the traversal thus far.
  using NodeMap = js::HashMap<Node, typename Handler::NodeData,
                              js::DefaultHasher<Node>, js::SystemAllocPolicy>;
  NodeMap visited;

 private:
  // Our handler object.
  Handler& handler;

  // A queue template. Appending and popping the front are constant time.
  // Wasted space is never more than some recent actual population plus the
  // current population.
  template <typename T>
  class Queue {
    js::Vector<T, 0, js::SystemAllocPolicy> head, tail;
    size_t frontIndex;

   public:
    Queue() : head(), tail(), frontIndex(0) {}
    bool empty() { return frontIndex >= head.length(); }
    T& front() {
      MOZ_ASSERT(!empty());
      return head[frontIndex];
    }
    void popFront() {
      MOZ_ASSERT(!empty());
      frontIndex++;
      if (frontIndex >= head.length()) {
        head.clearAndFree();
        head.swap(tail);
        frontIndex = 0;
      }
    }
    bool append(const T& elt) {
      return frontIndex == 0 ? head.append(elt) : tail.append(elt);
    }
  };

  // A queue of nodes that we have reached, but whose outgoing edges we
  // have not yet traversed. Nodes reachable in fewer edges are enqueued
  // earlier.
  Queue<Node> pending;

  // True if our traverse function has been called.
  bool traversalBegun;

  // True if we've been asked to stop the traversal.
  bool stopRequested;

  // True if we've been asked to abandon the current edge's referent.
  bool abandonRequested;

  // True if the node should be added to the |visited| set after calling the
  // handler.
  bool markReferentAsVisited;
};

}  // namespace ubi
}  // namespace JS

#endif  // js_UbiNodeBreadthFirst_h
