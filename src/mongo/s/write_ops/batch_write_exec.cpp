
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/batch_write_exec.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/owned_pointer_map.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);

//
// Map which allows associating ConnectionString hosts with TargetedWriteBatches
// This is needed since the dispatcher only returns hosts with responses.
//

// TODO: Unordered map?
typedef OwnedPointerMap<ShardId, TargetedWriteBatch> OwnedShardBatchMap;

WriteErrorDetail errorFromStatus(const Status& status) {
    WriteErrorDetail error;
    error.setStatus(status);
    return error;
}

// Helper to note several stale errors from a response
void noteStaleResponses(const std::vector<ShardError>& staleErrors, NSTargeter* targeter) {
    for (const auto& error : staleErrors) {
        targeter->noteStaleResponse(
            error.endpoint,
            StaleConfigInfo::parseFromCommandError(
                error.error.isErrInfoSet() ? error.error.getErrInfo() : BSONObj()));
    }
}

// The number of times we'll try to continue a batch op if no progress is being made. This only
// applies when no writes are occurring and metadata is not changing on reload.
const int kMaxRoundsWithoutProgress(5);

}  // namespace

void BatchWriteExec::executeBatch(OperationContext* opCtx,
                                  NSTargeter& targeter,
                                  const BatchedCommandRequest& clientRequest,
                                  BatchedCommandResponse* clientResponse,
                                  BatchWriteExecStats* stats) {
    const auto& nss(clientRequest.getNS());

    LOG(4) << "Starting execution of write batch of size "
           << static_cast<int>(clientRequest.sizeWriteOps()) << " for " << nss.ns();

    BatchWriteOp batchOp(opCtx, clientRequest);

    // Current batch status
    bool refreshedTargeter = false;
    int rounds = 0;
    int numCompletedOps = 0;
    int numRoundsWithoutProgress = 0;

    while (!batchOp.isFinished()) {
        //
        // Get child batches to send using the targeter
        //
        // Targeting errors can be caused by remote metadata changing (the collection could have
        // been dropped and recreated, for example with a new shard key).  If a remote metadata
        // change occurs *before* a client sends us a batch, we need to make sure that we don't
        // error out just because we're staler than the client - otherwise mongos will be have
        // unpredictable behavior.
        //
        // (If a metadata change happens *during* or *after* a client sends us a batch, however,
        // we make no guarantees about delivery.)
        //
        // For this reason, we don't record targeting errors until we've refreshed our targeting
        // metadata at least once *after* receiving the client batch - at that point, we know:
        //
        // 1) our new metadata is the same as the metadata when the client sent a batch, and so
        //    targeting errors are real.
        // OR
        // 2) our new metadata is a newer version than when the client sent a batch, and so
        //    the metadata must have changed after the client batch was sent.  We don't need to
        //    deliver in this case, since for all the client knows we may have gotten the batch
        //    exactly when the metadata changed.
        //

        OwnedPointerMap<ShardId, TargetedWriteBatch> childBatchesOwned;
        std::map<ShardId, TargetedWriteBatch*>& childBatches = childBatchesOwned.mutableMap();

        // If we've already had a targeting error, we've refreshed the metadata once and can
        // record target errors definitively.
        bool recordTargetErrors = refreshedTargeter;
        Status targetStatus = batchOp.targetBatch(targeter, recordTargetErrors, &childBatches);
        if (!targetStatus.isOK()) {
            // Don't do anything until a targeter refresh
            targeter.noteCouldNotTarget();
            refreshedTargeter = true;
            ++stats->numTargetErrors;
            dassert(childBatches.size() == 0u);
        }

        //
        // Send all child batches
        //

        const size_t numToSend = childBatches.size();
        size_t numSent = 0;

        while (numSent != numToSend) {
            // Collect batches out on the network, mapped by endpoint
            OwnedShardBatchMap ownedPendingBatches;
            OwnedShardBatchMap::MapType& pendingBatches = ownedPendingBatches.mutableMap();

            //
            // Construct the requests.
            //

            std::vector<AsyncRequestsSender::Request> requests;

            // Get as many batches as we can at once
            for (auto& childBatch : childBatches) {
                TargetedWriteBatch* const nextBatch = childBatch.second;

                // If the batch is nullptr, we sent it previously, so skip
                if (!nextBatch)
                    continue;

                // If we already have a batch for this shard, wait until the next time
                const auto& targetShardId = nextBatch->getEndpoint().shardName;

                if (pendingBatches.count(targetShardId))
                    continue;

                stats->noteTargetedShard(targetShardId);

                const auto request = [&] {
                    const auto shardBatchRequest(batchOp.buildBatchRequest(*nextBatch));

                    BSONObjBuilder requestBuilder;
                    shardBatchRequest.serialize(&requestBuilder);

                    {
                        OperationSessionInfo sessionInfo;

                        if (opCtx->getLogicalSessionId()) {
                            sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
                        }

                        sessionInfo.setTxnNumber(opCtx->getTxnNumber());
                        sessionInfo.serialize(&requestBuilder);
                    }

                    return requestBuilder.obj();
                }();

                LOG(4) << "Sending write batch to " << targetShardId << ": " << redact(request);

                requests.emplace_back(targetShardId, request);

                // Indicate we're done by setting the batch to nullptr. We'll only get duplicate
                // hostEndpoints if we have broadcast and non-broadcast endpoints for the same host,
                // so this should be pretty efficient without moving stuff around.
                childBatch.second = nullptr;

                // Recv-side is responsible for cleaning up the nextBatch when used
                pendingBatches.emplace(targetShardId, nextBatch);
            }

            AsyncRequestsSender ars(opCtx,
                                    Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                    clientRequest.getTargetingNS().db().toString(),
                                    requests,
                                    kPrimaryOnlyReadPreference,
                                    opCtx->getTxnNumber() ? Shard::RetryPolicy::kIdempotent
                                                          : Shard::RetryPolicy::kNoRetry);
            numSent += pendingBatches.size();

            //
            // Receive the responses.
            //

            while (!ars.done()) {
                // Block until a response is available.
                auto response = ars.next();

                // Get the TargetedWriteBatch to find where to put the response
                dassert(pendingBatches.find(response.shardId) != pendingBatches.end());
                TargetedWriteBatch* batch = pendingBatches.find(response.shardId)->second;

                // First check if we were able to target a shard host.
                if (!response.shardHostAndPort) {
                    invariant(!response.swResponse.isOK());

                    // Record a resolve failure
                    batchOp.noteBatchError(*batch,
                                           errorFromStatus(response.swResponse.getStatus()));

                    // TODO: It may be necessary to refresh the cache if stale, or maybe just cancel
                    // and retarget the batch
                    LOG(4) << "Unable to send write batch to " << batch->getEndpoint().shardName
                           << causedBy(response.swResponse.getStatus());

                    // We're done with this batch. Clean up when we can't resolve a host.
                    auto it = childBatches.find(batch->getEndpoint().shardName);
                    invariant(it != childBatches.end());
                    delete it->second;
                    it->second = nullptr;
                    continue;
                }

                const auto shardHost(std::move(*response.shardHostAndPort));

                // Then check if we successfully got a response.
                Status responseStatus = response.swResponse.getStatus();
                BatchedCommandResponse batchedCommandResponse;
                if (responseStatus.isOK()) {
                    std::string errMsg;
                    if (!batchedCommandResponse.parseBSON(response.swResponse.getValue().data,
                                                          &errMsg) ||
                        !batchedCommandResponse.isValid(&errMsg)) {
                        responseStatus = {ErrorCodes::FailedToParse, errMsg};
                    }
                }

                if (responseStatus.isOK()) {
                    TrackedErrors trackedErrors;
                    trackedErrors.startTracking(ErrorCodes::StaleShardVersion);
                    trackedErrors.startTracking(ErrorCodes::CannotImplicitlyCreateCollection);

                    LOG(4) << "Write results received from " << shardHost.toString() << ": "
                           << redact(batchedCommandResponse.toString());

                    // Dispatch was ok, note response
                    batchOp.noteBatchResponse(*batch, batchedCommandResponse, &trackedErrors);

                    // Note if anything was stale
                    const auto& staleErrors =
                        trackedErrors.getErrors(ErrorCodes::StaleShardVersion);
                    if (!staleErrors.empty()) {
                        noteStaleResponses(staleErrors, &targeter);
                        ++stats->numStaleBatches;
                    }

                    const auto& cannotImplicitlyCreateErrors =
                        trackedErrors.getErrors(ErrorCodes::CannotImplicitlyCreateCollection);
                    if (!cannotImplicitlyCreateErrors.empty()) {
                        // This forces the chunk manager to reload so we can attach the correct
                        // version on retry and make sure we route to the correct shard.
                        targeter.noteCouldNotTarget();
                    }

                    // Remember that we successfully wrote to this shard
                    // NOTE: This will record lastOps for shards where we actually didn't update
                    // or delete any documents, which preserves old behavior but is conservative
                    stats->noteWriteAt(shardHost,
                                       batchedCommandResponse.isLastOpSet()
                                           ? batchedCommandResponse.getLastOp()
                                           : repl::OpTime(),
                                       batchedCommandResponse.isElectionIdSet()
                                           ? batchedCommandResponse.getElectionId()
                                           : OID());
                } else {
                    // Error occurred dispatching, note it
                    if (ErrorCodes::isShutdownError(responseStatus.code()) &&
                        globalInShutdownDeprecated()) {
                        // Throw an error since the mongos itself is shutting down so this should
                        // be a top level error instead of a write error.
                        uassertStatusOK(responseStatus);
                    }
                    const Status status = responseStatus.withContext(
                        str::stream() << "Write results unavailable from " << shardHost);

                    batchOp.noteBatchError(*batch, errorFromStatus(status));

                    LOG(4) << "Unable to receive write results from " << shardHost
                           << causedBy(redact(status));
                }
            }
        }

        ++rounds;
        ++stats->numRounds;

        // If we're done, get out
        if (batchOp.isFinished())
            break;

        // MORE WORK TO DO

        //
        // Refresh the targeter if we need to (no-op if nothing stale)
        //

        bool targeterChanged = false;
        Status refreshStatus = targeter.refreshIfNeeded(opCtx, &targeterChanged);

        if (!refreshStatus.isOK()) {
            // It's okay if we can't refresh, we'll just record errors for the ops if
            // needed.
            warning() << "could not refresh targeter" << causedBy(refreshStatus.reason());
        }

        //
        // Ensure progress is being made toward completing the batch op
        //

        int currCompletedOps = batchOp.numWriteOpsIn(WriteOpState_Completed);
        if (currCompletedOps == numCompletedOps && !targeterChanged) {
            ++numRoundsWithoutProgress;
        } else {
            numRoundsWithoutProgress = 0;
        }
        numCompletedOps = currCompletedOps;

        if (numRoundsWithoutProgress > kMaxRoundsWithoutProgress) {
            batchOp.abortBatch(errorFromStatus(
                {ErrorCodes::NoProgressMade,
                 str::stream() << "no progress was made executing batch write op in "
                               << clientRequest.getNS().ns()
                               << " after "
                               << kMaxRoundsWithoutProgress
                               << " rounds ("
                               << numCompletedOps
                               << " ops completed in "
                               << rounds
                               << " rounds total)"}));
            break;
        }
    }

    auto nShardsOwningChunks = batchOp.getNShardsOwningChunks();
    if (nShardsOwningChunks.is_initialized())
        stats->noteNumShardsOwningChunks(nShardsOwningChunks.get());

    batchOp.buildClientResponse(clientResponse);

    LOG(4) << "Finished execution of write batch"
           << (clientResponse->isErrDetailsSet() ? " with write errors" : "")
           << (clientResponse->isErrDetailsSet() && clientResponse->isWriteConcernErrorSet()
                   ? " and"
                   : "")
           << (clientResponse->isWriteConcernErrorSet() ? " with write concern error" : "")
           << " for " << clientRequest.getNS();
}

void BatchWriteExecStats::noteTargetedShard(const ShardId& shardId) {
    _targetedShards.insert(shardId);
}

void BatchWriteExecStats::noteWriteAt(const HostAndPort& host,
                                      repl::OpTime opTime,
                                      const OID& electionId) {
    _writeOpTimes[ConnectionString(host)] = HostOpTime(opTime, electionId);
}

void BatchWriteExecStats::noteNumShardsOwningChunks(const int nShardsOwningChunks) {
    _numShardsOwningChunks.emplace(nShardsOwningChunks);
}

const std::set<ShardId>& BatchWriteExecStats::getTargetedShards() const {
    return _targetedShards;
}

const HostOpTimeMap& BatchWriteExecStats::getWriteOpTimes() const {
    return _writeOpTimes;
}

const boost::optional<int> BatchWriteExecStats::getNumShardsOwningChunks() const {
    return _numShardsOwningChunks;
}

}  // namespace mongo
