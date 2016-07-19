/**
*    Copyright (C) 2016 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/
#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/string_map.h"

namespace mongo {
class ViewDefinition;

/**
 * Validates that the graph of view dependencies is acyclic and within the allowed depth.
 * Each node is represented by an integer id, and stores integer ids for its parents and children
 * in the graph.
 *
 * This is owned and managed by the ViewCatalog.
 */
class ViewGraph {
    MONGO_DISALLOW_COPYING(ViewGraph);

public:
    static const int kMaxViewDepth;

    ViewGraph() = default;

    /**
     * Called when a view is added to the catalog. 'refs' are a list of namespaces that the view
     * represented by 'viewNss' references in its viewOn or pipeline. Checks if this view introduces
     * a cycle or max diameter. If an error is detected, it will not insert.
     */
    Status insertAndValidate(const NamespaceString& viewNss,
                             const std::vector<NamespaceString>& refs);

    /**
     * Called when view definitions are being reloaded from the catalog (e.g. on restart of mongod).
     * Does the same as insertAndValidate except does not check for cycles or max diameter.
     */
    void insertWithoutValidating(const NamespaceString& viewNss,
                                 const std::vector<NamespaceString>& refs);

    /**
     * Called when a view is removed from the catalog. If the view does not exist in the graph it is
     * a no-op. The view may also have a cycle or max diameter through it.
     */
    void remove(const NamespaceString& viewNss);

    /**
     * Deletes the view graph and all references to namespaces.
     */
    void clear();

private:
    // A graph node represents a namespace. The parent-child relation is defined as a parent
    // references the child either through viewOn or in $lookup/$graphLookup/$facet in its pipeline.
    // E.g. the command {create: "a", viewOn: "b", pipeline: [{$lookup: {from: "c"}}]}
    // means the node for "a" is a parent of nodes for "b" and "c" since it references them.
    struct Node {
        // Note, a view may refer to the same child more than once, but we only need to know the
        // set of children and parents, since we do not need to traverse duplicates.
        std::unordered_set<uint64_t> parents;
        std::unordered_set<uint64_t> children;
        std::string ns;
    };

    // Bookkeeping for graph traversals.
    struct NodeHeight {
        bool checked = false;
        int height = 0;
    };

    using HeightMap = std::unordered_map<uint64_t, NodeHeight>;

    /**
     * Recursively traverses parents of this node and computes their heights. Returns an error
     * if the maximum depth is exceeded.
     */
    ErrorCodes::Error _getParentsHeight(uint64_t currentId, int currentDepth, HeightMap* heightMap);

    /**
     * Recursively traverses children of the starting node and computes their heights. Returns an
     * error if the maximum depth is exceeded or a cycle is detected through the starting node.
     */
    ErrorCodes::Error _getChildrenHeightAndCheckCycle(uint64_t startingId,
                                                      uint64_t currentId,
                                                      int currentDepth,
                                                      HeightMap* heightMap,
                                                      std::vector<uint64_t>* cycleIds);

    /**
     * Gets the id for this namespace, and creates an id if it doesn't exist.
     */
    uint64_t _getNodeId(const NamespaceString& ns);

    // Maps namespaces to an internal node id. A mapping exists for every namespace referenced,
    // i.e. existing views, collections, and non-existing namespaces.
    StringMap<uint64_t> _namespaceIds;

    // Maps node ids to nodes. There is a 1-1 correspondance with _namespaceIds, hence the lifetime
    // of a node is the same as the lifetime as its corresponding node id.
    std::unordered_map<uint64_t, Node> _graph;
    static uint64_t _idCounter;
};
}  // namespace mongo
