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

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
// IWYU pragma: no_include "boost/move/detail/iterator_to_raw_pointer.hpp"
#include "mongo/base/status_with.h"
#include "mongo/db/exec/classic/batched_delete_stage.h"
#include "mongo/db/exec/classic/collection_scan.h"
#include "mongo/db/exec/classic/fetch.h"
#include "mongo/db/exec/classic/idhack.h"
#include "mongo/db/exec/classic/index_scan.h"
#include "mongo/db/exec/classic/limit.h"
#include "mongo/db/exec/classic/multi_iterator.h"
#include "mongo/db/exec/classic/update_stage.h"
#include "mongo/db/exec/classic/upsert_stage.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
    const CollectionPtr& collection,
    const BSONObj& keyPattern,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    const InternalPlanner::Direction direction) {
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
    CollectionScanParams::ScanBoundInclusion boundInclusion,
    bool shouldReturnEofOnFilterMismatch) {
    const auto& collection = *coll;
    invariant(collection);

    CollectionScanParams params;
    params.shouldWaitForOplogVisibility =
        shouldWaitForOplogVisibility(expCtx->getOperationContext(), collection, false);

    if (resumeAfterRecordId) {
        params.resumeScanPoint =
            ResumeScanPoint{*resumeAfterRecordId, false /* tolerateKeyNotFound */};
    }

    params.minRecord = std::move(minRecord);
    params.maxRecord = std::move(maxRecord);
    if (InternalPlanner::FORWARD == direction) {
        params.direction = CollectionScanParams::FORWARD;
    } else {
        params.direction = CollectionScanParams::BACKWARD;
    }
    params.boundInclusion = boundInclusion;
    params.shouldReturnEofOnFilterMismatch = shouldReturnEofOnFilterMismatch;
    return params;
}
}  // namespace

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::sampleCollection(
    OperationContext* opCtx,
    const CollectionAcquisition& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    boost::optional<int64_t> numSamples) {
    const auto& collectionPtr = collection.getCollectionPtr();
    invariant(collectionPtr);

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(collectionPtr->ns()).build();

    auto rsRandCursor = collectionPtr->getRecordStore()->getRandomCursor(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    std::unique_ptr<PlanStage> root =
        std::make_unique<MultiIteratorStage>(expCtx.get(), ws.get(), collection);
    static_cast<MultiIteratorStage*>(root.get())->addIterator(std::move(rsRandCursor));

    if (numSamples) {
        auto samples = *numSamples;
        invariant(samples >= 0,
                  "Number of samples must be >= 0, otherwise LimitStage it will never end");
        root = std::make_unique<LimitStage>(expCtx.get(), samples, ws.get(), std::move(root));
    }

    auto statusWithPlanExecutor = plan_executor_factory::make(
        expCtx, std::move(ws), std::move(root), collection, yieldPolicy, false);

    invariant(statusWithPlanExecutor.getStatus());
    return std::move(statusWithPlanExecutor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::collectionScan(
    OperationContext* opCtx,
    const CollectionAcquisition& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const Direction direction,
    const boost::optional<RecordId>& resumeAfterRecordId,
    boost::optional<RecordIdBound> minRecord,
    boost::optional<RecordIdBound> maxRecord,
    CollectionScanParams::ScanBoundInclusion boundInclusion,
    bool shouldReturnEofOnFilterMismatch) {
    return collectionScan({opCtx,
                           collection,
                           yieldPolicy,
                           direction,
                           resumeAfterRecordId,
                           minRecord,
                           maxRecord,
                           boundInclusion,
                           shouldReturnEofOnFilterMismatch});
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::collectionScan(
    CreateCollectionScanParams&& params) {
    const auto& collectionPtr = params.collection.getCollectionPtr();
    invariant(collectionPtr);

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

    auto expCtx = ExpressionContextBuilder{}.opCtx(params.opCtx).ns(collectionPtr->ns()).build();
    auto collScanParams = createCollectionScanParams(expCtx,
                                                     ws.get(),
                                                     &collectionPtr,
                                                     params.direction,
                                                     params.resumeAfterRecordId,
                                                     std::move(params.minRecord),
                                                     std::move(params.maxRecord),
                                                     params.boundInclusion,
                                                     params.shouldReturnEofOnFilterMismatch);

    auto cs = _collectionScan(expCtx, ws.get(), params.collection, collScanParams);

    // Takes ownership of 'ws' and 'cs'.
    auto statusWithPlanExecutor = plan_executor_factory::make(expCtx,
                                                              std::move(ws),
                                                              std::move(cs),
                                                              params.collection,
                                                              params.yieldPolicy,
                                                              params.plannerOptions);
    invariant(statusWithPlanExecutor.getStatus());
    return std::move(statusWithPlanExecutor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::collectionScan(
    OperationContext* opCtx,
    const CollectionAcquisition& coll,
    const CollectionScanParams& params,
    PlanYieldPolicy::YieldPolicy yieldPolicy) {
    tassert(10415300, "InternalPlanner::collectionScan expects collection to exist", coll.exists());

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(coll.nss()).build();
    auto cs = _collectionScan(expCtx, ws.get(), coll, params);

    // Takes ownership of 'ws' and 'cs'.
    auto statusWithPlanExecutor =
        plan_executor_factory::make(expCtx,
                                    std::move(ws),
                                    std::move(cs),
                                    coll,
                                    yieldPolicy,
                                    false /* whether owned BSON must be returned */);
    invariant(statusWithPlanExecutor.getStatus());
    return std::move(statusWithPlanExecutor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithCollectionScan(
    OperationContext* opCtx,
    CollectionAcquisition coll,
    std::unique_ptr<DeleteStageParams> params,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction,
    boost::optional<RecordIdBound> minRecord,
    boost::optional<RecordIdBound> maxRecord,
    CollectionScanParams::ScanBoundInclusion boundInclusion,
    std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams,
    const MatchExpression* filter,
    bool shouldReturnEofOnFilterMismatch) {
    const auto& collectionPtr = coll.getCollectionPtr();
    invariant(collectionPtr);
    if (shouldReturnEofOnFilterMismatch) {
        tassert(7010801,
                "MatchExpression filter must be provided when 'shouldReturnEofOnFilterMismatch' is "
                "set to true ",
                filter);
    }

    auto ws = std::make_unique<WorkingSet>();
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(collectionPtr->ns()).build();

    if (collectionPtr->isCapped()) {
        expCtx->setIsCappedDelete();
    }

    auto collScanParams = createCollectionScanParams(expCtx,
                                                     ws.get(),
                                                     &collectionPtr,
                                                     direction,
                                                     boost::none /* resumeAfterId */,
                                                     std::move(minRecord),
                                                     std::move(maxRecord),
                                                     boundInclusion,
                                                     shouldReturnEofOnFilterMismatch);

    auto root = _collectionScan(expCtx, ws.get(), coll, collScanParams, filter);

    root = _createAppropriateDeleteStage(
        expCtx, coll, std::move(params), std::move(batchedDeleteParams), ws.get(), root.release());


    auto executor = plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                coll,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */
    );
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::indexScan(
    OperationContext* opCtx,
    const CollectionAcquisition& coll,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction,
    int options) {
    auto ws = std::make_unique<WorkingSet>();
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(coll.nss()).build();

    std::unique_ptr<PlanStage> root = _indexScan(
        expCtx, ws.get(), coll, descriptor, startKey, endKey, boundInclusion, direction, options);

    auto executor = plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                coll,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */
    );
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithIndexScan(
    OperationContext* opCtx,
    CollectionAcquisition coll,
    std::unique_ptr<DeleteStageParams> params,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction,
    std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams) {
    const auto& collectionPtr = coll.getCollectionPtr();
    invariant(collectionPtr);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(collectionPtr->ns()).build();

    std::unique_ptr<PlanStage> root = _indexScan(expCtx,
                                                 ws.get(),
                                                 coll,
                                                 descriptor,
                                                 startKey,
                                                 endKey,
                                                 boundInclusion,
                                                 direction,
                                                 InternalPlanner::IXSCAN_FETCH);

    root = _createAppropriateDeleteStage(
        expCtx, coll, std::move(params), std::move(batchedDeleteParams), ws.get(), root.release());


    auto executor = plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                coll,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */
    );
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::shardKeyIndexScan(
    OperationContext* opCtx,
    const CollectionAcquisition& collection,
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
    auto params = convertIndexScanParamsToCollScanParams(opCtx,
                                                         collection.getCollectionPtr(),
                                                         shardKeyIdx.keyPattern(),
                                                         startKey,
                                                         endKey,
                                                         boundInclusion,
                                                         direction);
    return collectionScan(opCtx, collection, params, yieldPolicy);
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithShardKeyIndexScan(
    OperationContext* opCtx,
    CollectionAcquisition coll,
    std::unique_ptr<DeleteStageParams> params,
    const ShardKeyIndex& shardKeyIdx,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams,
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
                                   direction,
                                   std::move(batchedDeleteParams));
    }
    auto collectionScanParams = convertIndexScanParamsToCollScanParams(opCtx,
                                                                       coll.getCollectionPtr(),
                                                                       shardKeyIdx.keyPattern(),
                                                                       startKey,
                                                                       endKey,
                                                                       boundInclusion,
                                                                       direction);

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(coll.nss()).build();

    auto root = _collectionScan(expCtx, ws.get(), coll, collectionScanParams);

    root = _createAppropriateDeleteStage(
        expCtx, coll, std::move(params), std::move(batchedDeleteParams), ws.get(), root.release());

    auto executor = plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                coll,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */
    );
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::updateWithIdHack(
    OperationContext* opCtx,
    CollectionAcquisition collection,
    const UpdateStageParams& params,
    const IndexDescriptor* descriptor,
    const BSONObj& key,
    PlanYieldPolicy::YieldPolicy yieldPolicy) {
    const auto& collectionPtr = collection.getCollectionPtr();
    invariant(collectionPtr);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(collectionPtr->ns()).build();

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
                                                collection,
                                                yieldPolicy,
                                                false /* whether owned BSON must be returned */);

    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanStage> InternalPlanner::_collectionScan(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WorkingSet* ws,
    const CollectionAcquisition& coll,
    const CollectionScanParams& params,
    const MatchExpression* filter) {

    tassert(10397702, "Collection pointer must be initialized", coll.getCollectionPtr());

    return std::make_unique<CollectionScan>(expCtx.get(), coll, params, ws, filter);
}

std::unique_ptr<PlanStage> InternalPlanner::_indexScan(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WorkingSet* ws,
    const CollectionAcquisition& coll,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    Direction direction,
    int options) {
    tassert(10415301, "InternalPlanner::_indexScan expected collection to exist", coll.exists());
    const auto& collectionPtr = coll.getCollectionPtr();
    invariant(descriptor);

    IndexScanParams params(expCtx->getOperationContext(), collectionPtr, descriptor);
    params.direction = direction;
    params.bounds.isSimpleRange = true;
    params.bounds.startKey = startKey;
    params.bounds.endKey = endKey;
    params.bounds.boundInclusion = boundInclusion;
    params.shouldDedup =
        descriptor->getEntry()->isMultikey(expCtx->getOperationContext(), collectionPtr);

    std::unique_ptr<PlanStage> root =
        std::make_unique<IndexScan>(expCtx.get(), coll, std::move(params), ws, nullptr);

    if (InternalPlanner::IXSCAN_FETCH & options) {
        root = std::make_unique<FetchStage>(expCtx.get(), ws, std::move(root), nullptr, coll);
    }

    return root;
}

std::unique_ptr<PlanStage> InternalPlanner::_createAppropriateDeleteStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    CollectionAcquisition coll,
    std::unique_ptr<DeleteStageParams> params,
    std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams,
    WorkingSet* ws,
    PlanStage* child) {
    if (batchedDeleteParams) {
        return std::make_unique<BatchedDeleteStage>(
            expCtx.get(), std::move(params), std::move(batchedDeleteParams), ws, coll, child);
    } else {
        return std::make_unique<DeleteStage>(expCtx.get(), std::move(params), ws, coll, child);
    }
}

}  // namespace mongo
