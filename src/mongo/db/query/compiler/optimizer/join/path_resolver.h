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

    void addNode(NodeId nodeId, const FieldPath& embedPath);

    PathId addPath(NodeId nodeId, FieldPath fieldPath);
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
