/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/batched_delete_stage.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/executor/task_executor_cursor.h"

namespace mongo {

class Collection;
class CollectionPtr;
class CountRequest;

/**
 * Make an ExpressionContext to be used for non-aggregate commands. The result of this can be passed
 * into any of the getExecutor* functions.
 *
 * Note that the getExecutor* functions may change the collation on the returned ExpressionContext
 * if the collection has a default collation and no collation was specifically requested
 * ('requestCollation' is empty).
 */
boost::intrusive_ptr<ExpressionContext> makeExpressionContextForGetExecutor(
    OperationContext* opCtx,
    const BSONObj& requestCollation,
    const NamespaceString& nss,
    boost::optional<ExplainOptions::Verbosity> verbosity);

/**
 * Gets a plan executor for a query. If the query is valid and an executor could be created, returns
 * a StatusWith with the PlanExecutor. If the query cannot be executed, returns a Status indicating
 * why.
 *
 * If the caller provides a 'pipeline' pointer and the query is eligible for running in SBE, a
 * prefix of the pipeline might be moved into the provided 'canonicalQuery' for pushing down into
 * the find layer.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    QueryPlannerParams plannerOptions = QueryPlannerParams{},
    Pipeline* pipeline = nullptr,
    bool needsMerge = false,
    QueryMetadataBitSet unavailableMetadata = QueryMetadataBitSet{});

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getSearchMetadataExecutorSBE(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const NamespaceString& nss,
    const CanonicalQuery& cq,
    executor::TaskExecutorCursor metadataCursor);

/**
 * If possible, turn the provided QuerySolution into a QuerySolution that uses a DistinctNode
 * to provide results for the distinct command.
 *
 * When 'strictDistinctOnly' is false, any resulting QuerySolution will limit the number of
 * documents that need to be examined to compute the results of a distinct command, but it may not
 * guarantee that there are no duplicate values for the distinct field.
 *
 * If the provided solution could be mutated successfully, returns true, otherwise returns
 * false.
 */
bool turnIxscanIntoDistinctIxscan(QuerySolution* soln,
                                  const std::string& field,
                                  bool strictDistinctOnly);

/**
 * Attempts to get an executor that uses a DISTINCT_SCAN, intended for either a "distinct" command
 * or an aggregation pipeline that uses a $group stage with distinct-like semantics. If a
 * DISTINCT_SCAN cannot be created for the given arguments, returns
 * ErrorCodes::NoQueryExecutionPlans.
 *
 * Specify the QueryPlannerParams::STRICT_DISTINCT_ONLY flag in the 'plannerOptions' argument to
 * ensure that any resulting plan _guarantees_ it will return exactly one document per value of the
 * distinct field. For example, a DISTINCT_SCAN over index {a: 1, b: 1} will return documents that
 * are equal on both the 'a' and 'b' fields, meaning that there might be duplicated values of 'b' if
 * the corresponding values of 'a' are distinct. The distinct('b') command can reduce this set
 * further to only return distinct values of 'b', but {$group: {_id: '$b'}} doesn't do the further
 * reduction and instead would set the STRICT_DISTINCT_ONLY flag to prevent choosing a DISTINCT_SCAN
 * over the {a: 1, b: 1} index.
 *
 * Providing QueryPlannerParams::STRICT_DISTINCT_ONLY also implies that the resulting plan will not
 * "unwind" arrays. That is, it will not return separate values for each element in an array. For
 * example, in a collection with documents {a: [10, 11]}, {a: 12}, the distinct('a') command
 * should return "unwound" values 10, 11, and 12, but {$group: {_id: '$a'}} needs to see the
 * documents for the original [10, 11] and 12 values. Thus, the latter would use the
 * STRICT_DISTINCT_ONLY option to preserve the arrays.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> tryGetExecutorDistinct(
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    CanonicalDistinct& canonicalDistinct,
    bool flipDistinctScanDirection = false);

/*
 * Get a PlanExecutor for a query executing as part of a count command.
 *
 * Count doesn't care about actually examining its results; it just wants to walk through them.
 * As such, with certain covered queries, we can skip the overhead of fetching etc. when
 * executing a count.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorCount(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const CollectionPtr* collection,
    const CountCommandRequest& request,
    const NamespaceString& nss);

/**
 * Get a PlanExecutor for a delete operation. 'parsedDelete' describes the query predicate
 * and delete flags like 'isMulti'. The caller must hold the appropriate MODE_X or MODE_IX
 * locks, and must not release these locks until after the returned PlanExecutor is deleted.
 *
 * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
 *
 * If the delete operation is executed in explain mode, the 'verbosity' parameter should be
 * set to the requested verbosity level, or boost::none otherwise.
 *
 * The returned PlanExecutor will used the YieldPolicy returned by parsedDelete->yieldPolicy().
 *
 * Does not take ownership of its arguments.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDelete(
    OpDebug* opDebug,
    CollectionAcquisition coll,
    ParsedDelete* parsedDelete,
    boost::optional<ExplainOptions::Verbosity> verbosity);

/**
 * Get a PlanExecutor for an update operation. 'parsedUpdate' describes the query predicate
 * and update modifiers. The caller must hold the appropriate MODE_X or MODE_IX locks prior
 * to calling this function, and must not release these locks until after the returned
 * PlanExecutor is deleted.
 *
 * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
 *
 * If the delete operation is executed in explain mode, the 'verbosity' parameter should be
 * set to the requested verbosity level, or boost::none otherwise.
 *
 * The returned PlanExecutor will used the YieldPolicy returned by parsedUpdate->yieldPolicy().
 *
 * Does not take ownership of its arguments.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorUpdate(
    OpDebug* opDebug,
    CollectionAcquisition coll,
    ParsedUpdate* parsedUpdate,
    boost::optional<ExplainOptions::Verbosity> verbosity);

/**
 * Direction of collection scan plan executor returned by makeCollectionScanPlanExecutor() below.
 */
enum class CollectionScanDirection {
    kForward = 1,
    kBackward = -1,
};

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getCollectionScanExecutor(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    CollectionScanDirection scanDirection,
    const boost::optional<RecordId>& resumeAfterRecordId = boost::none);


}  // namespace mongo
