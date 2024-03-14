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


#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/num_hosts_targeted_metrics.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);

// Helper to note several stale shard errors from a response
void noteStaleShardResponses(OperationContext* opCtx,
                             const std::vector<ShardError>& staleErrors,
                             NSTargeter* targeter) {
    for (const auto& error : staleErrors) {
        LOGV2_DEBUG(22902,
                    4,
                    "Noting stale config response",
                    "shardId"_attr = error.endpoint.shardName,
                    "status"_attr = error.error.getStatus());

        auto extraInfo = error.error.getStatus().extraInfo<StaleConfigInfo>();
        invariant(extraInfo);
        targeter->noteStaleShardResponse(opCtx, error.endpoint, *extraInfo);
    }
}

// Helper to note several stale db errors from a response
void noteStaleDbResponses(OperationContext* opCtx,
                          const std::vector<ShardError>& staleErrors,
                          NSTargeter* targeter) {
    for (const auto& error : staleErrors) {
        LOGV2_DEBUG(22903,
                    4,
                    "Noting stale database response",
                    "shardId"_attr = error.endpoint.shardName,
                    "status"_attr = error.error.getStatus());
        auto extraInfo = error.error.getStatus().extraInfo<StaleDbRoutingVersion>();
        invariant(extraInfo);
        targeter->noteStaleDbResponse(opCtx, error.endpoint, *extraInfo);
    }
}

void noteCannotImplicitlyCreateCollectionResponses(OperationContext* opCtx,
                                                   const std::vector<ShardError>& createErrors,
                                                   NSTargeter* targeter) {
    for (const auto& error : createErrors) {
        LOGV2_DEBUG(8037202,
                    4,
                    "Noting cannot implicitly create collection response",
                    "status"_attr = error.error.getStatus());
        auto extraInfo = error.error.getStatus().extraInfo<CannotImplicitlyCreateCollectionInfo>();
        invariant(extraInfo);
        targeter->noteCannotImplicitlyCreateCollectionResponse(opCtx, *extraInfo);
    }
}

bool hasTransientTransactionError(const BatchedCommandResponse& response) {
    if (!response.isErrorLabelsSet()) {
        return false;
    }

    const auto& errorLabels = response.getErrorLabels();
    auto iter = std::find_if(errorLabels.begin(), errorLabels.end(), [](const std::string& label) {
        return label == ErrorLabel::kTransientTransaction;
    });
    return iter != errorLabels.end();
}

// The number of times we'll try to continue a batch op if no progress is being made. This only
// applies when no writes are occurring and metadata is not changing on reload.
const int kMaxRoundsWithoutProgress(5);

// Helper to parse all of the childBatches and construct the proper requests to send using the
// AsyncRequestSender.
std::vector<AsyncRequestsSender::Request> constructARSRequestsToSend(
    OperationContext* opCtx,
    NSTargeter& targeter,
    TargetedBatchMap& childBatches,
    TargetedBatchMap& pendingBatches,
    BatchWriteExecStats* stats,
    BatchWriteOp& batchOp,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) {
    std::vector<AsyncRequestsSender::Request> requests;
    // Get as many batches as we can at once
    for (auto&& childBatch : childBatches) {
        auto nextBatch = std::move(childBatch.second);

        // If the batch is nullptr, we sent it previously, so skip
        if (!nextBatch)
            continue;

        // If we already have a batch for this shard, wait until the next time
        const auto& targetShardId = nextBatch->getShardId();
        if (pendingBatches.count(targetShardId))
            continue;

        stats->noteTargetedShard(targetShardId);

        const auto request = [&] {
            const auto shardBatchRequest(batchOp.buildBatchRequest(
                *nextBatch, targeter, allowShardKeyUpdatesWithoutFullShardKeyInQuery));

            BSONObjBuilder requestBuilder;
            shardBatchRequest.serialize(&requestBuilder);
            logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &requestBuilder);

            return requestBuilder.obj();
        }();

        LOGV2_DEBUG(22905,
                    4,
                    "Sending write batch",
                    "shardId"_attr = targetShardId,
                    "request"_attr = redact(request));

        requests.emplace_back(targetShardId, request);

        // Indicate we're done by setting the batch to nullptr. We'll only get duplicate
        // hostEndpoints if we have broadcast and non-broadcast endpoints for the same host,
        // so this should be pretty efficient without moving stuff around.
        childBatch.second = nullptr;

        // Recv-side is responsible for cleaning up the nextBatch when used
        pendingBatches.emplace(targetShardId, std::move(nextBatch));
    }
    return requests;
}

// If the network response is OK then we process the response from the resmote shard. The returned
// boolean dictates if we should abort rest of the batch.
bool processResponseFromRemote(OperationContext* opCtx,
                               NSTargeter& targeter,
                               const ShardId& shardInfo,
                               const BatchedCommandResponse& batchedCommandResponse,
                               BatchWriteOp& batchOp,
                               TargetedWriteBatch* batch,
                               BatchWriteExecStats* stats) {
    // Stale routing info errors need to be tracked in order to trigger a refresh of the targeter.
    // On the other hand, errors caused by the catalog cache being temporarily unavailable (such as
    // ShardCannotRefreshDueToLocksHeld) are ignored in this context, since no deduction can be made
    // around possible placement changes.
    TrackedErrors trackedErrors;
    trackedErrors.startTracking(ErrorCodes::StaleConfig);
    trackedErrors.startTracking(ErrorCodes::StaleDbVersion);
    trackedErrors.startTracking(ErrorCodes::TenantMigrationAborted);
    trackedErrors.startTracking(ErrorCodes::CannotImplicitlyCreateCollection);

    LOGV2_DEBUG(22907,
                4,
                "Write results received",
                "shardInfo"_attr = shardInfo,
                "status"_attr = redact(batchedCommandResponse.toStatus()));

    // Dispatch was ok, note response
    batchOp.noteBatchResponse(*batch, batchedCommandResponse, &trackedErrors);

    // If we are in a transaction, we must fail the whole batch on any error.
    if (TransactionRouter::get(opCtx)) {
        // Note: this returns a bad status if any part of the batch failed.
        auto batchStatus = batchedCommandResponse.toStatus();
        if (!batchStatus.isOK() && batchStatus != ErrorCodes::WouldChangeOwningShard) {
            auto newStatus = batchStatus.withContext(
                str::stream() << "Encountered error from " << shardInfo << " during a transaction");

            // Throw when there is a transient transaction error since this
            // should be a top level error and not just a write error.
            if (hasTransientTransactionError(batchedCommandResponse)) {
                uassertStatusOK(newStatus);
            }

            return true;
        }
    }

    // Note if anything was stale
    auto staleConfigErrors = trackedErrors.getErrors(ErrorCodes::StaleConfig);
    const auto& staleDbErrors = trackedErrors.getErrors(ErrorCodes::StaleDbVersion);
    const auto& tenantMigrationAbortedErrors =
        trackedErrors.getErrors(ErrorCodes::TenantMigrationAborted);
    const auto& cannotImplicitlyCreateCollectionErrors =
        trackedErrors.getErrors(ErrorCodes::CannotImplicitlyCreateCollection);

    if (!staleConfigErrors.empty()) {
        invariant(staleDbErrors.empty());
        noteStaleShardResponses(opCtx, staleConfigErrors, &targeter);
        ++stats->numStaleShardBatches;
    }

    if (!staleDbErrors.empty()) {
        invariant(staleConfigErrors.empty());
        noteStaleDbResponses(opCtx, staleDbErrors, &targeter);
        ++stats->numStaleDbBatches;
    }

    if (!tenantMigrationAbortedErrors.empty()) {
        ++stats->numTenantMigrationAbortedErrors;
    }

    if (!cannotImplicitlyCreateCollectionErrors.empty()) {
        noteCannotImplicitlyCreateCollectionResponses(
            opCtx, cannotImplicitlyCreateCollectionErrors, &targeter);
    }

    return false;
}

// If the local process experiences an error trying to get the response from remote, process the
// error. The returned boolean dictates if we should abort the rest of the batch.
bool processErrorResponseFromLocal(OperationContext* opCtx,
                                   BatchWriteOp& batchOp,
                                   TargetedWriteBatch* batch,
                                   Status responseStatus,
                                   const ShardId& shardInfo,
                                   boost::optional<HostAndPort> shardHostAndPort) {
    if ((ErrorCodes::isShutdownError(responseStatus) ||
         responseStatus == ErrorCodes::CallbackCanceled) &&
        globalInShutdownDeprecated()) {
        // Throw an error since the mongos itself is shutting down so this should
        // be a top level error instead of a write error.
        uassertStatusOK(responseStatus);
    }

    // Error occurred dispatching, note it
    const Status status = responseStatus.withContext(
        str::stream() << "Write results unavailable "
                      << (shardHostAndPort ? "from "
                                           : "from failing to target a host in the shard ")
                      << shardInfo);

    batchOp.noteBatchError(*batch, write_ops::WriteError(0, status));

    LOGV2_DEBUG(22908,
                4,
                "Unable to receive write results",
                "shardInfo"_attr = shardInfo,
                "error"_attr = redact(status));

    // If we are in a transaction, we must stop immediately (even for unordered).
    if (TransactionRouter::get(opCtx)) {
        // Throw when there is a transient transaction error since this should be a
        // top level error and not just a write error.
        if (isTransientTransactionError(status.code(), false, false)) {
            uassertStatusOK(status);
        }

        return true;
    }

    return false;
}

// Iterates through all of the child batches and sends and processes each batch.
void executeChildBatches(OperationContext* opCtx,
                         NSTargeter& targeter,
                         const BatchedCommandRequest& clientRequest,
                         TargetedBatchMap& childBatches,
                         BatchWriteExecStats* stats,
                         BatchWriteOp& batchOp,
                         bool& abortBatch,
                         boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) {
    const size_t numToSend = childBatches.size();
    size_t numSent = 0;

    while (numSent != numToSend) {
        // Collect batches out on the network, mapped by endpoint
        TargetedBatchMap pendingBatches;

        auto requests = constructARSRequestsToSend(opCtx,
                                                   targeter,
                                                   childBatches,
                                                   pendingBatches,
                                                   stats,
                                                   batchOp,
                                                   allowShardKeyUpdatesWithoutFullShardKeyInQuery);
        bool isRetryableWriteNotInInternalTxn =
            opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);
        // Send the requests.
        MultiStatementTransactionRequestsSender ars(
            opCtx,
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
            clientRequest.getNS().dbName(),
            requests,
            kPrimaryOnlyReadPreference,
            isRetryableWriteNotInInternalTxn ? Shard::RetryPolicy::kIdempotent
                                             : Shard::RetryPolicy::kNoRetry);
        numSent += pendingBatches.size();

        std::vector<BatchedCommandResponse> batchResponses;
        batchResponses.reserve(pendingBatches.size());
        // Receive all of the responses.
        while (!ars.done()) {
            // Block until a response is available.
            auto response = ars.next();
            // Get the TargetedWriteBatch to find where to put the response
            dassert(pendingBatches.find(response.shardId) != pendingBatches.end());
            TargetedWriteBatch* batch = pendingBatches.find(response.shardId)->second.get();
            const auto shardInfo = response.shardHostAndPort ? response.shardHostAndPort->toString()
                                                             : batch->getShardId();

            // Then check if we successfully got a response.
            Status responseStatus = response.swResponse.getStatus();
            batchResponses.emplace_back();
            if (responseStatus.isOK()) {
                std::string errMsg;
                if (!batchResponses.back().parseBSON(response.swResponse.getValue().data,
                                                     &errMsg)) {
                    responseStatus = {ErrorCodes::FailedToParse, errMsg};
                }
            }

            if (responseStatus.isOK()) {
                if ((abortBatch = processResponseFromRemote(opCtx,
                                                            targeter,
                                                            shardInfo,
                                                            batchResponses.back(),
                                                            batchOp,
                                                            batch,
                                                            stats))) {
                    break;
                }
            } else {
                // The ARS failed to retrieve the response due to some sort of local failure.
                if ((abortBatch = processErrorResponseFromLocal(opCtx,
                                                                batchOp,
                                                                batch,
                                                                responseStatus,
                                                                shardInfo,
                                                                response.shardHostAndPort))) {
                    break;
                }
            }
        }
        batchOp.handleDeferredResponses(targeter.hasStaleShardResponse());
        batchOp.handleDeferredWriteConcernErrors();
    }
}

// Only processes one write response from the child batches. Currently this is used for the two
// phase protocol of the singleton writes without shard key and time-series retryable updates.
void processResponseForOnlyFirstBatch(OperationContext* opCtx,
                                      NSTargeter& targeter,
                                      BatchWriteOp& batchOp,
                                      TargetedBatchMap& childBatches,
                                      const BatchedCommandResponse& batchedCommandResponse,
                                      const Status& responseStatus,
                                      BatchWriteExecStats* stats,
                                      bool& abortBatch) {
    // Since we only send the write to a single shard, record the response of the write against the
    // first TargetedWriteBatch, and record no-ops for the remaining targeted shards. We always
    // resolve the first batch due to a quirk of this protocol running within an internal
    // transaction. This is because StaleShardVersion errors are automatically retried within the
    // internal transaction, and if there happened to be a moveChunk that changes the number of
    // targetable shards, the two phase protocol would still complete successfully, but the
    // childBatches here could potentially still only include targetedWrites for the previous subset
    // of shards before the moveChunk (since the StaleShardVersion error was not made visible here).
    // It isn't important which TargetedWriteBatch records the write response, just as long as only
    // one does so that the response on the client is still valid.
    bool hasRecordedWriteResponseForFirstBatch = false;
    for (auto&& childBatch : childBatches) {
        auto nextBatch = std::move(childBatch.second);

        // We expect that each TargetedWriteBatch should only contain 1 write op for each shard.
        invariant(nextBatch->getWrites().size() == 1);

        if (responseStatus.isOK()) {
            if (!hasRecordedWriteResponseForFirstBatch) {
                // Resolve the first child batch with the response of the write or a no-op response
                // if there was no matching document.
                if ((abortBatch = processResponseFromRemote(opCtx,
                                                            targeter,
                                                            nextBatch.get()->getShardId(),
                                                            batchedCommandResponse,
                                                            batchOp,
                                                            nextBatch.get(),
                                                            stats))) {
                    break;
                }
                hasRecordedWriteResponseForFirstBatch = true;
            } else {
                // Explicitly set the status so that debug builds won't invariant when checking the
                // status.
                BatchedCommandResponse noopBatchCommandResponse;
                noopBatchCommandResponse.setStatus(Status::OK());
                processResponseFromRemote(opCtx,
                                          targeter,
                                          nextBatch->getShardId(),
                                          noopBatchCommandResponse,
                                          batchOp,
                                          nextBatch.get(),
                                          stats);
            }
        } else {
            // The ARS failed to retrieve the response due to some sort of local failure.
            if ((abortBatch = processErrorResponseFromLocal(opCtx,
                                                            batchOp,
                                                            nextBatch.get(),
                                                            responseStatus,
                                                            nextBatch->getShardId(),
                                                            boost::none))) {
                break;
            }
        }
    }
}

void executeTwoPhaseWrite(OperationContext* opCtx,
                          NSTargeter& targeter,
                          BatchWriteOp& batchOp,
                          TargetedBatchMap& childBatches,
                          BatchWriteExecStats* stats,
                          const BatchedCommandRequest& clientRequest,
                          bool& abortBatch,
                          bool allowShardKeyUpdatesWithoutFullShardKeyInQuery) {
    const auto targetedWriteBatch = [&] {
        // If there is a targeted write with a sampleId, use that write instead in order to pass the
        // sampleId to the two phase write protocol. Otherwise, just choose the first targeted
        // write.
        for (auto&& [_ /* shardId */, childBatch] : childBatches) {
            auto nextBatch = childBatch.get();

            // For a write without shard key, we expect each TargetedWriteBatch in childBatches to
            // contain only one TargetedWrite directed to each shard.
            tassert(7208400,
                    "There must be only 1 targeted write in this targeted write batch.",
                    !nextBatch->getWrites().empty());

            auto targetedWrite = nextBatch->getWrites().begin()->get();
            if (targetedWrite->sampleId) {
                return nextBatch;
            }
        }
        return childBatches.begin()->second.get();
    }();

    auto cmdObj = batchOp
                      .buildBatchRequest(*targetedWriteBatch,
                                         targeter,
                                         allowShardKeyUpdatesWithoutFullShardKeyInQuery)
                      .toBSON();

    auto swRes = write_without_shard_key::runTwoPhaseWriteProtocol(
        opCtx, targeter.getNS(), std::move(cmdObj));

    Status responseStatus = swRes.getStatus();
    BatchedCommandResponse batchedCommandResponse;
    if (swRes.isOK()) {
        // Explicitly set the status of a no-op if there is no response.
        if (swRes.getValue().getResponse().isEmpty()) {
            batchedCommandResponse.setStatus(Status::OK());
        } else {
            std::string errMsg;
            if (!batchedCommandResponse.parseBSON(swRes.getValue().getResponse(), &errMsg)) {
                responseStatus = {ErrorCodes::FailedToParse, errMsg};
            }
        }
    }

    processResponseForOnlyFirstBatch(opCtx,
                                     targeter,
                                     batchOp,
                                     childBatches,
                                     batchedCommandResponse,
                                     responseStatus,
                                     stats,
                                     abortBatch);
}

void executeRetryableTimeseriesUpdate(OperationContext* opCtx,
                                      NSTargeter& targeter,
                                      BatchWriteOp& batchOp,
                                      TargetedBatchMap& childBatches,
                                      BatchWriteExecStats* stats,
                                      const BatchedCommandRequest& clientRequest,
                                      bool& abortBatch) {
    auto getIndex = [](const TargetedWriteBatch& batch) {
        const auto& writes = batch.getWrites();
        invariant(writes.size() == 1, std::to_string(writes.size()));
        return writes.front()->writeOpRef.first;
    };

    invariant(!childBatches.empty());
    auto index = getIndex(*childBatches.begin()->second);

    if constexpr (kDebugBuild) {
        if (childBatches.size() > 1) {
            // All child batches should be from the same update statement, which means they should
            // have the same index.
            for (auto&& [shardId, batch] : childBatches) {
                invariant(getIndex(*batch) == index);
            }
        }
    }

    auto wholeOp = clientRequest.getUpdateRequest();
    auto singleUpdateOp = timeseries::buildSingleUpdateOp(wholeOp, index);
    BatchedCommandRequest singleUpdateRequest(singleUpdateOp);
    const auto stmtId = write_ops::getStmtIdForWriteAt(wholeOp, index);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    txn_api::SyncTransactionWithRetries txn(
        opCtx, executor, nullptr /* resourceYielder */, inlineExecutor);
    BatchedCommandResponse batchedCommandResponse;
    auto swResult =
        txn.runNoThrow(opCtx,
                       [&singleUpdateRequest, &batchedCommandResponse, stmtId](
                           const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                           auto updateResponse =
                               txnClient.runCRUDOpSync(singleUpdateRequest, {stmtId});
                           batchedCommandResponse = std::move(updateResponse);

                           return SemiFuture<void>::makeReady();
                       });
    Status responseStatus = Status::OK();
    if (!swResult.isOK()) {
        responseStatus = swResult.getStatus();
    } else {
        if (!swResult.getValue().cmdStatus.isOK()) {
            responseStatus = swResult.getValue().cmdStatus;
        }
        if (auto wcError = swResult.getValue().wcError; !wcError.toStatus().isOK()) {
            batchedCommandResponse.setWriteConcernError(
                std::make_unique<WriteConcernErrorDetail>(wcError).release());
        }
    }

    processResponseForOnlyFirstBatch(opCtx,
                                     targeter,
                                     batchOp,
                                     childBatches,
                                     batchedCommandResponse,
                                     responseStatus,
                                     stats,
                                     abortBatch);
}

void executeTwoPhaseOrTimeSeriesWriteChildBatches(OperationContext* opCtx,
                                                  NSTargeter& targeter,
                                                  BatchWriteOp& batchOp,
                                                  TargetedBatchMap& childBatches,
                                                  BatchWriteExecStats* stats,
                                                  const BatchedCommandRequest& clientRequest,
                                                  bool& abortBatch,
                                                  WriteType writeType) {
    switch (writeType) {
        case WriteType::WithoutShardKeyOrId: {
            // If the WriteType is 'WithoutShardKeyOrId', then we have detected an
            // updateOne/deleteOne request without a shard key or _id. We will use a two phase
            // protocol to apply the write.
            tassert(6992000, "Executing write batches with a size of 0", childBatches.size() > 0u);

            auto allowShardKeyUpdatesWithoutFullShardKeyInQuery =
                opCtx->isRetryableWrite() || opCtx->inMultiDocumentTransaction();

            // If there is only 1 targetable shard, we can skip using the two phase write
            // protocol.
            if (targeter.getNShardsOwningChunks() == 1) {
                executeChildBatches(opCtx,
                                    targeter,
                                    clientRequest,
                                    childBatches,
                                    stats,
                                    batchOp,
                                    abortBatch,
                                    allowShardKeyUpdatesWithoutFullShardKeyInQuery);
            } else {
                // Execute the two phase write protocol for writes that cannot directly target a
                // shard. If there are any transaction errors, 'abortBatch' will be set.
                executeTwoPhaseWrite(opCtx,
                                     targeter,
                                     batchOp,
                                     childBatches,
                                     stats,
                                     clientRequest,
                                     abortBatch,
                                     allowShardKeyUpdatesWithoutFullShardKeyInQuery);
            }
            break;
        }
        case WriteType::TimeseriesRetryableUpdate:
            // If the WriteType is 'TimeseriesRetryableUpdate', then we have detected
            // a retryable time-series update request. We will run it in the internal
            // transaction api and collect the response.
            executeRetryableTimeseriesUpdate(
                opCtx, targeter, batchOp, childBatches, stats, clientRequest, abortBatch);
            break;
        default:
            MONGO_UNREACHABLE
    }
}
}  // namespace

void BatchWriteExec::executeBatch(OperationContext* opCtx,
                                  NSTargeter& targeter,
                                  const BatchedCommandRequest& clientRequest,
                                  BatchedCommandResponse* clientResponse,
                                  BatchWriteExecStats* stats) {
    const auto& nss(targeter.getNS());

    LOGV2_DEBUG(22904,
                4,
                "Starting execution of a write batch",
                logAttrs(nss),
                "size"_attr = clientRequest.sizeWriteOps());

    BatchWriteOp batchOp(opCtx, clientRequest);

    // Current batch status
    bool refreshedTargeter = false;
    int rounds = 0;
    int numCompletedOps = 0;
    int numRoundsWithoutProgress = 0;
    bool abortBatch = false;

    while (!batchOp.isFinished() && !abortBatch) {
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

        TargetedBatchMap childBatches;

        // If we've already had a targeting error, we've refreshed the metadata once and can
        // record target errors definitively.
        bool recordTargetErrors = refreshedTargeter;
        auto statusWithWriteType = batchOp.targetBatch(targeter, recordTargetErrors, &childBatches);
        if (!statusWithWriteType.isOK()) {
            // Don't do anything until a targeter refresh
            targeter.noteCouldNotTarget();
            refreshedTargeter = true;
            dassert(childBatches.size() == 0u);

            if (TransactionRouter::get(opCtx)) {
                // Throw when there is a transient transaction error since this should be a top
                // level error and not just a write error.
                if (isTransientTransactionError(statusWithWriteType.getStatus().code(),
                                                false /* hasWriteConcernError */,
                                                false /* isCommitOrAbort */)) {
                    uassertStatusOK(statusWithWriteType);
                }

                break;
            }
        } else {
            if (statusWithWriteType.getValue() == WriteType::Ordinary ||
                statusWithWriteType.getValue() == WriteType::WithoutShardKeyWithId) {
                // Tries to execute all of the child batches. If there are any transaction errors,
                // 'abortBatch' will be set.
                executeChildBatches(
                    opCtx,
                    targeter,
                    clientRequest,
                    childBatches,
                    stats,
                    batchOp,
                    abortBatch,
                    boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */);
            } else {
                executeTwoPhaseOrTimeSeriesWriteChildBatches(opCtx,
                                                             targeter,
                                                             batchOp,
                                                             childBatches,
                                                             stats,
                                                             clientRequest,
                                                             abortBatch,
                                                             statusWithWriteType.getValue());
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
        try {
            targeterChanged = targeter.createCollectionIfNeeded(opCtx);
            LOGV2_DEBUG_OPTIONS(4817406,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Starting post-migration commit refresh on the router");
            targeterChanged |= targeter.refreshIfNeeded(opCtx);
            LOGV2_DEBUG_OPTIONS(4817407,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Finished post-migration commit refresh on the router");
        } catch (const ExceptionFor<ErrorCodes::StaleEpoch>& ex) {
            LOGV2_DEBUG_OPTIONS(4817408,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Finished post-migration commit refresh on the router with error",
                                "error"_attr = redact(ex));
            batchOp.abortBatch(write_ops::WriteError(
                0, ex.toStatus("collection was dropped in the middle of the operation")));
            break;
        } catch (const DBException& ex) {
            LOGV2_DEBUG_OPTIONS(4817409,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Finished post-migration commit refresh on the router with error",
                                "error"_attr = redact(ex));

            // It's okay if we can't refresh, we'll just record errors for the ops if needed
            LOGV2_WARNING(22911, "Could not refresh targeter", "error"_attr = redact(ex));
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
            batchOp.abortBatch(write_ops::WriteError(
                0,
                {ErrorCodes::NoProgressMade,
                 str::stream() << "no progress was made executing batch write op in "
                               << clientRequest.getNS().toStringForErrorMsg() << " after "
                               << kMaxRoundsWithoutProgress << " rounds (" << numCompletedOps
                               << " ops completed in " << rounds << " rounds total)"}));
            break;
        }
    }

    auto nShardsOwningChunks = batchOp.getNShardsOwningChunks();
    if (nShardsOwningChunks)
        stats->noteNumShardsOwningChunks(*nShardsOwningChunks);

    stats->noteTargetedCollectionIsSharded(targeter.isTargetedCollectionSharded());

    batchOp.buildClientResponse(clientResponse);

    LOGV2_DEBUG(22910,
                4,
                "Finished execution of write batch",
                "succeededOrFailed"_attr =
                    (clientResponse->isErrDetailsSet() ? "failed" : "succeeded"),
                "wcSucceededOrFailed"_attr =
                    (clientResponse->isWriteConcernErrorSet() ? "failed" : "succeeded"),
                logAttrs(clientRequest.getNS()));
}

void BatchWriteExecStats::noteTargetedShard(const ShardId& shardId) {
    _targetedShards.insert(shardId);
}

void BatchWriteExecStats::noteNumShardsOwningChunks(const int nShardsOwningChunks) {
    _numShardsOwningChunks.emplace(nShardsOwningChunks);
}

void BatchWriteExecStats::noteTargetedCollectionIsSharded(bool isSharded) {
    _hasTargetedShardedCollection = isSharded;
}

const std::set<ShardId>& BatchWriteExecStats::getTargetedShards() const {
    return _targetedShards;
}

boost::optional<int> BatchWriteExecStats::getNumShardsOwningChunks() const {
    return _numShardsOwningChunks;
}

bool BatchWriteExecStats::hasTargetedShardedCollection() const {
    return _hasTargetedShardedCollection;
}

void updateHostsTargetedMetrics(OperationContext* opCtx,
                                BatchedCommandRequest::BatchType batchType,
                                int nShardsOwningChunks,
                                int nShardsTargeted,
                                bool isSharded) {
    NumHostsTargetedMetrics::QueryType writeType;
    switch (batchType) {
        case BatchedCommandRequest::BatchType_Insert:
            writeType = NumHostsTargetedMetrics::QueryType::kInsertCmd;
            break;
        case BatchedCommandRequest::BatchType_Update:
            writeType = NumHostsTargetedMetrics::QueryType::kUpdateCmd;
            break;
        case BatchedCommandRequest::BatchType_Delete:
            writeType = NumHostsTargetedMetrics::QueryType::kDeleteCmd;
            break;

            MONGO_UNREACHABLE;
    }

    auto targetType = NumHostsTargetedMetrics::get(opCtx).parseTargetType(
        opCtx, nShardsTargeted, nShardsOwningChunks, isSharded);
    NumHostsTargetedMetrics::get(opCtx).addNumHostsTargeted(writeType, targetType);
}

}  // namespace mongo
