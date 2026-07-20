// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/util/string_map.h"

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
    if (auto pathIt = resolvedPaths.find(path.underlyingFieldPath); pathIt != resolvedPaths.end()) {
        // We already know this path!
        return pathIt->second;
    }
    return addPath(std::move(path));
}

PathId PathResolver::addPath(ResolvedPath path) {
    PathId id = _resolvedPaths.size();
    auto it = _nodeLookups.find(path.nodeId);
    tassert(12835901, "Expected to know of this node already", it != _nodeLookups.end());
    it->second.resolvedPaths.try_emplace(path.underlyingFieldPath, id);
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

boost::optional<ResolvedPath> PathResolver::_resolve(
    const pipeline::dependency_graph::DependencyGraph& graph,
    const FieldPath& fieldPath,
    const DocumentSource* at,
    NodeId nodeId) {
    auto origin = graph.resolveFieldOrigin(at, fieldPath.fullPath());
    switch (origin.kind) {
        case pipeline::dependency_graph::FieldOriginKind::kBaseDocument: {
            // Path originates at base of pipeline for this graph.
            return ResolvedPath{nodeId, fieldPath};
        }
        case pipeline::dependency_graph::FieldOriginKind::kAlias: {
            // This is a rename! Get the base field name.
            tassert(12836501, "Expected an input field", origin.inputField);
            auto resolved =
                _resolve(graph, *origin.inputField, origin.modifyingStage.get(), nodeId);
            if (resolved) {
                resolved->fieldPathAfterRenames = fieldPath;
            }
            return resolved;
        }
        case pipeline::dependency_graph::FieldOriginKind::kSubpipeline: {
            // We don't support nested $lookups.
            tassert(12836500, "Don't support nested $lookups", nodeId == _baseNodeId);

            auto lookup = dynamic_cast<const DocumentSourceLookUp*>(origin.modifyingStage.get());
            if (!lookup) {
                // We only support $lookup sub-pipelines.
                return boost::none;
            }

            auto nodeIt = _lookupNodes.find(lookup);
            tassert(12835903, "Unexpected prior $lookup", nodeIt != _lookupNodes.end());

            // This comes from a previous $lookup! 'origin.inputField' already strips the embed
            // path for us.
            auto subGraph = _graph.getSubpipelineGraph(lookup);
            tassert(12836502, "Expected a dependency graph to be available", subGraph);
            return _resolve(*subGraph, *origin.inputField, nullptr, nodeIt->second);
        }

        case pipeline::dependency_graph::FieldOriginKind::kOther: {
            // Possible field computation/modification- bail.
            return boost::none;
        }
    }
    MONGO_UNREACHABLE_TASSERT(234);
}

boost::optional<PathId> PathResolver::resolve(const FieldPath& fieldPath,
                                              const DocumentSource* at,
                                              boost::optional<NodeId> nodeId) {
    const pipeline::dependency_graph::DependencyGraph* graph = nullptr;
    const bool mainPipelineResolution = !nodeId || nodeId == _baseNodeId;
    if (mainPipelineResolution) {
        graph = &_graph;
    } else if (auto it = _nodeLookups.find(*nodeId); it != _nodeLookups.end()) {
        graph = _graph.getSubpipelineGraph(it->second.sourceLookup);
    }
    tassert(12835902, "Expected to find a dependency graph", graph);

    // Make sure this path is actually valid before proceeding. We validate the path where it is
    // referenced for arrayness.
    if (!isPathValid(*graph, at, fieldPath)) {
        return boost::none;
    }

    boost::optional<ResolvedPath> resolved =
        _resolve(*graph, fieldPath, at, nodeId.get_value_or(_baseNodeId));
    if (!resolved) {
        return boost::none;
    }

    // One last check! If this is not originating from the base node, ensure that we check if it was
    // modified in any way by the sub-pipeline of the node that produced it (e.g. report renames/
    // ban computations). We don't do this for the base node, because we allow a trailing suffix
    // after our join-opt-eligible prefix to modify any path.
    if (!mainPipelineResolution) {
        tassert(12836400,
                "Expected to be resolving within a subpipeline, or unsupported nested $lookups",
                nodeId && resolved->nodeId == *nodeId);

        // We need to look for aliases- but we still need to check for any field modifications after
        // this was resolved.
        auto alias =
            graph->getBaseDocumentFieldAlias(nullptr, resolved->underlyingFieldPath.fullPath());
        auto subOrigin =
            graph->resolveFieldOrigin(nullptr, resolved->underlyingFieldPath.fullPath());
        if (alias) {
            // We have some rename of the base path after the point where our join predicate was
            // defined- track it.
            resolved->fieldPathAfterRenames = *alias;
        } else if (subOrigin.kind != pipeline::dependency_graph::FieldOriginKind::kBaseDocument) {
            // Path was modified- bail.
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
