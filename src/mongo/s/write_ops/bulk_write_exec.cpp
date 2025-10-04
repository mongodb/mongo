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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_common.h"
#include "mongo/db/commands/query_cmd/bulk_write_crud_op.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_parser.h"
#include "mongo/db/database_name.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/cannot_implicitly_create_collection_info.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_server_params_gen.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/commands/query_cmd/populate_cursor.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/coordinate_multi_update_util.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
#include <variant>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace bulk_write_exec {
namespace {

// The number of times we'll try to continue a batch op if no progress is being made. This only
// applies when no writes are occurring and metadata is not changing on reload.
const int kMaxRoundsWithoutProgress(5);

/**
 * Send and process the child batches. Each child batch is targeted at a unique shard: therefore one
 * shard will have only one batch incoming.
 */
void executeChildBatches(
    OperationContext* opCtx,
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    TargetedBatchMap& childBatches,
    BulkWriteOp& bulkWriteOp,
    stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery = boost::none) {
    // We are starting a new round of execution and so this should have been reset to false.
    invariant(!bulkWriteOp.shouldStopCurrentRound());
    std::vector<AsyncRequestsSender::Request> requests;
    for (auto& childBatch : childBatches) {
        bulkWriteOp.noteTargetedShard(*childBatch.second);

        auto request = [&]() {
            auto bulkReq = bulkWriteOp.buildBulkCommandRequest(
                targeters, *childBatch.second, allowShardKeyUpdatesWithoutFullShardKeyInQuery);

            // Transform the request into a sendable BSON.
            BSONObjBuilder builder;
            bulkReq.serialize(&builder);

            logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
            if (!TransactionRouter::get(opCtx)) {
                builder.append(WriteConcernOptions::kWriteConcernField,
                               opCtx->getWriteConcern().toBSON());
            }

            auto obj = builder.obj();

            // When running a debug build, verify that estSize is at least the BSON
            // serialization size.
            //
            // The estimated size doesn't take into account the size of the internal
            // '_allowShardKeyUpdatesWithoutFullShardKeyInQuery' field for updates. When
            // allowShardKeyUpdatesWithoutFullShardKeyInQuery is set, we are running a single
            // updateOne without shard key in its own child batch. So it doesn't matter what the
            // estimated size is, skip the debug check.
            dassert(allowShardKeyUpdatesWithoutFullShardKeyInQuery ||
                    childBatch.second->getEstimatedSizeBytes() >= obj.objsize());

            return obj;
        }();

        requests.emplace_back(childBatch.first, request);
    }

    // Note we check this rather than `isRetryableWrite()` because we do not want to retry
    // commands within retryable internal transactions.
    bool shouldRetry = opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);

    // Use MultiStatementTransactionRequestsSender to send any ready sub-batches to targeted
    // shard endpoints. Requests are sent on construction.
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        DatabaseName::kAdmin,
        requests,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        shouldRetry ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kNoRetry);

    while (!ars.done()) {
        // Block until a response is available.
        auto response = ars.next();

        // We wait for the responses from pending shards due to SERVER-85857, and skip processing
        // them - see 'BulkWriteOp._shouldStopCurrentRound' for more details.
        if (bulkWriteOp.shouldStopCurrentRound()) {
            ars.stopRetrying();
            continue;
        }
        // The BulkWriteOp may be marked finished early if we are in a transaction and encounter an
        // error, which aborts the transaction. In those cases, we do not bother processing those
        // responses.
        if (bulkWriteOp.isFinished()) {
            break;
        }

        auto iter = childBatches.find(response.shardId);
        tassert(8971901,
                "Unexpectedly could not find batches sent to a shard",
                iter != childBatches.end());

        TargetedWriteBatch* writeBatch = iter->second.get();
        tassert(8048101, "Unexpectedly could not find write batch for shard", writeBatch);

        // When the responseStatus is not OK, this means that mongos was unable to receive a
        // response from the shard the write batch was sent to, or mongos faced some other local
        // error (for example, mongos was shutting down).
        // The status being OK does not mean that all operations within the bulkWrite succeeded, nor
        // that we got an ok:1 response from the shard.
        if (!response.swResponse.getStatus().isOK()) {
            bulkWriteOp.processLocalChildBatchError(*writeBatch, response);
        } else {
            bulkWriteOp.processChildBatchResponseFromRemote(
                *writeBatch, response, errorsPerNamespace);
        }
    }
}

}  // namespace

void BulkWriteExecStats::noteTargetedShard(const BulkWriteCommandRequest& clientRequest,
                                           const TargetedWriteBatch& targetedBatch) {
    const ShardId& shardId = targetedBatch.getShardId();
    _targetedShards.insert(shardId);
    for (const auto& write : targetedBatch.getWrites()) {
        BulkWriteCRUDOp bulkWriteOp(clientRequest.getOps().at(write->writeOpRef.first));
        auto nsIdx = bulkWriteOp.getNsInfoIdx();
        auto batchType = convertOpType(bulkWriteOp.getType());
        _targetedShardsPerNsAndBatchType[nsIdx][batchType].insert(shardId);
    }
}

void BulkWriteExecStats::noteNumShardsOwningChunks(size_t nsIdx, int nShardsOwningChunks) {
    _numShardsOwningChunks[nsIdx] = nShardsOwningChunks;
}

void BulkWriteExecStats::noteTwoPhaseWriteProtocol(const BulkWriteCommandRequest& clientRequest,
                                                   const TargetedWriteBatch& targetedBatch,
                                                   size_t nsIdx,
                                                   int nShardsOwningChunks) {
    for (const auto& write : targetedBatch.getWrites()) {
        BulkWriteCRUDOp bulkWriteOp(clientRequest.getOps().at(write->writeOpRef.first));
        auto nsIdx = bulkWriteOp.getNsInfoIdx();
        auto batchType = convertOpType(bulkWriteOp.getType());
        // In this case, we aren't really targetting targetedBatch.getShardId, so only create the
        // batchType entry in the map. updateHostsTargetedMetrics reports kManyShards in the case no
        // shards is targeted overall.
        _targetedShardsPerNsAndBatchType[nsIdx][batchType];
    }

    noteNumShardsOwningChunks(nsIdx, nShardsOwningChunks);
}

void BulkWriteExecStats::updateMetrics(OperationContext* opCtx,
                                       const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                                       bool updatedShardKey) {
    // Record the number of shards targeted by this bulkWrite.
    CurOp::get(opCtx)->debug().nShards = _targetedShards.size();

    for (size_t nsIdx = 0; nsIdx < targeters.size(); ++nsIdx) {
        const auto& targeter = targeters[nsIdx];
        auto it = _targetedShardsPerNsAndBatchType.find(nsIdx);
        if (it == _targetedShardsPerNsAndBatchType.end()) {
            continue;
        }
        auto nShardsOwningChunks = getNumShardsOwningChunks(nsIdx);
        for (const auto& [batchType, shards] : it->second) {
            int nShards = shards.size();

            // If we have no information on the shards targeted, ignore updatedShardKey,
            // updateHostsTargetedMetrics will report this as TargetType::kManyShards.
            if (nShards != 0 && updatedShardKey) {
                nShards += 1;
            }

            if (nShardsOwningChunks.has_value()) {
                updateHostsTargetedMetrics(opCtx,
                                           batchType,
                                           nShardsOwningChunks.value(),
                                           nShards,
                                           targeter->isTargetedCollectionSharded());
            }
        }
    }
}

boost::optional<int> BulkWriteExecStats::getNumShardsOwningChunks(size_t nsIdx) const {
    auto it = _numShardsOwningChunks.find(nsIdx);
    if (it == _numShardsOwningChunks.end()) {
        return boost::none;
    }
    return it->second;
}

void executeRetryableTimeseriesUpdate(OperationContext* opCtx,
                                      TargetedBatchMap& childBatches,
                                      BulkWriteOp& bulkWriteOp) {
    invariant(!childBatches.empty());
    // Get the index of the targeted operation in the client bulkWrite request.
    auto opIdx = childBatches.begin()->second->getWrites()[0]->writeOpRef.first;

    // Construct a single-op update request based on the update operation at opIdx.
    auto& bulkWriteReq = bulkWriteOp.getClientRequest();

    BulkWriteCommandRequest singleUpdateRequest =
        bulk_write_common::makeSingleOpBulkWriteCommandRequest(bulkWriteReq, opIdx);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    txn_api::SyncTransactionWithRetries txn(
        opCtx, executor, nullptr /* resourceYielder */, inlineExecutor);
    BulkWriteCommandReply bulkWriteResponse;

    // Execute the singleUpdateRequest (a bulkWrite command) in an internal transaction to perform
    // the retryable timeseries update operation. This separate bulkWrite command will get executed
    // on its own via bulkWrite execute() logic again as a transaction, which handles retries of all
    // kinds. This function is just a client of the internal transaction spawned. As a result, we
    // must only receive a single final (non-retryable) response for the timeseries update
    // operation.
    auto swResult =
        txn.runNoThrow(opCtx,
                       [&singleUpdateRequest, &bulkWriteResponse](
                           const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                           auto updateResponse = txnClient.runCRUDOpSync(singleUpdateRequest);
                           bulkWriteResponse = std::move(updateResponse);

                           return SemiFuture<void>::makeReady();
                       });

    Status responseStatus = swResult.getStatus();
    WriteConcernErrorDetail wcError;
    if (responseStatus.isOK()) {
        if (!swResult.getValue().cmdStatus.isOK()) {
            responseStatus = swResult.getValue().cmdStatus;
        }
        wcError = swResult.getValue().wcError;
    }
    if (!responseStatus.isOK()) {
        // Set an error for the operation.
        bulkWriteResponse = createEmulatedErrorReply(responseStatus, 1, boost::none);
    }

    // We should only get back one reply item for the single update, unless we are in errorsOnly
    // mode then we can get 0.
    const auto& replyItems = bulkWriteResponse.getCursor().getFirstBatch();
    tassert(7934203,
            "unexpected reply for retryable timeseries update",
            replyItems.size() == 1 || replyItems.size() == 0);
    boost::optional<BulkWriteReplyItem> replyItem = boost::none;
    if (replyItems.size() == 1) {
        replyItem = replyItems[0];
    }

    LOGV2_DEBUG(7934204,
                4,
                "Processing bulk write response for retryable timeseries update",
                "opIdx"_attr = opIdx,
                "singleUpdateRequest"_attr = redact(singleUpdateRequest.toBSON()),
                "replyItem"_attr = replyItem,
                "wcError"_attr = wcError.toString());

    bulkWriteOp.noteWriteOpFinalResponse(opIdx,
                                         replyItem,
                                         bulkWriteResponse,
                                         ShardWCError(childBatches.begin()->first, wcError),
                                         bulkWriteResponse.getRetriedStmtIds());
}

void executeWriteWithoutShardKey(
    OperationContext* opCtx,
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    TargetedBatchMap& childBatches,
    BulkWriteOp& bulkWriteOp,
    stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace) {
    // If the targetStatus value is 'WithoutShardKeyOrId', then we have detected an
    // updateOne/deleteOne request without a shard key or _id. We will use a two
    // phase protocol to apply the write.
    tassert(7298300, "Executing empty write batch without shard key", !childBatches.empty());

    // Get the index of the targeted operation in the client bulkWrite request.
    const auto opIdx = childBatches.begin()->second->getWrites()[0]->writeOpRef.first;
    auto op = BulkWriteCRUDOp(bulkWriteOp.getClientRequest().getOps()[opIdx]);
    const auto nsIdx = op.getNsInfoIdx();
    auto& targeter = targeters[nsIdx];

    auto allowShardKeyUpdatesWithoutFullShardKeyInQuery =
        opCtx->isRetryableWrite() || opCtx->inMultiDocumentTransaction();

    // If there is a targeted write with a sampleId, use that write instead in order to pass the
    // sampleId to the two phase write protocol. Otherwise, just choose the first targeted write.
    const auto targetedWriteBatch = [&] {
        for (auto&& [_ /* shardId */, childBatch] : childBatches) {
            auto nextBatch = childBatch.get();

            // For a write without shard key, we expect each TargetedWriteBatch in childBatches
            // to contain only one TargetedWrite directed to each shard.
            tassert(7787100,
                    "There must be only 1 targeted write in this targeted write batch.",
                    nextBatch->getWrites().size() == 1);

            auto targetedWrite = nextBatch->getWrites().begin()->get();
            if (targetedWrite->sampleId) {
                return nextBatch;
            }
        }
        return childBatches.begin()->second.get();
    }();

    // Note: It is fine to use 'getAproxNShardsOwningChunks' here because the result is only
    // used to update stats.
    bulkWriteOp.noteTwoPhaseWriteProtocol(
        *targetedWriteBatch, nsIdx, targeter->getAproxNShardsOwningChunks());

    auto cmdObj = bulkWriteOp
                      .buildBulkCommandRequest(targeters,
                                               *targetedWriteBatch,
                                               allowShardKeyUpdatesWithoutFullShardKeyInQuery)
                      .toBSON();

    boost::optional<WriteConcernErrorDetail> wce;
    auto swRes = write_without_shard_key::runTwoPhaseWriteProtocol(
        opCtx, targeter->getNS(), std::move(cmdObj), wce);

    BulkWriteCommandReply bulkWriteResponse;
    WriteConcernErrorDetail wcError;
    if (wce.has_value()) {
        wcError = std::move(*wce);
    }

    Status responseStatus = swRes.getStatus();
    if (swRes.isOK()) {
        std::string errMsg;
        if (swRes.getValue().getResponse().isEmpty()) {
            // When we get an empty response, it means that the predicate didn't match anything
            // and no write was done. So we can just set a trivial ok response. Unless we are
            // running errors only in which case we set an empty vector.
            auto items = std::vector<mongo::BulkWriteReplyItem>{};
            if (!bulkWriteOp.getClientRequest().getErrorsOnly()) {
                items.push_back(BulkWriteReplyItem(0));
            }
            bulkWriteResponse.setCursor(
                BulkWriteCommandResponseCursor(0,  // cursorId
                                               items,
                                               NamespaceString::makeBulkWriteNSS(boost::none)));
            bulkWriteResponse.setNErrors(0);
            bulkWriteResponse.setNInserted(0);
            bulkWriteResponse.setNMatched(0);
            bulkWriteResponse.setNModified(0);
            bulkWriteResponse.setNUpserted(0);
            bulkWriteResponse.setNDeleted(0);
        } else {
            try {
                bulkWriteResponse = BulkWriteCommandReply::parse(
                    swRes.getValue().getResponse(),
                    IDLParserContext("BulkWriteCommandReplyForWriteWithoutShardKey"));
            } catch (const DBException& ex) {
                responseStatus = ex.toStatus().withContext(
                    "Failed to parse response from writes without shard key");
            }
        }
    }

    if (!responseStatus.isOK()) {
        // Set an error for the operation.
        bulkWriteResponse = createEmulatedErrorReply(responseStatus, 1, boost::none);
    }

    // We should get back just one reply item for the single update we are running.
    const auto& replyItems = bulkWriteResponse.getCursor().getFirstBatch();
    tassert(7298301,
            "unexpected bulkWrite reply for writes without shard key",
            replyItems.size() == 1 || replyItems.size() == 0);
    boost::optional<BulkWriteReplyItem> replyItem = boost::none;
    if (replyItems.size() == 1) {
        replyItem = replyItems[0];
    }

    LOGV2_DEBUG(7298302,
                4,
                "Processing bulk write response for writes without shard key",
                "opIdx"_attr = opIdx,
                "replyItem"_attr = replyItem,
                "wcError"_attr = wcError.toString());

    bulkWriteOp.noteWriteOpFinalResponse(opIdx,
                                         replyItem,
                                         bulkWriteResponse,
                                         ShardWCError(childBatches.begin()->first, wcError),
                                         bulkWriteResponse.getRetriedStmtIds());
}

void executeNonTargetedWriteWithoutShardKeyWithId(
    OperationContext* opCtx,
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    TargetedBatchMap& childBatches,
    BulkWriteOp& bulkWriteOp,
    stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace) {
    executeChildBatches(opCtx,
                        targeters,
                        childBatches,
                        bulkWriteOp,
                        errorsPerNamespace,
                        /*allowShardKeyUpdatesWithoutFullShardKeyInQuery=*/boost::none);

    // We need to know if the targeters encountered a Stale Shard Response
    // to determine whether to parse the _deferredResponses.
    bulkWriteOp.noteStaleResponses(targeters, errorsPerNamespace);
    bulkWriteOp.finishExecutingWriteWithoutShardKeyWithId();
}

void coordinateMultiUpdate(OperationContext* opCtx,
                           TargetedBatchMap& childBatches,
                           BulkWriteOp& bulkWriteOp) {
    auto bulkWriteResponse = coordinate_multi_update_util::executeCoordinateMultiUpdate(
        opCtx, childBatches, bulkWriteOp);

    const auto& replyItems = bulkWriteResponse.getCursor().getFirstBatch();
    tassert(8127600,
            "Unexpected reply for coordinateMultiUpdate",
            replyItems.size() == 1 || replyItems.size() == 0);
    boost::optional<BulkWriteReplyItem> replyItem = boost::none;
    if (replyItems.size() == 1) {
        replyItem = replyItems[0];
    }

    bulkWriteOp.noteWriteOpFinalResponse(
        coordinate_multi_update_util::getWriteOpIndex(childBatches),
        replyItem,
        bulkWriteResponse,
        ShardWCError(childBatches.begin()->first, WriteConcernErrorDetail{}),
        bulkWriteResponse.getRetriedStmtIds());
}

BulkWriteReplyInfo execute(OperationContext* opCtx,
                           const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                           const BulkWriteCommandRequest& clientRequest,
                           BulkWriteExecStats& execStats) {
    LOGV2_DEBUG(7263700,
                4,
                "Starting execution of a bulkWrite",
                "clientRequest"_attr = redact(clientRequest.toBSON()));

    BulkWriteOp bulkWriteOp(opCtx, clientRequest);

    bool refreshedTargeter = false;
    int rounds = 0;
    int numCompletedOps = 0;
    int numRoundsWithoutProgress = 0;
    Backoff backoff(Seconds(1), Seconds(2));

    while (!bulkWriteOp.isFinished()) {
        // Make sure we are not over our maximum memory allocation, if we are then mark the next
        // write op with an error and abort the operation.
        if (bulkWriteOp.aboveBulkWriteRepliesMaxSize()) {
            bulkWriteOp.abortDueToMaxSizeError();
        }

        // Target remaining ops with the appropriate targeter based on the namespace index and
        // re-batch ops based on their targeted shard id.
        TargetedBatchMap childBatches;

        // Divide and group ("target") the operations in the bulk write command. Some operations may
        // be split up (such as an update that needs to go to more than one shard), while others may
        // be grouped together if they need to go to the same shard.
        // These operations are grouped by shardId in the TargetedBatchMap childBatches.
        bool recordTargetErrors = refreshedTargeter;
        auto targetStatus = bulkWriteOp.target(targeters, recordTargetErrors, childBatches);
        if (!targetStatus.isOK()) {
            bulkWriteOp.processTargetingError(targetStatus);

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

            try {
                if (targetStatus.getValue() == WriteType::TimeseriesRetryableUpdate) {
                    executeRetryableTimeseriesUpdate(opCtx, childBatches, bulkWriteOp);
                } else if (targetStatus.getValue() == WriteType::WithoutShardKeyOrId) {
                    executeWriteWithoutShardKey(
                        opCtx, targeters, childBatches, bulkWriteOp, errorsPerNamespace);
                } else if (targetStatus.getValue() == WriteType::WithoutShardKeyWithId) {
                    executeNonTargetedWriteWithoutShardKeyWithId(
                        opCtx, targeters, childBatches, bulkWriteOp, errorsPerNamespace);
                } else if (targetStatus.getValue() == WriteType::MultiWriteBlockingMigrations) {
                    coordinateMultiUpdate(opCtx, childBatches, bulkWriteOp);
                } else {
                    // Send the child batches and wait for responses.
                    executeChildBatches(
                        opCtx,
                        targeters,
                        childBatches,
                        bulkWriteOp,
                        errorsPerNamespace,
                        /*allowShardKeyUpdatesWithoutFullShardKeyInQuery=*/boost::none);
                }
            } catch (const DBException&) {
                bulkWriteOp.noteStaleResponses(targeters, errorsPerNamespace);
                throw;
            }

            // If we saw any staleness errors, tell the targeters to invalidate their cache
            // so that they may be refreshed.
            // The staleness errors for WithoutShardKeyWithId writes have been evaluated earlier.
            if (targetStatus.getValue() != WriteType::WithoutShardKeyWithId)
                bulkWriteOp.noteStaleResponses(targeters, errorsPerNamespace);
        }

        rounds++;

        if (bulkWriteOp.isFinished()) {
            // No need to refresh the targeters if we are done.
            break;
        }

        // Refresh the targeter(s) if we received a target error or a stale config/db error.
        bool targeterChanged = false;
        try {
            for (auto& targeter : targeters) {
                targeterChanged |= targeter->createCollectionIfNeeded(opCtx);
            }
            LOGV2_DEBUG(7298200, 2, "Refreshing all targeters for bulkWrite");
            for (auto& targeter : targeters) {
                targeterChanged |= targeter->refreshIfNeeded(opCtx);
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

            bulkWriteOp.noteErrorForRemainingWrites(
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
        bulkWriteOp.setTargeterHasStaleShardResponse(false);
        LOGV2_DEBUG(7934202,
                    2,
                    "Completed a round of bulkWrite execution",
                    "rounds"_attr = rounds,
                    "numCompletedOps"_attr = numCompletedOps,
                    "targeterChanged"_attr = targeterChanged,
                    "numRoundsWithoutProgress"_attr = numRoundsWithoutProgress);

        if (numRoundsWithoutProgress > kMaxRoundsWithoutProgress) {
            bulkWriteOp.noteErrorForRemainingWrites(
                {ErrorCodes::NoProgressMade,
                 str::stream() << "no progress was made executing bulkWrite ops in after "
                               << kMaxRoundsWithoutProgress << " rounds (" << numCompletedOps
                               << " ops completed in " << rounds << " rounds total)"});
            break;
        }
        if (numRoundsWithoutProgress > 0) {
            sleepFor(backoff.nextSleep());
        }
    }

    for (size_t nsIdx = 0; nsIdx < targeters.size(); ++nsIdx) {
        // Note: It is fine to use 'getAproxNShardsOwningChunks' here because the result is only
        // used to update stats.
        bulkWriteOp.noteNumShardsOwningChunks(nsIdx,
                                              targeters[nsIdx]->getAproxNShardsOwningChunks());
    }

    LOGV2_DEBUG(7263701, 4, "Finished execution of bulkWrite");
    execStats = bulkWriteOp.getExecStats();
    return bulkWriteOp.generateReplyInfo();
}

BulkWriteCommandReply createEmulatedErrorReply(const Status& error,
                                               int errorCount,
                                               const boost::optional<TenantId>& tenantId) {
    std::vector<BulkWriteReplyItem> emulatedReplies;
    emulatedReplies.reserve(errorCount);

    for (int i = 0; i < errorCount; i++) {
        emulatedReplies.emplace_back(i, error);
    }

    BulkWriteCommandReply emulatedReply;
    emulatedReply.setCursor(BulkWriteCommandResponseCursor(
        0, emulatedReplies, NamespaceString::makeBulkWriteNSS(tenantId)));
    emulatedReply.setNErrors(errorCount);
    emulatedReply.setNDeleted(0);
    emulatedReply.setNModified(0);
    emulatedReply.setNInserted(0);
    emulatedReply.setNUpserted(0);
    emulatedReply.setNMatched(0);
    return emulatedReply;
}

/**
 * Calculates an estimate of the size, in bytes, required to store the common fields that will
 * go into each child batch command sent to a shard, i.e. all fields besides the actual write
 * ops.
 */
int computeBaseSizeEstimate(OperationContext* opCtx, const BulkWriteCommandRequest& clientRequest) {
    // For simplicity, we build a dummy bulk write command request that contains all the common
    // fields and serialize it to get the base command size. We only bother to copy over variable
    // size and/or optional fields, since the value of fields that are fixed-size and always present
    // (e.g. 'ordered') won't affect the size calculation.
    BulkWriteCommandRequest request;

    // We'll account for the size to store each individual nsInfo as we add them, so just put an
    // empty vector as a placeholder for the array. This will ensure we properly count the size of
    // the field name and the empty array.
    request.setNsInfo({});

    request.setDbName(clientRequest.getDbName());
    request.setLet(clientRequest.getLet());

    // We'll account for the size to store each individual op as we add them, so just put an empty
    // vector as a placeholder for the array. This will ensure we properly count the size of the
    // field name and the empty array.
    request.setOps({});

    if (opCtx->isRetryableWrite()) {
        // We'll account for the size to store each individual stmtId as we add ops, so similar to
        // above with ops, we just put an empty vector as a placeholder for now.
        request.setStmtIds({});
    }

    request.setBypassEmptyTsReplacement(clientRequest.getBypassEmptyTsReplacement());

    BSONObjBuilder builder;
    request.serialize(&builder);
    // Add writeConcern and lsid/txnNumber to ensure we save space for them.
    logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
    builder.append(WriteConcernOptions::kWriteConcernField, opCtx->getWriteConcern().toBSON());

    return builder.obj().objsize();
}

BulkCommandSizeEstimator::BulkCommandSizeEstimator(OperationContext* opCtx,
                                                   const BulkWriteCommandRequest& clientRequest)
    : _clientRequest(clientRequest),
      _isRetryableWriteOrInTransaction(opCtx->getTxnNumber().has_value()),
      _baseSizeEstimate(computeBaseSizeEstimate(opCtx, clientRequest)) {}

int BulkCommandSizeEstimator::getBaseSizeEstimate() const {
    return _baseSizeEstimate;
}

int BulkCommandSizeEstimator::getOpSizeEstimate(int opIdx, const ShardId& shardId) const {
    // If retryable writes are used, MongoS needs to send an additional array of stmtId(s)
    // corresponding to the statements that got routed to each individual shard, so they need to
    // be accounted in the potential request size so it does not exceed the max BSON size.
    int writeSizeBytes = BatchItemRef{&_clientRequest, opIdx}.estimateOpSizeInBytes() +
        write_ops::kWriteCommandBSONArrayPerElementOverheadBytes +
        (_isRetryableWriteOrInTransaction
             ? write_ops::kStmtIdSize + write_ops::kWriteCommandBSONArrayPerElementOverheadBytes
             : 0);

    const auto& bulkWriteOp = BulkWriteCRUDOp(_clientRequest.getOps()[opIdx]);

    // Get the set of nsInfos we've accounted for on this shardId.
    auto iter = _accountedForNsInfos.find(shardId);

    // If we have not accounted for this one already then increase the write size estimate by the
    // nsInfo size and store this index so it does not get counted again.
    if (iter == _accountedForNsInfos.end() || !iter->second.contains(bulkWriteOp.getNsInfoIdx())) {
        // Account for optional fields that can be set per namespace to have a conservative
        // estimate.
        static const ShardVersion mockShardVersion =
            ShardVersionFactory::make(ChunkVersion::IGNORED());
        static const DatabaseVersion mockDBVersion = DatabaseVersion(UUID::gen(), Timestamp());

        auto nsEntry = _clientRequest.getNsInfo()[bulkWriteOp.getNsInfoIdx()];
        nsEntry.setShardVersion(mockShardVersion);
        nsEntry.setDatabaseVersion(mockDBVersion);
        if (!nsEntry.getNs().isTimeseriesBucketsCollection()) {
            // This could be a timeseries view. To be conservative about the estimate, we
            // speculatively account for the additional size needed for the timeseries bucket
            // transalation and the 'isTimeseriesCollection' field.
            nsEntry.setNs(nsEntry.getNs().makeTimeseriesBucketsNamespace());
            nsEntry.setIsTimeseriesNamespace(true);
        }

        writeSizeBytes +=
            nsEntry.toBSON().objsize() + write_ops::kWriteCommandBSONArrayPerElementOverheadBytes;
    }

    return writeSizeBytes;
}

void BulkCommandSizeEstimator::addOpToBatch(int opIdx, const ShardId& shardId) {
    // Get the set of nsInfos we've accounted for on this shardId.
    _accountedForNsInfos.try_emplace(shardId, absl::flat_hash_set<int>());
    auto iter = _accountedForNsInfos.find(shardId);
    invariant(iter != _accountedForNsInfos.end());

    // If we have not accounted for this one already then increase the write size estimate by the
    // nsInfo size and store this index so it does not get counted again.
    const auto& bulkWriteOp = BulkWriteCRUDOp(_clientRequest.getOps()[opIdx]);
    iter->second.insert(bulkWriteOp.getNsInfoIdx());
}

BulkWriteOp::BulkWriteOp(OperationContext* opCtx, const BulkWriteCommandRequest& clientRequest)
    : _opCtx(opCtx),
      _clientRequest(clientRequest),
      _txnNum(_opCtx->getTxnNumber()),
      _writeConcern(opCtx->getWriteConcern()),
      _inTransaction(static_cast<bool>(TransactionRouter::get(opCtx))),
      _isRetryableWrite(opCtx->isRetryableWrite()) {
    _writeOps.reserve(_clientRequest.getOps().size());
    _retriedStmtIds = stdx::unordered_set<StmtId>();
    for (size_t i = 0; i < _clientRequest.getOps().size(); ++i) {
        _writeOps.emplace_back(BatchItemRef(&_clientRequest, i), _inTransaction);
    }
}

StatusWith<WriteType> BulkWriteOp::target(const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                                          bool recordTargetErrors,
                                          TargetedBatchMap& targetedBatches) {
    const auto ordered = _clientRequest.getOrdered();

    BulkCommandSizeEstimator sizeEstimator(_opCtx, _clientRequest);

    return targetWriteOps(
        _opCtx,
        _writeOps,
        ordered,
        recordTargetErrors,
        _pauseMigrationsDuringMultiUpdatesParameter,
        [&](const WriteOp& writeOp) -> const NSTargeter& {
            const auto opIdx = writeOp.getWriteItem().getItemIndex();
            const auto& bulkWriteOp = BulkWriteCRUDOp(_clientRequest.getOps()[opIdx]);
            return *targeters[bulkWriteOp.getNsInfoIdx()];
        },
        sizeEstimator,
        targetedBatches);
}

BulkWriteCommandRequest BulkWriteOp::buildBulkCommandRequest(
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    const TargetedWriteBatch& targetedBatch,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) const {
    BulkWriteCommandRequest request;

    // A single bulk command request batch may contain operations of different
    // types, i.e. they may be inserts, updates or deletes.
    std::vector<BulkWriteOpVariant> ops;
    std::vector<NamespaceInfoEntry> nsInfo = _clientRequest.getNsInfo();
    std::vector<NamespaceInfoEntry> batchNsInfo = std::vector<NamespaceInfoEntry>();

    // So we don't send unnecessary nsInfos in the subbatch we need to keep track of a mapping of
    // the original nsInfoIdx to the new nsInfoidx. If we don't do this then document sequenced
    // nsInfo array can cause child batches to be over the max BSON size.
    absl::flat_hash_map<int, int> nsInfoIndexMap;

    std::vector<int> stmtIds;
    if (_isRetryableWrite)
        stmtIds.reserve(targetedBatch.getNumOps());

    for (const auto& targetedWrite : targetedBatch.getWrites()) {
        const ItemIndexChildIndexPair& writeOpRef = targetedWrite->writeOpRef;
        ops.push_back(_clientRequest.getOps().at(writeOpRef.first));

        if (targetedWrite->sampleId.has_value()) {
            visit(
                OverloadedVisitor{
                    [&](mongo::BulkWriteInsertOp& op) { return; },
                    [&](mongo::BulkWriteUpdateOp& op) { op.setSampleId(targetedWrite->sampleId); },
                    [&](mongo::BulkWriteDeleteOp& op) { op.setSampleId(targetedWrite->sampleId); },
                },
                ops.back());
        }

        // Set the nsInfo's shardVersion & databaseVersion fields based on the endpoint
        // of each operation. Since some operations may be on the same namespace, this
        // might result in the same nsInfo entry being written to multiple times. This
        // is OK, since we know that in a single batch, all operations on the same
        // namespace MUST have the same shardVersion & databaseVersion.
        // Invariant checks that either the shardVersion & databaseVersion in nsInfo are
        // null OR the new versions in the targetedWrite match the existing version in
        // nsInfo.
        const auto& bulkWriteOp = BulkWriteCRUDOp(ops.back());
        auto nsIdx = bulkWriteOp.getNsInfoIdx();

        // See if we have already added this index to the childBatch. If not then we need to add it
        // here.
        auto iter = nsInfoIndexMap.find(nsIdx);
        if (iter == nsInfoIndexMap.end()) {
            batchNsInfo.push_back(nsInfo.at(nsIdx));
            iter = nsInfoIndexMap.insert({nsIdx, batchNsInfo.size() - 1}).first;
        }

        // Set the new nsInfoIdx on the op for the childBatch.
        visit(
            OverloadedVisitor{
                [&](auto& op) { op.setNsInfoIdx(iter->second); },
            },
            ops.back());

        auto& nsInfoEntry = batchNsInfo.at(iter->second);
        auto& targeter = targeters.at(nsIdx);

        auto isClientRequestOnTimeseriesBucketCollection =
            nsInfoEntry.getNs().isTimeseriesBucketsCollection();
        if (targeter->isTrackedTimeSeriesBucketsNamespace() &&
            !isClientRequestOnTimeseriesBucketCollection) {
            // For tracked timeseries collections, only the bucket collections are tracked. This
            // sets the namespace to the namespace of the tracked bucket collection.
            nsInfoEntry.setNs(targeter->getNS());
            if (!isRawDataOperation(_opCtx)) {
                nsInfoEntry.setIsTimeseriesNamespace(true);
            }
        }

        // If we are using the two phase write protocol introduced in PM-1632, we allow shard key
        // updates without specifying the full shard key in the query if we execute the update in a
        // retryable write/transaction.
        if (bulkWriteOp.getType() == BulkWriteCRUDOp::OpType::kUpdate &&
            allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
            auto mutableUpdateOp = get_if<BulkWriteUpdateOp>(&ops.back());
            mutableUpdateOp->setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
                allowShardKeyUpdatesWithoutFullShardKeyInQuery);
        }

        invariant((!nsInfoEntry.getShardVersion() ||
                   nsInfoEntry.getShardVersion() == targetedWrite->endpoint.shardVersion) &&
                  (!nsInfoEntry.getDatabaseVersion() ||
                   nsInfoEntry.getDatabaseVersion() == targetedWrite->endpoint.databaseVersion));

        nsInfoEntry.setShardVersion(targetedWrite->endpoint.shardVersion);
        nsInfoEntry.setDatabaseVersion(targetedWrite->endpoint.databaseVersion);

        if (_isRetryableWrite) {
            stmtIds.push_back(bulk_write_common::getStatementId(_clientRequest, writeOpRef.first));
        }
    }

    request.setOps(ops);
    request.setNsInfo(batchNsInfo);

    // It isn't necessary to copy the cursor options over, because the cursor options
    // are for use in the interaction between the mongos and the client and not
    // internally between the mongos and the mongods.
    request.setOrdered(_clientRequest.getOrdered());
    request.setBypassDocumentValidation(_clientRequest.getBypassDocumentValidation());
    request.setLet(_clientRequest.getLet());
    request.setErrorsOnly(_clientRequest.getErrorsOnly());

    if (_isRetryableWrite) {
        request.setStmtIds(stmtIds);
    }

    request.setBypassEmptyTsReplacement(_clientRequest.getBypassEmptyTsReplacement());

    request.setDbName(DatabaseName::kAdmin);

    return request;
}

bool BulkWriteOp::isFinished() const {
    // We encountered some error requiring us to abort execution. Note this may mean that some ops
    // are left in state pending.
    if (_aborted) {
        return true;
    }

    // TODO: Track ops lifetime.
    const bool ordered = _clientRequest.getOrdered();
    for (auto& writeOp : _writeOps) {
        if (writeOp.getWriteState() < WriteOpState_Completed) {
            return false;
        } else if (writeOp.getWriteState() == WriteOpState_Error) {
            // If the WriteOp's state is "WriteOpState_Error" and if '_inTransaction || ordered'
            // is true, then normally isFinished() will return true. Doing this allows the operation
            // to be aborted quickly, without having to wait for any remaining child ops.
            //
            // However, the logic in "cluster_write_cmd.cpp" and "cluster_find_and_modify_cmd.cpp"
            // that handles WouldChangeOwningShard errors requires that the current transaction
            // (if any) not be aborted when a WouldChangeOwningShard error occurs.
            //
            // Thus, if the WriteOp's state is "WriteOpState_Error" with a WouldChangeOwningShard
            // error and '_inTransaction || ordered' is true, and if the WriteOp still has pending
            // child ops, then isFinished() must return false to allow the pending child ops to
            // finish cleanly (to avoid causing the transaction to abort).
            if (writeOp.getOpError().getStatus().code() == ErrorCodes::WouldChangeOwningShard &&
                writeOp.hasPendingChildOps() && _inTransaction) {
                return false;
            }

            // If the BulkWriteOp is ordered or we're in a transaction -AND- if this WriteOp
            // encountered an error (excluding the WouldChangeOwningShard case handled above),
            // then return true to indicate that this BulkWriteOp is finished.
            if (_inTransaction || ordered) {
                return true;
            }
        }
    }
    return true;
}

bool BulkWriteOp::aboveBulkWriteRepliesMaxSize() const {
    return _approximateSize >= gBulkWriteMaxRepliesSize.loadRelaxed();
}

void BulkWriteOp::abortDueToMaxSizeError() {
    // Need to find the next writeOp so we can store an error in it.
    for (auto& writeOp : _writeOps) {
        if (writeOp.getWriteState() < WriteOpState_Completed) {
            writeOp.setOpError(write_ops::WriteError(
                0,
                Status{ErrorCodes::ExceededMemoryLimit,
                       fmt::format("BulkWrite response size exceeded limit ({} bytes)",
                                   _approximateSize)}));
            break;
        }
    }
    _aborted = true;
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

void BulkWriteOp::noteErrorForRemainingWrites(const Status& status) {
    dassert(!isFinished());
    dassert(numWriteOpsIn(WriteOpState_Pending) == 0);

    const auto ordered = _clientRequest.getOrdered();
    for (auto& writeOp : _writeOps) {
        if (writeOp.getWriteState() < WriteOpState_Completed) {
            const auto opIdx = writeOp.getWriteItem().getItemIndex();
            writeOp.setOpError(write_ops::WriteError(opIdx, status));

            // Only return the first error if we are ordered or are within a transaction.
            if (ordered || _inTransaction)
                break;
        }
    }

    dassert(isFinished());
}

void BulkWriteOp::processChildBatchResponseFromRemote(
    const TargetedWriteBatch& writeBatch,
    const AsyncRequestsSender::Response& response,
    boost::optional<stdx::unordered_map<NamespaceString, TrackedErrors>&> errorsPerNamespace) {
    invariant(response.swResponse.getStatus(), "Response status was unexpectedly not OK");

    auto childBatchResponse = response.swResponse.getValue();
    LOGV2_DEBUG(7279200,
                4,
                "Processing bulk write response from shard.",
                "shard"_attr = response.shardId,
                "response"_attr = childBatchResponse.data);

    auto childBatchStatus = getStatusFromCommandResult(childBatchResponse.data);
    if (childBatchStatus.isOK()) {
        auto bwReply = BulkWriteCommandReply::parse(childBatchResponse.data);
        if (bwReply.getWriteConcernError()) {
            saveWriteConcernError(
                response.shardId, bwReply.getWriteConcernError().value(), writeBatch);
        }

        // Capture the errors if any exist and mark the writes in the TargetedWriteBatch so that
        // they may be re-targeted if needed.
        noteChildBatchResponse(writeBatch, bwReply, errorsPerNamespace);
    } else {
        noteChildBatchError(writeBatch, childBatchStatus, errorsPerNamespace);

        // If we are in a transaction, we must abort execution on any error, excluding
        // WouldChangeOwningShard. We do not abort on WouldChangeOwningShard because the error is
        // returned from the shard and recorded here as a placeholder, as we will end up processing
        // the update (as a delete + insert on the corresponding shards in a txn) at the level of
        // ClusterBulkWriteCmd.
        if (TransactionRouter::get(_opCtx) &&
            childBatchStatus != ErrorCodes::WouldChangeOwningShard) {
            _aborted = true;

            auto errorReply = ErrorReply::parse(childBatchResponse.data);

            // Transient transaction errors should be returned directly as top level errors to allow
            // the client to retry.
            if (hasTransientTransactionErrorLabel(errorReply)) {
                const auto shardInfo = response.shardHostAndPort
                    ? response.shardHostAndPort->toString()
                    : writeBatch.getShardId();
                auto newStatus = childBatchStatus.withContext(
                    str::stream() << "Encountered error from " << shardInfo
                                  << " during a transaction");

                uassertStatusOK(newStatus);
            }
        }
    }
}

void BulkWriteOp::noteWriteOpResponse(const std::unique_ptr<TargetedWrite>& targetedWrite,
                                      WriteOp& op,
                                      const BulkWriteCommandReply& commandReply,
                                      size_t numOps,
                                      const boost::optional<const BulkWriteReplyItem&> replyItem) {
    if (op.getWriteType() == WriteType::WithoutShardKeyWithId) {
        // We have to extract the fields that are equivalent to 'n' in 'update' and 'delete'
        // command replies:
        // - For an update, this is 'nMatched' (rather than 'nUpdated', as it is possible the update
        // matches a document but the update modification is a no-op, e.g. it sets a field to its
        // current value, and in that case we should consider the write as done).
        // - For a delete, we can just consult 'nDeleted'.
        // Since the write is either an update or a delete, summing these two values gives us the
        // correct value of 'n'.
        auto n = commandReply.getNMatched() + commandReply.getNDeleted();
        op.noteWriteWithoutShardKeyWithIdResponse(_opCtx, *targetedWrite, n, numOps, replyItem);

        if (op.getWriteState() == WriteOpState_Completed) {
            _shouldStopCurrentRound = true;
        }
    } else {
        op.noteWriteComplete(_opCtx, *targetedWrite, replyItem);
    }
}

void BulkWriteOp::noteChildBatchResponse(
    const TargetedWriteBatch& targetedBatch,
    const BulkWriteCommandReply& commandReply,
    boost::optional<stdx::unordered_map<NamespaceString, TrackedErrors>&> errorsPerNamespace) {
    int firstTargetedWriteOpIdx = targetedBatch.getWrites().front()->writeOpRef.first;
    bool isWithoutShardKeyWithIdWrite =
        (_writeOps[firstTargetedWriteOpIdx].getWriteType() == WriteType::WithoutShardKeyWithId);
    bool shouldDeferWriteWithoutShardKeyReponse =
        isWithoutShardKeyWithIdWrite && targetedBatch.getNumOps() > 1;

    const auto replyItems =
        exhaustCursorForReplyItems(_opCtx, targetedBatch.getShardId(), commandReply);

    _nInserted += commandReply.getNInserted();
    _nDeleted += commandReply.getNDeleted();
    _nMatched += commandReply.getNMatched();
    _nUpserted += commandReply.getNUpserted();
    _nModified += commandReply.getNModified();

    // To support errorsOnly:true we need to keep separate track of the index in the replyItems
    // array and the index of the write ops we need to mark. This is because with errorsOnly we do
    // not guarantee that writeOps.size == replyItems.size, successful writes do not return a reply.
    // We need to be able to check if the write
    // op has the same index as the next reply we received, which is why we need to track 2
    // different indexes in this loop. Our goal is to iterate the arrays as such
    // writes:  [0, 1, 2, 3] -> [0, 1, 2, 3] -> [0, 1, 2, 3] -> [0, 1, 2, 3]
    //           ^                  ^                  ^                  ^
    // replies: [1, 3]       -> [1, 3]       -> [1, 3]       -> [1, 3]
    //           ^               ^                  ^               ^
    // Only moving forward in replies when we see a matching write op.
    int replyIndex = -1;
    // A batch will fail on an error if the request was sent with ordered:true or we are executing
    // the request within a transaction.
    bool batchWillContinue = !_clientRequest.getOrdered() && !_inTransaction;
    boost::optional<write_ops::WriteError> lastError;
    for (int writeOpIdx = 0; writeOpIdx < (int)targetedBatch.getWrites().size(); ++writeOpIdx) {
        const auto& write = targetedBatch.getWrites()[writeOpIdx];
        WriteOp& writeOp = _writeOps[write->writeOpRef.first];

        // This is only possible if we ran an errorsOnly:true command and succeeded all writes.
        if (replyItems.size() == 0) {
            tassert(8266001,
                    "bulkWrite should always get replies when not in errorsOnly",
                    _clientRequest.getErrorsOnly());
            if (shouldDeferWriteWithoutShardKeyReponse) {
                if (!_deferredResponses) {
                    _deferredResponses.emplace();
                }
                _deferredResponses->push_back(
                    std::make_tuple(&targetedBatch, commandReply, boost::none));
            }
            noteWriteOpResponse(
                write, writeOp, commandReply, targetedBatch.getNumOps(), boost::none);
            continue;
        }

        replyIndex++;

        // When an error is encountered on an ordered bulk write, it is impossible for any of the
        // remaining operations to have been executed. For that reason we reset them here so they
        // may be retargeted and retried if the error we saw is one we can retry after (e.g.
        // StaleConfig.).
        if (!batchWillContinue && lastError) {
            tassert(8266002,
                    "bulkWrite should not see replies after an error when ordered:true",
                    replyIndex >= (int)replyItems.size());
            writeOp.resetWriteToReady(_opCtx);
            continue;
        }

        // On most errors (for example, a DuplicateKeyError) unordered bulkWrite on a shard attempts
        // to execute following operations even if a preceding operation errored. This isn't true
        // for StaleConfig, StaleDbVersion of ShardCannotRefreshDueToLocksHeld errors. On these
        // errors, since the shard knows that following operations will fail for the same reason, it
        // stops right away (except for unordered timeseries inserts, see SERVER-80796).
        // As a consequence, although typically we can expect the size of replyItems to match the
        // size of the number of operations sent (even in the case of errors), when a
        // staleness/cache busy error is received the size of replyItems will be <= the size of the
        // number of operations. When this is the case, we treat all the remaining operations which
        // may not have a replyItem as having failed due to the same cause.
        bool isStaleError = lastError &&
            (lastError->getStatus().code() == ErrorCodes::StaleDbVersion ||
             ErrorCodes::isStaleShardVersionError(lastError->getStatus()) ||
             lastError->getStatus().code() == ErrorCodes::ShardCannotRefreshDueToLocksHeld ||
             lastError->getStatus() == ErrorCodes::CannotImplicitlyCreateCollection);

        if (batchWillContinue && isStaleError && (replyIndex == (int)replyItems.size())) {
            // Decrement the replyIndex so it keeps pointing to the same error (i.e. the
            // last error, which is a staleness error).
            LOGV2_DEBUG(7695304,
                        4,
                        "Duplicating the error for op",
                        "opIdx"_attr = write->writeOpRef.first,
                        "error"_attr = lastError->getStatus());
            replyIndex--;
        }

        // If we are out of replyItems but have more write ops then we must be in an ordered:false
        // errorsOnly:true bulkWrite where we have successful results after the last error.
        if (replyIndex >= (int)replyItems.size()) {
            tassert(8516601,
                    "bulkWrite received more replies than writes",
                    _clientRequest.getErrorsOnly());
            noteWriteOpResponse(
                write, writeOp, commandReply, targetedBatch.getNumOps(), boost::none);
            continue;
        }

        auto& reply = replyItems[replyIndex];

        // This can only happen when running an errorsOnly:true bulkWrite. We will only receive a
        // bulkWriteReplyItem for an error response when this flag is enabled. This means that
        // any writeOp which does not have a reply must have succeeded.
        // Since both the writeOps and the replies are stored in ascending index order this is
        // a safe assumption.
        // writeOpIdx can be > than reply.getIdx when we are duplicating the last error
        // as described in the block above.
        if (writeOpIdx < reply.getIdx()) {
            tassert(8266003,
                    "bulkWrite should get a reply for every write op when not in errorsOnly mode",
                    _clientRequest.getErrorsOnly());

            noteWriteOpResponse(
                write, writeOp, commandReply, targetedBatch.getNumOps(), boost::none);
            // We need to keep the replyIndex where it is until we see the op matching its index.
            replyIndex--;
            continue;
        }

        // A staleness error will end up being retried by mongos so we should not consider this
        // result final and count towards the maximum size limit for a response.
        if (!isStaleError) {
            _approximateSize += reply.getApproximateSize();
        }

        if (shouldDeferWriteWithoutShardKeyReponse) {
            if (!_deferredResponses) {
                _deferredResponses.emplace();
            }
            auto newReply = BulkWriteReplyItem::parse(reply.toBSON());
            _deferredResponses->push_back(std::make_tuple(&targetedBatch, commandReply, newReply));
        }

        if (reply.getStatus().isOK()) {
            noteWriteOpResponse(write, writeOp, commandReply, targetedBatch.getNumOps(), reply);
        } else {
            lastError.emplace(write->writeOpRef.first, reply.getStatus());
            writeOp.noteWriteError(_opCtx, *write, *lastError);

            auto origWrite = BulkWriteCRUDOp(_clientRequest.getOps()[write->writeOpRef.first]);
            auto nss = _clientRequest.getNsInfo()[origWrite.getNsInfoIdx()].getNs();

            // We don't always want to track errors per-namespace, e.g. when we encounter errors
            // local to mongos.
            if (errorsPerNamespace) {
                if (errorsPerNamespace->find(nss) == errorsPerNamespace->end()) {
                    TrackedErrors trackedErrors;
                    // Stale routing info errors need to be tracked in order to trigger a refresh of
                    // the targeter. On the other hand, errors caused by the catalog cache being
                    // temporarily unavailable (such as ShardCannotRefreshDueToLocksHeld) are
                    // ignored in this context, since no deduction can be made around possible
                    // placement changes.
                    trackedErrors.startTracking(ErrorCodes::StaleConfig);
                    trackedErrors.startTracking(ErrorCodes::StaleDbVersion);
                    trackedErrors.startTracking(ErrorCodes::CannotImplicitlyCreateCollection);
                    errorsPerNamespace->emplace(nss, trackedErrors);
                }

                auto trackedErrors = errorsPerNamespace->find(nss);
                invariant(trackedErrors != errorsPerNamespace->end());
                if (trackedErrors->second.isTracking(reply.getStatus().code())) {
                    trackedErrors->second.addError(ShardError(write->endpoint, *lastError));
                }
            }
        }
    }

    if (auto retriedStmtIds = commandReply.getRetriedStmtIds();
        retriedStmtIds && !retriedStmtIds->empty()) {
        for (auto retriedStmtId : *retriedStmtIds) {
            _retriedStmtIds->insert(retriedStmtId);
        }
    }
}

void BulkWriteOp::processTargetingError(const StatusWith<WriteType>& targetStatus) {
    invariant(!targetStatus.isOK());
    // Note that the targeting logic already handles recording the error for the appropriate
    // WriteOp, so we only need to update the BulkWriteOp state here.
    if (_inTransaction) {
        _aborted = true;

        // Throw when there is a transient transaction error since this should be a top
        // level error and not just a write error.
        if (isTransientTransactionError(targetStatus.getStatus().code(),
                                        false /* hasWriteConcernError */,
                                        false /* isCommitOrAbort */)) {
            uassertStatusOK(targetStatus);
        }
    }
}

void BulkWriteOp::abortIfNeeded(const mongo::Status& error) {
    invariant(!error.isOK());

    // If we see a local shutdown error, it means mongos itself is shutting down. A remote shutdown
    // error would have been returned with response.swResponse.getStatus() being OK.
    // If we see a local CallbackCanceled error, it is likely also due to mongos shutting down,
    // therefore shutting down executor thread pools and cancelling any work scheduled on them.
    // While we don't currently know of any other cases we'd see CallbackCanceled here, we check
    // the shutdown flag as well to ensure the cancellation is due to shutdown.
    // While the shutdown flag check is deprecated, that is because modules shouldn't consult it
    // to coordinate their own shutdowns. But it is OK to use here because we are only checking
    // whether a shutdown has started.
    if (ErrorCodes::isShutdownError(error) ||
        (error == ErrorCodes::CallbackCanceled && globalInShutdownDeprecated())) {
        // We shouldn't continue execution (even if unordered) if we are shutting down since
        // further batches will fail to execute as well.
        _aborted = true;

        // We want to throw such an error at the top level so that it can be returned to the client
        // directly with the appropriate error labels,  allowing them to retry it.
        uassertStatusOK(error);
    }

    // If we are in a transaction, we must stop immediately (even for unordered).
    if (_inTransaction) {
        // Even if we aren't throwing a top-level error, we won't continue processing any
        // outstanding writes after seeing this error since the transaction is aborted.
        _aborted = true;

        // Throw when there is a transient transaction error as those must be returned to the client
        // at the top level to allow them to retry.
        if (isTransientTransactionError(error.code(), false, false)) {
            uassertStatusOK(error);
        }
    }
}

void BulkWriteOp::processLocalChildBatchError(const TargetedWriteBatch& batch,
                                              const AsyncRequestsSender::Response& response) {
    const auto& responseStatus = response.swResponse.getStatus();
    invariant(!responseStatus.isOK(), "Response status was unexpectedly OK");

    const auto shardInfo =
        response.shardHostAndPort ? response.shardHostAndPort->toString() : batch.getShardId();

    const Status status = responseStatus.withContext(
        str::stream() << "bulkWrite results unavailable "
                      << (response.shardHostAndPort ? "from "
                                                    : "from failing to target a host in the shard ")
                      << shardInfo);

    noteChildBatchError(batch, status, boost::none);

    LOGV2_DEBUG(8048100,
                4,
                "Unable to receive bulkWrite results from shard",
                "shardInfo"_attr = shardInfo,
                "error"_attr = redact(status));

    abortIfNeeded(responseStatus);
}

void BulkWriteOp::noteChildBatchError(
    const TargetedWriteBatch& targetedBatch,
    const Status& status,
    boost::optional<stdx::unordered_map<NamespaceString, TrackedErrors>&> errorsPerNamespace) {
    // Treat an error to get a batch response as failures of the contained write(s).
    const int numErrors =
        (_clientRequest.getOrdered() || _inTransaction) ? 1 : targetedBatch.getWrites().size();
    auto emulatedReply =
        createEmulatedErrorReply(status, numErrors, _clientRequest.getDbName().tenantId());

    // This error isn't actually specific to any namespaces and so we do not want to track it.
    noteChildBatchResponse(targetedBatch, emulatedReply, errorsPerNamespace);
}

void BulkWriteOp::noteWriteOpFinalResponse(
    size_t opIdx,
    const boost::optional<BulkWriteReplyItem>& reply,
    const BulkWriteCommandReply& response,
    const ShardWCError& shardWCError,
    const boost::optional<std::vector<StmtId>>& retriedStmtIds) {
    WriteOp& writeOp = _writeOps[opIdx];

    // Cancel all childOps if any.
    writeOp.resetWriteToReady(_opCtx);

    if (!shardWCError.error.toStatus().isOK()) {
        saveWriteConcernError(shardWCError);
    }

    if (reply) {
        _approximateSize += reply->getApproximateSize();
    }

    if (response.getNErrors() == 0) {
        if (writeOp.getWriteItem().getOpType() == BatchedCommandRequest::BatchType_Insert) {
            _nInserted += response.getNInserted();
        } else if (writeOp.getWriteItem().getOpType() == BatchedCommandRequest::BatchType_Delete) {
            _nDeleted += response.getNDeleted();
        } else {
            _nModified += response.getNModified();
            _nMatched += response.getNMatched();
            _nUpserted += response.getNUpserted();
        }
        writeOp.setOpComplete(reply);
    } else {
        auto writeError = write_ops::WriteError(opIdx, reply->getStatus());
        writeOp.setOpError(writeError);
        abortIfNeeded(reply->getStatus());
    }

    if (retriedStmtIds && !retriedStmtIds->empty()) {
        for (auto retriedStmtId : *retriedStmtIds) {
            _retriedStmtIds->insert(retriedStmtId);
        }
    }
}

BulkWriteReplyInfo BulkWriteOp::generateReplyInfo() {
    dassert(isFinished());
    std::vector<BulkWriteReplyItem> replyItems;
    SummaryFields summary;
    summary.nInserted = _nInserted;
    summary.nDeleted = _nDeleted;
    summary.nMatched = _nMatched;
    summary.nModified = _nModified;
    summary.nUpserted = _nUpserted;
    replyItems.reserve(_writeOps.size());

    std::vector<boost::optional<std::string>> actualCollections(_clientRequest.getNsInfo().size(),
                                                                boost::none);
    std::deque<bool> hasContactedPrimaryShard(_clientRequest.getNsInfo().size(), false);

    const auto ordered = _clientRequest.getOrdered();
    for (auto& writeOp : _writeOps) {
        // If we encountered an error causing us to abort execution we may not have waited for
        // responses to all outstanding requests.
        dassert(writeOp.getWriteState() != WriteOpState_Pending || _aborted);
        auto writeOpState = writeOp.getWriteState();

        if (writeOpState == WriteOpState_Completed || writeOpState == WriteOpState_Error) {
            switch (writeOp.getWriteItem().getOpType()) {
                case BatchedCommandRequest::BatchType_Insert:
                    serviceOpCounters(ClusterRole::RouterServer).gotInsert();
                    break;
                case BatchedCommandRequest::BatchType_Update: {
                    // It is easier to handle the metric in handleWouldChangeOwningShardError for
                    // WouldChangeOwningShard. See getWouldChangeOwningShardErrorInfo for the batch
                    // size check. In the case of a WouldChangeOwningShard outside of a transaction,
                    // we will re-run cluster::bulkWrite so generateReplyInfo() gets called twice.
                    if (writeOpState != WriteOpState_Error ||
                        writeOp.getOpError().getStatus() != ErrorCodes::WouldChangeOwningShard ||
                        _writeOps.size() > 1) {
                        serviceOpCounters(ClusterRole::RouterServer).gotUpdate();
                    }

                    UpdateOpRef updateRef = writeOp.getWriteItem().getUpdateOp();

                    const auto opIdx = writeOp.getWriteItem().getItemIndex();
                    const auto& bulkWriteOp = BulkWriteCRUDOp(_clientRequest.getOps()[opIdx]);
                    const auto& ns = _clientRequest.getNsInfo()[bulkWriteOp.getNsInfoIdx()].getNs();
                    // 'isMulti' is set to false as the metrics for multi updates were registered
                    // for each operation individually.
                    bulk_write_common::incrementBulkWriteUpdateMetrics(getQueryCounters(_opCtx),
                                                                       ClusterRole::RouterServer,
                                                                       updateRef.getUpdateMods(),
                                                                       ns,
                                                                       updateRef.getArrayFilters(),
                                                                       false /* isMulti */);
                    break;
                }
                case BatchedCommandRequest::BatchType_Delete:
                    serviceOpCounters(ClusterRole::RouterServer).gotDelete();
                    break;
                default:
                    MONGO_UNREACHABLE
            }
        }

        if (writeOpState == WriteOpState_Completed) {
            if (writeOp.hasBulkWriteReplyItem()) {
                replyItems.push_back(writeOp.takeBulkWriteReplyItem());
            }
        } else if (writeOpState == WriteOpState_Error) {
            auto nsInfoIdx =
                BulkWriteCRUDOp(_clientRequest.getOps()[writeOp.getWriteItem().getItemIndex()])
                    .getNsInfoIdx();

            // Need to make a modifyable copy of the error.
            auto error = writeOp.getOpError();

            // If the error is not a collection UUID error then this function will not modify the
            // error, so we can call this on every iteration without checks.
            populateCollectionUUIDMismatch(_opCtx,
                                           &error,
                                           &actualCollections[nsInfoIdx],
                                           &hasContactedPrimaryShard[nsInfoIdx]);

            auto replyItem =
                BulkWriteReplyItem(writeOp.getWriteItem().getItemIndex(), error.getStatus());

            if (writeOp.hasBulkWriteReplyItem()) {
                auto successesReplyItem = writeOp.takeBulkWriteReplyItem();

                replyItem.setN(successesReplyItem.getN());
                replyItem.setNModified(successesReplyItem.getNModified());
                replyItem.setUpserted(successesReplyItem.getUpserted());
            } else {
                // If there was no previous successful response we still need to set nModified=0
                // for an update op since we lose that information in the BulkWriteReplyItem ->
                // WriteError transformation.
                if (writeOp.getWriteItem().getOpType() == BatchedCommandRequest::BatchType_Update) {
                    replyItem.setNModified(0);
                }
            }

            replyItems.emplace_back(replyItem);

            // We only count nErrors at the end of the command because it is simpler and less error
            // prone. If we counted errors as we encountered them we could hit edge cases where we
            // accidentally count the same error multiple times. At this point in the execution we
            // have already resolved any repeat errors.
            summary.nErrors++;
            // Only return the first error if we are ordered or are in a transaction.
            if (ordered || _inTransaction)
                break;
        }
    }
    std::vector<StmtId> retriedStmtIds;
    if (_retriedStmtIds.has_value()) {
        for (auto stmtId : *_retriedStmtIds) {
            retriedStmtIds.emplace_back(stmtId);
        }
    }
    return {std::move(replyItems), summary, generateWriteConcernError(), retriedStmtIds};
}

void BulkWriteOp::saveWriteConcernError(ShardId shardId,
                                        BulkWriteWriteConcernError wcError,
                                        const TargetedWriteBatch& writeBatch) {
    WriteConcernErrorDetail wce;
    wce.setStatus(Status(ErrorCodes::Error(wcError.getCode()), wcError.getErrmsg()));

    // WriteType::WithoutShardKeyWithId is always in its own batch, and so we only need to
    // inspect the first write here to determine if the batch is for a write of that type.
    auto opIdx = writeBatch.getWrites().front()->writeOpRef.first;
    if (_writeOps[opIdx].getWriteType() == WriteType::WithoutShardKeyWithId) {
        if (!_deferredWCErrors) {
            _deferredWCErrors.emplace();
        }
        (*_deferredWCErrors)[opIdx].push_back(ShardWCError(shardId, wce));
    } else {
        _wcErrors.push_back(ShardWCError(shardId, wce));
    }
}

void BulkWriteOp::saveWriteConcernError(ShardWCError shardWCError) {
    _wcErrors.push_back(std::move(shardWCError));
}

boost::optional<BulkWriteWriteConcernError> BulkWriteOp::generateWriteConcernError() const {
    if (auto mergedWce = mergeWriteConcernErrors(_wcErrors)) {
        auto totalWcError = BulkWriteWriteConcernError();
        totalWcError.setCode(mergedWce->toStatus().code());
        totalWcError.setErrmsg(mergedWce->toStatus().reason());

        return boost::optional<BulkWriteWriteConcernError>(totalWcError);
    }

    return boost::none;
}

void BulkWriteOp::noteStaleResponses(
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    const stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace) {
    auto& nsInfo = _clientRequest.getNsInfo();
    for (size_t i = 0; i < nsInfo.size(); i++) {
        auto& nsEntry = nsInfo.at(i);
        auto& targeter = targeters.at(i);
        // We must use the namespace from the original client request instead of the targeter's
        // namespace because the targeter's namespace could be pointing to the bucket collection for
        // tracked timeseries collections.
        auto errors = errorsPerNamespace.find(nsEntry.getNs());
        if (errors != errorsPerNamespace.cend()) {
            for (const auto& error : errors->second.getErrors(ErrorCodes::StaleConfig)) {
                LOGV2_DEBUG(7279201,
                            4,
                            "Noting stale config response.",
                            "shardId"_attr = error.endpoint.shardName,
                            "status"_attr = error.error.getStatus());
                targeter->noteStaleCollVersionResponse(
                    _opCtx, *error.error.getStatus().extraInfo<StaleConfigInfo>());
                setTargeterHasStaleShardResponse(true);
            }
            for (const auto& error : errors->second.getErrors(ErrorCodes::StaleDbVersion)) {
                LOGV2_DEBUG(7279202,
                            4,
                            "Noting stale database response.",
                            "shardId"_attr = error.endpoint.shardName,
                            "status"_attr = error.error.getStatus());
                targeter->noteStaleDbVersionResponse(
                    _opCtx, *error.error.getStatus().extraInfo<StaleDbRoutingVersion>());
                setTargeterHasStaleShardResponse(true);
            }
            for (const auto& error :
                 errors->second.getErrors(ErrorCodes::CannotImplicitlyCreateCollection)) {
                LOGV2_DEBUG(8037203,
                            0,
                            "Noting cannotImplicitlyCreateCollection response.",
                            "status"_attr = error.error.getStatus());
                targeter->noteCannotImplicitlyCreateCollectionResponse(
                    _opCtx,
                    *error.error.getStatus().extraInfo<CannotImplicitlyCreateCollectionInfo>());
            }
        }
    }
}

void BulkWriteOp::finishExecutingWriteWithoutShardKeyWithId() {
    if (_deferredResponses) {
        for (unsigned long idx = 0; idx < _deferredResponses->size(); idx++) {
            auto [targetedWriteBatch, response, replyItem] = _deferredResponses->at(idx);
            LOGV2_DEBUG(864041,
                        4,
                        "Processing deferred response for WithoutShardKeyWithId: ",
                        "idx"_attr = idx,
                        "shardId"_attr = targetedWriteBatch->getShardId().toString(),
                        "responseN"_attr = response.getNModified());
            const auto& write =
                targetedWriteBatch->getWrites()[idx % targetedWriteBatch->getWrites().size()];
            WriteOp& writeOp = _writeOps[write->writeOpRef.first];
            if (targeterHasStaleShardResponse()) {
                if (writeOp.getWriteState() != WriteOpState_Ready) {
                    writeOp.resetWriteToReady(_opCtx);
                }
            } else if (writeOp.getWriteState() != WriteOpState_Error) {
                auto nVal = response.getNModified() + response.getNDeleted();
                auto repl = replyItem.has_value()
                    ? boost::optional<const BulkWriteReplyItem&>(replyItem.value())
                    : boost::optional<const BulkWriteReplyItem&>(boost::none);
                writeOp.noteWriteWithoutShardKeyWithIdResponse(
                    _opCtx, *write, nVal, targetedWriteBatch->getNumOps(), repl);
            }
        }
        _deferredResponses = boost::none;
    }

    // See _deferredWCErrors for details.
    if (_deferredWCErrors) {
        for (auto& it : *_deferredWCErrors) {
            auto& op = _writeOps[it.first];
            invariant(op.getWriteType() == WriteType::WithoutShardKeyWithId);
            auto& wcErrors = it.second;
            if (op.getWriteState() >= WriteOpState_Completed) {
                _wcErrors.insert(_wcErrors.end(), wcErrors.begin(), wcErrors.end());
            } else {
                // If we are here for any op it means that the whole batch is retried.
                break;
            }
        }
        _deferredWCErrors = boost::none;
    }

    // Setting _shouldStopCurrentRound to false here allows for the processing of any pending
    // writeOps. The decision to stop retrying for the current writeOp is made before this point.
    _shouldStopCurrentRound = false;
}

void BulkWriteOp::noteTargetedShard(const TargetedWriteBatch& targetedBatch) {
    _stats.noteTargetedShard(_clientRequest, targetedBatch);
}

void BulkWriteOp::noteNumShardsOwningChunks(size_t nsIdx, int nShardsOwningChunks) {
    _stats.noteNumShardsOwningChunks(nsIdx, nShardsOwningChunks);
}

void BulkWriteOp::noteTwoPhaseWriteProtocol(const TargetedWriteBatch& targetedBatch,
                                            size_t nsIdx,
                                            int nShardsOwningChunks) {
    _stats.noteTwoPhaseWriteProtocol(_clientRequest, targetedBatch, nsIdx, nShardsOwningChunks);
}

void addIdsForInserts(BulkWriteCommandRequest& origCmdRequest) {
    std::vector<BulkWriteOpVariant> newOps;
    newOps.reserve(origCmdRequest.getOps().size());

    for (const auto& op : origCmdRequest.getOps()) {
        auto crudOp = BulkWriteCRUDOp(op);
        if (crudOp.getType() == BulkWriteCRUDOp::kInsert &&
            crudOp.getInsert()->getDocument()["_id"].eoo()) {
            auto insert = crudOp.getInsert();
            auto doc = insert->getDocument();
            BSONObjBuilder idInsertB;
            idInsertB.append("_id", OID::gen());
            idInsertB.appendElements(doc);
            auto newDoc = idInsertB.obj();
            auto newOp = BulkWriteInsertOp(insert->getNsInfoIdx(), std::move(newDoc));
            newOps.push_back(std::move(newOp));
        } else {
            newOps.push_back(std::move(op));
        }
    }

    origCmdRequest.setOps(newOps);
}


}  // namespace bulk_write_exec

}  // namespace mongo
