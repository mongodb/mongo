/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/session/logical_session_id.h"

#include <boost/optional/optional.hpp>


namespace mongo {
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForFindById(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    VariantCollectionPtrOrAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson);

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForFindByClusteredId(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    VariantCollectionPtrOrAcquisition coll,
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
    VariantCollectionPtrOrAcquisition coll,
    const IndexForExpressEquality& index,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson);

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForUpdate(
    OperationContext* opCtx,
    CollectionAcquisition collection,
    ParsedUpdate* parsedUpdate,
    bool returnOwnedBson);

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForDelete(
    OperationContext* opCtx, CollectionAcquisition collection, ParsedDelete* parsedDelete);

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
