/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {
/** Resolves field paths that are part of the Join Graph. The main function of the class is to
 * ensure that all field paths are correctly identified and processed for join optimization. By
 * default, all symbols resolve to a baseNode, specified in the constructor. If a new node with an
 * embedPath is added, paths matching that prefix resolve to the new node.
 */
class PathResolver {
public:
    PathResolver(NodeId baseNode, std::vector<ResolvedPath>& resolvedPaths)
        : _baseNode(baseNode), _resolvedPaths(resolvedPaths), _nodeMaps(_baseNode + 1) {}

    /**
     * Introduces a new prefix 'embedPath' in the resolver that is associated with the node
     * 'nodeId'.
     */
    void addNode(NodeId nodeId, const FieldPath& embedPath);

    /**
     * Add a new path to the given node. The known node prefixes are not consulted.
     * This function should be used when it is know for sure that this particular fieldPath comes
     * from the node 'nodeId'.
     * Unlike the 'resolve' function, this function never removes prefixes of the matched node from
     * the 'fieldPath'. It means, that even after call 'addNode(1, "a")', 'addNode(1, "a.b.c")'
     * resolves to the field "a.b.c" of node 1. However, 'resolve("a.b.c")' resolves to field "b.c"
     * of node 1 and 'resolve("a.a.b.c")' resolved to the field "a.b.c" of node 1.
     */
    PathId addPath(NodeId nodeId, FieldPath fieldPath);

    /**
     * The function looks for known prefixes, the longest match prefix defines a collection to which
     * this field path is resolved.
     * It is important to understand that 'addNode' calls changes how a particular paths is
     * resolved. Foe example:
     * 1. Call for "a.b.c", no prefixes matched, the field is resolved to "a.b.c" of node
     * '_baseNode'.
     * 2. After 'addNode(1, "a")' is called, the field "a.b.c" is resolved to "b.c" of node 1.
     * 3. After 'addNode(2, "a.b")' is called, the field "a.b.c" is resolved to "c" of node 2.
     * Because prefix "a.b" of node 2 is longer than prefix "a" of node 1.
     */
    PathId resolve(const FieldPath& path);

    const ResolvedPath& operator[](PathId PathId) const {
        return _resolvedPaths.at(PathId);
    }

private:
    /** Resolves the path by its longest prefix with the known node embedPaths and returns the
     * resolved node and the resolved field path without the embedPath of the node.
     */
    std::pair<NodeId, FieldPath> resolveNodeByEmbedPath(const FieldPath& fieldPath) const;

    NodeId _baseNode;
    absl::flat_hash_map<FieldPath, NodeId> _embedPathToNodeId;
    std::vector<ResolvedPath>& _resolvedPaths;
    std::vector<absl::flat_hash_map<FieldPath, PathId>> _nodeMaps;
};
}  // namespace mongo::join_ordering
