// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_cache/join_plan_cache_key.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_cache/plan_cache_key_info.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_hash.h"

#include <boost/container_hash/hash.hpp>

namespace mongo {

JoinPlanCacheKey::JoinPlanCacheKey(std::string shape, query_settings::QuerySettings querySettings)
    : _shape(std::move(shape)), _querySettings(std::move(querySettings)) {
    // The key is immutable, so the hash and size are computed once. hash() runs several times
    // per lookup, and keySizeInBytes() is called by the budget estimator on every insertion,
    // eviction, replacement, and removal.
    size_t h = canonical_query_encoder::computeHash(_shape);
    boost::hash_combine(h, query_settings::hash(_querySettings));
    _hash = static_cast<uint32_t>(h);
    _keySizeInBytes = sizeof(*this) + _shape.size() + _querySettings.toBSON().objsize();
}

JoinPlanCacheKey makeJoinPlanCacheKey(const join_ordering::JoinGraph& graph,
                                      const std::vector<join_ordering::ResolvedPath>& resolvedPaths,
                                      const MultipleCollectionAccessor& collections,
                                      const query_settings::QuerySettings& querySettings) {
    StringBuilder sb;
    sb << "n=" << graph.numNodes() << ";e=" << graph.numEdges() << '\n';

    // Node section: emit nodes in ascending nodeId order.
    for (size_t i = 0; i < graph.numNodes(); ++i) {
        const auto& node = graph.getNode(static_cast<join_ordering::NodeId>(i));

        // Emit collection name and embed path (if any) for the node. The nodeId is implicit in the
        // order of the nodes, so we don't need to emit it.
        sb << NamespaceStringUtil::serialize(node.collectionName,
                                             SerializationContext::stateDefault())
           << kEncodeSectionDelimiter << (node.embedPath ? node.embedPath->fullPath() : "")
           << kEncodeSectionDelimiter;

        // Emit the node's match expression shape and indexability discriminators
        if (node.accessPath) {
            auto shapeString =
                canonical_query_encoder::encodeCanonicalQueryForJoin(*node.accessPath);

            StringBuilder idxBuilder;
            const auto& coll = collections.lookupCollection(node.collectionName);
            tassert(12926105,
                    fmt::format("collection {} not found in MultipleCollectionAccessor",
                                node.collectionName.toStringForErrorMsg()),
                    coll);
            plan_cache_detail::encodeIndexability(
                node.accessPath->getPrimaryMatchExpression(),
                CollectionQueryInfo::get(coll).getPlanCacheIndexabilityState(),
                &idxBuilder);

            PlanCacheKeyInfo keyInfo(
                std::move(shapeString), idxBuilder.str(), query_settings::QuerySettings{});
            sb << keyInfo.toString();
        }
        sb << '\n';
    }

    // Section separator between nodes and edges.
    sb << '\n';

    auto encodeResolvedPath = [&](const join_ordering::ResolvedPath& path, StringBuilder& sb) {
        sb << kEncodeChildrenBegin << path.nodeId << ',';
        encodeUserString(path.underlyingFieldPath.fullPath(), &sb);
        sb << kEncodeChildrenEnd;
    };

    // Edge section: emit edges in ascending edgeId order.
    for (join_ordering::EdgeId i = 0; i < graph.numEdges(); ++i) {
        const auto& edge = graph.getEdge(i);

        // Emit the edge's left and right nodeIds
        sb << static_cast<int>(edge.left) << kEncodeSectionDelimiter << static_cast<int>(edge.right)
           << '\n';

        // Emit the predicates on the edge in order. The same query shape should result in the same
        // order of predicates, so we don't need to sort them.
        for (const auto& pred : edge.predicates) {
            const auto& left = resolvedPaths[pred.left];
            const auto& right = resolvedPaths[pred.right];
            encodeResolvedPath(left, sb);
            sb << kEncodeSectionDelimiter;
            encodeResolvedPath(right, sb);
            sb << kEncodeSectionDelimiter
               << (pred.op == join_ordering::JoinPredicate::Eq ? 'e' : 'x') << '\n';
        }
    }

    return JoinPlanCacheKey{sb.str(), querySettings};
}

}  // namespace mongo
