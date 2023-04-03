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
#include "mongo/db/exec/batched_delete_stage.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/parsed_distinct.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/update/update_driver.h"

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
    OperationContext* opCtx, const BSONObj& requestCollation, const NamespaceString& nss);

/**
 * Filter indexes retrieved from index catalog by
 * allowed indices in query settings.
 * Used by getExecutor().
 * This function is public to facilitate testing.
 */
void filterAllowedIndexEntries(const AllowedIndicesFilter& allowedIndicesFilter,
                               std::vector<IndexEntry>* indexEntries);

/**
 * Fills out information about secondary collections held by 'collections' in 'plannerParams'.
 */
std::map<NamespaceString, SecondaryCollectionInfo> fillOutSecondaryCollectionsInformation(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const CanonicalQuery* canonicalQuery);

/**
 * Fill out the provided 'plannerParams' for the 'canonicalQuery' operating on the collection
 * 'collection'.
 */
void fillOutPlannerParams(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          const CanonicalQuery* canonicalQuery,
                          QueryPlannerParams* plannerParams);
/**
 * Overload of the above function that does two things:
 * - Calls the single collection overload of 'fillOutPlannerParams' on the main collection held
 * by 'collections'
 * - Calls 'fillOutSecondaryCollectionsInformation' to store information about the set of
 * secondary collections held by 'collections' on 'plannerParams'.
 */
void fillOutPlannerParams(OperationContext* opCtx,
                          const MultipleCollectionAccessor& collections,
                          const CanonicalQuery* canonicalQuery,
                          QueryPlannerParams* plannerParams);

/**
 * Return whether or not any component of the path 'path' is multikey given an index key pattern
 * and multikeypaths. If no multikey metdata is available for the index, and the index is marked
 * multikey, conservatively assumes that a component of 'path' _is_ multikey. The 'isMultikey'
 * property of an index is false for indexes that definitely have no multikey paths.
 */
bool isAnyComponentOfPathMultikey(const BSONObj& indexKeyPattern,
                                  bool isMultikey,
                                  const MultikeyPaths& indexMultikeyInfo,
                                  StringData path);

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
                                           const CollectionPtr& collection,
                                           const IndexCatalogEntry& ice,
                                           const CanonicalQuery* canonicalQuery = nullptr);

/**
 * Converts the catalog metadata for an index into an ColumnIndexEntry, which is a format that is
 * meant to be consumed by the query planner. This function can perform index reads and should not
 * be called unless access to the storage engine is permitted.
 */
ColumnIndexEntry columnIndexEntryFromIndexCatalogEntry(OperationContext* opCtx,
                                                       const CollectionPtr& collection,
                                                       const IndexCatalogEntry& ice);

/**
 * Determines whether or not to wait for oplog visibility for a query. This is only used for
 * collection scans on the oplog.
 */
bool shouldWaitForOplogVisibility(OperationContext* opCtx,
                                  const CollectionPtr& collection,
                                  bool tailable);

/**
 * Get a plan executor for a query.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 *
 * If the caller provides a 'extractAndAttachPipelineStages' function and the query is eligible for
 * pushdown into the find layer this function will be invoked to extract pipeline stages and
 * attach them to the provided 'CanonicalQuery'. This function should capture the Pipeline that
 * stages should be extracted from. If the boolean 'attachOnly' argument is true, it will only find
 * and attach the applicable stages to the query. If it is false, it will remove the extracted
 * stages from the pipeline.
 *
 * Note that the first overload takes a 'MultipleCollectionAccessor' and can construct a
 * PlanExecutor over multiple collections, while the second overload takes a single 'CollectionPtr'
 * and can only construct a PlanExecutor over a single collection.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutor(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::function<void(CanonicalQuery*, bool)> extractAndAttachPipelineStages,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const QueryPlannerParams& plannerOptions);

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutor(
    OperationContext* opCtx,
    const CollectionPtr* collection,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::function<void(CanonicalQuery*, bool)> extractAndAttachPipelineStages,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions = 0);

/**
 * Get a plan executor for a .find() operation. The executor will have a 'YIELD_AUTO' yield policy
 * unless a false value for 'permitYield' or being part of a multi-document transaction forces it to
 * have a 'NO_INTERRUPT' yield policy.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 *
 * If the caller provides a 'extractAndAttachPipelineStages' function and the query is eligible for
 * pushdown into the find layer this function will be invoked to extract pipeline stages and
 * attach them to the provided 'CanonicalQuery'. This function should capture the Pipeline that
 * stages should be extracted from. If the boolean 'attachOnly' argument is true, it will only find
 * and attach the applicable stages to the query. If it is false, it will remove the extracted
 * stages from the pipeline.
 *
 * Note that the first overload takes a 'MultipleCollectionAccessor' and can construct a
 * PlanExecutor over multiple collections, while the second overload takes a single
 * 'CollectionPtr' and can only construct a PlanExecutor over a single collection.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::function<void(CanonicalQuery*, bool)> extractAndAttachPipelineStages,
    bool permitYield = false,
    QueryPlannerParams plannerOptions = QueryPlannerParams{});

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    const CollectionPtr* collection,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::function<void(CanonicalQuery*, bool)> extractAndAttachPipelineStages,
    bool permitYield = false,
    size_t plannerOptions = QueryPlannerParams::DEFAULT);

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
 * order specified in parsedDistinct->getQuery()->getFindCommandRequest().getSort(), DISTINCT_SCAN
 * will follow that sort order, ensuring that it chooses the correct document from each group to
 * compute any $first accumulators.
 *
 * Specify the QueryPlannerParams::STRICT_DISTINCT_ONLY flag in the 'params' argument to ensure that
 * any resulting plan _guarantees_ it will return exactly one document per value of the distinct
 * field. Without this flag, getExecutorDistinct() may use a plan that takes advantage of
 * DISTINCT_SCAN to filter some but not all duplicates (so that de-duplication is still necessary
 * after query execution), or it may fall back to a regular IXSCAN.
 *
 * Providing QueryPlannerParams::STRICT_DISTINCT_ONLY also implies that the resulting plan may not
 * "unwind" arrays. That is, it will not return separate values for each element in an array. For
 * example, in a collection with documents {a: [10, 11]}, {a: 12}, a distinct command on field 'a'
 * can process the "unwound" values 10, 11, and 12, but a $group by 'a' needs to see documents for
 * the original [10, 11] and 12 values. In the latter case (in which the caller provides a
 * STRICT_DISTINCT_ONLY), a DISTINCT_SCAN is not possible, and the caller would have to fall back
 * to a different plan.
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
    const CollectionPtr* collection,
    size_t plannerOptions,
    ParsedDistinct* parsedDistinct,
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
    bool explain,
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
    const CollectionPtr* collection,
    ParsedDelete* parsedDelete,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    DeleteStageParams::DocumentCounter&& documentCounter = nullptr);

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
    const CollectionPtr* collection,
    ParsedUpdate* parsedUpdate,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    UpdateStageParams::DocumentCounter&& documentCounter = nullptr);

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
