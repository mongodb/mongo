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

#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"

#include <boost/optional/optional.hpp>

namespace mongo::join_ordering {
namespace {
/**
 * Ensures path can't include a numeric component.
 */
bool hasNumericPathComponent(const FieldPath& fp) {
    for (size_t i = 0; i < fp.getPathLength(); ++i) {
        if (FieldRef::isNumericPathComponentLenient(fp.getFieldName(i))) {
            return true;
        }
    }
    return false;
}

/**
 * Ensures join predicate paths can't include arrays and doesn't have a numeric component.
 */
bool isPathValid(const pipeline::dependency_graph::DependencyGraph& graph,
                 const DocumentSource* at,
                 const FieldPath& path) {
    return !hasNumericPathComponent(path) && !graph.canPathBeArray(at, path.fullPath());
}
}  // namespace

PathResolver::PathResolver(NodeId baseNode,
                           const pipeline::dependency_graph::DependencyGraph& graph)
    : _graph(graph), _baseNodeId(baseNode) {
    _nodeLookups.emplace(baseNode, nullptr);
}

boost::optional<PathId> PathResolver::addPathOrGetExisting(ResolvedPath path) {
    auto it = _nodeLookups.find(path.nodeId);
    if (it == _nodeLookups.end()) {
        // We should know about this node already, but if we don't, just bail.
        return boost::none;
    }

    auto& resolvedPaths = it->second.resolvedPaths;
    if (auto pathIt = resolvedPaths.find(path.fieldName); pathIt != resolvedPaths.end()) {
        // We already know this path!
        return pathIt->second;
    }
    return addPath(std::move(path));
}

PathId PathResolver::addPath(ResolvedPath path) {
    PathId id = _resolvedPaths.size();
    auto it = _nodeLookups.find(path.nodeId);
    tassert(12835901, "Expected to know of this node already", it != _nodeLookups.end());
    it->second.resolvedPaths.try_emplace(path.fieldName, id);
    _resolvedPaths.push_back(std::move(path));
    return id;
}

bool PathResolver::trackEmbedPath(const DocumentSourceLookUp& lookup, NodeId nodeId) {
    const auto& embedPath = lookup.getAsField();

    if (_lookupNodes.contains(&lookup) || _nodeLookups.contains(nodeId)) {
        // Bail- its illegal to track the same node/$lookup multiple times.
        return false;
    }

    // First check we're not shadowing any local field paths. We only need to check the base
    // collection paths here.
    for (auto&& [fp, _] : _nodeLookups[_baseNodeId].resolvedPaths) {
        if (embedPath.isPrefixOf(fp) || fp.isPrefixOf(embedPath)) {
            return false;
        }
    }

    auto src = _graph.getPrevModifyingStage(&lookup, embedPath.fullPath());
    if (auto lookup = dynamic_cast<const DocumentSourceLookUp*>(src.get()); lookup) {
        // This embed path came from a prior $lookup! This means it collides with a previous "as"
        // field, and so it is not safe to reorder in the general case. Bail.
        tassert(12835905,
                "Expected to know about this lookup",
                _lookupNodes.find(lookup) != _lookupNodes.end());
        return false;
    }

    // This may have come from the base collection, OR from a field
    // computation/rename/modification/etc. However, we don't care! The "as" field completely
    // replaces (no traversals) whatever field it references. This means it effectively supercedes
    // any possible prior modification of this path on the main pipeline (which is fine, because we
    // apply embeddings to the main collection document after we project any fields in the single
    // table access plan).
    _lookupNodes.emplace(&lookup, nodeId);
    _nodeLookups.emplace(nodeId, &lookup);
    return true;
}

boost::optional<PathId> PathResolver::resolve(const FieldPath& fieldPath,
                                              const DocumentSource* at,
                                              boost::optional<NodeId> nodeId) {
    const pipeline::dependency_graph::DependencyGraph* graph = nullptr;
    if (nodeId && *nodeId != _baseNodeId) {
        if (auto it = _nodeLookups.find(*nodeId); it != _nodeLookups.end()) {
            graph = _graph.getSubpipelineGraph(it->second.sourceLookup);
        }
    } else {
        graph = &_graph;
    }
    tassert(12835902, "Expected to find a dependency graph", graph);

    // Make sure this path is actually valid before proceeding. We validate the path where it is
    // referenced for arrayness.
    if (!isPathValid(*graph, at, fieldPath)) {
        return boost::none;
    }

    boost::optional<ResolvedPath> resolved;
    auto src = graph->getPrevModifyingStage(at, fieldPath.fullPath());
    if (src == nullptr) {
        // Path originates at base of pipeline for this graph.
        resolved = ResolvedPath{nodeId.get_value_or(_baseNodeId), fieldPath};

    } else if (auto lookup = dynamic_cast<const DocumentSourceLookUp*>(src.get()); lookup) {
        // Path comes from a $lookup!
        tassert(
            12835903, "Unexpected prior $lookup", _lookupNodes.find(lookup) != _lookupNodes.end());

        // This comes from a previous $lookup! Strip the embedding field.
        auto asEmbedding = lookup->getAsField();
        // Ensure this in fact is prefixed by the "as" field.
        if (!asEmbedding.isPrefixOf(fieldPath)) {
            // Can happen if our path is actually an ancestor of a previous $lookup embedding field.
            // This is a conflict.
            return boost::none;
        } else if (asEmbedding.getPathLength() == fieldPath.getPathLength()) {
            // Path matches embedding path. Bail out of join opt.
            return boost::none;
        }

        // One last check: did the $lookup sub-pipeline modify this field? If so, bail- only support
        // fields coming directly from a collection.
        auto strippedPath = fieldPath.subtractPrefix(asEmbedding.getPathLength());
        auto subGraph = _graph.getSubpipelineGraph(src.get());
        tassert(12835904, "Expected to find a dependency graph for lookup", subGraph);
        // Note: nullptr here indicates "end of subpipeline".
        if (subGraph->getPrevModifyingStage(nullptr, strippedPath.fullPath())) {
            // This must have been modified in the $lookup subpipeline! Bail.
            return boost::none;
        }

        // Validate the original path where it is referenced for arrayness.
        if (!isPathValid(*graph, at, fieldPath)) {
            return boost::none;
        }

        resolved = ResolvedPath{_lookupNodes[lookup], strippedPath};
    }

    // This comes from a rename/ field computation! Bail- we don't support this.
    if (!resolved) {
        return boost::none;
    }

    // One last check! If this is not originating from the base node, ensure that it is not in
    // fact modified in any way by the sub-pipeline of the node that produced it. This is
    // because we rely on the CQ we generate for this node not modifying this field. We don't do
    // this for the base node, because we allow a trailing suffix after our join-opt-eligible prefix
    // to modify any path.
    // TODO SERVER-128365: Support renames.
    if (resolved->nodeId != _baseNodeId) {
        graph = _graph.getSubpipelineGraph(_nodeLookups[resolved->nodeId].sourceLookup);
        tassert(12836400, "Expected to find a dependency graph for lookup", graph);

        if (graph->getPrevModifyingStage(nullptr, resolved->fieldName.fullPath())) {
            // This path was modified at some point by our sub-pipeline! Bail.
            return boost::none;
        }
    }

    return addPathOrGetExisting(*resolved);
}

std::vector<ResolvedPath> PathResolver::releaseResolvedPaths(size_t maxNodeIdExclusive) {
    // Erase all paths belonging to a reserved node that hasn't actually made it into the graph.
    auto it = _resolvedPaths.begin();
    while (it != _resolvedPaths.end() && it->nodeId < maxNodeIdExclusive) {
        it++;
    }
    _resolvedPaths.erase(it, _resolvedPaths.end());
    return std::move(_resolvedPaths);
}

}  // namespace mongo::join_ordering
