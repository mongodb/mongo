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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/batched_delete_stage.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id.h"

#include <cstdint>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class BSONObj;
class Collection;
class CollectionPtr;
class IndexDescriptor;
class OperationContext;
class PlanStage;
class CollectionAcquisition;
class WorkingSet;
struct UpdateStageParams;

/**
 * The internal planner is a one-stop shop for "off-the-shelf" plans.  Most internal procedures
 * that do not require advanced queries could be served by plans already in here.
 */
class InternalPlanner {
public:
    enum Direction {
        FORWARD = 1,
        BACKWARD = -1,
    };

    enum IndexScanOptions {
        // The client is interested in the default outputs of an index scan: BSONObj of the key,
        // RecordId of the record that's indexed.  The client does its own fetching if required.
        IXSCAN_DEFAULT = 0,

        // The client wants the fetched object and the RecordId that refers to it.  Delegating
        // the fetch to the runner allows fetching outside of a lock.
        IXSCAN_FETCH = 1,
    };

    /**
     * Convenient struct used to store all parameters needed to create a collectionScan executor.
     */
    struct CreateCollectionScanParams {
        OperationContext* opCtx;
        VariantCollectionPtrOrAcquisition collection;
        PlanYieldPolicy::YieldPolicy yieldPolicy;
        Direction direction = FORWARD;
        const boost::optional<RecordId>& resumeAfterRecordId = boost::none;
        boost::optional<RecordIdBound> minRecord = boost::none;
        boost::optional<RecordIdBound> maxRecord = boost::none;
        CollectionScanParams::ScanBoundInclusion boundInclusion =
            CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords;
        bool shouldReturnEofOnFilterMismatch = false;
        size_t plannerOptions = 0;
    };

    /**
     * Returns a sampling of the given collection with up to 'numSamples'. If the caller doesn't
     * provide a value for 'numSamples' then the executor will return an infinite stream of random
     * documents of the collection.
     *
     * Note that the set of documents returned can contain duplicates. Sampling is performed
     * without memory of the previous results.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> sampleCollection(
        OperationContext* opCtx,
        const CollectionAcquisition& collection,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        boost::optional<int64_t> numSamples = boost::none);

    /**
     * Returns a collection scan. Refer to CollectionScanParams for usage of 'minRecord' and
     * 'maxRecord'.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> collectionScan(
        OperationContext* opCtx,
        const VariantCollectionPtrOrAcquisition& collection,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        Direction direction = FORWARD,
        const boost::optional<RecordId>& resumeAfterRecordId = boost::none,
        boost::optional<RecordIdBound> minRecord = boost::none,
        boost::optional<RecordIdBound> maxRecord = boost::none,
        CollectionScanParams::ScanBoundInclusion boundInclusion =
            CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords,
        bool shouldReturnEofOnFilterMismatch = false);

    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> collectionScan(
        CreateCollectionScanParams&& params);

    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> collectionScan(
        OperationContext* opCtx,
        const VariantCollectionPtrOrAcquisition& collection,
        const CollectionScanParams& params,
        PlanYieldPolicy::YieldPolicy yieldPolicy);

    /**
     * Returns a FETCH => DELETE plan, or a FETCH => BATCHED_DELETE plan if 'batchedDeleteParams' is
     * set.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> deleteWithCollectionScan(
        OperationContext* opCtx,
        CollectionAcquisition collection,
        std::unique_ptr<DeleteStageParams> deleteStageParams,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        Direction direction = FORWARD,
        boost::optional<RecordIdBound> minRecord = boost::none,
        boost::optional<RecordIdBound> maxRecord = boost::none,
        CollectionScanParams::ScanBoundInclusion boundInclusion =
            CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords,
        std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams = nullptr,
        const MatchExpression* filter = nullptr,
        bool shouldReturnEofOnFilterMismatch = false);

    /**
     * Returns an index scan.  Caller owns returned pointer.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> indexScan(
        OperationContext* opCtx,
        const VariantCollectionPtrOrAcquisition& collection,
        const IndexDescriptor* descriptor,
        const BSONObj& startKey,
        const BSONObj& endKey,
        BoundInclusion boundInclusion,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        Direction direction = FORWARD,
        int options = IXSCAN_DEFAULT);

    /**
     * Returns an IXSCAN => FETCH => DELETE plan, or an IXSCAN => FETCH => BATCHED_DELETE plan if
     * 'batchedDeleteParams' is set.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> deleteWithIndexScan(
        OperationContext* opCtx,
        CollectionAcquisition collection,
        std::unique_ptr<DeleteStageParams> params,
        const IndexDescriptor* descriptor,
        const BSONObj& startKey,
        const BSONObj& endKey,
        BoundInclusion boundInclusion,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        Direction direction = FORWARD,
        std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams = nullptr);

    /**
     * Returns a scan over the 'shardKeyIdx'. If the 'shardKeyIdx' is a non-clustered index, returns
     * an index scan. If the 'shardKeyIdx' is a clustered idx, returns a bounded collection scan
     * since the clustered index does not require a separate index lookup table.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> shardKeyIndexScan(
        OperationContext* opCtx,
        const VariantCollectionPtrOrAcquisition& collection,
        const ShardKeyIndex& shardKeyIdx,
        const BSONObj& startKey,
        const BSONObj& endKey,
        BoundInclusion boundInclusion,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        Direction direction = FORWARD,
        int options = IXSCAN_DEFAULT);


    /**
     * Returns an IXSCAN => FETCH => DELETE plan when 'shardKeyIdx' indicates the index is a
     * standard index or a COLLSCAN => DELETE when 'shardKeyIdx' indicates the index is a clustered
     * index.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> deleteWithShardKeyIndexScan(
        OperationContext* opCtx,
        CollectionAcquisition collection,
        std::unique_ptr<DeleteStageParams> params,
        const ShardKeyIndex& shardKeyIdx,
        const BSONObj& startKey,
        const BSONObj& endKey,
        BoundInclusion boundInclusion,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams,
        Direction direction = FORWARD);

    /**
     * Returns an IDHACK => UPDATE plan.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> updateWithIdHack(
        OperationContext* opCtx,
        CollectionAcquisition collection,
        const UpdateStageParams& params,
        const IndexDescriptor* descriptor,
        const BSONObj& key,
        PlanYieldPolicy::YieldPolicy yieldPolicy);

private:
    /**
     * Returns a plan stage that can be used for a collection scan.
     *
     * Used as a helper for collectionScan() and deleteWithCollectionScan().
     */
    static std::unique_ptr<PlanStage> _collectionScan(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        WorkingSet* ws,
        const VariantCollectionPtrOrAcquisition& coll,
        Direction direction,
        const boost::optional<RecordId>& resumeAfterRecordId = boost::none,
        const boost::optional<RecordId>& minRecord = boost::none,
        const boost::optional<RecordId>& maxRecord = boost::none);

    static std::unique_ptr<PlanStage> _collectionScan(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        WorkingSet* ws,
        const VariantCollectionPtrOrAcquisition& coll,
        const CollectionScanParams& params,
        const MatchExpression* filter = nullptr);

    /**
     * Returns a plan stage that is either an index scan or an index scan with a fetch stage.
     *
     * Used as a helper for indexScan() and deleteWithIndexScan().
     */
    static std::unique_ptr<PlanStage> _indexScan(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        WorkingSet* ws,
        const VariantCollectionPtrOrAcquisition& collection,
        const IndexDescriptor* descriptor,
        const BSONObj& startKey,
        const BSONObj& endKey,
        BoundInclusion boundInclusion,
        Direction direction = FORWARD,
        int options = IXSCAN_DEFAULT);

    /**
     * Returns a plan stage that is either a DeleteStage or a BatchedDeleteStage.
     *
     * Used as a helper when it is necessary to create a delete stage.
     */
    static std::unique_ptr<PlanStage> _createAppropriateDeleteStage(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        CollectionAcquisition coll,
        std::unique_ptr<DeleteStageParams> params,
        std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams,
        WorkingSet* ws,
        PlanStage* child);
};

}  // namespace mongo
