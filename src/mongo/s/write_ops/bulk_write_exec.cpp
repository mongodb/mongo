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
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/commands/bulk_write_parser.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace bulk_write_exec {
namespace {

// The number of times we'll try to continue a batch op if no progress is being made. This only
// applies when no writes are occurring and metadata is not changing on reload.
const int kMaxRoundsWithoutProgress(5);

// Send and process the child batches. Each child batch is targeted at a unique shard: therefore
// one shard will have only one batch incoming.
void executeChildBatches(OperationContext* opCtx,
                         TargetedBatchMap& childBatches,
                         BulkWriteOp& bulkWriteOp,
                         stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (auto& childBatch : childBatches) {
        auto request = [&]() {
            auto bulkReq = bulkWriteOp.buildBulkCommandRequest(*childBatch.second);

            // Transform the request into a sendable BSON.
            BSONObjBuilder builder;
            bulkReq.serialize(BSONObj(), &builder);

            logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
            builder.append(WriteConcernOptions::kWriteConcernField,
                           opCtx->getWriteConcern().toBSON());

            return builder.obj();
        }();

        requests.emplace_back(childBatch.first, request);
    }

    bool isRetryableWrite = opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);

    // Use MultiStatementTransactionRequestsSender to send any ready sub-batches to targeted
    // shard endpoints. Requests are sent on construction.
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        DatabaseName::kAdmin,
        requests,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        isRetryableWrite ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kNoRetry);

    while (!ars.done()) {
        // Block until a response is available.
        auto response = ars.next();

        Status responseStatus = response.swResponse.getStatus();
        // TODO (SERVER-76957): The status may not be OK, handle it.
        invariant(responseStatus.isOK());

        auto bwReply = BulkWriteCommandReply::parse(IDLParserContext("bulkWrite"),
                                                    response.swResponse.getValue().data);

        // TODO (SERVER-76958): Iterate through the cursor rather than looking only at the
        // first batch.
        auto cursor = bwReply.getCursor();
        const auto& replyItems = cursor.getFirstBatch();
        TargetedWriteBatch* writeBatch = childBatches.find(response.shardId)->second.get();

        // Capture the errors if any exist and mark the writes in the TargetedWriteBatch so that
        // they may be re-targeted if needed.
        bulkWriteOp.noteBatchResponse(*writeBatch, replyItems, errorsPerNamespace);
    }
}

void noteStaleResponses(
    OperationContext* opCtx,
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    const stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace) {
    for (auto& targeter : targeters) {
        auto errors = errorsPerNamespace.find(targeter->getNS());
        if (errors != errorsPerNamespace.cend()) {
            for (const auto& error : errors->second.getErrors(ErrorCodes::StaleConfig)) {
                LOGV2_DEBUG(7279201,
                            4,
                            "Noting stale config response.",
                            "shardId"_attr = error.endpoint.shardName,
                            "status"_attr = error.error.getStatus());
                targeter->noteStaleShardResponse(
                    opCtx, error.endpoint, *error.error.getStatus().extraInfo<StaleConfigInfo>());
            }
            for (const auto& error : errors->second.getErrors(ErrorCodes::StaleDbVersion)) {
                LOGV2_DEBUG(7279202,
                            4,
                            "Noting stale database response.",
                            "shardId"_attr = error.endpoint.shardName,
                            "status"_attr = error.error.getStatus());
                targeter->noteStaleDbResponse(
                    opCtx,
                    error.endpoint,
                    *error.error.getStatus().extraInfo<StaleDbRoutingVersion>());
            }
        }
    }
}

}  // namespace

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

        // Divide and group ("target") the operations in the bulk write command. Some operations may
        // be split up (such as an update that needs to go to more than one shard), while others may
        // be grouped together if they need to go to the same shard.
        // These operations are grouped by shardId in the TargetedBatchMap childBatches.
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
        } else {
            stdx::unordered_map<NamespaceString, TrackedErrors> errorsPerNamespace;

            // Send the child batches and wait for responses.
            executeChildBatches(opCtx, childBatches, bulkWriteOp, errorsPerNamespace);

            // If we saw any staleness errors, tell the targeters to invalidate their cache
            // so that they may be refreshed.
            noteStaleResponses(opCtx, targeters, errorsPerNamespace);
        }

        if (bulkWriteOp.isFinished()) {
            // No need to refresh the targeters if we are done.
            break;
        }

        // Refresh the targeter(s) if we received a target error or a stale config/db error.
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
      _writeConcern(opCtx->getWriteConcern()),
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

    request.setDbName(DatabaseName::kAdmin);

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

void BulkWriteOp::noteBatchResponse(
    TargetedWriteBatch& targetedBatch,
    const std::vector<BulkWriteReplyItem>& replyItems,
    stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace) {
    LOGV2_DEBUG(7279200,
                4,
                "Processing bulk write response from shard.",
                "shard"_attr = targetedBatch.getShardId(),
                "replyItems"_attr = replyItems);
    int index = -1;
    bool ordered = _clientRequest.getOrdered();
    boost::optional<write_ops::WriteError> lastError;
    for (const auto& write : targetedBatch.getWrites()) {
        ++index;
        WriteOp& writeOp = _writeOps[write->writeOpRef.first];
        // TODO (SERVER-76953) : Handle unordered operations
        // When an error is encountered on an ordered bulk write, it is impossible for any of the
        // remaining operations to have been executed. For that reason we cancel them here so they
        // may be retargeted and retried.
        if (ordered && lastError) {
            invariant(index >= (int)replyItems.size());
            writeOp.cancelWrites(&*lastError);
            continue;
        }

        auto& reply = replyItems[index];

        if (reply.getStatus().isOK()) {
            writeOp.noteWriteComplete(*write);
        } else {
            lastError.emplace(reply.getIdx(), reply.getStatus());
            writeOp.noteWriteError(*write, *lastError);

            auto origWrite = BulkWriteCRUDOp(_clientRequest.getOps()[write->writeOpRef.first]);
            auto nss = _clientRequest.getNsInfo()[origWrite.getNsInfoIdx()].getNs();

            if (errorsPerNamespace.find(nss) == errorsPerNamespace.end()) {
                TrackedErrors trackedErrors;
                trackedErrors.startTracking(ErrorCodes::StaleConfig);
                trackedErrors.startTracking(ErrorCodes::StaleDbVersion);
                errorsPerNamespace.emplace(nss, trackedErrors);
            }

            auto trackedErrors = errorsPerNamespace.find(nss);
            invariant(trackedErrors != errorsPerNamespace.end());
            if (trackedErrors->second.isTracking(reply.getStatus().code())) {
                trackedErrors->second.addError(ShardError(write->endpoint, *lastError));
            }
        }
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
