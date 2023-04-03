/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/s/write_ops/bulk_write_exec.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

// The number of times we'll try to continue a batch op if no progress is being made. This only
// applies when no writes are occurring and metadata is not changing on reload.
const int kMaxRoundsWithoutProgress(5);

}  // namespace
namespace bulk_write_exec {

std::vector<BulkWriteReplyItem> execute(OperationContext* opCtx,
                                        const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                                        const BulkWriteCommandRequest& clientRequest) {
    LOGV2_DEBUG(7263700,
                4,
                "Starting execution of a bulkWrite",
                "size"_attr = clientRequest.getOps().size(),
                "nsInfoSize"_attr = clientRequest.getNsInfo().size());

    BulkWriteOp bulkWriteOp(opCtx, clientRequest);

    bool refreshedTargeter = false;
    int rounds = 0;
    int numCompletedOps = 0;
    int numRoundsWithoutProgress = 0;

    while (!bulkWriteOp.isFinished()) {
        // 1: Target remaining ops with the appropriate targeter based on the namespace index and
        // re-batch ops based on their targeted shard id.
        TargetedBatchMap childBatches;

        bool recordTargetErrors = refreshedTargeter;
        auto targetStatus = bulkWriteOp.target(targeters, recordTargetErrors, childBatches);
        if (!targetStatus.isOK()) {
            dassert(childBatches.size() == 0u);
            // The target error comes from one of the targeters. But to avoid getting another target
            // error from another targeter in retry, we simply refresh all targeters and only retry
            // once for target errors. The performance hit should be negligible as target errors
            // should be rare.
            for (auto& targeter : targeters) {
                targeter->noteCouldNotTarget();
            }
            refreshedTargeter = true;
        }

        // 2: Use MultiStatementTransactionRequestsSender to send any ready sub-batches to targeted
        // shard endpoints.

        // 3: Wait for responses for all those sub-batches and keep track of the responses from
        // sub-batches based on the op index in the original bulkWrite command. Abort the batch upon
        // errors for ordered writes or transactions.
        // TODO(SERVER-72792): Remove the logic below that mimics ok responses and process real
        // batch responses.
        for (const auto& childBatch : childBatches) {
            bulkWriteOp.noteBatchResponse(*childBatch.second);
        }


        // 4: Refresh the targeter(s) if we receive a target error or a stale config/db error.
        if (bulkWriteOp.isFinished()) {
            // No need to refresh the targeters if we are done.
            break;
        }

        bool targeterChanged = false;
        try {
            LOGV2_DEBUG(7298200, 2, "Refreshing all targeters for bulkWrite");
            for (auto& targeter : targeters) {
                targeterChanged = targeter->refreshIfNeeded(opCtx);
            }
            LOGV2_DEBUG(7298201,
                        2,
                        "Successfully refreshed all targeters for bulkWrite",
                        "targeterChanged"_attr = targeterChanged);
        } catch (const ExceptionFor<ErrorCodes::StaleEpoch>& ex) {
            LOGV2_DEBUG(
                7298203,
                2,
                "Failed to refresh all targeters for bulkWrite because collection was dropped",
                "error"_attr = redact(ex));

            bulkWriteOp.abortBatch(
                ex.toStatus("collection was dropped in the middle of the operation"));
            break;
        } catch (const DBException& ex) {
            LOGV2_WARNING(7298204,
                          "Failed to refresh all targeters for bulkWrite",
                          "error"_attr = redact(ex));
        }

        int currCompletedOps = bulkWriteOp.numWriteOpsIn(WriteOpState_Completed);
        if (currCompletedOps == numCompletedOps && !targeterChanged) {
            ++numRoundsWithoutProgress;
        } else {
            numRoundsWithoutProgress = 0;
        }
        numCompletedOps = currCompletedOps;

        if (numRoundsWithoutProgress > kMaxRoundsWithoutProgress) {
            bulkWriteOp.abortBatch(
                {ErrorCodes::NoProgressMade,
                 str::stream() << "no progress was made executing bulkWrite ops in after "
                               << kMaxRoundsWithoutProgress << " rounds (" << numCompletedOps
                               << " ops completed in " << rounds << " rounds total)"});
            break;
        }
    }

    LOGV2_DEBUG(7263701, 4, "Finished execution of bulkWrite");
    return bulkWriteOp.generateReplyItems();
}

BulkWriteOp::BulkWriteOp(OperationContext* opCtx, const BulkWriteCommandRequest& clientRequest)
    : _opCtx(opCtx),
      _clientRequest(clientRequest),
      _txnNum(_opCtx->getTxnNumber()),
      _inTransaction(static_cast<bool>(TransactionRouter::get(opCtx))),
      _isRetryableWrite(opCtx->isRetryableWrite()) {
    _writeOps.reserve(_clientRequest.getOps().size());
    for (size_t i = 0; i < _clientRequest.getOps().size(); ++i) {
        _writeOps.emplace_back(BatchItemRef(&_clientRequest, i), _inTransaction);
    }
}

StatusWith<bool> BulkWriteOp::target(const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                                     bool recordTargetErrors,
                                     TargetedBatchMap& targetedBatches) {
    const auto ordered = _clientRequest.getOrdered();

    return targetWriteOps(
        _opCtx,
        _writeOps,
        ordered,
        recordTargetErrors,
        // getTargeterFn:
        [&](const WriteOp& writeOp) -> const NSTargeter& {
            const auto opIdx = writeOp.getWriteItem().getItemIndex();
            const auto& bulkWriteOp = BulkWriteCRUDOp(_clientRequest.getOps()[opIdx]);
            return *targeters[bulkWriteOp.getNsInfoIdx()];
        },
        // getWriteSizeFn:
        [&](const WriteOp& writeOp) {
            // TODO(SERVER-73536): Account for the size of the
            // outgoing request.
            return 1;
        },
        targetedBatches);
}

BulkWriteCommandRequest BulkWriteOp::buildBulkCommandRequest(
    const TargetedWriteBatch& targetedBatch) const {
    BulkWriteCommandRequest request;

    // TODO (SERVER-73281): Support update / delete operations on bulkWrite cmd on mongos.
    // A single bulk command request batch may contain operations of different
    // types, i.e. they may be inserts, updates or deletes.
    std::vector<
        stdx::variant<mongo::BulkWriteInsertOp, mongo::BulkWriteUpdateOp, mongo::BulkWriteDeleteOp>>
        ops;
    std::vector<NamespaceInfoEntry> nsInfo = _clientRequest.getNsInfo();

    for (auto&& targetedWrite : targetedBatch.getWrites()) {
        const WriteOpRef& writeOpRef = targetedWrite->writeOpRef;
        ops.push_back(_clientRequest.getOps().at(writeOpRef.first));

        // Set the nsInfo's shardVersion & databaseVersion fields based on the endpoint
        // of each operation. Since some operations may be on the same namespace, this
        // might result in the same nsInfo entry being written to multiple times. This
        // is OK, since we know that in a single batch, all operations on the same
        // namespace MUST have the same shardVersion & databaseVersion.
        // Invariant checks that either the shardVersion & databaseVersion in nsInfo are
        // null OR the new versions in the targetedWrite match the existing version in
        // nsInfo.
        const auto& bulkWriteOp = BulkWriteCRUDOp(ops.back());
        auto& nsInfoEntry = nsInfo.at(bulkWriteOp.getNsInfoIdx());

        invariant((!nsInfoEntry.getShardVersion() ||
                   nsInfoEntry.getShardVersion() == targetedWrite->endpoint.shardVersion) &&
                  (!nsInfoEntry.getDatabaseVersion() ||
                   nsInfoEntry.getDatabaseVersion() == targetedWrite->endpoint.databaseVersion));

        nsInfoEntry.setShardVersion(targetedWrite->endpoint.shardVersion);
        nsInfoEntry.setDatabaseVersion(targetedWrite->endpoint.databaseVersion);
    }

    request.setOps(ops);
    request.setNsInfo(nsInfo);

    // It isn't necessary to copy the cursor options over, because the cursor options
    // are for use in the interaction between the mongos and the client and not
    // internally between the mongos and the mongods.
    request.setOrdered(_clientRequest.getOrdered());
    request.setBypassDocumentValidation(_clientRequest.getBypassDocumentValidation());

    // TODO (SERVER-72989): Attach stmtIds etc. when building support for retryable
    // writes on mongos

    return request;
}

bool BulkWriteOp::isFinished() const {
    // TODO: Track ops lifetime.
    const bool ordered = _clientRequest.getOrdered();
    for (auto& writeOp : _writeOps) {
        if (writeOp.getWriteState() < WriteOpState_Completed) {
            return false;
        } else if (ordered && writeOp.getWriteState() == WriteOpState_Error) {
            return true;
        }
    }
    return true;
}

const WriteOp& BulkWriteOp::getWriteOp_forTest(int i) const {
    return _writeOps[i];
}

int BulkWriteOp::numWriteOpsIn(WriteOpState opState) const {
    return std::accumulate(
        _writeOps.begin(), _writeOps.end(), 0, [opState](int sum, const WriteOp& writeOp) {
            return sum + (writeOp.getWriteState() == opState ? 1 : 0);
        });
}

void BulkWriteOp::abortBatch(const Status& status) {
    dassert(!isFinished());
    dassert(numWriteOpsIn(WriteOpState_Pending) == 0);

    const auto ordered = _clientRequest.getOrdered();
    for (auto& writeOp : _writeOps) {
        if (writeOp.getWriteState() < WriteOpState_Completed) {
            const auto opIdx = writeOp.getWriteItem().getItemIndex();
            writeOp.setOpError(write_ops::WriteError(opIdx, status));

            // Only return the first error if we are ordered.
            if (ordered)
                break;
        }
    }

    dassert(isFinished());
}

// TODO(SERVER-72792): Finish this and process real batch responses.
void BulkWriteOp::noteBatchResponse(const TargetedWriteBatch& targetedBatch) {
    for (auto&& write : targetedBatch.getWrites()) {
        WriteOp& writeOp = _writeOps[write->writeOpRef.first];
        writeOp.noteWriteComplete(*write);
    }
}

std::vector<BulkWriteReplyItem> BulkWriteOp::generateReplyItems() const {
    dassert(isFinished());
    std::vector<BulkWriteReplyItem> replyItems;
    replyItems.reserve(_writeOps.size());

    const auto ordered = _clientRequest.getOrdered();
    for (auto& writeOp : _writeOps) {
        dassert(writeOp.getWriteState() != WriteOpState_Pending);
        if (writeOp.getWriteState() == WriteOpState_Completed) {
            replyItems.emplace_back(writeOp.getWriteItem().getItemIndex());
        } else if (writeOp.getWriteState() == WriteOpState_Error) {
            replyItems.emplace_back(writeOp.getWriteItem().getItemIndex(),
                                    writeOp.getOpError().getStatus());
            // Only return the first error if we are ordered.
            if (ordered)
                break;
        }
    }

    return replyItems;
}
}  // namespace bulk_write_exec

}  // namespace mongo
