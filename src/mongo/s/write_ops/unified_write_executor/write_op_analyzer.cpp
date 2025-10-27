/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"

#include "mongo/db/raw_data_operation.h"
#include "mongo/s/write_ops/coordinate_multi_update_util.h"
#include "mongo/s/write_ops/write_op_helper.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace unified_write_executor {


void WriteOpAnalyzerImpl::recordTargetingStats(OperationContext* opCtx,
                                               const CollectionRoutingInfoTargeter& targeter,
                                               const NSTargeter::TargetingResult& tr,
                                               const WriteOp& op) {
    // FindAndModify command does not record the following metrics.
    if (op.isFindAndModify()) {
        return;
    }

    size_t nsIdx = BulkWriteCRUDOp(op.getBulkWriteOp()).getNsInfoIdx();
    _stats.recordTargetingStats(tr.endpoints,
                                nsIdx,
                                targeter.isTargetedCollectionSharded(),
                                targeter.getAproxNShardsOwningChunks(),
                                op.getType());
}

StatusWith<Analysis> WriteOpAnalyzerImpl::analyze(OperationContext* opCtx,
                                                  RoutingContext& routingCtx,
                                                  const WriteOp& op) try {
    const auto& cri = routingCtx.getCollectionRoutingInfo(op.getNss());
    // TODO SERVER-103782 Don't use CRITargeter.
    CollectionRoutingInfoTargeter targeter(op.getNss(), cri);
    // TODO SERVER-103781 Add support for kPartialKeyWithId.
    // TODO SERVER-103146 Add kChangesOwnership.
    // TODO SERVER-103781 Add support for "WriteWithoutShardKeyWithId" writes.
    NSTargeter::TargetingResult tr;
    switch (op.getType()) {
        case WriteType::kInsert: {
            tr.endpoints.emplace_back(
                targeter.targetInsert(opCtx, op.getItemRef().getInsertOp().getDocument()));
        } break;
        case WriteType::kUpdate: {
            tr = targeter.targetUpdate(opCtx, op.getItemRef());
        } break;
        case WriteType::kDelete: {
            tr = targeter.targetDelete(opCtx, op.getItemRef());
        } break;
        default: {
            MONGO_UNREACHABLE;
        } break;
    }

    tassert(10346500, "Expected write to affect at least one shard", !tr.endpoints.empty());
    const auto& cm = cri.getChunkManager();
    const bool isShardedTimeseries = targeter.isTrackedTimeSeriesNamespace();
    const bool isUpdate = op.getType() == WriteType::kUpdate;
    const bool isRetryableWrite = opCtx->isRetryableWrite();
    const bool inTxn = static_cast<bool>(TransactionRouter::get(opCtx));
    const bool isRawData = isRawDataOperation(opCtx);
    const bool isTimeseriesRetryableUpdateOp =
        isShardedTimeseries && isUpdate && isRetryableWrite && !inTxn && !isRawData;

    const bool enableMultiWriteBlockingMigrations =
        coordinate_multi_update_util::shouldCoordinateMultiWrite(
            opCtx, _pauseMigrationsDuringMultiUpdatesParameter);
    const bool isMultiWrite = op.isMulti();
    const bool isDelete = op.getType() == WriteType::kDelete;
    const bool isMultiWriteBlockingMigrations =
        (isUpdate || isDelete) && isMultiWrite && enableMultiWriteBlockingMigrations;

    auto targetedSampleId = analyze_shard_key::tryGenerateTargetedSampleId(
        opCtx, targeter.getNS(), op.getItemRef().getOpType(), tr.endpoints);

    if (tr.useTwoPhaseWriteProtocol || tr.isNonTargetedRetryableWriteWithId) {
        recordTargetingStats(opCtx, targeter, tr, op);
        return Analysis{
            BatchType::kNonTargetedWrite, std::move(tr.endpoints), std::move(targetedSampleId)};
    } else if (isMultiWriteBlockingMigrations) {
        return Analysis{BatchType::kMultiWriteBlockingMigrations,
                        std::move(tr.endpoints),
                        std::move(targetedSampleId)};
    } else if (isTimeseriesRetryableUpdateOp) {
        // Special case for time series since an update could affect two documents in the underlying
        // buckets collection.
        tassert(10413901,
                "Unified Write Executor does not support viewful timeseries collections",
                cm.isNewTimeseriesWithoutView());
        // Targetting code in this path can only handle writes with the full shardKey in the query.
        tassert(10413902,
                "Writes without shard key must go through non-targeted path",
                !(tr.useTwoPhaseWriteProtocol || tr.isNonTargetedRetryableWriteWithId));
        return Analysis{
            BatchType::kInternalTransaction, std::move(tr.endpoints), std::move(targetedSampleId)};
    } else if (tr.endpoints.size() == 1) {
        recordTargetingStats(opCtx, targeter, tr, op);
        return Analysis{
            BatchType::kSingleShard, std::move(tr.endpoints), std::move(targetedSampleId)};
    } else {
        recordTargetingStats(opCtx, targeter, tr, op);
        // For updates/upserts/deletes running outside of a transaction that need to target more
        // than one endpoint, all shards are targeted -AND- 'shardVersion' is set to IGNORED on all
        // endpoints. The exception to this is when 'onlyTargetDataOwningShardsForMultiWrites' is
        // true.
        const bool targetAllShards = (isUpdate || isDelete) &&
            write_op_helpers::shouldTargetAllShardsSVIgnored(inTxn, op.isMulti());
        if (targetAllShards) {
            auto endpoints = targeter.targetAllShards(opCtx);

            for (auto& endpoint : endpoints) {
                endpoint.shardVersion->setPlacementVersionIgnored();
            }
            tr.endpoints = std::move(endpoints);

            // Regenerate the targetedSampleId since we changed the endpoints to target all shards.
            targetedSampleId = analyze_shard_key::tryGenerateTargetedSampleId(
                opCtx, targeter.getNS(), op.getItemRef().getOpType(), tr.endpoints);
        }

        return Analysis{
            BatchType::kMultiShard, std::move(tr.endpoints), std::move(targetedSampleId)};
    }
} catch (const DBException& ex) {
    auto status = ex.toStatus();

    LOGV2_DEBUG(10896516, 4, "Encountered targeting error", "error"_attr = redact(status));

    return status;
}

}  // namespace unified_write_executor
}  // namespace mongo
