// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/write_ops/canonical_delete.h"
#include "mongo/db/query/write_ops/canonical_update.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>


namespace mongo {
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForFindById(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    CollectionAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson);

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForFindByClusteredId(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    CollectionAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson);

struct IndexForExpressEquality {
    IndexForExpressEquality(IndexEntry index, bool coversProjection)
        : index(std::move(index)), coversProjection(coversProjection) {}

    bool operator==(const IndexForExpressEquality& rhs) const = default;

    IndexEntry index;
    bool coversProjection;
};

std::ostream& operator<<(std::ostream& stream, const IndexForExpressEquality& i);

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForFindByUserIndex(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    CollectionAcquisition coll,
    const IndexForExpressEquality& index,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson);

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForUpdate(
    OperationContext* opCtx,
    CollectionAcquisition collection,
    CanonicalUpdate& canonicalUpdate,
    bool returnOwnedBson);

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForDelete(
    OperationContext* opCtx, CollectionAcquisition collection, CanonicalDelete& canonicalDelete);

/**
 * Tries to find an index suitable for use in the express equality path. Excludes indexes which
 * cannot 1) satisfy the given query with exact bounds and 2) provably return at most one result
 * doc. If at least one suitable index remains, returns the entry for the index with the fewest
 * fields. If not, returns nullptr. If an index exists, that can cover project, will return this
 * index and set coversProjection flag to true.
 */
boost::optional<IndexForExpressEquality> getIndexForExpressEquality(
    const CanonicalQuery& cq, const QueryPlannerParams& plannerParams);

inline BSONObj getQueryFilterMaybeUnwrapEq(const BSONObj& query) {
    // We allow queries of the shape {_id: {$eq: <value>}} to use the express path, but we
    // want to pass in BSON of the shape {_id: <value>} to the executor for consistency and
    // because a later code path may rely on this shape. Note that we don't have to use
    // 'isExactMatchOnId' here since we know we haven't reached this code via the eligibility
    // check on the CanonicalQuery's MatchExpression (since there was no CanonicalQuery created
    // for this path). Therefore, we know the incoming query is either exactly of the shape
    // {_id: <value>} or {_id: {$eq: <value>}}.
    if (const auto idValue = query["_id"]; idValue.isABSONObj() && idValue.Obj().hasField("$eq")) {
        return idValue.Obj()["$eq"].wrap("_id");
    }
    return query;
}
}  // namespace mongo
