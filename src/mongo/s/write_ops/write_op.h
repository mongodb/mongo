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

#pragma once

#include <absl/container/flat_hash_set.h>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <absl/hash/hash.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/ns_targeter.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/uuid.h"

namespace mongo {

struct TargetedWrite;
class WriteOp;

enum WriteOpState {
    // Item is ready to be targeted
    WriteOpState_Ready,

    // Item is targeted and we're waiting for outstanding shard requests to populate
    // responses
    WriteOpState_Pending,

    // This is used for WriteType::WriteWithoutShardKeyWithId to defer responses for child write ops
    // with n = 0 from shards only until we are sure that there won't be a retry of broadcast.
    WriteOpState_Deferred,

    // Op was successful, write completed
    // We assume all states higher than this one are *final*
    WriteOpState_Completed,

    // This is used for WriteType::WriteWithoutShardKeyWithId for child write ops only when we
    // decide we do not need to send the child op request or wait for its response from the targeted
    // shard.
    WriteOpState_NoOp,

    // Op failed with some error
    WriteOpState_Error,
};

enum class WriteType {
    Ordinary,
    TimeseriesRetryableUpdate,
    WithoutShardKeyOrId,
    WithoutShardKeyWithId,
};

/**
 * State of a write in-progress (to a single shard) which is one part of a larger write
 * operation.
 *
 * As above, the write op may finish in either a successful (_Completed) or unsuccessful
 * (_Error) state.
 */
struct ChildWriteOp {
    ChildWriteOp(WriteOp* const parent) : parentOp(parent) {}

    const WriteOp* const parentOp;

    WriteOpState state{WriteOpState_Ready};

    // non-zero when state == _Pending
    // Not owned here but tracked for reporting
    TargetedWrite* pendingWrite{nullptr};

    // filled when state > _Pending
    std::unique_ptr<ShardEndpoint> endpoint;

    // filled when state == _Error
    boost::optional<write_ops::WriteError> error;

    // filled when state == _Complete or state == _Deferred and this is an op from a bulkWrite
    // command.
    boost::optional<BulkWriteReplyItem> bulkWriteReplyItem;
};

/**
 * State of a single write item in-progress from a client request.
 *
 * The lifecyle of a write op:
 *
 *   0. Begins at _Ready,
 *
 *   1a. Targeted, and a ChildWriteOp created to track the state of each returned TargetedWrite.
 *       The state is changed to _Pending.
 *   1b. If the op cannot be targeted, the error is set directly (_Error), and the write op is
 *       completed.
 *
 *   2a.  The current TargetedWrites are cancelled, and the op state is reset to _Ready
 *   2b.  TargetedWrites finish successfully and unsuccessfully.
 *
 *   On the last error arriving...
 *
 *   3a. If the errors allow for retry, the WriteOp is reset to _Ready, previous ChildWriteOps
 *       are placed in the history, and goto 0.
 *   3b. If the errors don't allow for retry, they are combined into a single error and the
 *       state is changed to _Error.
 *   3c. If there are no errors, the state is changed to _Completed.
 *
 * WriteOps finish in a _Completed or _Error state.
 */
class WriteOp {
public:
    WriteOp(BatchItemRef itemRef, bool inTxn) : _itemRef(std::move(itemRef)), _inTxn(inTxn) {}

    /**
     * Returns the write item for this operation
     */
    const BatchItemRef& getWriteItem() const;

    /**
     * Returns the op's current state.
     */
    WriteOpState getWriteState() const;

    /**
     * Returns the op's error.
     *
     * Can only be used in state _Error
     */
    const write_ops::WriteError& getOpError() const;

    /**
     * Check if we have a stashed BulkWriteReplyItem so we can safely call
     * takeBulkWriteReplyItem. A writeOp for bulkWrite may not have one if
     * the command was run with errorsOnly=true.
     *
     * Can only be used in state _Complete.
     */
    bool hasBulkWriteReplyItem() const;

    /**
     * Take's the op's underlying BulkWriteReplyItem. This method must only be called one time
     * as the original value will be moved out when it is called.
     *
     * Can only be used in state _Complete and when this WriteOp is from the bulkWrite command.
     */
    BulkWriteReplyItem takeBulkWriteReplyItem();

    /**
     * Creates TargetedWrite operations for every applicable shard, which contain the
     * information needed to send the child writes generated from this write item.
     *
     * The ShardTargeter determines the ShardEndpoints to send child writes to, but is not
     * modified by this operation.
     */
    void targetWrites(OperationContext* opCtx,
                      const NSTargeter& targeter,
                      std::vector<std::unique_ptr<TargetedWrite>>* targetedWrites,
                      bool* useTwoPhaseWriteProtocol = nullptr,
                      bool* isNonTargetedWriteWithoutShardKeyWithExactId = nullptr);

    /**
     * Returns the number of child writes that were last targeted.
     */
    size_t getNumTargeted();

    /**
     * Resets the state of this write op to _Ready and stops waiting for any outstanding
     * TargetedWrites.
     *
     * Can only be called when state is _Pending, or is a no-op if called when the state
     * is still _Ready (and therefore no writes are pending).
     */
    void resetWriteToReady();

    /**
     * Marks the targeted write as finished for this write op. Optionally, if this write is part of
     * a bulkWrite command and has per-statement replies, stores the associated BulkWriteReplyItem.
     *
     * One of noteWriteComplete or noteWriteError should be called exactly once for every
     * TargetedWrite.
     */
    void noteWriteComplete(const TargetedWrite& targetedWrite,
                           boost::optional<const BulkWriteReplyItem&> reply = boost::none);

    /**
     * Stores the error response of a TargetedWrite for later use, marks the write as finished.
     *
     * As above, one of noteWriteComplete or noteWriteError should be called exactly once for
     * every TargetedWrite.
     */
    void noteWriteError(const TargetedWrite& targetedWrite, const write_ops::WriteError& error);

    /**
     * Marks the write op complete if n is 1 along with transitioning any pending child write ops to
     * WriteOpState::NoOp. If n is 0 then defers the state update of the child write op until later.
     */
    void noteWriteWithoutShardKeyWithIdResponse(
        const TargetedWrite& targetedWrite,
        int n,
        boost::optional<const BulkWriteReplyItem&> bulkWriteReplyItem);

    /**
     * Sets the reply for this write op directly, and forces the state to _Completed.
     *
     * Should only be used when in state _Ready.
     */
    void setOpComplete(boost::optional<BulkWriteReplyItem> bulkWriteReplyItem);

    /**
     * Sets the error for this write op directly, and forces the state to _Error.
     *
     * Should only be used when in state _Ready.
     */
    void setOpError(const write_ops::WriteError& error);

    /**
     * Sets the WriteType for this WriteOp.
     */
    void setWriteType(WriteType writeType);

    WriteType getWriteType() const;

    /**
     * Combines the pointed-to BulkWriteReplyItems into a single item. Used for merging the results
     * of multiple ChildWriteOps into a single reply item.
     */
    boost::optional<BulkWriteReplyItem> combineBulkWriteReplyItems(
        std::vector<BulkWriteReplyItem const*> replies);

    const std::vector<ChildWriteOp>& getChildWriteOps_forTest() const;

private:
    /**
     * Updates the op state after new information is received.
     */
    void _updateOpState();

    // Owned elsewhere, reference to a batch with a write item
    const BatchItemRef _itemRef;

    // What stage of the operation we are at
    WriteOpState _state{WriteOpState_Ready};

    // filled when state == _Pending
    std::vector<ChildWriteOp> _childOps;

    // filled when state == _Error
    boost::optional<write_ops::WriteError> _error;

    // filled for bulkWrite op when state == _Complete or before we reset state to _Ready after
    // receiving successful replies from some shards with a retryable error.
    boost::optional<BulkWriteReplyItem> _bulkWriteReplyItem;

    // Whether this write is part of a transaction.
    const bool _inTxn;

    // stores the shards where this write operation succeeded
    absl::flat_hash_set<ShardId> _successfulShardSet;

    WriteType _writeType{WriteType::Ordinary};
};
// First value is write item index in the batch, second value is child write op index
typedef std::pair<int, int> WriteOpRef;

/**
 * A write with A) a request targeted at a particular shard endpoint, and B) a response targeted
 * at a particular WriteOp.
 *
 * TargetedWrites are the link between the RPC layer and the in-progress write
 * operation.
 */
struct TargetedWrite {
    TargetedWrite(const ShardEndpoint& endpoint,
                  WriteOpRef writeOpRef,
                  boost::optional<UUID> sampleId)
        : endpoint(endpoint), writeOpRef(writeOpRef), sampleId(sampleId) {}

    // Where to send the write
    ShardEndpoint endpoint;

    // Where to find the write item and put the response
    // TODO: Could be a more complex handle, shared between write state and networking code if
    // we need to be able to cancel ops.
    WriteOpRef writeOpRef;

    // The unique sample id for the write if it has been chosen for sampling.
    boost::optional<UUID> sampleId;
};

/**
 * Data structure representing the information needed to make a batch request, along with
 * pointers to where the resulting responses should be placed.
 *
 * Internal support for storage as a doubly-linked list, to allow the TargetedWriteBatch to
 * efficiently be registered for reporting.
 */
class TargetedWriteBatch {
    TargetedWriteBatch(const TargetedWriteBatch&) = delete;
    TargetedWriteBatch& operator=(const TargetedWriteBatch&) = delete;

public:
    /**
     * baseCommandSizeBytes specifies an estimate of the size of the corresponding batch request
     * command prior to adding any write ops to it.
     */
    TargetedWriteBatch(const ShardId& shardId, const int baseCommandSizeBytes)
        : _shardId(shardId), _estimatedSizeBytes(baseCommandSizeBytes) {}

    const ShardId& getShardId() const {
        return _shardId;
    }

    const std::vector<std::unique_ptr<TargetedWrite>>& getWrites() const {
        return _writes;
    };

    size_t getNumOps() const {
        return _writes.size();
    }

    int getEstimatedSizeBytes() const {
        return _estimatedSizeBytes;
    }

    /**
     * TargetedWrite is owned here once given to the TargetedWriteBatch.
     */
    void addWrite(std::unique_ptr<TargetedWrite> targetedWrite, int estWriteSize);

private:
    // Where to send the batch
    const ShardId _shardId;

    // Where the responses go
    // TargetedWrite*s are owned by the TargetedWriteBatch
    std::vector<std::unique_ptr<TargetedWrite>> _writes;

    // Conservatively estimated size of the batch command, for ensuring it doesn't grow past the
    // maximum BSON size.
    int _estimatedSizeBytes;
};

}  // namespace mongo
