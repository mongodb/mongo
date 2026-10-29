// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"
#include "mongo/s/write_ops/write_op_helper.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {

bool analysisTypeSupportsGrouping(AnalysisType type);

struct EmptyBatch {
    std::vector<WriteOp> getWriteOps() const {
        return std::vector<WriteOp>{};
    }

    std::set<NamespaceString> getInvolvedNamespaces() const {
        return std::set<NamespaceString>{};
    }
};

struct SimpleWriteBatch {
    // Given that a write command can target multiple collections, we store one shard version per
    // namespace to support batching ops which target different namespaces on the same shard.
    struct ShardRequest {
        absl::flat_hash_map<NamespaceString, ShardEndpoint> versionByNss;
        std::set<NamespaceString> nssIsViewfulTimeseries;
        std::vector<WriteOp> ops;
        std::map<WriteOpId, UUID> sampleIds;
        int sizeEstimate;  // Stores the size of the base command and all of the ops.
    };

    std::map<ShardId, ShardRequest> requestByShardId;
    bool isRetryableWriteWithId = false;
    absl::flat_hash_set<WriteOpId> opsUsingSVIgnored;

    static SimpleWriteBatch makeEmpty(bool isRetryableWriteWithId) {
        return SimpleWriteBatch{{}, isRetryableWriteWithId};
    }

    std::vector<WriteOp> getWriteOps() const {
        std::vector<WriteOp> result;
        absl::flat_hash_set<WriteOpId> dedup;

        for (const auto& [_, req] : requestByShardId) {
            for (const auto& op : req.ops) {
                if (dedup.insert(getWriteOpId(op)).second) {
                    result.emplace_back(op);
                }
            }
        }

        return result;
    }

    std::set<NamespaceString> getInvolvedNamespaces() const {
        std::set<NamespaceString> result;
        for (const auto& [_, req] : requestByShardId) {
            for (const auto& [nss, _] : req.versionByNss) {
                result.insert(nss);
            }
        }
        return result;
    }
};

struct TwoPhaseWriteBatch {
    WriteOp op;
    boost::optional<UUID> sampleId;
    bool isViewfulTimeseries;

    std::vector<WriteOp> getWriteOps() const {
        std::vector<WriteOp> result;
        result.emplace_back(op);
        return result;
    }

    std::set<NamespaceString> getInvolvedNamespaces() const {
        return {op.getNss()};
    }
};

struct InternalTransactionBatch {
    WriteOp op;
    boost::optional<UUID> sampleId;

    std::vector<WriteOp> getWriteOps() const {
        std::vector<WriteOp> result;
        result.emplace_back(op);
        return result;
    }

    std::set<NamespaceString> getInvolvedNamespaces() const {
        return {op.getNss()};
    }
};

struct MultiWriteBlockingMigrationsBatch {
    WriteOp op;
    boost::optional<UUID> sampleId;
    bool isViewfulTimeseries;

    std::vector<WriteOp> getWriteOps() const {
        std::vector<WriteOp> result;
        result.emplace_back(op);
        return result;
    }

    std::set<NamespaceString> getInvolvedNamespaces() const {
        return {op.getNss()};
    }
};

struct WriteBatch {
    std::variant<EmptyBatch,
                 SimpleWriteBatch,
                 TwoPhaseWriteBatch,
                 InternalTransactionBatch,
                 MultiWriteBlockingMigrationsBatch>
        data;

    explicit operator bool() const {
        return !isEmptyBatch();
    }

    bool operator!() const {
        return isEmptyBatch();
    }

    bool isEmptyBatch() const {
        return holds_alternative<EmptyBatch>(data);
    }

    std::vector<WriteOp> getWriteOps() const {
        return std::visit([](const auto& inner) { return inner.getWriteOps(); }, data);
    }

    std::set<NamespaceString> getInvolvedNamespaces() const {
        return std::visit([](const auto& inner) { return inner.getInvolvedNamespaces(); }, data);
    }
};

struct BatcherResult {
    WriteBatch batch;
    std::vector<std::pair<WriteOp, Status>> opsWithErrors;
    bool transientTxnError = false;

    bool hasTransientTxnError() const {
        return !opsWithErrors.empty() && transientTxnError;
    }
    const Status& getTransientTxnError() const {
        tassert(11272109, "Expected transient transaction error", hasTransientTxnError());
        return opsWithErrors.front().second;
    }
};

/**
 * Based on the analysis of the write ops, this class bundles multiple write ops into batches to be
 * sent to shards.
 */
class WriteOpBatcher {
public:
    WriteOpBatcher(WriteOpProducer& producer, WriteOpAnalyzer& analyzer, WriteCommandRef cmdRef)
        : _producer(producer), _analyzer(analyzer), _cmdRef(std::move(cmdRef)) {}

    virtual ~WriteOpBatcher() = default;

    /**
     * This method makes a new batch using ops taken from the producer and returns it. Depending on
     * the results from analyzing the ops from the producer, the batch returned may have different
     * types. If the producer has no more ops, this function returns an EmptyBatch.
     */
    virtual BatcherResult getNextBatch(OperationContext* opCtx, RoutingContext& routingCtx) = 0;

    /**
     * This method consumes all remaining ops from the producer and returns these ops in a vector.
     */
    std::vector<WriteOp> getAllRemainingOps() {
        return _producer.consumeAllRemainingOps();
    }

    /**
     * Mark a write op to be reprocessed, which will in turn be reanalyzed and rebatched.
     */
    void markOpReprocess(const WriteOp& op) {
        _producer.markOpReprocess(op);
    }

    /**
     * Mark a list of write ops to be reprocessed, which will in turn be reanalyzed and rebatched.
     */
    void markOpReprocess(const std::vector<WriteOp>& ops) {
        for (const auto& op : ops) {
            _producer.markOpReprocess(op);
        }
    }

    /**
     * Marks all of the write ops in 'batch' to be reprocessed. These ops in turn will be reanalyzed
     * and rebatched.
     */
    void markBatchReprocess(WriteBatch batch);

    /**
     * Returns true if there are no more ops left to batch.
     */
    bool isDone() {
        return _producer.peekNext() == boost::none;
    }

    /**
     * Instructs the batcher to not make any more batches. After this method is called, isDone()
     * will always return true and getNextBatch() will always return an EmptyBatch.
     */
    void stopMakingBatches() {
        _producer.stopProducingOps();
    }

    /**
     * Marks the shards that ops already succeeded in case we only need to retry parts
     * of any ops.
     */
    void noteSuccessfulShards(
        absl::flat_hash_map<WriteOpId, std::set<ShardId>> successfulShardsToAdd) {
        for (auto& [opId, successfulShards] : successfulShardsToAdd) {
            auto it = _successfulShardMap.find(opId);
            if (it == _successfulShardMap.cend()) {
                _successfulShardMap.emplace(opId, std::move(successfulShards));
            } else {
                std::move(successfulShards.begin(),
                          successfulShards.end(),
                          std::inserter(it->second, it->second.end()));
            }
        }
    }

    /**
     * Removes any shards that already succeeded from the endpoints for the operations with the
     * provided id.
     */
    void removeSuccessfulShardsFromEndpoints(WriteOpId opId,
                                             std::vector<ShardEndpoint>& endpoints) {
        // Remove shards that already succeeded in previous attempts.
        auto it = _successfulShardMap.find(opId);
        if (it != _successfulShardMap.cend()) {
            std::erase_if(endpoints, [&](auto&& e) { return it->second.count(e.shardName); });
        }
    }

protected:
    /**
     * If the write command is not in a transaction and '_retryOnTargetError' is true, then discard
     * the current batch, refresh the catalog cache, set '_retryOnTargetError' to false. The caller
     * should return an empty batch, and we intentionally do not consume the op or record the error
     * in this case.
     */
    bool retryOnTargetError(RoutingContext& routingCtx, Status status);

    WriteOpProducer& _producer;
    WriteOpAnalyzer& _analyzer;

    // Boolean flag that controls how target errors are handled.
    bool _retryOnTargetError = true;

    // Tracks which shards operations already succeeded on.
    absl::flat_hash_map<WriteOpId, std::set<ShardId>> _successfulShardMap;

    const WriteCommandRef _cmdRef;
};

class OrderedWriteOpBatcher : public WriteOpBatcher {
public:
    OrderedWriteOpBatcher(WriteOpProducer& producer,
                          WriteOpAnalyzer& analyzer,
                          WriteCommandRef cmdRef)
        : WriteOpBatcher(producer, analyzer, cmdRef) {}

    BatcherResult getNextBatch(OperationContext* opCtx, RoutingContext& routingCtx) override;
};

class UnorderedWriteOpBatcher : public WriteOpBatcher {
public:
    UnorderedWriteOpBatcher(WriteOpProducer& producer,
                            WriteOpAnalyzer& analyzer,
                            WriteCommandRef cmdRef)
        : WriteOpBatcher(producer, analyzer, cmdRef) {}

    BatcherResult getNextBatch(OperationContext* opCtx, RoutingContext& routingCtx) override;
};

}  // namespace unified_write_executor
}  // namespace mongo
