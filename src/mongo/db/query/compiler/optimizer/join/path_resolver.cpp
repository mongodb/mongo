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

#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"

#include <boost/optional/optional.hpp>

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

boost::optional<PathId> PathResolver::resolve(const FieldPath& path) {
    if (auto resolved = resolveNodeByEmbedPath(path); resolved) {
        auto [nodeId, pathWithoutEmbedPath] = *resolved;
        return boost::make_optional(addPath(nodeId, std::move(pathWithoutEmbedPath)));
    } else {
        return boost::none;
    }
}

bool PathResolver::pathResolvesToJoinNode(const FieldPath& asField, NodeId baseNodeId) {
    if (auto resolved = resolveNodeByEmbedPath(asField); resolved) {
        auto [nodeId, pathWithoutEmbedPath] = *resolved;
        if (nodeId == baseNodeId) {
            // Signal that $lookup "as" field shadows an earlier localField.
            if (shadowsResolvedPath(nodeId, pathWithoutEmbedPath)) {
                return true;
            }
            // Hooray, this path is not owned by any join nodes in the graph so it's good to go!
            return false;
        }
        // This represents the case where the path in the asField is already owned by/attributed
        // to a previous join node in the graph. Take for example this pipeline:
        // {$lookup: {as: x, ...}}
        // {$lookup: {as: x.y, ...}}
        // By the time we resolve 'x.y' we see that the prefix 'x' is already owned by the
        // join node created from the first $lookup stage.
        return true;
    }
    // This is the case where the second lookup "as" field is a prefix of the first's.
    // See code comment in resolveNodeByEmbedPath() for more details.
    return true;
}

boost::optional<std::pair<NodeId, FieldPath>> PathResolver::resolveNodeByEmbedPath(
    const FieldPath& fieldPath) const {

    for (auto scopePos = _scopes.rbegin(); scopePos != _scopes.rend(); ++scopePos) {
        if (!scopePos->embedPath.has_value()) {
            // Base node case: no prefix substraction is required.
            return boost::make_optional(std::make_pair(scopePos->nodeId, fieldPath));
        } else if (scopePos->embedPath->isPrefixOf(fieldPath)) {
            if (scopePos->embedPath->getPathLength() == fieldPath.getPathLength()) {
                // The field cannot be resolved because it is the same as a previously
                // specified/attributed prefix eg
                return boost::none;
            }
            return boost::make_optional(std::make_pair(
                scopePos->nodeId, fieldPath.subtractPrefix(scopePos->embedPath->getPathLength())));
        } else if (fieldPath.isPrefixOf(*scopePos->embedPath)) {
            // This field cannot be resolved, likely because it is attributable to multiple nodes in
            // the graph. eg Considering collection A:
            // [{a: 1, b: 2, x: { c: 2}}]
            // and the following query:
            // {$lookup: {as: "x.y", from: B, ...}},
            // {$unwind: "$x.y"},
            // {$lookup: {as: "x", from: C, ...}},
            // {$unwind: "$x"},
            //
            // The first lookup produces this intermediary pipeline result for each doc in coll A:
            //
            //  { ...base fields from coll A..., x: { c: ..., y: [documents on B that match the
            //  first lookup]] } }
            //
            // In this case, $x contains data from node A and node B so we cannot attribute to a
            // single node when we attempt to resolve $x during the second lookup. More importantly,
            // the second join will overwrite the x field entirely,
            //  which we want to prevent. So in this case, we return boost::none and fallback.
            return boost::none;
        }
    }

    // Base node with empty embedPath is a prefix for every path.
    MONGO_UNREACHABLE_TASSERT(11721301);
}

bool PathResolver::shadowsResolvedPath(NodeId nodeId, const FieldPath& path) const {
    for (const auto& resolvedPath : _resolvedPaths) {
        if (resolvedPath.nodeId != nodeId) {
            continue;
        }
        // isPrefixOf() also returns true for equal paths, so this catches exact overwrites as well
        // as overwrites of an ancestor or descendant of an already-resolved path.
        if (path.isPrefixOf(resolvedPath.fieldName) || resolvedPath.fieldName.isPrefixOf(path)) {
            return true;
        }
    }
    return false;
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
