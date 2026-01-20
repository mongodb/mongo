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

#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"

namespace mongo::join_ordering {
PathResolver::PathResolver(NodeId baseNode, std::vector<ResolvedPath>& resolvedPaths)
    : _resolvedPaths(resolvedPaths) {
    _scopes.emplace_back(baseNode);
}

void PathResolver::addNode(NodeId nodeId, const FieldPath& embedPath) {
    dassert(!getScopeForNode(nodeId).has_value(), "This node has been already added");
    _scopes.emplace_back(nodeId, embedPath);
}

PathId PathResolver::addPath(NodeId nodeId, FieldPath fieldPath) {
    auto scope = getScopeForNode(nodeId);
    tassert(11721300, "Unknown node id", scope.has_value());
    auto [it, inserted] =
        scope->paths.emplace(std::move(fieldPath), static_cast<PathId>(_resolvedPaths.size()));
    if (inserted) {
        _resolvedPaths.emplace_back(nodeId, it->first);
    }
    return it->second;
}

PathId PathResolver::resolve(const FieldPath& path) {
    auto [nodeId, pathWithoutEmbedPath] = resolveNodeByEmbedPath(path);
    return addPath(nodeId, std::move(pathWithoutEmbedPath));
}

std::pair<NodeId, FieldPath> PathResolver::resolveNodeByEmbedPath(
    const FieldPath& fieldPath) const {
    for (auto scopePos = _scopes.rbegin(); scopePos != _scopes.rend(); ++scopePos) {
        if (!scopePos->embedPath.has_value()) {
            // Base node case: no prefix substraction is required.
            return {scopePos->nodeId, fieldPath};
        } else if (scopePos->embedPath->isPrefixOf(fieldPath)) {
            if (scopePos->embedPath->getPathLength() == fieldPath.getPathLength()) {
                uasserted(10985001,
                          str::stream() << "The path '" << fieldPath.fullPath()
                                        << "' cannot be resolved because it coflicts with "
                                           "a previously specified document prefix.");
            }

            return {scopePos->nodeId,
                    fieldPath.subtractPrefix(scopePos->embedPath->getPathLength())};
        }
    }

    // Base node with empty embedPath is a prefix for every path.
    MONGO_UNREACHABLE_TASSERT(11721301);
}

boost::optional<PathResolver::Scope&> PathResolver::getScopeForNode(NodeId node) {
    for (auto& scope : _scopes) {
        if (scope.nodeId == node) {
            return scope;
        }
    }
    return boost::none;
}
}  // namespace mongo::join_ordering
