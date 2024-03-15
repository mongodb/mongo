/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_UbiNodeShortestPaths_h
#define js_UbiNodeShortestPaths_h

#include "mozilla/Maybe.h"

#include <utility>

#include "js/AllocPolicy.h"
#include "js/GCAPI.h"
#include "js/UbiNode.h"
#include "js/UbiNodeBreadthFirst.h"
#include "js/UniquePtr.h"

namespace JS {
namespace ubi {

/**
 * A back edge along a path in the heap graph.
 */
struct JS_PUBLIC_API BackEdge {
 private:
  Node predecessor_;
  EdgeName name_;

 public:
  using Ptr = js::UniquePtr<BackEdge>;

  BackEdge() : predecessor_(), name_(nullptr) {}

  [[nodiscard]] bool init(const Node& predecessor, Edge& edge) {
    MOZ_ASSERT(!predecessor_);
    MOZ_ASSERT(!name_);

    predecessor_ = predecessor;
    name_ = std::move(edge.name);
    return true;
  }

  BackEdge(const BackEdge&) = delete;
  BackEdge& operator=(const BackEdge&) = delete;

  BackEdge(BackEdge&& rhs)
      : predecessor_(rhs.predecessor_), name_(std::move(rhs.name_)) {
    MOZ_ASSERT(&rhs != this);
  }

  BackEdge& operator=(BackEdge&& rhs) {
    this->~BackEdge();
    new (this) BackEdge(std::move(rhs));
    return *this;
  }

  Ptr clone() const;

  const EdgeName& name() const { return name_; }
  EdgeName& name() { return name_; }

  const JS::ubi::Node& predecessor() const { return predecessor_; }
};

/**
 * A path is a series of back edges from which we discovered a target node.
 */
using Path = JS::ubi::Vector<BackEdge*>;

/**
 * The `JS::ubi::ShortestPaths` type represents a collection of up to N shortest
 * retaining paths for each of a target set of nodes, starting from the same
 * root node.
 */
struct JS_PUBLIC_API ShortestPaths {
 private:
  // Types, type aliases, and data members.

  using BackEdgeVector = JS::ubi::Vector<BackEdge::Ptr>;
  using NodeToBackEdgeVectorMap =
      js::HashMap<Node, BackEdgeVector, js::DefaultHasher<Node>,
                  js::SystemAllocPolicy>;

  struct Handler;
  using Traversal = BreadthFirst<Handler>;

  /**
   * A `JS::ubi::BreadthFirst` traversal handler that records back edges for
   * how we reached each node, allowing us to reconstruct the shortest
   * retaining paths after the traversal.
   */
  struct Handler {
    using NodeData = BackEdge;

    ShortestPaths& shortestPaths;
    size_t totalMaxPathsToRecord;
    size_t totalPathsRecorded;

    explicit Handler(ShortestPaths& shortestPaths)
        : shortestPaths(shortestPaths),
          totalMaxPathsToRecord(shortestPaths.targets_.count() *
                                shortestPaths.maxNumPaths_),
          totalPathsRecorded(0) {}

    bool operator()(Traversal& traversal, const JS::ubi::Node& origin,
                    JS::ubi::Edge& edge, BackEdge* back, bool first) {
      MOZ_ASSERT(back);
      MOZ_ASSERT(origin == shortestPaths.root_ ||
                 traversal.visited.has(origin));
      MOZ_ASSERT(totalPathsRecorded < totalMaxPathsToRecord);

      if (first && !back->init(origin, edge)) {
        return false;
      }

      if (!shortestPaths.targets_.has(edge.referent)) {
        return true;
      }

      // If `first` is true, then we moved the edge's name into `back` in
      // the above call to `init`. So clone that back edge to get the
      // correct edge name. If `first` is not true, then our edge name is
      // still in `edge`. This accounts for the asymmetry between
      // `back->clone()` in the first branch, and the `init` call in the
      // second branch.

      if (first) {
        BackEdgeVector paths;
        if (!paths.reserve(shortestPaths.maxNumPaths_)) {
          return false;
        }
        auto cloned = back->clone();
        if (!cloned) {
          return false;
        }
        paths.infallibleAppend(std::move(cloned));
        if (!shortestPaths.paths_.putNew(edge.referent, std::move(paths))) {
          return false;
        }
        totalPathsRecorded++;
      } else {
        auto ptr = shortestPaths.paths_.lookup(edge.referent);
        MOZ_ASSERT(ptr,
                   "This isn't the first time we have seen the target node "
                   "`edge.referent`. "
                   "We should have inserted it into shortestPaths.paths_ the "
                   "first time we "
                   "saw it.");

        if (ptr->value().length() < shortestPaths.maxNumPaths_) {
          auto thisBackEdge = js::MakeUnique<BackEdge>();
          if (!thisBackEdge || !thisBackEdge->init(origin, edge)) {
            return false;
          }
          ptr->value().infallibleAppend(std::move(thisBackEdge));
          totalPathsRecorded++;
        }
      }

      MOZ_ASSERT(totalPathsRecorded <= totalMaxPathsToRecord);
      if (totalPathsRecorded == totalMaxPathsToRecord) {
        traversal.stop();
      }

      return true;
    }
  };

  // The maximum number of paths to record for each node.
  uint32_t maxNumPaths_;

  // The root node we are starting the search from.
  Node root_;

  // The set of nodes we are searching for paths to.
  NodeSet targets_;

  // The resulting paths.
  NodeToBackEdgeVectorMap paths_;

  // Need to keep alive the traversal's back edges so we can walk them later
  // when the traversal is over when recreating the shortest paths.
  Traversal::NodeMap backEdges_;

 private:
  // Private methods.

  ShortestPaths(uint32_t maxNumPaths, const Node& root, NodeSet&& targets)
      : maxNumPaths_(maxNumPaths),
        root_(root),
        targets_(std::move(targets)),
        paths_(targets_.count()),
        backEdges_() {
    MOZ_ASSERT(maxNumPaths_ > 0);
    MOZ_ASSERT(root_);
  }

 public:
  // Public methods.

  ShortestPaths(ShortestPaths&& rhs)
      : maxNumPaths_(rhs.maxNumPaths_),
        root_(rhs.root_),
        targets_(std::move(rhs.targets_)),
        paths_(std::move(rhs.paths_)),
        backEdges_(std::move(rhs.backEdges_)) {
    MOZ_ASSERT(this != &rhs, "self-move is not allowed");
  }

  ShortestPaths& operator=(ShortestPaths&& rhs) {
    this->~ShortestPaths();
    new (this) ShortestPaths(std::move(rhs));
    return *this;
  }

  ShortestPaths(const ShortestPaths&) = delete;
  ShortestPaths& operator=(const ShortestPaths&) = delete;

  /**
   * Construct a new `JS::ubi::ShortestPaths`, finding up to `maxNumPaths`
   * shortest retaining paths for each target node in `targets` starting from
   * `root`.
   *
   * The resulting `ShortestPaths` instance must not outlive the
   * `JS::ubi::Node` graph it was constructed from.
   *
   *   - For `JS::ubi::Node` graphs backed by the live heap graph, this means
   *     that the `ShortestPaths`'s lifetime _must_ be contained within the
   *     scope of the provided `AutoCheckCannotGC` reference because a GC will
   *     invalidate the nodes.
   *
   *   - For `JS::ubi::Node` graphs backed by some other offline structure
   *     provided by the embedder, the resulting `ShortestPaths`'s lifetime is
   *     bounded by that offline structure's lifetime.
   *
   * Returns `mozilla::Nothing()` on OOM failure. It is the caller's
   * responsibility to handle and report the OOM.
   */
  static mozilla::Maybe<ShortestPaths> Create(JSContext* cx,
                                              AutoCheckCannotGC& noGC,
                                              uint32_t maxNumPaths,
                                              const Node& root,
                                              NodeSet&& targets) {
    MOZ_ASSERT(targets.count() > 0);
    MOZ_ASSERT(maxNumPaths > 0);

    ShortestPaths paths(maxNumPaths, root, std::move(targets));

    Handler handler(paths);
    Traversal traversal(cx, handler, noGC);
    traversal.wantNames = true;
    if (!traversal.addStart(root) || !traversal.traverse()) {
      return mozilla::Nothing();
    }

    // Take ownership of the back edges we created while traversing the
    // graph so that we can follow them from `paths_` and don't
    // use-after-free.
    paths.backEdges_ = std::move(traversal.visited);

    return mozilla::Some(std::move(paths));
  }

  /**
   * Get an iterator over each target node we searched for retaining paths
   * for. The returned iterator must not outlive the `ShortestPaths`
   * instance.
   */
  NodeSet::Iterator targetIter() const { return targets_.iter(); }

  /**
   * Invoke the provided functor/lambda/callable once for each retaining path
   * discovered for `target`. The `func` is passed a single `JS::ubi::Path&`
   * argument, which contains each edge along the path ordered starting from
   * the root and ending at the target, and must not outlive the scope of the
   * call.
   *
   * Note that it is possible that we did not find any paths from the root to
   * the given target, in which case `func` will not be invoked.
   */
  template <class Func>
  [[nodiscard]] bool forEachPath(const Node& target, Func func) {
    MOZ_ASSERT(targets_.has(target));

    auto ptr = paths_.lookup(target);

    // We didn't find any paths to this target, so nothing to do here.
    if (!ptr) {
      return true;
    }

    MOZ_ASSERT(ptr->value().length() <= maxNumPaths_);

    Path path;
    for (const auto& backEdge : ptr->value()) {
      path.clear();

      if (!path.append(backEdge.get())) {
        return false;
      }

      Node here = backEdge->predecessor();
      MOZ_ASSERT(here);

      while (here != root_) {
        auto p = backEdges_.lookup(here);
        MOZ_ASSERT(p);
        if (!path.append(&p->value())) {
          return false;
        }
        here = p->value().predecessor();
        MOZ_ASSERT(here);
      }

      path.reverse();

      if (!func(path)) {
        return false;
      }
    }

    return true;
  }
};

#ifdef DEBUG
// A helper function to dump the first `maxNumPaths` shortest retaining paths to
// `node` from the GC roots. Useful when GC things you expect to have been
// reclaimed by the collector haven't been!
//
// Usage:
//
//     JSObject* foo = ...;
//     JS::ubi::dumpPaths(rt, JS::ubi::Node(foo));
JS_PUBLIC_API void dumpPaths(JSRuntime* rt, Node node,
                             uint32_t maxNumPaths = 10);
#endif

}  // namespace ubi
}  // namespace JS

#endif  // js_UbiNodeShortestPaths_h
