/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/plan_cache/join_plan_cache_key.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_cache/plan_cache_key_info.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"

namespace mongo {

JoinPlanCacheKey makeJoinPlanCacheKey(const join_ordering::JoinGraph& graph,
                                      const std::vector<join_ordering::ResolvedPath>& resolvedPaths,
                                      const MultipleCollectionAccessor& collections) {
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
        encodeUserString(path.fieldName.fullPath(), &sb);
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

    return sb.str();
}

}  // namespace mongo
