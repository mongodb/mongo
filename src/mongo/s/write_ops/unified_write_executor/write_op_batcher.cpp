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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace unified_write_executor {

void WriteOpBatcher::markBatchReprocess(WriteBatch batch) {
    markOpReprocess(batch.getWriteOps());
}

void OrderedWriteOpBatcher::markUnrecoverableError() {
    _producer.stopProducingOps();
}

namespace {
bool writeTypeSupportsGrouping(BatchType writeType) {
    return writeType == kSingleShard || writeType == kMultiShard;
}

bool isCompatibleWithBatch(SimpleWriteBatch& writeBatch,
                           NamespaceString nss,
                           BatchType opType,
                           std::vector<ShardEndpoint>& shardsAffected) {
    if (!writeTypeSupportsGrouping(opType)) {
        return false;
    }

    // The new op cannot be included in the current batch if, for a given endpoint and namespace,
    // it needs to target a different shard version vs. what the current batch is targeting for
    // that endpoint and namespace.
    // TODO SERVER-104264: Once we account for 'OnlyTargetDataOwningShardsForMultiWritesParam'
    // cluster parameter, revisit this logic.
    for (const auto& shard : shardsAffected) {
        auto it = writeBatch.requestByShardId.find(shard.shardName);
        if (it != writeBatch.requestByShardId.end()) {
            auto versionFound = it->second.versionByNss.find(nss);
            // If the namespace is already in the batch, the shard version must be the same. If it's
            // not, we need a new batch.
            if (versionFound != it->second.versionByNss.end() && versionFound->second != shard) {
                LOGV2_DEBUG(10387002,
                            4,
                            "Cannot add op to batch because namespace was already targeted "
                            "with a different shard version",
                            "nss"_attr = nss);

                return false;
            }
        }
    }

    return true;
}

void addSingleShardWriteOpToBatch(
    SimpleWriteBatch& writeBatch,
    WriteOp& writeOp,
    std::vector<ShardEndpoint>& shardsAffected,
    boost::optional<analyze_shard_key::TargetedSampleId>& targetedSampleId) {
    tassert(10387000,
            "Single shard write type should only target a single shard",
            shardsAffected.size() == 1);

    const auto& shard = shardsAffected.front();
    const auto& shardName = shard.shardName;

    auto it = writeBatch.requestByShardId.find(shardName);
    if (it != writeBatch.requestByShardId.end()) {
        SimpleWriteBatch::ShardRequest& request = it->second;
        request.ops.push_back(writeOp);

        auto nss = writeOp.getNss();
        auto versionFound = request.versionByNss.find(nss);
        if (versionFound != request.versionByNss.end()) {
            tassert(10387001,
                    "Shard version for the same namespace need to be the same",
                    versionFound->second == shardsAffected.front());
        }
        request.versionByNss.emplace_hint(versionFound, nss, shardsAffected.front());
    } else {
        writeBatch.requestByShardId.emplace(
            shardName,
            SimpleWriteBatch::ShardRequest{std::map<NamespaceString, ShardEndpoint>{
                                               {writeOp.getNss(), shardsAffected.front()}},
                                           std::vector<WriteOp>{writeOp}});
    }

    if (targetedSampleId && targetedSampleId->isFor(shardName)) {
        auto& request = writeBatch.requestByShardId[shardName];
        request.sampleIds.emplace(writeOp.getId(), targetedSampleId->getId());
    }
}

void addMultiShardWriteOpToBatch(
    SimpleWriteBatch& writeBatch,
    WriteOp& writeOp,
    std::vector<ShardEndpoint>& shardsAffected,
    boost::optional<analyze_shard_key::TargetedSampleId>& targetedSampleId) {
    for (const auto& shardEndpoint : shardsAffected) {
        std::vector<ShardEndpoint> shardEndpoints{shardEndpoint};
        addSingleShardWriteOpToBatch(writeBatch, writeOp, shardEndpoints, targetedSampleId);
    }
}

void addWriteOpToBatch(SimpleWriteBatch& writeBatch, WriteOp& writeOp, Analysis& analysis) {
    switch (analysis.type) {
        case kSingleShard:
            addSingleShardWriteOpToBatch(
                writeBatch, writeOp, analysis.shardsAffected, analysis.targetedSampleId);
            break;
        case kMultiShard:
            addMultiShardWriteOpToBatch(
                writeBatch, writeOp, analysis.shardsAffected, analysis.targetedSampleId);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

StatusWith<WriteBatch> OrderedWriteOpBatcher::getNextBatch(OperationContext* opCtx,
                                                           RoutingContext& routingCtx,
                                                           const ErrorHandlerFn& eh) {
    boost::optional<SimpleWriteBatch> simpleBatch;
    boost::optional<ShardId> shardId;

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
            // If a target error occurred, call 'action = eh()'. If 'action' is kReturnError, then
            // return the error to the caller.
            const bool errorIsOutOfOrder = simpleBatch.has_value();
            auto action =
                eh ? eh(*writeOp, swAnalysis.getStatus(), errorIsOutOfOrder) : kReturnError;
            if (action == kReturnError) {
                if (simpleBatch) {
                    markBatchReprocess(WriteBatch{std::move(*simpleBatch)});
                    simpleBatch = boost::none;
                }
                return std::move(swAnalysis.getStatus());
            }
            // If 'action' is kConsumeErrorAndReturnEmptyBatch, consume the error and return an
            // empty batch to the caller.
            if (action == kConsumeErrorAndReturnEmptyBatch) {
                _producer.advance();
                if (simpleBatch) {
                    markBatchReprocess(WriteBatch{std::move(*simpleBatch)});
                    simpleBatch = boost::none;
                }
                return WriteBatch{};
            }
            // Otherwise, consume the error and return the batch we've built so far.
            _producer.advance();
            break;
        }

        // If analyze() was successful, see if 'writeOp' can be added to the current batch.
        auto& analysis = swAnalysis.getValue();
        tassert(10346700, "Expected at least one affected shard", !analysis.shardsAffected.empty());

        // If this is not the first op, see if it's compatible with the current batch.
        if (simpleBatch) {
            // If this op isn't compatible, stop and return the current batch.
            if (analysis.type != kSingleShard ||
                analysis.shardsAffected.front().shardName != *shardId) {
                break;
            }
            // Consume 'writeOp' and add it to the current batch and keep looping to see if more ops
            // can be added to the batch.
            addSingleShardWriteOpToBatch(
                *simpleBatch, *writeOp, analysis.shardsAffected, analysis.targetedSampleId);
            _producer.advance();
            continue;
        }

        // Consume the first op.
        _producer.advance();

        if (analysis.type == kSingleShard) {
            // If this is the first op and it's kSingleShard, then consume 'writeOp' and add it to
            // a SimpleBatch and keep looping to see if more ops can be added to the batch.
            simpleBatch.emplace();
            shardId = analysis.shardsAffected.front().shardName;
            addSingleShardWriteOpToBatch(
                *simpleBatch, *writeOp, analysis.shardsAffected, analysis.targetedSampleId);
        } else if (analysis.type == kMultiShard) {
            // If this is the first op and it's kMultiShard, then consume the op and put it in a
            // SimpleWriteBatch by itself and return the batch.
            SimpleWriteBatch batch;
            addMultiShardWriteOpToBatch(
                batch, *writeOp, analysis.shardsAffected, analysis.targetedSampleId);
            return WriteBatch{std::move(batch)};
        } else if (analysis.type == kNonTargetedWrite) {
            // If this is the first op and it's kNonTargetedWrite, then consume the op and put it in
            // a NonTargetedWriteBatch by itself and return the batch.
            auto sampleId = analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
            return WriteBatch{NonTargetedWriteBatch{*writeOp, std::move(sampleId)}};
        } else if (analysis.type == kInternalTransaction) {
            // For ops needing an internal transaction they are placed in their own batch.
            auto sampleId = analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
            return WriteBatch{InternalTransactionBatch{*writeOp, std::move(sampleId)}};
        } else {
            MONGO_UNREACHABLE_TASSERT(10346701);
        }
    }

    // Return the batch we've built.
    return simpleBatch ? WriteBatch{std::move(*simpleBatch)} : WriteBatch{};
}

StatusWith<WriteBatch> UnorderedWriteOpBatcher::getNextBatch(OperationContext* opCtx,
                                                             RoutingContext& routingCtx,
                                                             const ErrorHandlerFn& eh) {
    // When this method returns (or when an exception is thrown), mark the ops in 'opsToReprocess'
    // for re-processing.
    std::vector<WriteOp> opsToReprocess;
    ON_BLOCK_EXIT([&] { markOpReprocess(opsToReprocess); });

    boost::optional<SimpleWriteBatch> simpleBatch;

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
                analyzedOp.emplace(std::move(*writeOp), std::move(swAnalysis.getValue()));
                break;
            }
            // If a target error occurred, call 'action = eh()'. If 'action' is kReturnError, then
            // return the error to the caller.
            const bool errorIsOutOfOrder = false;
            auto action =
                eh ? eh(*writeOp, swAnalysis.getStatus(), errorIsOutOfOrder) : kReturnError;
            if (action == kReturnError) {
                if (simpleBatch) {
                    markBatchReprocess(WriteBatch{std::move(*simpleBatch)});
                    simpleBatch = boost::none;
                }
                return std::move(swAnalysis.getStatus());
            }
            // If 'action' is kConsumeErrorAndReturnEmptyBatch, consume the error and return an
            // empty batch to the caller.
            if (action == kConsumeErrorAndReturnEmptyBatch) {
                _producer.advance();
                if (simpleBatch) {
                    markBatchReprocess(WriteBatch{std::move(*simpleBatch)});
                    simpleBatch = boost::none;
                }
                return WriteBatch{};
            }
            // Otherwise, consume the error and then continue going around the inner loop.
            _producer.advance();
        }

        // If the producer has been exhausted, return the current batch to the caller.
        if (!analyzedOp) {
            break;
        }

        auto& [writeOp, analysis] = *analyzedOp;
        if (!simpleBatch) {
            // If the first WriteOp is kNonTargetedWrite, then consume the op and put it in a
            // NonTargetedWriteBatch by itself and return the batch.
            if (analysis.type == kNonTargetedWrite) {
                _producer.advance();
                auto sampleId =
                    analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
                return WriteBatch{NonTargetedWriteBatch{writeOp, std::move(sampleId)}};
            }
            if (analysis.type == kInternalTransaction) {
                _producer.advance();
                auto sampleId =
                    analysis.targetedSampleId.map([](auto& sid) { return sid.getId(); });
                return WriteBatch{InternalTransactionBatch{writeOp, std::move(sampleId)}};
            }
            // If the op is kSingleShard or kMultiShard, then add the op to a SimpleBatch and keep
            // looping to see if more ops can be added to the batch.
            simpleBatch.emplace();
            addWriteOpToBatch(*simpleBatch, writeOp, analysis);
        } else {
            // Check if 'writeOp' is compatible with the current batch.
            if (isCompatibleWithBatch(
                    *simpleBatch, writeOp.getNss(), analysis.type, analysis.shardsAffected)) {
                // Add 'writeOp' to the current batch.
                addWriteOpToBatch(*simpleBatch, writeOp, analysis);
            } else {
                // If 'writeOp' cannot be added to the current batch, then we add 'writeOp' to
                // the 'opsToReprocess' vector.
                opsToReprocess.emplace_back(writeOp);
            }
        }

        // Consume 'writeOp' and continue looking for more ops to add to the batch.
        _producer.advance();
    }

    // Return the batch we've built.
    return simpleBatch ? WriteBatch{std::move(*simpleBatch)} : WriteBatch{};
}

}  // namespace unified_write_executor
}  // namespace mongo
