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

#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"

#include "mongo/logv2/log.h"
#include "mongo/s/transaction_router.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace unified_write_executor {

namespace {
bool writeTypeSupportsGrouping(BatchType writeType) {
    return writeType == kSingleShard || writeType == kMultiShard;
}
}  // namespace

template <bool Ordered>
class SimpleBatchBuilderBase {
public:
    SimpleBatchBuilderBase(WriteOpBatcher& batcher) : _batcher(batcher) {}

    /**
     * If ops were added to '_batch' but done() was not called, mark all the ops in '_batch' for
     * reprocessing.
     */
    ~SimpleBatchBuilderBase() {
        if (_batch) {
            _batcher.markOpReprocess(_batch->getWriteOps());
        }
    }

    operator bool() const {
        return _batch.has_value();
    }

    bool operator!() const {
        return !_batch;
    }

    bool isEmpty() const {
        return !_batch;
    }

    /**
     * Returns true if the specified op ('nss' / 'analysis') can be added to the current batch,
     * otherwise returns false.
     */
    bool isCompatibleWithBatch(NamespaceString nss, Analysis& analysis) const {
        const auto& endpoints = analysis.shardsAffected;
        // If the op's type is not compatible with SimpleBatch, return false.
        if (!writeTypeSupportsGrouping(analysis.type)) {
            return false;
        }
        // Verify that there is at least one endpoint. Also, if this op is kSingleShard, verify
        // that there's exactly one endpoint.
        tassert(10896511, "Expected at least one affected shard", !endpoints.empty());
        tassert(10896512,
                "Single shard write type should only target a single shard",
                analysis.type != kSingleShard || endpoints.size() == 1);
        // If the current batch is empty, return true.
        if (!_batch || _batch->requestByShardId.empty()) {
            return true;
        }
        // If the write command is ordered -AND- if either the op targets multiple shards, the
        // current batch targets multiple shards, or the op and the current batch target different
        // shards, then return false.
        if (Ordered &&
            (analysis.type != kSingleShard || _batch->requestByShardId.size() > 1 ||
             endpoints.front().shardName != _batch->requestByShardId.begin()->first)) {
            return false;
        }
        // If, for some endpoint, the op needs to target a different shard version for namespace
        // 'nss' vs. what the current batch is targeting for that endpoint and namespace, return
        // false. Otherwise, return true.
        return !wasShardAlreadyTargetedWithDifferentShardVersion(nss, analysis);
    }

    /**
     * Adds 'writeOp' to the current batch. This method will fail with a tassert if writeOp's type
     * is not compatible with SimpleWriteBatch.
     */
    void addOp(WriteOp& writeOp, Analysis& analysis) {
        tassert(10896513,
                "Expected op to be compatible with SimpleWriteBatch",
                writeTypeSupportsGrouping(analysis.type));

        if (!_batch) {
            _batch.emplace();
        }

        for (const auto& shard : analysis.shardsAffected) {
            auto it = _batch->requestByShardId.find(shard.shardName);
            if (it != _batch->requestByShardId.end()) {
                SimpleWriteBatch::ShardRequest& request = it->second;
                request.ops.push_back(writeOp);

                auto nss = writeOp.getNss();
                auto versionFound = request.versionByNss.find(nss);
                if (versionFound != request.versionByNss.end()) {
                    tassert(10387001,
                            "Shard version for the same namespace need to be the same",
                            versionFound->second == shard);
                }

                request.versionByNss.emplace_hint(versionFound, nss, shard);
            } else {
                _batch->requestByShardId.emplace(
                    shard.shardName,
                    SimpleWriteBatch::ShardRequest{
                        std::map<NamespaceString, ShardEndpoint>{{writeOp.getNss(), shard}},
                        std::vector<WriteOp>{writeOp}});
            }

            const auto& targetedSampleId = analysis.targetedSampleId;
            if (targetedSampleId && targetedSampleId->isFor(shard.shardName)) {
                auto& request = _batch->requestByShardId[shard.shardName];
                request.sampleIds.emplace(writeOp.getId(), targetedSampleId->getId());
            }
        }
    }

    /**
     * Finish the current batch being built and return it.
     */
    WriteBatch done() {
        WriteBatch result = _batch ? WriteBatch{std::move(*_batch)} : WriteBatch{};
        _batch = boost::none;
        return result;
    }

protected:
    /**
     * This helper method will return true if-and-only-if 'analysis', for some endpoint, needs to
     * target a different shard version for namespace 'nss' vs. what the current batch is targeting
     * for that endpoint and namespace.
     */
    bool wasShardAlreadyTargetedWithDifferentShardVersion(NamespaceString nss,
                                                          Analysis& analysis) const {
        // TODO SERVER-104264: Once we account for 'OnlyTargetDataOwningShardsForMultiWritesParam'
        // cluster parameter, revisit this logic.
        for (const auto& shard : analysis.shardsAffected) {
            auto it = _batch->requestByShardId.find(shard.shardName);
            if (it != _batch->requestByShardId.end()) {
                auto versionFound = it->second.versionByNss.find(nss);
                // If the namespace is already in the batch, the shard version must be the same.
                // If it's not, we need a new batch.
                if (versionFound != it->second.versionByNss.end() &&
                    versionFound->second != shard) {
                    LOGV2_DEBUG(10387002,
                                4,
                                "Cannot add op to batch because namespace was already targeted "
                                "with a different shard version",
                                "nss"_attr = nss);

                    return true;
                }
            }
        }

        return false;
    }

    WriteOpBatcher& _batcher;
    boost::optional<SimpleWriteBatch> _batch;
};

class OrderedSimpleBatchBuilder : public SimpleBatchBuilderBase<true> {
public:
    using SimpleBatchBuilderBase::SimpleBatchBuilderBase;
};

class UnorderedSimpleBatchBuilder : public SimpleBatchBuilderBase<false> {
public:
    using SimpleBatchBuilderBase::SimpleBatchBuilderBase;
};

void WriteOpBatcher::markBatchReprocess(WriteBatch batch) {
    markOpReprocess(batch.getWriteOps());
}

WriteOpBatcher::Result OrderedWriteOpBatcher::getNextBatch(OperationContext* opCtx,
                                                           RoutingContext& routingCtx) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    std::vector<std::pair<WriteOp, Status>> opsWithErrors;
    OrderedSimpleBatchBuilder builder(*this);

    for (;;) {
        // Peek at the next op from the producer. If the producer has been exhausted, return the
        // current batch to the caller.
        auto writeOp = _producer.peekNext();
        if (!writeOp) {
            break;
        }

        // Call analyze(). If an error occurs, handle it.
        auto swAnalysis = _analyzer.analyze(opCtx, routingCtx, *writeOp);
        if (!swAnalysis.isOK()) {
            // If the write command is running in a transaction, then discard the current batch,
            // consume the op, record the error, and return an empty batch.
            if (inTransaction) {
                opsWithErrors.emplace_back(*writeOp, swAnalysis.getStatus());
                _producer.advance();

                LOGV2_DEBUG(10896514,
                            2,
                            "Aborting write command due to error in transaction",
                            "error"_attr = redact(swAnalysis.getStatus()));

                return {WriteBatch{}, std::move(opsWithErrors)};
            }
            // If the write command is not in a transaction and '_retryOnTargetError' is true, then
            // discard the current batch, refresh the catalog cache, set '_retryOnTargetError' to
            // false, and return an empty batch. For this case, we intentionally do not consume the
            // op or record the error.
            if (_retryOnTargetError) {
                LOGV2_DEBUG(10896515,
                            2,
                            "Encountered a targeter error, will refresh RoutingContext",
                            "error"_attr = redact(swAnalysis.getStatus()));

                for (const auto& nss : routingCtx.getNssList()) {
                    routingCtx.onStaleShardVersionError(nss, boost::none /*wantedVersion*/);
                }

                _retryOnTargetError = false;
                return {WriteBatch{}, std::move(opsWithErrors)};
            }
            // When the write command is not in a transaction and '_retryOnTargetError' is false,
            // if the current batch is empty, then we consume the op, record the error, and return
            // an empty batch.
            if (!builder) {
                opsWithErrors.emplace_back(*writeOp, swAnalysis.getStatus());
                _producer.advance();
                return {WriteBatch{}, std::move(opsWithErrors)};
            }
            // When the write command is not in a transaction and '_retryOnTargetError' is false,
            // if the current batch is _not_ empty, we choose to not record/consume the error and
            // return the batch we've built so far.
            break;
        }

        // If analyze() was successful, see if 'writeOp' can be added to the current batch.
        auto& analysis = swAnalysis.getValue();

        // Skips any operations for which all the shards already got successful replies.
        removeSuccessfulShardsFromEndpoints(writeOp->getId(), analysis.shardsAffected);
        if (analysis.shardsAffected.empty()) {
            _producer.advance();
            continue;
        }

        if (builder) {
            // If this is not the first op, see if it's compatible with the current batch.
            if (builder.isCompatibleWithBatch(writeOp->getNss(), analysis)) {
                // If 'writeOp', consume it and add it to the current batch, and keep looping to see
                // if more ops can be added to the batch.
                _producer.advance();
                builder.addOp(*writeOp, analysis);
            } else {
                // If this op isn't compatible, break out of the loop and return the current batch.
                break;
            }
        } else {
            // Consume the first op.
            _producer.advance();
            // If the first WriteOp is kNonTargetedWrite, then consume the op and put it in a
            // NonTargetedWriteBatch by itself and return the batch.
            if (analysis.type == kNonTargetedWrite) {
                auto sampleId =
                    analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
                return {WriteBatch{NonTargetedWriteBatch{*writeOp, std::move(sampleId)}},
                        std::move(opsWithErrors)};
            }
            // If the first WriteOp is kInternalTransaction, put it in a InternalTransactionBatch
            // by itself and return the batch.
            if (analysis.type == kInternalTransaction) {
                auto sampleId =
                    analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
                return {WriteBatch{InternalTransactionBatch{*writeOp, std::move(sampleId)}},
                        std::move(opsWithErrors)};
            }
            // If the first WriteOp is kMultiWriteBlockingMigration, put it in a
            // MultiWriteBlockingMigrationsBatch by itself and return the batch.
            if (analysis.type == kMultiWriteBlockingMigrations) {
                auto sampleId =
                    analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
                return {
                    WriteBatch{MultiWriteBlockingMigrationsBatch{*writeOp, std::move(sampleId)}},
                    std::move(opsWithErrors)};
            }
            // If the first WriteOp is kMultiShard, then add the op to a SimpleBatch and then break
            // and return the batch.
            if (analysis.type == kMultiShard) {
                builder.addOp(*writeOp, analysis);
                break;
            }
            // If the op is kSingleShard, then add the op to a SimpleBatch and keep looping to see
            // if more ops can be added to the batch.
            builder.addOp(*writeOp, analysis);
        }
    }

    // Return the batch we've built so far.
    return {builder.done(), std::move(opsWithErrors)};
}

WriteOpBatcher::Result UnorderedWriteOpBatcher::getNextBatch(OperationContext* opCtx,
                                                             RoutingContext& routingCtx) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    // When this method returns (or when an exception is thrown), mark the ops in 'opsToReprocess'
    // for re-processing.
    std::vector<WriteOp> opsToReprocess;
    ON_BLOCK_EXIT([&] { markOpReprocess(opsToReprocess); });

    std::vector<std::pair<WriteOp, Status>> opsWithErrors;
    UnorderedSimpleBatchBuilder builder(*this);

    // This outer loop searches for ops to add to the current batch.
    for (;;) {
        boost::optional<std::pair<WriteOp, Analysis>> analyzedOp;

        // This inner loop that repeatedly peeks at the next op from the producer until either:
        // (i) we successfully analyze an op, or (ii) we exhaust the producer, or (iii) we encounter
        // an error that requires returning to the caller.
        while (auto writeOp = _producer.peekNext()) {
            // Call analyze(). If it succeeds, store 'writeOp' and 'swAnalysis.getValue()' into
            // 'analyzedOp' and break out of the inner loop.
            auto swAnalysis = _analyzer.analyze(opCtx, routingCtx, *writeOp);
            if (swAnalysis.isOK()) {
                // Advance past any ops where the operation already succeeded.
                auto analysis = swAnalysis.getValue();

                // Skips any operations for which all the shards already got successful replies.
                removeSuccessfulShardsFromEndpoints(writeOp->getId(), analysis.shardsAffected);
                if (analysis.shardsAffected.empty()) {
                    _producer.advance();
                    continue;
                }

                analyzedOp.emplace(std::move(*writeOp), std::move(analysis));
                break;
            }
            // If the write command is running in a transaction, then discard the current batch,
            // consume the op, record the error, and return an empty batch.
            if (inTransaction) {
                opsWithErrors.emplace_back(std::move(*writeOp), swAnalysis.getStatus());
                _producer.advance();

                LOGV2_DEBUG(10896517,
                            2,
                            "Aborting write command due to error in transaction",
                            "error"_attr = redact(swAnalysis.getStatus()));

                return {WriteBatch{}, std::move(opsWithErrors)};
            }
            // If the write command is not in a transaction and '_retryOnTargetError' is true, then
            // discard the current batch, refresh the catalog cache, set '_retryOnTargetError' to
            // false, and return an empty batch. For this case, we intentionally do not consume the
            // op or record the error.
            if (_retryOnTargetError) {
                LOGV2_DEBUG(10896518,
                            2,
                            "Encountered a targeter error, will refresh RoutingContext",
                            "error"_attr = redact(swAnalysis.getStatus()));

                for (const auto& nss : routingCtx.getNssList()) {
                    routingCtx.onStaleShardVersionError(nss, boost::none /*wantedVersion*/);
                }

                _retryOnTargetError = false;
                return {WriteBatch{}, std::move(opsWithErrors)};
            }
            // If the write command is not in a transaction and '_retryOnTargetError' is false,
            // then we consume the op, record the error, and continue looking for more ops to add
            // to the batch we're building.
            opsWithErrors.emplace_back(std::move(*writeOp), swAnalysis.getStatus());
            _producer.advance();
        }

        // If the producer has been exhausted, return the current batch to the caller.
        if (!analyzedOp) {
            break;
        }

        auto& [writeOp, analysis] = *analyzedOp;

        if (builder) {
            // Consume 'writeOp'.
            _producer.advance();
            // Check if 'writeOp' is compatible with the current batch.
            if (builder.isCompatibleWithBatch(writeOp.getNss(), analysis)) {
                // Add 'writeOp' to the current batch.
                builder.addOp(writeOp, analysis);
            } else {
                // If 'writeOp' cannot be added to the current batch, then we add 'writeOp' to
                // the 'opsToReprocess' vector.
                opsToReprocess.emplace_back(writeOp);
            }
            // Continue looping looking for more ops to add to the batch.
        } else {
            // Consume 'writeOp'.
            _producer.advance();
            // If the first WriteOp is kNonTargetedWrite, then consume the op and put it in a
            // NonTargetedWriteBatch by itself and return the batch.
            if (analysis.type == kNonTargetedWrite) {
                auto sampleId =
                    analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
                return {WriteBatch{NonTargetedWriteBatch{writeOp, std::move(sampleId)}},
                        std::move(opsWithErrors)};
            }
            // If the first WriteOp is kInternalTransaction, then consume the op and put it in a
            // InternalTransactionBatch by itself and return the batch.
            if (analysis.type == kInternalTransaction) {
                auto sampleId =
                    analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
                return {WriteBatch{InternalTransactionBatch{writeOp, std::move(sampleId)}},
                        std::move(opsWithErrors)};
            }
            // If the first WriteOp is kMultiWriteBlockingMigration, put it in a
            // MultiWriteBlockingMigrationsBatch by itself and return the batch.
            if (analysis.type == kMultiWriteBlockingMigrations) {
                auto sampleId =
                    analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
                return {WriteBatch{MultiWriteBlockingMigrationsBatch{writeOp, std::move(sampleId)}},
                        std::move(opsWithErrors)};
            }
            // If the op is kSingleShard or kMultiShard, then add the op to a SimpleBatch and keep
            // looping to see if more ops can be added to the batch.
            builder.addOp(writeOp, analysis);
        }
    }

    // Return the batch we've built so far.
    return {builder.done(), std::move(opsWithErrors)};
}

}  // namespace unified_write_executor
}  // namespace mongo
