/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_UbiNodeDominatorTree_h
#define js_UbiNodeDominatorTree_h

#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"
#include "mozilla/Move.h"
#include "mozilla/UniquePtr.h"

#include "js/AllocPolicy.h"
#include "js/UbiNode.h"
#include "js/UbiNodePostOrder.h"
#include "js/Utility.h"
#include "js/Vector.h"

namespace JS {
namespace ubi {

/**
 * In a directed graph with a root node `R`, a node `A` is said to "dominate" a
 * node `B` iff every path from `R` to `B` contains `A`. A node `A` is said to
 * be the "immediate dominator" of a node `B` iff it dominates `B`, is not `B`
 * itself, and does not dominate any other nodes which also dominate `B` in
 * turn.
 *
 * If we take every node from a graph `G` and create a new graph `T` with edges
 * to each node from its immediate dominator, then `T` is a tree (each node has
 * only one immediate dominator, or none if it is the root). This tree is called
 * a "dominator tree".
 *
 * This class represents a dominator tree constructed from a `JS::ubi::Node`
 * heap graph. The domination relationship and dominator trees are useful tools
 * for analyzing heap graphs because they tell you:
 *
 *   - Exactly what could be reclaimed by the GC if some node `A` became
 *     unreachable: those nodes which are dominated by `A`,
 *
 *   - The "retained size" of a node in the heap graph, in contrast to its
 *     "shallow size". The "shallow size" is the space taken by a node itself,
 *     not counting anything it references. The "retained size" of a node is its
 *     shallow size plus the size of all the things that would be collected if
 *     the original node wasn't (directly or indirectly) referencing them. In
 *     other words, the retained size is the shallow size of a node plus the
 *     shallow sizes of every other node it dominates. For example, the root
 *     node in a binary tree might have a small shallow size that does not take
 *     up much space itself, but it dominates the rest of the binary tree and
 *     its retained size is therefore significant (assuming no external
 *     references into the tree).
 *
 * The simple, engineered algorithm presented in "A Simple, Fast Dominance
 * Algorithm" by Cooper el al[0] is used to find dominators and construct the
 * dominator tree. This algorithm runs in O(n^2) time, but is faster in practice
 * than alternative algorithms with better theoretical running times, such as
 * Lengauer-Tarjan which runs in O(e * log(n)). The big caveat to that statement
 * is that Cooper et al found it is faster in practice *on control flow graphs*
 * and I'm not convinced that this property also holds on *heap* graphs. That
 * said, the implementation of this algorithm is *much* simpler than
 * Lengauer-Tarjan and has been found to be fast enough at least for the time
 * being.
 *
 * [0]: http://www.cs.rice.edu/~keith/EMBED/dom.pdf
 */
class JS_PUBLIC_API(DominatorTree)
{
  private:
    // Types.

    using PredecessorSets = js::HashMap<Node, NodeSetPtr, js::DefaultHasher<Node>,
                                        js::SystemAllocPolicy>;
    using NodeToIndexMap = js::HashMap<Node, uint32_t, js::DefaultHasher<Node>,
                                       js::SystemAllocPolicy>;
    class DominatedSets;

  public:
    class DominatedSetRange;

    /**
     * A pointer to an immediately dominated node.
     *
     * Don't use this type directly; it is no safer than regular pointers. This
     * is only for use indirectly with range-based for loops and
     * `DominatedSetRange`.
     *
     * @see JS::ubi::DominatorTree::getDominatedSet
     */
    class DominatedNodePtr
    {
        friend class DominatedSetRange;

        const JS::ubi::Vector<Node>& postOrder;
        const uint32_t* ptr;

        DominatedNodePtr(const JS::ubi::Vector<Node>& postOrder, const uint32_t* ptr)
          : postOrder(postOrder)
          , ptr(ptr)
        { }

      public:
        bool operator!=(const DominatedNodePtr& rhs) const { return ptr != rhs.ptr; }
        void operator++() { ptr++; }
        const Node& operator*() const { return postOrder[*ptr]; }
    };

    /**
     * A range of immediately dominated `JS::ubi::Node`s for use with
     * range-based for loops.
     *
     * @see JS::ubi::DominatorTree::getDominatedSet
     */
    class DominatedSetRange
    {
        friend class DominatedSets;

        const JS::ubi::Vector<Node>& postOrder;
        const uint32_t* beginPtr;
        const uint32_t* endPtr;

        DominatedSetRange(JS::ubi::Vector<Node>& postOrder, const uint32_t* begin, const uint32_t* end)
          : postOrder(postOrder)
          , beginPtr(begin)
          , endPtr(end)
        {
            MOZ_ASSERT(begin <= end);
        }

      public:
        DominatedNodePtr begin() const {
            MOZ_ASSERT(beginPtr <= endPtr);
            return DominatedNodePtr(postOrder, beginPtr);
        }

        DominatedNodePtr end() const {
            return DominatedNodePtr(postOrder, endPtr);
        }

        size_t length() const {
            MOZ_ASSERT(beginPtr <= endPtr);
            return endPtr - beginPtr;
        }

        /**
         * Safely skip ahead `n` dominators in the range, in O(1) time.
         *
         * Example usage:
         *
         *     mozilla::Maybe<DominatedSetRange> range = myDominatorTree.getDominatedSet(myNode);
         *     if (range.isNothing()) {
         *         // Handle unknown nodes however you see fit...
         *         return false;
         *     }
         *
         *     // Don't care about the first ten, for whatever reason.
         *     range->skip(10);
         *     for (const JS::ubi::Node& dominatedNode : *range) {
         *         // ...
         *     }
         */
        void skip(size_t n) {
            beginPtr += n;
            if (beginPtr > endPtr)
                beginPtr = endPtr;
        }
    };

  private:
    /**
     * The set of all dominated sets in a dominator tree.
     *
     * Internally stores the sets in a contiguous array, with a side table of
     * indices into that contiguous array to denote the start index of each
     * individual set.
     */
    class DominatedSets
    {
        JS::ubi::Vector<uint32_t> dominated;
        JS::ubi::Vector<uint32_t> indices;

        DominatedSets(JS::ubi::Vector<uint32_t>&& dominated, JS::ubi::Vector<uint32_t>&& indices)
          : dominated(mozilla::Move(dominated))
          , indices(mozilla::Move(indices))
        { }

      public:
        // DominatedSets is not copy-able.
        DominatedSets(const DominatedSets& rhs) = delete;
        DominatedSets& operator=(const DominatedSets& rhs) = delete;

        // DominatedSets is move-able.
        DominatedSets(DominatedSets&& rhs)
          : dominated(mozilla::Move(rhs.dominated))
          , indices(mozilla::Move(rhs.indices))
        {
            MOZ_ASSERT(this != &rhs, "self-move not allowed");
        }
        DominatedSets& operator=(DominatedSets&& rhs) {
            this->~DominatedSets();
            new (this) DominatedSets(mozilla::Move(rhs));
            return *this;
        }

        /**
         * Create the DominatedSets given the mapping of a node index to its
         * immediate dominator. Returns `Some` on success, `Nothing` on OOM
         * failure.
         */
        static mozilla::Maybe<DominatedSets> Create(const JS::ubi::Vector<uint32_t>& doms) {
            auto length = doms.length();
            MOZ_ASSERT(length < UINT32_MAX);

            // Create a vector `dominated` holding a flattened set of buckets of
            // immediately dominated children nodes, with a lookup table
            // `indices` mapping from each node to the beginning of its bucket.
            //
            // This has three phases:
            //
            // 1. Iterate over the full set of nodes and count up the size of
            //    each bucket. These bucket sizes are temporarily stored in the
            //    `indices` vector.
            //
            // 2. Convert the `indices` vector to store the cumulative sum of
            //    the sizes of all buckets before each index, resulting in a
            //    mapping from node index to one past the end of that node's
            //    bucket.
            //
            // 3. Iterate over the full set of nodes again, filling in bucket
            //    entries from the end of the bucket's range to its
            //    beginning. This decrements each index as a bucket entry is
            //    filled in. After having filled in all of a bucket's entries,
            //    the index points to the start of the bucket.

            JS::ubi::Vector<uint32_t> dominated;
            JS::ubi::Vector<uint32_t> indices;
            if (!dominated.growBy(length) || !indices.growBy(length))
                return mozilla::Nothing();

            // 1
            memset(indices.begin(), 0, length * sizeof(uint32_t));
            for (uint32_t i = 0; i < length; i++)
                indices[doms[i]]++;

            // 2
            uint32_t sumOfSizes = 0;
            for (uint32_t i = 0; i < length; i++) {
                sumOfSizes += indices[i];
                MOZ_ASSERT(sumOfSizes <= length);
                indices[i] = sumOfSizes;
            }

            // 3
            for (uint32_t i = 0; i < length; i++) {
                auto idxOfDom = doms[i];
                indices[idxOfDom]--;
                dominated[indices[idxOfDom]] = i;
            }

#ifdef DEBUG
            // Assert that our buckets are non-overlapping and don't run off the
            // end of the vector.
            uint32_t lastIndex = 0;
            for (uint32_t i = 0; i < length; i++) {
                MOZ_ASSERT(indices[i] >= lastIndex);
                MOZ_ASSERT(indices[i] < length);
                lastIndex = indices[i];
            }
#endif

            return mozilla::Some(DominatedSets(mozilla::Move(dominated), mozilla::Move(indices)));
        }

        /**
         * Get the set of nodes immediately dominated by the node at
         * `postOrder[nodeIndex]`.
         */
        DominatedSetRange dominatedSet(JS::ubi::Vector<Node>& postOrder, uint32_t nodeIndex) const {
            MOZ_ASSERT(postOrder.length() == indices.length());
            MOZ_ASSERT(nodeIndex < indices.length());
            auto end = nodeIndex == indices.length() - 1
                ? dominated.end()
                : &dominated[indices[nodeIndex + 1]];
            return DominatedSetRange(postOrder, &dominated[indices[nodeIndex]], end);
        }
    };

  private:
    // Data members.
    JS::ubi::Vector<Node> postOrder;
    NodeToIndexMap nodeToPostOrderIndex;
    JS::ubi::Vector<uint32_t> doms;
    DominatedSets dominatedSets;
    mozilla::Maybe<JS::ubi::Vector<JS::ubi::Node::Size>> retainedSizes;

  private:
    // We use `UNDEFINED` as a sentinel value in the `doms` vector to signal
    // that we haven't found any dominators for the node at the corresponding
    // index in `postOrder` yet.
    static const uint32_t UNDEFINED = UINT32_MAX;

    DominatorTree(JS::ubi::Vector<Node>&& postOrder, NodeToIndexMap&& nodeToPostOrderIndex,
                  JS::ubi::Vector<uint32_t>&& doms, DominatedSets&& dominatedSets)
        : postOrder(mozilla::Move(postOrder))
        , nodeToPostOrderIndex(mozilla::Move(nodeToPostOrderIndex))
        , doms(mozilla::Move(doms))
        , dominatedSets(mozilla::Move(dominatedSets))
        , retainedSizes(mozilla::Nothing())
    { }

    static uint32_t intersect(JS::ubi::Vector<uint32_t>& doms, uint32_t finger1, uint32_t finger2) {
        while (finger1 != finger2) {
            if (finger1 < finger2)
                finger1 = doms[finger1];
            else if (finger2 < finger1)
                finger2 = doms[finger2];
        }
        return finger1;
    }

    // Do the post order traversal of the heap graph and populate our
    // predecessor sets.
    static MOZ_MUST_USE bool doTraversal(JSContext* cx, AutoCheckCannotGC& noGC, const Node& root,
                                         JS::ubi::Vector<Node>& postOrder,
                                         PredecessorSets& predecessorSets) {
        uint32_t nodeCount = 0;
        auto onNode = [&](const Node& node) {
            nodeCount++;
            if (MOZ_UNLIKELY(nodeCount == UINT32_MAX))
                return false;
            return postOrder.append(node);
        };

        auto onEdge = [&](const Node& origin, const Edge& edge) {
            auto p = predecessorSets.lookupForAdd(edge.referent);
            if (!p) {
                mozilla::UniquePtr<NodeSet, DeletePolicy<NodeSet>> set(js_new<NodeSet>());
                if (!set ||
                    !set->init() ||
                    !predecessorSets.add(p, edge.referent, mozilla::Move(set)))
                {
                    return false;
                }
            }
            MOZ_ASSERT(p && p->value());
            return p->value()->put(origin);
        };

        PostOrder traversal(cx, noGC);
        return traversal.init() &&
               traversal.addStart(root) &&
               traversal.traverse(onNode, onEdge);
    }

    // Populates the given `map` with an entry for each node to its index in
    // `postOrder`.
    static MOZ_MUST_USE bool mapNodesToTheirIndices(JS::ubi::Vector<Node>& postOrder,
                                                    NodeToIndexMap& map) {
        MOZ_ASSERT(!map.initialized());
        MOZ_ASSERT(postOrder.length() < UINT32_MAX);
        uint32_t length = postOrder.length();
        if (!map.init(length))
            return false;
        for (uint32_t i = 0; i < length; i++)
            map.putNewInfallible(postOrder[i], i);
        return true;
    }

    // Convert the Node -> NodeSet predecessorSets to a index -> Vector<index>
    // form.
    static MOZ_MUST_USE bool convertPredecessorSetsToVectors(
        const Node& root,
        JS::ubi::Vector<Node>& postOrder,
        PredecessorSets& predecessorSets,
        NodeToIndexMap& nodeToPostOrderIndex,
        JS::ubi::Vector<JS::ubi::Vector<uint32_t>>& predecessorVectors)
    {
        MOZ_ASSERT(postOrder.length() < UINT32_MAX);
        uint32_t length = postOrder.length();

        MOZ_ASSERT(predecessorVectors.length() == 0);
        if (!predecessorVectors.growBy(length))
            return false;

        for (uint32_t i = 0; i < length - 1; i++) {
            auto& node = postOrder[i];
            MOZ_ASSERT(node != root,
                       "Only the last node should be root, since this was a post order traversal.");

            auto ptr = predecessorSets.lookup(node);
            MOZ_ASSERT(ptr,
                       "Because this isn't the root, it had better have predecessors, or else how "
                       "did we even find it.");

            auto& predecessors = ptr->value();
            if (!predecessorVectors[i].reserve(predecessors->count()))
                return false;
            for (auto range = predecessors->all(); !range.empty(); range.popFront()) {
                auto ptr = nodeToPostOrderIndex.lookup(range.front());
                MOZ_ASSERT(ptr);
                predecessorVectors[i].infallibleAppend(ptr->value());
            }
        }
        predecessorSets.finish();
        return true;
    }

    // Initialize `doms` such that the immediate dominator of the `root` is the
    // `root` itself and all others are `UNDEFINED`.
    static MOZ_MUST_USE bool initializeDominators(JS::ubi::Vector<uint32_t>& doms,
                                                  uint32_t length) {
        MOZ_ASSERT(doms.length() == 0);
        if (!doms.growByUninitialized(length))
            return false;
        doms[length - 1] = length - 1;
        for (uint32_t i = 0; i < length - 1; i++)
            doms[i] = UNDEFINED;
        return true;
    }

    void assertSanity() const {
        MOZ_ASSERT(postOrder.length() == doms.length());
        MOZ_ASSERT(postOrder.length() == nodeToPostOrderIndex.count());
        MOZ_ASSERT_IF(retainedSizes.isSome(), postOrder.length() == retainedSizes->length());
    }

    MOZ_MUST_USE bool computeRetainedSizes(mozilla::MallocSizeOf mallocSizeOf) {
        MOZ_ASSERT(retainedSizes.isNothing());
        auto length = postOrder.length();

        retainedSizes.emplace();
        if (!retainedSizes->growBy(length)) {
            retainedSizes = mozilla::Nothing();
            return false;
        }

        // Iterate in forward order so that we know all of a node's children in
        // the dominator tree have already had their retained size
        // computed. Then we can simply say that the retained size of a node is
        // its shallow size (JS::ubi::Node::size) plus the retained sizes of its
        // immediate children in the tree.

        for (uint32_t i = 0; i < length; i++) {
            auto size = postOrder[i].size(mallocSizeOf);

            for (const auto& dominated : dominatedSets.dominatedSet(postOrder, i)) {
                // The root node dominates itself, but shouldn't contribute to
                // its own retained size.
                if (dominated == postOrder[length - 1]) {
                    MOZ_ASSERT(i == length - 1);
                    continue;
                }

                auto ptr = nodeToPostOrderIndex.lookup(dominated);
                MOZ_ASSERT(ptr);
                auto idxOfDominated = ptr->value();
                MOZ_ASSERT(idxOfDominated < i);
                size += retainedSizes.ref()[idxOfDominated];
            }

            retainedSizes.ref()[i] = size;
        }

        return true;
    }

  public:
    // DominatorTree is not copy-able.
    DominatorTree(const DominatorTree&) = delete;
    DominatorTree& operator=(const DominatorTree&) = delete;

    // DominatorTree is move-able.
    DominatorTree(DominatorTree&& rhs)
      : postOrder(mozilla::Move(rhs.postOrder))
      , nodeToPostOrderIndex(mozilla::Move(rhs.nodeToPostOrderIndex))
      , doms(mozilla::Move(rhs.doms))
      , dominatedSets(mozilla::Move(rhs.dominatedSets))
      , retainedSizes(mozilla::Move(rhs.retainedSizes))
    {
        MOZ_ASSERT(this != &rhs, "self-move is not allowed");
    }
    DominatorTree& operator=(DominatorTree&& rhs) {
        this->~DominatorTree();
        new (this) DominatorTree(mozilla::Move(rhs));
        return *this;
    }

    /**
     * Construct a `DominatorTree` of the heap graph visible from `root`. The
     * `root` is also used as the root of the resulting dominator tree.
     *
     * The resulting `DominatorTree` instance must not outlive the
     * `JS::ubi::Node` graph it was constructed from.
     *
     *   - For `JS::ubi::Node` graphs backed by the live heap graph, this means
     *     that the `DominatorTree`'s lifetime _must_ be contained within the
     *     scope of the provided `AutoCheckCannotGC` reference because a GC will
     *     invalidate the nodes.
     *
     *   - For `JS::ubi::Node` graphs backed by some other offline structure
     *     provided by the embedder, the resulting `DominatorTree`'s lifetime is
     *     bounded by that offline structure's lifetime.
     *
     * In practice, this means that within SpiderMonkey we must treat
     * `DominatorTree` as if it were backed by the live heap graph and trust
     * that embedders with knowledge of the graph's implementation will do the
     * Right Thing.
     *
     * Returns `mozilla::Nothing()` on OOM failure. It is the caller's
     * responsibility to handle and report the OOM.
     */
    static mozilla::Maybe<DominatorTree>
    Create(JSContext* cx, AutoCheckCannotGC& noGC, const Node& root) {
        JS::ubi::Vector<Node> postOrder;
        PredecessorSets predecessorSets;
        if (!predecessorSets.init() || !doTraversal(cx, noGC, root, postOrder, predecessorSets))
            return mozilla::Nothing();

        MOZ_ASSERT(postOrder.length() < UINT32_MAX);
        uint32_t length = postOrder.length();
        MOZ_ASSERT(postOrder[length - 1] == root);

        // From here on out we wish to avoid hash table lookups, and we use
        // indices into `postOrder` instead of actual nodes wherever
        // possible. This greatly improves the performance of this
        // implementation, but we have to pay a little bit of upfront cost to
        // convert our data structures to play along first.

        NodeToIndexMap nodeToPostOrderIndex;
        if (!mapNodesToTheirIndices(postOrder, nodeToPostOrderIndex))
            return mozilla::Nothing();

        JS::ubi::Vector<JS::ubi::Vector<uint32_t>> predecessorVectors;
        if (!convertPredecessorSetsToVectors(root, postOrder, predecessorSets, nodeToPostOrderIndex,
                                             predecessorVectors))
            return mozilla::Nothing();

        JS::ubi::Vector<uint32_t> doms;
        if (!initializeDominators(doms, length))
            return mozilla::Nothing();

        bool changed = true;
        while (changed) {
            changed = false;

            // Iterate over the non-root nodes in reverse post order.
            for (uint32_t indexPlusOne = length - 1; indexPlusOne > 0; indexPlusOne--) {
                MOZ_ASSERT(postOrder[indexPlusOne - 1] != root);

                // Take the intersection of every predecessor's dominator set;
                // that is the current best guess at the immediate dominator for
                // this node.

                uint32_t newIDomIdx = UNDEFINED;

                auto& predecessors = predecessorVectors[indexPlusOne - 1];
                auto range = predecessors.all();
                for ( ; !range.empty(); range.popFront()) {
                    auto idx = range.front();
                    if (doms[idx] != UNDEFINED) {
                        newIDomIdx = idx;
                        break;
                    }
                }

                MOZ_ASSERT(newIDomIdx != UNDEFINED,
                           "Because the root is initialized to dominate itself and is the first "
                           "node in every path, there must exist a predecessor to this node that "
                           "also has a dominator.");

                for ( ; !range.empty(); range.popFront()) {
                    auto idx = range.front();
                    if (doms[idx] != UNDEFINED)
                        newIDomIdx = intersect(doms, newIDomIdx, idx);
                }

                // If the immediate dominator changed, we will have to do
                // another pass of the outer while loop to continue the forward
                // dataflow.
                if (newIDomIdx != doms[indexPlusOne - 1]) {
                    doms[indexPlusOne - 1] = newIDomIdx;
                    changed = true;
                }
            }
        }

        auto maybeDominatedSets = DominatedSets::Create(doms);
        if (maybeDominatedSets.isNothing())
            return mozilla::Nothing();

        return mozilla::Some(DominatorTree(mozilla::Move(postOrder),
                                           mozilla::Move(nodeToPostOrderIndex),
                                           mozilla::Move(doms),
                                           mozilla::Move(*maybeDominatedSets)));
    }

    /**
     * Get the root node for this dominator tree.
     */
    const Node& root() const {
        return postOrder[postOrder.length() - 1];
    }

    /**
     * Return the immediate dominator of the given `node`. If `node` was not
     * reachable from the `root` that this dominator tree was constructed from,
     * then return the null `JS::ubi::Node`.
     */
    Node getImmediateDominator(const Node& node) const {
        assertSanity();
        auto ptr = nodeToPostOrderIndex.lookup(node);
        if (!ptr)
            return Node();

        auto idx = ptr->value();
        MOZ_ASSERT(idx < postOrder.length());
        return postOrder[doms[idx]];
    }

    /**
     * Get the set of nodes immediately dominated by the given `node`. If `node`
     * is not a member of this dominator tree, return `Nothing`.
     *
     * Example usage:
     *
     *     mozilla::Maybe<DominatedSetRange> range = myDominatorTree.getDominatedSet(myNode);
     *     if (range.isNothing()) {
     *         // Handle unknown node however you see fit...
     *         return false;
     *     }
     *
     *     for (const JS::ubi::Node& dominatedNode : *range) {
     *         // Do something with each immediately dominated node...
     *     }
     */
    mozilla::Maybe<DominatedSetRange> getDominatedSet(const Node& node) {
        assertSanity();
        auto ptr = nodeToPostOrderIndex.lookup(node);
        if (!ptr)
            return mozilla::Nothing();

        auto idx = ptr->value();
        MOZ_ASSERT(idx < postOrder.length());
        return mozilla::Some(dominatedSets.dominatedSet(postOrder, idx));
    }

    /**
     * Get the retained size of the given `node`. The size is placed in
     * `outSize`, or 0 if `node` is not a member of the dominator tree. Returns
     * false on OOM failure, leaving `outSize` unchanged.
     */
    MOZ_MUST_USE bool getRetainedSize(const Node& node, mozilla::MallocSizeOf mallocSizeOf,
                                      Node::Size& outSize) {
        assertSanity();
        auto ptr = nodeToPostOrderIndex.lookup(node);
        if (!ptr) {
            outSize = 0;
            return true;
        }

        if (retainedSizes.isNothing() && !computeRetainedSizes(mallocSizeOf))
            return false;

        auto idx = ptr->value();
        MOZ_ASSERT(idx < postOrder.length());
        outSize = retainedSizes.ref()[idx];
        return true;
    }
};

} // namespace ubi
} // namespace JS

#endif // js_UbiNodeDominatorTree_h
