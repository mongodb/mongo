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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/exec_shard_filter_policy.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/group_from_first_document_transformation.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <utility>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
class Collection;
class CollectionPtr;
class DocumentSourceCursor;
class DocumentSourceMatch;
class DocumentSourceSort;
class ExpressionContext;

class SkipThenLimit;
class OperationContext;
class Pipeline;
struct PlanSummaryStats;

/**
 * PipelineD is an extension of the Pipeline class, but with additional material that references
 * symbols that are not available in mongos, where the remainder of the Pipeline class also
 * functions.  PipelineD is a friend of Pipeline so that it can have equal access to Pipeline's
 * members.
 *
 * See the friend declaration in Pipeline.
 */
class PipelineD {
public:
    /**
     * This callback function is called to attach a query PlanExecutor to the given Pipeline by
     * creating a specific DocumentSourceCursor stage using the provided PlanExecutor, and adding
     * the new stage to the pipeline.
     */
    using AttachExecutorCallback = std::function<void(
        const MultipleCollectionAccessor&,
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>,
        Pipeline*,
        const boost::intrusive_ptr<DocumentSourceCursor::CatalogResourceHandle>&)>;

    /**
     * A tuple to represent the result of query executors, includes a main executor, its pipeline
     * attaching callback function, and a vector of additional executors that help to serve the
     * aggregation.
     */
    struct BuildQueryExecutorResult {
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> mainExecutor;
        AttachExecutorCallback attachExecutorCallback;
        std::vector<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> additionalExecutors;
    };

    /**
     * This method looks for early pipeline stages that can be folded into the underlying
     * PlanExecutor, and removes those stages from the pipeline when they can be absorbed by the
     * PlanExecutor. For example, an early $match can be removed and replaced with a
     * DocumentSourceCursor containing a PlanExecutor that will do an index scan.
     *
     * Callers must take care to ensure that 'nss' and each collection referenced in
     * 'collections' is locked in at least IS-mode.
     *
     * When not null, 'aggRequest' provides access to pipeline command options such as hint.
     *
     * The 'collections' parameter can reference any number of collections.
     *
     * This method will not add a $cursor stage to the pipeline, but will create a PlanExecutor and
     * a callback function. The executor and the callback can later be used to create the $cursor
     * stage and add it to the pipeline by calling 'attachInnerQueryExecutorToPipeline()' method.
     * If the pipeline doesn't require a $cursor stage, the plan executor will be returned as
     * 'nullptr'.
     */
    static BuildQueryExecutorResult buildInnerQueryExecutor(
        const MultipleCollectionAccessor& collections,
        const NamespaceString& nss,
        const AggregateCommandRequest* aggRequest,
        Pipeline* pipeline,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{});

    /**
     * Completes creation of the $cursor stage using the given callback pair obtained by calling
     * 'buildInnerQueryExecutor()' method. If the callback doesn't hold a valid PlanExecutor, the
     * method does nothing. Otherwise, a new $cursor stage is created using the given PlanExecutor,
     * and added to the pipeline. The 'collections' parameter can reference any number of
     * collections. 'transactionResourcesStasher' must point to a
     ShardRole::TransactionResourcesStasher that will hold the ShardRole::TransactionResources
     associated with 'collections' and 'exec'.

     */
    static void attachInnerQueryExecutorToPipeline(
        const MultipleCollectionAccessor& collection,
        AttachExecutorCallback attachExecutorCallback,
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
        Pipeline* pipeline,
        const boost::intrusive_ptr<DocumentSourceCursor::CatalogResourceHandle>&
            catalogResourceHandle);

    /**
     * This method combines 'buildInnerQueryExecutor()' and 'attachInnerQueryExecutorToPipeline()'
     * into a single call to support auto completion of the cursor stage creation process. Can be
     * used when the executor attachment phase doesn't need to be deferred and the $cursor stage
     * can be created right after building the executor. 'transactionResourcesStasher' must point to
     a ShardRole::TransactionResourcesStasher that will hold the ShardRole::TransactionResources
     associated with 'collections'.
     */
    static void buildAndAttachInnerQueryExecutorToPipeline(
        const MultipleCollectionAccessor& collections,
        const NamespaceString& nss,
        const AggregateCommandRequest* aggRequest,
        Pipeline* pipeline,
        const boost::intrusive_ptr<DocumentSourceCursor::CatalogResourceHandle>&
            transactionResourcesStasher,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{});

    static Timestamp getLatestOplogTimestamp(const exec::agg::Pipeline* pipeline);

    /**
     * Retrieves postBatchResumeToken from the 'pipeline' if it is available. Returns an empty
     * object otherwise.
     */
    static BSONObj getPostBatchResumeToken(const exec::agg::Pipeline* pipeline);

    // Returns true if it is a $search pipeline, 'featureFlagSearchInSbe' is enabled and
    // forceClassicEngine is false.
    static bool isSearchPresentAndEligibleForSbe(const Pipeline* pipeline);

private:
    PipelineD();  // does not exist:  prevent instantiation

    /**
     * Build a PlanExecutor and prepare callback to create a generic DocumentSourceCursor for
     * the 'pipeline'.
     */
    static BuildQueryExecutorResult buildInnerQueryExecutorGeneric(
        const MultipleCollectionAccessor& collections,
        const NamespaceString& nss,
        const AggregateCommandRequest* aggRequest,
        Pipeline* pipeline,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{});

    /**
     * Helper to perform bounded sort optimization rewrites on time-series collections. The rewrite
     * involves a replacement of the $sort stage with a bounded sort stage.
     *
     *  See ../db/query/timeseries/README.md#_internalboundedsort-on-the-timefield for a description
     * of the bounded sort optimization.
     */
    static void performBoundedSortOptimization(PlanStage* rootStage,
                                               Pipeline* pipeline,
                                               const DocumentSourceSort* sort,
                                               DocumentSourceInternalUnpackBucket* unpack);

    static BuildQueryExecutorResult buildInnerQueryExecutorSearch(
        const MultipleCollectionAccessor& collections,
        const NamespaceString& nss,
        const AggregateCommandRequest* aggRequest,
        Pipeline* pipeline);

    /**
     * Build a PlanExecutor and prepare a callback to create a special DocumentSourceGeoNearCursor
     * for the 'pipeline'. Unlike 'buildInnerQueryExecutorGeneric()', throws if the main collection
     * defined on 'collections' does not exist, as the $geoNearCursor requires a 2d or 2dsphere
     * index.
     *
     * Note that this method takes a 'MultipleCollectionAccessor' even though
     * DocumentSourceGeoNearCursor only operates over a single collection because the underlying
     * execution API expects a 'MultipleCollectionAccessor'.
     */
    static BuildQueryExecutorResult buildInnerQueryExecutorGeoNear(
        const MultipleCollectionAccessor& collections,
        const NamespaceString& nss,
        const AggregateCommandRequest* aggRequest,
        Pipeline* pipeline);

    /**
     * Build a PlanExecutor and prepare a callback to create a special DocumentSourceSample or a
     * DocumentSourceInternalUnpackBucket stage that has been rewritten to sample buckets using a
     * storage engine supplied random cursor if the heuristics used for the optimization allows. If
     * the optimized $sample plan cannot or should not be produced, returns a null PlanExecutor
     * pointer.
     */
    static BuildQueryExecutorResult buildInnerQueryExecutorSample(
        DocumentSourceSample* sampleStage,
        DocumentSourceInternalUnpackBucket* unpackBucketStage,
        const CollectionAcquisition& collection,
        Pipeline* pipeline);

    /**
     * Returns a 'PlanExecutor' which uses a random cursor to sample documents if successful as
     * determined by the boolean. Returns {} if the storage engine doesn't support random cursors,
     * or if 'sampleSize' is a large enough percentage of the collection.
     *
     * Note: this function may mutate the input 'pipeline' sources in the case of timeseries
     * collections. It always pushes down the $_internalUnpackBucket source to the PlanStage layer.
     * If the chosen execution plan is ARHASH and we are in an unsharded environment, the $sample
     * stage will also be erased and pushed down. In the sharded case, we still need a separate
     * $sample stage to preserve sorting metadata for the AsyncResultsMerger to merge samples
     * returned by multiple shards.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
    createRandomCursorExecutor(const CollectionAcquisition& coll,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               Pipeline* pipeline,
                               long long sampleSize,
                               long long numRecords,
                               boost::optional<timeseries::BucketUnpacker> bucketUnpacker);

    typedef bool IndexSortOrderAgree;
    typedef bool IndexOrderedByMinTime;

    /*
     * Takes a leaf plan stage and a sort pattern and returns a pair if they support the Bucket
     * Unpacking with Sort Optimization. The pair includes whether the index order and sort order
     * agree with each other as its first member and the order of the index as the second parameter.
     *
     * Note that the index scan order is different from the index order.
     */
    static boost::optional<std::pair<IndexSortOrderAgree, IndexOrderedByMinTime>> supportsSort(
        const timeseries::BucketUnpacker& bucketUnpacker, PlanStage* root, const SortPattern& sort);

    /* This is a helper method for supportsSort. It takes the current iterator for the index
     * keyPattern, the direction of the index scan, the timeField path we're sorting on, and the
     * direction of the sort. It returns a pair if this data agrees and none if it doesn't.
     *
     * The pair contains whether the index order and the sort order agree with each other as the
     * firstmember and the order of the index as the second parameter.
     *
     * Note that the index scan order may be different from the index order.
     * N.B.: It ASSUMES that there are two members left in the keyPatternIter iterator, and that the
     * timeSortFieldPath is in fact the path on time.
     */
    static boost::optional<std::pair<IndexSortOrderAgree, IndexOrderedByMinTime>> checkTimeHelper(
        const timeseries::BucketUnpacker& bucketUnpacker,
        BSONObj::iterator& keyPatternIter,
        bool scanIsForward,
        const FieldPath& timeSortFieldPath,
        bool sortIsAscending);

    static bool sortAndKeyPatternPartAgreeAndOnMeta(
        const timeseries::BucketUnpacker& bucketUnpacker,
        StringData keyPatternFieldName,
        const FieldPath& sortFieldPath);
};

// Public-facing version of internal function for use in join-ordering opt.
StatusWith<std::unique_ptr<CanonicalQuery>> createCanonicalQuery(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    Pipeline& pipeline);
}  // namespace mongo
