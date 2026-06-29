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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/util/modules.h"

#include <vector>

#include <absl/container/flat_hash_map.h>

namespace mongo::join_ordering {
/**
 * This class is a wrapper on dependency analysis that allows join optimization to additionally
 * track which $lookup a particular join predicate field originated from. Its primary function is to
 * "resolve paths" so that we can determine which join node a path in a join predicate actually
 * originated from (and if indeed it came directly from one of the collections we're joining on).
 *
 * For example, the following pipeline on collection "a" would map "a" to node 0, "b" to 1, and "c"
 * to 2:
 * [
 *   {$lookup: {from: "b", localField: "x", foreignField: "x", as: "e"}},
 *   {$unwind: "$e"},
 *   {$lookup: {from: "c", localField: "b.y.z", foreignField: "y.m", as: "f"}},
 *   {$unwind: "$f"},
 * ]
 * and produces the following resolved paths: [{"x", 0}, {"x", 1}, {"y.z", 1}, {"y.m", 2}]
 */
class PathResolver {
public:
    PathResolver(NodeId baseNode, const pipeline::dependency_graph::DependencyGraph& graph);

    PathResolver(PathResolver&&) = delete;
    PathResolver(const PathResolver&) = delete;
    PathResolver& operator=(const PathResolver& other) = delete;
    PathResolver& operator=(PathResolver&& other) = delete;

    /**
     * This function looks at the provided 'fieldPath' (a potential join predicate field) extracted
     * from DocumentSource 'at', and attempts to either find an existing resolved path or register a
     * new one, returning the corresponding PathId (index into 'resolvedPaths'). The provided
     * 'nodeId' is used to specify the scope in which node this path originated from so that the
     * correct dependency graph can be identified (e.g. is this in a sub-pipeline?)- if not
     * provided, this function assumes the path exists on the main pipeline and resolves the path
     * accordingly. Note that if 'at' is set to nullptr, then this refers to the end of the
     * sub-pipeline.
     *
     * It returns boost::none if a path cannot be resolved- for example, if the path was introduced
     * by a stage other than one of our $lookups (e.g. a computed field in a $project). We only
     * support fields that can be directly traced back to their base collection.
     */
    boost::optional<PathId> resolve(const FieldPath& fieldPath,
                                    const DocumentSource* at,
                                    boost::optional<NodeId> nodeId = boost::none);

    /**
     * This function is allows us to know that future paths passed into resolve() may reference the
     * "as" field of this $lookup, and that this $lookup corresponds to the given nodeId (which is
     * used to create new paths resolving to this node).
     *
     * It returns a success boolean. It may fail if the "as" field of a $lookup shadows a previously
     * resolved join predicate path, or if we're trying to re-register the same $lookup. It will
     * also fail if trying to track an already-known $lookup or a known 'nodeId'.
     */
    bool trackEmbedPath(const DocumentSourceLookUp& lookup, NodeId nodeId);

    const std::vector<ResolvedPath>& resolvedPaths() const {
        return _resolvedPaths;
    }

    std::vector<ResolvedPath> releaseResolvedPaths() {
        return std::move(_resolvedPaths);
    }

    const ResolvedPath& operator[](PathId pathId) const {
        return _resolvedPaths.at(pathId);
    }

private:
    // Helpers to update or search _resolvedPaths.
    PathId addPath(ResolvedPath path);
    boost::optional<PathId> addPathOrGetExisting(ResolvedPath path);

    const pipeline::dependency_graph::DependencyGraph& _graph;
    NodeId _baseNodeId;

    /**
     * Helper struct to allow checking for already-resolved fields per node quickly.
     */
    struct NodeInfo {
        // $lookup from which this join-node was sourced.
        const DocumentSourceLookUp* sourceLookup;
        // Paths resolved to this particular join-node.
        absl::flat_hash_map<FieldPath, PathId> resolvedPaths;
    };

    // Poor man's bimap- we need to the right $lookup for a given 'nodeId', and vice-versa.
    absl::flat_hash_map<const DocumentSourceLookUp*, NodeId> _lookupNodes;
    absl::flat_hash_map<NodeId, NodeInfo> _nodeLookups;
    std::vector<ResolvedPath> _resolvedPaths;
};

}  // namespace mongo::join_ordering
