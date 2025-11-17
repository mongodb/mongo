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

#pragma once

#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {

struct EmptyBatch {
    std::vector<WriteOp> getWriteOps() const {
        return std::vector<WriteOp>{};
    }

    std::set<NamespaceString> getInvolvedNamespaces() const {
        return std::set<NamespaceString>{};
    }

    bool isFindAndModify() const {
        return false;
    }
};

struct SimpleWriteBatch {
    // Given that a write command can target multiple collections,
    // we store one shard version per namespace to support batching ops which target the same shard,
    // but target different namespaces.
    struct ShardRequest {
        std::map<NamespaceString, ShardEndpoint> versionByNss;
        std::set<NamespaceString> nssIsViewfulTimeseries;
        std::vector<WriteOp> ops;
        std::map<WriteOpId, UUID> sampleIds;
    };

    std::map<ShardId, ShardRequest> requestByShardId;

    std::vector<WriteOp> getWriteOps() const {
        std::vector<WriteOp> result;
        absl::flat_hash_set<WriteOpId> dedup;

        for (const auto& [_, req] : requestByShardId) {
            for (const auto& op : req.ops) {
                if (dedup.insert(op.getId()).second) {
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

    bool isFindAndModify() const {
        return requestByShardId.begin()->second.ops.front().isFindAndModify();
    }
};

struct NonTargetedWriteBatch {
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

    bool isFindAndModify() const {
        return op.isFindAndModify();
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

    bool isFindAndModify() const {
        return op.isFindAndModify();
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

    bool isFindAndModify() const {
        return op.isFindAndModify();
    }
};

struct WriteBatch {
    std::variant<EmptyBatch,
                 SimpleWriteBatch,
                 NonTargetedWriteBatch,
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

    bool isFindAndModify() const {
        return std::visit([](const auto& inner) { return inner.isFindAndModify(); }, data);
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
    WriteOpBatcher(WriteOpProducer& producer, WriteOpAnalyzer& analyzer)
        : _producer(producer), _analyzer(analyzer) {}

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

    bool getRetryOnTargetError() const {
        return _retryOnTargetError;
    }

    void setRetryOnTargetError(bool b) {
        _retryOnTargetError = b;
    }

    /**
     * Marks the shards that ops already succeeded in case we only need to retry parts
     * of any ops.
     */
    void noteSuccessfulShards(std::map<WriteOpId, std::set<ShardId>> successfulShardsToAdd) {
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
    WriteOpProducer& _producer;
    WriteOpAnalyzer& _analyzer;

    // Boolean flag that controls how target errors are handled.
    bool _retryOnTargetError = true;

    // Tracks which shards operations already succeeded on.
    std::map<WriteOpId, std::set<ShardId>> _successfulShardMap;
};

class OrderedWriteOpBatcher : public WriteOpBatcher {
public:
    OrderedWriteOpBatcher(WriteOpProducer& producer, WriteOpAnalyzer& analyzer)
        : WriteOpBatcher(producer, analyzer) {}

    BatcherResult getNextBatch(OperationContext* opCtx, RoutingContext& routingCtx) override;
};

class UnorderedWriteOpBatcher : public WriteOpBatcher {
public:
    UnorderedWriteOpBatcher(WriteOpProducer& producer, WriteOpAnalyzer& analyzer)
        : WriteOpBatcher(producer, analyzer) {}

    BatcherResult getNextBatch(OperationContext* opCtx, RoutingContext& routingCtx) override;
};

}  // namespace unified_write_executor
}  // namespace mongo
