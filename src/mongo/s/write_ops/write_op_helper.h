// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/commands/query_cmd/bulk_write_crud_op.h"
#include "mongo/db/commands/query_cmd/bulk_write_parser.h"
#include "mongo/s/write_ops/write_command_ref.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace write_op_helpers {

class BatchCommandSizeEstimatorBase {
public:
    BatchCommandSizeEstimatorBase() = default;
    virtual ~BatchCommandSizeEstimatorBase() = default;

    virtual int getBaseSizeEstimate() const = 0;
    virtual int getOpSizeEstimate(int opIdx, const ShardId& shard) const = 0;
    virtual void addOpToBatch(int opIdx, const ShardId& shard) = 0;

protected:
    // Copy/move constructors and assignment operators are declared protected to prevent slicing.
    // Derived classes can supply public copy/move constructors and assignment operators if desired.
    BatchCommandSizeEstimatorBase(const BatchCommandSizeEstimatorBase&) = default;
    BatchCommandSizeEstimatorBase(BatchCommandSizeEstimatorBase&&) = default;
    BatchCommandSizeEstimatorBase& operator=(const BatchCommandSizeEstimatorBase&) = default;
    BatchCommandSizeEstimatorBase& operator=(BatchCommandSizeEstimatorBase&&) = default;
};

class BulkCommandSizeEstimator final : public write_op_helpers::BatchCommandSizeEstimatorBase {
public:
    explicit BulkCommandSizeEstimator(OperationContext* opCtx,
                                      const BulkWriteCommandRequest& clientRequest);

    int getBaseSizeEstimate() const final;
    int getOpSizeEstimate(int opIdx, const ShardId& shardId) const final;
    void addOpToBatch(int opIdx, const ShardId& shardId) final;

private:
    const BulkWriteCommandRequest& _clientRequest;
    const bool _isRetryableWriteOrInTransaction;
    const int _baseSizeEstimate;

    // targetWriteOps() can target writes to different shards which will end up being executed
    // inside different child batches. We need to keep a map of shardId to a set of all of the
    // nsInfo indexes we have account for the size of. We only want to count each nsInfoIdx once
    // per child batch.
    absl::flat_hash_map<ShardId, absl::flat_hash_set<NamespaceString>> _accountedForNsInfos;
};


class BatchedCommandSizeEstimator final : public write_op_helpers::BatchCommandSizeEstimatorBase {
public:
    explicit BatchedCommandSizeEstimator(OperationContext* opCtx,
                                         const BatchedCommandRequest& clientRequest);

    int getBaseSizeEstimate() const final;
    int getOpSizeEstimate(int opIdx, const ShardId& shardId) const final;
    void addOpToBatch(int opIdx, const ShardId& shardId) final {}

private:
    const BatchedCommandRequest& _clientRequest;
    const bool _isRetryableWriteOrInTransaction;
    const int _baseSizeEstimate;
};

class NoBatchCommandSizeEstimator final : public write_op_helpers::BatchCommandSizeEstimatorBase {
public:
    explicit NoBatchCommandSizeEstimator() {}

    int getBaseSizeEstimate() const final {
        return 0;
    }
    int getOpSizeEstimate(int opIdx, const ShardId& shardId) const final {
        return 0;
    }
    void addOpToBatch(int opIdx, const ShardId& shardId) final {}
};

bool isRetryErrCode(int errCode);

template <typename ItemType, typename GetCodeFn>
bool errorsAllSame(const std::vector<ItemType>& items, GetCodeFn getCodeFn) {
    tassert(10412301, "Expected at least one item", !items.empty());

    auto code = getCodeFn(items.front());
    return std::all_of(++items.begin(), items.end(), [code, &getCodeFn](const ItemType& item) {
        return getCodeFn(item) == code;
    });
}

template <typename ItemType, typename GetCodeFn>
bool hasOnlyOneNonRetryableError(const std::vector<ItemType>& items, GetCodeFn getCodeFn) {
    return std::count_if(items.begin(), items.end(), [&getCodeFn](const ItemType& item) {
               auto code = getCodeFn(item);
               return code != ErrorCodes::OK && !isRetryErrCode(code);
           }) == 1;
}

template <typename ItemType, typename GetCodeFn>
bool hasAnyNonRetryableError(const std::vector<ItemType>& items, GetCodeFn getCodeFn) {
    return std::count_if(items.begin(), items.end(), [&getCodeFn](const ItemType& item) {
               auto code = getCodeFn(item);
               return code != ErrorCodes::OK && !isRetryErrCode(code);
           }) > 0;
}

template <typename ItemType, typename GetCodeFn>
ItemType getFirstNonRetryableError(const std::vector<ItemType>& items, GetCodeFn getCodeFn) {
    auto nonRetryableError =
        std::find_if(items.begin(), items.end(), [&getCodeFn](const ItemType& item) {
            auto code = getCodeFn(item);
            return code != ErrorCodes::OK && !isRetryErrCode(code);
        });

    tassert(10412307, "No non-retryable error found", nonRetryableError != items.end());
    return *nonRetryableError;
}

/**
 * Fetch and return the value of the "onlyTargetDataOwningShardsForMultiWrites" cluster param.
 */
bool isOnlyTargetDataOwningShardsForMultiWritesEnabled();

/**
 * Return true if 'status' contains a CollectionUUIDMismatch error without an actual namespace,
 * otherwise return false.
 */
bool isCollUUIDMismatchWithoutActualNamespace(const Status& status);

int computeBaseSizeEstimate(OperationContext* opCtx, const BulkWriteCommandRequest& client);

BulkWriteDeleteOp toBulkWriteDelete(const write_ops::DeleteOpEntry& op);

BulkWriteUpdateOp toBulkWriteUpdate(const write_ops::UpdateOpEntry& op);

BulkWriteOpVariant getOrMakeBulkWriteOp(WriteOpRef op);

write_ops::UpdateOpEntry getOrMakeUpdateOpEntry(UpdateOpRef updateOp);

write_ops::DeleteOpEntry getOrMakeDeleteOpEntry(DeleteOpRef deleteOp);

}  // namespace write_op_helpers
}  // namespace mongo
