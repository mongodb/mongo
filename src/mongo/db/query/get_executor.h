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

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/parsed_distinct.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/update/update_driver.h"

namespace mongo {

class Collection;
class CountRequest;

/**
 * Filter indexes retrieved from index catalog by
 * allowed indices in query settings.
 * Used by getExecutor().
 * This function is public to facilitate testing.
 */
void filterAllowedIndexEntries(const AllowedIndicesFilter& allowedIndicesFilter,
                               std::vector<IndexEntry>* indexEntries);

/**
 * Fill out the provided 'plannerParams' for the 'canonicalQuery' operating on the collection
 * 'collection'.  Exposed for testing.
 */
void fillOutPlannerParams(OperationContext* opCtx,
                          Collection* collection,
                          CanonicalQuery* canonicalQuery,
                          QueryPlannerParams* plannerParams);

/**
 * Converts the catalog metadata for an index into an IndexEntry, which is a format that is meant to
 * be consumed by the query planner. This function can perform index reads and should not be called
 * unless access to the storage engine is permitted.
 *
 * When 'canonicalQuery' is not null, only multikey metadata paths that intersect with the query
 * field set will be retrieved for a multikey wildcard index. Otherwise all multikey metadata paths
 * will be retrieved.
 */
IndexEntry indexEntryFromIndexCatalogEntry(OperationContext* opCtx,
                                           const IndexCatalogEntry& ice,
                                           const CanonicalQuery* canonicalQuery = nullptr);

/**
 * Converts the catalog metadata for an index into a CoreIndexInfo, which is a format that is meant
 * to be used to update the plan cache. This function has no side effects and is safe to call in
 * all contexts.
 */
CoreIndexInfo indexInfoFromIndexCatalogEntry(const IndexCatalogEntry& ice);

/**
 * Determines whether or not to wait for oplog visibility for a query. This is only used for
 * collection scans on the oplog.
 */
bool shouldWaitForOplogVisibility(OperationContext* opCtx,
                                  const Collection* collection,
                                  bool tailable);

/**
 * Get a plan executor for a query.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutor(
    OperationContext* opCtx,
    Collection* collection,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    PlanExecutor::YieldPolicy yieldPolicy,
    size_t plannerOptions = 0);

/**
 * Get a plan executor for a .find() operation.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    Collection* collection,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    size_t plannerOptions = QueryPlannerParams::DEFAULT);

/**
 * Returns a plan executor for a legacy OP_QUERY find.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorLegacyFind(
    OperationContext* opCtx,
    Collection* collection,
    std::unique_ptr<CanonicalQuery> canonicalQuery);

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
 * Get an executor that potentially uses a DISTINCT_SCAN, intended for either a "distinct" command
 * or an aggregation pipeline that uses a $group stage with distinct-like semantics.
 *
 * Distinct is unique in that it doesn't care about getting all the results; it just wants all
 * possible values of a certain field.  As such, we can skip lots of data in certain cases (see body
 * of method for detail).
 *
 * A $group stage on a single field behaves similarly to a distinct command. If it has no
 * accumulators or only $first accumulators, the $group command only needs to visit one document for
 * each distinct value of the grouped-by (_id) field to compute its result. When there is a sort
 * order specified in parsedDistinct->getQuery()->getQueryRequest.getSort(), the DISTINCT_SCAN will
 * follow that sort order, ensuring that it chooses the correct document from each group to compute
 * any $first accumulators.
 *
 * Specify the QueryPlannerParams::STRICT_DISTINCT_ONLY flag in the 'params' argument to ensure that
 * any resulting plan _guarantees_ it will return exactly one document per value of the distinct
 * field. Without this flag, getExecutorDistinct() may use a plan that takes advantage of
 * DISTINCT_SCAN to filter some but not all duplicates (so that de-duplication is still necessary
 * after query execution), or it may fall back to a regular IXSCAN.
 *
 * Note that this function uses the projection in 'parsedDistinct' to produce a covered query when
 * possible, but when a covered query is not possible, the resulting plan may elide the projection
 * stage (instead returning entire fetched documents).
 *
 * For example, a distinct query on field 'b' could use a DISTINCT_SCAN over index {a: 1, b: 1}.
 * This plan will reduce the output set by filtering out documents that are equal on both the 'a'
 * and 'b' fields, but it could still output documents with equal 'b' values if their 'a' fields are
 * distinct.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDistinct(
    OperationContext* opCtx,
    Collection* collection,
    size_t plannerOptions,
    ParsedDistinct* parsedDistinct);

/*
 * Get a PlanExecutor for a query executing as part of a count command.
 *
 * Count doesn't care about actually examining its results; it just wants to walk through them.
 * As such, with certain covered queries, we can skip the overhead of fetching etc. when
 * executing a count.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorCount(
    OperationContext* opCtx, Collection* collection, const CountRequest& request, bool explain);

/**
 * Get a PlanExecutor for a delete operation. 'parsedDelete' describes the query predicate
 * and delete flags like 'isMulti'. The caller must hold the appropriate MODE_X or MODE_IX
 * locks, and must not release these locks until after the returned PlanExecutor is deleted.
 *
 * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
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
    OperationContext* opCtx, OpDebug* opDebug, Collection* collection, ParsedDelete* parsedDelete);

/**
 * Get a PlanExecutor for an update operation. 'parsedUpdate' describes the query predicate
 * and update modifiers. The caller must hold the appropriate MODE_X or MODE_IX locks prior
 * to calling this function, and must not release these locks until after the returned
 * PlanExecutor is deleted.
 *
 * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
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
    OperationContext* opCtx, OpDebug* opDebug, Collection* collection, ParsedUpdate* parsedUpdate);
}  // namespace mongo
