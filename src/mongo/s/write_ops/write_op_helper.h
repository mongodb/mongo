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
 * Returns whether an operation should target all shards with ShardVersion::IGNORED(). This is
 * true for multi: true writes where 'onlyTargetDataOwningShardsForMultiWrites' is false and we are
 * not in a transaction.
 */
bool shouldTargetAllShardsSVIgnored(bool inTransaction, bool isMulti);

/**
 * Used to check if a partially applied (successful on some shards but not others)operation has an
 * errors that is safe to ignore. UUID mismatch errors are safe to ignore if the actualCollection is
 * null in conjuntion with other successful operations. This is true because it means we wrongly
 * targeted a non-owning shard with the operation and we wouldn't have applied any modifications
 * anyway. Note this is only safe if we're using ShardVersion::IGNORED since we're ignoring any
 * placement concern and broadcasting to all shards.
 */
bool isSafeToIgnoreErrorInPartiallyAppliedOp(const Status& status);

int computeBaseSizeEstimate(OperationContext* opCtx, const BulkWriteCommandRequest& client);

BulkWriteDeleteOp toBulkWriteDelete(const write_ops::DeleteOpEntry& op);

BulkWriteUpdateOp toBulkWriteUpdate(const write_ops::UpdateOpEntry& op);

BulkWriteOpVariant getOrMakeBulkWriteOp(WriteOpRef op);

write_ops::UpdateOpEntry getOrMakeUpdateOpEntry(UpdateOpRef updateOp);

write_ops::DeleteOpEntry getOrMakeDeleteOpEntry(DeleteOpRef deleteOp);

}  // namespace write_op_helpers
}  // namespace mongo
