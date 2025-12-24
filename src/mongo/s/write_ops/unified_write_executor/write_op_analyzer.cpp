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

#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
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

    _stats.recordTargetingStats(tr.endpoints,
                                op.getNsInfoIdx(),
                                targeter.isTargetedCollectionSharded(),
                                targeter.getAproxNShardsOwningChunks(),
                                getWriteOpType(op));
}

StatusWith<Analysis> WriteOpAnalyzerImpl::analyze(OperationContext* opCtx,
                                                  RoutingContext& routingCtx,
                                                  const WriteOp& op) try {
    // TODO SERVER-106874 remove the namespace translation check in this function entirely once 9.0
    // becomes last LTS. By then we will only have viewless timeseries that do not require nss
    // translation.
    // TODO SERVER-103782 Don't use CRITargeter.
    auto nss = op.getNss();
    CollectionRoutingInfoTargeter targeter(nss, routingCtx);
    // TODO SERVER-103146 Add kChangesOwnership.
    NSTargeter::TargetingResult tr;
    switch (getWriteOpType(op)) {
        case WriteType::kInsert: {
            tr.endpoints.emplace_back(targeter.targetInsert(opCtx, op.getInsertOp().getDocument()));
        } break;
        case WriteType::kUpdate: {
            tr = targeter.targetUpdate(opCtx, op);
        } break;
        case WriteType::kDelete: {
            tr = targeter.targetDelete(opCtx, op);
        } break;
        default: {
            MONGO_UNREACHABLE;
        } break;
    }

    tassert(10346500, "Expected write to affect at least one shard", !tr.endpoints.empty());
    const bool isTimeseries = targeter.isTrackedTimeSeriesNamespace();
    const bool isUpdate = getWriteOpType(op) == WriteType::kUpdate;
    const bool isRetryableWrite = opCtx->isRetryableWrite();
    const bool inTxn = static_cast<bool>(TransactionRouter::get(opCtx));
    const bool isRawData = isRawDataOperation(opCtx);
    const bool isTimeseriesRetryableUpdateOp =
        isTimeseries && isUpdate && isRetryableWrite && !inTxn && !isRawData;
    // We consider the request to be on the main namespace of a viewful timeseries collection when
    // the underlying CRI is a non-viewless timeseries entry and the client request is on the main
    // namespace not the buckets namespace.
    auto isRequestOnTimeseriesBucketCollection = nss.isTimeseriesBucketsCollection();
    const bool isViewfulTimeseries =
        targeter.isTrackedTimeSeriesBucketsNamespace() && !isRequestOnTimeseriesBucketCollection;

    const bool enableMultiWriteBlockingMigrations =
        coordinate_multi_update_util::shouldCoordinateMultiWrite(
            opCtx, _pauseMigrationsDuringMultiUpdatesParameter);
    const bool isMultiWrite = op.getMulti();
    const bool isDelete = getWriteOpType(op) == WriteType::kDelete;
    const bool isMultiWriteBlockingMigrations =
        (isUpdate || isDelete) && isMultiWrite && enableMultiWriteBlockingMigrations;

    if (isTimeseries && op.isFindAndModify()) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform findAndModify with sort on a timeseries collection",
                !op.getSort() || isRawDataOperation(opCtx));
    }

    auto targetedSampleId = analyze_shard_key::tryGenerateTargetedSampleId(
        opCtx, targeter.getNS(), op.getOpType(), tr.endpoints);

    if (tr.isNonTargetedRetryableWriteWithId) {
        recordTargetingStats(opCtx, targeter, tr, op);
        return Analysis{AnalysisType::kRetryableWriteWithId,
                        std::move(tr.endpoints),
                        isViewfulTimeseries,
                        std::move(targetedSampleId)};
    } else if (tr.useTwoPhaseWriteProtocol) {
        recordTargetingStats(opCtx, targeter, tr, op);
        return Analysis{AnalysisType::kTwoPhaseWrite,
                        std::move(tr.endpoints),
                        isViewfulTimeseries,
                        std::move(targetedSampleId)};
    } else if (isMultiWriteBlockingMigrations) {
        return Analysis{AnalysisType::kMultiWriteBlockingMigrations,
                        std::move(tr.endpoints),
                        isViewfulTimeseries,
                        std::move(targetedSampleId)};
    } else if (isTimeseriesRetryableUpdateOp) {
        // Special case for time series since an update could affect two documents in the underlying
        // buckets collection.
        // Targetting code in this path can only handle writes with the full shardKey in the query.
        tassert(10413902,
                "Writes without shard key must go through non-targeted path",
                !(tr.useTwoPhaseWriteProtocol || tr.isNonTargetedRetryableWriteWithId));
        // Note we do not translate viewful timeseries collection namespace here, it will be
        // translated within the transaction when we analyze the request again.
        // Note also that we do not record any targeting stats here as we will do so when we analyze
        // the request a second time.
        return Analysis{AnalysisType::kInternalTransaction,
                        std::move(tr.endpoints),
                        false /* isTimeseries */,
                        std::move(targetedSampleId)};
    } else if (tr.endpoints.size() == 1) {
        recordTargetingStats(opCtx, targeter, tr, op);
        return Analysis{AnalysisType::kSingleShard,
                        std::move(tr.endpoints),
                        isViewfulTimeseries,
                        std::move(targetedSampleId)};
    } else {
        // For updates/upserts/deletes running outside of a transaction that need to target more
        // than one endpoint, all shards are targeted -AND- 'shardVersion' is set to IGNORED on all
        // endpoints. The exception to this is when 'onlyTargetDataOwningShardsForMultiWrites' is
        // true.
        const bool targetAllShards = (isUpdate || isDelete) &&
            write_op_helpers::shouldTargetAllShardsSVIgnored(inTxn, op.getMulti());
        if (targetAllShards) {
            auto endpoints = targeter.targetAllShards(opCtx);

            for (auto& endpoint : endpoints) {
                endpoint.shardVersion->setPlacementVersionIgnored();
            }
            tr.endpoints = std::move(endpoints);

            // Regenerate the targetedSampleId since we changed the endpoints to target all shards.
            targetedSampleId = analyze_shard_key::tryGenerateTargetedSampleId(
                opCtx, targeter.getNS(), op.getOpType(), tr.endpoints);
        }

        recordTargetingStats(opCtx, targeter, tr, op);

        return Analysis{AnalysisType::kMultiShard,
                        std::move(tr.endpoints),
                        isViewfulTimeseries,
                        std::move(targetedSampleId)};
    }
} catch (const DBException& ex) {
    auto status = ex.toStatus();

    LOGV2_DEBUG(10896516, 4, "Encountered targeting error", "error"_attr = redact(status));

    return status;
}

}  // namespace unified_write_executor
}  // namespace mongo
