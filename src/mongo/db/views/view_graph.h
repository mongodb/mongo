/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/string_map.h"

namespace mongo {
class ViewDefinition;

/**
 * Represents the dependencies of views on other namespaces and validates that this graph is acyclic
 * and smaller than the maximum diameter. It also checks that the views in each connected component
 * have compatible collations, and that all possible view pipelines stay within the maximum size in
 * bytes.
 *
 * This is owned and managed by the ViewCatalog.
 */
class ViewGraph {
public:
    static const int kMaxViewDepth;
    static const int kMaxViewPipelineSizeBytes;

    ViewGraph() = default;

    /**
     * Inserts a new node for 'view' in the graph, which must not already be present. 'refs'
     * contains the list of namespaces referred to by the view in its "viewOn" or "pipeline".
     * 'pipelineSize' is the size of the view's pipeline, in bytes. If inserting the view would
     * violate one of the graph's validity properties, the insertion is reverted and a non-OK status
     * is returned.
     *
     * This method is intended for validating a view that is created or modified by a user
     * operation.
     */
    Status insertAndValidate(const ViewDefinition& view,
                             const std::vector<NamespaceString>& refs,
                             int pipelineSize);

    /**
     * Like insertAndValidate(), inserts a new node for 'view' in the graph, which must not already
     * be present. However, no validation is performed. The insertion is not rolled back even if it
     * puts the graph into an invalid state.
     *
     * This method is intended for quickly repopulating the graph with view definitions that are
     * assumed to be already valid.
     */
    void insertWithoutValidating(const ViewDefinition& view,
                                 const std::vector<NamespaceString>& refs,
                                 int pipelineSize);

    /**
     * Called when a view is removed from the catalog. If the view does not exist in the graph it is
     * a no-op. The view may also have a cycle or max diameter through it.
     */
    void remove(const NamespaceString& viewNss);

    /**
     * Deletes the view graph and all references to namespaces.
     */
    void clear();

    /**
     * Returns the number of namespaces tracked by the view graph. Only exposed for testing.
     */
    size_t size() const;

private:
    // A graph node represents a namespace. We say that a node A is a parent of B if A is a view and
    // it references B via its "viewOn" or $lookup/$graphLookup/$facet in its "pipeline".
    //
    // This node represents a view namespace if and only if 'children' is nonempty and 'collator' is
    // set.
    struct Node {
        Node() = default;
        Node(const Node& other)
            : nss(other.nss), children(other.children), parents(other.parents), size(other.size) {
            if (other.collator) {
                collator = CollatorInterface::cloneCollator(other.collator.get());
            }
        }

        /**
         * Returns true if this node represents a view.
         */
        bool isView() const {
            return !children.empty();
        }

        // The fully-qualified namespace that this node represents.
        NamespaceString nss;

        // Represents the namespaces depended on by this view. A view may refer to the same child
        // more than once, but we store the children as a set because each namespace need not be
        // traversed more than once.
        stdx::unordered_set<uint64_t> children;

        // Represents the views that depend on this namespace.
        stdx::unordered_set<uint64_t> parents;

        // When set to nullptr, the view either has the binary collation or this namespace is not a
        // view and we don't care about its collator. Verify if view with isView. ViewGraph owns the
        // collator in order to keep pointer alive after insertion.
        std::unique_ptr<const CollatorInterface> collator;

        // The size of this view's "pipeline", in bytes.
        int size = 0;
    };

    // Bookkeeping for graph traversals.
    struct NodeStats {
        bool checked = false;
        int height = 0;
        int cumulativeSize = 0;
    };

    using StatsMap = stdx::unordered_map<uint64_t, NodeStats>;

    /**
     * Recursively validates the parents of 'currentId', filling out statistics about the node
     * represented by that id in 'statsMap'. The recursion level is tracked in 'currentDepth' to
     * limit recursive calls.
     *
     * A non-OK status is returned if 'currentId' or its parents violate any of the graph's
     * validity properties.
     */
    Status _validateParents(uint64_t currentId, int currentDepth, StatsMap* statsMap);

    /**
     * Recursively validates the children of 'currentId', filling out statistics about the node
     * represented by that id in 'statsMap'. The recursion level is tracked in 'currentDepth' to
     * limit recursive calls. Both 'startingId' and 'traversalIds' are used to detect cycles.
     *
     * A non-OK status is returned if 'currentId' or its children violate any of the graph's
     * validity properties.
     */
    Status _validateChildren(uint64_t startingId,
                             uint64_t currentId,
                             int currentDepth,
                             StatsMap* statsMap,
                             std::vector<uint64_t>* traversalIds);

    /**
     * Gets the id for this namespace, and creates an id if it doesn't exist.
     */
    uint64_t _getNodeId(const NamespaceString& ns);

    // Maps namespaces to an internal node id. A mapping exists for every namespace referenced,
    // i.e. existing views, collections, and non-existing namespaces.
    stdx::unordered_map<NamespaceString, uint64_t> _namespaceIds;

    // Maps node ids to nodes. There is a 1-1 correspondence with _namespaceIds, hence the lifetime
    // of a node is the same as the lifetime as its corresponding node id.
    stdx::unordered_map<uint64_t, Node> _graph;
    uint64_t _idCounter = 0;
};
}  // namespace mongo
