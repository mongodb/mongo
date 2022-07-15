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

#include "mongo/platform/basic.h"

#include "mongo/db/query/internal_plans.h"

#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/upsert_stage.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/record_id_helpers.h"

namespace mongo {

namespace {
CollectionScanParams::ScanBoundInclusion getScanBoundInclusion(BoundInclusion indexBoundInclusion) {
    switch (indexBoundInclusion) {
        case BoundInclusion::kExcludeBothStartAndEndKeys:
            return CollectionScanParams::ScanBoundInclusion::kExcludeBothStartAndEndRecords;
        case BoundInclusion::kIncludeStartKeyOnly:
            return CollectionScanParams::ScanBoundInclusion::kIncludeStartRecordOnly;
        case BoundInclusion::kIncludeEndKeyOnly:
            return CollectionScanParams::ScanBoundInclusion::kIncludeEndRecordOnly;
        case BoundInclusion::kIncludeBothStartAndEndKeys:
            return CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords;
        default:
            MONGO_UNREACHABLE;
    }
}

// Construct collection scan params for a scan over the collection's cluster key. Callers must
// confirm the collection's cluster key matches the keyPattern.
CollectionScanParams convertIndexScanParamsToCollScanParams(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    const BSONObj& keyPattern,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    const InternalPlanner::Direction direction) {
    const auto& collection = *coll;

    dassert(collection->isClustered() &&
            clustered_util::matchesClusterKey(keyPattern, collection->getClusteredInfo()));
    invariant(collection->getDefaultCollator() == nullptr);

    boost::optional<RecordIdBound> startRecord, endRecord;
    if (!startKey.isEmpty()) {
        startRecord = RecordIdBound(record_id_helpers::keyForElem(startKey.firstElement()));
    }
    if (!endKey.isEmpty()) {
        endRecord = RecordIdBound(record_id_helpers::keyForElem(endKey.firstElement()));
    }

    // For a forward scan, the startKey is the minRecord. For a backward scan, it is the maxRecord.
    auto minRecord = (direction == InternalPlanner::FORWARD) ? startRecord : endRecord;
    auto maxRecord = (direction == InternalPlanner::FORWARD) ? endRecord : startRecord;

    if (minRecord && maxRecord) {
        // Regardless of direction, the minRecord should always be less than the maxRecord
        dassert(minRecord->recordId() < maxRecord->recordId(),
                str::stream() << "Expected the minRecord " << minRecord
                              << " to be less than the maxRecord " << maxRecord
                              << " on a bounded collection scan. Original startKey and endKey for "
                                 "index scan ["
                              << startKey << ", " << endKey << "]. Is FORWARD? "
                              << (direction == InternalPlanner::FORWARD));
    }

    CollectionScanParams params;
    params.minRecord = minRecord;
    params.maxRecord = maxRecord;

    if (InternalPlanner::FORWARD == direction) {
        params.direction = CollectionScanParams::FORWARD;
    } else {
        params.direction = CollectionScanParams::BACKWARD;
    }
    params.boundInclusion = getScanBoundInclusion(boundInclusion);
    return params;
}

CollectionScanParams createCollectionScanParams(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WorkingSet* ws,
    const CollectionPtr* coll,
    InternalPlanner::Direction direction,
    const boost::optional<RecordId>& resumeAfterRecordId,
    boost::optional<RecordIdBound> minRecord,
    boost::optional<RecordIdBound> maxRecord,
    CollectionScanParams::ScanBoundInclusion boundInclusion) {
    const auto& collection = *coll;
    invariant(collection);

    CollectionScanParams params;
    params.shouldWaitForOplogVisibility =
        shouldWaitForOplogVisibility(expCtx->opCtx, collection, false);
    params.resumeAfterRecordId = resumeAfterRecordId;
    params.minRecord = minRecord;
    params.maxRecord = maxRecord;
    if (InternalPlanner::FORWARD == direction) {
        params.direction = CollectionScanParams::FORWARD;
    } else {
        params.direction = CollectionScanParams::BACKWARD;
    }
    params.boundInclusion = boundInclusion;
    return params;
}
}  // namespace

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::collectionScan(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const Direction direction,
    const boost::optional<RecordId>& resumeAfterRecordId,
    boost::optional<RecordIdBound> minRecord,
    boost::optional<RecordIdBound> maxRecord,
    CollectionScanParams::ScanBoundInclusion boundInclusion) {
    const auto& collection = *coll;
    invariant(collection);

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    auto collScanParams = createCollectionScanParams(expCtx,
                                                     ws.get(),
                                                     coll,
                                                     direction,
                                                     resumeAfterRecordId,
                                                     minRecord,
                                                     maxRecord,
                                                     boundInclusion);

    auto cs = _collectionScan(expCtx, ws.get(), &collection, collScanParams);

    // Takes ownership of 'ws' and 'cs'.
    auto statusWithPlanExecutor =
        plan_executor_factory::make(expCtx,
                                    std::move(ws),
                                    std::move(cs),
                                    &collection,
                                    yieldPolicy,
                                    false /* whether owned BSON must be returned */);
    invariant(statusWithPlanExecutor.isOK());
    return std::move(statusWithPlanExecutor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::collectionScan(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    const CollectionScanParams& params,
    PlanYieldPolicy::YieldPolicy yieldPolicy) {
    const auto& collection = *coll;
    invariant(collection);

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());
    auto cs = _collectionScan(expCtx, ws.get(), &collection, params);

    // Takes ownership of 'ws' and 'cs'.
    auto statusWithPlanExecutor =
        plan_executor_factory::make(expCtx,
                                    std::move(ws),
                                    std::move(cs),
                                    &collection,
                                    yieldPolicy,
                                    false /* whether owned BSON must be returned */);
    invariant(statusWithPlanExecutor.isOK());
    return std::move(statusWithPlanExecutor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithCollectionScan(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    std::unique_ptr<DeleteStageParams> params,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction,
    boost::optional<RecordIdBound> minRecord,
    boost::optional<RecordIdBound> maxRecord,
    CollectionScanParams::ScanBoundInclusion boundInclusion,
    std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams) {
    const auto& collection = *coll;
    invariant(collection);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    if (collection->isCapped()) {
        expCtx->setIsCappedDelete();
    }

    auto collScanParams = createCollectionScanParams(expCtx,
                                                     ws.get(),
                                                     coll,
                                                     direction,
                                                     boost::none /* resumeAfterId */,
                                                     minRecord,
                                                     maxRecord,
                                                     boundInclusion);

    auto root = _collectionScan(expCtx, ws.get(), &collection, collScanParams);

    if (batchedDeleteParams) {
        root = std::make_unique<BatchedDeleteStage>(expCtx.get(),
                                                    std::move(params),
                                                    std::move(batchedDeleteParams),
                                                    ws.get(),
                                                    collection,
                                                    root.release());
    } else {
        root = std::make_unique<DeleteStage>(
            expCtx.get(), std::move(params), ws.get(), collection, root.release());
    }

    auto executor = plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                &collection,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */
    );
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::indexScan(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction,
    int options) {
    const auto& collection = *coll;
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    std::unique_ptr<PlanStage> root = _indexScan(expCtx,
                                                 ws.get(),
                                                 &collection,
                                                 descriptor,
                                                 startKey,
                                                 endKey,
                                                 boundInclusion,
                                                 direction,
                                                 options);

    auto executor = plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                &collection,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */
    );
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithIndexScan(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    std::unique_ptr<DeleteStageParams> params,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction,
    std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams) {
    const auto& collection = *coll;
    invariant(collection);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    std::unique_ptr<PlanStage> root = _indexScan(expCtx,
                                                 ws.get(),
                                                 &collection,
                                                 descriptor,
                                                 startKey,
                                                 endKey,
                                                 boundInclusion,
                                                 direction,
                                                 InternalPlanner::IXSCAN_FETCH);

    if (batchedDeleteParams) {
        root = std::make_unique<BatchedDeleteStage>(expCtx.get(),
                                                    std::move(params),
                                                    std::move(batchedDeleteParams),
                                                    ws.get(),
                                                    collection,
                                                    root.release());
    } else {
        root = std::make_unique<DeleteStage>(
            expCtx.get(), std::move(params), ws.get(), collection, root.release());
    }

    auto executor = plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                &collection,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */
    );
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::shardKeyIndexScan(
    OperationContext* opCtx,
    const CollectionPtr* collection,
    const ShardKeyIndex& shardKeyIdx,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction,
    int options) {
    if (shardKeyIdx.descriptor() != nullptr) {
        return indexScan(opCtx,
                         collection,
                         shardKeyIdx.descriptor(),
                         startKey,
                         endKey,
                         boundInclusion,
                         yieldPolicy,
                         direction,
                         options);
    }
    // Do a clustered collection scan.
    auto params = convertIndexScanParamsToCollScanParams(
        opCtx, collection, shardKeyIdx.keyPattern(), startKey, endKey, boundInclusion, direction);
    return collectionScan(opCtx, collection, params, yieldPolicy);
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithShardKeyIndexScan(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    std::unique_ptr<DeleteStageParams> params,
    const ShardKeyIndex& shardKeyIdx,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction) {
    if (shardKeyIdx.descriptor()) {
        return deleteWithIndexScan(opCtx,
                                   coll,
                                   std::move(params),
                                   shardKeyIdx.descriptor(),
                                   startKey,
                                   endKey,
                                   boundInclusion,
                                   yieldPolicy,
                                   direction);
    }
    auto collectionScanParams = convertIndexScanParamsToCollScanParams(
        opCtx, coll, shardKeyIdx.keyPattern(), startKey, endKey, boundInclusion, direction);

    const auto& collection = *coll;
    invariant(collection);

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    auto root = _collectionScan(expCtx, ws.get(), &collection, collectionScanParams);
    root = std::make_unique<DeleteStage>(
        expCtx.get(), std::move(params), ws.get(), collection, root.release());

    auto executor = plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                &collection,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */
    );
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::updateWithIdHack(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    const UpdateStageParams& params,
    const IndexDescriptor* descriptor,
    const BSONObj& key,
    PlanYieldPolicy::YieldPolicy yieldPolicy) {
    const auto& collection = *coll;
    invariant(collection);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    auto idHackStage =
        std::make_unique<IDHackStage>(expCtx.get(), key, ws.get(), collection, descriptor);

    const bool isUpsert = params.request->isUpsert();
    auto root = (isUpsert ? std::make_unique<UpsertStage>(
                                expCtx.get(), params, ws.get(), collection, idHackStage.release())
                          : std::make_unique<UpdateStage>(
                                expCtx.get(), params, ws.get(), collection, idHackStage.release()));

    auto executor = plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                &collection,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */
    );
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanStage> InternalPlanner::_collectionScan(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WorkingSet* ws,
    const CollectionPtr* coll,
    const CollectionScanParams& params) {

    const auto& collection = *coll;
    invariant(collection);

    return std::make_unique<CollectionScan>(expCtx.get(), collection, params, ws, nullptr);
}

std::unique_ptr<PlanStage> InternalPlanner::_indexScan(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WorkingSet* ws,
    const CollectionPtr* coll,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    Direction direction,
    int options) {
    const auto& collection = *coll;
    invariant(collection);
    invariant(descriptor);

    IndexScanParams params(expCtx->opCtx, collection, descriptor);
    params.direction = direction;
    params.bounds.isSimpleRange = true;
    params.bounds.startKey = startKey;
    params.bounds.endKey = endKey;
    params.bounds.boundInclusion = boundInclusion;
    params.shouldDedup = descriptor->getEntry()->isMultikey(expCtx->opCtx, collection);

    std::unique_ptr<PlanStage> root =
        std::make_unique<IndexScan>(expCtx.get(), collection, std::move(params), ws, nullptr);

    if (InternalPlanner::IXSCAN_FETCH & options) {
        root = std::make_unique<FetchStage>(expCtx.get(), ws, std::move(root), nullptr, collection);
    }

    return root;
}

}  // namespace mongo
