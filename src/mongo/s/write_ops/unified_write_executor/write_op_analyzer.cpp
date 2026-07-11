// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"

#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/s/write_ops/coordinate_multi_update_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace unified_write_executor {

void WriteOpAnalyzerImpl::recordTargetingStats(OperationContext* opCtx,
                                               const CollectionRoutingInfo& cri,
                                               const TargetOpResult& tr,
                                               const WriteOp& op) {
    // FindAndModify command does not record the following metrics.
    if (op.isFindAndModify()) {
        return;
    }

    int aproxNShardsOwningChunks =
        cri.hasRoutingTable() ? cri.getChunkManager().getAproxNShardsOwningChunks() : 0;

    _stats.recordTargetingStats(tr.endpoints,
                                op.getNsInfoIdx(),
                                cri.isSharded(),
                                aproxNShardsOwningChunks,
                                getWriteOpType(op));
}

StatusWith<Analysis> WriteOpAnalyzerImpl::analyze(OperationContext* opCtx,
                                                  RoutingContext& routingCtx,
                                                  const WriteOp& op) try {
    auto nss = op.getNss();
    bool isViewfulTimeseries = false;
    bool usesSVIgnored = false;

    // TODO SERVER-106874 remove the namespace translation check entirely once 9.0 becomes last
    // LTS. By then we will only have viewless timeseries that do not require nss translation.
    auto [it, inserted] = _timeseriesBucketsNSSCache.try_emplace(nss);
    if (inserted) {
        it->second = nss.makeTimeseriesBucketsNamespace();
    }
    const NamespaceString& bucketsNss = it->second;

    if (routingCtx.hasNss(bucketsNss)) {
        nss = bucketsNss;
        isViewfulTimeseries = true;
    }

    const CollectionRoutingInfo& cri = routingCtx.getCollectionRoutingInfo(nss);
    const bool isSharded = cri.isSharded();
    const bool isTimeseriesCollection =
        cri.hasRoutingTable() && cri.getChunkManager().isTimeseriesCollection();
    invariant(!cri.hasRoutingTable() || cri.getChunkManager().getNss() == nss);

    // TODO SERVER-103146 Add kChangesOwnership.
    TargetOpResult tr;
    switch (getWriteOpType(op)) {
        case WriteType::kInsert: {
            tr.endpoints.emplace_back(
                targetInsert(opCtx, nss, cri, isViewfulTimeseries, op.getInsertOp().getDocument()));
        } break;
        case WriteType::kUpdate: {
            tr = targetUpdate(opCtx, nss, cri, isViewfulTimeseries, op);
        } break;
        case WriteType::kDelete: {
            tr = targetDelete(opCtx, nss, cri, isViewfulTimeseries, op);
        } break;
        default: {
            MONGO_UNREACHABLE;
        } break;
    }

    tassert(10346500, "Expected write to affect at least one shard", !tr.endpoints.empty());
    const bool isUpdate = getWriteOpType(op) == WriteType::kUpdate;
    const bool isDelete = getWriteOpType(op) == WriteType::kDelete;
    const bool isMultiWrite = op.getMulti();
    const bool inTxn = static_cast<bool>(TransactionRouter::get(opCtx));
    const bool isRawData = isRawDataOperation(opCtx);
    const bool isTimeseriesRetryableUpdateOp =
        isTimeseriesCollection && isUpdate && opCtx->isRetryableWrite() && !inTxn && !isRawData;

    const bool enableMultiWriteBlockingMigrations =
        coordinate_multi_update_util::shouldCoordinateMultiWrite(
            opCtx, _pauseMigrationsDuringMultiUpdatesParameter);

    const bool isTargetDataOwningShardsOnlyMultiWrite =
        (isMultiWrite && !enableMultiWriteBlockingMigrations &&
         write_op_helpers::isOnlyTargetDataOwningShardsForMultiWritesEnabled());

    if (isTimeseriesCollection && isSharded && op.isFindAndModify()) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform findAndModify with sort on a sharded timeseries collection",
                !op.getSort() || isRawData);
    }

    AnalysisType type = [&] {
        if (isUpdate || isDelete) {
            if (!isMultiWrite && tr.isNonTargetedRetryableWriteWithId) {
                return AnalysisType::kRetryableWriteWithId;
            }
            if (!isMultiWrite && tr.useTwoPhaseWriteProtocol) {
                return AnalysisType::kTwoPhaseWrite;
            }
            if (isMultiWrite && enableMultiWriteBlockingMigrations) {
                return AnalysisType::kMultiWriteBlockingMigrations;
            }
            if (isTimeseriesRetryableUpdateOp) {
                return AnalysisType::kInternalTransaction;
            }
        }
        return tr.endpoints.size() > 1 ? AnalysisType::kMultiShard : AnalysisType::kSingleShard;
    }();

    if (isSharded && !inTxn) {
        if (type == AnalysisType::kRetryableWriteWithId) {
            // For kRetryableWriteWithId batches, we need to target all shards.
            //
            // TODO SERVER-101167: For kRetryableWriteWithId batches, we should only target the
            // shards that are needed (instead of targeting all shards).
            tr.endpoints = targetAllShards(opCtx, cri);
        }

        if (type == AnalysisType::kMultiShard && !isTargetDataOwningShardsOnlyMultiWrite) {
            // For kMultiShard batches, if 'isTargetDataOwningShardsOnlyMultiWrite' is false and
            // we're not running in a transaction, then we need to target all shards -AND- we need
            // to set 'shardVersion' to IGNORED on all endpoints.
            //
            // (When 'isTargetDataOwningShardsOnlyMultiWrite' is true, StaleConfig errors with
            // partially applied writes will cause the write command to fail with a non-retryable
            // QueryPlanKilled error, and the user may choose to manually re-run the command if
            // desired.)
            //
            // Currently there are two cases where this block of code is reached:
            //   1) multi:true updates/upserts/deletes outside of transaction (where
            //      'isTimeseriesRetryableUpdateOp' and 'enableMultiWriteBlockingMigrations' and
            //      'isTargetDataOwningShardsOnlyMultiWrite' are all false)
            //   2) non-retryable or sessionless multi:false non-upsert updates/deletes
            //      that have an _id equality outside of a transaction (where
            //      'isTimeseriesRetryableUpdateOp' is false)
            //
            // TODO SPM-1153: Implement a new approach for multi:true updates/upserts/deletes that
            // does not need set 'shardVersion' to IGNORED and that can target only the relevant
            // shards when 'type' is kMultiShard (instead of targeting all shards).
            //
            // TODO SPM-3673: For non-retryable/sessionless multi:false non-upsert updates/deletes
            // that have an _id equality, implement a different approach that doesn't need to set
            // 'shardVersion' to IGNORED and that can target only the relevant shards when
            // 'type' is kMultiShard (instead of targeting all shards).
            tr.endpoints = targetAllShards(opCtx, cri);

            // Record that we set shardVersion to IGNORED for this op.
            usesSVIgnored = true;

            for (auto& endpoint : tr.endpoints) {
                auto& shardVersion = endpoint.shardVersion;
                tassert(11841901,
                        "Expected collection be sharded",
                        shardVersion && *shardVersion != ShardVersion::UNTRACKED());

                shardVersion->setPlacementVersionIgnored();
            }
        }
    }

    auto sampleId =
        analyze_shard_key::tryGenerateTargetedSampleId(opCtx, nss, op.getOpType(), tr.endpoints);

    // For kInternalTransaction batches, we do not translate viewful timeseries collection namespace
    // here, as it will be translated within the transaction when we analyze the request again.
    if (type == AnalysisType::kInternalTransaction) {
        isViewfulTimeseries = false;
    }

    // Record targeting stats. (For kInternalTransaction batches and kMultiWriteBlockingMigrations
    // batches, we skip recording the stats here because it will be handled later.)
    if (type != AnalysisType::kInternalTransaction &&
        type != AnalysisType::kMultiWriteBlockingMigrations) {
        recordTargetingStats(opCtx, cri, tr, op);
    }

    return Analysis{
        type, std::move(tr.endpoints), isViewfulTimeseries, std::move(sampleId), usesSVIgnored};
} catch (const DBException& ex) {
    auto status = ex.toStatus();

    LOGV2_DEBUG(10896516, 4, "Encountered targeting error", "error"_attr = redact(status));

    return status;
}

}  // namespace unified_write_executor
}  // namespace mongo
